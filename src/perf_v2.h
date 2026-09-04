#ifndef C_PERF_V2_H
#define C_PERF_V2_H

#include <sys/ioctl.h>
#ifdef __linux__
#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif
#endif
#ifdef __APPLE__
#include <sys/clonefile.h>
#endif

/* Internal helpers used by src/cli.c. Nothing here is part of cbuild.h. */
static bool compiler_read_depfile(const char *path, StrVec *deps);
static bool compiler_copy_atomic(const char *from, const char *to);

static int compiler_perf_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    return (int)n;
}

static int compiler_default_jobs(void) {
    int n = compiler_perf_cpu_count() / 2;
    return n > 0 ? n : 1;
}

static double compiler_system_load(void) {
#ifdef __linux__
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return 0.0;
    double load = 0.0;
    bool ok = fscanf(f, "%lf", &load) == 1;
    fclose(f);
    return ok ? load : 0.0;
#elif defined(__APPLE__)
    double load = 0.0;
    return getloadavg(&load, 1) == 1 ? load : 0.0;
#else
    return 0.0;
#endif
}

static int compiler_adaptive_jobs(int requested) {
    if (!compiler_perf.adaptive_jobs || requested <= 1) return requested > 0 ? requested : 1;
    double load = compiler_system_load();
    if (load <= 0.0) return requested;
    int cpus = compiler_perf_cpu_count();
    int busy = (int)load;
    if ((double)busy < load) ++busy;
    int free_cpus = cpus - busy;
    if (free_cpus < 1) free_cpus = 1;
    int adaptive = free_cpus / 2;
    if (adaptive < 1) adaptive = 1;
    return adaptive < requested ? adaptive : requested;
}

static void compiler_abs_path(const char *path, char out[PATH_MAX]) {
    if (!path || !*path) {
        if (!getcwd(out, PATH_MAX)) c__copy(out, PATH_MAX, ".");
        return;
    }
    if (path[0] == '/') {
        c__copy(out, PATH_MAX, path);
        return;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        c__copy(out, PATH_MAX, path);
        return;
    }
    path_join(out, cwd, path);
}

static void compiler_perf_cache_path(const char *kind, const char *path, const char *suffix, char out[PATH_MAX]) {
    char absolute[PATH_MAX], cache[PATH_MAX], perf[PATH_MAX], root[PATH_MAX], shard_dir[PATH_MAX];
    char shard[3], key[17], name[64];
    compiler_abs_path(path, absolute);
    hash_u64_hex(hash_string(absolute), key);
    cache_root(cache);
    path_join(perf, cache, "perf");
    path_join(root, perf, kind);
    shard[0] = key[0]; shard[1] = key[1]; shard[2] = '\0';
    path_join(shard_dir, root, shard);
    mkdir_p(shard_dir);
    int n = snprintf(name, sizeof(name), "%s%s", key, suffix ? suffix : "");
    if (n < 0 || n >= (int)sizeof(name)) die("performance cache name too long");
    path_join(out, shard_dir, name);
}

static bool compiler_perf_stat(const char *path, time_t *sec, long *nsec, off_t *size) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    *sec = st.st_mtime;
#ifdef __APPLE__
    *nsec = st.st_mtimespec.tv_nsec;
#else
    *nsec = st.st_mtim.tv_nsec;
#endif
    *size = st.st_size;
    return true;
}

static bool compiler_persistent_hash_lookup(const char *path, time_t sec, long nsec, off_t size, uint64_t *out) {
    char meta[PATH_MAX], absolute[PATH_MAX];
    compiler_perf_cache_path("hash", path, ".hash", meta);
    compiler_abs_path(path, absolute);
    FILE *f = fopen(meta, "r");
    if (!f) return false;
    long long saved_sec = 0, saved_size = 0;
    long saved_nsec = 0;
    unsigned long long saved_hash = 0;
    char saved_path[PATH_MAX];
    bool ok = fscanf(f, "%lld %ld %lld %llx\n", &saved_sec, &saved_nsec, &saved_size, &saved_hash) == 4 &&
              fgets(saved_path, sizeof(saved_path), f) != NULL;
    fclose(f);
    if (!ok) return false;
    char *nl = strpbrk(saved_path, "\r\n");
    if (nl) *nl = '\0';
    if (saved_sec != (long long)sec || saved_nsec != nsec || saved_size != (long long)size || strcmp(saved_path, absolute)) return false;
    *out = (uint64_t)saved_hash;
    return true;
}

