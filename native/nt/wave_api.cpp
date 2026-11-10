#include "audio_engine.h"
#include "kernel_context.h"
#include "text.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace raven {

namespace {

constexpr uint32_t mmsyserr_error = 1, mmsyserr_baddeviceid = 2, mmsyserr_invalhandle = 5, mmsyserr_nomem = 7, mmsyserr_notsupported = 8,
	mmsyserr_invalflag = 10, mmsyserr_invalparam = 11, waverr_badformat = 32, waverr_stillplaying = 33, waverr_unprepared = 34;
constexpr uint32_t whdr_done = 0x1, whdr_prepared = 0x2, whdr_beginloop = 0x4, whdr_endloop = 0x8, whdr_inqueue = 0x10;
constexpr uint32_t callback_kind = 0x70000, callback_null = 0, callback_event = 0x50000, wave_format_query = 0x1;
constexpr uint64_t wave_mapper = 0xffffffffU;

// One opened device over a stream of the sound engine. The buffers wait in order; the device pulls their frames into the stream's ring
// as it makes room, and a buffer is done once the stream's position, what was played, has passed its last frame.
class WaveOut : public KernelObject {
public:
	WaveOut(const AudioFormat &format, uint32_t callback, uint64_t target, std::function<void()> wake)
		: format(format), callback(callback), target(target)
	{
		AudioStream::Options options;
		options.format = format;
		options.notify = std::move(wake);
		options.refill = [this](AudioStream &) {
			std::lock_guard lock(mutex);
			feed();
		};
		stream = AudioStream::open(std::move(options));
		if (stream)
			stream->start();
	}
	bool opened() const { return stream != nullptr; }
	void enqueue(uint64_t header, std::vector<uint8_t> data)
	{
		std::lock_guard lock(mutex);
		data.resize(data.size() / format.block * format.block);
		enqueued += data.size() / format.block;
		queued.push_back({header, std::move(data), 0, enqueued});
		feed();
	}
	// Every buffer not played yet comes back at once, in order, and the position starts again from zero; a paused device plays again.
	std::vector<uint64_t> reset()
	{
		stream->stop();
		std::vector<uint64_t> dropped;
		{
			std::lock_guard lock(mutex);
			for (const auto &item : queued)
				dropped.push_back(item.header);
			queued.clear();
			enqueued = 0;
		}
		stream->reset();
		stream->start();
		return dropped;
	}
	void pause(bool value) { value ? stream->stop() : stream->start(); }
	bool busy()
	{
		std::lock_guard lock(mutex);
		return !queued.empty();
	}
	// The buffers whose last frame was played, in order.
	std::vector<uint64_t> take_finished()
	{
		const uint64_t played = stream->position();
		std::vector<uint64_t> finished;
		std::lock_guard lock(mutex);
		while (!queued.empty() && queued.front().end <= played) {
			finished.push_back(queued.front().header);
			queued.pop_front();
		}
		return finished;
	}
	uint64_t position_bytes() const { return stream->position() * format.block; }

