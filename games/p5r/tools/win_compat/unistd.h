// POSIX shim for building android/wonder/importer (nximport) with MSVC on Windows. Only what the importer uses.
#pragma once
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

typedef intptr_t ssize_t;
#define off_t int64_t
// Every open in the importer passes O_CLOEXEC: use it to force binary mode (text mode would rewrite bytes).
#define O_CLOEXEC (_O_BINARY | _O_NOINHERIT)
// 64-bit sizes: the NSPs are larger than 4 GB.
#define stat _stat64
#define fstat _fstat64
#define mkdir(p, m) _mkdir(p)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#define fdatasync(fd) _commit(fd)

static inline ssize_t pread(int fd, void* buf, size_t n, int64_t off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (n > (1u << 30)) n = 1u << 30;
    OVERLAPPED ov{};
    ov.Offset = (DWORD)(uint64_t(off) & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(uint64_t(off) >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)n, &got, &ov)) return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    return (ssize_t)got;
}
