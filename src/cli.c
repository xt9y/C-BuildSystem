#define C_DEP_CMAKE C_DEP_RESERVED
#define cmake_options compile_flags
#define main c_legacy_main
#include "main.c"
#undef main
#undef cmake_options
#undef C_DEP_CMAKE

#include <signal.h>
#include <spawn.h>

extern char **environ;

typedef struct CompilerPerfOptions {
    int jobs;
    int unity;              /* 0=off, -1=auto, >=2=fixed chunk size */
    bool object_cache;
    bool profile;
    bool fast_debug;
    bool adaptive_jobs;
    bool jobs_explicit;
    bool explain;
    char linker[32];
} CompilerPerfOptions;

static CompilerPerfOptions compiler_perf = {0};

#include "perf_v2.h"

static const char *compiler_ar(void) {
    const char *ar = getenv("AR");
    return (ar && *ar) ? ar : "ar";
}

static bool compiler_cpp_source(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return false;
    return !strcmp(ext, ".cpp") || !strcmp(ext, ".cc") || !strcmp(ext, ".cxx") || !strcmp(ext, ".mm");
}

static bool compiler_c_standard_flag(const char *flag) {
    return !strncmp(flag, "-std=c", 6) && strncmp(flag, "-std=c++", 8);
}

static bool compiler_cpp_standard_flag(const char *flag) {
    return !strncmp(flag, "-std=c++", 8) || !strncmp(flag, "-std=gnu++", 10);
}

static void compiler_push_standard(StrVec *a, const char *source) {
    vec_push(a, compiler_cpp_source(source) ? "-std=c++17" : "-std=c11");
}

static void compiler_push_source_flags(StrVec *a, const C_StringList *flags, const char *source) {
    bool cpp = compiler_cpp_source(source);
    for (size_t i = 0; i < flags->count; ++i) {
        const char *flag = flags->items[i];
        if (cpp && compiler_c_standard_flag(flag)) continue;
        if (!cpp && compiler_cpp_standard_flag(flag)) continue;
        vec_push(a, flag);
    }
}

static int compiler_cpu_count(void) {
    return compiler_perf_cpu_count();
}

static int compiler_positive_int(const char *s, const char *what) {
    if (!s || !*s) die("%s requires a positive integer", what);
    char *end = NULL;
    errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || !end || *end || n < 1 || n > 1024) die("invalid %s: %s", what, s);
    return (int)n;
}

static void compiler_perf_defaults(void) {
    compiler_perf.jobs = compiler_default_jobs();
    compiler_perf.unity = 0;
    compiler_perf.object_cache = true;
    compiler_perf.profile = false;
    compiler_perf.fast_debug = false;
    compiler_perf.adaptive_jobs = false;
    compiler_perf.jobs_explicit = false;
    compiler_perf.explain = false;
    compiler_perf.linker[0] = '\0';

    const char *jobs = getenv("C_JOBS");
    if (jobs && *jobs) {
        compiler_perf.jobs = compiler_positive_int(jobs, "C_JOBS");
        compiler_perf.jobs_explicit = true;
    }
    const char *unity = getenv("C_UNITY");
    if (unity && *unity && strcmp(unity, "0")) {
        compiler_perf.unity = !strcmp(unity, "auto") ? -1 : compiler_positive_int(unity, "C_UNITY");
    }
    const char *cache = getenv("C_OBJECT_CACHE");
    if (cache && (!strcmp(cache, "0") || !strcmp(cache, "false") || !strcmp(cache, "off"))) compiler_perf.object_cache = false;
    const char *profile = getenv("C_PROFILE");
    if (profile && strcmp(profile, "0") && strcmp(profile, "false") && strcmp(profile, "off")) compiler_perf.profile = true;
    const char *fast_debug = getenv("C_FAST_DEBUG");
    if (fast_debug && strcmp(fast_debug, "0") && strcmp(fast_debug, "false") && strcmp(fast_debug, "off")) compiler_perf.fast_debug = true;
    const char *adaptive = getenv("C_ADAPTIVE_JOBS");
    if (adaptive && strcmp(adaptive, "0") && strcmp(adaptive, "false") && strcmp(adaptive, "off")) compiler_perf.adaptive_jobs = true;
    const char *explain = getenv("C_EXPLAIN");
    if (explain && strcmp(explain, "0") && strcmp(explain, "false") && strcmp(explain, "off")) compiler_perf.explain = true;
    const char *linker = getenv("C_LINKER");
    if (linker && *linker) c__copy(compiler_perf.linker, sizeof(compiler_perf.linker), linker);
}

static char **compiler_filter_perf_options(int argc, char **argv, int *out_argc) {
    char **out = calloc((size_t)argc + 1, sizeof(*out));
    if (!out) die("out of memory");
    int n = 0;
    bool forwarded = false;
    for (int i = 0; i < argc; ++i) {
        const char *arg = argv[i];
        if (i < 2 || forwarded) {
            out[n++] = argv[i];
            continue;
        }
        if (!strcmp(arg, "--")) {
            forwarded = true;
            out[n++] = argv[i];
            continue;
        }
        if (!strcmp(arg, "-j") || !strcmp(arg, "--jobs")) {
            if (i + 1 >= argc) die("%s requires a value", arg);
            compiler_perf.jobs = compiler_positive_int(argv[++i], "jobs");
            compiler_perf.jobs_explicit = true;
            continue;
        }
        if (!strncmp(arg, "-j", 2) && arg[2]) {
            compiler_perf.jobs = compiler_positive_int(arg + 2, "jobs");
            compiler_perf.jobs_explicit = true;
            continue;
        }
        if (!strncmp(arg, "--jobs=", 7)) {
            compiler_perf.jobs = compiler_positive_int(arg + 7, "jobs");
            compiler_perf.jobs_explicit = true;
            continue;
        }
        if (!strcmp(arg, "--unity")) {
            compiler_perf.unity = 8;
            continue;
        }
        if (!strncmp(arg, "--unity=", 8)) {
            const char *value = arg + 8;
            compiler_perf.unity = !strcmp(value, "auto") ? -1 : compiler_positive_int(value, "unity chunk size");
            continue;
        }
        if (!strcmp(arg, "--no-unity")) {
            compiler_perf.unity = 0;
            continue;
        }
        if (!strcmp(arg, "--no-object-cache")) {
            compiler_perf.object_cache = false;
            continue;
        }
        if (!strcmp(arg, "--object-cache")) {
            compiler_perf.object_cache = true;
            continue;
        }
        if (!strcmp(arg, "--profile")) {
            compiler_perf.profile = true;
            continue;
        }
        if (!strcmp(arg, "--explain")) {
            compiler_perf.explain = true;
            continue;
        }
        if (!strcmp(arg, "--fast-debug")) {
            compiler_perf.fast_debug = true;
            continue;
        }
        if (!strcmp(arg, "--adaptive-jobs")) {
            compiler_perf.adaptive_jobs = true;
            continue;
        }
        if (!strcmp(arg, "--no-adaptive-jobs")) {
            compiler_perf.adaptive_jobs = false;
            continue;
        }
        if (!strcmp(arg, "--linker")) {
            if (i + 1 >= argc) die("--linker requires a value");
            c__copy(compiler_perf.linker, sizeof(compiler_perf.linker), argv[++i]);
            continue;
        }
        if (!strncmp(arg, "--linker=", 9)) {
            c__copy(compiler_perf.linker, sizeof(compiler_perf.linker), arg + 9);
            continue;
        }
        out[n++] = argv[i];
    }
    out[n] = NULL;
    *out_argc = n;
    return out;
}

static void compiler_print_command(StrVec *args) {
    fprintf(stderr, "  $ ");
    for (size_t i = 0; i < args->count; ++i) fprintf(stderr, "%s%s", i ? " " : "", args->items[i]);
    fputc('\n', stderr);
}

