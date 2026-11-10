#include "completion_port.h"
#include "event.h"
#include "event_pulse.h"
#include "kernel_context.h"
#include "threads.h"

#include <sys/timerfd.h>
#include <unistd.h>

#include <chrono>
#include <map>
#include <memory>
#include <vector>

namespace raven {

namespace {

constexpr uint32_t mmsyserr_baddeviceid = 2, mmsyserr_invalparam = 11;
constexpr uint32_t time_periodic = 0x1, time_callback_event_set = 0x10, time_callback_event_pulse = 0x20, time_callback_type = 0xf0,
	time_kill_synchronous = 0x100;
// The range timeGetDevCaps reports.
constexpr uint32_t shortest_delay = 1, longest_delay = 1000000;

struct MultimediaTimer {
	uint32_t delay = 0, flags = 0;
	uint64_t callback = 0, user = 0;
	std::chrono::steady_clock::time_point deadline;
	// Cleared when the timer is killed or a one-shot timer has fired; the entry stays while its callback runs, so a kill can wait for it.
	bool active = true;
	std::shared_ptr<GuestThread> running;
	bool finished() const { return !running || running->state == GuestThread::State::done; }
};

struct TimerState {
	std::map<uint32_t, std::shared_ptr<MultimediaTimer>> timers;
	// Windows runs every callback on one timer thread, so callbacks of different timers never overlap: the latest one started.
	std::shared_ptr<GuestThread> running;
	uint32_t next_id = 1;
	// Wakes an idle process when the next timer is due instead of at the scheduler's 20 ms poll; -1 when it could not be made.
	int clock = -1;
	bool watched = false;
	~TimerState()
	{
		if (clock >= 0)
			::close(clock);
	}
	bool busy() const { return running && running->state != GuestThread::State::done; }
	bool any_active() const
	{
		for (const auto &[id, timer] : timers)
			if (timer->active)
				return true;
		return false;
	}
	void drain() const
	{
		uint64_t expirations = 0;
		if (clock >= 0)
			while (::read(clock, &expirations, sizeof expirations) > 0) {}
	}
	// Arms the clock for the earliest deadline that can fire: function timers wait while another callback runs, and the end of that
	// callback is seen by the scheduler itself, so arming for them then would only spin the idle wait.
	void arm() const
	{
		if (clock < 0)
			return;
		auto earliest = std::chrono::steady_clock::time_point::max();
		for (const auto &[id, timer] : timers) {
			const bool event = timer->flags & (time_callback_event_set | time_callback_event_pulse);
			if (timer->active && (event || !busy()))
				earliest = std::min(earliest, timer->deadline);
		}
		itimerspec when{};
		if (earliest != std::chrono::steady_clock::time_point::max()) {
			const auto since = std::chrono::duration_cast<std::chrono::nanoseconds>(earliest.time_since_epoch()).count();
			// Zero would disarm the clock; a deadline already passed still has to fire.
			when.it_value = since > 0 ? timespec{since / 1000000000, since % 1000000000} : timespec{0, 1};
		}
		::timerfd_settime(clock, TFD_TIMER_ABSTIME, &when, nullptr);
	}
};

void fire(TimerState &state, KernelContext &context, EventPulses &pulses)
{
	if (state.timers.empty())
		return;
	state.drain();
	const auto now = std::chrono::steady_clock::now();
	const auto reschedule = [now](MultimediaTimer &timer) {
		if (timer.flags & time_periodic)
			timer.deadline = std::max(timer.deadline + std::chrono::milliseconds(timer.delay), now);
		else
			timer.active = false;
	};
	for (const auto &[id, timer] : state.timers) {
		if (!timer->active || now < timer->deadline || !(timer->flags & (time_callback_event_set | time_callback_event_pulse)))
			continue;
		if (timer->flags & time_callback_event_pulse)
			pulses.pulse(timer->callback);
		else
			context.objects.set_event(timer->callback, true);
		reschedule(*timer);
	}
	// One function callback at a time, the most overdue first. A callback thread maps its stack, which a serial guest answered without
	// stopping cannot have: the next look starts it.
	if (!state.busy() && (!context.process.serving() || context.process.parallel())) {
		std::shared_ptr<MultimediaTimer> due;
		uint32_t due_id = 0;
		for (const auto &[id, timer] : state.timers)
			if (timer->active && now >= timer->deadline && !(timer->flags & (time_callback_event_set | time_callback_event_pulse)) &&
				(!due || timer->deadline < due->deadline)) {
				due = timer;
				due_id = id;
			}
		if (due) {
			const auto thread = context.threads.create(due->callback, due_id, 1024 * 1024, false);
			thread->arguments = {due_id, 0, due->user, 0, 0};
			due->running = state.running = thread;
			reschedule(*due);
		}
	}
	std::erase_if(state.timers, [](const auto &entry) { return !entry.second->active && entry.second->finished(); });
	state.arm();
}

}

void register_winmm_api(ApiRegistry &registry, KernelContext &context, std::shared_ptr<EventPulses> pulses)
{
	auto state = std::make_shared<TimerState>();
	// Services look while threads run, so a program that busy-polls still gets its callbacks; pumps look while every thread waits.
	context.threads.add_service([state, pulses, &context] { fire(*state, context, *pulses); });
	context.threads.add_pump([state, pulses, &context] { fire(*state, context, *pulses); });
	context.threads.add_external([state] { return state->any_active(); });
	// A thread that spins on a flag a callback sets, without calling the runtime, must still let the callback start: the scheduler slices
	// it while a timer is armed.
	context.threads.add_due([state] { return state->any_active(); });

	// timeSetEvent(uDelay, uResolution, lpTimeProc, dwUser, fuEvent): a callback on a new guest thread, as Windows' timer thread
	// would call it, or an event set or pulsed with no thread at all. The resolution changes nothing: the host clock is finer.
	registry.add("timeSetEvent", 5, [state, &context](ApiCall &call) {
		const auto delay = call.u32(0);
		const auto type = call.u32(4) & time_callback_type;
		const bool known = type == 0 || type == time_callback_event_set || type == time_callback_event_pulse;
		if (delay < shortest_delay || delay > longest_delay || !call[2] || !known) {
			call.ret(0);
			return;
		}
		if (state->clock < 0)
			state->clock = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
		auto timer = std::make_shared<MultimediaTimer>();
		timer->delay = delay;
		timer->flags = call.u32(4);
		timer->callback = call[2];
		// A 32-bit caller's dwUser of 0xffffffff arrives widened like a pseudo handle; the callback receives what was passed.
		timer->user = call.pointer_width() == 4 ? uint64_t(call.u32(3)) : call[3];
		timer->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
		uint32_t id = state->next_id;
		while (!id || state->timers.contains(id))
			++id;
		state->next_id = id + 1;
		state->timers.emplace(id, timer);
		if (state->clock >= 0 && !state->watched) {
			state->watched = true;
			context.reactor->add([state] {
				state->drain();
				if (state->any_active())
					return false;
				state->watched = false;
				return true;
			}, state->clock, POLLIN);
		}
		state->arm();
		call.ret(id);
	});
	// timeKillEvent(uTimerID): no callback starts after it; with TIME_KILL_SYNCHRONOUS it also waits for one still running, unless
	// it is that callback that asks.
	registry.add("timeKillEvent", 1, [state, &context](ApiCall &call) {
		const auto found = state->timers.find(call.u32(0));
		if (found == state->timers.end() || (!found->second->active && found->second->finished())) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		const auto timer = found->second;
		state->timers.erase(found);
		timer->active = false;
		state->arm();
		if ((timer->flags & time_kill_synchronous) && !timer->finished() && timer->running.get() != &context.threads.current())
			context.threads.block([timer] { return timer->finished(); }, std::nullopt);
		call.ret(0);
	});

	// No auxiliary audio device; wave_api registers MIDI and the host PlaySound fallback.
	registry.add("auxGetNumDevs", 0, [](ApiCall &call) { call.ret(0); });
	for (const auto *name : {"auxGetDevCapsA", "auxGetDevCapsW"})
		registry.add(name, 3, [](ApiCall &call) { call.ret(mmsyserr_baddeviceid); });
	// Windows 95 onwards answer version 3.10.
	registry.add("mmsystemGetVersion", 0, [](ApiCall &call) { call.ret(0x030a); });
}

}