	const AudioFormat format;
	const uint32_t callback;
	const uint64_t target;
	// Left and right halves of the word; applied as frames go into the ring, so a change reaches buffers already queued.
	std::atomic<uint32_t> volume = 0xffffffffU;

private:
	struct Item {
		uint64_t header;
		std::vector<uint8_t> data;
		// Bytes already in the ring, and the stream frame just past the buffer's last.
		size_t sent;
		uint64_t end;
	};
	// With the mutex held: as much of the waiting buffers as the ring has room for.
	void feed()
	{
		const uint32_t level = volume.load(std::memory_order_relaxed);
		for (auto &item : queued) {
			if (item.sent == item.data.size())
				continue;
			const uint64_t frames = (item.data.size() - item.sent) / format.block;
			uint64_t taken;
			if (level == 0xffffffffU)
				taken = stream->write(item.data.data() + item.sent, frames);
			else {
				const uint64_t room = std::min(frames, stream->space());
				scratch.assign(item.data.begin() + item.sent, item.data.begin() + item.sent + room * format.block);
				scale(scratch, level);
				taken = stream->write(scratch.data(), room);
			}
			item.sent += taken * format.block;
			if (taken < frames)
				return;
		}
	}
	// The formats a volume applies to: 16-bit integers and floats, the left half of the word for the first channel, the right for the
	// second, and the left again for any further.
	void scale(std::vector<uint8_t> &samples, uint32_t level) const
	{
		const float left = float(level & 0xffff) / 65535.0f, right = float(level >> 16) / 65535.0f;
		const size_t channels = format.channels;
		if (format.floating)
			for (size_t index = 0; index + 4 <= samples.size(); index += 4) {
				float value;
				std::memcpy(&value, samples.data() + index, 4);
				value *= (index / 4) % channels == 1 ? right : left;
				std::memcpy(samples.data() + index, &value, 4);
			}
		else if (format.bits == 16)
			for (size_t index = 0; index + 2 <= samples.size(); index += 2) {
				int16_t value;
				std::memcpy(&value, samples.data() + index, 2);
				value = int16_t(float(value) * ((index / 2) % channels == 1 ? right : left));
				std::memcpy(samples.data() + index, &value, 2);
			}
	}

	std::mutex mutex;
	std::deque<Item> queued;
	std::vector<uint8_t> scratch;
	uint64_t enqueued = 0;
	// Last, so the device's threads end before the queue they feed from goes.
	std::unique_ptr<AudioStream> stream;
};

}

void register_midi_api(ApiRegistry &registry)
{
	// No MIDI port, in or out: a program lists none, and opening one finds no such device; the mapper finds no device behind it.
	constexpr uint32_t midierr_nodevice = 68, midi_mapper = 0xffffffffU;
	for (const char *name : {"midiInGetNumDevs", "midiOutGetNumDevs"})
		registry.add(name, 0, [](ApiCall &call) { call.ret(0); });
	for (const char *name : {"midiInGetDevCapsW", "midiInGetDevCapsA", "midiOutGetDevCapsW", "midiOutGetDevCapsA"})
		registry.add(name, 3, [](ApiCall &call) { call.ret(mmsyserr_baddeviceid); });
	for (const char *name : {"midiInOpen", "midiOutOpen"})
		registry.add(name, 5, [](ApiCall &call) {
			if (call[0])
				call.store_pointer(call[0], 0);
			call.ret(call.u32(1) == midi_mapper ? midierr_nodevice : mmsyserr_baddeviceid);
		});
}