static int compiler_spawn(StrVec *args, bool verbose, pid_t *pid) {
    if (!args->count) return 0;
    if (verbose) compiler_print_command(args);
    int rc = posix_spawnp(pid, args->items[0], NULL, NULL, args->items, environ);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

static int compiler_wait(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static int compiler_run_process(StrVec *args, bool verbose) {
    pid_t pid = -1;
    if (compiler_spawn(args, verbose, &pid) != 0) {
        fprintf(stderr, "c: error: cannot start %s: %s\n", args->count ? args->items[0] : "process", strerror(errno));
        return 127;
    }
    return compiler_wait(pid);
}

typedef struct CompilerStamp {
    time_t sec;
    long nsec;
    off_t size;
    bool exists;
} CompilerStamp;

static bool compiler_stamp_equal(CompilerStamp a, CompilerStamp b) {
    return a.exists == b.exists && (!a.exists || (a.sec == b.sec && a.nsec == b.nsec && a.size == b.size));
}

static int compiler_stamp_compare(CompilerStamp a, CompilerStamp b) {
    if (!a.exists || !b.exists) return a.exists ? 1 : b.exists ? -1 : 0;
    if (a.sec != b.sec) return a.sec > b.sec ? 1 : -1;
    if (a.nsec != b.nsec) return a.nsec > b.nsec ? 1 : -1;
    return 0;
}

static CompilerStamp compiler_stat_now(const char *path) {
    struct stat st;
    CompilerStamp out = {0};
    if (stat(path, &st) != 0) return out;
    out.exists = true;
    out.sec = st.st_mtime;
#ifdef __APPLE__
    out.nsec = st.st_mtimespec.tv_nsec;
#else
    out.nsec = st.st_mtim.tv_nsec;
#endif
    out.size = st.st_size;
    return out;
}

#define COMPILER_PATH_CACHE_SIZE 4096

typedef struct CompilerPathCacheEntry {
    uint64_t key;
    char *path;
    CompilerStamp stamp;
} CompilerPathCacheEntry;

static CompilerPathCacheEntry compiler_stat_cache[COMPILER_PATH_CACHE_SIZE];

static CompilerStamp compiler_cached_stamp(const char *path) {
    uint64_t key = hash_string(path);
    size_t slot = (size_t)(key % COMPILER_PATH_CACHE_SIZE);
    CompilerPathCacheEntry *e = &compiler_stat_cache[slot];
    if (e->path && e->key == key && !strcmp(e->path, path)) return e->stamp;
    free(e->path);
    e->path = xstrdup(path);
    e->key = key;
    e->stamp = compiler_stat_now(path);
    return e->stamp;
}

typedef struct CompilerHashCacheEntry {
    uint64_t key;
    char *path;
    CompilerStamp stamp;
    uint64_t hash;
    bool valid;
} CompilerHashCacheEntry;

static CompilerHashCacheEntry compiler_hash_cache[COMPILER_PATH_CACHE_SIZE];

static bool compiler_hash_file(const char *path, uint64_t *out) {
    CompilerStamp stamp = compiler_cached_stamp(path);
    if (!stamp.exists) return false;
    uint64_t key = hash_string(path);
    size_t slot = (size_t)(key % COMPILER_PATH_CACHE_SIZE);
    CompilerHashCacheEntry *e = &compiler_hash_cache[slot];
    if (e->valid && e->path && e->key == key && !strcmp(e->path, path) && compiler_stamp_equal(e->stamp, stamp)) {
        *out = e->hash;
        return true;
    }
    uint64_t persistent = 0;
    if (compiler_persistent_hash_lookup(path, stamp.sec, stamp.nsec, stamp.size, &persistent)) {
        free(e->path);
        e->path = xstrdup(path);
        e->key = key;
        e->stamp = stamp;
        e->hash = persistent;
        e->valid = true;
        *out = persistent;
        return true;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint64_t h = 1469598103934665603ULL;
    unsigned char buf[32768];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f))) h = hash_update(h, buf, n);
    if (ferror(f)) { fclose(f); return false; }
    fclose(f);

    free(e->path);
    e->path = xstrdup(path);
    e->key = key;
    e->stamp = stamp;
    e->hash = h;
    e->valid = true;
    compiler_persistent_hash_store(path, stamp.sec, stamp.nsec, stamp.size, h);
    *out = h;
    return true;
}

static bool compiler_hash_file_uncached(const char *path, uint64_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint64_t h = 1469598103934665603ULL;
    unsigned char buf[32768];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f))) h = hash_update(h, buf, n);
    if (ferror(f)) { fclose(f); return false; }
    if (fclose(f) != 0) return false;
    *out = h;
    return true;
}

static bool compiler_read_depfile(const char *path, StrVec *deps) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long end = ftell(f);
    if (end < 0) { fclose(f); return false; }
    rewind(f);
    char *buf = malloc((size_t)end + 1);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)end, f);
    fclose(f);
    if (got != (size_t)end) { free(buf); return false; }
    buf[end] = '\0';

    char *colon = strchr(buf, ':');
    if (!colon) { free(buf); return false; }
    char *p = colon + 1;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || (*p == '\\' && p[1] == '\n')) {
            if (*p == '\\' && p[1] == '\n') p += 2;
            else ++p;
        }
        if (!*p) break;
        char token[PATH_MAX];
        size_t len = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            if (*p == '\\' && p[1] == '\n') { p += 2; continue; }
            if (*p == '\\' && p[1]) ++p;
            if (len + 1 >= sizeof(token)) { free(buf); return false; }
            token[len++] = *p++;
        }
        token[len] = '\0';
        if (len) vec_push(deps, token);
    }
    free(buf);
    return deps->count != 0;
}

static bool compiler_depfile_fresh(const char *obj, const char *depfile, const char *src) {
    CompilerStamp object = compiler_stat_now(obj);
    if (!object.exists) return false;
    CompilerStamp source = compiler_cached_stamp(src);
    if (!source.exists || compiler_stamp_compare(source, object) > 0) return false;

    StrVec deps = {0};
    if (!compiler_read_depfile_persistent(depfile, &deps)) return false;
    bool fresh = true;
    for (size_t i = 0; i < deps.count; ++i) {
        CompilerStamp dep = compiler_cached_stamp(deps.items[i]);
        if (!dep.exists || compiler_stamp_compare(dep, object) > 0) { fresh = false; break; }
    }
    vec_free(&deps);
    return fresh;
}

static bool compiler_find_executable(const char *name, char out[PATH_MAX]) {
    if (!name || !*name) return false;
    if (strchr(name, '/')) {
        char resolved[PATH_MAX];
        if (realpath(name, resolved)) c__copy(out, PATH_MAX, resolved);
        else c__copy(out, PATH_MAX, name);
        return access(out, X_OK) == 0;
    }
    const char *path = getenv("PATH");
    if (!path) return false;
    char *copy = xstrdup(path);
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char candidate[PATH_MAX];
        path_join(candidate, *dir ? dir : ".", name);
        if (access(candidate, X_OK) == 0) {
            char resolved[PATH_MAX];
            if (realpath(candidate, resolved)) c__copy(out, PATH_MAX, resolved);
            else c__copy(out, PATH_MAX, candidate);
            free(copy);
            return true;
        }
    }
    free(copy);
    return false;
}

static uint64_t compiler_tool_identity(const char *tool) {
    char path[PATH_MAX];
    uint64_t h = hash_string(tool ? tool : "");
    if (!compiler_find_executable(tool, path)) return h;
    CompilerStamp st = compiler_stat_now(path);
    h = hash_update(h, path, strlen(path));
    h = hash_update(h, &st.sec, sizeof(st.sec));
    h = hash_update(h, &st.nsec, sizeof(st.nsec));
    h = hash_update(h, &st.size, sizeof(st.size));
    return h;
}

static uint64_t compiler_command_cache_hash(const StrVec *cmd, const char *source) {
    uint64_t h = 1469598103934665603ULL;
    uint64_t tool = compiler_tool_identity(cmd->count ? cmd->items[0] : "");
    h = hash_update(h, &tool, sizeof(tool));
    h = hash_update(h, source, strlen(source));
    uint64_t source_hash = 0;
    if (compiler_hash_file(source, &source_hash)) h = hash_update(h, &source_hash, sizeof(source_hash));

    for (size_t i = 1; i < cmd->count; ++i) {
        const char *arg = cmd->items[i];
        if (!strcmp(arg, "-o") || !strcmp(arg, "-MF")) { if (i + 1 < cmd->count) ++i; continue; }
        if (!strcmp(arg, "-c") || !strcmp(arg, source)) continue;
        h = hash_update(h, arg, strlen(arg));
        const char zero = '\0';
        h = hash_update(h, &zero, 1);
    }

    static const char *envs[] = {
        "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
        "SDKROOT", "MACOSX_DEPLOYMENT_TARGET"
    };
    for (size_t i = 0; i < C_ARRAY_LEN(envs); ++i) {
        const char *v = getenv(envs[i]);
        if (v) h = hash_update(h, v, strlen(v));
    }
    return h;
}

static void compiler_object_cache_paths(const char key[17], char obj[PATH_MAX], char dep[PATH_MAX], char meta[PATH_MAX]) {
    char cache[PATH_MAX], root[PATH_MAX], shard[3], dir[PATH_MAX];
    cache_root(cache);
    path_join(root, cache, "objects");
    shard[0] = key[0]; shard[1] = key[1]; shard[2] = '\0';
    path_join(dir, root, shard);
    mkdir_p(dir);
    snprintf(obj, PATH_MAX, "%s/%s.o", dir, key);
    snprintf(dep, PATH_MAX, "%s/%s.d", dir, key);
    snprintf(meta, PATH_MAX, "%s/%s.meta", dir, key);
}