static void compiler_persistent_hash_store(const char *path, time_t sec, long nsec, off_t size, uint64_t hash) {
    char meta[PATH_MAX], temp[PATH_MAX], absolute[PATH_MAX];
    compiler_perf_cache_path("hash", path, ".hash", meta);
    compiler_abs_path(path, absolute);
    int n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", meta, (long)getpid());
    if (n < 0 || n >= (int)sizeof(temp)) return;
    FILE *f = fopen(temp, "w");
    if (!f) return;
    bool ok = fprintf(f, "%lld %ld %lld %016llx\n%s\n",
                      (long long)sec, nsec, (long long)size,
                      (unsigned long long)hash, absolute) >= 0;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(temp, meta) != 0) unlink(temp);
}

static bool compiler_dep_cache_load(const char *depfile, StrVec *deps) {
    time_t sec = 0; long nsec = 0; off_t size = 0;
    if (!compiler_perf_stat(depfile, &sec, &nsec, &size)) return false;
    char cache_path[PATH_MAX];
    compiler_perf_cache_path("deps", depfile, ".deps", cache_path);
    FILE *f = fopen(cache_path, "r");
    if (!f) return false;
    long long saved_sec = 0, saved_size = 0; long saved_nsec = 0;
    if (fscanf(f, "%lld %ld %lld\n", &saved_sec, &saved_nsec, &saved_size) != 3 ||
        saved_sec != (long long)sec || saved_nsec != nsec || saved_size != (long long)size) {
        fclose(f);
        return false;
    }
    char line[PATH_MAX * 2];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (*line) vec_push(deps, line);
    }
    fclose(f);
    return deps->count != 0;
}

static void compiler_dep_cache_store(const char *depfile, const StrVec *deps) {
    time_t sec = 0; long nsec = 0; off_t size = 0;
    if (!compiler_perf_stat(depfile, &sec, &nsec, &size)) return;
    char cache_path[PATH_MAX], temp[PATH_MAX];
    compiler_perf_cache_path("deps", depfile, ".deps", cache_path);
    int n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", cache_path, (long)getpid());
    if (n < 0 || n >= (int)sizeof(temp)) return;
    FILE *f = fopen(temp, "w");
    if (!f) return;
    bool ok = fprintf(f, "%lld %ld %lld\n", (long long)sec, nsec, (long long)size) >= 0;
    for (size_t i = 0; ok && i < deps->count; ++i) {
        if (fprintf(f, "%s\n", deps->items[i]) < 0) ok = false;
    }
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(temp, cache_path) != 0) unlink(temp);
}

static bool compiler_read_depfile_persistent(const char *path, StrVec *deps) {
    if (compiler_dep_cache_load(path, deps)) return true;
    if (!compiler_read_depfile(path, deps)) return false;
    compiler_dep_cache_store(path, deps);
    return true;
}

static bool compiler_clone_atomic(const char *from, const char *to) {
    char temp[PATH_MAX];
    int n = snprintf(temp, sizeof(temp), "%s.clone.%ld", to, (long)getpid());
    if (n < 0 || n >= (int)sizeof(temp)) return false;
    unlink(temp);
#ifdef __APPLE__
    if (clonefile(from, temp, 0) == 0) {
        if (rename(temp, to) == 0) return true;
        unlink(temp);
    }
#elif defined(__linux__)
    int in = open(from, O_RDONLY);
    if (in >= 0) {
        int out = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (out >= 0) {
            if (ioctl(out, FICLONE, in) == 0) {
                close(out); close(in);
                if (rename(temp, to) == 0) return true;
                unlink(temp);
                return false;
            }
            close(out);
            unlink(temp);
        }
        close(in);
    }
#endif
    return false;
}

static bool compiler_clone_or_copy(const char *from, const char *to) {
    return compiler_clone_atomic(from, to) || compiler_copy_atomic(from, to);
}

typedef struct CompilerProfileRecord {
    char source[PATH_MAX];
    double ms;
    bool cache_hit;
} CompilerProfileRecord;

#define COMPILER_PROFILE_MAX 512
static CompilerProfileRecord compiler_profile_records[COMPILER_PROFILE_MAX];
static size_t compiler_profile_count = 0;
static size_t compiler_profile_cache_hits = 0;
static size_t compiler_profile_compiled = 0;

