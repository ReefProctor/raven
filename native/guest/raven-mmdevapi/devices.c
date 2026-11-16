/* SPDX-License-Identifier: MIT
 *
 * The endpoints of mmdevapi.dll: the enumerator, the default speakers and microphone with their properties, the class factory and
 * ActivateAudioInterfaceAsync.
 */
#include "mmdevapi_private.h"

CRITICAL_SECTION audio_lock;

/* The endpoints' own GUIDs, which their ids and PKEY_AudioEndpoint_GUID carry. */
#define RENDER_GUID_TEXT L"{5A1D0E01-7A3C-4B8E-9A52-524156454E01}"
#define CAPTURE_GUID_TEXT L"{5A1D0E02-7A3C-4B8E-9A52-524156454E02}"

int same_guid(const GUID *left, const GUID *right) { return !memcmp(left, right, sizeof(GUID)); }

WCHAR *copy_string(const WCHAR *text)
{
	const size_t size = ((size_t)lstrlenW(text) + 1) * sizeof(WCHAR);
	WCHAR *copy = CoTaskMemAlloc(size);
	if (copy)
		memcpy(copy, text, size);
	return copy;
}

static WCHAR upper(WCHAR character) { return character >= 'a' && character <= 'z' ? character - ('a' - 'A') : character; }

static int same_text(const WCHAR *left, const WCHAR *right)
{
	for (;; left++, right++) {
		if (upper(*left) != upper(*right))
			return 0;
		if (!*left)
			return 1;
	}
}

/* Whether text holds part anywhere, ignoring case. */
static int holds_text(const WCHAR *text, const WCHAR *part)
{
	for (; *text; text++) {
		const WCHAR *left = text, *right = part;
		while (*right && upper(*left) == upper(*right))
			left++, right++;
		if (!*right)
			return 1;
	}
	return 0;
}

WAVEFORMATEX *copy_format(const WAVEFORMATEX *format)
{
	/* Of the formats the engine plays, only the extensible one carries more than a WAVEFORMATEX; cbSize is not trusted past that. */
	const BOOL extensible = format->wFormatTag == WAVE_FORMAT_EXTENSIBLE;
	const size_t size = extensible ? sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX);
	WAVEFORMATEX *copy = CoTaskMemAlloc(size);
	if (copy) {
		memcpy(copy, format, size);
		copy->cbSize = extensible ? sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) : 0;
	}
	return copy;
}

static void fill_mix_format(WAVEFORMATEXTENSIBLE *format)
{
	memset(format, 0, sizeof(*format));
	format->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	format->Format.nChannels = MIX_CHANNELS;
	format->Format.nSamplesPerSec = MIX_RATE;
	format->Format.wBitsPerSample = 32;
	format->Format.nBlockAlign = MIX_CHANNELS * 4;
	format->Format.nAvgBytesPerSec = MIX_RATE * MIX_CHANNELS * 4;
	format->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	format->Samples.wValidBitsPerSample = 32;
	format->dwChannelMask = KSAUDIO_SPEAKER_STEREO;
	format->SubFormat = subtype_float;
}

WAVEFORMATEX *mix_format(void)
{
	WAVEFORMATEXTENSIBLE *format = CoTaskMemAlloc(sizeof(*format));
	if (format)
		fill_mix_format(format);
	return (WAVEFORMATEX *)format;
}

/* The natural logarithm and the exponential, for the decibels of the volume objects: this component links against no runtime. */
static double log_of(double value)
{
	static const double ln2 = 0.69314718055994530942;
	int exponent = 0;
	while (value >= 2) {
		value /= 2;
		exponent++;
	}
	while (value < 1) {
		value *= 2;
		exponent--;
	}
	const double z = (value - 1) / (value + 1), square = z * z;
	double term = z, sum = 0;
	for (int k = 1; k < 40; k += 2) {
		sum += term / k;
		term *= square;
	}
	return 2 * sum + exponent * ln2;
}

static double exp_of(double value)
{
	static const double ln2 = 0.69314718055994530942;
	int halvings = 0;
	while (value > 0.5) {
		value -= ln2;
		halvings++;
	}
	while (value < -0.5) {
		value += ln2;
		halvings--;
	}
	double term = 1, sum = 1;
	for (int k = 1; k < 24; k++) {
		term *= value / k;
		sum += term;
	}
	for (; halvings > 0; halvings--)
		sum *= 2;
	for (; halvings < 0; halvings++)
		sum /= 2;
	return sum;
}

float decibels_of(float scalar)
{
	if (scalar <= 0)
		return MIN_DECIBELS;
	const double decibels = 20 * log_of(scalar) / 2.30258509299404568402;
	return decibels < MIN_DECIBELS ? MIN_DECIBELS : decibels > 0 ? 0 : (float)decibels;
}

float scalar_of(float decibels)
{
	if (decibels <= MIN_DECIBELS)
		return 0;
	if (decibels >= 0)
		return 1;
	return (float)exp_of(decibels / 20 * 2.30258509299404568402);
}

UINT64 counter_now(void)
{
	LARGE_INTEGER counter, frequency;
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	const UINT64 ticks = counter.QuadPart, rate = frequency.QuadPart;
	return ticks / rate * 10000000 + ticks % rate * 10000000 / rate;
}

/* ----- Property store ----- */

typedef struct property_store {
	IPropertyStore IPropertyStore_iface;
	LONG ref;
	device *device;
	DWORD access;
} property_store;

static property_store *store_of(IPropertyStore *iface) { return CONTAINING_RECORD(iface, property_store, IPropertyStore_iface); }

static const PROPERTYKEY *const device_keys[] = {
	&PKEY_Device_FriendlyName, &PKEY_Device_DeviceDesc, &PKEY_DeviceInterface_FriendlyName, &PKEY_AudioEndpoint_FormFactor,
	&PKEY_AudioEndpoint_GUID, &PKEY_AudioEndpoint_PhysicalSpeakers, &PKEY_AudioEngine_DeviceFormat,
};
#define DEVICE_KEY_COUNT (sizeof(device_keys) / sizeof(device_keys[0]))