static bool compiler_copy_atomic(const char *from, const char *to) {
    char temp[PATH_MAX];
    if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", to, (long)getpid()) >= (int)sizeof(temp)) return false;
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    FILE *out = fopen(temp, "wb");
    if (!out) { fclose(in); return false; }
    unsigned char buf[32768];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in))) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    if (fflush(out) != 0) ok = false;
    if (ok && fsync(fileno(out)) != 0) ok = false;
    if (fclose(in) != 0) ok = false;
    if (fclose(out) != 0) ok = false;
    if (!ok || rename(temp, to) != 0) { unlink(temp); return false; }
    return true;
}

static bool compiler_object_cache_restore(const char key[17], const char *obj, const char *depf) {
    if (!compiler_perf.object_cache) return false;
    char cached_obj[PATH_MAX], cached_dep[PATH_MAX], meta[PATH_MAX];
    compiler_object_cache_paths(key, cached_obj, cached_dep, meta);
    if (!file_exists(cached_obj) || !file_exists(cached_dep) || !file_exists(meta)) return false;

    FILE *f = fopen(meta, "r");
    if (!f) return false;
    char line[PATH_MAX + 64];
    if (!fgets(line, sizeof(line), f) || strcmp(line, "c-object-cache-v1\n")) { fclose(f); return false; }
    bool valid = true;
    while (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (!tab) { valid = false; break; }
        *tab++ = '\0';
        char *nl = strpbrk(tab, "\r\n");
        if (nl) *nl = '\0';
        char *end = NULL;
        unsigned long long expected = strtoull(line, &end, 16);
        uint64_t actual = 0;
        if (!end || *end || !compiler_hash_file(tab, &actual) || actual != (uint64_t)expected) { valid = false; break; }
    }
    fclose(f);
    if (!valid) {
        unlink(cached_obj);
        unlink(cached_dep);
        unlink(meta);
        return false;
    }
    return compiler_clone_or_copy(cached_obj, obj) && compiler_clone_or_copy(cached_dep, depf);
}

static void compiler_object_cache_store(const char key[17], const char *obj, const char *depf) {
    if (!compiler_perf.object_cache || !file_exists(obj) || !file_exists(depf)) return;
    StrVec deps = {0};
    if (!compiler_read_depfile(depf, &deps)) return;

    char cached_obj[PATH_MAX], cached_dep[PATH_MAX], meta[PATH_MAX];
    compiler_object_cache_paths(key, cached_obj, cached_dep, meta);
    char temp_meta[PATH_MAX];
    if (snprintf(temp_meta, sizeof(temp_meta), "%s.tmp.%ld", meta, (long)getpid()) >= (int)sizeof(temp_meta)) { vec_free(&deps); return; }
    FILE *f = fopen(temp_meta, "w");
    if (!f) { vec_free(&deps); return; }
    fputs("c-object-cache-v1\n", f);
    bool ok = true;
    for (size_t i = 0; i < deps.count; ++i) {
        uint64_t h = 0;
        if (!compiler_hash_file(deps.items[i], &h)) { ok = false; break; }
        if (fprintf(f, "%016llx\t%s\n", (unsigned long long)h, deps.items[i]) < 0) { ok = false; break; }
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) { unlink(temp_meta); vec_free(&deps); return; }
    if (!compiler_clone_or_copy(obj, cached_obj) || !compiler_clone_or_copy(depf, cached_dep) || rename(temp_meta, meta) != 0) unlink(temp_meta);
    vec_free(&deps);
}

typedef struct CompilerTask {
    StrVec cmd;
    char *source;
    char *obj;
    char *depf;
    char key[17];
    pid_t pid;
    bool active;
    double estimate_ms;
    double started_ms;
} CompilerTask;

static bool compiler_object_ready(const char *obj) {
    return ready_marker_matches(obj, "object");
}

static void compiler_object_invalidate(const char *obj, const char *depf) {
    if (obj) remove_ready_marker(obj);
    if (obj && unlink(obj) != 0 && errno != ENOENT) die("cannot invalidate object %s: %s", obj, strerror(errno));
    if (depf && unlink(depf) != 0 && errno != ENOENT) die("cannot invalidate depfile %s: %s", depf, strerror(errno));
}

static void compiler_task_free(CompilerTask *t) {
    vec_free(&t->cmd);
    free(t->source);
    free(t->obj);
    free(t->depf);
    memset(t, 0, sizeof(*t));
}

typedef enum CompilerPrepareResult {
    COMPILER_PREP_FRESH = 0,
    COMPILER_PREP_CACHE = 1,
    COMPILER_PREP_BUILD = 2
} CompilerPrepareResult;

static CompilerPrepareResult compiler_prepare_task(CompilerTask *task, StrVec *cmd, const char *source, const char *obj, const char *depf) {
    if (compiler_object_ready(obj) && compiler_depfile_fresh(obj, depf, source)) {
        if (compiler_perf.explain) note("WHY", "%s is fresh", source);
        vec_free(cmd);
        return COMPILER_PREP_FRESH;
    }

    uint64_t h = compiler_command_cache_hash(cmd, source);
    char key[17];
    hash_u64_hex(h, key);
    if (compiler_object_cache_restore(key, obj, depf)) {
        write_ready_marker(obj, "object");
        if (compiler_perf.explain) note("WHY", "%s restored from object cache", source);
        compiler_profile_cached(source);
        vec_free(cmd);
        return COMPILER_PREP_CACHE;
    }

    if (compiler_perf.explain) note("WHY", "%s rebuild required (source/dependency/command changed or object missing)", source);
    compiler_object_invalidate(obj, depf);
    task->cmd = *cmd;
    memset(cmd, 0, sizeof(*cmd));
    task->source = xstrdup(source);
    task->obj = xstrdup(obj);
    task->depf = xstrdup(depf);
    c__copy(task->key, sizeof(task->key), key);
    task->estimate_ms = compiler_source_estimate(source);
    return COMPILER_PREP_BUILD;
}

static void compiler_cancel_tasks(CompilerTask *tasks, size_t count) {
    for (size_t i = 0; i < count; ++i) if (tasks[i].active) kill(tasks[i].pid, SIGTERM);
    for (size_t i = 0; i < count; ++i) if (tasks[i].active) { compiler_wait(tasks[i].pid); tasks[i].active = false; }
    for (size_t i = 0; i < count; ++i) if (tasks[i].obj) compiler_object_invalidate(tasks[i].obj, tasks[i].depf);
}

static size_t compiler_execute_tasks(CompilerTask *tasks, size_t count, const Options *opt, const char *error_message) {
    if (!count) return 0;
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            if (tasks[j].estimate_ms > tasks[i].estimate_ms) {
                CompilerTask tmp = tasks[i];
                tasks[i] = tasks[j];
                tasks[j] = tmp;
            }
        }
    }
    size_t next = 0, finished = 0, running = 0;
    size_t limit = (size_t)compiler_adaptive_jobs(compiler_perf.jobs);
    if (limit < 1) limit = 1;
    if (limit > count) limit = count;

    while (finished < count) {
        while (next < count && running < limit) {
            note("CC", "%s", tasks[next].source);
            if (compiler_spawn(&tasks[next].cmd, opt->verbose, &tasks[next].pid) != 0) {
                compiler_cancel_tasks(tasks, count);
                die("cannot start compiler for %s: %s", tasks[next].source, strerror(errno));
            }
            tasks[next].active = true;
            tasks[next].started_ms = compiler_perf_now_ms();
            ++running;
            ++next;
        }

        int status = 0;
        pid_t done;
        do { done = waitpid(-1, &status, 0); } while (done < 0 && errno == EINTR);
        if (done < 0) {
            compiler_cancel_tasks(tasks, count);
            die("waitpid: %s", strerror(errno));
        }

        CompilerTask *task = NULL;
        for (size_t i = 0; i < count; ++i) if (tasks[i].active && tasks[i].pid == done) { task = &tasks[i]; break; }
        if (!task) continue;
        task->active = false;
        --running;
        ++finished;
        int rc = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
        if (rc != 0) {
            compiler_cancel_tasks(tasks, count);
            die("%s: %s", error_message, task->source);
        }
        compiler_history_record(task->source, compiler_perf_now_ms() - task->started_ms);
        compiler_object_cache_store(task->key, task->obj, task->depf);
    }
    for (size_t i = 0; i < count; ++i) write_ready_marker(tasks[i].obj, "object");
    return count;
}

static int compiler_language_kind(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    if (!strcmp(ext, ".mm")) return 3;
    if (!strcmp(ext, ".m")) return 2;
    if (!strcmp(ext, ".cpp") || !strcmp(ext, ".cc") || !strcmp(ext, ".cxx")) return 1;
    return 0;
}

static const char *compiler_unity_extension(int kind) {
    if (kind == 3) return ".mm";
    if (kind == 2) return ".m";
    if (kind == 1) return ".cpp";
    return ".c";
}

