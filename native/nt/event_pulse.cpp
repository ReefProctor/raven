#include "event_pulse.h"

#include "event.h"
#include "kernel_context.h"
#include "threads.h"

namespace raven {

bool EventPulses::pulse(uint64_t handle)
{
	const auto event = context.objects.object_as<EventObject>(handle);
	if (!event)
		return false;
	event->pulse_host_waiters();
	for (const auto id : context.threads.thread_ids()) {
		const auto thread = context.threads.find(id);
		// A thread whose wait holds already is released by something else.
		if (!thread || thread->state != GuestThread::State::waiting || !thread->ready || thread->ready())
			continue;
		event->grant_pulse(id);
		if (!thread->ready()) {
			event->revoke_pulse(id);
			continue;
		}
		pulses.push_back({event, thread, thread->waiting_since});
		if (!event->manual_reset())
			break;
	}
	return true;
}

void EventPulses::settle()
{
	std::erase_if(pulses, [](const Pulse &pulse) {
		if (pulse.thread->state == GuestThread::State::waiting && pulse.thread->waiting_since == pulse.since)
			return false;
		pulse.event->revoke_pulse(pulse.thread->id);
		return true;
	});
}

std::shared_ptr<EventPulses> register_pulse_event(ApiRegistry &registry, KernelContext &context)
{
	auto pulses = std::make_shared<EventPulses>(context);
	context.threads.add_service([pulses] { pulses->settle(); });
	context.threads.add_pump([pulses] { pulses->settle(); });
	registry.add("PulseEvent", 1, [pulses](ApiCall &call) {
		pulses->settle();
		if (pulses->pulse(call[0]))
			call.ret(1);
		else
			call.fail(6, 0);
	});
	return pulses;
}

}
