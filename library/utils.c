// Copyright (c) 2022-2023.  NetFoundry Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <uv.h>
#include <tlsuv/tlsuv.h>
#include <ziti/ziti_model.h>
#include <ziti/ziti_log.h>
#include <stdarg.h>

#include "utils.h"

#if _WIN32
#include <time.h>
#endif


#if !defined(ZITI_VERSION)
#define ZITI_VERSION unknown
#endif

#if !defined(ZITI_BRANCH)
#define ZITI_BRANCH "<no-branch>"
#define ZITI_COMMIT "<sha>"
#endif

/*
 * https://sourceforge.net/p/predef/wiki/OperatingSystems/
 */
#if defined(WIN32)
#define ZITI_OS Windows
#elif defined(__ANDROID__)
#define ZITI_OS Android
#elif defined(__linux__)
#define ZITI_OS Linux
#elif defined(__APPLE__)
#define ZITI_OS MacOS
#else
#define ZITI_OS UKNOWN
#endif

/*
 * from https://sourceforge.net/p/predef/wiki/Architectures/
 */
#if defined(__aarch64__)
#define ZITI_ARCH arm64
#elif defined(__arm__)
#define ZITI_ARCH arm
#elif defined(__amd64__)
#define ZITI_ARCH amd64
#elif defined(__i386__)
#define ZITI_ARCH x86
#else
#define ZITI_ARCH UKNOWN
#endif

#define LEVEL_LBL(lvl) #lvl,
static const char *const level_labels[] = {
        DEBUG_LEVELS(LEVEL_LBL)
};

static const char *basename(const char *path);

const char *ziti_get_build_version(int verbose) {
    if (verbose) {
        return "\n\tVersion:\t" to_str(ZITI_VERSION)
               "\n\tBuild Date:\t" to_str(BUILD_DATE)
               "\n\tGit Branch:\t" to_str(ZITI_BRANCH)
               "\n\tGit SHA:\t" to_str(ZITI_COMMIT)
               "\n\tOS:\t" to_str(ZITI_OS)
               "\n\tArch:\t" to_str(ZITI_ARCH)
               "\n";

    }
#ifdef ZITI_BUILDNUM
    return to_str(ZITI_VERSION) "-" to_str(ZITI_BUILDNUM);
#else
    return to_str(ZITI_VERSION);
#endif
}

const char* ziti_git_branch() {
    return to_str(ZITI_BRANCH);
}

const char* ziti_git_commit() {
    return to_str(ZITI_COMMIT);
}

static const char *TLSUV_MODULE = "tlsuv";

static model_map log_levels;
static int ziti_log_lvl = ZITI_LOG_DEFAULT_LEVEL;
static FILE *ziti_debug_out;
static bool log_initialized = false;
static uv_pid_t log_pid = 0;

static const char *(*get_elapsed)();

static const char *get_elapsed_time();

static const char *get_utc_time();

static void flush_log(uv_prepare_t *p);

static void default_log_writer(int level, const char *loc, const char *msg, size_t msglen);

static uv_loop_t *ts_loop;
static uint64_t starttime;
static uint64_t last_update;
static char log_timestamp[32];

static uv_key_t logbufs;

static uv_prepare_t log_flusher;
static log_writer logger = NULL;

static void init_debug(uv_loop_t *loop);

static void init_uv_mbed_log();

void ziti_log_init(uv_loop_t *loop, int level, log_writer log_func) {
    init_uv_mbed_log();

    init_debug(loop);

    if (level == ZITI_LOG_DEFAULT_LEVEL) {
        level = ziti_log_lvl;
    }

    if (log_func == NULL) {
        // keep the logger if it was already set
        ziti_log_set_logger(logger ? logger : default_log_writer);
    } else {
        ziti_log_set_logger(log_func);
    }

    ziti_log_set_level(level, NULL);
}

void ziti_log_set_level(int level, const char *marker) {
    if (level > TRACE) {
        level = TRACE;
    } else if (level < 0) {
        level = ZITI_LOG_DEFAULT_LEVEL;
    }

    if (level == ZITI_LOG_DEFAULT_LEVEL) {
        if (marker) {
            model_map_remove(&log_levels, marker);
        }
    } else {
        if (marker) {
            model_map_set(&log_levels, marker, (void *) (uintptr_t) level);
            if (strcmp(marker, TLSUV_MODULE) == 0) {
                tlsuv_set_debug(level, tlsuv_logger);
            }
        } else {
            ziti_log_lvl = level;
        }
    }

    if (logger) {
        int l = level == ZITI_LOG_DEFAULT_LEVEL ? ziti_log_lvl : level;
        const char *lbl = level_labels[l];
        ZITI_LOG(INFO, "set log level: %s=%d/%s", marker ? marker : "root", l, lbl);
    }
}