static HRESULT WINAPI store_QueryInterface(IPropertyStore *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IPropertyStore)) {
		*out = iface;
		IPropertyStore_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI store_AddRef(IPropertyStore *iface) { return InterlockedIncrement(&store_of(iface)->ref); }

static ULONG WINAPI store_Release(IPropertyStore *iface)
{
	property_store *store = store_of(iface);
	const ULONG ref = InterlockedDecrement(&store->ref);
	if (!ref)
		FREE(store);
	return ref;
}

static HRESULT WINAPI store_GetCount(IPropertyStore *iface, DWORD *count)
{
	if (!count)
		return E_POINTER;
	*count = DEVICE_KEY_COUNT;
	return S_OK;
}

static HRESULT WINAPI store_GetAt(IPropertyStore *iface, DWORD index, PROPERTYKEY *key)
{
	if (!key)
		return E_POINTER;
	if (index >= DEVICE_KEY_COUNT)
		return E_INVALIDARG;
	*key = *device_keys[index];
	return S_OK;
}

static int same_key(const PROPERTYKEY *left, const PROPERTYKEY *right) { return same_guid(&left->fmtid, &right->fmtid) && left->pid == right->pid; }

static HRESULT string_value(PROPVARIANT *value, const WCHAR *text)
{
	value->vt = VT_LPWSTR;
	value->pwszVal = copy_string(text);
	return value->pwszVal ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI store_GetValue(IPropertyStore *iface, REFPROPERTYKEY key, PROPVARIANT *value)
{
	device *device = store_of(iface)->device;
	if (!key || !value)
		return E_POINTER;
	memset(value, 0, sizeof(*value));
	if (same_key(key, &PKEY_Device_FriendlyName))
		return string_value(value, device->name);
	if (same_key(key, &PKEY_Device_DeviceDesc))
		return string_value(value, device->flow == eRender ? L"Speakers" : L"Microphone");
	if (same_key(key, &PKEY_DeviceInterface_FriendlyName))
		return string_value(value, L"Raven Audio");
	if (same_key(key, &PKEY_AudioEndpoint_GUID))
		return string_value(value, device->flow == eRender ? RENDER_GUID_TEXT : CAPTURE_GUID_TEXT);
	if (same_key(key, &PKEY_AudioEndpoint_FormFactor)) {
		value->vt = VT_UI4;
		value->ulVal = device->flow == eRender ? Speakers : Microphone;
		return S_OK;
	}
	if (same_key(key, &PKEY_AudioEndpoint_PhysicalSpeakers)) {
		value->vt = VT_UI4;
		value->ulVal = KSAUDIO_SPEAKER_STEREO;
		return S_OK;
	}
	if (same_key(key, &PKEY_AudioEngine_DeviceFormat)) {
		WAVEFORMATEXTENSIBLE *format = CoTaskMemAlloc(sizeof(*format));
		if (!format)
			return E_OUTOFMEMORY;
		fill_mix_format(format);
		value->vt = VT_BLOB;
		value->blob.cbSize = sizeof(*format);
		value->blob.pBlobData = (BYTE *)format;
		return S_OK;
	}
	/* A property the endpoint does not have is empty, as on Windows. */
	return S_OK;
}

static HRESULT WINAPI store_SetValue(IPropertyStore *iface, REFPROPERTYKEY key, REFPROPVARIANT value)
{
	if (!key || !value)
		return E_POINTER;
	if (store_of(iface)->access == STGM_READ)
		return STG_E_ACCESSDENIED;
	/* The endpoints' properties are fixed; Windows also denies programs without administrator rights. */
	return E_ACCESSDENIED;
}

static HRESULT WINAPI store_Commit(IPropertyStore *iface) { return store_of(iface)->access == STGM_READ ? STG_E_ACCESSDENIED : S_OK; }

static const IPropertyStoreVtbl store_vtbl = {
	store_QueryInterface, store_AddRef, store_Release, store_GetCount, store_GetAt, store_GetValue, store_SetValue, store_Commit,
};

/* ----- Endpoints ----- */

static device *device_from(IMMDevice *iface) { return CONTAINING_RECORD(iface, device, IMMDevice_iface); }
static device *endpoint_from(IMMEndpoint *iface) { return CONTAINING_RECORD(iface, device, IMMEndpoint_iface); }

static HRESULT WINAPI device_QueryInterface(IMMDevice *iface, REFIID riid, void **out)
{
	device *device = device_from(iface);
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IMMDevice))
		*out = &device->IMMDevice_iface;
	else if (same_guid(riid, &IID_IMMEndpoint))
		*out = &device->IMMEndpoint_iface;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	return S_OK;
}

/* The endpoints belong to the process: references are counted by nobody. */
static ULONG WINAPI device_AddRef(IMMDevice *iface) { return 2; }
static ULONG WINAPI device_Release(IMMDevice *iface) { return 1; }

static HRESULT WINAPI device_Activate(IMMDevice *iface, REFIID riid, DWORD context, PROPVARIANT *params, void **out)
{
	device *device = device_from(iface);
	if (!riid || !out)
		return E_POINTER;
	*out = NULL;
	if (same_guid(riid, &IID_IAudioClient) || same_guid(riid, &IID_IAudioClient2) || same_guid(riid, &IID_IAudioClient3))
		return create_client(device, out);
	if (same_guid(riid, &IID_IAudioEndpointVolume) || same_guid(riid, &IID_IAudioEndpointVolumeEx))
		*out = &device->volume.IAudioEndpointVolumeEx_iface;
	else if (same_guid(riid, &IID_IAudioSessionManager) || same_guid(riid, &IID_IAudioSessionManager2))
		*out = &device->IAudioSessionManager2_iface;
	else if (same_guid(riid, &IID_IAudioMeterInformation))
		*out = &device->IAudioMeterInformation_iface;
	else
		return E_NOINTERFACE;
	return S_OK;
}

static HRESULT WINAPI device_OpenPropertyStore(IMMDevice *iface, DWORD access, IPropertyStore **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (access != STGM_READ && access != STGM_WRITE && access != STGM_READWRITE)
		return E_INVALIDARG;
	property_store *store = ALLOC(sizeof(*store));
	if (!store)
		return E_OUTOFMEMORY;
	store->IPropertyStore_iface.lpVtbl = &store_vtbl;
	store->ref = 1;
	store->device = device_from(iface);
	store->access = access;
	*out = &store->IPropertyStore_iface;
	return S_OK;
}