static void compiler_write_unity_file(const char *path, StrVec *sources, size_t *indices, size_t count, const char *root_prefix) {
    if (file_exists(path)) return;
    char temp[PATH_MAX];
    if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(temp)) die("unity source path too long");
    FILE *f = fopen(temp, "w");
    if (!f) die("cannot write unity source %s", temp);
    fputs("/* generated by c --unity */\n", f);
    for (size_t i = 0; i < count; ++i) {
        const char *src = sources->items[indices[i]];
        fputs("#include \"", f);
        if (src[0] != '/' && root_prefix) fputs(root_prefix, f);
        for (const char *p = src; *p; ++p) {
            if (*p == '\\' || *p == '"') fputc('\\', f);
            fputc(*p, f);
        }
        fputs("\"\n", f);
    }
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) { unlink(temp); die("cannot finish unity source %s", path); }
    if (rename(temp, path) != 0) { int saved = errno; unlink(temp); errno = saved; die("cannot publish unity source %s: %s", path, strerror(errno)); }
}

static void compiler_make_unity_sources(StrVec *sources, const char *unity_dir, const char *root_prefix, StrVec *out) {
    int mode = compiler_perf.unity;
    if (mode == 0 || mode == 1 || sources->count <= 1) {
        for (size_t i = 0; i < sources->count; ++i) vec_push(out, sources->items[i]);
        return;
    }
    mkdir_p(unity_dir);
    for (int kind = 0; kind < 4; ++kind) {
        size_t *indices = calloc(sources->count, sizeof(*indices));
        if (!indices) die("out of memory");
        size_t n = 0;
        for (size_t i = 0; i < sources->count; ++i) if (compiler_language_kind(sources->items[i]) == kind) indices[n++] = i;
        size_t start = 0, chunk = 0;
        while (start < n) {
            size_t count = 0;
            if (mode == -1) {
                size_t max_count = (size_t)compiler_unity_auto_mode(sources);
                double cost = 0.0;
                while (start + count < n && count < max_count) {
                    cost += compiler_source_estimate(sources->items[indices[start + count]]);
                    ++count;
                    if (count >= 2 && cost >= 600.0) break;
                }
            } else {
                count = n - start;
                if (count > (size_t)mode) count = (size_t)mode;
            }
            if (count == 0) count = 1;
            if (count == 1) {
                vec_push(out, sources->items[indices[start]]);
                start += count;
                ++chunk;
                continue;
            }
            uint64_t membership = compiler_unity_membership_hash(sources, indices + start, count);
            char member_hex[17], name[96], path[PATH_MAX];
            hash_u64_hex(membership, member_hex);
            snprintf(name, sizeof(name), "unity-%d-%zu-%s%s", kind, chunk, member_hex, compiler_unity_extension(kind));
            path_join(path, unity_dir, name);
            compiler_write_unity_file(path, sources, indices + start, count, root_prefix);
            vec_push(out, path);
            start += count;
            ++chunk;
        }
        free(indices);
    }
}

static void compiler_dep_root(const C_Dependency *d, const DepState *state, char out[PATH_MAX]) {
    if (d->subdir[0]) path_join(out, state->source, d->subdir);
    else c__copy(out, PATH_MAX, state->source);
}

static void compiler_dep_library(const C_Dependency *d, const DepState *state, char out[PATH_MAX]) {
    char name[C_MAX_NAME + 8];
    snprintf(name, sizeof(name), "lib%s.a", d->name);
    path_join(out, state->package, name);
}

static void compiler_build_dependency(const C_Dependency *d, const Options *opt, DepState *state) {
    if (d->kind != C_DEP_SOURCE) return;
    if (!d->source_patterns.count) die("source dependency %s has no sources; use c_dep_sources()", d->name);

    char library[PATH_MAX];
    compiler_dep_library(d, state, library);
    if (file_exists(library) && ready_marker_matches(library, "artifact")) {
        note("CACHED", "%s", d->name);
        return;
    }

    mkdir_p(state->package);
    char objdir[PATH_MAX];
    path_join(objdir, state->package, ".objs");
    mkdir_p(objdir);
    char root[PATH_MAX];
    compiler_dep_root(d, state, root);

    StrVec sources = {0}, compile_sources = {0}, objects = {0};
    for (size_t i = 0; i < d->source_patterns.count; ++i) {
        char pattern[PATH_MAX];
        path_join(pattern, root, d->source_patterns.items[i]);
        expand_pattern(pattern, &sources);
    }
    char unity_dir[PATH_MAX];
    path_join(unity_dir, state->package, ".unity");
    compiler_make_unity_sources(&sources, unity_dir, "", &compile_sources);

    CompilerTask *tasks = calloc(compile_sources.count, sizeof(*tasks));
    if (!tasks) die("out of memory");
    size_t task_count = 0, cache_hits = 0;
    note("DEP", "%s", d->name);

    for (size_t i = 0; i < compile_sources.count; ++i) {
        const char *source = compile_sources.items[i];
        char obj[PATH_MAX], depf[PATH_MAX];
        object_path(objdir, source, obj);
        if (strlen(obj) + 3 >= sizeof(depf)) die("object path too long: %s", obj);
        c__copy(depf, sizeof(depf), obj); strcat(depf, ".d");
        vec_push(&objects, obj);

        StrVec a = {0};
        vec_push(&a, opt->cc);
        compiler_push_standard(&a, source);
        if (opt->release) vec_push(&a, "-O2");
        else if (compiler_perf.fast_debug) { vec_push(&a, "-O1"); vec_push(&a, "-g1"); }
        else { vec_push(&a, "-O0"); vec_push(&a, "-g"); }
        vec_push(&a, "-MMD"); vec_push(&a, "-MF"); vec_push(&a, depf);
        char root_inc[PATH_MAX + 3]; snprintf(root_inc, sizeof(root_inc), "-I%s", root); vec_push(&a, root_inc);
        for (size_t j = 0; j < d->include_dirs.count; ++j) {
            char inc[PATH_MAX], arg[PATH_MAX + 3];
            path_join(inc, root, d->include_dirs.items[j]);
            snprintf(arg, sizeof(arg), "-I%s", inc); vec_push(&a, arg);
        }
        compiler_push_source_flags(&a, &d->compile_flags, source);
        vec_push(&a, "-c"); vec_push(&a, source); vec_push(&a, "-o"); vec_push(&a, obj);

        CompilerPrepareResult prep = compiler_prepare_task(&tasks[task_count], &a, source, obj, depf);
        if (prep == COMPILER_PREP_BUILD) ++task_count;
        else if (prep == COMPILER_PREP_CACHE) ++cache_hits;
    }
    if (cache_hits) note("CACHED", "%zu object%s", cache_hits, cache_hits == 1 ? "" : "s");
    compiler_execute_tasks(tasks, task_count, opt, "dependency compile failed");
    for (size_t i = 0; i < task_count; ++i) compiler_task_free(&tasks[i]);
    free(tasks);

    StrVec ar = {0};
    vec_push(&ar, compiler_ar()); vec_push(&ar, "rcs"); vec_push(&ar, library);
    for (size_t i = 0; i < objects.count; ++i) vec_push(&ar, objects.items[i]);
    note("AR", "%s", library);
    if (run_process_atomic_output(&ar, opt->verbose, library) != 0) die("dependency archive failed: %s", d->name);
    vec_free(&ar); vec_free(&sources); vec_free(&compile_sources); vec_free(&objects);
}

static void compiler_prepare_target_links(C_Build *b, DepState states[]) {
    for (size_t ti = 0; ti < b->target_count; ++ti) {
        C_Target *t = &b->targets[ti];
        C_StringList ordered = {0};
        for (size_t i = 0; i < t->dep_count; ++i) {
            C_Dependency *d = t->deps[i];
            if (d->kind != C_DEP_SOURCE) continue;
            ptrdiff_t idx = d - b->deps;
            if (idx < 0 || (size_t)idx >= b->dep_count) die("target %s has invalid dependency", t->name);
            char library[PATH_MAX]; compiler_dep_library(d, &states[idx], library); c__push(&ordered, library);
        }
        for (size_t i = 0; i < t->system_links.count; ++i) {
            char arg[C_MAX_NAME + 3]; snprintf(arg, sizeof(arg), "-l%s", t->system_links.items[i]); c__push(&ordered, arg);
        }
#ifdef __APPLE__
        for (size_t i = 0; i < t->frameworks.count; ++i) { c__push(&ordered, "-framework"); c__push(&ordered, t->frameworks.items[i]); }
#endif
        for (size_t i = 0; i < t->ldflags.count; ++i) c__push(&ordered, t->ldflags.items[i]);
        free_c_list(&t->system_links); free_c_list(&t->frameworks); free_c_list(&t->ldflags); t->ldflags = ordered;
    }
    for (size_t i = 0; i < b->dep_count; ++i) if (b->deps[i].kind == C_DEP_SOURCE) b->deps[i].kind = C_DEP_HEADER_ONLY;
}

