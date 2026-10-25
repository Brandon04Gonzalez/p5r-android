// time:u / time:a / time:s: clocks, time zone, and the shared memory the SDK reads the clocks from.
#include <chrono>
#include <ctime>

#include "ipc.h"

namespace nx {

namespace {

// UUID of the steady clock source (any constant works; the SDK compares system clock time points against it).
constexpr uint64_t kSourceLo = 0x4E58525443524543ull, kSourceHi = 0x484144455354494Dull;

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t steady_seconds() { return int64_t(system_tick() / 19200000); }

// Portable UTC <-> calendar helpers (MSVC and bionic spell these differently).
void utc_tm(time_t t, std::tm& out) {
#if defined(_WIN32)
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
}

int64_t utc_from_tm(std::tm& tm) {
#if defined(_WIN32)
    return int64_t(_mkgmtime(&tm));
#else
    return int64_t(timegm(&tm));
#endif
}

// Seconds west of UTC, like _get_timezone (so local = utc - offset).
long utc_offset_west() {
    time_t now = std::time(nullptr);
    std::tm lt{}, gt{};
#if defined(_WIN32)
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    utc_tm(now, gt);
    lt.tm_isdst = 0;
    return long(utc_from_tm(gt) - utc_from_tm(lt));
}

std::shared_ptr<KSharedMemory> time_shared_memory() {
    static std::shared_ptr<KSharedMemory> shm = [] {
        auto s = make_shared_memory("time", 0x1000);
        uint64_t p = s->host;
        // LockFreeAtomicType<SteadyClockContext> at 0x0: u32 counter, pad, value[2] {u64 internal_offset; uuid}
        for (int k = 0; k < 2; k++) {
            st<uint64_t>(p + 0x8 + k * 0x18, 0);
            st<uint64_t>(p + 0x10 + k * 0x18, kSourceLo);
            st<uint64_t>(p + 0x18 + k * 0x18, kSourceHi);
        }
        // Local (0x38) and network (0x80) system clocks: {s64 offset; {s64 time_point; uuid}}
        int64_t offset = unix_now() - steady_seconds();
        for (uint64_t base : {0x38ull, 0x80ull})
            for (int k = 0; k < 2; k++) {
                uint64_t v = p + base + 8 + k * 0x20;
                st<int64_t>(v, offset);
                st<int64_t>(v + 8, 0);
                st<uint64_t>(v + 16, kSourceLo);
                st<uint64_t>(v + 24, kSourceHi);
            }
        return s;
    }();
    return shm;
}

std::shared_ptr<Service> system_clock() {
    auto s = std::make_shared<Service>("ISystemClock");
    s->cmds[0] = [](IpcCtx& c) { c.push<int64_t>(unix_now()); return 0u; };  // GetCurrentTime
    s->cmds[2] = [](IpcCtx& c) {                                              // GetSystemClockContext
        c.push<int64_t>(unix_now() - steady_seconds());
        c.push<int64_t>(0);
        c.push<uint64_t>(kSourceLo);
        c.push<uint64_t>(kSourceHi);
        return 0u;
    };
    return s;
}

std::shared_ptr<Service> steady_clock() {
    auto s = std::make_shared<Service>("ISteadyClock");
    s->cmds[0] = [](IpcCtx& c) {  // GetCurrentTimePoint
        c.push<int64_t>(steady_seconds());
        c.push<uint64_t>(kSourceLo);
        c.push<uint64_t>(kSourceHi);
        return 0u;
    };
    return s;
}

// Calendar conversion in UTC (the host's local offset is applied as the "rule").
void push_calendar(IpcCtx& c, int64_t t) {
    long offset = utc_offset_west();
    time_t lt = time_t(t - offset);
    std::tm tm{};
    utc_tm(lt, tm);
    // CalendarTime {s16 year; s8 month, day, hour, minute, second; pad}
    c.push<int16_t>(int16_t(tm.tm_year + 1900));
    c.push<int8_t>(int8_t(tm.tm_mon + 1));
    c.push<int8_t>(int8_t(tm.tm_mday));
    c.push<int8_t>(int8_t(tm.tm_hour));
    c.push<int8_t>(int8_t(tm.tm_min));
    c.push<int8_t>(int8_t(tm.tm_sec));
    c.push<int8_t>(0);
    // CalendarAdditionalInfo {u32 day_of_week; u32 day_of_year; char tz_name[8]; u32 is_dst; s32 utc_offset}
    c.push<uint32_t>(uint32_t(tm.tm_wday));
    c.push<uint32_t>(uint32_t(tm.tm_yday));
    char name[8] = "UTC";
    for (char ch : name) c.push<char>(ch);
    c.push<uint32_t>(0);
    c.push<int32_t>(int32_t(-offset));
}

std::shared_ptr<Service> time_zone() {
    auto s = std::make_shared<Service>("ITimeZoneService");
    s->cmds[0] = [](IpcCtx& c) {  // GetDeviceLocationName
        char name[0x24] = "UTC";
        for (char ch : name) c.push<char>(ch);
        return 0u;
    };
    s->cmds[100] = [](IpcCtx& c) { push_calendar(c, c.arg<int64_t>(0)); return 0u; };  // ToCalendarTime (rule in buffer)
    s->cmds[101] = [](IpcCtx& c) { push_calendar(c, c.arg<int64_t>(0)); return 0u; };  // ToCalendarTimeWithMyRule
    s->cmds[201] = [](IpcCtx& c) {  // ToPosixTimeWithMyRule(CalendarTime) -> count, times in buffer
        std::tm tm{};
        tm.tm_year = c.arg<int16_t>(0) - 1900;
        tm.tm_mon = c.arg<int8_t>(2) - 1;
        tm.tm_mday = c.arg<int8_t>(3);
        tm.tm_hour = c.arg<int8_t>(4);
        tm.tm_min = c.arg<int8_t>(5);
        tm.tm_sec = c.arg<int8_t>(6);
        long offset = utc_offset_west();
        int64_t t = utc_from_tm(tm) + offset;
        Buffer out = c.out_buf(0);
        if (out.addr && out.size >= 8) st<int64_t>(out.addr, t);
        c.push<uint32_t>(1);
        return 0u;
    };
    return s;
}

void time_static(Service& s) {
    s.cmds[0] = [](IpcCtx& c) { c.push_object(system_clock()); return 0u; };  // GetStandardUserSystemClock
    s.cmds[1] = [](IpcCtx& c) { c.push_object(system_clock()); return 0u; };  // GetStandardNetworkSystemClock
    s.cmds[2] = [](IpcCtx& c) { c.push_object(steady_clock()); return 0u; };  // GetStandardSteadyClock
    s.cmds[3] = [](IpcCtx& c) { c.push_object(time_zone()); return 0u; };     // GetTimeZoneService
    s.cmds[4] = [](IpcCtx& c) { c.push_object(system_clock()); return 0u; };  // GetStandardLocalSystemClock
    s.cmds[5] = [](IpcCtx& c) { c.push_object(system_clock()); return 0u; };  // GetEphemeralNetworkSystemClock
    s.cmds[20] = [](IpcCtx& c) {                                              // GetSharedMemoryNativeHandle
        c.out_copy.push_back(handle_add(time_shared_memory()));
        return 0u;
    };
    s.cmds[100] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // IsStandardUserSystemClockAutomaticCorrectionEnabled
    s.cmds[300] = [](IpcCtx& c) { c.push<int64_t>(unix_now()); return 0u; };  // CalculateMonotonicSystemClockBaseTimePoint
}

ServiceRegistrar r_time_u("time:u", [] { auto s = std::make_shared<Service>("time:u"); time_static(*s); return s; });
ServiceRegistrar r_time_a("time:a", [] { auto s = std::make_shared<Service>("time:a"); time_static(*s); return s; });
ServiceRegistrar r_time_s("time:s", [] { auto s = std::make_shared<Service>("time:s"); time_static(*s); return s; });

}  // namespace

}  // namespace nx
