#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stddef.h>

#include "../../native/nt/raven_audio.h"

/* mmdevapi.dll as programs meet it, against the device that plays to nowhere in real time (RAVEN_AUDIO=null):
 * - the enumerator COM creates, its two endpoints, their ids, states, flows and properties;
 * - an event-driven render client paced by the engine, its padding and clock, and the errors of every call out of order; the frames a
 *   program writes reach the shared ring, those that wrap past its end and those released silent too;
 * - a capture client's packets of a period, their positions, and the discontinuity after the ring overflowed;
 * - the stream, session and endpoint volumes, the callbacks they tell, and the gain they leave on the stream's page;
 * - IAudioClient3's engine periods and ActivateAudioInterfaceAsync, by interface class, by endpoint id and by device interface path. */

#define E_NOTFOUND_ HRESULT_FROM_WIN32(ERROR_NOT_FOUND)

static const GUID subtype_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/* No C runtime is linked; zeroing and copying structures still ask for these. */
void *memset(void *target, int value, size_t size)
{
	volatile unsigned char *bytes = target;
	while (size--)
		*bytes++ = (unsigned char)value;
	return target;
}

void *memcpy(void *target, const void *source, size_t size)
{
	volatile unsigned char *to = target;
	const unsigned char *from = source;
	while (size--)
		*to++ = *from++;
	return target;
}

int memcmp(const void *left, const void *right, size_t size)
{
	const unsigned char *a = left, *b = right;
	for (; size; size--, a++, b++)
		if (*a != *b)
			return *a - *b;
	return 0;
}

static int same_text(const WCHAR *left, const WCHAR *right)
{
	while (*left && *left == *right)
		left++, right++;
	return *left == *right;
}

static BOOL close_to(float value, float expected) { return value > expected - 0.001f && value < expected + 0.001f; }

/* The performance counter in the 100-nanosecond units of IAudioClock's and IAudioCaptureClient's timestamps. */
static UINT64 counter_now(void)
{
	LARGE_INTEGER counter, frequency;
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	return (UINT64)(counter.QuadPart / frequency.QuadPart) * 10000000 +
		(UINT64)(counter.QuadPart % frequency.QuadPart) * 10000000 / (UINT64)frequency.QuadPart;
}

static float page_volume(RavenAudioShared *page)
{
	union {
		UINT32 bits;
		float level;
	} volume = {__atomic_load_n(&page->volume, __ATOMIC_ACQUIRE)};
	return volume.level;
}

static WAVEFORMATEX format_of(WORD tag, DWORD rate, WORD channels, WORD bits)
{
	WAVEFORMATEX format = {0};
	format.wFormatTag = tag;
	format.nChannels = channels;
	format.nSamplesPerSec = rate;
	format.wBitsPerSample = bits;
	format.nBlockAlign = (WORD)(channels * bits / 8);
	format.nAvgBytesPerSec = rate * format.nBlockAlign;
	return format;
}

/* ----- Listeners ----- */

typedef struct session_listener {
	IAudioSessionEvents IAudioSessionEvents_iface;
	LONG ref, volumes, active, inactive;
	float volume;
} session_listener;

