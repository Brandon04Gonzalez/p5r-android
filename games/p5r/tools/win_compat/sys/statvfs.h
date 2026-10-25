// POSIX shim (see ../unistd.h): free space of the output folder's drive.
#pragma once
#include <stdint.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
struct statvfs {
    uint64_t f_bavail = 0, f_frsize = 1, f_bsize = 1;
};
static inline int statvfs(const char* path, struct statvfs* vs) {
    ULARGE_INTEGER avail{};
    if (!GetDiskFreeSpaceExA(path, &avail, nullptr, nullptr)) return -1;
    vs->f_bavail = avail.QuadPart;
    vs->f_frsize = vs->f_bsize = 1;
    return 0;
}
