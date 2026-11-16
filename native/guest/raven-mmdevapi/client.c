/* SPDX-License-Identifier: MIT
 *
 * WASAPI's clients in mmdevapi.dll: IAudioClient3 in shared mode with its render and capture clients, clock and stream volume, each on a
 * stream of the runtime's sound engine. Render writes frames at write_frames and the engine plays from read_frames; capture reads packets
 * of a device period from read_frames as the engine records at write_frames.
 */
#include "mmdevapi_private.h"

client *clients;


static client *client_of(IAudioClient3 *iface) { return CONTAINING_RECORD(iface, client, IAudioClient3_iface); }
static client *render_of(IAudioRenderClient *iface) { return CONTAINING_RECORD(iface, client, IAudioRenderClient_iface); }
static client *capture_of(IAudioCaptureClient *iface) { return CONTAINING_RECORD(iface, client, IAudioCaptureClient_iface); }
static client *clock_of(IAudioClock *iface) { return CONTAINING_RECORD(iface, client, IAudioClock_iface); }
static client *clock2_of(IAudioClock2 *iface) { return CONTAINING_RECORD(iface, client, IAudioClock2_iface); }
static client *stream_volume_of(IAudioStreamVolume *iface) { return CONTAINING_RECORD(iface, client, IAudioStreamVolume_iface); }

static BOOL capturing(const client *audio) { return audio->device->flow == eCapture; }

/* ----- IAudioClient3 ----- */

