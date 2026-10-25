// nn::ro (runtime-loaded NROs). The only NRO Hades ships is the Epic Online Services SDK (romfs:/nro/
// EOSSDK-Switch-Shipping.nro), used for online features. It is not recompiled: loading reports failure and the
// game carries on offline.
#include <nx/runtime.h>

namespace nx {

namespace {

constexpr uint32_t kRoNotLoaded = (2u << 9) | 22u;  // ro module 22

void ro_ok(CPU& c) { c.x[0] = 0; }

void GetBufferSize(CPU& c) {  // (size_t* out, const void* nro)
    st<uint64_t>(c.x[0], 0x1000);
    c.x[0] = 0;
}

void LoadModule(CPU& c) {
    log("nn::ro::LoadModule: NRO not supported (EOS SDK); reporting failure");
    c.x[0] = kRoNotLoaded;
}

}  // namespace

NX_HLE("_ZN2nn2ro10InitializeEv", ro_ok);
NX_HLE("_ZN2nn2ro8FinalizeEv", ro_ok);
NX_HLE("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", ro_ok);
NX_HLE("_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", ro_ok);
NX_HLE("_ZN2nn2ro13GetBufferSizeEPmPKv", GetBufferSize);
NX_HLE("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmib", LoadModule);
NX_HLE("_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", ro_ok);

}  // namespace nx