static HRESULT WINAPI device_GetId(IMMDevice *iface, LPWSTR *id)
{
	if (!id)
		return E_POINTER;
	*id = copy_string(device_from(iface)->id);
	return *id ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI device_GetState(IMMDevice *iface, DWORD *state)
{
	if (!state)
		return E_POINTER;
	*state = DEVICE_STATE_ACTIVE;
	return S_OK;
}

static const IMMDeviceVtbl device_vtbl = {
	device_QueryInterface, device_AddRef, device_Release, device_Activate, device_OpenPropertyStore, device_GetId, device_GetState,
};

static HRESULT WINAPI endpoint_QueryInterface(IMMEndpoint *iface, REFIID riid, void **out)
{
	return device_QueryInterface(&endpoint_from(iface)->IMMDevice_iface, riid, out);
}
static ULONG WINAPI endpoint_AddRef(IMMEndpoint *iface) { return 2; }
static ULONG WINAPI endpoint_Release(IMMEndpoint *iface) { return 1; }

static HRESULT WINAPI endpoint_GetDataFlow(IMMEndpoint *iface, EDataFlow *flow)
{
	if (!flow)
		return E_POINTER;
	*flow = endpoint_from(iface)->flow;
	return S_OK;
}

static const IMMEndpointVtbl endpoint_vtbl = {endpoint_QueryInterface, endpoint_AddRef, endpoint_Release, endpoint_GetDataFlow};

static device devices[2] = {
	{.flow = eRender, .id = L"{0.0.0.00000000}." RENDER_GUID_TEXT, .name = L"Speakers (Raven Audio)"},
	{.flow = eCapture, .id = L"{0.0.1.00000000}." CAPTURE_GUID_TEXT, .name = L"Microphone (Raven Audio)"},
};

device *device_of(EDataFlow flow) { return &devices[flow == eRender ? 0 : 1]; }

/* ----- Collection ----- */

typedef struct collection {
	IMMDeviceCollection IMMDeviceCollection_iface;
	LONG ref;
	UINT count;
	device *items[2];
} collection;

static collection *collection_of(IMMDeviceCollection *iface) { return CONTAINING_RECORD(iface, collection, IMMDeviceCollection_iface); }

static HRESULT WINAPI collection_QueryInterface(IMMDeviceCollection *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IMMDeviceCollection)) {
		*out = iface;
		IMMDeviceCollection_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI collection_AddRef(IMMDeviceCollection *iface) { return InterlockedIncrement(&collection_of(iface)->ref); }

static ULONG WINAPI collection_Release(IMMDeviceCollection *iface)
{
	collection *items = collection_of(iface);
	const ULONG ref = InterlockedDecrement(&items->ref);
	if (!ref)
		FREE(items);
	return ref;
}

static HRESULT WINAPI collection_GetCount(IMMDeviceCollection *iface, UINT *count)
{
	if (!count)
		return E_POINTER;
	*count = collection_of(iface)->count;
	return S_OK;
}

static HRESULT WINAPI collection_Item(IMMDeviceCollection *iface, UINT index, IMMDevice **out)
{
	collection *items = collection_of(iface);
	if (!out)
		return E_POINTER;
	if (index >= items->count) {
		*out = NULL;
		return E_INVALIDARG;
	}
	*out = &items->items[index]->IMMDevice_iface;
	return S_OK;
}

static const IMMDeviceCollectionVtbl collection_vtbl = {
	collection_QueryInterface, collection_AddRef, collection_Release, collection_GetCount, collection_Item,
};

/* ----- Enumerator ----- */

static HRESULT WINAPI enumerator_QueryInterface(IMMDeviceEnumerator *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IMMDeviceEnumerator)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI enumerator_AddRef(IMMDeviceEnumerator *iface) { return 2; }
static ULONG WINAPI enumerator_Release(IMMDeviceEnumerator *iface) { return 1; }

static HRESULT WINAPI enumerator_EnumAudioEndpoints(IMMDeviceEnumerator *iface, EDataFlow flow, DWORD mask, IMMDeviceCollection **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if ((unsigned)flow > eAll || !mask || (mask & ~DEVICE_STATEMASK_ALL))
		return E_INVALIDARG;
	collection *items = ALLOC(sizeof(*items));
	if (!items)
		return E_OUTOFMEMORY;
	items->IMMDeviceCollection_iface.lpVtbl = &collection_vtbl;
	items->ref = 1;
	if (mask & DEVICE_STATE_ACTIVE) {
		if (flow == eRender || flow == eAll)
			items->items[items->count++] = &devices[0];
		if (flow == eCapture || flow == eAll)
			items->items[items->count++] = &devices[1];
	}
	*out = &items->IMMDeviceCollection_iface;
	return S_OK;
}

static HRESULT WINAPI enumerator_GetDefaultAudioEndpoint(IMMDeviceEnumerator *iface, EDataFlow flow, ERole role, IMMDevice **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if ((flow != eRender && flow != eCapture) || (unsigned)role >= ERole_enum_count)
		return E_INVALIDARG;
	*out = &device_of(flow)->IMMDevice_iface;
	return S_OK;
}

static HRESULT WINAPI enumerator_GetDevice(IMMDeviceEnumerator *iface, LPCWSTR id, IMMDevice **out)
{
	if (!id || !out)
		return E_POINTER;
	for (int index = 0; index < 2; index++)
		if (same_text(id, devices[index].id)) {
			*out = &devices[index].IMMDevice_iface;
			return S_OK;
		}
	*out = NULL;
	return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

/* The two endpoints never come or go, nor does the default change: there is nothing to tell a listener. */
static HRESULT WINAPI enumerator_RegisterEndpointNotificationCallback(IMMDeviceEnumerator *iface, IMMNotificationClient *client)
{
	return client ? S_OK : E_POINTER;
}

static HRESULT WINAPI enumerator_UnregisterEndpointNotificationCallback(IMMDeviceEnumerator *iface, IMMNotificationClient *client)
{
	return client ? S_OK : E_POINTER;
}

static const IMMDeviceEnumeratorVtbl enumerator_vtbl = {
	enumerator_QueryInterface, enumerator_AddRef, enumerator_Release, enumerator_EnumAudioEndpoints, enumerator_GetDefaultAudioEndpoint,
	enumerator_GetDevice, enumerator_RegisterEndpointNotificationCallback, enumerator_UnregisterEndpointNotificationCallback,
};

static IMMDeviceEnumerator enumerator = {&enumerator_vtbl};

/* ----- Class factory ----- */

static HRESULT WINAPI factory_QueryInterface(IClassFactory *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IClassFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef(IClassFactory *iface) { return 2; }
static ULONG WINAPI factory_Release(IClassFactory *iface) { return 1; }

static HRESULT WINAPI factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	return IMMDeviceEnumerator_QueryInterface(&enumerator, riid, out);
}

static HRESULT WINAPI factory_LockServer(IClassFactory *iface, BOOL lock) { return S_OK; }

static const IClassFactoryVtbl factory_vtbl = {
	factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInstance, factory_LockServer,
};

static IClassFactory factory = {&factory_vtbl};

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!same_guid(clsid, &CLSID_MMDeviceEnumerator))
		return CLASS_E_CLASSNOTAVAILABLE;
	return factory_QueryInterface(&factory, riid, out);
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

/* ----- ActivateAudioInterfaceAsync ----- */

typedef struct activation {
	IActivateAudioInterfaceAsyncOperation IActivateAudioInterfaceAsyncOperation_iface;
	LONG ref;
	IActivateAudioInterfaceCompletionHandler *handler;
	HRESULT result;
	IUnknown *activated;
} activation;

static activation *activation_of(IActivateAudioInterfaceAsyncOperation *iface)
{
	return CONTAINING_RECORD(iface, activation, IActivateAudioInterfaceAsyncOperation_iface);
}

static HRESULT WINAPI activation_QueryInterface(IActivateAudioInterfaceAsyncOperation *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IActivateAudioInterfaceAsyncOperation) ||
		same_guid(riid, &IID_IAgileObject)) {
		*out = iface;
		IActivateAudioInterfaceAsyncOperation_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI activation_AddRef(IActivateAudioInterfaceAsyncOperation *iface) { return InterlockedIncrement(&activation_of(iface)->ref); }

static ULONG WINAPI activation_Release(IActivateAudioInterfaceAsyncOperation *iface)
{
	activation *operation = activation_of(iface);
	const ULONG ref = InterlockedDecrement(&operation->ref);
	if (!ref) {
		if (operation->activated)
			IUnknown_Release(operation->activated);
		IActivateAudioInterfaceCompletionHandler_Release(operation->handler);
		FREE(operation);
	}
	return ref;
}

static HRESULT WINAPI activation_GetActivateResult(IActivateAudioInterfaceAsyncOperation *iface, HRESULT *result, IUnknown **out)
{
	activation *operation = activation_of(iface);
	if (!result || !out)
		return E_POINTER;
	*result = operation->result;
	*out = operation->activated;
	if (operation->activated)
		IUnknown_AddRef(operation->activated);
	return S_OK;
}

static const IActivateAudioInterfaceAsyncOperationVtbl activation_vtbl = {
	activation_QueryInterface, activation_AddRef, activation_Release, activation_GetActivateResult,
};

static DWORD WINAPI complete_activation(void *parameter)
{
	activation *operation = parameter;
	IActivateAudioInterfaceCompletionHandler_ActivateCompleted(operation->handler, &operation->IActivateAudioInterfaceAsyncOperation_iface);
	IActivateAudioInterfaceAsyncOperation_Release(&operation->IActivateAudioInterfaceAsyncOperation_iface);
	return 0;
}

/* Names the default endpoint of a flow by its device interface class, as Windows' DeviceInformation does, or an endpoint by its id, bare or
 * in the interface path MediaDevice gives (\\?\SWD#MMDEVAPI#<id>#{<class>}). */
static device *device_named(const WCHAR *path)
{
	if (same_text(path, L"{E6327CAD-DCEC-4949-AE8A-991E976A79D2}"))
		return &devices[0];
	if (same_text(path, L"{2EEF81BE-33FA-4800-9670-1CD474972C3F}"))
		return &devices[1];
	for (int index = 0; index < 2; index++)
		if (holds_text(path, devices[index].id))
			return &devices[index];
	return NULL;
}

/* The interface is made at once; the handler is told on a thread of its own, as on Windows, where it never runs on the caller's. */
__declspec(dllexport) HRESULT WINAPI ActivateAudioInterfaceAsync(const WCHAR *path, REFIID riid, PROPVARIANT *params,
	IActivateAudioInterfaceCompletionHandler *handler, IActivateAudioInterfaceAsyncOperation **out)
{
	if (!path || !riid || !handler || !out)
		return E_POINTER;
	*out = NULL;
	activation *operation = ALLOC(sizeof(*operation));
	if (!operation)
		return E_OUTOFMEMORY;
	operation->IActivateAudioInterfaceAsyncOperation_iface.lpVtbl = &activation_vtbl;
	operation->ref = 2;
	operation->handler = handler;
	IActivateAudioInterfaceCompletionHandler_AddRef(handler);
	device *device = device_named(path);
	if (!device)
		operation->result = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	else
		operation->result = device_Activate(&device->IMMDevice_iface, riid, CLSCTX_ALL, params, (void **)&operation->activated);
	HANDLE thread = CreateThread(NULL, 0, complete_activation, operation, 0, NULL);
	if (!thread) {
		operation->ref = 1;
		IActivateAudioInterfaceAsyncOperation_Release(&operation->IActivateAudioInterfaceAsyncOperation_iface);
		return HRESULT_FROM_WIN32(GetLastError());
	}
	CloseHandle(thread);
	*out = &operation->IActivateAudioInterfaceAsyncOperation_iface;
	return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(instance);
		InitializeCriticalSection(&audio_lock);
		for (int index = 0; index < 2; index++) {
			devices[index].IMMDevice_iface.lpVtbl = &device_vtbl;
			devices[index].IMMEndpoint_iface.lpVtbl = &endpoint_vtbl;
			volume_init(&devices[index]);
		}
	}
	return TRUE;
}