static void compiler_resolve_all(C_Build *b, const Options *opt, DepState states[]) {
    LockFile lock;
    load_lock(&lock);
    for (size_t i = 0; i < b->dep_count; ++i) {
        C_Dependency *d = &b->deps[i];
        if (d->kind == C_DEP_RESERVED) die("external build-system dependencies are not supported; use c_dep_source()");
        resolve_dependency(d, opt, &lock, &states[i], false);
    }
    save_lock(&lock);

    size_t source_indices[C_MAX_DEPS];
    size_t source_count = 0;
    for (size_t i = 0; i < b->dep_count; ++i) if (b->deps[i].kind == C_DEP_SOURCE) source_indices[source_count++] = i;
    int total_jobs = compiler_adaptive_jobs(compiler_perf.jobs);
    if (source_count <= 1 || total_jobs <= 1) {
        for (size_t i = 0; i < source_count; ++i) {
            size_t idx = source_indices[i];
            compiler_build_dependency(&b->deps[idx], opt, &states[idx]);
        }
    } else {
        size_t worker_limit = source_count < (size_t)total_jobs ? source_count : (size_t)total_jobs;
        pid_t pids[C_MAX_DEPS] = {0};
        size_t dep_for_pid[C_MAX_DEPS] = {0};
        size_t next = 0, running = 0, finished = 0;
        int jobs_per_dep = total_jobs / (int)worker_limit;
        if (jobs_per_dep < 1) jobs_per_dep = 1;
        while (finished < source_count) {
            while (next < source_count && running < worker_limit) {
                size_t idx = source_indices[next];
                pid_t pid = fork();
                if (pid < 0) die("fork dependency build: %s", strerror(errno));
                if (pid == 0) {
                    compiler_perf.jobs = jobs_per_dep;
                    compiler_perf.adaptive_jobs = false;
                    compiler_build_dependency(&b->deps[idx], opt, &states[idx]);
                    fflush(NULL);
                    _exit(0);
                }
                pids[next] = pid;
                dep_for_pid[next] = idx;
                ++next;
                ++running;
            }
            int status = 0;
            pid_t done;
            do { done = waitpid(-1, &status, 0); } while (done < 0 && errno == EINTR);
            if (done < 0) die("waitpid dependency build: %s", strerror(errno));
            size_t slot = source_count;
            for (size_t i = 0; i < next; ++i) if (pids[i] == done) { slot = i; break; }
            if (slot == source_count) continue;
            pids[slot] = 0;
            --running;
            ++finished;
            int rc = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
            if (rc != 0) die("dependency build failed: %s", b->deps[dep_for_pid[slot]].name);
        }
    }
    compiler_prepare_target_links(b, states);
}

static void compiler_append_target_compile_flags(StrVec *a, const C_Target *t, C_Build *b, DepState states[], const char *source) {
    for (size_t i = 0; i < t->includes.count; ++i) { char x[PATH_MAX + 3]; snprintf(x, sizeof(x), "-I%s", t->includes.items[i]); vec_push(a, x); }
    for (size_t i = 0; i < t->defines.count; ++i) { char x[PATH_MAX + 3]; snprintf(x, sizeof(x), "-D%s", t->defines.items[i]); vec_push(a, x); }
    compiler_push_source_flags(a, &t->cflags, source);
    for (size_t i = 0; i < t->dep_count; ++i) {
        C_Dependency *d = t->deps[i];
        ptrdiff_t dep_index = d - b->deps;
        if (dep_index < 0 || (size_t)dep_index >= b->dep_count) die("target %s has invalid dependency", t->name);
        DepState *s = &states[dep_index];
        char root[PATH_MAX];
        if (d->subdir[0]) path_join(root, s->source, d->subdir); else c__copy(root, sizeof(root), s->source);
        if (d->include_dirs.count == 0) { char x[PATH_MAX + 3]; snprintf(x, sizeof(x), "-I%s", root); vec_push(a, x); }
        else for (size_t j = 0; j < d->include_dirs.count; ++j) {
            char inc[PATH_MAX], x[PATH_MAX + 3]; path_join(inc, root, d->include_dirs.items[j]); snprintf(x, sizeof(x), "-I%s", inc); vec_push(a, x);
        }
    }
}

static void compiler_run_generators(C_Target *t, const Options *opt) {
    if (t->generated_outputs.count != t->generated_inputs.count ||
        t->generated_outputs.count != t->generated_commands.count)
        die("target %s has an invalid generated-source description", t->name);
    if (!t->generated_outputs.count) return;

    char state_root[PATH_MAX];
    path_join(state_root, "build", ".generated");
    mkdir_p(state_root);

    for (size_t i = 0; i < t->generated_outputs.count; ++i) {
        const char *output = t->generated_outputs.items[i];
        const char *input = t->generated_inputs.items[i];
        const char *command = t->generated_commands.items[i];
        bool need = !file_exists(output);
        uint64_t input_hash = 0;
        if (input[0] && !compiler_hash_file_uncached(input, &input_hash))
            die("generated input not found or unreadable: %s", input);
        uint64_t desired_state = hash_string(command);
        desired_state = hash_update(desired_state, &input_hash, sizeof(input_hash));
        char desired_hex[17]; hash_u64_hex(desired_state, desired_hex);

        uint64_t h = hash_string(t->name);
        h = hash_update(h, output, strlen(output));
        char key[17]; hash_u64_hex(h, key);
        char stamp[PATH_MAX]; path_join(stamp, state_root, key);
        FILE *sf = fopen(stamp, "r");
        char previous_state[17] = {0};
        char previous_output[17] = {0};
        if (!sf || fscanf(sf, "%16s %16s", previous_state, previous_output) != 2) need = true;
        if (sf) fclose(sf);
        if (strcmp(previous_state, desired_hex)) need = true;
        if (!need) {
            uint64_t output_hash = 0;
            char output_hex[17];
            if (!compiler_hash_file_uncached(output, &output_hash)) need = true;
            else {
                hash_u64_hex(output_hash, output_hex);
                if (strcmp(previous_output, output_hex)) need = true;
            }
        }

        if (!need) {
            if (compiler_perf.explain) note("WHY", "%s generator is fresh", output);
            continue;
        }

        char parent[PATH_MAX]; c__copy(parent, sizeof(parent), output);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = '\0'; if (parent[0]) mkdir_p(parent); }
        note("GEN", "%s", output);
        StrVec a = {0}; vec_push(&a, "sh"); vec_push(&a, "-c"); vec_push(&a, command);
        if (compiler_run_process(&a, opt->verbose) != 0) { vec_free(&a); die("generator failed for %s", output); }
        vec_free(&a);
        if (!file_exists(output)) die("generator did not produce %s", output);

        char temp[PATH_MAX];
        if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", stamp, (long)getpid()) >= (int)sizeof(temp))
            die("generator stamp path too long");
        uint64_t generated_hash = 0;
        if (!compiler_hash_file_uncached(output, &generated_hash)) die("cannot hash generated output: %s", output);
        char generated_hex[17]; hash_u64_hex(generated_hash, generated_hex);
        FILE *out = fopen(temp, "w");
        if (!out) die("cannot write generator state: %s", strerror(errno));
        if (fprintf(out, "%s %s\n", desired_hex, generated_hex) < 0 || fflush(out) != 0 || fsync(fileno(out)) != 0 || fclose(out) != 0) {
            unlink(temp); die("cannot persist generator state");
        }
        if (rename(temp, stamp) != 0) { unlink(temp); die("cannot install generator state: %s", strerror(errno)); }
    }
}