static HRESULT WINAPI client_QueryInterface(IAudioClient3 *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IAudioClient) || same_guid(riid, &IID_IAudioClient2) ||
		same_guid(riid, &IID_IAudioClient3)) {
		*out = iface;
		IAudioClient3_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI client_AddRef(IAudioClient3 *iface) { return InterlockedIncrement(&client_of(iface)->ref); }

static ULONG WINAPI client_Release(IAudioClient3 *iface)
{
	client *audio = client_of(iface);
	const ULONG ref = InterlockedDecrement(&audio->ref);
	if (ref)
		return ref;
	if (audio->initialized) {
		EnterCriticalSection(&audio_lock);
		for (client **link = &clients; *link; link = &(*link)->next)
			if (*link == audio) {
				*link = audio->next;
				break;
			}
		LeaveCriticalSection(&audio_lock);
		RavenAudioClose(audio->stream);
		if (audio->started)
			session_started(audio->session, FALSE);
		CoTaskMemFree(audio->format);
		if (audio->scratch)
			FREE(audio->scratch);
	}
	DeleteCriticalSection(&audio->lock);
	FREE(audio);
	return 0;
}

static HRESULT WINAPI client_Initialize(IAudioClient3 *iface, AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME duration,
	REFERENCE_TIME periodicity, const WAVEFORMATEX *format, LPCGUID session_guid)
{
	static const DWORD known = AUDCLNT_STREAMFLAGS_CROSSPROCESS | AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
		AUDCLNT_STREAMFLAGS_NOPERSIST | AUDCLNT_STREAMFLAGS_RATEADJUST | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
		AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_SESSIONFLAGS_EXPIREWHENUNOWNED | AUDCLNT_SESSIONFLAGS_DISPLAY_HIDE |
		AUDCLNT_SESSIONFLAGS_DISPLAY_HIDEWHENEXPIRED;
	client *audio = client_of(iface);
	HRESULT result = S_OK;
	if (!format)
		return E_POINTER;
	if ((mode != AUDCLNT_SHAREMODE_SHARED && mode != AUDCLNT_SHAREMODE_EXCLUSIVE) || (flags & ~known) || duration < 0 || periodicity < 0)
		return E_INVALIDARG;
	EnterCriticalSection(&audio->lock);
	if (audio->initialized)
		result = AUDCLNT_E_ALREADY_INITIALIZED;
	else if (mode == AUDCLNT_SHAREMODE_EXCLUSIVE)
		result = AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
	else if (flags & AUDCLNT_STREAMFLAGS_LOOPBACK)
		/* Loopback records what the speakers play; the engine has no such stream yet. */
		result = capturing(audio) ? AUDCLNT_E_WRONG_ENDPOINT_TYPE : AUDCLNT_E_ENDPOINT_CREATE_FAILED;
	else if (!format_valid(format) || (format->nSamplesPerSec != MIX_RATE && !(flags & AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM)))
		result = AUDCLNT_E_UNSUPPORTED_FORMAT;
	if (result != S_OK) {
		LeaveCriticalSection(&audio->lock);
		return result;
	}
	/* Shared mode always runs at the device period; the buffer holds at least three, as Windows' engine gives. */
	const UINT64 rate = format->nSamplesPerSec;
	const UINT32 period = (UINT32)(rate * DEFAULT_PERIOD / 10000000);
	/* Ten seconds is the largest buffer; a longer duration would also overflow the frame count below. */
	if ((UINT64)duration > 10 * 10000000ull) {
		LeaveCriticalSection(&audio->lock);
		return AUDCLNT_E_BUFFER_SIZE_ERROR;
	}
	UINT64 frames = ((UINT64)duration * rate + 9999999) / 10000000;
	if (frames < 3 * (UINT64)period)
		frames = 3 * (UINT64)period;
	/* A capture ring of whole periods, so no packet of a period wraps in it. */
	if (capturing(audio))
		frames = (frames + period - 1) / period * period;
	if (frames > rate * 10) {
		LeaveCriticalSection(&audio->lock);
		return AUDCLNT_E_BUFFER_SIZE_ERROR;
	}
	session *owner;
	result = session_for(audio->device, session_guid, &owner);
	if (FAILED(result)) {
		LeaveCriticalSection(&audio->lock);
		return result;
	}
	audio->format = copy_format(format);
	audio->scratch = capturing(audio) ? NULL : ALLOC((SIZE_T)frames * format->nBlockAlign);
	if (!audio->format || (!capturing(audio) && !audio->scratch)) {
		CoTaskMemFree(audio->format);
		audio->format = NULL;
		if (audio->scratch)
			FREE(audio->scratch);
		audio->scratch = NULL;
		LeaveCriticalSection(&audio->lock);
		return E_OUTOFMEMORY;
	}
	audio->stream = RavenAudioOpen(format, capturing(audio) ? RAVEN_AUDIO_CAPTURE : 0, (DWORD)frames, period, NULL, &audio->shared);
	if (!audio->stream) {
		CoTaskMemFree(audio->format);
		audio->format = NULL;
		if (audio->scratch)
			FREE(audio->scratch);
		audio->scratch = NULL;
		LeaveCriticalSection(&audio->lock);
		return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
	}
	audio->ring = (BYTE *)audio->shared + RAVEN_AUDIO_DATA_OFFSET;
	audio->flags = flags;
	audio->buffer_frames = (UINT32)frames;
	audio->period_frames = period;
	audio->frame_bytes = format->nBlockAlign;
	audio->session = owner;
	for (UINT channel = 0; channel < MAX_CHANNELS; channel++)
		audio->channels[channel] = 1;
	EnterCriticalSection(&audio_lock);
	audio->next = clients;
	clients = audio;
	update_volumes();
	LeaveCriticalSection(&audio_lock);
	InterlockedExchange(&audio->initialized, 1);
	LeaveCriticalSection(&audio->lock);
	return S_OK;
}

static HRESULT WINAPI client_GetBufferSize(IAudioClient3 *iface, UINT32 *frames)
{
	client *audio = client_of(iface);
	if (!frames)
		return E_POINTER;
	if (!audio->initialized)
		return AUDCLNT_E_NOT_INITIALIZED;
	*frames = audio->buffer_frames;
	return S_OK;
}

static HRESULT WINAPI client_GetStreamLatency(IAudioClient3 *iface, REFERENCE_TIME *latency)
{
	client *audio = client_of(iface);
	if (!latency)
		return E_POINTER;
	if (!audio->initialized)
		return AUDCLNT_E_NOT_INITIALIZED;
	*latency = DEFAULT_PERIOD;
	return S_OK;
}

/* The capture packet ready now: a device period of frames, or fewer when the ring wraps first or the stream stopped with less. */
static UINT32 next_packet(client *audio)
{
	const UINT64 read = load64(&audio->shared->read_frames), available = load64(&audio->shared->write_frames) - read;
	const UINT32 contiguous = audio->buffer_frames - (UINT32)(read % audio->buffer_frames);
	UINT32 packet = audio->period_frames;
	if (available < packet)
		packet = (UINT32)available;
	if (contiguous < packet)
		packet = contiguous;
	if (packet < audio->period_frames && packet < contiguous && __atomic_load_n(&audio->shared->running, __ATOMIC_ACQUIRE))
		return 0;
	return packet;
}

/* Lock-free: the counters are the engine's and the program's own, and read whole. */
static HRESULT WINAPI client_GetCurrentPadding(IAudioClient3 *iface, UINT32 *padding)
{
	client *audio = client_of(iface);
	if (!padding)
		return E_POINTER;
	if (!audio->initialized)
		return AUDCLNT_E_NOT_INITIALIZED;
	/* Recorded frames not yet taken, or written frames not yet played. */
	*padding = (UINT32)(load64(&audio->shared->write_frames) - load64(&audio->shared->read_frames));
	return S_OK;
}

static HRESULT WINAPI client_IsFormatSupported(IAudioClient3 *iface, AUDCLNT_SHAREMODE mode, const WAVEFORMATEX *format,
	WAVEFORMATEX **closest)
{
	if (!format)
		return E_POINTER;
	if (mode == AUDCLNT_SHAREMODE_EXCLUSIVE) {
		if (closest)
			*closest = NULL;
		return AUDCLNT_E_UNSUPPORTED_FORMAT;
	}
	if (mode != AUDCLNT_SHAREMODE_SHARED)
		return E_INVALIDARG;
	if (!closest)
		return E_POINTER;
	*closest = NULL;
	if (!format_valid(format))
		return AUDCLNT_E_UNSUPPORTED_FORMAT;
	if (format->nSamplesPerSec == MIX_RATE)
		return S_OK;
	WAVEFORMATEX *match = copy_format(format);
	if (!match)
		return E_OUTOFMEMORY;
	match->nSamplesPerSec = MIX_RATE;
	match->nAvgBytesPerSec = MIX_RATE * match->nBlockAlign;
	*closest = match;
	return S_FALSE;
}

static HRESULT WINAPI client_GetMixFormat(IAudioClient3 *iface, WAVEFORMATEX **format)
{
	if (!format)
		return E_POINTER;
	*format = mix_format();
	return *format ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI client_GetDevicePeriod(IAudioClient3 *iface, REFERENCE_TIME *preferred, REFERENCE_TIME *minimum)
{
	if (!preferred && !minimum)
		return E_POINTER;
	if (preferred)
		*preferred = DEFAULT_PERIOD;
	if (minimum)
		*minimum = MINIMUM_PERIOD;
	return S_OK;
}

static HRESULT WINAPI client_Start(IAudioClient3 *iface)
{
	client *audio = client_of(iface);
	HRESULT result = S_OK;
	EnterCriticalSection(&audio->lock);
	if (!audio->initialized)
		result = AUDCLNT_E_NOT_INITIALIZED;
	else if (audio->started)
		result = AUDCLNT_E_NOT_STOPPED;
	else if ((audio->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !audio->event)
		result = AUDCLNT_E_EVENTHANDLE_NOT_SET;
	else {
		RavenAudioStart(audio->stream);
		audio->started = TRUE;
	}
	LeaveCriticalSection(&audio->lock);
	if (result == S_OK)
		session_started(audio->session, TRUE);
	return result;
}

static HRESULT WINAPI client_Stop(IAudioClient3 *iface)
{
	client *audio = client_of(iface);
	HRESULT result = S_OK;
	EnterCriticalSection(&audio->lock);
	if (!audio->initialized)
		result = AUDCLNT_E_NOT_INITIALIZED;
	else if (!audio->started)
		result = S_FALSE;
	else {
		RavenAudioStop(audio->stream);
		audio->started = FALSE;
	}
	LeaveCriticalSection(&audio->lock);
	if (result == S_OK)
		session_started(audio->session, FALSE);
	return result;
}

static HRESULT WINAPI client_Reset(IAudioClient3 *iface)
{
	client *audio = client_of(iface);
	HRESULT result = S_OK;
	EnterCriticalSection(&audio->lock);
	if (!audio->initialized)
		result = AUDCLNT_E_NOT_INITIALIZED;
	else if (audio->started)
		result = AUDCLNT_E_NOT_STOPPED;
	else if (audio->pending)
		result = AUDCLNT_E_BUFFER_OPERATION_PENDING;
	else {
		RavenAudioReset(audio->stream);
		audio->glitches = audio->last_position = 0;
	}
	LeaveCriticalSection(&audio->lock);
	return result;
}

static HRESULT WINAPI client_SetEventHandle(IAudioClient3 *iface, HANDLE event)
{
	client *audio = client_of(iface);
	HRESULT result = S_OK;
	if (!event)
		return E_INVALIDARG;
	EnterCriticalSection(&audio->lock);
	if (!audio->initialized)
		result = AUDCLNT_E_NOT_INITIALIZED;
	else if (!(audio->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
		result = AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
	else if (!RavenAudioSetEvent(audio->stream, event))
		result = E_INVALIDARG;
	else
		audio->event = event;
	LeaveCriticalSection(&audio->lock);
	return result;
}

static HRESULT WINAPI client_GetService(IAudioClient3 *iface, REFIID riid, void **out)
{
	client *audio = client_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!audio->initialized)
		return AUDCLNT_E_NOT_INITIALIZED;
	if (same_guid(riid, &IID_IAudioRenderClient)) {
		if (capturing(audio))
			return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
		*out = &audio->IAudioRenderClient_iface;
	} else if (same_guid(riid, &IID_IAudioCaptureClient)) {
		if (!capturing(audio))
			return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
		*out = &audio->IAudioCaptureClient_iface;
	} else if (same_guid(riid, &IID_IAudioClock))
		*out = &audio->IAudioClock_iface;
	else if (same_guid(riid, &IID_IAudioClock2))
		*out = &audio->IAudioClock2_iface;
	else if (same_guid(riid, &IID_IAudioStreamVolume))
		*out = &audio->IAudioStreamVolume_iface;
	else if (same_guid(riid, &IID_ISimpleAudioVolume)) {
		*out = &audio->session->ISimpleAudioVolume_iface;
		return S_OK;
	} else if (same_guid(riid, &IID_IAudioSessionControl) || same_guid(riid, &IID_IAudioSessionControl2)) {
		*out = &audio->session->IAudioSessionControl2_iface;
		return S_OK;
	} else if (same_guid(riid, &IID_IChannelAudioVolume)) {
		*out = &audio->session->IChannelAudioVolume_iface;
		return S_OK;
	} else
		return E_NOINTERFACE;
	/* The client's own services keep it alive. */
	IAudioClient3_AddRef(iface);
	return S_OK;
}

static HRESULT WINAPI client_IsOffloadCapable(IAudioClient3 *iface, AUDIO_STREAM_CATEGORY category, BOOL *capable)
{
	if (!capable)
		return E_POINTER;
	*capable = FALSE;
	return S_OK;
}

/* shortcut: the category and options are accepted and dropped, pass them to the host when its streams carry a role. */
static HRESULT WINAPI client_SetClientProperties(IAudioClient3 *iface, const AudioClientProperties *properties)
{
	client *audio = client_of(iface);
	if (!properties)
		return E_POINTER;
	if (properties->cbSize < offsetof(AudioClientProperties, Options))
		return E_INVALIDARG;
	if (properties->bIsOffload)
		return AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE;
	return audio->initialized ? AUDCLNT_E_ALREADY_INITIALIZED : S_OK;
}

static HRESULT WINAPI client_GetBufferSizeLimits(IAudioClient3 *iface, const WAVEFORMATEX *format, BOOL event_driven,
	REFERENCE_TIME *minimum, REFERENCE_TIME *maximum)
{
	if (!format || !minimum || !maximum)
		return E_POINTER;
	if (!format_valid(format))
		return AUDCLNT_E_UNSUPPORTED_FORMAT;
	*minimum = 3 * DEFAULT_PERIOD;
	*maximum = 10 * (REFERENCE_TIME)10000000;
	return S_OK;
}

/* The engine runs at one period, a hundredth of a second of the stream's own rate: the default, the fundamental, the least and the most. */
static HRESULT WINAPI client_GetSharedModeEnginePeriod(IAudioClient3 *iface, const WAVEFORMATEX *format, UINT32 *preferred,
	UINT32 *fundamental, UINT32 *minimum, UINT32 *maximum)
{
	if (!format || !preferred || !fundamental || !minimum || !maximum)
		return E_POINTER;
	if (!format_valid(format))
		return AUDCLNT_E_UNSUPPORTED_FORMAT;
	*preferred = *fundamental = *minimum = *maximum = format->nSamplesPerSec / 100;
	return S_OK;
}

static HRESULT WINAPI client_GetCurrentSharedModeEnginePeriod(IAudioClient3 *iface, WAVEFORMATEX **format, UINT32 *period)
{
	if (!format || !period)
		return E_POINTER;
	*format = mix_format();
	if (!*format)
		return E_OUTOFMEMORY;
	*period = MIX_RATE / 100;
	return S_OK;
}

static HRESULT WINAPI client_InitializeSharedAudioStream(IAudioClient3 *iface, DWORD flags, UINT32 period, const WAVEFORMATEX *format,
	LPCGUID session_guid)
{
	if (!format)
		return E_POINTER;
	if (format_valid(format) && period != format->nSamplesPerSec / 100)
		return AUDCLNT_E_INVALID_DEVICE_PERIOD;
	return client_Initialize(iface, AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, format, session_guid);
}

static const IAudioClient3Vtbl client_vtbl = {
	client_QueryInterface, client_AddRef, client_Release, client_Initialize, client_GetBufferSize, client_GetStreamLatency,
	client_GetCurrentPadding, client_IsFormatSupported, client_GetMixFormat, client_GetDevicePeriod, client_Start, client_Stop,
	client_Reset, client_SetEventHandle, client_GetService, client_IsOffloadCapable, client_SetClientProperties,
	client_GetBufferSizeLimits, client_GetSharedModeEnginePeriod, client_GetCurrentSharedModeEnginePeriod,
	client_InitializeSharedAudioStream,
};

/* ----- IAudioRenderClient ----- */

static HRESULT WINAPI render_QueryInterface(IAudioRenderClient *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IAudioRenderClient)) {
		*out = iface;
		IAudioRenderClient_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI render_AddRef(IAudioRenderClient *iface) { return client_AddRef(&render_of(iface)->IAudioClient3_iface); }
static ULONG WINAPI render_Release(IAudioRenderClient *iface) { return client_Release(&render_of(iface)->IAudioClient3_iface); }

/* Copies frames between a flat block and the ring at a frame counter, wrapping at the end of the ring. */
static void copy_ring(client *audio, UINT64 at, BYTE *flat, UINT32 frames, BOOL into_ring)
{
	const UINT32 start = (UINT32)(at % audio->buffer_frames), first = frames < audio->buffer_frames - start ? frames : audio->buffer_frames - start;
	BYTE *ring = audio->ring + (SIZE_T)start * audio->frame_bytes;
	if (into_ring) {
		memcpy(ring, flat, (SIZE_T)first * audio->frame_bytes);
		memcpy(audio->ring, flat + (SIZE_T)first * audio->frame_bytes, (SIZE_T)(frames - first) * audio->frame_bytes);
	} else {
		memcpy(flat, ring, (SIZE_T)first * audio->frame_bytes);
		memcpy(flat + (SIZE_T)first * audio->frame_bytes, audio->ring, (SIZE_T)(frames - first) * audio->frame_bytes);
	}
}

static HRESULT WINAPI render_GetBuffer(IAudioRenderClient *iface, UINT32 frames, BYTE **data)
{
	client *audio = render_of(iface);
	HRESULT result = S_OK;
	if (!data)
		return E_POINTER;
	*data = NULL;
	EnterCriticalSection(&audio->lock);
	const UINT64 written = load64(&audio->shared->write_frames);
	if (audio->pending)
		result = AUDCLNT_E_OUT_OF_ORDER;
	else if (frames > audio->buffer_frames - (UINT32)(written - load64(&audio->shared->read_frames)))
		result = AUDCLNT_E_BUFFER_TOO_LARGE;
	else if (frames) {
		const UINT32 start = (UINT32)(written % audio->buffer_frames);
		/* Frames that would wrap past the end of the ring are written into scratch and copied in when released. */
		audio->in_scratch = start + frames > audio->buffer_frames;
		*data = audio->in_scratch ? audio->scratch : audio->ring + (SIZE_T)start * audio->frame_bytes;
		audio->pending = frames;
	}
	LeaveCriticalSection(&audio->lock);
	return result;
}

static HRESULT WINAPI render_ReleaseBuffer(IAudioRenderClient *iface, UINT32 frames, DWORD flags)
{
	client *audio = render_of(iface);
	HRESULT result = S_OK;
	if (flags & ~AUDCLNT_BUFFERFLAGS_SILENT)
		return E_INVALIDARG;
	EnterCriticalSection(&audio->lock);
	if (!audio->pending)
		result = frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
	else if (frames > audio->pending)
		result = AUDCLNT_E_INVALID_SIZE;
	else {
		const UINT64 written = load64(&audio->shared->write_frames);
		if (frames) {
			BYTE *data = audio->in_scratch ? audio->scratch
						       : audio->ring + (SIZE_T)(written % audio->buffer_frames) * audio->frame_bytes;
			/* Unsigned 8-bit samples are silent at their midpoint. */
			if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
				memset(data, audio->format->wBitsPerSample == 8 ? 0x80 : 0, (SIZE_T)frames * audio->frame_bytes);
			if (audio->in_scratch)
				copy_ring(audio, written, audio->scratch, frames, TRUE);
			store64(&audio->shared->write_frames, written + frames);
		}
		audio->pending = 0;
	}
	LeaveCriticalSection(&audio->lock);
	return result;
}

static const IAudioRenderClientVtbl render_vtbl = {
	render_QueryInterface, render_AddRef, render_Release, render_GetBuffer, render_ReleaseBuffer,
};

/* ----- IAudioCaptureClient ----- */

static HRESULT WINAPI capture_QueryInterface(IAudioCaptureClient *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IAudioCaptureClient)) {
		*out = iface;
		IAudioCaptureClient_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI capture_AddRef(IAudioCaptureClient *iface) { return client_AddRef(&capture_of(iface)->IAudioClient3_iface); }
static ULONG WINAPI capture_Release(IAudioCaptureClient *iface) { return client_Release(&capture_of(iface)->IAudioClient3_iface); }

static HRESULT WINAPI capture_GetBuffer(IAudioCaptureClient *iface, BYTE **data, UINT32 *frames, DWORD *flags, UINT64 *device_position,
	UINT64 *counter)
{
	client *audio = capture_of(iface);
	HRESULT result = S_OK;
	if (!data || !frames || !flags)
		return E_POINTER;
	*data = NULL;
	*frames = 0;
	*flags = 0;
	EnterCriticalSection(&audio->lock);
	if (audio->pending)
		result = AUDCLNT_E_OUT_OF_ORDER;
	else if (!(audio->pending = next_packet(audio)))
		result = AUDCLNT_S_BUFFER_EMPTY;
	else {
		RavenAudioShared *shared = audio->shared;
		const UINT64 read = load64(&shared->read_frames), written = load64(&shared->write_frames),
			glitches = load64(&shared->glitch_frames);
		*data = audio->ring + (SIZE_T)(read % audio->buffer_frames) * audio->frame_bytes;
		*frames = audio->pending;
		/* Told until the packet is released: one released empty comes again with the same flag. */
		if (glitches != audio->glitches)
			*flags |= AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
		audio->packet_glitches = glitches;
		if (device_position)
			*device_position = read;
		/* The packet's first frame was recorded as long before the newest as the frames after it last. */
		if (counter) {
			const UINT64 stamp = load64(&shared->position_counter), before = (written - read) * 10000000 / shared->rate;
			*counter = stamp > before ? stamp - before : stamp;
		}
	}
	LeaveCriticalSection(&audio->lock);
	return result;
}

static HRESULT WINAPI capture_ReleaseBuffer(IAudioCaptureClient *iface, UINT32 frames)
{
	client *audio = capture_of(iface);
	HRESULT result = S_OK;
	EnterCriticalSection(&audio->lock);
	if (!audio->pending)
		result = frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
	else if (frames && frames != audio->pending)
		result = AUDCLNT_E_INVALID_SIZE;
	else {
		/* Releasing none keeps the packet for the next GetBuffer. */
		if (frames) {
			store64(&audio->shared->read_frames, load64(&audio->shared->read_frames) + frames);
			audio->glitches = audio->packet_glitches;
		}
		audio->pending = 0;
	}
	LeaveCriticalSection(&audio->lock);
	return result;
}

static HRESULT WINAPI capture_GetNextPacketSize(IAudioCaptureClient *iface, UINT32 *frames)
{
	if (!frames)
		return E_POINTER;
	*frames = next_packet(capture_of(iface));
	return S_OK;
}

static const IAudioCaptureClientVtbl capture_vtbl = {
	capture_QueryInterface, capture_AddRef, capture_Release, capture_GetBuffer, capture_ReleaseBuffer, capture_GetNextPacketSize,
};

/* ----- IAudioClock and IAudioClock2 ----- */

static HRESULT WINAPI clock_QueryInterface(IAudioClock *iface, REFIID riid, void **out)
{
	client *audio = clock_of(iface);
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IAudioClock))
		*out = &audio->IAudioClock_iface;
	else if (same_guid(riid, &IID_IAudioClock2))
		*out = &audio->IAudioClock2_iface;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	client_AddRef(&audio->IAudioClient3_iface);
	return S_OK;
}

static ULONG WINAPI clock_AddRef(IAudioClock *iface) { return client_AddRef(&clock_of(iface)->IAudioClient3_iface); }
static ULONG WINAPI clock_Release(IAudioClock *iface) { return client_Release(&clock_of(iface)->IAudioClient3_iface); }

/* Bytes of the stream per second: the clock counts the stream's bytes, as Windows' shared mode does. */
static HRESULT WINAPI clock_GetFrequency(IAudioClock *iface, UINT64 *frequency)
{
	client *audio = clock_of(iface);
	if (!frequency)
		return E_POINTER;
	*frequency = (UINT64)audio->format->nSamplesPerSec * audio->frame_bytes;
	return S_OK;
}

/* The frames the device has played or recorded, carried on from the engine's last update by the time since, never past what was written
 * nor more than a period on, so the position moves smoothly between the engine's updates. */
static UINT64 position_now(client *audio, UINT64 *counter)
{
	RavenAudioShared *shared = audio->shared;
	UINT64 position = load64(&shared->position_frames);
	const UINT64 stamp = load64(&shared->position_counter);
	if (__atomic_load_n(&shared->running, __ATOMIC_ACQUIRE)) {
		const UINT64 now = counter_now();
		UINT64 extra = now > stamp ? (now - stamp) * shared->rate / 10000000 : 0;
		if (extra > audio->period_frames)
			extra = audio->period_frames;
		position += extra;
		if (!capturing(audio)) {
			const UINT64 written = load64(&shared->write_frames);
			if (position > written)
				position = written;
		}
	}
	/* The engine's next update may fall short of a guess past an underrun: the clock never goes back. */
	EnterCriticalSection(&audio->lock);
	if (position < audio->last_position)
		position = audio->last_position;
	audio->last_position = position;
	LeaveCriticalSection(&audio->lock);
	/* The counter is when the position was read, as Windows gives it, running or not. */
	if (counter)
		*counter = counter_now();
	return position;
}

static HRESULT WINAPI clock_GetPosition(IAudioClock *iface, UINT64 *position, UINT64 *counter)
{
	client *audio = clock_of(iface);
	if (!position)
		return E_POINTER;
	*position = position_now(audio, counter) * audio->frame_bytes;
	return S_OK;
}

static HRESULT WINAPI clock_GetCharacteristics(IAudioClock *iface, DWORD *characteristics)
{
	if (!characteristics)
		return E_POINTER;
	*characteristics = 0;
	return S_OK;
}

static const IAudioClockVtbl clock_vtbl = {
	clock_QueryInterface, clock_AddRef, clock_Release, clock_GetFrequency, clock_GetPosition, clock_GetCharacteristics,
};

static HRESULT WINAPI clock2_QueryInterface(IAudioClock2 *iface, REFIID riid, void **out)
{
	return clock_QueryInterface(&clock2_of(iface)->IAudioClock_iface, riid, out);
}
static ULONG WINAPI clock2_AddRef(IAudioClock2 *iface) { return client_AddRef(&clock2_of(iface)->IAudioClient3_iface); }
static ULONG WINAPI clock2_Release(IAudioClock2 *iface) { return client_Release(&clock2_of(iface)->IAudioClient3_iface); }

static HRESULT WINAPI clock2_GetDevicePosition(IAudioClock2 *iface, UINT64 *position, UINT64 *counter)
{
	if (!position)
		return E_POINTER;
	*position = position_now(clock2_of(iface), counter);
	return S_OK;
}

static const IAudioClock2Vtbl clock2_vtbl = {clock2_QueryInterface, clock2_AddRef, clock2_Release, clock2_GetDevicePosition};

/* ----- IAudioStreamVolume ----- */

static HRESULT WINAPI stream_volume_QueryInterface(IAudioStreamVolume *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IAudioStreamVolume)) {
		*out = iface;
		IAudioStreamVolume_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI stream_volume_AddRef(IAudioStreamVolume *iface) { return client_AddRef(&stream_volume_of(iface)->IAudioClient3_iface); }
static ULONG WINAPI stream_volume_Release(IAudioStreamVolume *iface) { return client_Release(&stream_volume_of(iface)->IAudioClient3_iface); }

static HRESULT WINAPI stream_volume_GetChannelCount(IAudioStreamVolume *iface, UINT32 *count)
{
	if (!count)
		return E_POINTER;
	*count = stream_volume_of(iface)->format->nChannels;
	return S_OK;
}


static HRESULT WINAPI stream_volume_SetChannelVolume(IAudioStreamVolume *iface, UINT32 index, const float level)
{
	client *audio = stream_volume_of(iface);
	if (index >= audio->format->nChannels || !level_valid(level))
		return E_INVALIDARG;
	EnterCriticalSection(&audio_lock);
	audio->channels[index] = level;
	update_volumes();
	LeaveCriticalSection(&audio_lock);
	return S_OK;
}

static HRESULT WINAPI stream_volume_GetChannelVolume(IAudioStreamVolume *iface, UINT32 index, float *level)
{
	client *audio = stream_volume_of(iface);
	if (!level)
		return E_POINTER;
	if (index >= audio->format->nChannels)
		return E_INVALIDARG;
	EnterCriticalSection(&audio_lock);
	*level = audio->channels[index];
	LeaveCriticalSection(&audio_lock);
	return S_OK;
}

static HRESULT WINAPI stream_volume_SetAllVolumes(IAudioStreamVolume *iface, UINT32 count, const float *levels)
{
	client *audio = stream_volume_of(iface);
	if (!levels)
		return E_POINTER;
	if (count != audio->format->nChannels)
		return E_INVALIDARG;
	for (UINT32 index = 0; index < count; index++)
		if (!level_valid(levels[index]))
			return E_INVALIDARG;
	EnterCriticalSection(&audio_lock);
	for (UINT32 index = 0; index < count; index++)
		audio->channels[index] = levels[index];
	update_volumes();
	LeaveCriticalSection(&audio_lock);
	return S_OK;
}

static HRESULT WINAPI stream_volume_GetAllVolumes(IAudioStreamVolume *iface, UINT32 count, float *levels)
{
	client *audio = stream_volume_of(iface);
	if (!levels)
		return E_POINTER;
	if (count != audio->format->nChannels)
		return E_INVALIDARG;
	EnterCriticalSection(&audio_lock);
	for (UINT32 index = 0; index < count; index++)
		levels[index] = audio->channels[index];
	LeaveCriticalSection(&audio_lock);
	return S_OK;
}

static const IAudioStreamVolumeVtbl stream_volume_vtbl = {
	stream_volume_QueryInterface, stream_volume_AddRef, stream_volume_Release, stream_volume_GetChannelCount,
	stream_volume_SetChannelVolume, stream_volume_GetChannelVolume, stream_volume_SetAllVolumes, stream_volume_GetAllVolumes,
};

HRESULT create_client(device *device, void **out)
{
	client *audio = ALLOC(sizeof(*audio));
	if (!audio)
		return E_OUTOFMEMORY;
	audio->IAudioClient3_iface.lpVtbl = &client_vtbl;
	audio->IAudioRenderClient_iface.lpVtbl = &render_vtbl;
	audio->IAudioCaptureClient_iface.lpVtbl = &capture_vtbl;
	audio->IAudioClock_iface.lpVtbl = &clock_vtbl;
	audio->IAudioClock2_iface.lpVtbl = &clock2_vtbl;
	audio->IAudioStreamVolume_iface.lpVtbl = &stream_volume_vtbl;
	audio->ref = 1;
	audio->device = device;
	InitializeCriticalSection(&audio->lock);
	*out = &audio->IAudioClient3_iface;
	return S_OK;
}
