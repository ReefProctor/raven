#include "kernel_context.h"

namespace raven {

void register_winmm_api(ApiRegistry &registry)
{
	constexpr uint32_t mmsyserr_baddeviceid = 2;
	registry.add("auxGetNumDevs", 0, [](ApiCall &call) { call.ret(0); });
	for (const auto *name : {"auxGetDevCapsA", "auxGetDevCapsW"})
		registry.add(name, 3, [](ApiCall &call) { call.ret(mmsyserr_baddeviceid); });
	// Windows 95 onwards answer version 3.10.
	registry.add("mmsystemGetVersion", 0, [](ApiCall &call) { call.ret(0x030a); });
}

}
