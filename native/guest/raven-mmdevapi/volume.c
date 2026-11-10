/* SPDX-License-Identifier: MIT
 *
 * The volume and session objects of mmdevapi.dll: audio sessions with their simple and channel volumes, the session manager, the endpoint
 * volume and the peak meter. The engine scales a stream by one gain: the product of its endpoint's, its session's and its own volumes, each
 * the mean of its channels where it has them.
 */
#include "mmdevapi_private.h"

static session *session_of_control(IAudioSessionControl2 *iface) { return CONTAINING_RECORD(iface, session, IAudioSessionControl2_iface); }
static session *session_of_simple(ISimpleAudioVolume *iface) { return CONTAINING_RECORD(iface, session, ISimpleAudioVolume_iface); }
static session *session_of_channels(IChannelAudioVolume *iface) { return CONTAINING_RECORD(iface, session, IChannelAudioVolume_iface); }
static endpoint_volume *endpoint_of(IAudioEndpointVolumeEx *iface) { return CONTAINING_RECORD(iface, endpoint_volume, IAudioEndpointVolumeEx_iface); }
static device *manager_of(IAudioSessionManager2 *iface) { return CONTAINING_RECORD(iface, device, IAudioSessionManager2_iface); }

static float mean(const float *levels, UINT count)
{
	float sum = 0;
	for (UINT index = 0; index < count; index++)
		sum += levels[index];
	return count ? sum / count : 1;
}

static BOOL level_valid(float level) { return level >= 0 && level <= 1; }

void update_volumes(void)
{
	for (client *audio = clients; audio; audio = audio->next) {
		const endpoint_volume *endpoint = &audio->device->volume;
		const session *owner = audio->session;
		float gain = endpoint->mute ? 0 : endpoint->master * mean(endpoint->channels, MIX_CHANNELS);
		gain *= owner->mute ? 0 : owner->volume * mean(owner->channels, MIX_CHANNELS);
		gain *= mean(audio->channels, audio->format->nChannels);
		union {
			float level;
			UINT32 bits;
		} volume = {gain};
		__atomic_store_n(&audio->shared->volume, volume.bits, __ATOMIC_RELEASE);
	}
}

/* Adds a listener to a growing array of interface pointers, holding a reference to it. */
static HRESULT add_listener(IUnknown ***listeners, UINT *count, UINT *capacity, IUnknown *listener)
{
	if (*count == *capacity) {
		const UINT grown = *capacity ? *capacity * 2 : 4;
		IUnknown **larger = ALLOC(grown * sizeof(*larger));
		if (!larger)
			return E_OUTOFMEMORY;
		if (*count)
			memcpy(larger, *listeners, *count * sizeof(*larger));
		if (*listeners)
			FREE(*listeners);
		*listeners = larger;
		*capacity = grown;
	}
	IUnknown_AddRef(listener);
	(*listeners)[(*count)++] = listener;
	return S_OK;
}

static HRESULT remove_listener(IUnknown **listeners, UINT *count, IUnknown *listener)
{
	for (UINT index = 0; index < *count; index++)
		if (listeners[index] == listener) {
			memmove(listeners + index, listeners + index + 1, (*count - index - 1) * sizeof(*listeners));
			(*count)--;
			IUnknown_Release(listener);
			return S_OK;
		}
	return E_INVALIDARG;
}

/* The listeners at this moment, each with a reference of its own, to be told once audio_lock is left; NULL when there are none. */
static IUnknown **snapshot(IUnknown **listeners, UINT count)
{
	if (!count)
		return NULL;
	IUnknown **copy = ALLOC(count * sizeof(*copy));
	if (!copy)
		return NULL;
	for (UINT index = 0; index < count; index++) {
		copy[index] = listeners[index];
		IUnknown_AddRef(copy[index]);
	}
	return copy;
}

static void release_snapshot(IUnknown **listeners, UINT count)
{
	if (!listeners)
		return;
	for (UINT index = 0; index < count; index++)
		IUnknown_Release(listeners[index]);
	FREE(listeners);
}

#define SESSION_SNAPSHOT(owner, copy, count)                                                        \
	do {                                                                                        \
		count = (owner)->listener_count;                                                    \
		copy = (IAudioSessionEvents **)snapshot((IUnknown **)(owner)->listeners, count);    \
		if (!copy)                                                                          \
			count = 0;                                                                  \
	} while (0)

/* ----- Sessions ----- */

static size_t text_length(const WCHAR *text)
{
	size_t length = 0;
	while (text[length])
		length++;
	return length;
}

/* A heap copy of a string the program passed, for the session to keep. */
static WCHAR *keep_string(const WCHAR *text)
{
	const size_t size = (text_length(text) + 1) * sizeof(WCHAR);
	WCHAR *copy = ALLOC(size);
	if (copy)
		memcpy(copy, text, size);
	return copy;
}