static HRESULT WINAPI session_QueryInterface(IAudioSessionEvents *iface, REFIID riid, void **out)
{
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IAudioSessionEvents)) {
		*out = iface;
		IAudioSessionEvents_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG WINAPI session_AddRef(IAudioSessionEvents *iface) { return InterlockedIncrement(&((session_listener *)iface)->ref); }
static ULONG WINAPI session_Release(IAudioSessionEvents *iface) { return InterlockedDecrement(&((session_listener *)iface)->ref); }
static HRESULT WINAPI session_OnDisplayNameChanged(IAudioSessionEvents *iface, LPCWSTR name, LPCGUID context) { return S_OK; }
static HRESULT WINAPI session_OnIconPathChanged(IAudioSessionEvents *iface, LPCWSTR path, LPCGUID context) { return S_OK; }
static HRESULT WINAPI session_OnSimpleVolumeChanged(IAudioSessionEvents *iface, float volume, BOOL mute, LPCGUID context)
{
	session_listener *listener = (session_listener *)iface;
	listener->volume = volume;
	listener->volumes++;
	return S_OK;
}
static HRESULT WINAPI session_OnChannelVolumeChanged(IAudioSessionEvents *iface, DWORD count, float levels[], DWORD channel, LPCGUID context)
{
	return S_OK;
}
static HRESULT WINAPI session_OnGroupingParamChanged(IAudioSessionEvents *iface, LPCGUID grouping, LPCGUID context) { return S_OK; }
static HRESULT WINAPI session_OnStateChanged(IAudioSessionEvents *iface, AudioSessionState state)
{
	session_listener *listener = (session_listener *)iface;
	if (state == AudioSessionStateActive)
		listener->active++;
	else if (state == AudioSessionStateInactive)
		listener->inactive++;
	return S_OK;
}
static HRESULT WINAPI session_OnSessionDisconnected(IAudioSessionEvents *iface, AudioSessionDisconnectReason reason) { return S_OK; }

static IAudioSessionEventsVtbl session_listener_vtbl = {
	session_QueryInterface, session_AddRef, session_Release, session_OnDisplayNameChanged, session_OnIconPathChanged,
	session_OnSimpleVolumeChanged, session_OnChannelVolumeChanged, session_OnGroupingParamChanged, session_OnStateChanged,
	session_OnSessionDisconnected,
};

typedef struct endpoint_listener {
	IAudioEndpointVolumeCallback IAudioEndpointVolumeCallback_iface;
	LONG ref, notified;
	float master;
	BOOL muted;
	GUID context;
} endpoint_listener;

static HRESULT WINAPI endpoint_QueryInterface(IAudioEndpointVolumeCallback *iface, REFIID riid, void **out)
{
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IAudioEndpointVolumeCallback)) {
		*out = iface;
		IAudioEndpointVolumeCallback_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG WINAPI endpoint_AddRef(IAudioEndpointVolumeCallback *iface) { return InterlockedIncrement(&((endpoint_listener *)iface)->ref); }
static ULONG WINAPI endpoint_Release(IAudioEndpointVolumeCallback *iface) { return InterlockedDecrement(&((endpoint_listener *)iface)->ref); }
static HRESULT WINAPI endpoint_OnNotify(IAudioEndpointVolumeCallback *iface, PAUDIO_VOLUME_NOTIFICATION_DATA data)
{
	endpoint_listener *listener = (endpoint_listener *)iface;
	listener->master = data->fMasterVolume;
	listener->muted = data->bMuted;
	listener->context = data->guidEventContext;
	listener->notified++;
	return S_OK;
}

static IAudioEndpointVolumeCallbackVtbl endpoint_listener_vtbl = {endpoint_QueryInterface, endpoint_AddRef, endpoint_Release, endpoint_OnNotify};

typedef struct completion {
	IActivateAudioInterfaceCompletionHandler IActivateAudioInterfaceCompletionHandler_iface;
	LONG ref;
	HANDLE done;
	DWORD thread;
} completion;

static HRESULT WINAPI completion_QueryInterface(IActivateAudioInterfaceCompletionHandler *iface, REFIID riid, void **out)
{
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IActivateAudioInterfaceCompletionHandler) ||
		IsEqualGUID(riid, &IID_IAgileObject)) {
		*out = iface;
		IActivateAudioInterfaceCompletionHandler_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG WINAPI completion_AddRef(IActivateAudioInterfaceCompletionHandler *iface) { return InterlockedIncrement(&((completion *)iface)->ref); }
static ULONG WINAPI completion_Release(IActivateAudioInterfaceCompletionHandler *iface) { return InterlockedDecrement(&((completion *)iface)->ref); }
static HRESULT WINAPI completion_ActivateCompleted(IActivateAudioInterfaceCompletionHandler *iface, IActivateAudioInterfaceAsyncOperation *operation)
{
	completion *handler = (completion *)iface;
	handler->thread = GetCurrentThreadId();
	SetEvent(handler->done);
	return S_OK;
}

static IActivateAudioInterfaceCompletionHandlerVtbl completion_vtbl = {
	completion_QueryInterface, completion_AddRef, completion_Release, completion_ActivateCompleted,
};

/* ----- Endpoints ----- */

static IMMDeviceEnumerator *enumerator;
static IMMDevice *speakers, *microphone;

static int endpoints(void)
{
	if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&enumerator))) return 1;
	IMMDeviceCollection *all = NULL, *rendering = NULL;
	UINT count = 0;
	if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eAll, DEVICE_STATE_ACTIVE, &all)) ||
		FAILED(IMMDeviceCollection_GetCount(all, &count)) || count != 2) return 2;
	if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eRender, DEVICE_STATEMASK_ALL, &rendering)) ||
		FAILED(IMMDeviceCollection_GetCount(rendering, &count)) || count != 1) return 3;
	IMMDevice *listed = NULL;
	if (IMMDeviceCollection_Item(all, 2, &listed) != E_INVALIDARG || listed) return 4;
	if (FAILED(IMMDeviceCollection_Item(rendering, 0, &listed))) return 4;
	IMMDeviceCollection_Release(rendering);
	IMMDeviceCollection_Release(all);

	if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &speakers)) ||
		FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eCapture, eCommunications, &microphone))) return 5;
	IMMDevice *none = NULL;
	if (IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eAll, eConsole, &none) != E_INVALIDARG || none) return 6;
	WCHAR *id = NULL, *listed_id = NULL, *capture_id = NULL;
	if (FAILED(IMMDevice_GetId(speakers, &id)) || FAILED(IMMDevice_GetId(listed, &listed_id)) || !same_text(id, listed_id)) return 7;
	if (FAILED(IMMDevice_GetId(microphone, &capture_id)) || same_text(id, capture_id)) return 7;
	IMMDevice *found = NULL;
	if (FAILED(IMMDeviceEnumerator_GetDevice(enumerator, capture_id, &found)) || found != microphone) return 8;
	if (IMMDeviceEnumerator_GetDevice(enumerator, L"{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}", &found) != E_NOTFOUND_ || found)
		return 9;
	CoTaskMemFree(id);
	CoTaskMemFree(listed_id);
	CoTaskMemFree(capture_id);

	DWORD state = 0;
	IMMEndpoint *endpoint = NULL;
	EDataFlow flow = eAll;
	if (FAILED(IMMDevice_GetState(speakers, &state)) || state != DEVICE_STATE_ACTIVE) return 10;
	if (FAILED(IMMDevice_QueryInterface(microphone, &IID_IMMEndpoint, (void **)&endpoint)) || FAILED(IMMEndpoint_GetDataFlow(endpoint, &flow)) ||
		flow != eCapture) return 11;
	IMMEndpoint_Release(endpoint);

	IPropertyStore *store = NULL;
	PROPVARIANT value;
	PropVariantInit(&value);
	if (FAILED(IMMDevice_OpenPropertyStore(speakers, STGM_READ, &store))) return 12;
	if (FAILED(IPropertyStore_GetValue(store, &PKEY_Device_FriendlyName, &value)) || value.vt != VT_LPWSTR || !value.pwszVal[0]) return 13;
	PropVariantClear(&value);
	if (FAILED(IPropertyStore_GetValue(store, &PKEY_AudioEndpoint_FormFactor, &value)) || value.vt != VT_UI4 || value.ulVal != Speakers) return 14;
	if (FAILED(IPropertyStore_GetValue(store, &PKEY_AudioEngine_DeviceFormat, &value)) || value.vt != VT_BLOB ||
		value.blob.cbSize < sizeof(WAVEFORMATEX) || ((WAVEFORMATEX *)value.blob.pBlobData)->nSamplesPerSec != 48000) return 15;
	PropVariantClear(&value);
	IPropertyStore_Release(store);
	return 0;
}