static char *compiler_build_target(C_Build *b, C_Target *t, DepState states[], const Options *opt) {
    StrVec sources = {0}, compile_sources = {0}, objects = {0};
    compiler_run_generators(t, opt);
    expand_sources(t, b, states, &sources);
    if (sources.count == 0) die("target %s has no sources", t->name);
    compiler_profile_reset();
    int unity_mode = t->unity_chunk ? t->unity_chunk : compiler_perf.unity;
    uint64_t sig = target_signature(t, b, states, opt, &sources);
    uint64_t tool = compiler_tool_identity(opt->cc);
    sig = hash_update(sig, &tool, sizeof(tool));
    sig = hash_update(sig, &unity_mode, sizeof(unity_mode));
    sig = hash_update(sig, &compiler_perf.fast_debug, sizeof(compiler_perf.fast_debug));
    sig = hash_update(sig, compiler_perf.linker, strlen(compiler_perf.linker));
    char sighex[17]; hash_u64_hex(sig, sighex);
    char objdir[PATH_MAX]; path_join(objdir, "build/.objs", sighex); mkdir_p(objdir);
    char unity_root[PATH_MAX], unity_dir[PATH_MAX];
    path_join(unity_root, "build", ".unity"); path_join(unity_dir, unity_root, sighex);
    int saved_unity = compiler_perf.unity;
    compiler_perf.unity = unity_mode;
    compiler_make_unity_sources(&sources, unity_dir, "../../../", &compile_sources);
    compiler_perf.unity = saved_unity;

    char cwd[PATH_MAX]; if (!getcwd(cwd, sizeof(cwd))) die("getcwd failed");
    FILE *db = fopen("compile_commands.json", "w"); bool first = true; if (db) fprintf(db, "[\n");
    CompilerTask *tasks = calloc(compile_sources.count, sizeof(*tasks));
    if (!tasks) die("out of memory");
    size_t task_count = 0, cache_hits = 0;

    for (size_t i = 0; i < compile_sources.count; ++i) {
        const char *source = compile_sources.items[i];
        char obj[PATH_MAX], depf[PATH_MAX];
        object_path(objdir, source, obj);
        if (strlen(obj) + 3 >= sizeof(depf)) die("object path too long: %s", obj);
        c__copy(depf, sizeof(depf), obj); strcat(depf, ".d"); vec_push(&objects, obj);

        StrVec a = {0};
        vec_push(&a, opt->cc); compiler_push_standard(&a, source);
        if (opt->release) vec_push(&a, "-O2");
        else if (compiler_perf.fast_debug) { vec_push(&a, "-O1"); vec_push(&a, "-g1"); }
        else { vec_push(&a, "-O0"); vec_push(&a, "-g"); }
        vec_push(&a, "-MMD"); vec_push(&a, "-MF"); vec_push(&a, depf);
        compiler_append_target_compile_flags(&a, t, b, states, source);
        if (t->kind == C_TARGET_SHARED_LIBRARY) vec_push(&a, "-fPIC");
        vec_push(&a, "-c"); vec_push(&a, source); vec_push(&a, "-o"); vec_push(&a, obj);
        if (db) write_compile_command(db, &first, &a, source, cwd);

        CompilerPrepareResult prep = compiler_prepare_task(&tasks[task_count], &a, source, obj, depf);
        if (prep == COMPILER_PREP_BUILD) ++task_count;
        else if (prep == COMPILER_PREP_CACHE) ++cache_hits;
    }
    if (db) { fprintf(db, "\n]\n"); fclose(db); }
    if (cache_hits) note("CACHED", "%zu object%s", cache_hits, cache_hits == 1 ? "" : "s");
    size_t compiled = compiler_execute_tasks(tasks, task_count, opt, "compile failed");
    for (size_t i = 0; i < task_count; ++i) compiler_task_free(&tasks[i]);
    free(tasks);

    char profile_dir[PATH_MAX]; path_join(profile_dir, "build", opt->release ? "release" : "debug"); mkdir_p(profile_dir);
    char *output = malloc(PATH_MAX); if (!output) die("out of memory");
    char outname[C_MAX_NAME + 16];
    if (t->kind == C_TARGET_STATIC_LIBRARY) snprintf(outname, sizeof(outname), "%s.a", t->name);
    else if (t->kind == C_TARGET_SHARED_LIBRARY) {
#ifdef __APPLE__
        snprintf(outname, sizeof(outname), "lib%s.dylib", t->name);
#else
        snprintf(outname, sizeof(outname), "lib%s.so", t->name);
#endif
    } else snprintf(outname, sizeof(outname), "%s", t->name);
    path_join(output, profile_dir, outname);
    bool relink = !file_exists(output) || !ready_marker_matches(output, "artifact"); time_t outt = mtime_of(output);
    double link_ms = 0.0;
    for (size_t i = 0; i < objects.count; ++i) if (mtime_of(objects.items[i]) > outt) relink = true;

    if (relink || compiled || cache_hits) {
        if (t->kind == C_TARGET_STATIC_LIBRARY) {
            StrVec a = {0}; vec_push(&a, compiler_ar()); vec_push(&a, "rcs"); vec_push(&a, output);
            for (size_t i = 0; i < objects.count; ++i) vec_push(&a, objects.items[i]);
            note("AR", "%s", output); double link_started = compiler_perf_now_ms(); if (run_process_atomic_output(&a, opt->verbose, output) != 0) die("archive failed"); link_ms = compiler_perf_now_ms() - link_started; vec_free(&a);
        } else {
            StrVec a = {0}; vec_push(&a, opt->cc); compiler_append_linker(&a);
            if (t->kind == C_TARGET_SHARED_LIBRARY) {
#ifdef __APPLE__
                vec_push(&a, "-dynamiclib");
#else
                vec_push(&a, "-shared");
#endif
            }
            for (size_t i = 0; i < objects.count; ++i) vec_push(&a, objects.items[i]);
            append_link_flags(&a, t, b, states); vec_push(&a, "-o"); vec_push(&a, output);
            note("LINK", "%s", output); double link_started = compiler_perf_now_ms(); if (run_process_atomic_output(&a, opt->verbose, output) != 0) die("link failed"); link_ms = compiler_perf_now_ms() - link_started; vec_free(&a);
        }
    } else note("CACHED", "%s", t->name);

    compiler_profile_report(t->name, link_ms);
    vec_free(&sources); vec_free(&compile_sources); vec_free(&objects); return output;
}

static size_t compiler_target_index(C_Build *b, C_Target *t) {
    if (!b || !t) die("invalid target graph");
    ptrdiff_t idx = t - b->targets;
    if (idx < 0 || (size_t)idx >= b->target_count) die("target dependency does not belong to this build");
    return (size_t)idx;
}

static void compiler_append_target_link_closure(C_Build *b, C_Target *owner, C_Target *dep,
                                                char *graph_outputs[], bool seen[]) {
    size_t idx = compiler_target_index(b, dep);
    if (seen[idx]) return;
    seen[idx] = true;
    if (dep->kind != C_TARGET_STATIC_LIBRARY && dep->kind != C_TARGET_SHARED_LIBRARY)
        die("target %s links non-library target %s", owner->name, dep->name);
    if (!graph_outputs[idx]) die("internal target graph error for %s", dep->name);
    c__push(&owner->ldflags, graph_outputs[idx]);
    for (size_t i = 0; i < dep->target_dep_count; ++i)
        compiler_append_target_link_closure(b, owner, dep->target_deps[i], graph_outputs, seen);
}

static char *compiler_build_target_graph(C_Build *b, C_Target *t, DepState states[], const Options *opt,
                                         unsigned char graph_state[], char *graph_outputs[]) {
    size_t idx = compiler_target_index(b, t);
    if (graph_state[idx] == 1) die("cyclic target dependency involving %s", t->name);
    if (graph_state[idx] == 2) return xstrdup(graph_outputs[idx]);
    graph_state[idx] = 1;
    for (size_t i = 0; i < t->target_dep_count; ++i) {
        char *dep_output = compiler_build_target_graph(b, t->target_deps[i], states, opt, graph_state, graph_outputs);
        free(dep_output);
    }
    bool seen[C_MAX_TARGETS] = {0};
    for (size_t i = 0; i < t->target_dep_count; ++i)
        compiler_append_target_link_closure(b, t, t->target_deps[i], graph_outputs, seen);
    graph_outputs[idx] = compiler_build_target(b, t, states, opt);
    graph_state[idx] = 2;
    return xstrdup(graph_outputs[idx]);
}

static void compiler_free_graph_outputs(C_Build *b, char *graph_outputs[]) {
    for (size_t i = 0; i < b->target_count; ++i) free(graph_outputs[i]);
}

static void compiler_cmd_build_or_run(const Options *opt, bool run) {
    C_Build *b = alloc_build(); load_build(opt, b);
    DepState states[C_MAX_DEPS] = {0}; compiler_resolve_all(b, opt, states);
    unsigned char graph_state[C_MAX_TARGETS] = {0}; char *graph_outputs[C_MAX_TARGETS] = {0};
    C_Target *t = select_target(b, opt); char *output = compiler_build_target_graph(b, t, states, opt, graph_state, graph_outputs);
    if (run) {
        if (t->kind != C_TARGET_EXECUTABLE && t->kind != C_TARGET_TEST) die("target %s is not executable", t->name);
        note("RUN", "%s", output);
        StrVec a = {0}; char exec[PATH_MAX];
        if (output[0] == '/') snprintf(exec, sizeof(exec), "%s", output); else snprintf(exec, sizeof(exec), "./%s", output);
        vec_push(&a, exec); for (int i = 0; i < opt->run_argc; ++i) vec_push(&a, opt->run_argv[i]);
        int rc = compiler_run_process(&a, opt->verbose); vec_free(&a); free(output); compiler_free_graph_outputs(b, graph_outputs); free_build(b); exit(rc);
    }
    free(output); compiler_free_graph_outputs(b, graph_outputs); free_build(b);
}

