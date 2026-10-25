// Networking at the API level: the console is offline. nifm reports no network, sockets fail, name resolution
// and SSL fail; the game's online features (EOS, news) switch themselves off.
#include <nx/runtime.h>

namespace nx {

namespace {

constexpr uint32_t kNifmNoNetwork = (3400u << 9) | 110u;  // nifm error "network not available"
constexpr uint32_t kGenericFail = (1u << 9) | 110u;

void ok(CPU& c) { c.x[0] = 0; }
void no(CPU& c) { c.x[0] = 0; }        // bool false
void fail(CPU& c) { c.x[0] = kNifmNoNetwork; }
void minus_one(CPU& c) { c.x[0] = uint64_t(-1); }
void result_fail(CPU& c) { c.x[0] = kGenericFail; }
void enetdown(CPU& c) { c.x[0] = 50; }  // ENETDOWN
void eai_fail(CPU& c) { c.x[0] = 4; }   // EAI_FAIL
void htonl_(CPU& c) { c.x[0] = bswap(uint32_t(c.x[0])); }
void htons_(CPU& c) { c.x[0] = bswap(uint16_t(c.x[0])); }
void show_error(CPU& c) {
    log("nn::err::ShowError(%#x)", uint32_t(c.x[0]));
}

}  // namespace

NX_HLE("_ZN2nn4nifm10InitializeEv", ok);
NX_HLE("_ZN2nn4nifm20SubmitNetworkRequestEv", ok);
NX_HLE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", ok);
NX_HLE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", no);
NX_HLE("_ZN2nn4nifm20CancelNetworkRequestEv", ok);
NX_HLE("_ZN2nn4nifm18IsNetworkAvailableEv", no);
NX_HLE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", fail);
NX_HLE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", fail);
NX_HLE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", ok);
NX_HLE("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", minus_one);
NX_HLE("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", minus_one);
NX_HLE("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", minus_one);
NX_HLE("_ZN2nn6socket6ListenEii", minus_one);
NX_HLE("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", minus_one);
NX_HLE("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", minus_one);
NX_HLE("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", minus_one);
NX_HLE("_ZN2nn6socket5CloseEi", minus_one);
NX_HLE("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", minus_one);
NX_HLE("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", minus_one);
NX_HLE("_ZN2nn6socket8ShutdownEiNS0_14ShutdownMethodE", minus_one);
NX_HLE("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", minus_one);
NX_HLE("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", minus_one);
NX_HLE("_ZN2nn6socket12GetLastErrorEv", enetdown);
NX_HLE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", eai_fail);
NX_HLE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", ok);
NX_HLE("_ZN2nn6socket9InetHtonlEj", htonl_);
NX_HLE("_ZN2nn6socket9InetHtonsEt", htons_);
NX_HLE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", result_fail);
NX_HLE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", result_fail);
NX_HLE("_ZN2nn3err9ShowErrorENS_6ResultE", show_error);

}  // namespace nx
