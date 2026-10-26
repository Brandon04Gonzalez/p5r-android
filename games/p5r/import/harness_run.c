// Runs the shader translator library (libspirvharness.so, games/p5r/tools/build_harness_android.sh) from adb shell,
// to test it on a phone: harness_run <lib path> --batch <extracted dir> --out <dir> [--jobs N] [--limit N] --no-val
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

typedef int (*MainFn)(int, const char**);
typedef void (*ProgressFn)(int*, int*);

static ProgressFn g_progress;
static volatile int g_finished;

static void* watch(void* arg) {
    (void)arg;
    while (!g_finished) {
        sleep(10);
        int done = 0, total = 0;
        g_progress(&done, &total);
        fprintf(stderr, "progress %d/%d\n", done, total);
    }
    return NULL;
}

int main(int argc, const char** argv) {
    if (argc < 3) return fprintf(stderr, "usage: harness_run <lib> <harness args...>\n"), 2;
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) return fprintf(stderr, "dlopen: %s\n", dlerror()), 1;
    MainFn run = (MainFn)dlsym(lib, "spirv_harness_main");
    g_progress = (ProgressFn)dlsym(lib, "spirv_harness_progress");
    if (!run || !g_progress) return fprintf(stderr, "missing exports\n"), 1;
    pthread_t t;
    pthread_create(&t, NULL, watch, NULL);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int rc = run(argc - 2, argv + 2);
    clock_gettime(CLOCK_MONOTONIC, &b);
    g_finished = 1;
    fprintf(stderr, "spirv_harness_main -> %d in %.1f s\n", rc, (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9);
    return rc;
}
