#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <mfapi.h>
#include <mferror.h>
#include <mmreg.h>
#include <msacm.h>
#include <strmif.h>

static int same_guid(const GUID *left, const GUID *right)
{
	return left->Data1 == right->Data1 && left->Data2 == right->Data2 && left->Data3 == right->Data3 &&
		left->Data4[0] == right->Data4[0] && left->Data4[1] == right->Data4[1] &&
		left->Data4[2] == right->Data4[2] && left->Data4[3] == right->Data4[3] &&
		left->Data4[4] == right->Data4[4] && left->Data4[5] == right->Data4[5] &&
		left->Data4[6] == right->Data4[6] && left->Data4[7] == right->Data4[7];
}

static const GUID media_type_audio = {0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID wave_format_representation = {0x05589f81, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
static const GUID mf_audio_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID mf_audio_pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID mf_mt_subtype = {0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
static const GUID mf_audio_channel_mask = {0x55fb5765, 0x644a, 0x4caf, {0x84, 0x79, 0x93, 0x89, 0x83, 0xbb, 0x15, 0x88}};
static const GUID mf_audio_valid_bits = {0xd9bf8d6a, 0x9530, 0x4b7c, {0x9d, 0xdf, 0xff, 0x6f, 0xd5, 0x8b, 0xbd, 0x06}};

static BOOL CALLBACK count_driver(HACMDRIVERID id, DWORD_PTR instance, DWORD support)
{
	(void)id;
	(void)support;
	++*(DWORD *)instance;
	return TRUE;
}

static BOOL CALLBACK count_tag(HACMDRIVERID id, LPACMFORMATTAGDETAILSW details, DWORD_PTR instance, DWORD support)
{
	(void)id;
	(void)details;
	(void)support;
	++*(DWORD *)instance;
	return TRUE;
}

static BOOL CALLBACK count_format(HACMDRIVERID id, LPACMFORMATDETAILSW details, DWORD_PTR instance, DWORD support)
{
	(void)id;
	(void)details;
	(void)support;
	++*(DWORD *)instance;
	return TRUE;
}

/* Exercise the runtime's multimedia thread registration, Media Foundation sample objects, and PCM/MS ADPCM conversion. */
int raven_entry(void)
{
	if (MFStartup(MF_VERSION, MFSTARTUP_FULL) != S_OK)
		return 35;
	DWORD task_index = 0xdead;
	HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
	if (!task || task_index != 0)
		return 1;
	if (!AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH))
		return 2;
	ULONG responsiveness = 0xdead;
	if (AvQuerySystemResponsiveness(task, &responsiveness) != S_OK || responsiveness != 0)
		return 3;
	if (!AvRevertMmThreadCharacteristics(task))
		return 4;

	IMFSample *sample = NULL;
	if (FAILED(MFCreateSample(&sample)) || !sample)
		return 5;
	IMFMediaBuffer *empty_contiguous = NULL;
	if (IMFSample_ConvertToContiguousBuffer(sample, &empty_contiguous) != E_UNEXPECTED || empty_contiguous)
		return 37;
	LONGLONG sample_time = 0;
	if (IMFSample_SetSampleTime(sample, 123456) != S_OK || IMFSample_GetSampleTime(sample, &sample_time) != S_OK ||
		sample_time != 123456)
		return 6;
	WAVEFORMATEX wave = {WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0};
	IMFMediaType *type = NULL;
	if (FAILED(MFCreateMediaType(&type)) || !type)
		return 7;
	if (MFInitMediaTypeFromWaveFormatEx(type, &wave, sizeof(wave)) != S_OK)
		return 8;
	IMFMediaType *unsupported_type = NULL;
	struct {
		WAVEFORMATEX format;
		BYTE extra[22];
	} unsupported_wave = {{WAVE_FORMAT_EXTENSIBLE, 2, 48000, 192000, 4, 16, 22}, {0}};
	if (FAILED(MFCreateMediaType(&unsupported_type)) || !unsupported_type)
		return 35;
	if (MFInitMediaTypeFromWaveFormatEx(unsupported_type, &unsupported_wave.format, sizeof(unsupported_wave)) !=
		MF_E_INVALIDMEDIATYPE)
		return 36;
	IMFMediaType_Release(unsupported_type);
	WAVEFORMATEXTENSIBLE valid_extensible = {0};
	valid_extensible.Format = (WAVEFORMATEX){WAVE_FORMAT_EXTENSIBLE, 2, 48000, 384000, 8, 32, 22};
	valid_extensible.Samples.wValidBitsPerSample = 32;
	valid_extensible.dwChannelMask = 3;
	valid_extensible.SubFormat = mf_audio_float;
	IMFMediaType *extensible_type = NULL;
	if (FAILED(MFCreateMediaType(&extensible_type)) || !extensible_type ||
		MFInitMediaTypeFromWaveFormatEx(extensible_type, &valid_extensible.Format, sizeof(valid_extensible)) != S_OK)
		return 38;
	GUID extensible_subtype;
	UINT32 channel_mask = 0, valid_bits = 0, extensible_size = 0;
	if (IMFMediaType_GetGUID(extensible_type, &mf_mt_subtype, &extensible_subtype) != S_OK ||
		!same_guid(&extensible_subtype, &mf_audio_float) ||
		IMFMediaType_GetUINT32(extensible_type, &mf_audio_channel_mask, &channel_mask) != S_OK || channel_mask != 3 ||
		IMFMediaType_GetUINT32(extensible_type, &mf_audio_valid_bits, &valid_bits) != S_OK || valid_bits != 32)
		return 39;
	WAVEFORMATEX *extensible_roundtrip = NULL;
	if (MFCreateWaveFormatExFromMFMediaType(extensible_type, &extensible_roundtrip, &extensible_size, 0) != S_OK ||
		!extensible_roundtrip || extensible_roundtrip->wFormatTag != WAVE_FORMAT_IEEE_FLOAT)
		return 40;
	CoTaskMemFree(extensible_roundtrip);
	IMFMediaType_Release(extensible_type);
	valid_extensible.Format.wBitsPerSample = 16;
	valid_extensible.Format.nBlockAlign = 4;
	valid_extensible.Format.nAvgBytesPerSec = 192000;
	valid_extensible.Samples.wValidBitsPerSample = 16;
	valid_extensible.SubFormat = mf_audio_pcm;
	extensible_type = NULL;
	if (FAILED(MFCreateMediaType(&extensible_type)) || !extensible_type ||
		MFInitMediaTypeFromWaveFormatEx(extensible_type, &valid_extensible.Format, sizeof(valid_extensible)) != S_OK ||
		MFCreateWaveFormatExFromMFMediaType(extensible_type, &extensible_roundtrip, &extensible_size, 0) != S_OK ||
		!extensible_roundtrip || extensible_roundtrip->wFormatTag != WAVE_FORMAT_PCM)
		return 55;
	CoTaskMemFree(extensible_roundtrip);
	if (MFCreateWaveFormatExFromMFMediaType(extensible_type, &extensible_roundtrip, &extensible_size,
			MFWaveFormatExConvertFlag_ForceExtensible) != S_OK || extensible_size != sizeof(WAVEFORMATEXTENSIBLE) ||
		extensible_roundtrip->wFormatTag != WAVE_FORMAT_EXTENSIBLE || extensible_roundtrip->cbSize != 22 ||
		((WAVEFORMATEXTENSIBLE *)extensible_roundtrip)->dwChannelMask != 3 ||
		((WAVEFORMATEXTENSIBLE *)extensible_roundtrip)->Samples.wValidBitsPerSample != 16 ||
		!same_guid(&((WAVEFORMATEXTENSIBLE *)extensible_roundtrip)->SubFormat, &mf_audio_pcm))
		return 56;
	CoTaskMemFree(extensible_roundtrip);
	IMFMediaType_Release(extensible_type);
	GUID major;
	if (IMFMediaType_GetMajorType(type, &major) != S_OK || !same_guid(&major, &media_type_audio))
		return 9;
	WAVEFORMATEX *roundtrip = NULL;
	UINT32 format_size = 0;
	if (MFCreateWaveFormatExFromMFMediaType(type, &roundtrip, &format_size, 0) != S_OK || !roundtrip ||
		format_size < sizeof(*roundtrip) || roundtrip->nSamplesPerSec != wave.nSamplesPerSec ||
		roundtrip->nChannels != wave.nChannels || roundtrip->wBitsPerSample != wave.wBitsPerSample)
		return 10;
	CoTaskMemFree(roundtrip);
	void *representation = NULL;
	const AM_MEDIA_TYPE *media;
	if (IMFMediaType_GetRepresentation(type, wave_format_representation, &representation) != S_OK || !(media = representation) ||
		!same_guid(&media->majortype, &media_type_audio) || !same_guid(&media->subtype, &mf_audio_pcm) ||
		!same_guid(&media->formattype, &wave_format_representation) || media->cbFormat != sizeof(WAVEFORMATEX) ||
		media->lSampleSize != wave.nBlockAlign ||
		((const WAVEFORMATEX *)media->pbFormat)->nSamplesPerSec != wave.nSamplesPerSec ||
		IMFMediaType_FreeRepresentation(type, wave_format_representation, representation) != S_OK)
		return 34;
	if (IMFMediaType_GetRepresentation(type, mf_mt_subtype, &representation) != MF_E_UNSUPPORTED_REPRESENTATION || representation)
		return 57;
	/* A missing key deletes without error, a NULL value only asks whether the key exists, and a blob needs its bytes. */
	GUID index_key;
	UINT32 item_count = 0;
	PROPVARIANT empty_blob = {0};
	empty_blob.vt = VT_BLOB;
	empty_blob.blob.cbSize = 4;
	if (IMFMediaType_DeleteItem(type, &mf_audio_channel_mask) != S_OK ||
		IMFMediaType_GetItem(type, &mf_mt_subtype, NULL) != S_OK ||
		IMFMediaType_GetItem(type, &mf_audio_channel_mask, NULL) != MF_E_ATTRIBUTENOTFOUND ||
		IMFMediaType_GetCount(type, &item_count) != S_OK || !item_count ||
		IMFMediaType_GetItemByIndex(type, item_count, &index_key, NULL) != E_INVALIDARG ||
		IMFMediaType_GetItemByIndex(type, item_count - 1, &index_key, NULL) != S_OK ||
		IMFMediaType_SetItem(type, &mf_audio_channel_mask, &empty_blob) != E_INVALIDARG ||
		IMFMediaType_LockStore(type) != S_OK || IMFMediaType_UnlockStore(type) != S_OK)
		return 58;
	IMFMediaBuffer *buffer = NULL;
	if (MFCreateMemoryBuffer(4096, &buffer) != S_OK || !buffer)
		return 11;
	BYTE *data = NULL;
	DWORD max_length = 0;
	if (IMFMediaBuffer_Lock(buffer, &data, &max_length, NULL) != S_OK || !data || max_length != 4096)
		return 12;
	data[0] = 0x5a;
	if (IMFMediaBuffer_Unlock(buffer) != S_OK || IMFMediaBuffer_SetCurrentLength(buffer, 1) != S_OK)
		return 13;
	if (IMFSample_AddBuffer(sample, buffer) != S_OK || IMFSample_GetTotalLength(sample, &max_length) != S_OK ||
		max_length != 1)
		return 14;
	IMFMediaBuffer *contiguous = NULL;
	if (IMFSample_ConvertToContiguousBuffer(sample, &contiguous) != S_OK || contiguous != buffer)
		return 15;
	IMFMediaBuffer_Release(contiguous);
	IMFMediaBuffer *second = NULL;
	if (MFCreateMemoryBuffer(2, &second) != S_OK || IMFMediaBuffer_Lock(second, &data, NULL, NULL) != S_OK)
		return 41;
	data[0] = 0x11;
	data[1] = 0x22;
	if (IMFMediaBuffer_Unlock(second) != S_OK || IMFMediaBuffer_SetCurrentLength(second, 2) != S_OK ||
		IMFSample_AddBuffer(sample, second) != S_OK ||
		IMFSample_ConvertToContiguousBuffer(sample, &contiguous) != S_OK || contiguous == buffer)
		return 42;
	DWORD sample_buffer_count = 0, contiguous_length = 0;
	if (IMFSample_GetBufferCount(sample, &sample_buffer_count) != S_OK || sample_buffer_count != 1 ||
		IMFMediaBuffer_GetCurrentLength(contiguous, &contiguous_length) != S_OK || contiguous_length != 3 ||
		IMFMediaBuffer_Lock(contiguous, &data, NULL, NULL) != S_OK ||
		data[0] != 0x5a || data[1] != 0x11 || data[2] != 0x22 ||
		IMFMediaBuffer_Unlock(contiguous) != S_OK)
		return 43;
	IMFMediaBuffer_Release(contiguous);
	IMFMediaBuffer_Release(second);
	IMFMediaBuffer_Release(buffer);
	IMFMediaType_Release(type);
	IMFSample_Release(sample);
	IMFMediaBuffer *aligned = NULL;
	if (MFCreateAlignedMemoryBuffer(4096, MF_16_BYTE_ALIGNMENT, &aligned) != S_OK || !aligned)
		return 16;
	if (IMFMediaBuffer_Lock(aligned, &data, NULL, NULL) != S_OK || ((ULONG_PTR)data & 15))
		return 17;
	IMFMediaBuffer_Unlock(aligned);
	IMFMediaBuffer_Release(aligned);
	aligned = (IMFMediaBuffer *)0xdead;
	if (MFCreateAlignedMemoryBuffer(4096, 16, &aligned) != E_INVALIDARG || aligned)
		return 27;
	WAVEFORMATEX pcm = {WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0};
	BYTE compressed_storage[sizeof(WAVEFORMATEX) + 4 + 7 * sizeof(ADPCMCOEFSET)] = {0};
	WAVEFORMATEX *compressed = (WAVEFORMATEX *)compressed_storage;
	compressed->wFormatTag = WAVE_FORMAT_ADPCM;
	if (acmGetVersion() < 0x04000000 || acmFormatSuggest(NULL, &pcm, compressed, sizeof(compressed_storage),
		ACM_FORMATSUGGESTF_WFORMATTAG) != MMSYSERR_NOERROR || compressed->wFormatTag != WAVE_FORMAT_ADPCM)
		return 18;
	ADPCMWAVEFORMAT *adpcm = (ADPCMWAVEFORMAT *)compressed;
	if (adpcm->wSamplesPerBlock != 500 || adpcm->wNumCoef != 7)
		return 19;
	WAVEFORMATEX suggested_pcm;
	for (DWORD i = 0; i < sizeof(suggested_pcm); ++i)
		((BYTE *)&suggested_pcm)[i] = 0xcc;
	if (acmFormatSuggest(NULL, compressed, &suggested_pcm, sizeof(suggested_pcm), 0) != MMSYSERR_NOERROR ||
		suggested_pcm.wFormatTag != WAVE_FORMAT_PCM || suggested_pcm.wBitsPerSample != 16)
		return 44;
	for (DWORD i = 0; i < sizeof(suggested_pcm); ++i)
		((BYTE *)&suggested_pcm)[i] = 0xcc;
	suggested_pcm.wBitsPerSample = 8;
	if (acmFormatSuggest(NULL, &pcm, &suggested_pcm, sizeof(suggested_pcm), ACM_FORMATSUGGESTF_WBITSPERSAMPLE) !=
		MMSYSERR_NOERROR || suggested_pcm.wBitsPerSample != 8 || suggested_pcm.nBlockAlign != 1)
		return 45;
	suggested_pcm.nChannels = 2;
	if (acmFormatSuggest(NULL, &pcm, &suggested_pcm, sizeof(suggested_pcm), ACM_FORMATSUGGESTF_NCHANNELS) != ACMERR_NOTPOSSIBLE)
		return 46;
	HACMSTREAM encoder = NULL;
	if (acmStreamOpen(&encoder, NULL, &pcm, compressed, NULL, 0, 0, 0) != MMSYSERR_NOERROR || !encoder)
		return 20;
	DWORD encoded_size = 0;
	if (acmStreamSize(encoder, 1000, &encoded_size, ACM_STREAMSIZEF_SOURCE) != MMSYSERR_NOERROR || encoded_size != 256)
		return 21;
	SHORT pcm_samples[500];
	for (DWORD i = 0; i < 500; ++i)
		pcm_samples[i] = (SHORT)((LONG)((i * 97) % 12000) - 6000);
	BYTE encoded[256] = {0};
	ACMSTREAMHEADER encode_header = {0};
	encode_header.cbStruct = sizeof(encode_header);
	encode_header.pbSrc = (LPBYTE)pcm_samples;
	encode_header.cbSrcLength = sizeof(pcm_samples);
	encode_header.pbDst = encoded;
	encode_header.cbDstLength = sizeof(encoded);
	if (acmStreamPrepareHeader(encoder, &encode_header, 0) != MMSYSERR_NOERROR ||
		acmStreamConvert(encoder, &encode_header, ACM_STREAMCONVERTF_BLOCKALIGN) != MMSYSERR_NOERROR ||
		encode_header.cbSrcLengthUsed != sizeof(pcm_samples) || encode_header.cbDstLengthUsed != sizeof(encoded))
		return 22;
	if (acmStreamUnprepareHeader(encoder, &encode_header, 0) != MMSYSERR_NOERROR ||
		acmStreamClose(encoder, 0) != MMSYSERR_NOERROR)
		return 23;
	HACMSTREAM decoder = NULL;
	WAVEFORMATEX decoded_format = pcm;
	if (acmStreamOpen(&decoder, NULL, compressed, &decoded_format, NULL, 0, 0, 0) != MMSYSERR_NOERROR || !decoder)
		return 24;
	DWORD decoded_size = 0;
	if (acmStreamSize(decoder, 0, &decoded_size, ACM_STREAMSIZEF_SOURCE) != MMSYSERR_INVALPARAM ||
		acmStreamSize(decoder, 1500, &decoded_size, ACM_STREAMSIZEF_DESTINATION) != MMSYSERR_NOERROR || decoded_size != 256 ||
		acmStreamSize(decoder, 255, &decoded_size, ACM_STREAMSIZEF_SOURCE) != ACMERR_NOTPOSSIBLE)
		return 47;
	SHORT decoded_samples[500] = {0};
	ACMSTREAMHEADER decode_header = {0};
	decode_header.cbStruct = sizeof(decode_header);
	decode_header.pbSrc = encoded;
	decode_header.cbSrcLength = sizeof(encoded);
	decode_header.pbDst = (LPBYTE)decoded_samples;
	decode_header.cbDstLength = sizeof(decoded_samples);
	if (acmStreamConvert(decoder, &decode_header, ACM_STREAMCONVERTF_BLOCKALIGN) != ACMERR_UNPREPARED)
		return 59;
	if (acmStreamPrepareHeader(decoder, &decode_header, 0) != MMSYSERR_NOERROR ||
		acmStreamConvert(decoder, &decode_header, ACM_STREAMCONVERTF_BLOCKALIGN) != MMSYSERR_NOERROR ||
		decode_header.cbSrcLengthUsed != sizeof(encoded) || decode_header.cbDstLengthUsed != sizeof(decoded_samples))
		return 25;
	/* A block that starts at the largest delta and keeps the code that grows it fastest still decodes. The volatile index keeps the
	 * compiler from calling memset, which this fixture does not link. */
	encoded[1] = encoded[2] = 0xff;
	for (volatile DWORD i = 7; i < sizeof(encoded); ++i)
		encoded[i] = 0x88;
	if (acmStreamConvert(decoder, &decode_header, ACM_STREAMCONVERTF_BLOCKALIGN) != MMSYSERR_NOERROR ||
		decode_header.cbSrcLengthUsed != sizeof(encoded) || decode_header.cbDstLengthUsed != sizeof(decoded_samples) ||
		decoded_samples[499] != -32768)
		return 60;
	if (acmStreamUnprepareHeader(decoder, &decode_header, 0) != MMSYSERR_NOERROR ||
		acmStreamClose(decoder, 0) != MMSYSERR_NOERROR)
		return 26;
	ACMFORMATTAGDETAILSW tag = {0};
	tag.cbStruct = sizeof(tag);
	tag.dwFormatTagIndex = 1;
	if (acmFormatTagDetailsW(NULL, &tag, ACM_FORMATTAGDETAILSF_INDEX) != MMSYSERR_NOERROR ||
		tag.dwFormatTag != WAVE_FORMAT_ADPCM || tag.cStandardFormats == 0)
		return 28;
	tag.dwFormatTag = WAVE_FORMAT_ADPCM;
	if (acmFormatTagDetailsW(NULL, &tag, ACM_FORMATTAGDETAILSF_FORMATTAG) != MMSYSERR_NOERROR ||
		tag.dwFormatTagIndex != 1)
		return 48;
	ACMFORMATDETAILSW format_details = {0};
	BYTE format_storage[sizeof(WAVEFORMATEX) + 4 + 7 * sizeof(ADPCMCOEFSET)] = {0};
	format_details.cbStruct = sizeof(format_details);
	format_details.pwfx = (WAVEFORMATEX *)format_storage;
	format_details.cbwfx = sizeof(format_storage);
	format_details.dwFormatTag = WAVE_FORMAT_PCM;
	format_details.dwFormatIndex = 0;
	if (acmFormatDetailsW(NULL, &format_details, ACM_FORMATDETAILSF_INDEX) != MMSYSERR_NOERROR ||
		format_details.pwfx->wFormatTag != WAVE_FORMAT_PCM || format_details.pwfx->nChannels != 1)
		return 49;
	DWORD enumerated = 0;
	ACMFORMATTAGDETAILSW tag_filter = {0};
	tag_filter.cbStruct = sizeof(tag_filter);
	if (acmFormatTagEnumW(NULL, &tag_filter, count_tag, (DWORD_PTR)&enumerated, 0) != MMSYSERR_NOERROR ||
		enumerated != 2)
		return 50;
	enumerated = 0;
	if (acmFormatEnumW(NULL, &format_details, count_format, (DWORD_PTR)&enumerated, 0) != MMSYSERR_NOERROR ||
		enumerated != 9)
		return 51;
	enumerated = 0;
	if (acmDriverEnum(count_driver, (DWORD_PTR)&enumerated, 0) != MMSYSERR_NOERROR || enumerated != 1)
		return 52;
	HACMDRIVER driver = NULL;
	HACMDRIVERID driver_id = NULL;
	ACMDRIVERDETAILSW driver_details = {0};
	if (acmDriverOpen(&driver, (HACMDRIVERID)1, 0) != MMSYSERR_NOERROR ||
		acmDriverID((HACMOBJ)driver, &driver_id, 0) != MMSYSERR_NOERROR || driver_id != (HACMDRIVERID)1)
		return 53;
	driver_details.cbStruct = sizeof(driver_details);
	if (acmDriverDetailsW(driver_id, &driver_details, 0) != MMSYSERR_NOERROR ||
		driver_details.cFormatTags != 2 || acmDriverClose(driver, 0) != MMSYSERR_NOERROR)
		return 54;
	WAVEFORMATEX stereo_pcm = {WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0};
	BYTE stereo_compressed_storage[sizeof(WAVEFORMATEX) + 4 + 7 * sizeof(ADPCMCOEFSET)] = {0};
	WAVEFORMATEX *stereo_compressed = (WAVEFORMATEX *)stereo_compressed_storage;
	stereo_compressed->wFormatTag = WAVE_FORMAT_ADPCM;
	if (acmFormatSuggest(NULL, &stereo_pcm, stereo_compressed, sizeof(stereo_compressed_storage),
		ACM_FORMATSUGGESTF_WFORMATTAG) != MMSYSERR_NOERROR ||
		((ADPCMWAVEFORMAT *)stereo_compressed)->wSamplesPerBlock != 244)
		return 29;
	HACMSTREAM stereo_encoder = NULL;
	if (acmStreamOpen(&stereo_encoder, NULL, &stereo_pcm, stereo_compressed, NULL, 0, 0, 0) != MMSYSERR_NOERROR)
		return 30;
	SHORT stereo_samples[244 * 2];
	for (DWORD i = 0; i < 244; ++i) {
		stereo_samples[i * 2] = (SHORT)((LONG)((i * 83) % 10000) - 5000);
		stereo_samples[i * 2 + 1] = (SHORT)((LONG)((i * 61) % 8000) - 4000);
	}
	BYTE stereo_encoded[256] = {0};
	ACMSTREAMHEADER stereo_header = {0};
	stereo_header.cbStruct = sizeof(stereo_header);
	stereo_header.pbSrc = (LPBYTE)stereo_samples;
	stereo_header.cbSrcLength = sizeof(stereo_samples);
	stereo_header.pbDst = stereo_encoded;
	stereo_header.cbDstLength = sizeof(stereo_encoded);
	if (acmStreamPrepareHeader(stereo_encoder, &stereo_header, 0) != MMSYSERR_NOERROR ||
		acmStreamConvert(stereo_encoder, &stereo_header, ACM_STREAMCONVERTF_BLOCKALIGN) != MMSYSERR_NOERROR ||
		stereo_header.cbDstLengthUsed != sizeof(stereo_encoded) ||
		acmStreamUnprepareHeader(stereo_encoder, &stereo_header, 0) != MMSYSERR_NOERROR ||
		acmStreamClose(stereo_encoder, 0) != MMSYSERR_NOERROR)
		return 31;
	HACMSTREAM stereo_decoder = NULL;
	if (acmStreamOpen(&stereo_decoder, NULL, stereo_compressed, &stereo_pcm, NULL, 0, 0, 0) != MMSYSERR_NOERROR)
		return 32;
	SHORT stereo_decoded[244 * 2] = {0};
	ACMSTREAMHEADER stereo_decode_header = {0};
	stereo_decode_header.cbStruct = sizeof(stereo_decode_header);
	stereo_decode_header.pbSrc = stereo_encoded;
	stereo_decode_header.cbSrcLength = sizeof(stereo_encoded);
	stereo_decode_header.pbDst = (LPBYTE)stereo_decoded;
	stereo_decode_header.cbDstLength = sizeof(stereo_decoded);
	if (acmStreamPrepareHeader(stereo_decoder, &stereo_decode_header, 0) != MMSYSERR_NOERROR ||
		acmStreamConvert(stereo_decoder, &stereo_decode_header, ACM_STREAMCONVERTF_BLOCKALIGN) != MMSYSERR_NOERROR ||
		stereo_decode_header.cbDstLengthUsed != sizeof(stereo_decoded) ||
		acmStreamUnprepareHeader(stereo_decoder, &stereo_decode_header, 0) != MMSYSERR_NOERROR ||
		acmStreamClose(stereo_decoder, 0) != MMSYSERR_NOERROR)
		return 33;
	if (MFShutdown() != S_OK || MFShutdown() != MF_E_SHUTDOWN)
		return 36;
	return 0;
}
