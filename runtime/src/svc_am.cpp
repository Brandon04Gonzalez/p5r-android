// am services (appletOE): application proxy, focus/operation-mode state, launch parameters.
#include <cstdio>
#include <cstdlib>
#include <deque>

#include "ipc.h"
#include "language.h"
#include "users.h"

namespace nx {

namespace {

constexpr uint32_t kAmNoMessage = 0x680;  // am error 3
constexpr uint32_t kMsgFocusStateChanged = 15;
constexpr uint32_t kMsgOperationModeChanged = 30;
constexpr uint32_t kMsgPerformanceModeChanged = 31;

struct AppletState {
    std::deque<uint32_t> messages{kMsgFocusStateChanged, kMsgOperationModeChanged, kMsgPerformanceModeChanged};
    std::shared_ptr<EventState> message_event = std::make_shared<EventState>();
};

AppletState& applet() {
    static AppletState s;
    return s;
}

uint32_t event_handle(std::shared_ptr<EventState> st) {
    auto e = std::make_shared<KEvent>();
    e->state = std::move(st);
    return handle_add(e);
}

std::shared_ptr<Service> svc(const char* name, std::function<void(Service&)> setup) {
    auto s = std::make_shared<Service>(name);
    setup(*s);
    return s;
}

std::shared_ptr<Service> common_state_getter() {
    return svc("ICommonStateGetter", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) {  // GetEventHandle
            applet().message_event->signaled = !applet().messages.empty();
            c.out_copy.push_back(event_handle(applet().message_event));
            return 0u;
        };
        s.cmds[1] = [](IpcCtx& c) {  // ReceiveMessage
            auto& a = applet();
            if (a.messages.empty()) {
                a.message_event->signaled = false;
                return kAmNoMessage;
            }
            c.push<uint32_t>(a.messages.front());
            a.messages.pop_front();
            if (a.messages.empty()) a.message_event->signaled = false;
            return 0u;
        };
        // NX_HANDHELD=1: handheld mode, so the game renders for 1280x720 (a lighter GPU load) instead of docked 1080p.
        static const bool handheld = std::getenv("NX_HANDHELD") != nullptr;
        s.cmds[5] = [](IpcCtx& c) { c.push<uint8_t>(handheld ? 0 : 1); return 0u; };   // GetOperationMode
        s.cmds[6] = [](IpcCtx& c) { c.push<uint32_t>(handheld ? 0 : 1); return 0u; };  // GetPerformanceMode
        s.cmds[9] = [](IpcCtx& c) { c.push<uint8_t>(1); return 0u; };   // GetCurrentFocusState: in focus
        // SetCpuBoostMode(CpuBoostMode): Wonder asks for the boost around every load. A PC has no clock to raise, so
        // the request is taken and dropped (no output). Same answer as the generic stub, without its log line.
        s.cmds[66] = [](IpcCtx&) { return 0u; };
        // NX_RENDER_RES=<w>x<h>: the size the game renders at, while it stays docked (a resolution scale: the game sizes
        // its window textures and targets from this answer; the present step stretches the picture to the window).
        static const std::pair<uint32_t, uint32_t> res = [] {
            uint32_t w = handheld ? 1280 : 1920, h = handheld ? 720 : 1080, rw = 0, rh = 0;
            if (const char* e = std::getenv("NX_RENDER_RES"); e && std::sscanf(e, "%ux%u", &rw, &rh) == 2 && rw >= 320 && rh >= 180)
                w = rw, h = rh;
            return std::pair<uint32_t, uint32_t>{w, h};
        }();
        s.cmds[60] = [](IpcCtx& c) {  // GetDefaultDisplayResolution
            c.push<uint32_t>(res.first);
            c.push<uint32_t>(res.second);
            return 0u;
        };
        s.cmds[61] = [](IpcCtx& c) {  // GetDefaultDisplayResolutionChangeEvent
            c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
            return 0u;
        };
    });
}

std::shared_ptr<Service> self_controller() {
    return svc("ISelfController", [](Service& s) {
        s.cmds[40] = [](IpcCtx& c) { c.push<uint64_t>(1); return 0u; };  // CreateManagedDisplayLayer
        s.cmds[0] = [](IpcCtx&) { return 0u; };                             // Exit
        s.cmds[9] = [](IpcCtx& c) {  // GetLibraryAppletLaunchableEvent: signaled, a library applet can be launched
            auto e = std::make_shared<EventState>();
            e->signaled = true;
            c.out_copy.push_back(event_handle(e));
            return 0u;
        };
        s.cmds[62] = [](IpcCtx&) { return 0u; };                            // SetIdleTimeDetectionExtension
        for (uint32_t cmd : {19u, 50u, 71u, 110u})                           // AccumulatedSuspendedTickChangedEvent etc.
            s.cmds[cmd] = [](IpcCtx& c) {
                c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
                return 0u;
            };
    });
}

