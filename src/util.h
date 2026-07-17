#ifndef NCF_UTIL_H
#define NCF_UTIL_H
#ifdef __cplusplus
extern "C" {
#endif

#include <ctype.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <NickelHook.h>

#include "config.h"

// The mod version, baked in by NickelHook.mk (git describe). Logged on every line and in the
// startup block, so a user-attached log always says exactly which build produced it.
#ifndef NH_VERSION
#define NH_VERSION "dev"
#endif

// Cap the on-device log so it can't grow without bound across many boots. On the first write of
// a boot, if the log is larger than this it's rotated to a single ".old" generation. A healthy
// device is quiet (only the startup block), so this is reached only by a long-lived or verbose one.
#ifndef NCF_LOG_MAX_BYTES
#define NCF_LOG_MAX_BYTES (256 * 1024)
#endif

__attribute__((unused)) static inline char *strtrim(char *s) {
    if (!s)
        return NULL;

    char *a = s;
    char *b = s + strlen(s);
    for (; a < b && isspace((unsigned char)(*a)); a++);
    for (; b > a && isspace((unsigned char)(*(b - 1))); b--);
    *b = '\0';
    return a;
}

__attribute__((unused)) static inline void ncf_log_file_line(const char *file, int line, const char *fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    msg[sizeof(msg) - 1] = '\0';

    nh_log("%s (%s:%d)", msg, file, line);

    // The file sink is opt-in via ncf_log (default on); syslog above always gets the line.
    if (!ncf_should_log_file())
        return;

    mkdir(NCF_CONFIG_DIR, 0755);

    // Rotate once per process, on the first file write of the boot. A benign race if two threads
    // hit this first (at most a redundant rename); the flag keeps it to one check per process.
    static bool ncf_log_rotate_checked = false;
    if (!ncf_log_rotate_checked) {
        ncf_log_rotate_checked = true;
        struct stat st;
        if (stat(NCF_CONFIG_DIR "/nickel-cover-fix.log", &st) == 0 && st.st_size > NCF_LOG_MAX_BYTES)
            rename(NCF_CONFIG_DIR "/nickel-cover-fix.log", NCF_CONFIG_DIR "/nickel-cover-fix.log.old");
    }

    FILE *f = fopen(NCF_CONFIG_DIR "/nickel-cover-fix.log", "a");
    if (!f)
        return;

    // localtime_r, not localtime: logging happens on whatever thread hit a problem (hooks run on
    // Nickel's render/UI threads), and localtime's shared static buffer is a data race between
    // concurrent callers.
    time_t now = time(NULL);
    struct tm tmbuf;
    struct tm *tm = localtime_r(&now, &tmbuf);
    if (tm) {
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d ",
            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec);
    }

    fprintf(f, "NickelCoverFix " NH_VERSION ": %s (%s:%d)\n", msg, file, line);
    fclose(f);
}

#define NCF_LOG(fmt, ...) ncf_log_file_line(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif
