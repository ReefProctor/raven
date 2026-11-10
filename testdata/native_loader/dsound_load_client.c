#include <windows.h>

typedef DWORD(WINAPI *marker_function)(void);

int raven_entry(void)
{
	HMODULE first = LoadLibraryA("dsound.dll");
	if (!first)
		return 1;
	marker_function marker = (marker_function)GetProcAddress(first, "RavenLocalDsoundMarker");
#ifdef EXPECT_LOCAL_DSOUND
	if (!marker || marker() != 0x5241564eu)
		return 2;
#else
	if (!GetProcAddress(first, "DirectSoundCreate8"))
		return 3;
#endif
	if (!FreeLibrary(first))
		return 4;
	HMODULE second = LoadLibraryA("dsound.dll");
	if (!second)
		return 5;
	marker = (marker_function)GetProcAddress(second, "RavenLocalDsoundMarker");
#ifdef EXPECT_LOCAL_DSOUND
	if (!marker || marker() != 0x5241564eu)
		return 6;
#else
	if (!GetProcAddress(second, "DirectSoundCreate8"))
		return 7;
#endif
	return FreeLibrary(second) ? 0 : 8;
}