std::shared_ptr<Service> window_controller() {
    return svc("IWindowController", [](Service& s) {
        s.cmds[1] = [](IpcCtx& c) { c.push<uint64_t>(0x51); return 0u; };  // GetAppletResourceUserId
    });
}

std::string hex_dump(const std::vector<uint8_t>& d, size_t max = 0x40) {
    std::string out;
    char b[4];
    for (size_t i = 0; i < d.size() && i < max; i++) {
        std::snprintf(b, sizeof b, "%02x", d[i]);
        out += b;
    }
    return out;
}

// ILibraryAppletAccessor of one library applet. Only the account applets (playerSelect, id 0x10) are emulated, with no
// UI: a "create user" request makes a new local user, a "select user" request answers with the last opened one.
// Switchbrew: Applet Manager services (ILibraryAppletAccessor) and the playerSelect applet (PselUiMode).
std::shared_ptr<Service> library_applet(uint32_t applet_id, uint32_t mode) {
    struct State {
        uint32_t id;
        std::shared_ptr<EventState> done = std::make_shared<EventState>();
        std::shared_ptr<EventState> out_ready = std::make_shared<EventState>();
        std::shared_ptr<EventState> inter_ready = std::make_shared<EventState>();
        std::vector<std::vector<uint8_t>> in;
        std::deque<std::shared_ptr<Service>> out;
        bool started = false;
        uint32_t result = 0;
    };
    auto st = std::make_shared<State>();
    st->id = applet_id;
    log("am: CreateLibraryApplet(id 0x%x, mode %u)", applet_id, mode);
    return svc("ILibraryAppletAccessor", [st](Service& s) {
        s.cmds[0] = [st](IpcCtx& c) {  // GetAppletStateChangedEvent
            c.out_copy.push_back(event_handle(st->done));
            return 0u;
        };
        s.cmds[1] = [st](IpcCtx& c) { c.push<uint8_t>(st->started); return 0u; };  // IsCompleted
        s.cmds[10] = [st](IpcCtx&) {                                                // Start
            for (size_t i = 0; i < st->in.size(); i++)
                log("am: applet 0x%x input storage #%zu (%zu bytes): %s", st->id, i, st->in[i].size(), hex_dump(st->in[i]).c_str());
            st->started = true;
            std::vector<uint8_t> out;
            if (st->id == 0x10 && !st->in.empty()) {  // playerSelect: the last input is PselUiSettings (u32 UiMode first)
                const auto& in = st->in.back();
                uint32_t ui = in.size() >= 4 ? uint32_t(in[0] | in[1] << 8 | in[2] << 16 | uint32_t(in[3]) << 24) : 0;
                UserInfo u;
                if (ui == 1 || ui == 5) {  // UserCreator / UserCreatorForStarter
                    u = user_create();
                    log("am: playerSelect UserCreator -> %s", u.valid() ? u.nickname.c_str() : "no room (8 users)");
                } else if (ui == 0) {  // UserSelector
                    u = user_last_opened();
                    log("am: playerSelect UserSelector -> %s", u.nickname.c_str());
                } else {
                    log("am: playerSelect UiMode %u not handled", ui);
                }
                out.assign(0x18, 0);  // PselUiReturnArg {u64 result; Uid}
                if (u.valid()) {
                    std::memcpy(out.data() + 8, &u.lo, 8);
                    std::memcpy(out.data() + 16, &u.hi, 8);
                    user_opened(u.lo, u.hi);
                } else {
                    uint64_t cancelled = 0x2;  // nothing chosen
                    std::memcpy(out.data(), &cancelled, 8);
                }
            } else if (st->id == 0x11) { // swkbd (SoftwareKeyboard)
                // Swkbd output storage: u32 SwkbdResult (0 = Ok) + string buffer (0x7D4 bytes)
                out.assign(0x7D8, 0);
                log("am: swkbd applet (0x11) invoked; returning default OK output storage");
            } else {
                log("am: library applet 0x%x is not emulated; answering with zeroed output storage", st->id);
                // Never return a 0-byte storage: callers read a return header from PopOutData
                out.assign(0x1000, 0);
            }
            st->out.push_back(make_storage(std::move(out)));
            st->out_ready->signaled = true;
            st->done->signaled = true;
            return 0u;
        };
        s.cmds[20] = [](IpcCtx&) { return 0u; };  // RequestExit
        s.cmds[25] = [](IpcCtx&) { return 0u; };  // Terminate
        s.cmds[30] = [st](IpcCtx&) { return st->result; };  // GetResult
        s.cmds[50] = [](IpcCtx&) { return 0u; };  // SetOutOfFocusApplicationSuspendingEnabled
        s.cmds[100] = [st](IpcCtx& c) {           // PushInData(IStorage)
            if (!c.in_services.empty())
                if (auto b = storage_blob(c.in_services[0])) st->in.push_back(*b);
            return 0u;
        };
        s.cmds[101] = [st](IpcCtx& c) {  // PopOutData -> IStorage
            if (st->out.empty()) return 0x2A80u;
            c.push_object(st->out.front());
            st->out.pop_front();
            if (st->out.empty()) st->out_ready->signaled = false;
            return 0u;
        };
        s.cmds[102] = [](IpcCtx&) { return 0u; };  // PushExtraStorage
        s.cmds[103] = [](IpcCtx&) { return 0u; };  // PushInteractiveInData
        s.cmds[104] = [](IpcCtx&) { return 0x2A80u; };  // PopInteractiveOutData: none
        s.cmds[105] = [st](IpcCtx& c) { c.out_copy.push_back(event_handle(st->out_ready)); return 0u; };  // GetPopOutDataEvent
        s.cmds[106] = [st](IpcCtx& c) { c.out_copy.push_back(event_handle(st->inter_ready)); return 0u; };
        s.cmds[110] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // NeedsToExitProcess
        s.cmds[150] = [](IpcCtx&) { return 0u; };                         // RequestForAppletToGetForeground
    });
}

