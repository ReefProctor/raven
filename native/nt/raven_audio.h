#pragma once

/*
 * The control page of an audio stream that the runtime and a guest component (mmdevapi, dsound, winmm) share without calling each other.
 * RavenAudioOpen in ravenhost.dll maps it into the guest; the samples follow it at RAVEN_AUDIO_DATA_OFFSET as a ring of buffer_frames
 * frames. A frame's place in the ring is its counter modulo buffer_frames.
 *
 * Render: the guest writes frames at write_frames and then advances it; the runtime consumes from read_frames and advances that.
 * Capture: the runtime writes and advances write_frames; the guest reads and advances read_frames.
 * The counters only grow until RavenAudioReset sets them to zero. They are 64-bit and aligned, read and written with atomic operations
 * (acquire loads, release stores); a 32-bit guest uses its 8-byte atomic instructions on them.
 *
 * position_frames is what the device has played (render) or captured (capture) since the last reset, refreshed at least every period;
 * position_counter is the performance counter (QueryPerformanceCounter, 100 ns units) when it was refreshed.
 */

#include <stdint.h>

#define RAVEN_AUDIO_CAPTURE 0x1u
#define RAVEN_AUDIO_DATA_OFFSET 4096u

typedef struct RavenAudioShared {
	uint64_t write_frames;
	uint64_t read_frames;
	uint64_t position_frames;
	uint64_t position_counter;
	/* Frames the device wanted while the ring had none (render) or that found the ring full and were dropped (capture). */
	uint64_t glitch_frames;
	uint32_t running;
	/* The stream volume as the bits of an IEEE float, 1.0 when untouched; the runtime scales 16-bit and float samples with it. */
	uint32_t volume;
	uint32_t buffer_frames;
	uint32_t frame_bytes;
	uint32_t rate;
	uint32_t channels;
	uint32_t bits;
	/* 1 for integer PCM, 3 for IEEE float, as the WAVE_FORMAT tags. */
	uint32_t format_tag;
	uint32_t period_frames;
	uint32_t flags;
} RavenAudioShared;

#ifdef __cplusplus
static_assert(sizeof(RavenAudioShared) == 80 && alignof(RavenAudioShared) == 8, "the shared audio page has one layout for every guest");
#endif