void register_wave_api(ApiRegistry &registry, KernelContext &context)
{
	register_midi_api(registry);
	auto &objects = context.objects;
	auto devices = std::make_shared<std::map<uint64_t, std::weak_ptr<WaveOut>>>();
	const auto device_of = [&objects](uint64_t handle) { return objects.object_as<WaveOut>(handle); };
	const auto flags_at = [](ApiCall &call, uint64_t header) { return header + (call.pointer_width() == 4 ? 16 : 24); };

	// A finished buffer's header turns done and leaves the queue, and an event callback is told.
	const auto finish = [&context](WaveOut &device, const std::vector<uint64_t> &headers) {
		const uint64_t offset = context.process.mode() == NativeMode::x86 ? 16 : 24;
		for (const auto header : headers) {
			try {
				const auto raw = context.process.read_memory(header + offset, 4);
				uint32_t flags;
				std::memcpy(&flags, raw.data(), 4);
				flags = (flags | whdr_done) & ~whdr_inqueue;
				context.process.write_memory(header + offset, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&flags), 4));
			} catch (...) {
			}
			if (device.callback == callback_event && device.target)
				context.objects.set_event(device.target, true);
		}
	};
	// Played buffers are marked done whenever the scheduler runs; the device's thread wakes it once a period while sound plays.
	context.threads.add_service([devices, finish] {
		for (auto entry = devices->begin(); entry != devices->end();) {
			const auto device = entry->second.lock();
			if (!device) {
				entry = devices->erase(entry);
				continue;
			}
			finish(*device, device->take_finished());
			++entry;
		}
	});
	const auto wake = context.threads.remote_nudge();

	registry.add("waveOutGetNumDevs", 0, [](ApiCall &call) { call.ret(1); });
	// System and file sounds are accepted without being played, so a program that beeps or chimes goes on as if it had.
	for (const auto *name : {"PlaySound", "PlaySoundA", "PlaySoundW"})
		registry.add(name, 3, [](ApiCall &call) { call.ret(1); });
	for (const auto *name : {"sndPlaySoundA", "sndPlaySoundW"})
		registry.add(name, 2, [](ApiCall &call) { call.ret(1); });
	// There is no capture device: recording programs hear that none exists.
	registry.add("waveInGetNumDevs", 0, [](ApiCall &call) { call.ret(0); });
	// There is no capture device and no mixer: their calls report a bad device or no driver, as Windows does on such a machine.
	registry.add("waveInMessage", 4, [](ApiCall &call) { call.ret(2); });
	registry.add("waveInOpen", 6, [](ApiCall &call) {
		if (call[0])
			call.store_pointer(call[0], 0);
		call.ret(2);
	});
	for (const char *name : {"waveInClose", "waveInStart", "waveInStop", "waveInReset"})
		registry.add(name, 1, [](ApiCall &call) { call.ret(5); });
	registry.add("mixerGetNumDevs", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("mixerOpen", 5, [](ApiCall &call) {
		if (call[0])
			call.store_pointer(call[0], 0);
		call.ret(6);
	});
	registry.add("mixerGetID", 3, [](ApiCall &call) { call.ret(6); });
	for (const auto *name : {"waveInGetDevCapsW", "waveInGetDevCapsA"})
		registry.add(name, 3, [](ApiCall &call) { call.ret(mmsyserr_baddeviceid); });
	for (const bool wide : {true, false})
		registry.add(wide ? "waveOutGetDevCapsW" : "waveOutGetDevCapsA", 3, [wide](ApiCall &call) {
			if (call[0] != 0 && call[0] != wave_mapper && uint32_t(call[0]) != uint32_t(wave_mapper)) {
				call.ret(mmsyserr_baddeviceid);
				return;
			}
			const size_t size = wide ? 84 : 52;
			if (!call[1] || call.u32(2) < 8) {
				call.ret(mmsyserr_invalparam);
				return;
			}
			std::vector<uint8_t> caps(size, 0);
			const uint16_t manufacturer = 1, product = 2;
			std::memcpy(caps.data(), &manufacturer, 2);
			std::memcpy(caps.data() + 2, &product, 2);
			const std::string name = "Speakers (Raven)";
			for (size_t index = 0; index < name.size(); ++index)
				caps[8 + index * (wide ? 2 : 1)] = uint8_t(name[index]);
			const size_t after = wide ? 72 : 40;
			const uint32_t formats = 0xfffff;
			const uint16_t channels = 2;
			const uint32_t support = 0x4 | 0x8;
			std::memcpy(caps.data() + after, &formats, 4);
			std::memcpy(caps.data() + after + 4, &channels, 2);
			std::memcpy(caps.data() + after + 8, &support, 4);
			call.write(call[1], std::span<const uint8_t>(caps.data(), std::min<size_t>(size, call.u32(2))));
			call.ret(0);
		});
	registry.add("waveOutOpen", 6, [&objects, devices, wake](ApiCall &call) {
		const uint32_t flags = call.u32(5);
		if (call[1] != 0 && uint32_t(call[1]) != uint32_t(wave_mapper)) {
			call.ret(mmsyserr_baddeviceid);
			return;
		}
		if (!call[2]) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		const auto format = read_wave_format(call, call[2]);
		if (!format) {
			call.ret(waverr_badformat);
			return;
		}
		if (flags & wave_format_query) {
			call.ret(0);
			return;
		}
		// Window, thread and function callbacks need the window system or a guest thread of their own; only events are told here.
		const uint32_t kind = flags & callback_kind;
		if (kind != callback_null && kind != callback_event) {
			call.ret(kind == 0x10000 || kind == 0x20000 || kind == 0x30000 ? mmsyserr_notsupported : mmsyserr_invalflag);
			return;
		}
		if (!call[0]) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		const auto device = std::make_shared<WaveOut>(*format, kind, kind == callback_event ? call[3] : 0, wake);
		if (!device->opened()) {
			call.ret(mmsyserr_nomem);
			return;
		}
		const auto made = objects.add_object(device);
		if (made.status) {
			call.ret(mmsyserr_nomem);
			return;
		}
		(*devices)[made.value] = device;
		call.store_pointer(call[0], made.value);
		if (kind == callback_event && call[3])
			objects.set_event(call[3], true);
		call.ret(0);
	});
	registry.add("waveOutPrepareHeader", 3, [device_of, flags_at](ApiCall &call) {
		if (!device_of(call[0])) {
			call.ret(mmsyserr_invalhandle);
			return;
		}
		if (!call[1] || call.u32(2) < (call.pointer_width() == 4 ? 32U : 48U)) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		const uint64_t at = flags_at(call, call[1]);
		call.store<uint32_t>(at, call.load<uint32_t>(at) | whdr_prepared);
		call.ret(0);
	});
	registry.add("waveOutUnprepareHeader", 3, [device_of, flags_at](ApiCall &call) {
		if (!device_of(call[0])) {
			call.ret(mmsyserr_invalhandle);
			return;
		}
		if (!call[1]) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		const uint64_t at = flags_at(call, call[1]);
		const uint32_t flags = call.load<uint32_t>(at);
		if (flags & whdr_inqueue) {
			call.ret(waverr_stillplaying);
			return;
		}
		call.store<uint32_t>(at, flags & ~whdr_prepared);
		call.ret(0);
	});
	// The samples are taken when the buffer is written, as the header promises they stay put until it is done. A buffer that begins and
	// ends a loop plays as often as it asks.
	registry.add("waveOutWrite", 3, [device_of, flags_at](ApiCall &call) {
		const auto device = device_of(call[0]);
		if (!device) {
			call.ret(mmsyserr_invalhandle);
			return;
		}
		if (!call[1]) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		const uint64_t at = flags_at(call, call[1]);
		const uint32_t flags = call.load<uint32_t>(at);
		if (!(flags & whdr_prepared)) {
			call.ret(waverr_unprepared);
			return;
		}
		if (flags & whdr_inqueue) {
			call.ret(waverr_stillplaying);
			return;
		}
		const uint64_t data = call.load_pointer(call[1]);
		const uint32_t length = call.load<uint32_t>(call[1] + call.pointer_width());
		std::vector<uint8_t> samples = length ? call.read(data, length) : std::vector<uint8_t>();
		const uint32_t loops = call.load<uint32_t>(at + 4);
		if ((flags & whdr_beginloop) && (flags & whdr_endloop) && loops > 1 && size_t(length) * loops <= (64u << 20)) {
			std::vector<uint8_t> repeated;
			repeated.reserve(size_t(length) * loops);
			for (uint32_t index = 0; index < loops; ++index)
				repeated.insert(repeated.end(), samples.begin(), samples.end());
			samples = std::move(repeated);
		}
		call.store<uint32_t>(at, (flags & ~whdr_done) | whdr_inqueue);
		device->enqueue(call[1], std::move(samples));
		call.ret(0);
	});
	// The buffers come back before the call returns, as programs that free them right after it expect.
	registry.add("waveOutReset", 1, [device_of, finish](ApiCall &call) {
		const auto device = device_of(call[0]);
		if (!device) {
			call.ret(mmsyserr_invalhandle);
			return;
		}
		finish(*device, device->take_finished());
		finish(*device, device->reset());
		call.ret(0);
	});
	registry.add("waveOutPause", 1, [device_of](ApiCall &call) {
		const auto device = device_of(call[0]);
		call.ret(device ? (device->pause(true), 0U) : mmsyserr_invalhandle);
	});
	registry.add("waveOutRestart", 1, [device_of](ApiCall &call) {
		const auto device = device_of(call[0]);
		call.ret(device ? (device->pause(false), 0U) : mmsyserr_invalhandle);
	});
	registry.add("waveOutClose", 1, [&objects, device_of](ApiCall &call) {
		const auto device = device_of(call[0]);
		if (!device) {
			call.ret(mmsyserr_invalhandle);
			return;
		}
		if (device->busy()) {
			call.ret(waverr_stillplaying);
			return;
		}
		objects.close(call[0]);
		call.ret(0);
	});
	// MMTIME: the kind asked for (milliseconds, samples or bytes; other kinds get bytes) and the position played since the last reset.
	registry.add("waveOutGetPosition", 3, [device_of](ApiCall &call) {
		const auto device = device_of(call[0]);
		if (!device) {
			call.ret(mmsyserr_invalhandle);
			return;
		}
		if (!call[1] || call.u32(2) < 12) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		const uint64_t bytes = device->position_bytes();
		uint32_t kind = call.load<uint32_t>(call[1]), value;
		if (kind == 1)
			value = uint32_t(bytes * 1000 / std::max(device->format.bytes_per_second(), 1U));
		else if (kind == 2)
			value = uint32_t(bytes / std::max(device->format.block, 1U));
		else {
			kind = 4;
			value = uint32_t(bytes);
		}
		call.store<uint32_t>(call[1], kind);
		call.store<uint32_t>(call[1] + 4, value);
		call.ret(0);
	});
	registry.add("waveOutGetVolume", 2, [device_of](ApiCall &call) {
		const auto device = device_of(call[0]);
		if (!call[1]) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		call.store<uint32_t>(call[1], device ? device->volume.load() : 0xffffffffU);
		call.ret(0);
	});
	registry.add("waveOutSetVolume", 2, [device_of](ApiCall &call) {
		if (const auto device = device_of(call[0]))
			device->volume = call.u32(1);
		call.ret(0);
	});
	registry.add("waveOutGetID", 2, [device_of](ApiCall &call) {
		if (!device_of(call[0])) {
			call.ret(mmsyserr_invalhandle);
			return;
		}
		if (call[1])
			call.store<uint32_t>(call[1], 0);
		call.ret(0);
	});
	registry.add("waveOutMessage", 4, [](ApiCall &call) { call.ret(mmsyserr_notsupported); });
	for (const bool wide : {true, false})
		registry.add(wide ? "waveOutGetErrorTextW" : "waveOutGetErrorTextA", 3, [wide](ApiCall &call) {
			static const std::map<uint32_t, std::string> texts{{0, "The command completed successfully."},
				{mmsyserr_error, "An unspecified error occurred."}, {mmsyserr_baddeviceid, "The device identifier is out of range."},
				{mmsyserr_invalhandle, "The handle is not valid."}, {mmsyserr_nomem, "There is not enough memory."},
				{mmsyserr_notsupported, "The function is not supported."}, {mmsyserr_invalflag, "A flag is not valid."},
				{mmsyserr_invalparam, "A parameter is not valid."}, {waverr_badformat, "The wave format is not supported."},
				{waverr_stillplaying, "The device is still playing."}, {waverr_unprepared, "The header is not prepared."}};
			const auto found = texts.find(call.u32(0));
			if (found == texts.end()) {
				call.ret(mmsyserr_baddeviceid);
				return;
			}
			if (!call[1] || !call.u32(2)) {
				call.ret(mmsyserr_invalparam);
				return;
			}
			if (wide)
				call.put_wide(call[1], from_utf8(found->second), call.u32(2));
			else
				call.put_narrow(call[1], found->second, call.u32(2));
			call.ret(0);
		});
}

}