std::shared_ptr<Service> library_applet_creator() {
    return svc("ILibraryAppletCreator", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(library_applet(c.arg<uint32_t>(0), c.arg<uint32_t>(4))); return 0u; };
        s.cmds[1] = [](IpcCtx&) { return 0u; };                              // TerminateAllLibraryApplets
        s.cmds[2] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };       // AreAnyLibraryAppletsLeft
        s.cmds[10] = [](IpcCtx& c) {                                         // CreateStorage(s64 size)
            int64_t n = c.arg<int64_t>(0);
            c.push_object(make_storage(std::vector<uint8_t>(n > 0 && n < (1 << 24) ? size_t(n) : 0, 0)));
            return 0u;
        };
        s.cmds[11] = [](IpcCtx& c) { // CreateTransferMemoryStorage(bool writable, s64 size, handle)
            int64_t n = c.arg<int64_t>(8);
            if (n <= 0 || n >= (1 << 24)) n = c.arg<int64_t>(0);
            size_t cap = (n > 0 && n < (1 << 24)) ? size_t(n) : 0x10000;
            log("am: CreateTransferMemoryStorage(size 0x%zx)", cap);
            c.push_object(make_storage(std::vector<uint8_t>(cap, 0)));
            return 0u;
        };
    });
}

std::shared_ptr<Service> application_functions() {
    return svc("IApplicationFunctions", [](Service& s) {
        s.cmds[1] = [](IpcCtx& c) {  // PopLaunchParameter(kind): the preselected user, once
            static bool popped = false;
            if (c.arg<uint32_t>(0) != 2 || popped) return 0x2A80u;
            popped = true;
            c.push_object(preselected_user_storage());
            return 0u;
        };
        s.cmds[20] = [](IpcCtx& c) { c.push<uint64_t>(0); return 0u; };  // EnsureSaveData
        s.cmds[21] = [](IpcCtx& c) {                                      // GetDesiredLanguage
            uint64_t code = desired_language();
            char tag[9] = {0};
            std::memcpy(tag, &code, 8);
            log("am: desired language = %s", tag);
            c.push<uint64_t>(code);
            return 0u;
        };
        s.cmds[40] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // NotifyRunning
        s.cmds[50] = [](IpcCtx& c) {                                     // GetPseudoDeviceId
            c.push<uint64_t>(0x1234);
            c.push<uint64_t>(0x5678);
            return 0u;
        };
        s.cmds[130] = [](IpcCtx& c) {  // GetGpuErrorDetectedSystemEvent
            c.out_copy.push_back(event_handle(std::make_shared<EventState>()));
            return 0u;
        };
        s.cmds[210] = [](IpcCtx&) { return 0u; };  // UnsetGameCardEject... (stub)
    });
}

std::shared_ptr<Service> application_proxy() {
    return svc("IApplicationProxy", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(common_state_getter()); return 0u; };
        s.cmds[1] = [](IpcCtx& c) { c.push_object(self_controller()); return 0u; };
        s.cmds[2] = [](IpcCtx& c) { c.push_object(window_controller()); return 0u; };
        s.cmds[3] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IAudioController")); return 0u; };
        s.cmds[4] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IDisplayController")); return 0u; };
        s.cmds[10] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IProcessWindingController")); return 0u; };
        s.cmds[11] = [](IpcCtx& c) { c.push_object(library_applet_creator()); return 0u; };
        s.cmds[20] = [](IpcCtx& c) { c.push_object(application_functions()); return 0u; };
        s.cmds[1000] = [](IpcCtx& c) { c.push_object(std::make_shared<Service>("IDebugFunctions")); return 0u; };
    });
}

ServiceRegistrar reg_applet_oe("appletOE", [] {
    return svc("appletOE", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(application_proxy()); return 0u; };  // OpenApplicationProxy
    });
});

}  // namespace

}  // namespace nx