static void compiler_cmd_test(const Options *opt) {
    C_Build *b = alloc_build(); load_build(opt, b);
    DepState states[C_MAX_DEPS] = {0}; compiler_resolve_all(b, opt, states);
    unsigned char graph_state[C_MAX_TARGETS] = {0}; char *graph_outputs[C_MAX_TARGETS] = {0};
    C_Target *targets[C_MAX_TARGETS] = {0};
    char *outputs[C_MAX_TARGETS] = {0};
    size_t tests = 0;
    for (size_t i = 0; i < b->target_count; ++i) {
        C_Target *t = &b->targets[i];
        if (t->kind != C_TARGET_TEST || (opt->target_name && strcmp(t->name, opt->target_name))) continue;
        targets[tests] = t;
        outputs[tests] = compiler_build_target_graph(b, t, states, opt, graph_state, graph_outputs);
        ++tests;
    }
    if (!tests) die("no test targets defined; use c_test() in build.c");

    pid_t pids[C_MAX_TARGETS] = {0};
    bool active[C_MAX_TARGETS] = {0};
    size_t next = 0, running = 0, finished = 0;
    size_t limit = (size_t)compiler_adaptive_jobs(compiler_perf.jobs);
    if (limit < 1) limit = 1;
    if (limit > tests) limit = tests;
    while (finished < tests) {
        while (next < tests && running < limit) {
            note("TEST", "%s", targets[next]->name);
            StrVec a = {0}; char exec[PATH_MAX];
            snprintf(exec, sizeof(exec), "./%s", outputs[next]);
            vec_push(&a, exec);
            if (compiler_spawn(&a, opt->verbose, &pids[next]) != 0) die("cannot start test: %s", targets[next]->name);
            vec_free(&a);
            active[next] = true;
            ++next; ++running;
        }
        int status = 0; pid_t done;
        do { done = waitpid(-1, &status, 0); } while (done < 0 && errno == EINTR);
        if (done < 0) die("waitpid test: %s", strerror(errno));
        size_t slot = tests;
        for (size_t i = 0; i < tests; ++i) if (active[i] && pids[i] == done) { slot = i; break; }
        if (slot == tests) continue;
        active[slot] = false; --running; ++finished;
        int rc = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
        if (rc != 0) {
            for (size_t i = 0; i < tests; ++i) if (active[i]) kill(pids[i], SIGTERM);
            die("test failed: %s", targets[slot]->name);
        }
    }
    for (size_t i = 0; i < tests; ++i) free(outputs[i]);
    compiler_free_graph_outputs(b, graph_outputs);
    note("PASS", "%zu test target%s", tests, tests == 1 ? "" : "s");
    free_build(b);
}

static void compiler_cmd_doctor(const Options *opt) {
    struct utsname u;
    uname(&u);
    printf("c %s\n\n", C_VERSION);
    printf("Platform   %s %s\n", u.sysname, u.machine);
    printf("Compiler   %s%s\n", opt->cc, command_exists(opt->cc) ? "" : "  [missing]");
    printf("Archiver   %s%s\n", compiler_ar(), command_exists(compiler_ar()) ? "" : "  [missing]");
    printf("Git        %s\n", command_exists("git") ? "ok" : "missing");
    printf("CPUs       %d\n", compiler_cpu_count());
    printf("Jobs       %d%s\n", compiler_perf.jobs,
           compiler_perf.jobs_explicit ? " (explicit)" : " (half CPUs default)");
    printf("Adaptive   %s\n", compiler_perf.adaptive_jobs ? "on" : "off");
    printf("Obj cache  %s\n", compiler_perf.object_cache ? "on" : "off");
    printf("Unity      %s", compiler_perf.unity ? "on" : "off");
    if (compiler_perf.unity == -1) printf(" (auto)");
    else if (compiler_perf.unity > 1) printf(" (chunk %d)", compiler_perf.unity);
    putchar('\n');
    printf("Fast debug %s\n", compiler_perf.fast_debug ? "on" : "off");
    const char *linker = compiler_selected_linker();
    printf("Linker     %s\n",
           linker ? linker : compiler_perf.linker[0] ? compiler_perf.linker : "system default");
    char cache[PATH_MAX];
    cache_root(cache);
    printf("Cache      %s\n", cache);
}

static int compiler_watch_build_once(const Options *opt) {
    pid_t pid = fork();
    if (pid < 0) die("fork watch build: %s", strerror(errno));
    if (pid == 0) {
        compiler_cmd_build_or_run(opt, false);
        fflush(NULL);
        _exit(0);
    }
    return compiler_wait(pid);
}

static void compiler_cmd_watch(const Options *opt) {
    int rc = compiler_watch_build_once(opt);
    uint64_t fingerprint = compiler_watch_fingerprint();
    note("WATCH", "waiting for changes%s", rc == 0 ? "" : " (last build failed)");
    for (;;) {
        compiler_watch_sleep();
        uint64_t next = compiler_watch_fingerprint();
        if (next == fingerprint) continue;
        fingerprint = next;
        note("WATCH", "change detected");
        rc = compiler_watch_build_once(opt);
        note("WATCH", "waiting for changes%s", rc == 0 ? "" : " (last build failed)");
    }
}

static int compiler_dispatch(int argc, char **argv) {
    compiler_perf_defaults();
    int filtered_argc = 0;
    char **filtered_argv = compiler_filter_perf_options(argc, argv, &filtered_argc);
    Options opt = parse_options(filtered_argc, filtered_argv);
    int rc = 0;
    if (!strcmp(opt.command, "build")) compiler_cmd_build_or_run(&opt, false);
    else if (!strcmp(opt.command, "run")) compiler_cmd_build_or_run(&opt, true);
    else if (!strcmp(opt.command, "watch")) compiler_cmd_watch(&opt);
    else if (!strcmp(opt.command, "test")) compiler_cmd_test(&opt);
    else if (!strcmp(opt.command, "doctor")) compiler_cmd_doctor(&opt);
    else rc = c_legacy_main(filtered_argc, filtered_argv);
    free(filtered_argv);
    return rc;
}

static bool cli_is_command(const char *s) {
    static const char *commands[] = {
        "init", "build", "run", "watch", "fetch", "update", "deps", "test",
        "clean", "cache", "doctor", "help", "--help", "-h",
        "version", "--version"
    };
    for (size_t i = 0; i < C_ARRAY_LEN(commands); ++i) if (!strcmp(s, commands[i])) return true;
    return false;
}

static bool cli_is_version_or_help(const char *s) {
    return !strcmp(s, "help") || !strcmp(s, "--help") || !strcmp(s, "-h") || !strcmp(s, "version") || !strcmp(s, "--version");
}

static bool cli_is_action(const char *s) {
    return !strcmp(s, "init") || !strcmp(s, "build") || !strcmp(s, "run") || !strcmp(s, "watch") || !strcmp(s, "fetch") || !strcmp(s, "update") || !strcmp(s, "test") || !strcmp(s, "clean") || !strcmp(s, "cache");
}

static bool cli_jobs_option(const char *arg) {
    return !strcmp(arg, "-j") || !strcmp(arg, "--jobs");
}

static bool cli_perf_option(const char *arg) {
    return (!strncmp(arg, "-j", 2) && arg[2]) || !strncmp(arg, "--jobs=", 7) ||
           !strcmp(arg, "--unity") || !strncmp(arg, "--unity=", 8) || !strcmp(arg, "--no-unity") ||
           !strcmp(arg, "--object-cache") || !strcmp(arg, "--no-object-cache") ||
           !strcmp(arg, "--profile") || !strcmp(arg, "--explain") || !strcmp(arg, "--fast-debug") ||
           !strcmp(arg, "--adaptive-jobs") || !strcmp(arg, "--no-adaptive-jobs") ||
           !strncmp(arg, "--linker=", 9);
}

static bool cli_has_flag(int argc, char **argv, const char *a, const char *b) {
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--")) break;
        if (!strcmp(argv[i], a) || (b && !strcmp(argv[i], b))) return true;
        if ((!strcmp(argv[i], "--cc") || !strcmp(argv[i], "--linker") || cli_jobs_option(argv[i])) && i + 1 < argc) ++i;
    }
    return false;
}

static const char *cli_target(int argc, char **argv) {
    for (int i = 2; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--")) break;
        if (!strcmp(arg, "--release") || !strcmp(arg, "-Drelease") || !strcmp(arg, "-v") || !strcmp(arg, "--verbose") || cli_perf_option(arg)) continue;
        if ((!strcmp(arg, "--cc") || !strcmp(arg, "--linker") || cli_jobs_option(arg)) && i + 1 < argc) { ++i; continue; }
        if (arg[0] != '-') return arg;
    }
    return NULL;
}