/* ----- Render ----- */

static IAudioClient *activate(IMMDevice *device, REFIID riid)
{
	IAudioClient *client = NULL;
	return SUCCEEDED(IMMDevice_Activate(device, riid, CLSCTX_ALL, NULL, (void **)&client)) ? client : NULL;
}

static float *ring;
static RavenAudioShared *page;
static UINT32 buffer_frames;
static UINT64 written;

/* Writes frames numbered by the stream's frame counter into the buffer, two channels of each. */
static HRESULT write_frames(IAudioRenderClient *render, UINT32 frames)
{
	BYTE *data = NULL;
	HRESULT result = IAudioRenderClient_GetBuffer(render, frames, &data);
	if (FAILED(result))
		return result;
	float *samples = (float *)data;
	for (UINT32 frame = 0; frame < frames; frame++)
		samples[frame * 2] = samples[frame * 2 + 1] = (float)(written + frame);
	result = IAudioRenderClient_ReleaseBuffer(render, frames, 0);
	if (SUCCEEDED(result))
		written += frames;
	return result;
}

static IAudioClient *speaker_client;
static IAudioRenderClient *render;
static session_listener sessions = {{&session_listener_vtbl}, 1};

static int playback(void)
{
	IAudioClient *client = activate(speakers, &IID_IAudioClient);
	if (!client) return 20;
	speaker_client = client;
	WAVEFORMATEX *mix = NULL;
	if (FAILED(IAudioClient_GetMixFormat(client, &mix)) || mix->wFormatTag != WAVE_FORMAT_EXTENSIBLE || mix->nSamplesPerSec != 48000 ||
		mix->nChannels != 2 || mix->wBitsPerSample != 32 || mix->cbSize < 22 ||
		!IsEqualGUID(&((WAVEFORMATEXTENSIBLE *)mix)->SubFormat, &subtype_float)) return 21;

	/* The shared engine runs at its own rate: another one is answered with the closest the engine takes. */
	WAVEFORMATEX cd = format_of(WAVE_FORMAT_PCM, 44100, 2, 16), *closest = NULL;
	if (IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED, &cd, &closest) != S_FALSE || !closest ||
		closest->nSamplesPerSec != 48000 || closest->nChannels != 2 || closest->wBitsPerSample != 16) return 22;
	CoTaskMemFree(closest);
	WAVEFORMATEX pcm = format_of(WAVE_FORMAT_PCM, 48000, 2, 16), odd = format_of(WAVE_FORMAT_PCM, 48000, 2, 12);
	if (IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED, &pcm, &closest) != S_OK || closest) return 23;
	if (IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED, &odd, &closest) != AUDCLNT_E_UNSUPPORTED_FORMAT) return 23;
	if (IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_EXCLUSIVE, mix, NULL) != AUDCLNT_E_UNSUPPORTED_FORMAT) return 24;
	/* A plain format's cbSize describes nothing that follows it: a wild one, at the very end of its memory, is never read past. */
	{
		BYTE *page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!page) return 24;
		WAVEFORMATEX *wild = (WAVEFORMATEX *)(page + 4096 - sizeof(WAVEFORMATEX));
		*wild = format_of(WAVE_FORMAT_IEEE_FLOAT, 44100, 2, 32);
		wild->cbSize = 0xFFFF;
		if (IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED, wild, &closest) != S_FALSE || !closest ||
			closest->cbSize || closest->wFormatTag != WAVE_FORMAT_IEEE_FLOAT || closest->nSamplesPerSec != 48000) return 24;
		CoTaskMemFree(closest);
		closest = NULL;
		VirtualFree(page, 0, MEM_RELEASE);
	}
	REFERENCE_TIME preferred = 0, minimum = 0;
	if (FAILED(IAudioClient_GetDevicePeriod(client, &preferred, &minimum)) || preferred != 100000 || !minimum || minimum > preferred) return 25;

	/* Nothing runs before Initialize, and a stream runs in shared mode at the engine's rate. */
	UINT32 frames = 0;
	if (IAudioClient_GetBufferSize(client, &frames) != AUDCLNT_E_NOT_INITIALIZED || IAudioClient_Start(client) != AUDCLNT_E_NOT_INITIALIZED)
		return 26;
	if (IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 100000, 100000, mix, NULL) != AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
		return 27;
	if (IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, mix, NULL) == S_OK) return 27;
	if (IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, &cd, NULL) != AUDCLNT_E_UNSUPPORTED_FORMAT) return 28;
	/* A duration whose frame count does not fit 64 bits is refused, not wrapped into a small buffer. */
	if (IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, 0x7fffffffffffffffll, 0, mix, NULL) != AUDCLNT_E_BUFFER_SIZE_ERROR ||
		IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, 100000001, 0, mix, NULL) != AUDCLNT_E_BUFFER_SIZE_ERROR)
		return 199;
	if (IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, NULL) != S_OK) return 29;
	if (IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, NULL) !=
		AUDCLNT_E_ALREADY_INITIALIZED) return 30;
	/* Three periods of 10 ms at least. */
	if (FAILED(IAudioClient_GetBufferSize(client, &buffer_frames)) || buffer_frames != 1440) return 31;
	if (IAudioClient_Start(client) != AUDCLNT_E_EVENTHANDLE_NOT_SET) return 32;
	const HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (IAudioClient_SetEventHandle(client, event) != S_OK) return 33;

	IAudioClock *clock = NULL;
	IAudioCaptureClient *wrong = NULL;
	if (FAILED(IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render)) ||
		FAILED(IAudioClient_GetService(client, &IID_IAudioClock, (void **)&clock))) return 34;
	if (IAudioClient_GetService(client, &IID_IAudioCaptureClient, (void **)&wrong) != AUDCLNT_E_WRONG_ENDPOINT_TYPE || wrong) return 34;
	UINT64 frequency = 0, position = 0;
	if (FAILED(IAudioClock_GetFrequency(clock, &frequency)) || frequency != 48000 * 8) return 35;
	UINT64 stamp = 0;
	if (FAILED(IAudioClock_GetPosition(clock, &position, &stamp)) || position || !stamp || stamp > counter_now()) return 35;
	IAudioClock2 *clock2 = NULL;
	if (FAILED(IAudioClient_GetService(client, &IID_IAudioClock2, (void **)&clock2)) || !clock2) return 35;
	IAudioClock2_Release(clock2);

	/* The first buffer starts the ring, and its address is that of the stream's page. */
	BYTE *data = NULL;
	if (IAudioRenderClient_GetBuffer(render, buffer_frames + 1, &data) != AUDCLNT_E_BUFFER_TOO_LARGE || data) return 36;
	if (FAILED(IAudioRenderClient_GetBuffer(render, buffer_frames, &data)) || !data) return 37;
	ring = (float *)data;
	page = (RavenAudioShared *)(data - RAVEN_AUDIO_DATA_OFFSET);
	if (page->buffer_frames != buffer_frames || page->period_frames != 480 || page->rate != 48000 || page->frame_bytes != 8) return 38;
	if (IAudioRenderClient_GetBuffer(render, 10, &data) != AUDCLNT_E_OUT_OF_ORDER) return 39;
	if (IAudioRenderClient_ReleaseBuffer(render, buffer_frames + 1, 0) != AUDCLNT_E_INVALID_SIZE) return 39;
	if (IAudioRenderClient_ReleaseBuffer(render, buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT) != S_OK) return 40;
	written = buffer_frames;
	if (page->write_frames != buffer_frames || ring[0] != 0 || ring[buffer_frames * 2 - 1] != 0) return 40;
	UINT32 padding = 0;
	if (FAILED(IAudioClient_GetCurrentPadding(client, &padding)) || padding != buffer_frames) return 41;
	if (IAudioRenderClient_GetBuffer(render, 1, &data) != AUDCLNT_E_BUFFER_TOO_LARGE) return 41;

	/* Half a second played as a program does it: woken each period, it fills what was played. */
	IAudioSessionControl *control = NULL;
	if (FAILED(IAudioClient_GetService(client, &IID_IAudioSessionControl, (void **)&control)) ||
		FAILED(IAudioSessionControl_RegisterAudioSessionNotification(control, &sessions.IAudioSessionEvents_iface))) return 42;
	/* A session is one object: its control and its volumes reach each other, also when found through the session enumerator. */
	{
		ISimpleAudioVolume *simple = NULL;
		IChannelAudioVolume *channels = NULL;
		IAudioSessionControl2 *control2 = NULL;
		IUnknown *first = NULL, *second = NULL;
		if (FAILED(IAudioSessionControl_QueryInterface(control, &IID_ISimpleAudioVolume, (void **)&simple)) ||
			FAILED(ISimpleAudioVolume_QueryInterface(simple, &IID_IChannelAudioVolume, (void **)&channels)) ||
			FAILED(IChannelAudioVolume_QueryInterface(channels, &IID_IAudioSessionControl2, (void **)&control2)) ||
			FAILED(IAudioSessionControl_QueryInterface(control, &IID_IUnknown, (void **)&first)) ||
			FAILED(ISimpleAudioVolume_QueryInterface(simple, &IID_IUnknown, (void **)&second)) || first != second) return 62;
		IUnknown_Release(first);
		IUnknown_Release(second);
		IAudioSessionControl2_Release(control2);
		IChannelAudioVolume_Release(channels);
		ISimpleAudioVolume_Release(simple);
		IAudioSessionManager2 *manager = NULL;
		IAudioSessionEnumerator *list = NULL;
		IAudioSessionControl *listed = NULL;
		int count = 0;
		if (FAILED(IMMDevice_Activate(speakers, &IID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void **)&manager)) ||
			FAILED(IAudioSessionManager2_GetSessionEnumerator(manager, &list)) || FAILED(IAudioSessionEnumerator_GetCount(list, &count)) ||
			count < 1 || FAILED(IAudioSessionEnumerator_GetSession(list, 0, &listed)) ||
			FAILED(IAudioSessionControl_QueryInterface(listed, &IID_ISimpleAudioVolume, (void **)&simple))) return 63;
		ISimpleAudioVolume_Release(simple);
		IAudioSessionControl_Release(listed);
		IAudioSessionEnumerator_Release(list);
		IAudioSessionManager2_Release(manager);
	}
	const DWORD start = GetTickCount();
	const UINT64 before_start = counter_now();
	if (IAudioClient_Start(client) != S_OK || !page->running || sessions.active != 1) return 43;
	/* Just started, the clock has moved no further than the time since allows: it does not leap a period from a stale stamp. */
	if (FAILED(IAudioClock_GetPosition(clock, &position, &stamp)) || stamp < before_start ||
		position / 8 > (stamp - before_start) * 48000 / 10000000 + 1) return 43;
	if (IAudioClient_Start(client) != AUDCLNT_E_NOT_STOPPED || IAudioClient_Reset(client) != AUDCLNT_E_NOT_STOPPED) return 44;
	int wakes = 0;
	UINT64 last = 0;
	while (written < 24000) {
		if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) return 45;
		++wakes;
		if (FAILED(IAudioClient_GetCurrentPadding(client, &padding)) || padding > buffer_frames) return 46;
		if (padding < buffer_frames && FAILED(write_frames(render, buffer_frames - padding))) return 47;
		UINT64 counter = 0;
		if (FAILED(IAudioClock_GetPosition(clock, &position, &counter)) || position < last || position > written * 8 || !counter) return 48;
		last = position;
	}
	const DWORD took = GetTickCount() - start;
	if (wakes < 20 || took < 350 || took > 3000) return 49;
	if (!last) return 50;

	/* Stopped, the clock stays; the frames played and written from counters that never wrap. */
	if (IAudioClient_Stop(client) != S_OK || page->running || sessions.inactive != 1) return 51;
	if (IAudioClient_Stop(client) != S_FALSE) return 51;
	UINT64 stopped = 0;
	Sleep(30);
	if (FAILED(IAudioClock_GetPosition(clock, &stopped, NULL)) || stopped < last || stopped > written * 8) return 52;
	if (FAILED(IAudioClock_GetPosition(clock, &position, NULL)) || position != stopped) return 52;

	/* A pending buffer keeps the stream from being reset; reset, the ring and the clock start again from nothing. */
	if (IAudioClient_Reset(client) != S_OK || page->write_frames || page->read_frames || page->position_frames) return 54;
	if (FAILED(IAudioClock_GetPosition(clock, &position, NULL)) || position) return 54;
	if (FAILED(IAudioRenderClient_GetBuffer(render, 1, &data)) || IAudioClient_Reset(client) != AUDCLNT_E_BUFFER_OPERATION_PENDING) return 53;
	if (IAudioRenderClient_ReleaseBuffer(render, 0, 0) != S_OK || IAudioClient_Reset(client) != S_OK) return 53;
	written = 0;

	/* Frames written across the end of the ring land on both sides of it, and those released silent are silence. */
	if (FAILED(write_frames(render, 1000))) return 55;
	if (IAudioClient_Start(client) != S_OK) return 55;
	while (page->read_frames < 600)
		if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) return 56;
	if (IAudioClient_Stop(client) != S_OK) return 56;
	if (FAILED(IAudioRenderClient_GetBuffer(render, 600, &data)) || (float *)data == ring + 2000) return 57;
	float *samples = (float *)data;
	for (UINT32 frame = 0; frame < 600; frame++)
		samples[frame * 2] = samples[frame * 2 + 1] = (float)(1000 + frame);
	if (FAILED(IAudioRenderClient_ReleaseBuffer(render, 600, 0))) return 57;
	for (UINT32 frame = 0; frame < 600; frame++) {
		const UINT32 at = (1000 + frame) % buffer_frames;
		if (ring[at * 2] != (float)(1000 + frame) || ring[at * 2 + 1] != (float)(1000 + frame)) return 58;
	}
	if (ring[(buffer_frames - 1) * 2] != (float)(buffer_frames - 1) || ring[0] != (float)buffer_frames) return 58;
	if (FAILED(IAudioRenderClient_GetBuffer(render, 100, &data)) || (float *)data != ring + (1600 % buffer_frames) * 2) return 59;
	memset(data, 0x7f, 100 * 8);
	if (FAILED(IAudioRenderClient_ReleaseBuffer(render, 100, AUDCLNT_BUFFERFLAGS_SILENT))) return 59;
	for (UINT32 frame = 0; frame < 100 * 2; frame++)
		if (ring[(1600 % buffer_frames) * 2 + frame] != 0) return 60;
	if (IAudioRenderClient_ReleaseBuffer(render, 0, 0) != S_OK || IAudioRenderClient_ReleaseBuffer(render, 1, 0) != AUDCLNT_E_OUT_OF_ORDER) return 61;

	IAudioClock_Release(clock);
	IAudioSessionControl_Release(control);
	CloseHandle(event);
	CoTaskMemFree(mix);
	return 0;
}

