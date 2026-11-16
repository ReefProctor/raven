/* SPDX-License-Identifier: MIT
 *
 * mmdevapi.dll for each width: the audio endpoints, WASAPI's render and capture clients and the volume and session objects, Raven code on
 * the public headers. A client's samples go through the shared page of a stream of the runtime's sound engine (raven_audio.h): the calls
 * that move them (GetBuffer, ReleaseBuffer, GetCurrentPadding, GetPosition) never leave the guest.
 */
#pragma once

#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define CONST_VTABLE
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
#include <string.h>

#include "../../nt/raven_audio.h"

/* endpointvolume.h of the Windows SDK declares the peak meter; MinGW's does not. */
#ifndef __IAudioMeterInformation_INTERFACE_DEFINED__
#define __IAudioMeterInformation_INTERFACE_DEFINED__
DEFINE_GUID(IID_IAudioMeterInformation, 0xc02216f6, 0x8c67, 0x4b5b, 0x9d, 0x00, 0xd0, 0x08, 0xe7, 0x3e, 0x00, 0x64);
typedef struct IAudioMeterInformation IAudioMeterInformation;
typedef struct IAudioMeterInformationVtbl {
	HRESULT(STDMETHODCALLTYPE *QueryInterface)(IAudioMeterInformation *This, REFIID riid, void **object);
	ULONG(STDMETHODCALLTYPE *AddRef)(IAudioMeterInformation *This);
	ULONG(STDMETHODCALLTYPE *Release)(IAudioMeterInformation *This);
	HRESULT(STDMETHODCALLTYPE *GetPeakValue)(IAudioMeterInformation *This, float *peak);
	HRESULT(STDMETHODCALLTYPE *GetMeteringChannelCount)(IAudioMeterInformation *This, UINT *count);
	HRESULT(STDMETHODCALLTYPE *GetChannelsPeakValues)(IAudioMeterInformation *This, UINT32 count, float *peaks);
	HRESULT(STDMETHODCALLTYPE *QueryHardwareSupport)(IAudioMeterInformation *This, DWORD *mask);
} IAudioMeterInformationVtbl;
struct IAudioMeterInformation {
	CONST_VTBL IAudioMeterInformationVtbl *lpVtbl;
};
#endif

/* The extensible format's subtypes; MinGW declares ksmedia.h's but leaves them to a library this component does not link. */
static const GUID subtype_pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID subtype_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/* The heap functions need the guest RTL component, which a build may not provision; LocalAlloc is always served. */
#define ALLOC(size) LocalAlloc(LPTR, (size))
#define FREE(block) LocalFree(block)

static inline BOOL level_valid(float level) { return level >= 0 && level <= 1; }

/* The mix format of both endpoints: what the engine mixes in on Windows, and what the sound server is asked for when a program takes it. */
#define MIX_RATE 48000u
#define MIX_CHANNELS 2u
#define MAX_CHANNELS 8u
/* The device period, 10 ms, and the shortest one a program may ask for, 3 ms, in 100-nanosecond units. */
#define DEFAULT_PERIOD 100000
#define MINIMUM_PERIOD 30000
#define MIN_DECIBELS (-65.25f)

/* A program's sounds in one session of one endpoint: the volume and mute every client in it is scaled by, its channel volumes, and the
 * programs listening. Sessions live as long as the process. */
typedef struct session {
	IAudioSessionControl2 IAudioSessionControl2_iface;
	ISimpleAudioVolume ISimpleAudioVolume_iface;
	IChannelAudioVolume IChannelAudioVolume_iface;
	struct session *next;
	struct device *device;
	GUID guid;
	float volume;
	BOOL mute;
	float channels[MIX_CHANNELS];
	/* Clients of the session started now: the session is active while there is one. */
	UINT running;
	WCHAR *display_name, *icon_path;
	GUID grouping;
	IAudioSessionEvents **listeners;
	UINT listener_count, listener_capacity;
} session;

/* The endpoint's volume, which every client of the endpoint is scaled by. */
typedef struct endpoint_volume {
	IAudioEndpointVolumeEx IAudioEndpointVolumeEx_iface;
	float master;
	float channels[MIX_CHANNELS];
	BOOL mute;
	IAudioEndpointVolumeCallback **listeners;
	UINT listener_count, listener_capacity;
} endpoint_volume;

/* One of the two endpoints, the default speakers and microphone: objects of the process, never freed. */
typedef struct device {
	IMMDevice IMMDevice_iface;
	IMMEndpoint IMMEndpoint_iface;
	IAudioSessionManager2 IAudioSessionManager2_iface;
	IAudioMeterInformation IAudioMeterInformation_iface;
	EDataFlow flow;
	const WCHAR *id, *name;
	endpoint_volume volume;
	session *sessions;
} device;

typedef struct client {
	IAudioClient3 IAudioClient3_iface;
	IAudioRenderClient IAudioRenderClient_iface;
	IAudioCaptureClient IAudioCaptureClient_iface;
	IAudioClock IAudioClock_iface;
	IAudioClock2 IAudioClock2_iface;
	IAudioStreamVolume IAudioStreamVolume_iface;
	LONG ref;
	device *device;
	CRITICAL_SECTION lock;
	/* Set by Initialize once the fields below it are; they do not change afterwards. */
	volatile LONG initialized;
	DWORD flags;
	WAVEFORMATEX *format;
	HANDLE stream, event;
	RavenAudioShared *shared;
	BYTE *ring;
	UINT32 buffer_frames, period_frames, frame_bytes;
	BOOL started;
	session *session;
	float channels[MAX_CHANNELS];
	/* Frames handed out by GetBuffer and not yet released, and whether they sit in scratch because the ring wraps under them. */
	UINT32 pending;
	BYTE *scratch;
	BOOL in_scratch;
	/* Capture: frames dropped so far, so the next packet after more were dropped is flagged. */
	UINT64 glitches;
	/* The engine's glitch count when the pending capture packet was handed out. */
	UINT64 packet_glitches;
	/* The clock's last answer, in frames. */
	UINT64 last_position;
	struct client *next;
} client;

/* Guards the sessions, the endpoint volumes and the list of clients; never held while calling into the program. */
extern CRITICAL_SECTION audio_lock;
extern client *clients;

int same_guid(const GUID *left, const GUID *right);
WCHAR *copy_string(const WCHAR *text);
WAVEFORMATEX *copy_format(const WAVEFORMATEX *format);
WAVEFORMATEX *mix_format(void);
float decibels_of(float scalar);
float scalar_of(float decibels);
/* QueryPerformanceCounter in 100-nanosecond units, as the shared page's position_counter. */
UINT64 counter_now(void);

device *device_of(EDataFlow flow);
HRESULT create_client(device *device, void **out);
HRESULT session_for(device *device, const GUID *guid, session **out);
/* Writes into each client's page the volume the engine plays it at; with audio_lock held. */
void update_volumes(void);
void session_started(session *session, BOOL started);
void volume_init(device *device);