static bool cli_color(void) {
    const char *term = getenv("TERM");
    return isatty(STDERR_FILENO) && !getenv("NO_COLOR") && (!term || strcmp(term, "dumb"));
}

static const char *cli_style(bool color, const char *code) { return color ? code : ""; }

static double cli_now_ms(void) {
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static const char *cli_step_name(const char *kind) {
    if (!strcmp(kind, "CONFIG")) return "configure";
    if (!strcmp(kind, "FETCH")) return "fetch";
    if (!strcmp(kind, "UPDATE")) return "update";
    if (!strcmp(kind, "DEP")) return "dependency";
    if (!strcmp(kind, "GEN")) return "generate";
    if (!strcmp(kind, "WHY")) return "reason";
    if (!strcmp(kind, "CC")) return "compile";
    if (!strcmp(kind, "AR")) return "archive";
    if (!strcmp(kind, "LINK")) return "link";
    if (!strcmp(kind, "CACHED")) return "cached";
    if (!strcmp(kind, "RUN")) return "run";
    if (!strcmp(kind, "DONE")) return "done";
    if (!strcmp(kind, "TEST")) return "test";
    if (!strcmp(kind, "PASS")) return "pass";
    if (!strcmp(kind, "WATCH")) return "watch";
    if (!strcmp(kind, "CLEAN")) return "clean";
    if (!strcmp(kind, "INIT")) return "init";
    return NULL;
}

static bool cli_parse_note(const char *line, char kind[8], const char **message) {
    size_t n = strlen(line);
    if (n < 10 || line[0] != ' ' || line[1] != ' ' || line[9] != ' ') return false;
    memcpy(kind, line + 2, 7); kind[7] = '\0';
    for (int i = 6; i >= 0 && kind[i] == ' '; --i) kind[i] = '\0';
    if (!cli_step_name(kind)) return false;
    *message = line + 10; return true;
}

static void cli_heading(const char *command, int argc, char **argv) {
    if (!cli_is_action(command)) return;
    bool color = cli_color(); const char *bold = cli_style(color, "\x1b[1m"); const char *dim = cli_style(color, "\x1b[2m"); const char *reset = cli_style(color, "\x1b[0m");
    const char *target = cli_target(argc, argv);
    fprintf(stderr, "%s%s%s", bold, command, reset);
    if (target && strcmp(command, "cache")) fprintf(stderr, " %s", target);
    if (!strcmp(command, "build") || !strcmp(command, "run") || !strcmp(command, "watch") || !strcmp(command, "test")) fprintf(stderr, " %s[%s]%s", dim, cli_has_flag(argc, argv, "--release", "-Drelease") ? "release" : "debug", reset);
    fputc('\n', stderr);
}

static void cli_step(const char *kind, const char *message) {
    bool color = cli_color(); const char *dim = cli_style(color, "\x1b[2m"); const char *cyan = cli_style(color, "\x1b[36m"); const char *green = cli_style(color, "\x1b[32m"); const char *reset = cli_style(color, "\x1b[0m");
    const char *name = cli_step_name(kind); const char *style = (!strcmp(kind, "CACHED")) ? dim : (!strcmp(kind, "PASS") || !strcmp(kind, "DONE")) ? green : cyan;
    fprintf(stderr, "%s  ├─%s %s%-10s%s %s", dim, reset, style, name, reset, message);
    size_t n = strlen(message); if (!n || message[n - 1] != '\n') fputc('\n', stderr);
}

static void cli_finish(int rc, double elapsed_ms) {
    bool color = cli_color(); const char *dim = cli_style(color, "\x1b[2m"); const char *green = cli_style(color, "\x1b[32m"); const char *red = cli_style(color, "\x1b[31m"); const char *reset = cli_style(color, "\x1b[0m");
    if (rc == 0) fprintf(stderr, "%s  └─%s %ssuccess%s %s%.0f ms%s\n", dim, reset, green, reset, dim, elapsed_ms, reset);
    else fprintf(stderr, "%s  └─%s %sfailed%s %s(exit %d, %.0f ms)%s\n", dim, reset, red, reset, dim, rc, elapsed_ms, reset);
}

static bool cli_quiet_stdout(const char *command, const char *target) {
    if (!strcmp(command, "build") || !strcmp(command, "run") || !strcmp(command, "watch") || !strcmp(command, "fetch") || !strcmp(command, "update") || !strcmp(command, "test") || !strcmp(command, "clean") || !strcmp(command, "init")) return true;
    return !strcmp(command, "cache") && target && !strcmp(target, "clean");
}

static int cli_run_segment(int argc, char **argv) {
    const char *command = argc > 1 ? argv[1] : "help"; const char *target = cli_target(argc, argv);
    bool action = cli_is_action(command); bool verbose = cli_has_flag(argc, argv, "-v", "--verbose"); bool quiet = cli_quiet_stdout(command, target); bool show_program_output = false;
    double started = cli_now_ms(); if (action) cli_heading(command, argc, argv);

    int fds[2]; if (pipe(fds) != 0) { perror("pipe"); return 1; }
    pid_t pid = fork(); if (pid < 0) { perror("fork"); close(fds[0]); close(fds[1]); return 1; }
    if (pid == 0) {
        close(fds[0]); if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127); close(fds[1]); setvbuf(stdout, NULL, _IONBF, 0);
        int rc = compiler_dispatch(argc, argv); fflush(NULL); _exit(rc);
    }

    close(fds[1]); FILE *stream = fdopen(fds[0], "r"); if (!stream) { close(fds[0]); waitpid(pid, NULL, 0); return 1; }
    char *line = NULL; size_t cap = 0;
    while (getline(&line, &cap, stream) >= 0) {
        char kind[8]; const char *message = NULL;
        if (cli_parse_note(line, kind, &message)) { if (action) cli_step(kind, message); else fputs(line, stdout); if (!strcmp(kind, "RUN") || !strcmp(kind, "TEST")) show_program_output = true; continue; }
        if (!quiet || verbose || show_program_output) { fputs(line, stdout); fflush(stdout); }
    }
    free(line); fclose(stream);
    int status = 0; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
    if (action) cli_finish(rc, cli_now_ms() - started);

    if (rc == 0 && cli_is_version_or_help(command) && (!strcmp(command, "help") || !strcmp(command, "--help") || !strcmp(command, "-h"))) {
        fputs("\nchaining:\n  c clean build run\n  c fetch build test\n\nperformance:\n  -j N / -jN / --jobs=N    parallel jobs (default: half available CPUs)\n  --adaptive-jobs          reduce jobs when the machine is already busy\n  --unity[=N|auto]         optional fixed or history-balanced unity chunks\n  --profile                show cache/build timings and slowest files\n  --fast-debug             use -O1 -g1 for faster debug compiles\n  --linker=NAME|auto       select mold/lld/another compiler-driver linker\n  --no-object-cache        disable the persistent global object cache\n\nwatch:\n  c watch [target]         keep the build process warm and rebuild on changes\n\nenvironment:\n  C_JOBS=N                 default parallel job count\n  C_ADAPTIVE_JOBS=1        adaptive load-aware job limiting\n  C_UNITY=N|auto           default unity mode\n  C_OBJECT_CACHE=0         disable global object caching\n  C_PROFILE=1              enable build timing reports\n  C_FAST_DEBUG=1           enable faster debug compilation\n  C_LINKER=NAME|auto       linker selection\n\nCommands run left-to-right and stop on the first failure.\nArguments after `--` belong to `c run` and end the command chain.\n", stdout);
    }
    return rc;
}

static bool cli_cache_clean_argument(int start, int i, char **argv) {
    return i == start + 1 && !strcmp(argv[start], "cache") && !strcmp(argv[i], "clean");
}

int main(int argc, char **argv) {
    if (argc < 2) return cli_run_segment(argc, argv);
    int start = 1; bool first_action = true;
    while (start < argc) {
        int end = argc; bool forwarded_args = false;
        for (int i = start + 1; i < argc; ++i) {
            if (!strcmp(argv[i], "--")) { forwarded_args = true; break; }
            if (cli_is_command(argv[i]) && !cli_cache_clean_argument(start, i, argv)) { end = i; break; }
        }
        if (forwarded_args) end = argc;
        int seg_argc = 1 + (end - start); char **seg_argv = calloc((size_t)seg_argc + 1, sizeof(*seg_argv));
        if (!seg_argv) { fputs("c: error: out of memory\n", stderr); return 1; }
        seg_argv[0] = argv[0]; for (int i = start; i < end; ++i) seg_argv[1 + i - start] = argv[i];
        bool segment_action = seg_argc > 1 && cli_is_action(seg_argv[1]);
        if (segment_action && !first_action) fputc('\n', stderr);
        if (segment_action) first_action = false;
        int rc = cli_run_segment(seg_argc, seg_argv); free(seg_argv); if (rc != 0) return rc;
        if (end >= argc) break;
        start = end;
    }
    return 0;
}