/* ----- Volumes ----- */

static int volumes(void)
{
	ISimpleAudioVolume *simple = NULL;
	IAudioStreamVolume *stream = NULL;
	IAudioEndpointVolume *endpoint = NULL;
	if (FAILED(IAudioClient_GetService(speaker_client, &IID_ISimpleAudioVolume, (void **)&simple)) ||
		FAILED(IAudioClient_GetService(speaker_client, &IID_IAudioStreamVolume, (void **)&stream)) ||
		FAILED(IMMDevice_Activate(speakers, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&endpoint))) return 70;
	if (!close_to(page_volume(page), 1)) return 71;

	/* The session's volume, told to its listeners with the caller's context. */
	float level = 0;
	if (ISimpleAudioVolume_SetMasterVolume(simple, 1.5f, NULL) != E_INVALIDARG) return 72;
	if (FAILED(ISimpleAudioVolume_SetMasterVolume(simple, 0.5f, NULL)) || FAILED(ISimpleAudioVolume_GetMasterVolume(simple, &level)) ||
		!close_to(level, 0.5f)) return 73;
	if (sessions.volumes != 1 || !close_to(sessions.volume, 0.5f) || !close_to(page_volume(page), 0.5f)) return 74;
	/* A stream that starts in a session already turned down plays at its volume from its first frame. */
	IAudioClient *late = activate(speakers, &IID_IAudioClient);
	IAudioRenderClient *late_render = NULL;
	BYTE *late_data = NULL;
	const WAVEFORMATEX stereo = format_of(WAVE_FORMAT_IEEE_FLOAT, 48000, 2, 32);
	if (!late || FAILED(IAudioClient_Initialize(late, AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, &stereo, NULL)) ||
		FAILED(IAudioClient_GetService(late, &IID_IAudioRenderClient, (void **)&late_render)) ||
		FAILED(IAudioRenderClient_GetBuffer(late_render, 1, &late_data))) return 88;
	if (!close_to(page_volume((RavenAudioShared *)(late_data - RAVEN_AUDIO_DATA_OFFSET)), 0.5f)) return 88;
	if (IAudioRenderClient_ReleaseBuffer(late_render, 0, 0) != S_OK) return 88;
	IAudioRenderClient_Release(late_render);
	IAudioClient_Release(late);

	/* The endpoint's, in scalar and in decibels, and its mute. */
	static const GUID context = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
	endpoint_listener listener = {{&endpoint_listener_vtbl}, 1};
	if (FAILED(IAudioEndpointVolume_RegisterControlChangeNotify(endpoint, &listener.IAudioEndpointVolumeCallback_iface))) return 75;
	if (FAILED(IAudioEndpointVolume_SetMasterVolumeLevelScalar(endpoint, 0.25f, &context))) return 76;
	if (listener.notified != 1 || !close_to(listener.master, 0.25f) || listener.muted || !IsEqualGUID(&listener.context, &context)) return 77;
	float decibels = 0, low = 0, high = 0, step = 0;
	if (FAILED(IAudioEndpointVolume_GetMasterVolumeLevel(endpoint, &decibels)) || decibels > -12.0f || decibels < -12.1f) return 78;
	if (FAILED(IAudioEndpointVolume_GetVolumeRange(endpoint, &low, &high, &step)) || high != 0 || low >= 0 || step <= 0) return 78;
	if (!close_to(page_volume(page), 0.125f)) return 79;
	if (FAILED(IAudioEndpointVolume_SetMute(endpoint, TRUE, NULL)) || listener.notified != 2 || !listener.muted) return 80;
	BOOL muted = FALSE;
	if (FAILED(IAudioEndpointVolume_GetMute(endpoint, &muted)) || !muted || page_volume(page) != 0) return 80;
	if (FAILED(IAudioEndpointVolume_SetMute(endpoint, FALSE, NULL)) || FAILED(IAudioEndpointVolume_SetMasterVolumeLevelScalar(endpoint, 1, NULL)))
		return 81;
	if (FAILED(IAudioEndpointVolume_UnregisterControlChangeNotify(endpoint, &listener.IAudioEndpointVolumeCallback_iface))) return 81;
	if (FAILED(IAudioEndpointVolume_SetMasterVolumeLevelScalar(endpoint, 1, NULL)) || listener.notified != 4 || listener.ref != 1) return 82;

	/* The stream's own channels. */
	UINT32 count = 0;
	const float levels[2] = {1, 0};
	float read[2] = {0};
	if (FAILED(IAudioStreamVolume_GetChannelCount(stream, &count)) || count != 2) return 83;
	if (IAudioStreamVolume_SetChannelVolume(stream, 2, 1) != E_INVALIDARG || IAudioStreamVolume_SetAllVolumes(stream, 1, levels) != E_INVALIDARG)
		return 84;
	if (FAILED(IAudioStreamVolume_SetAllVolumes(stream, 2, levels)) || FAILED(IAudioStreamVolume_GetAllVolumes(stream, 2, read)) ||
		read[0] != 1 || read[1] != 0) return 85;
	if (!close_to(page_volume(page), 0.25f)) return 86;
	const float full[2] = {1, 1};
	if (FAILED(IAudioStreamVolume_SetAllVolumes(stream, 2, full)) || FAILED(ISimpleAudioVolume_SetMasterVolume(simple, 1, NULL)) ||
		!close_to(page_volume(page), 1)) return 87;

	ISimpleAudioVolume_Release(simple);
	IAudioStreamVolume_Release(stream);
	IAudioRenderClient_Release(render);
	IAudioClient_Release(speaker_client);
	return 0;
}

