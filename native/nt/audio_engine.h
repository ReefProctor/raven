#pragma once

#include "raven_audio.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace raven {

class ApiCall;

struct AudioFormat {
	uint32_t rate = 0, channels = 0, bits = 0, block = 0;
	bool floating = false;
	uint32_t bytes_per_second() const { return rate * block; }
};

// WAVEFORMATEX, or WAVEFORMATEXTENSIBLE naming PCM or IEEE float in its subformat: integer samples of 8, 16, 24 or 32 bits or 32-bit
// floats, one to eight channels.
std::optional<AudioFormat> read_wave_format(ApiCall &call, uint64_t address);

// One stream of the host sound engine: a ring of frames in memory that can also be mapped into the guest, behind it a device that
// consumes (render) or fills (capture) the ring at the format's pace. The device is the sound server (PulseAudio's protocol, which
// PipeWire serves too) or, without one or when RAVEN_AUDIO=null, a clock that plays to nowhere and records silence in real time.
//
// The ring has one producer and one consumer. For render the producer is the stream's owner, through write() or the guest through the
// mapped page, and the consumer is the device; for capture it is the other way round. `refill` and `notify` run on a host thread of the
// device: the first just before the device takes frames, so a host owner can top the ring up from its own queue, the second once a
// period, so the owner can wake the guest. Neither may call start, stop, reset or the destructor.
class AudioStream {
public:
	struct Options {
		AudioFormat format;
		bool capture = false;
		// The ring's size and the device's period, in frames; zero means a tenth of a second and a hundredth.
		uint32_t buffer_frames = 0, period_frames = 0;
		std::function<void()> notify;
		std::function<void(AudioStream &)> refill;
	};
	// A stream that is stopped, empty and at position zero; nullptr if the shared memory cannot be made.
	static std::unique_ptr<AudioStream> open(Options options);
	virtual ~AudioStream();
	AudioStream(const AudioStream &) = delete;
	AudioStream &operator=(const AudioStream &) = delete;

	// "pulse" or "null".
	virtual std::string_view backend() const = 0;
	virtual void start() = 0;
	// Returns once the device takes no more frames from the ring; the position stays where it is.
	virtual void stop() = 0;
	// Stops and drops every frame in the ring and in the device; the counters and the position start again from zero.
	virtual void reset() = 0;

	// The memory the guest maps: the control page and the ring, `mapping_size()` bytes of `descriptor()`.
	int descriptor() const { return memory; }
	size_t mapping_size() const { return size; }
	RavenAudioShared &shared() { return *page; }
	const AudioFormat &format() const { return stream_format; }
	uint32_t buffer_frames() const { return page->buffer_frames; }
	uint32_t period_frames() const { return page->period_frames; }
	bool running() const;
	// Played (render) or captured (capture) frames since the last reset.
	uint64_t position() const;
	// Render producer side: the frames there is room for, and a copy of whole frames into the ring (as many as fit).
	uint64_t space() const;
	uint64_t write(const uint8_t *frames, uint64_t count);
	// Capture consumer side: the frames waiting, and a copy out of the ring.
	uint64_t available() const;
	uint64_t read(uint8_t *frames, uint64_t count);

protected:
	AudioStream(Options options, int memory, size_t size, RavenAudioShared *page);
	uint8_t *ring() { return reinterpret_cast<uint8_t *>(page) + RAVEN_AUDIO_DATA_OFFSET; }
	std::atomic_ref<uint64_t> counter(uint64_t &field) const { return std::atomic_ref<uint64_t>(field); }
	// Device side: frames the ring holds for the device and taking them (render), room and filling it (capture).
	uint64_t pending() const;
	void consume(uint8_t *out, uint64_t count);
	void produce(const uint8_t *in, uint64_t count);
	void set_position(uint64_t frames);
	void clear_counters();
	void set_running(bool value);
	float volume() const;

	const AudioFormat stream_format;
	const bool capturing;
	const std::function<void()> notify;
	const std::function<void(AudioStream &)> refill;

private:
	int memory;
	size_t size;
	RavenAudioShared *page;
};

}