static double compiler_perf_now_ms(void) {
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static double compiler_history_read(const char *source) {
    char path[PATH_MAX];
    compiler_perf_cache_path("timings", source, ".time", path);
    FILE *f = fopen(path, "r");
    if (!f) return 0.0;
    double ema = 0.0;
    unsigned long count = 0;
    bool ok = fscanf(f, "%lf %lu", &ema, &count) == 2 && count > 0 && ema > 0.0;
    fclose(f);
    return ok ? ema : 0.0;
}

static double compiler_source_estimate(const char *source) {
    double history = compiler_history_read(source);
    if (history > 0.0) return history;
    struct stat st;
    double size_kb = stat(source, &st) == 0 ? (double)st.st_size / 1024.0 : 1.0;
    const char *ext = strrchr(source, '.');
    double factor = 0.8;
    if (ext && (!strcmp(ext, ".cpp") || !strcmp(ext, ".cc") || !strcmp(ext, ".cxx") || !strcmp(ext, ".mm"))) factor = 3.5;
    else if (ext && !strcmp(ext, ".m")) factor = 1.5;
    double estimate = size_kb * factor;
    return estimate > 1.0 ? estimate : 1.0;
}

static void compiler_profile_add(const char *source, double ms, bool cache_hit) {
    if (cache_hit) ++compiler_profile_cache_hits;
    else ++compiler_profile_compiled;
    if (!compiler_perf.profile || compiler_profile_count >= COMPILER_PROFILE_MAX) return;
    CompilerProfileRecord *r = &compiler_profile_records[compiler_profile_count++];
    c__copy(r->source, sizeof(r->source), source);
    r->ms = ms;
    r->cache_hit = cache_hit;
}

static void compiler_history_record(const char *source, double ms) {
    if (ms <= 0.0) return;
    char path[PATH_MAX], temp[PATH_MAX];
    compiler_perf_cache_path("timings", source, ".time", path);
    double previous = 0.0;
    unsigned long count = 0;
    FILE *in = fopen(path, "r");
    if (in) {
        if (fscanf(in, "%lf %lu", &previous, &count) != 2) { previous = 0.0; count = 0; }
        fclose(in);
    }
    double ema = count ? previous * 0.70 + ms * 0.30 : ms;
    int n = snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
    if (n >= 0 && n < (int)sizeof(temp)) {
        FILE *out = fopen(temp, "w");
        if (out) {
            bool ok = fprintf(out, "%.6f %lu\n", ema, count + 1) >= 0;
            if (fclose(out) != 0) ok = false;
            if (!ok || rename(temp, path) != 0) unlink(temp);
        }
    }
    compiler_profile_add(source, ms, false);
}

static void compiler_profile_cached(const char *source) {
    compiler_profile_add(source, 0.0, true);
}

static void compiler_profile_reset(void) {
    compiler_profile_count = 0;
    compiler_profile_cache_hits = 0;
    compiler_profile_compiled = 0;
}

static int compiler_profile_record_cmp(const void *a, const void *b) {
    const CompilerProfileRecord *ra = a, *rb = b;
    if (ra->cache_hit != rb->cache_hit) return ra->cache_hit ? 1 : -1;
    return ra->ms < rb->ms ? 1 : ra->ms > rb->ms ? -1 : 0;
}

static void compiler_profile_report(const char *target, double link_ms) {
    if (!compiler_perf.profile) return;
    CompilerProfileRecord copy[COMPILER_PROFILE_MAX];
    size_t n = compiler_profile_count;
    memcpy(copy, compiler_profile_records, n * sizeof(copy[0]));
    qsort(copy, n, sizeof(copy[0]), compiler_profile_record_cmp);
    fprintf(stderr, "\nprofile [%s]\n", target ? target : "build");
    fprintf(stderr, "  jobs       %d%s\n", compiler_adaptive_jobs(compiler_perf.jobs), compiler_perf.adaptive_jobs ? " adaptive" : "");
    fprintf(stderr, "  compiled   %zu\n", compiler_profile_compiled);
    fprintf(stderr, "  cache hits %zu\n", compiler_profile_cache_hits);
    fprintf(stderr, "  link       %.2f ms\n", link_ms);
    size_t shown = 0;
    for (size_t i = 0; i < n && shown < 10; ++i) {
        if (copy[i].cache_hit) continue;
        fprintf(stderr, "  %8.2f ms  %s\n", copy[i].ms, copy[i].source);
        ++shown;
    }
}

static int compiler_unity_auto_mode(const StrVec *sources) {
    if (!sources || sources->count < 3) return 1;
    double total = 0.0;
    for (size_t i = 0; i < sources->count; ++i) total += compiler_source_estimate(sources->items[i]);
    double avg = total / (double)sources->count;
    if (avg < 30.0) return 16;
    if (avg < 120.0) return 8;
    if (avg < 400.0) return 4;
    return 2;
}

static uint64_t compiler_unity_membership_hash(StrVec *sources, size_t *indices, size_t count) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < count; ++i) {
        const char *s = sources->items[indices[i]];
        h = hash_update(h, s, strlen(s));
        const char zero = '\0';
        h = hash_update(h, &zero, 1);
    }
    return h;
}