/* ----- Capture ----- */

static int recording(void)
{
	IAudioClient *client = activate(microphone, &IID_IAudioClient);
	if (!client) return 100;
	WAVEFORMATEX format = format_of(WAVE_FORMAT_PCM, 48000, 1, 16);
	if (IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, &format, NULL) !=
		AUDCLNT_E_WRONG_ENDPOINT_TYPE) return 101;
	if (IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, &format, NULL) != S_OK) return 102;
	const HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (IAudioClient_SetEventHandle(client, event) != AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED) return 103;
	IAudioCaptureClient *capture = NULL;
	IAudioRenderClient *wrong = NULL;
	if (FAILED(IAudioClient_GetService(client, &IID_IAudioCaptureClient, (void **)&capture))) return 104;
	if (IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&wrong) != AUDCLNT_E_WRONG_ENDPOINT_TYPE) return 104;
	UINT32 frames = 0, buffer = 0, padding = 0;
	DWORD flags = 0;
	BYTE *data = NULL;
	UINT64 device_position = 0, counter = 0;
	if (FAILED(IAudioClient_GetBufferSize(client, &buffer)) || buffer != 1440) return 105;
	if (IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL) != AUDCLNT_S_BUFFER_EMPTY || data || frames) return 106;

	/* Polled, packets come a period at a time, each where the last ended. */
	if (IAudioClient_Start(client) != S_OK) return 107;
	UINT64 expected = 0;
	const DWORD start = GetTickCount();
	BOOL kept = FALSE;
	while (expected < 4800) {
		if (GetTickCount() - start > 3000) return 108;
		if (FAILED(IAudioCaptureClient_GetNextPacketSize(capture, &frames))) return 108;
		if (!frames) {
			Sleep(2);
			continue;
		}
		if (FAILED(IAudioClient_GetCurrentPadding(client, &padding)) || padding < frames || padding > buffer) return 109;
		if (IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, &device_position, &counter) != S_OK) return 110;
		if (frames != 480 || device_position != expected || flags || !counter || counter > counter_now() || ((short *)data)[0] ||
			((short *)data)[479]) return 111;
		if (IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL) != AUDCLNT_E_OUT_OF_ORDER) return 112;
		if (IAudioCaptureClient_ReleaseBuffer(capture, 1) != AUDCLNT_E_INVALID_SIZE) return 112;
		/* Released empty, the packet is the next one again. */
		if (expected == 960 && !kept) {
			kept = TRUE;
			if (IAudioCaptureClient_ReleaseBuffer(capture, 0) != S_OK) return 113;
			continue;
		}
		if (IAudioCaptureClient_ReleaseBuffer(capture, 480) != S_OK) return 113;
		expected += 480;
	}
	if (GetTickCount() - start < 60) return 114;

	/* Left unread, the ring fills and the next packet says something was lost before it. */
	Sleep(100);
	if (IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, &device_position, NULL) != S_OK) return 115;
	if (!(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) || frames != 480 || device_position != expected) return 116;
	/* Released empty, the packet comes again still marked; consumed, the mark goes with it. */
	if (IAudioCaptureClient_ReleaseBuffer(capture, 0) != S_OK) return 116;
	if (IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, &device_position, NULL) != S_OK ||
		!(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) || device_position != expected) return 116;
	if (IAudioCaptureClient_ReleaseBuffer(capture, frames) != S_OK) return 116;
	/* Read out in time again, the packets after it are whole. */
	for (int packets = 0; IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL) == S_OK; packets++)
		if (packets > 4 || IAudioCaptureClient_ReleaseBuffer(capture, frames) != S_OK) return 117;
	for (const DWORD waited = GetTickCount();;) {
		if (GetTickCount() - waited > 1000) return 117;
		if (IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL) == S_OK)
			break;
		Sleep(2);
	}
	if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) return 117;
	if (IAudioCaptureClient_ReleaseBuffer(capture, frames) != S_OK) return 117;

	/* Stopped, what is left is read out, then the buffer is empty. */
	if (IAudioClient_Stop(client) != S_OK) return 118;
	for (int packets = 0;; packets++) {
		HRESULT result = IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL);
		if (result == AUDCLNT_S_BUFFER_EMPTY)
			break;
		if (result != S_OK || !frames || packets > 5 || IAudioCaptureClient_ReleaseBuffer(capture, frames) != S_OK) return 119;
	}
	if (FAILED(IAudioClient_GetCurrentPadding(client, &padding)) || padding) return 119;

	/* An event-driven recording is woken each period. */
	IAudioClient *evented = activate(microphone, &IID_IAudioClient2);
	if (!evented) return 120;
	if (IAudioClient_Initialize(evented, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, &format, NULL) != S_OK ||
		IAudioClient_SetEventHandle(evented, event) != S_OK) return 120;
	IAudioCaptureClient *events = NULL;
	if (FAILED(IAudioClient_GetService(evented, &IID_IAudioCaptureClient, (void **)&events)) || IAudioClient_Start(evented) != S_OK) return 121;
	for (int packets = 0; packets < 5; packets++) {
		if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) return 122;
		while (IAudioCaptureClient_GetBuffer(events, &data, &frames, &flags, NULL, NULL) == S_OK)
			if (IAudioCaptureClient_ReleaseBuffer(events, frames) != S_OK) return 123;
	}
	IAudioClient_Stop(evented);
	IAudioCaptureClient_Release(events);
	IAudioClient_Release(evented);
	IAudioCaptureClient_Release(capture);
	IAudioClient_Release(client);
	CloseHandle(event);
	return 0;
}