int ziti_log_level(const char *module, const char *file) {
    int level;

    file = basename(file);
    if (file) {
        level = (int) (uintptr_t) model_map_get(&log_levels, file);
        if (level) { return level; }
    }

    if (module) {
        level = (int) (uintptr_t) model_map_get(&log_levels, module);
        if (level) { return level; }
    }

    return ziti_log_lvl;
}

const char* ziti_log_level_label() {
    int num_levels = sizeof(level_labels) / sizeof(const char *);
    if (ziti_log_lvl >= 0 && ziti_log_lvl < num_levels) {
        return level_labels[ziti_log_lvl];
    } else {
        return NULL;
    }
}

void ziti_log_set_level_by_label(const char* log_level) {
    int lvl = ZITI_LOG_DEFAULT_LEVEL;
    int num_levels = sizeof(level_labels) / sizeof(const char *);
    for (int i = 0;i < num_levels; i++) {
        if (strcasecmp(log_level, level_labels[i]) == 0) {
            lvl = i;
        }
    }
    if (lvl != ZITI_LOG_DEFAULT_LEVEL) {
        ziti_log_set_level(lvl, NULL);
    }
}

void ziti_log_set_logger(log_writer log) {
    logger = log;
}

static void init_uv_mbed_log() {
    char *lvl;
    if ((lvl = getenv("TLSUV_DEBUG")) != NULL) {
        int l = (int) strtol(lvl, NULL, 10);
        tlsuv_set_debug(l, tlsuv_logger);
    }
}

static void child_init() {
    log_initialized = false;
    log_pid = uv_os_getpid();
}

static void init_debug(uv_loop_t *loop) {
    if (log_initialized) {
        return;
    }
#if defined(PTHREAD_ONCE_INIT)
    pthread_atfork(NULL, NULL, child_init);
#endif
    uv_key_create(&logbufs);
    log_pid = uv_os_getpid();
    get_elapsed = get_elapsed_time;
    char *ts_format = getenv("ZITI_TIME_FORMAT");
    if (ts_format && strcasecmp("utc", ts_format) == 0) {
        get_elapsed = get_utc_time;
    }
    ts_loop = loop;
    log_initialized = true;

    if (ziti_log_lvl == ZITI_LOG_DEFAULT_LEVEL) {
        ziti_log_lvl = ERROR;
    }

    model_list levels = {0};
    str_split(getenv("ZITI_LOG"), ";", &levels);

    const char *lvl;
    int l;
    MODEL_LIST_FOREACH(lvl, levels) {
        char *eq = strchr(lvl, '=');
        if (eq) {
            l = (int) strtol(eq + 1, NULL, 10);
            model_map_set_key(&log_levels, lvl, eq - lvl, (void *) (intptr_t) l);
        }
        else {
            l = (int) strtol(lvl, NULL, 10);
            ziti_log_lvl = l;
        }
    }
    model_list_clear(&levels, free);

    int tlsuv_level = (int) (intptr_t) model_map_get(&log_levels, TLSUV_MODULE);
    if (tlsuv_level > 0) {
        tlsuv_set_debug(tlsuv_level, tlsuv_logger);
    }

    ziti_debug_out = stderr;

    starttime = uv_now(loop);

    uv_prepare_init(loop, &log_flusher);
    uv_unref((uv_handle_t *) &log_flusher);
    uv_prepare_start(&log_flusher, flush_log);
}

#if _WIN32 && defined(_MSC_VER)
static const char DIR_SEP = '\\';
#else
static const char DIR_SEP = '/';
#endif

static const char *basename(const char *path) {
    if (path == NULL) { return NULL; }

    char *last_slash = strrchr(path, DIR_SEP);
    if (last_slash) { return last_slash + 1; }
    return path;
}

void ziti_logger(int level, const char *module, const char *file, unsigned int line, const char *func, FORMAT_STRING(const char *fmt), ...) {
    static size_t loglinelen = 1024;

    log_writer logfunc = logger;
    if (logfunc == NULL) { return; }

    char *logbuf = (char *) uv_key_get(&logbufs);
    if (!logbuf) {
        logbuf = malloc(loglinelen);
        uv_key_set(&logbufs, logbuf);
    }

    char location[128];
    char *last_slash = strrchr(file, DIR_SEP);

    int modlen = 16;
    if (module == NULL) {
        if (last_slash == NULL) {
            modlen = 0;
        }
        else {
            char *p = last_slash;
            while (p > file) {
                p--;
                if (*p == DIR_SEP) {
                    p++;
                    break;
                }
            }
            module = p;
            modlen = (int) (last_slash - p);
        }
    }

    if (last_slash) {
        file = last_slash + 1;
    }
    if (func && func[0]) {
        snprintf(location, sizeof(location), "%.*s:%s:%u %s()", modlen, module, file, line, func);
    }
    else {
        snprintf(location, sizeof(location), "%.*s:%s:%u", modlen, module, file, line);
    }

    va_list argp;
    va_start(argp, fmt);
    int len = vsnprintf(logbuf, loglinelen, fmt, argp);
    va_end(argp);

    if (len > loglinelen) {
        len = (int) loglinelen;
    }

    logfunc(level, location, logbuf, len);
}

