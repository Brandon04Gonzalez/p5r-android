// Touch injector for scenario replays (replay.py): reads lines from stdin and writes input events to an evdev node.
//   B              a gesture begins: the time base restarts now
//   E d type code value   wait until d microseconds after the previous event (or the B), then write the event
//   S              answers "ok" once everything before it is written
// Build: aarch64-linux-android30-clang -O2 -static inject.c -o inject. Usage: inject [/dev/input/event2]
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void add_us(struct timespec* t, long long us) {
    t->tv_sec += us / 1000000;
    t->tv_nsec += (us % 1000000) * 1000;
    while (t->tv_nsec >= 1000000000) t->tv_nsec -= 1000000000, t->tv_sec++;
}

int main(int argc, char** argv) {
    int fd = open(argc > 1 ? argv[1] : "/dev/input/event2", O_WRONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    char line[256];
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    printf("ready\n");
    while (fgets(line, sizeof line, stdin)) {
        if (line[0] == 'B') {
            clock_gettime(CLOCK_MONOTONIC, &next);
        } else if (line[0] == 'E') {
            long long d;
            unsigned type, code;
            int value;
            if (sscanf(line + 1, "%lld %u %u %d", &d, &type, &code, &value) != 4) continue;
            add_us(&next, d);
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
            struct input_event ev;
            memset(&ev, 0, sizeof ev);
            ev.type = (unsigned short)type, ev.code = (unsigned short)code, ev.value = value;
            if (write(fd, &ev, sizeof ev) != sizeof ev) perror("write");
        } else if (line[0] == 'S') {
            printf("ok\n");
        }
    }
    return 0;
}