/* ----- IAudioClient3 and activation ----- */

typedef HRESULT (WINAPI *activate_function)(LPCWSTR, REFIID, PROPVARIANT *, IActivateAudioInterfaceCompletionHandler *,
	IActivateAudioInterfaceAsyncOperation **);

static HRESULT activate_by_path(activate_function activate_async, const WCHAR *path, IUnknown **out)
{
	completion handler = {{&completion_vtbl}, 1, CreateEventW(NULL, TRUE, FALSE, NULL)};
	IActivateAudioInterfaceAsyncOperation *operation = NULL;
	HRESULT result = activate_async(path, &IID_IAudioClient, NULL, &handler.IActivateAudioInterfaceCompletionHandler_iface, &operation);
	if (FAILED(result))
		return E_UNEXPECTED;
	if (WaitForSingleObject(handler.done, 2000) != WAIT_OBJECT_0 || handler.thread == GetCurrentThreadId())
		return E_UNEXPECTED;
	HRESULT activated = E_UNEXPECTED;
	*out = NULL;
	if (FAILED(IActivateAudioInterfaceAsyncOperation_GetActivateResult(operation, &activated, out)))
		return E_UNEXPECTED;
	IActivateAudioInterfaceAsyncOperation_Release(operation);
	/* The operation's thread lets the handler go once it returns. */
	for (int wait = 0; handler.ref != 1 && wait < 100; wait++)
		Sleep(5);
	CloseHandle(handler.done);
	return handler.ref == 1 ? activated : E_UNEXPECTED;
}