/* A session is one object: its control and volumes reach each other, as programs that enumerate sessions to set their volume expect. */
static HRESULT session_query(session *owner, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IAudioSessionControl) || same_guid(riid, &IID_IAudioSessionControl2))
		*out = &owner->IAudioSessionControl2_iface;
	else if (same_guid(riid, &IID_ISimpleAudioVolume))
		*out = &owner->ISimpleAudioVolume_iface;
	else if (same_guid(riid, &IID_IChannelAudioVolume))
		*out = &owner->IChannelAudioVolume_iface;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	return S_OK;
}

static HRESULT WINAPI control_QueryInterface(IAudioSessionControl2 *iface, REFIID riid, void **out)
{
	return session_query(session_of_control(iface), riid, out);
}

/* Sessions belong to the process: references are counted by nobody. */
static ULONG WINAPI control_AddRef(IAudioSessionControl2 *iface) { return 2; }
static ULONG WINAPI control_Release(IAudioSessionControl2 *iface) { return 1; }

static HRESULT WINAPI control_GetState(IAudioSessionControl2 *iface, AudioSessionState *state)
{
	session *owner = session_of_control(iface);
	if (!state)
		return E_POINTER;
	EnterCriticalSection(&audio_lock);
	*state = owner->running ? AudioSessionStateActive : AudioSessionStateInactive;
	LeaveCriticalSection(&audio_lock);
	return S_OK;
}

