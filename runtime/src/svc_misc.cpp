// Small services that only need to answer "yes, fine": parental controls, add-on content, logging, power.
#include <cstdlib>
#include "ipc.h"

namespace nx {

namespace {

std::shared_ptr<Service> make(const char* name, std::function<void(Service&)> setup = {}) {
    auto s = std::make_shared<Service>(name);
    if (setup) setup(*s);
    return s;
}

std::shared_ptr<Service> parental_control() {
    return make("IParentalControlService", [](Service& s) {
        s.cmds[1403] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // IsRestrictionEnabled
        s.cmds[1001] = [](IpcCtx&) { return 0u; };                          // CheckFreeCommunicationPermission
    });
}

void pctl(Service& s) {
    s.cmds[0] = [](IpcCtx& c) { c.push_object(parental_control()); return 0u; };  // CreateService
    s.cmds[1] = [](IpcCtx& c) { c.push_object(parental_control()); return 0u; };  // CreateServiceWithoutInitialize
}

ServiceRegistrar r_pctl("pctl", [] { return make("pctl", pctl); });
ServiceRegistrar r_pctl_s("pctl:s", [] { return make("pctl:s", pctl); });
ServiceRegistrar r_pctl_a("pctl:a", [] { return make("pctl:a", pctl); });
ServiceRegistrar r_pctl_r("pctl:r", [] { return make("pctl:r", pctl); });

ServiceRegistrar r_lm("lm", [] {
    return make("lm", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(make("ILogger")); return 0u; };  // OpenLogger
    });
});

ServiceRegistrar r_apm("apm", [] {
    return make("apm", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(make("ISession")); return 0u; };  // OpenSession
        s.cmds[1] = [](IpcCtx& c) {  // GetPerformanceMode: boost (docked), normal with NX_HANDHELD (svc_am.cpp)
            c.push<uint32_t>(std::getenv("NX_HANDHELD") ? 0 : 1);
            return 0u;
        };
    });
});

ServiceRegistrar r_aoc("aoc:u", [] {
    return make("aoc:u", [](Service& s) {
        s.cmds[2] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };  // CountAddOnContent (old)
        s.cmds[3] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };  // ListAddOnContent (old)
        s.cmds[7] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };
        s.cmds[8] = [](IpcCtx& c) {                                       // GetAddOnContentListChangedEvent
            auto e = std::make_shared<KEvent>();
            e->state = std::make_shared<EventState>();
            c.out_copy.push_back(handle_add(e));
            return 0u;
        };
    });
});

// friend:u (P4AU asks it for its play-history key at boot, and for the notification event): no friends, nothing to
// notify. IFriendService's unknown commands succeed with no output.
std::shared_ptr<Service> event_service(const char* name, uint32_t cmd) {
    return make(name, [cmd](Service& s) {
        s.cmds[cmd] = [](IpcCtx& c) {
            auto e = std::make_shared<KEvent>();
            e->state = std::make_shared<EventState>();
            c.out_copy.push_back(handle_add(e));
            return 0u;
        };
    });
}

void friend_service(Service& s) {
    s.cmds[0] = [](IpcCtx& c) {  // CreateFriendService
        c.push_object(make("IFriendService", [](Service& f) {
            f.cmds[10101] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };  // GetFriendList: count 0
            f.cmds[10102] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };  // UpdateFriendInfo
            f.cmds[10110] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };  // GetFriendProfileImage
            f.cmds[10600] = [](IpcCtx&) { return 0u; };                           // DeclareOpenOnlinePlaySession
            f.cmds[10601] = [](IpcCtx&) { return 0u; };                           // DeclareCloseOnlinePlaySession
            f.cmds[10610] = [](IpcCtx&) { return 0u; };                           // UpdateUserPresence
        }));
        return 0u;
    };
    s.cmds[1] = [](IpcCtx& c) { c.push_object(event_service("INotificationService", 0)); return 0u; };  // CreateNotificationService
    s.cmds[2] = [](IpcCtx& c) { c.push_object(make("IDaemonSuspendSessionService")); return 0u; };
}

ServiceRegistrar r_friend_u("friend:u", [] { return make("friend:u", friend_service); });
ServiceRegistrar r_friend_v("friend:v", [] { return make("friend:v", friend_service); });
ServiceRegistrar r_friend_m("friend:m", [] { return make("friend:m", friend_service); });
ServiceRegistrar r_friend_s("friend:s", [] { return make("friend:s", friend_service); });
ServiceRegistrar r_friend_a("friend:a", [] { return make("friend:a", friend_service); });

}  // namespace

}  // namespace nx