static void default_log_writer(int level, const char *loc, const char *msg, size_t msglen) {
    const char *elapsed = get_elapsed();
    fprintf(ziti_debug_out, "(%u)[%s] %7s %s %.*s\n", log_pid, elapsed, level_labels[level], loc, (unsigned int) msglen, msg);
}

void tlsuv_logger(int level, const char *file, unsigned int line, const char *msg) {
    ziti_logger(level, TLSUV_MODULE, file, line, NULL, "%s", msg);
}

static void flush_log(uv_prepare_t *p) {
    fflush(ziti_debug_out);
}

static const char *get_elapsed_time() {
    uint64_t now = uv_now(ts_loop);
    if (now > last_update) {
        last_update = now;
        unsigned long long elapsed = now - starttime;
        snprintf(log_timestamp, sizeof(log_timestamp), "%9llu.%03llu", (elapsed / 1000), (elapsed % 1000));
    }
    return log_timestamp;
}

static const char *get_utc_time() {
    uint64_t now = uv_now(ts_loop);
    if (now > last_update) {
        last_update = now;

        uv_timeval64_t ts;
        uv_gettimeofday(&ts);
        struct tm *tm = gmtime(&ts.tv_sec);

        snprintf(log_timestamp, sizeof(log_timestamp), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                 1900 + tm->tm_year, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec, ts.tv_usec / 1000
        );
    }
    return log_timestamp;
}

int lt_zero(int v) { return v < 0; }

void hexDump (char *desc, void *addr, int len) {
    ZITI_LOG(DEBUG, " ");
    int i;
    unsigned char buffLine[17];
    unsigned char *pc = (unsigned char*)addr;
    if (desc != NULL){
       printf ("%s:\n", desc);
    }
    for (i = 0; i < len; i++) {
        if ((i % 16) == 0) {
            if (i != 0) {
                printf ("  %s\n", buffLine);
            }
            printf ("  %07x ", i);
        }
        printf ("%02x", pc[i]);
        if ((i % 2) == 1) {
            printf (" "); 
        }
        if ((pc[i] < 0x20) || (pc[i] > 0x7e)) {
            buffLine[i % 16] = '.';
        }
        else{
           buffLine[i % 16] = pc[i];
        }    

        buffLine[(i % 16) + 1] = '\0'; //Clears the next array buffLine
    }
    while ((i % 16) != 0) {
        printf ("   ");
        i++;
    }
    printf ("  %s\n", buffLine);
    fflush(stdout); 
    ZITI_LOG(DEBUG, " ");
}

void ziti_fmt_time(char* time_str, size_t time_str_sz, uv_timeval64_t* tv) {
    if (tv == NULL) {
        strncpy(time_str, "null tv", time_str_sz);
    } else {
        struct tm* start_tm = gmtime(&tv->tv_sec);
        strftime(time_str, time_str_sz, "%Y-%m-%dT%H:%M:%S", start_tm);
    }
}

void hexify(const uint8_t *bin, size_t bin_len, char sep, char **buf) {
    static char hex[] = "0123456789abcdef";
    size_t out_size = sep ? bin_len * 3 : bin_len * 2 + 1;
    char *out = malloc(out_size);
    char *p = out;
    for (int i = 0; i < bin_len; i++) {
        unsigned char b = bin[i];
        if (sep && i > 0) { *p++ = sep; }
        *p++ = hex[b >> 4];
        *p++ = hex[b & 0xf];
    }
    *p = 0;
    *buf = out;
}


size_t str_split(const char *str, const char *delim, model_list *result) {
    size_t count = 0;
    if (str) {
        const char *sep = str;
        do {
            const char *s = sep;
            char *val;
            if ((sep = strpbrk(s, delim)) != NULL) {
                size_t tok_len = sep++ - s;
                val = calloc(1, tok_len + 1);
                strncpy(val, s, tok_len);
            } else {
                val = strdup(s);
            }
            model_list_append(result, val);
            count++;
        } while (sep);
    }

    return count;
}