static int third(void)
{
	IAudioClient3 *client = (IAudioClient3 *)activate(speakers, &IID_IAudioClient3);
	if (!client) return 130;
	WAVEFORMATEX *mix = NULL;
	UINT32 preferred = 0, fundamental = 0, minimum = 0, maximum = 0, current = 0, frames = 0;
	if (FAILED(IAudioClient3_GetMixFormat(client, &mix))) return 131;
	if (FAILED(IAudioClient3_GetSharedModeEnginePeriod(client, mix, &preferred, &fundamental, &minimum, &maximum)) || preferred != 480 ||
		fundamental != 480 || minimum != 480 || maximum != 480) return 132;
	WAVEFORMATEX *engine = NULL;
	if (FAILED(IAudioClient3_GetCurrentSharedModeEnginePeriod(client, &engine, &current)) || current != 480 || engine->nSamplesPerSec != 48000)
		return 133;
	CoTaskMemFree(engine);
	if (IAudioClient3_InitializeSharedAudioStream(client, 0, 240, mix, NULL) != AUDCLNT_E_INVALID_DEVICE_PERIOD) return 134;
	if (IAudioClient3_InitializeSharedAudioStream(client, 0, 480, mix, NULL) != S_OK) return 135;
	if (FAILED(IAudioClient3_GetBufferSize(client, &frames)) || frames != 1440) return 135;
	CoTaskMemFree(mix);
	IAudioClient3_Release(client);

	const HMODULE library = GetModuleHandleW(L"mmdevapi.dll");
	const activate_function activate_async = library ? (activate_function)GetProcAddress(library, "ActivateAudioInterfaceAsync") : NULL;
	if (!activate_async) return 136;
	WCHAR *id = NULL;
	if (FAILED(IMMDevice_GetId(microphone, &id))) return 137;
	WCHAR path[200] = L"\\\\?\\SWD#MMDEVAPI#";
	int length = 0;
	while (path[length])
		length++;
	for (const WCHAR *part = id; *part;)
		path[length++] = *part++;
	for (const WCHAR *part = L"#{2eef81be-33fa-4800-9670-1cd474972c3f}"; *part;)
		path[length++] = *part++;
	path[length] = 0;
	CoTaskMemFree(id);
	const WCHAR *paths[] = {L"{E6327CAD-DCEC-4949-AE8A-991E976A79D2}", path};
	for (int index = 0; index < 2; index++) {
		IUnknown *activated = NULL;
		IAudioClient *audio = NULL;
		if (activate_by_path(activate_async, paths[index], &activated) != S_OK || !activated) return 138 + index;
		if (FAILED(IUnknown_QueryInterface(activated, &IID_IAudioClient, (void **)&audio))) return 140;
		WAVEFORMATEX format = format_of(WAVE_FORMAT_PCM, 48000, 2, 16);
		if (IAudioClient_Initialize(audio, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, &format, NULL) !=
			(index ? AUDCLNT_E_WRONG_ENDPOINT_TYPE : AUDCLNT_E_ENDPOINT_CREATE_FAILED)) return 141;
		IAudioClient_Release(audio);
		IUnknown_Release(activated);
	}
	IUnknown *nothing = NULL;
	if (activate_by_path(activate_async, L"{00000000-0000-0000-0000-000000000000}", &nothing) != E_NOTFOUND_ || nothing) return 142;
	return 0;
}

int raven_entry(void)
{
	if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 200;
	int failed = endpoints();
	if (!failed)
		failed = playback();
	if (!failed)
		failed = volumes();
	if (!failed)
		failed = recording();
	if (!failed)
		failed = third();
	CoUninitialize();
	return failed;
}