static bool compiler_linker_available(const char *name) {
    if (!name || !*name) return false;
    if (!strcmp(name, "lld")) return command_exists("ld.lld") || command_exists("lld");
    return command_exists(name);
}

static const char *compiler_selected_linker(void) {
    if (!compiler_perf.linker[0]) return NULL;
    if (strcmp(compiler_perf.linker, "auto")) return compiler_perf.linker;
    if (compiler_linker_available("mold")) return "mold";
    if (compiler_linker_available("lld")) return "lld";
    return NULL;
}

static void compiler_append_linker(StrVec *a) {
    const char *linker = compiler_selected_linker();
    if (!linker) return;
    char flag[64];
    int n = snprintf(flag, sizeof(flag), "-fuse-ld=%s", linker);
    if (n < 0 || n >= (int)sizeof(flag)) die("linker name too long");
    vec_push(a, flag);
}

#ifdef __APPLE__
/*
 * Atomic publication links into a temporary path and renames the finished
 * artifact. Darwin records that temporary path as a dylib's install name
 * unless an explicit stable name is supplied, leaving consumers pointing at
 * a directory that is deleted immediately after publication.
 */
static int compiler_run_process_atomic_output(StrVec *args, bool verbose, const char *output) {
    bool dynamic_library = false;
    for (size_t i = 0; i < args->count; ++i) {
        if (!strcmp(args->items[i], "-dynamiclib")) {
            dynamic_library = true;
            break;
        }
    }
    if (dynamic_library) {
        char install_name[PATH_MAX + 32];
        int n = snprintf(install_name, sizeof(install_name), "-Wl,-install_name,%s", output);
        if (n < 0 || n >= (int)sizeof(install_name)) {
            errno = ENAMETOOLONG;
            return 1;
        }
        vec_push(args, install_name);
    }
    return run_process_atomic_output(args, verbose, output);
}
#define run_process_atomic_output compiler_run_process_atomic_output
#endif

static bool compiler_watch_skip_name(const char *name) {
    return !strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".git") ||
           !strcmp(name, "build") || !strcmp(name, ".rendercheck") ||
           !strcmp(name, "compile_commands.json");
}

static int compiler_name_cmp(const void *a, const void *b) {
    const char *const *sa = a, *const *sb = b;
    return strcmp(*sa, *sb);
}

static uint64_t compiler_watch_hash_tree(const char *path, uint64_t h) {
    struct stat st;
    if (lstat(path, &st) != 0) return h;
    h = hash_update(h, path, strlen(path));
    h = hash_update(h, &st.st_mode, sizeof(st.st_mode));
    h = hash_update(h, &st.st_size, sizeof(st.st_size));
    h = hash_update(h, &st.st_mtime, sizeof(st.st_mtime));
#ifdef __APPLE__
    long ns = st.st_mtimespec.tv_nsec;
#else
    long ns = st.st_mtim.tv_nsec;
#endif
    h = hash_update(h, &ns, sizeof(ns));
    if (!S_ISDIR(st.st_mode)) return h;

    DIR *d = opendir(path);
    if (!d) return h;
    StrVec names = {0};
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (compiler_watch_skip_name(ent->d_name)) continue;
        vec_push(&names, ent->d_name);
    }
    closedir(d);
    qsort(names.items, names.count, sizeof(names.items[0]), compiler_name_cmp);
    for (size_t i = 0; i < names.count; ++i) {
        char child[PATH_MAX];
        path_join(child, path, names.items[i]);
        h = compiler_watch_hash_tree(child, h);
    }
    vec_free(&names);
    return h;
}

static uint64_t compiler_watch_fingerprint(void) {
    return compiler_watch_hash_tree(".", 1469598103934665603ULL);
}

static void compiler_watch_sleep(void) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 250000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

#endif