static HRESULT get_text(WCHAR *const *field, LPWSTR *out)
{
	if (!out)
		return E_POINTER;
	EnterCriticalSection(&audio_lock);
	*out = copy_string(*field ? *field : L"");
	LeaveCriticalSection(&audio_lock);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI control_GetDisplayName(IAudioSessionControl2 *iface, LPWSTR *name)
{
	return get_text(&session_of_control(iface)->display_name, name);
}

static HRESULT WINAPI control_SetDisplayName(IAudioSessionControl2 *iface, LPCWSTR name, LPCGUID context)
{
	session *owner = session_of_control(iface);
	IAudioSessionEvents **listeners;
	UINT count;
	if (!name)
		return E_POINTER;
	WCHAR *copy = keep_string(name);
	if (!copy)
		return E_OUTOFMEMORY;
	EnterCriticalSection(&audio_lock);
	if (owner->display_name)
		FREE(owner->display_name);
	owner->display_name = copy;
	SESSION_SNAPSHOT(owner, listeners, count);
	LeaveCriticalSection(&audio_lock);
	for (UINT index = 0; index < count; index++)
		IAudioSessionEvents_OnDisplayNameChanged(listeners[index], name, context);
	release_snapshot((IUnknown **)listeners, count);
	return S_OK;
}

static HRESULT WINAPI control_GetIconPath(IAudioSessionControl2 *iface, LPWSTR *path)
{
	return get_text(&session_of_control(iface)->icon_path, path);
}

static HRESULT WINAPI control_SetIconPath(IAudioSessionControl2 *iface, LPCWSTR path, LPCGUID context)
{
	session *owner = session_of_control(iface);
	IAudioSessionEvents **listeners;
	UINT count;
	if (!path)
		return E_POINTER;
	WCHAR *copy = keep_string(path);
	if (!copy)
		return E_OUTOFMEMORY;
	EnterCriticalSection(&audio_lock);
	if (owner->icon_path)
		FREE(owner->icon_path);
	owner->icon_path = copy;
	SESSION_SNAPSHOT(owner, listeners, count);
	LeaveCriticalSection(&audio_lock);
	for (UINT index = 0; index < count; index++)
		IAudioSessionEvents_OnIconPathChanged(listeners[index], path, context);
	release_snapshot((IUnknown **)listeners, count);
	return S_OK;
}

static HRESULT WINAPI control_GetGroupingParam(IAudioSessionControl2 *iface, GUID *grouping)
{
	session *owner = session_of_control(iface);
	if (!grouping)
		return E_POINTER;
	EnterCriticalSection(&audio_lock);
	*grouping = owner->grouping;
	LeaveCriticalSection(&audio_lock);
	return S_OK;
}

static HRESULT WINAPI control_SetGroupingParam(IAudioSessionControl2 *iface, LPCGUID grouping, LPCGUID context)
{
	session *owner = session_of_control(iface);
	IAudioSessionEvents **listeners;
	UINT count;
	if (!grouping)
		return E_POINTER;
	EnterCriticalSection(&audio_lock);
	owner->grouping = *grouping;
	SESSION_SNAPSHOT(owner, listeners, count);
	LeaveCriticalSection(&audio_lock);
	for (UINT index = 0; index < count; index++)
		IAudioSessionEvents_OnGroupingParamChanged(listeners[index], grouping, context);
	release_snapshot((IUnknown **)listeners, count);
	return S_OK;
}

static HRESULT WINAPI control_RegisterAudioSessionNotification(IAudioSessionControl2 *iface, IAudioSessionEvents *events)
{
	session *owner = session_of_control(iface);
	if (!events)
		return E_POINTER;
	EnterCriticalSection(&audio_lock);
	const HRESULT result = add_listener((IUnknown ***)&owner->listeners, &owner->listener_count, &owner->listener_capacity, (IUnknown *)events);
	LeaveCriticalSection(&audio_lock);
	return result;
}

static HRESULT WINAPI control_UnregisterAudioSessionNotification(IAudioSessionControl2 *iface, IAudioSessionEvents *events)
{
	session *owner = session_of_control(iface);
	if (!events)
		return E_POINTER;
	EnterCriticalSection(&audio_lock);
	const HRESULT result = remove_listener((IUnknown **)owner->listeners, &owner->listener_count, (IUnknown *)events);
	LeaveCriticalSection(&audio_lock);
	return result;
}

/* The session's identifier: its endpoint's id and the session's GUID; the instance's adds the process. */
static HRESULT session_identifier(session *owner, BOOL instance, LPWSTR *out)
{
	WCHAR guid[40], text[160];
	size_t length = 0;
	if (!out)
		return E_POINTER;
	if (!StringFromGUID2(&owner->guid, guid, 40))
		return E_FAIL;
	for (const WCHAR *part = owner->device->id; *part;)
		text[length++] = *part++;
	text[length++] = '|';
	for (const WCHAR *part = guid; *part;)
		text[length++] = *part++;
	if (instance) {
		WCHAR digits[12];
		int count = 0;
		DWORD process = GetCurrentProcessId();
		do {
			digits[count++] = (WCHAR)('0' + process % 10);
			process /= 10;
		} while (process);
		text[length++] = '|';
		while (count)
			text[length++] = digits[--count];
	}
	text[length] = 0;
	*out = copy_string(text);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI control_GetSessionIdentifier(IAudioSessionControl2 *iface, LPWSTR *id)
{
	return session_identifier(session_of_control(iface), FALSE, id);
}

static HRESULT WINAPI control_GetSessionInstanceIdentifier(IAudioSessionControl2 *iface, LPWSTR *id)
{
	return session_identifier(session_of_control(iface), TRUE, id);
}

static HRESULT WINAPI control_GetProcessId(IAudioSessionControl2 *iface, DWORD *process)
{
	if (!process)
		return E_POINTER;
	*process = GetCurrentProcessId();
	return S_OK;
}

static HRESULT WINAPI control_IsSystemSoundsSession(IAudioSessionControl2 *iface) { return S_FALSE; }

/* No other program's sounds duck this one's. */
static HRESULT WINAPI control_SetDuckingPreference(IAudioSessionControl2 *iface, BOOL opt_out) { return S_OK; }

static const IAudioSessionControl2Vtbl control_vtbl = {
	control_QueryInterface, control_AddRef, control_Release, control_GetState, control_GetDisplayName, control_SetDisplayName,
	control_GetIconPath, control_SetIconPath, control_GetGroupingParam, control_SetGroupingParam,
	control_RegisterAudioSessionNotification, control_UnregisterAudioSessionNotification, control_GetSessionIdentifier,
	control_GetSessionInstanceIdentifier, control_GetProcessId, control_IsSystemSoundsSession, control_SetDuckingPreference,
};

static HRESULT WINAPI simple_QueryInterface(ISimpleAudioVolume *iface, REFIID riid, void **out)
{
	return session_query(session_of_simple(iface), riid, out);
}

static ULONG WINAPI simple_AddRef(ISimpleAudioVolume *iface) { return 2; }
static ULONG WINAPI simple_Release(ISimpleAudioVolume *iface) { return 1; }

/* Applies a change to the session's simple volume and tells its listeners, as Windows does, whoever made it. */
static HRESULT simple_changed(session *owner, const float *level, const BOOL *mute, LPCGUID context)
{
	IAudioSessionEvents **listeners;
	UINT count;
	EnterCriticalSection(&audio_lock);
	if (level)
		owner->volume = *level;
	if (mute)
		owner->mute = *mute;
	update_volumes();
	const float now = owner->volume;
	const BOOL muted = owner->mute;
	SESSION_SNAPSHOT(owner, listeners, count);
	LeaveCriticalSection(&audio_lock);
	for (UINT index = 0; index < count; index++)
		IAudioSessionEvents_OnSimpleVolumeChanged(listeners[index], now, muted, context);
	release_snapshot((IUnknown **)listeners, count);
	return S_OK;
}

static HRESULT WINAPI simple_SetMasterVolume(ISimpleAudioVolume *iface, float level, LPCGUID context)
{
	if (!level_valid(level))
		return E_INVALIDARG;
	return simple_changed(session_of_simple(iface), &level, NULL, context);
}

static HRESULT WINAPI simple_GetMasterVolume(ISimpleAudioVolume *iface, float *level)
{
	if (!level)
		return E_POINTER;
	EnterCriticalSection(&audio_lock);
	*level = session_of_simple(iface)->volume;
	LeaveCriticalSection(&audio_lock);
	return S_OK;
}

static HRESULT WINAPI simple_SetMute(ISimpleAudioVolume *iface, BOOL mute, LPCGUID context)
{
	mute = mute != FALSE;
	return simple_changed(session_of_simple(iface), NULL, &mute, context);
}

static HRESULT WINAPI simple_GetMute(ISimpleAudioVolume *iface, BOOL *mute)
{
	if (!mute)
		return E_POINTER;
	EnterCriticalSection(&audio_lock);
	*mute = session_of_simple(iface)->mute;
	LeaveCriticalSection(&audio_lock);
	return S_OK;
}

static const ISimpleAudioVolumeVtbl simple_vtbl = {
	simple_QueryInterface, simple_AddRef, simple_Release, simple_SetMasterVolume, simple_GetMasterVolume, simple_SetMute, simple_GetMute,
};

static HRESULT WINAPI channels_QueryInterface(IChannelAudioVolume *iface, REFIID riid, void **out)
{
	return session_query(session_of_channels(iface), riid, out);
}

static ULONG WINAPI channels_AddRef(IChannelAudioVolume *iface) { return 2; }
static ULONG WINAPI channels_Release(IChannelAudioVolume *iface) { return 1; }

static HRESULT WINAPI channels_GetChannelCount(IChannelAudioVolume *iface, UINT32 *count)
{
	if (!count)
		return E_POINTER;
	*count = MIX_CHANNELS;
	return S_OK;
}

/* Sets channel volumes of the session from levels, all of them when changed is (DWORD)-1, and tells the listeners. */
static HRESULT channels_changed(session *owner, const float *levels, DWORD changed, LPCGUID context)
{
	IAudioSessionEvents **listeners;
	UINT count;
	float now[MIX_CHANNELS];
	EnterCriticalSection(&audio_lock);
	for (UINT index = 0; index < MIX_CHANNELS; index++)
		if (changed == (DWORD)-1 || changed == index)
			owner->channels[index] = levels[changed == (DWORD)-1 ? index : 0];
	update_volumes();
	memcpy(now, owner->channels, sizeof(now));
	SESSION_SNAPSHOT(owner, listeners, count);
	LeaveCriticalSection(&audio_lock);
	for (UINT index = 0; index < count; index++)
		IAudioSessionEvents_OnChannelVolumeChanged(listeners[index], MIX_CHANNELS, now, changed, context);
	release_snapshot((IUnknown **)listeners, count);
	return S_OK;
}

static HRESULT WINAPI channels_SetChannelVolume(IChannelAudioVolume *iface, UINT32 index, const float level, LPCGUID context)
{
	if (index >= MIX_CHANNELS || !level_valid(level))
		return E_INVALIDARG;
	return channels_changed(session_of_channels(iface), &level, index, context);
}

static HRESULT WINAPI channels_GetChannelVolume(IChannelAudioVolume *iface, UINT32 index, float *level)
{
	if (!level)
		return E_POINTER;
	if (index >= MIX_CHANNELS)
		return E_INVALIDARG;
	EnterCriticalSection(&audio_lock);
	*level = session_of_channels(iface)->channels[index];
	LeaveCriticalSection(&audio_lock);
	return S_OK;
}

static HRESULT WINAPI channels_SetAllVolumes(IChannelAudioVolume *iface, UINT32 count, const float *levels, LPCGUID context)
{
	if (!levels)
		return E_POINTER;
	if (count != MIX_CHANNELS)
		return E_INVALIDARG;
	for (UINT32 index = 0; index < count; index++)
		if (!level_valid(levels[index]))
			return E_INVALIDARG;
	return channels_changed(session_of_channels(iface), levels, (DWORD)-1, context);
}

static HRESULT WINAPI channels_GetAllVolumes(IChannelAudioVolume *iface, UINT32 count, float *levels)
{
	if (!levels)
		return E_POINTER;
	if (count != MIX_CHANNELS)
		return E_INVALIDARG;
	EnterCriticalSection(&audio_lock);
	memcpy(levels, session_of_channels(iface)->channels, sizeof(float) * MIX_CHANNELS);
	LeaveCriticalSection(&audio_lock);
	return S_OK;
}

static const IChannelAudioVolumeVtbl channels_vtbl = {
	channels_QueryInterface, channels_AddRef, channels_Release, channels_GetChannelCount, channels_SetChannelVolume,
	channels_GetChannelVolume, channels_SetAllVolumes, channels_GetAllVolumes,
};

HRESULT session_for(device *device, const GUID *guid, session **out)
{
	static const GUID none;
	if (!guid)
		guid = &none;
	EnterCriticalSection(&audio_lock);
	session *owner = device->sessions;
	while (owner && !same_guid(&owner->guid, guid))
		owner = owner->next;
	if (!owner && (owner = ALLOC(sizeof(*owner)))) {
		owner->IAudioSessionControl2_iface.lpVtbl = &control_vtbl;
		owner->ISimpleAudioVolume_iface.lpVtbl = &simple_vtbl;
		owner->IChannelAudioVolume_iface.lpVtbl = &channels_vtbl;
		owner->device = device;
		owner->guid = *guid;
		owner->volume = 1;
		for (UINT index = 0; index < MIX_CHANNELS; index++)
			owner->channels[index] = 1;
		owner->next = device->sessions;
		device->sessions = owner;
	}
	LeaveCriticalSection(&audio_lock);
	*out = owner;
	return owner ? S_OK : E_OUTOFMEMORY;
}

void session_started(session *owner, BOOL started)
{
	IAudioSessionEvents **listeners = NULL;
	UINT count = 0;
	EnterCriticalSection(&audio_lock);
	const BOOL changed = started ? ++owner->running == 1 : --owner->running == 0;
	if (changed)
		SESSION_SNAPSHOT(owner, listeners, count);
	LeaveCriticalSection(&audio_lock);
	for (UINT index = 0; index < count; index++)
		IAudioSessionEvents_OnStateChanged(listeners[index], started ? AudioSessionStateActive : AudioSessionStateInactive);
	release_snapshot((IUnknown **)listeners, count);
}

/* ----- Session manager ----- */

typedef struct session_list {
	IAudioSessionEnumerator IAudioSessionEnumerator_iface;
	LONG ref;
	UINT count;
	session *items[];
} session_list;

static session_list *list_of(IAudioSessionEnumerator *iface) { return CONTAINING_RECORD(iface, session_list, IAudioSessionEnumerator_iface); }

static HRESULT WINAPI list_QueryInterface(IAudioSessionEnumerator *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IAudioSessionEnumerator)) {
		*out = iface;
		IAudioSessionEnumerator_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI list_AddRef(IAudioSessionEnumerator *iface) { return InterlockedIncrement(&list_of(iface)->ref); }

static ULONG WINAPI list_Release(IAudioSessionEnumerator *iface)
{
	session_list *list = list_of(iface);
	const ULONG ref = InterlockedDecrement(&list->ref);
	if (!ref)
		FREE(list);
	return ref;
}

static HRESULT WINAPI list_GetCount(IAudioSessionEnumerator *iface, int *count)
{
	if (!count)
		return E_POINTER;
	*count = (int)list_of(iface)->count;
	return S_OK;
}

static HRESULT WINAPI list_GetSession(IAudioSessionEnumerator *iface, int index, IAudioSessionControl **out)
{
	session_list *list = list_of(iface);
	if (!out)
		return E_POINTER;
	if (index < 0 || (UINT)index >= list->count) {
		*out = NULL;
		return E_INVALIDARG;
	}
	*out = (IAudioSessionControl *)&list->items[index]->IAudioSessionControl2_iface;
	return S_OK;
}

static const IAudioSessionEnumeratorVtbl list_vtbl = {list_QueryInterface, list_AddRef, list_Release, list_GetCount, list_GetSession};

static HRESULT WINAPI manager_QueryInterface(IAudioSessionManager2 *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IAudioSessionManager) || same_guid(riid, &IID_IAudioSessionManager2)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI manager_AddRef(IAudioSessionManager2 *iface) { return 2; }
static ULONG WINAPI manager_Release(IAudioSessionManager2 *iface) { return 1; }

static HRESULT WINAPI manager_GetAudioSessionControl(IAudioSessionManager2 *iface, LPCGUID guid, DWORD flags, IAudioSessionControl **out)
{
	session *owner;
	if (!out)
		return E_POINTER;
	*out = NULL;
	const HRESULT result = session_for(manager_of(iface), guid, &owner);
	if (SUCCEEDED(result))
		*out = (IAudioSessionControl *)&owner->IAudioSessionControl2_iface;
	return result;
}

static HRESULT WINAPI manager_GetSimpleAudioVolume(IAudioSessionManager2 *iface, LPCGUID guid, DWORD cross_process, ISimpleAudioVolume **out)
{
	session *owner;
	if (!out)
		return E_POINTER;
	*out = NULL;
	const HRESULT result = session_for(manager_of(iface), guid, &owner);
	if (SUCCEEDED(result))
		*out = &owner->ISimpleAudioVolume_iface;
	return result;
}

static HRESULT WINAPI manager_GetSessionEnumerator(IAudioSessionManager2 *iface, IAudioSessionEnumerator **out)
{
	device *device = manager_of(iface);
	UINT count = 0;
	if (!out)
		return E_POINTER;
	*out = NULL;
	EnterCriticalSection(&audio_lock);
	for (session *owner = device->sessions; owner; owner = owner->next)
		count++;
	session_list *list = ALLOC(sizeof(*list) + count * sizeof(session *));
	if (list) {
		list->IAudioSessionEnumerator_iface.lpVtbl = &list_vtbl;
		list->ref = 1;
		for (session *owner = device->sessions; owner; owner = owner->next)
			list->items[list->count++] = owner;
	}
	LeaveCriticalSection(&audio_lock);
	if (!list)
		return E_OUTOFMEMORY;
	*out = &list->IAudioSessionEnumerator_iface;
	return S_OK;
}

/* Sessions are made only by this process and nothing ducks: no listener of these is ever told anything. */
static HRESULT WINAPI manager_RegisterSessionNotification(IAudioSessionManager2 *iface, IAudioSessionNotification *notification)
{
	return notification ? S_OK : E_POINTER;
}

static HRESULT WINAPI manager_UnregisterSessionNotification(IAudioSessionManager2 *iface, IAudioSessionNotification *notification)
{
	return notification ? S_OK : E_POINTER;
}

static HRESULT WINAPI manager_RegisterDuckNotification(IAudioSessionManager2 *iface, LPCWSTR id, IAudioVolumeDuckNotification *notification)
{
	return notification ? S_OK : E_POINTER;
}

static HRESULT WINAPI manager_UnregisterDuckNotification(IAudioSessionManager2 *iface, IAudioVolumeDuckNotification *notification)
{
	return notification ? S_OK : E_POINTER;
}

static const IAudioSessionManager2Vtbl manager_vtbl = {
	manager_QueryInterface, manager_AddRef, manager_Release, manager_GetAudioSessionControl, manager_GetSimpleAudioVolume,
	manager_GetSessionEnumerator, manager_RegisterSessionNotification, manager_UnregisterSessionNotification,
	manager_RegisterDuckNotification, manager_UnregisterDuckNotification,
};

/* ----- Endpoint volume ----- */

static HRESULT WINAPI endpoint_QueryInterface(IAudioEndpointVolumeEx *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IAudioEndpointVolume) || same_guid(riid, &IID_IAudioEndpointVolumeEx)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI endpoint_AddRef(IAudioEndpointVolumeEx *iface) { return 2; }
static ULONG WINAPI endpoint_Release(IAudioEndpointVolumeEx *iface) { return 1; }

static HRESULT WINAPI endpoint_RegisterControlChangeNotify(IAudioEndpointVolumeEx *iface, IAudioEndpointVolumeCallback *callback)
{
	endpoint_volume *volume = endpoint_of(iface);
	if (!callback)
		return E_POINTER;
	EnterCriticalSection(&audio_lock);
	const HRESULT result = add_listener((IUnknown ***)&volume->listeners, &volume->listener_count, &volume->listener_capacity, (IUnknown *)callback);
	LeaveCriticalSection(&audio_lock);
	return result;
}

static HRESULT WINAPI endpoint_UnregisterControlChangeNotify(IAudioEndpointVolumeEx *iface, IAudioEndpointVolumeCallback *callback)
{
	endpoint_volume *volume = endpoint_of(iface);
	if (!callback)
		return E_POINTER;
	EnterCriticalSection(&audio_lock);
	const HRESULT result = remove_listener((IUnknown **)volume->listeners, &volume->listener_count, (IUnknown *)callback);
	LeaveCriticalSection(&audio_lock);
	return result;
}

/* Changes the endpoint volume under audio_lock: the master level, a channel's level, or the mute, whichever is given. Then tells the
 * listeners the whole volume, as Windows does on every change. */
static HRESULT endpoint_changed(endpoint_volume *volume, const float *master, UINT channel, const float *level, const BOOL *mute, LPCGUID context)
{
	IAudioEndpointVolumeCallback **listeners;
	UINT count;
	EnterCriticalSection(&audio_lock);
	if (master)
		volume->master = *master;
	if (level)
		volume->channels[channel] = *level;
	if (mute)
		volume->mute = *mute;
	update_volumes();
	count = volume->listener_count;
	listeners = (IAudioEndpointVolumeCallback **)snapshot((IUnknown **)volume->listeners, count);
	if (!listeners)
		count = 0;
	AUDIO_VOLUME_NOTIFICATION_DATA *data = count ? ALLOC(sizeof(*data) + (MIX_CHANNELS - 1) * sizeof(float)) : NULL;
	if (data) {
		data->guidEventContext = context ? *context : GUID_NULL;
		data->bMuted = volume->mute;
		data->fMasterVolume = volume->master;
		data->nChannels = MIX_CHANNELS;
		memcpy(data->afChannelVolumes, volume->channels, sizeof(volume->channels));
	}
	LeaveCriticalSection(&audio_lock);
	if (data) {
		for (UINT index = 0; index < count; index++)
			IAudioEndpointVolumeCallback_OnNotify(listeners[index], data);
		FREE(data);
	}
	release_snapshot((IUnknown **)listeners, count);
	return S_OK;
}

static HRESULT WINAPI endpoint_GetChannelCount(IAudioEndpointVolumeEx *iface, UINT *count)
{
	if (!count)
		return E_POINTER;
	*count = MIX_CHANNELS;
	return S_OK;
}

static BOOL decibels_valid(float decibels) { return decibels >= MIN_DECIBELS && decibels <= 0; }

static HRESULT WINAPI endpoint_SetMasterVolumeLevel(IAudioEndpointVolumeEx *iface, float decibels, LPCGUID context)
{
	if (!decibels_valid(decibels))
		return E_INVALIDARG;
	const float level = scalar_of(decibels);
	return endpoint_changed(endpoint_of(iface), &level, 0, NULL, NULL, context);
}

static HRESULT WINAPI endpoint_SetMasterVolumeLevelScalar(IAudioEndpointVolumeEx *iface, float level, LPCGUID context)
{
	if (!level_valid(level))
		return E_INVALIDARG;
	return endpoint_changed(endpoint_of(iface), &level, 0, NULL, NULL, context);
}

static float endpoint_read(endpoint_volume *volume, int channel)
{
	EnterCriticalSection(&audio_lock);
	const float level = channel < 0 ? volume->master : volume->channels[channel];
	LeaveCriticalSection(&audio_lock);
	return level;
}

static HRESULT WINAPI endpoint_GetMasterVolumeLevel(IAudioEndpointVolumeEx *iface, float *decibels)
{
	if (!decibels)
		return E_POINTER;
	*decibels = decibels_of(endpoint_read(endpoint_of(iface), -1));
	return S_OK;
}

static HRESULT WINAPI endpoint_GetMasterVolumeLevelScalar(IAudioEndpointVolumeEx *iface, float *level)
{
	if (!level)
		return E_POINTER;
	*level = endpoint_read(endpoint_of(iface), -1);
	return S_OK;
}

static HRESULT WINAPI endpoint_SetChannelVolumeLevel(IAudioEndpointVolumeEx *iface, UINT channel, float decibels, LPCGUID context)
{
	if (channel >= MIX_CHANNELS || !decibels_valid(decibels))
		return E_INVALIDARG;
	const float level = scalar_of(decibels);
	return endpoint_changed(endpoint_of(iface), NULL, channel, &level, NULL, context);
}

static HRESULT WINAPI endpoint_SetChannelVolumeLevelScalar(IAudioEndpointVolumeEx *iface, UINT channel, float level, LPCGUID context)
{
	if (channel >= MIX_CHANNELS || !level_valid(level))
		return E_INVALIDARG;
	return endpoint_changed(endpoint_of(iface), NULL, channel, &level, NULL, context);
}

static HRESULT WINAPI endpoint_GetChannelVolumeLevel(IAudioEndpointVolumeEx *iface, UINT channel, float *decibels)
{
	if (!decibels)
		return E_POINTER;
	if (channel >= MIX_CHANNELS)
		return E_INVALIDARG;
	*decibels = decibels_of(endpoint_read(endpoint_of(iface), (int)channel));
	return S_OK;
}

static HRESULT WINAPI endpoint_GetChannelVolumeLevelScalar(IAudioEndpointVolumeEx *iface, UINT channel, float *level)
{
	if (!level)
		return E_POINTER;
	if (channel >= MIX_CHANNELS)
		return E_INVALIDARG;
	*level = endpoint_read(endpoint_of(iface), (int)channel);
	return S_OK;
}

static HRESULT WINAPI endpoint_SetMute(IAudioEndpointVolumeEx *iface, BOOL mute, LPCGUID context)
{
	mute = mute != FALSE;
	return endpoint_changed(endpoint_of(iface), NULL, 0, NULL, &mute, context);
}

static HRESULT WINAPI endpoint_GetMute(IAudioEndpointVolumeEx *iface, BOOL *mute)
{
	if (!mute)
		return E_POINTER;
	EnterCriticalSection(&audio_lock);
	*mute = endpoint_of(iface)->mute;
	LeaveCriticalSection(&audio_lock);
	return S_OK;
}

/* A hundred steps of a hundredth of the scalar level each. */
#define VOLUME_STEPS 100u

static UINT step_of(float level) { return (UINT)(level * VOLUME_STEPS + 0.5f); }

static HRESULT WINAPI endpoint_GetVolumeStepInfo(IAudioEndpointVolumeEx *iface, UINT *step, UINT *count)
{
	if (!step && !count)
		return E_POINTER;
	if (step)
		*step = step_of(endpoint_read(endpoint_of(iface), -1));
	if (count)
		*count = VOLUME_STEPS + 1;
	return S_OK;
}

static HRESULT endpoint_step(IAudioEndpointVolumeEx *iface, int direction, LPCGUID context)
{
	endpoint_volume *volume = endpoint_of(iface);
	int step = (int)step_of(endpoint_read(volume, -1)) + direction;
	if (step < 0)
		step = 0;
	if (step > (int)VOLUME_STEPS)
		step = VOLUME_STEPS;
	const float level = (float)step / VOLUME_STEPS;
	return endpoint_changed(volume, &level, 0, NULL, NULL, context);
}

static HRESULT WINAPI endpoint_VolumeStepUp(IAudioEndpointVolumeEx *iface, LPCGUID context) { return endpoint_step(iface, 1, context); }
static HRESULT WINAPI endpoint_VolumeStepDown(IAudioEndpointVolumeEx *iface, LPCGUID context) { return endpoint_step(iface, -1, context); }

/* The volume is applied in software. */
static HRESULT WINAPI endpoint_QueryHardwareSupport(IAudioEndpointVolumeEx *iface, DWORD *mask)
{
	if (!mask)
		return E_POINTER;
	*mask = 0;
	return S_OK;
}

static HRESULT WINAPI endpoint_GetVolumeRange(IAudioEndpointVolumeEx *iface, float *minimum, float *maximum, float *increment)
{
	if (!minimum || !maximum || !increment)
		return E_POINTER;
	*minimum = MIN_DECIBELS;
	*maximum = 0;
	*increment = 0.03125f;
	return S_OK;
}

static HRESULT WINAPI endpoint_GetVolumeRangeChannel(IAudioEndpointVolumeEx *iface, UINT channel, float *minimum, float *maximum,
	float *increment)
{
	if (channel >= MIX_CHANNELS)
		return E_INVALIDARG;
	return endpoint_GetVolumeRange(iface, minimum, maximum, increment);
}

static const IAudioEndpointVolumeExVtbl endpoint_vtbl = {
	endpoint_QueryInterface, endpoint_AddRef, endpoint_Release, endpoint_RegisterControlChangeNotify,
	endpoint_UnregisterControlChangeNotify, endpoint_GetChannelCount, endpoint_SetMasterVolumeLevel,
	endpoint_SetMasterVolumeLevelScalar, endpoint_GetMasterVolumeLevel, endpoint_GetMasterVolumeLevelScalar,
	endpoint_SetChannelVolumeLevel, endpoint_SetChannelVolumeLevelScalar, endpoint_GetChannelVolumeLevel,
	endpoint_GetChannelVolumeLevelScalar, endpoint_SetMute, endpoint_GetMute, endpoint_GetVolumeStepInfo, endpoint_VolumeStepUp,
	endpoint_VolumeStepDown, endpoint_QueryHardwareSupport, endpoint_GetVolumeRange, endpoint_GetVolumeRangeChannel,
};

/* ----- Peak meter ----- */

static HRESULT WINAPI meter_QueryInterface(IAudioMeterInformation *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IAudioMeterInformation)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI meter_AddRef(IAudioMeterInformation *iface) { return 2; }
static ULONG WINAPI meter_Release(IAudioMeterInformation *iface) { return 1; }

/* The engine mixes in the sound server, out of the guest's sight: the meter reads silence. */
static HRESULT WINAPI meter_GetPeakValue(IAudioMeterInformation *iface, float *peak)
{
	if (!peak)
		return E_POINTER;
	*peak = 0;
	return S_OK;
}

static HRESULT WINAPI meter_GetMeteringChannelCount(IAudioMeterInformation *iface, UINT *count)
{
	if (!count)
		return E_POINTER;
	*count = MIX_CHANNELS;
	return S_OK;
}

static HRESULT WINAPI meter_GetChannelsPeakValues(IAudioMeterInformation *iface, UINT32 count, float *peaks)
{
	if (!peaks)
		return E_POINTER;
	if (count != MIX_CHANNELS)
		return E_INVALIDARG;
	for (UINT32 index = 0; index < count; index++)
		peaks[index] = 0;
	return S_OK;
}

static HRESULT WINAPI meter_QueryHardwareSupport(IAudioMeterInformation *iface, DWORD *mask)
{
	if (!mask)
		return E_POINTER;
	*mask = 0;
	return S_OK;
}

static const IAudioMeterInformationVtbl meter_vtbl = {
	meter_QueryInterface, meter_AddRef, meter_Release, meter_GetPeakValue, meter_GetMeteringChannelCount, meter_GetChannelsPeakValues,
	meter_QueryHardwareSupport,
};

void volume_init(device *device)
{
	device->IAudioSessionManager2_iface.lpVtbl = &manager_vtbl;
	device->IAudioMeterInformation_iface.lpVtbl = &meter_vtbl;
	device->volume.IAudioEndpointVolumeEx_iface.lpVtbl = &endpoint_vtbl;
	device->volume.device = device;
	device->volume.master = 1;
	for (UINT index = 0; index < MIX_CHANNELS; index++)
		device->volume.channels[index] = 1;
}
