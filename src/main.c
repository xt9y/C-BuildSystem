#define _POSIX_C_SOURCE 200809L

#include "cbuild.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define C_VERSION "1.0.0"
#define C_ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))

typedef struct StrVec {
    char **items;
    size_t count;
    size_t cap;
} StrVec;

typedef struct LockEntry {
    char name[C_MAX_NAME];
    char url[C_MAX_PATH];
    char requested[C_MAX_NAME];
    char resolved[80];
} LockEntry;

typedef struct LockFile {
    LockEntry entries[C_MAX_DEPS];
    size_t count;
} LockFile;

typedef struct DepState {
    char source[PATH_MAX];
    char package[PATH_MAX];
    char resolved[80];
} DepState;

typedef struct Options {
    const char *command;
    const char *target_name;
    const char *cc;
    bool release;
    bool verbose;
    int run_argc;
    char **run_argv;
} Options;

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "c: error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void note(const char *kind, const char *fmt, ...) {
    va_list ap;
    fprintf(stdout, "  %-7s ", kind);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

static char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) die("out of memory");
    return p;
}

static void vec_push(StrVec *v, const char *s) {
    if (v->count + 1 >= v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->items = xrealloc(v->items, v->cap * sizeof(v->items[0]));
    }
    v->items[v->count++] = xstrdup(s);
    v->items[v->count] = NULL;
}

static void vec_free(StrVec *v) {
    for (size_t i = 0; i < v->count; ++i) free(v->items[i]);
    free(v->items);
    memset(v, 0, sizeof(*v));
}

static uint64_t hash_update(uint64_t h, const void *data, size_t len) {
    const unsigned char *p = data;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t hash_string(const char *s) {
    return hash_update(1469598103934665603ULL, s, strlen(s));
}

static uint64_t hash_file_seed(uint64_t h, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return hash_update(h, path, strlen(path));
    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f))) h = hash_update(h, buf, n);
    fclose(f);
    return h;
}

static void hash_u64_hex(uint64_t h, char out[17]) {
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

static void hash_hex(const char *s, char out[17]) {
    snprintf(out, 17, "%016llx", (unsigned long long)hash_string(s));
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static time_t mtime_of(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mtime;
}

static void path_join(char out[PATH_MAX], const char *a, const char *b) {
    if (!a || !a[0]) snprintf(out, PATH_MAX, "%s", b ? b : "");
    else if (!b || !b[0]) snprintf(out, PATH_MAX, "%s", a);
    else if (a[strlen(a) - 1] == '/') snprintf(out, PATH_MAX, "%s%s", a, b);
    else snprintf(out, PATH_MAX, "%s/%s", a, b);
}

static void mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t n = strlen(tmp);
    if (n == 0) return;
    if (tmp[n - 1] == '/') tmp[n - 1] = '\0';
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) die("mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) die("mkdir %s: %s", tmp, strerror(errno));
}

static int remove_tree(const char *path) {
    struct stat root;
    if (lstat(path, &root) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(root.st_mode) || S_ISLNK(root.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *ent;
    char child[PATH_MAX];
    int rc = 0;
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        path_join(child, path, ent->d_name);
        struct stat st;
        if (lstat(child, &st) != 0) { rc = -1; break; }
        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) rc = remove_tree(child);
        else rc = unlink(child);
        if (rc != 0) break;
    }
    int saved = errno;
    closedir(d);
    errno = saved;
    if (rc == 0) rc = rmdir(path);
    return rc;
}

static bool path_exists_nofollow(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

static bool is_real_dir(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}

static void ready_marker_path(const char *entry, char out[PATH_MAX]) {
    if (snprintf(out, PATH_MAX, "%s.c-ready", entry) >= PATH_MAX) die("cache marker path too long: %s", entry);
}

static bool text_file_equals(const char *path, const char *expected) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[C_MAX_PATH + 256];
    bool ok = fgets(line, sizeof(line), f) != NULL;
    if (ok) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        ok = !strcmp(line, expected);
    }
    if (ok) {
        int ch;
        while ((ch = fgetc(f)) != EOF) if (ch != '\n' && ch != '\r' && ch != ' ' && ch != '\t') { ok = false; break; }
    }
    fclose(f);
    return ok;
}

static void write_text_file_atomic(const char *path, const char *text) {
    char temp[PATH_MAX];
    if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(temp))
        die("atomic file path too long: %s", path);
    FILE *f = fopen(temp, "w");
    if (!f) die("cannot create temporary file %s: %s", temp, strerror(errno));
    bool ok = fprintf(f, "%s\n", text ? text : "") >= 0 && fflush(f) == 0;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) { unlink(temp); die("cannot finish temporary file %s", temp); }
    if (rename(temp, path) != 0) { int saved = errno; unlink(temp); errno = saved; die("cannot publish %s: %s", path, strerror(errno)); }
}

static bool ready_marker_matches(const char *entry, const char *value) {
    char marker[PATH_MAX];
    ready_marker_path(entry, marker);
    return text_file_equals(marker, value);
}

static void write_ready_marker(const char *entry, const char *value) {
    char marker[PATH_MAX];
    ready_marker_path(entry, marker);
    write_text_file_atomic(marker, value);
}

static void remove_ready_marker(const char *entry) {
    char marker[PATH_MAX];
    ready_marker_path(entry, marker);
    if (unlink(marker) != 0 && errno != ENOENT) die("cannot remove cache marker %s: %s", marker, strerror(errno));
}

static void remove_cache_entry(const char *entry) {
    remove_ready_marker(entry);
    if (remove_tree(entry) != 0 && errno != ENOENT) die("cannot remove cache entry %s: %s", entry, strerror(errno));
}

static void make_private_temp_dir(const char *final_path, char temp[PATH_MAX]) {
    if (snprintf(temp, PATH_MAX, "%s.tmp.%ld", final_path, (long)getpid()) >= PATH_MAX)
        die("temporary directory path too long: %s", final_path);
    if (remove_tree(temp) != 0 && errno != ENOENT) die("cannot clear temporary directory %s: %s", temp, strerror(errno));
    if (mkdir(temp, 0700) != 0) die("cannot create temporary directory %s: %s", temp, strerror(errno));
}

static int run_process(StrVec *args, bool verbose, const char *cwd) {
    if (!args->count) return 0;
    if (verbose) {
        fprintf(stderr, "  $ ");
        for (size_t i = 0; i < args->count; ++i) fprintf(stderr, "%s%s", i ? " " : "", args->items[i]);
        fputc('\n', stderr);
    }
    pid_t pid = fork();
    if (pid < 0) die("fork: %s", strerror(errno));
    if (pid == 0) {
        if (cwd && chdir(cwd) != 0) { perror("chdir"); _exit(127); }
        execvp(args->items[0], args->items);
        perror(args->items[0]);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static char *capture_process(StrVec *args, const char *cwd) {
    int fds[2];
    if (pipe(fds) != 0) die("pipe: %s", strerror(errno));
    pid_t pid = fork();
    if (pid < 0) die("fork: %s", strerror(errno));
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        if (cwd && chdir(cwd) != 0) _exit(127);
        execvp(args->items[0], args->items);
        _exit(127);
    }
    close(fds[1]);
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    if (!buf) die("out of memory");
    for (;;) {
        if (len + 128 >= cap) { cap *= 2; buf = xrealloc(buf, cap); }
        ssize_t n = read(fds[0], buf + len, cap - len - 1);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    buf[len] = '\0';
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) buf[--len] = '\0';
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { free(buf); return NULL; }
    return buf;
}

static bool git_mirror_valid(const char *mirror) {
    if (!is_real_dir(mirror)) return false;
    StrVec bare = {0};
    vec_push(&bare, "git"); vec_push(&bare, "--git-dir"); vec_push(&bare, mirror);
    vec_push(&bare, "rev-parse"); vec_push(&bare, "--is-bare-repository");
    char *result = capture_process(&bare, NULL); vec_free(&bare);
    bool ok = result && !strcmp(result, "true");
    free(result);
    if (!ok) return false;
    StrVec fsck = {0};
    vec_push(&fsck, "git"); vec_push(&fsck, "--git-dir"); vec_push(&fsck, mirror);
    vec_push(&fsck, "fsck"); vec_push(&fsck, "--connectivity-only");
    int rc = run_process(&fsck, false, NULL); vec_free(&fsck);
    return rc == 0;
}

static bool git_mirror_has_commit(const char *mirror, const char *resolved) {
    char expr[128];
    if (snprintf(expr, sizeof(expr), "%s^{commit}", resolved) >= (int)sizeof(expr)) return false;
    StrVec a = {0};
    vec_push(&a, "git"); vec_push(&a, "--git-dir"); vec_push(&a, mirror);
    vec_push(&a, "cat-file"); vec_push(&a, "-e"); vec_push(&a, expr);
    int rc = run_process(&a, false, NULL); vec_free(&a);
    return rc == 0;
}

static int git_fetch_mirror(const char *mirror, const Options *opt) {
    StrVec fetch = {0};
    vec_push(&fetch, "git"); vec_push(&fetch, "--git-dir"); vec_push(&fetch, mirror);
    vec_push(&fetch, "fetch"); vec_push(&fetch, "--prune"); vec_push(&fetch, "origin");
    int rc = run_process(&fetch, opt->verbose, NULL); vec_free(&fetch);
    return rc;
}

static int run_process_atomic_output(StrVec *args, bool verbose, const char *output) {
    char temp_dir[PATH_MAX];
    if (snprintf(temp_dir, sizeof(temp_dir), "%s.tmp.XXXXXX", output) >= (int)sizeof(temp_dir)) return 1;
    if (!mkdtemp(temp_dir)) return 1;
    char temp_output[PATH_MAX]; path_join(temp_output, temp_dir, "artifact");
    bool replaced = false;
    for (size_t i = 0; i < args->count; ++i) {
        if (!strcmp(args->items[i], output)) {
            free(args->items[i]);
            args->items[i] = xstrdup(temp_output);
            replaced = true;
        }
    }
    if (!replaced) { remove_tree(temp_dir); errno = EINVAL; return 1; }
    int rc = run_process(args, verbose, NULL);
    if (rc == 0 && rename(temp_output, output) != 0) rc = 1;
    int saved = errno;
    (void)remove_tree(temp_dir);
    errno = saved;
    if (rc == 0) write_ready_marker(output, "artifact");
    return rc;
}

static const char *home_dir(void) {
    const char *h = getenv("HOME");
    if (!h || !*h) die("HOME is not set");
    return h;
}

static void cache_root(char out[PATH_MAX]) {
    const char *override = getenv("C_CACHE_DIR");
    if (override && *override) { snprintf(out, PATH_MAX, "%s", override); return; }
#ifdef __APPLE__
    snprintf(out, PATH_MAX, "%s/Library/Caches/c", home_dir());
#else
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) snprintf(out, PATH_MAX, "%s/c", xdg);
    else snprintf(out, PATH_MAX, "%s/.cache/c", home_dir());
#endif
}

static void copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); die("cannot write %s", to); }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in))) {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); die("write failed: %s", to); }
    }
    fclose(in);
    fclose(out);
}

static bool executable_path(char out[PATH_MAX]) {
#ifdef __APPLE__
    uint32_t size = PATH_MAX;
    char tmp[PATH_MAX];
    if (_NSGetExecutablePath(tmp, &size) != 0) return false;
    return realpath(tmp, out) != NULL;
#else
    ssize_t n = readlink("/proc/self/exe", out, PATH_MAX - 1);
    if (n <= 0) return false;
    out[n] = '\0';
    return true;
#endif
}

static bool bundled_header_path(char out[PATH_MAX]) {
    char exe[PATH_MAX];
    if (!executable_path(exe)) return false;
    char *slash = strrchr(exe, '/');
    if (!slash) return false;
    *slash = '\0';
    char parent[PATH_MAX];
    path_join(parent, exe, "..");
    char inc[PATH_MAX];
    path_join(inc, parent, "include");
    path_join(out, inc, "cbuild.h");
    return file_exists(out);
}

static void write_embedded_header(const char *path) {
    const char *src = getenv("C_INCLUDE_DIR");
    char from[PATH_MAX];
    if (src && *src) {
        path_join(from, src, "cbuild.h");
        if (file_exists(from)) { copy_file(from, path); return; }
    }
    if (bundled_header_path(from)) { copy_file(from, path); return; }
#ifdef CBUILD_HEADER_PATH
    if (file_exists(CBUILD_HEADER_PATH)) { copy_file(CBUILD_HEADER_PATH, path); return; }
#endif
    die("cannot locate cbuild.h; reinstall c or set C_INCLUDE_DIR");
}

static void compile_build_script(const Options *opt, char so_path[PATH_MAX]) {
    if (!file_exists("build.c")) die("build.c not found (run `c init` first)");
    char cache[PATH_MAX], script_dir[PATH_MAX], header[PATH_MAX];
    cache_root(cache);
    path_join(script_dir, cache, "scripts");
    mkdir_p(script_dir);
    path_join(header, script_dir, "cbuild.h");
    write_embedded_header(header);

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) die("getcwd: %s", strerror(errno));
    uint64_t h = 1469598103934665603ULL;
    h = hash_update(h, cwd, strlen(cwd));
    h = hash_update(h, C_VERSION, strlen(C_VERSION));
    h = hash_update(h, opt->cc, strlen(opt->cc));
    h = hash_file_seed(h, "build.c");
    h = hash_file_seed(h, header);
    char key[17];
    hash_u64_hex(h, key);
#ifdef __APPLE__
    char module_name[32]; snprintf(module_name, sizeof(module_name), "%s.dylib", key);
#else
    char module_name[32]; snprintf(module_name, sizeof(module_name), "%s.so", key);
#endif
    path_join(so_path, script_dir, module_name);
    if (file_exists(so_path)) return;

    StrVec a = {0};
    vec_push(&a, opt->cc);
    vec_push(&a, "-std=c11");
    vec_push(&a, "-O2");
#ifdef __APPLE__
    vec_push(&a, "-dynamiclib");
#else
    vec_push(&a, "-shared");
    vec_push(&a, "-fPIC");
#endif
    char inc[PATH_MAX + 3]; snprintf(inc, sizeof(inc), "-I%s", script_dir); vec_push(&a, inc);
    vec_push(&a, "build.c");
    char temp_so[PATH_MAX];
    if (snprintf(temp_so, sizeof(temp_so), "%s.tmp.%ld", so_path, (long)getpid()) >= (int)sizeof(temp_so))
        die("build script cache path too long");
    vec_push(&a, "-o"); vec_push(&a, temp_so);
    note("CONFIG", "build.c");
    int rc = run_process(&a, opt->verbose, NULL);
    vec_free(&a);
    if (rc != 0) {
        unlink(temp_so);
        die("failed to compile build.c");
    }
    if (rename(temp_so, so_path) != 0) {
        unlink(temp_so);
        die("cannot publish compiled build.c: %s", strerror(errno));
    }
}

static C_Build *alloc_build(void) {
    C_Build *b = calloc(1, sizeof(*b));
    if (!b) die("out of memory");
    b->default_target = -1;
    return b;
}

static void free_c_list(C_StringList *list) {
    for (size_t i = 0; i < list->count; ++i) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static void free_build(C_Build *b) {
    if (!b) return;
    for (size_t i = 0; i < b->target_count; ++i) {
        C_Target *t = &b->targets[i];
        free_c_list(&t->sources);
        free_c_list(&t->includes);
        free_c_list(&t->defines);
        free_c_list(&t->cflags);
        free_c_list(&t->ldflags);
        free_c_list(&t->system_links);
        free_c_list(&t->frameworks);
        free_c_list(&t->generated_outputs);
        free_c_list(&t->generated_inputs);
        free_c_list(&t->generated_commands);
    }
    for (size_t i = 0; i < b->dep_count; ++i) {
        C_Dependency *d = &b->deps[i];
        free_c_list(&d->links);
        free_c_list(&d->include_dirs);
        free_c_list(&d->source_patterns);
        free_c_list(&d->cmake_options);
    }
    free(b);
}

static void load_build(const Options *opt, C_Build *b) {
    memset(b, 0, sizeof(*b));
    b->default_target = -1;
    char so[PATH_MAX];
    compile_build_script(opt, so);
    void *h = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *why = dlerror();
        char first_error[512];
        snprintf(first_error, sizeof(first_error), "%s", why ? why : "invalid cached module");
        if (unlink(so) != 0 && errno != ENOENT) die("cannot remove invalid build.c cache entry: %s", strerror(errno));
        compile_build_script(opt, so);
        h = dlopen(so, RTLD_NOW | RTLD_LOCAL);
        if (!h) die("cannot load build.c after cache recovery (previous error: %s): %s", first_error, dlerror());
    }
    void (*fn)(C_Build *) = NULL;
    void *sym = dlsym(h, "build");
    memcpy(&fn, &sym, sizeof(fn));
    if (!fn) die("build.c must export `void build(C_Build *b)`");
    fn(b);
    dlclose(h);
    if (b->target_count == 0 && strcmp(opt->command, "fetch") && strcmp(opt->command, "deps")) die("build.c defines no targets");
}

static void load_lock(LockFile *lock) {
    memset(lock, 0, sizeof(*lock));
    FILE *f = fopen("c.lock", "r");
    if (!f) return;
    char line[4096];
    LockEntry cur = {0};
    bool in = false;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "[[dependency]]", 14)) {
            if (in && lock->count < C_MAX_DEPS) lock->entries[lock->count++] = cur;
            memset(&cur, 0, sizeof(cur)); in = true; continue;
        }
        char key[64], val[C_MAX_PATH];
        if (sscanf(line, "%63[^=] = \"%1023[^\"]\"", key, val) == 2) {
            for (char *p = key + strlen(key) - 1; p >= key && (*p == ' ' || *p == '\t'); --p) *p = '\0';
            if (!strcmp(key, "name")) c__copy(cur.name, sizeof(cur.name), val);
            else if (!strcmp(key, "url")) c__copy(cur.url, sizeof(cur.url), val);
            else if (!strcmp(key, "requested")) c__copy(cur.requested, sizeof(cur.requested), val);
            else if (!strcmp(key, "resolved")) c__copy(cur.resolved, sizeof(cur.resolved), val);
        }
    }
    if (in && lock->count < C_MAX_DEPS) lock->entries[lock->count++] = cur;
    fclose(f);
}

static void save_lock(const LockFile *lock) {
    if (lock->count == 0) return;
    char temp[PATH_MAX];
    if (snprintf(temp, sizeof(temp), "c.lock.tmp.%ld", (long)getpid()) >= (int)sizeof(temp)) die("lockfile temp path too long");
    FILE *f = fopen(temp, "w");
    if (!f) die("cannot write temporary c.lock: %s", strerror(errno));
    fprintf(f, "# Generated by c. Commit this file.\n\n");
    for (size_t i = 0; i < lock->count; ++i) {
        const LockEntry *e = &lock->entries[i];
        fprintf(f, "[[dependency]]\nname = \"%s\"\nurl = \"%s\"\nrequested = \"%s\"\nresolved = \"%s\"\n\n",
                e->name, e->url, e->requested, e->resolved);
    }
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) { unlink(temp); die("cannot finish c.lock"); }
    if (rename(temp, "c.lock") != 0) { int saved = errno; unlink(temp); errno = saved; die("cannot publish c.lock: %s", strerror(errno)); }
}

static LockEntry *lock_for(LockFile *lock, const C_Dependency *d) {
    for (size_t i = 0; i < lock->count; ++i) {
        LockEntry *e = &lock->entries[i];
        if (!strcmp(e->name, d->name) && !strcmp(e->url, d->git) && !strcmp(e->requested, d->ref)) return e;
    }
    return NULL;
}

static void ensure_git_mirror(const C_Dependency *d, const Options *opt, char mirror[PATH_MAX]) {
    char cache[PATH_MAX], repos[PATH_MAX], h[17];
    cache_root(cache); path_join(repos, cache, "git"); mkdir_p(repos); hash_hex(d->git, h);
    char mirror_name[32]; snprintf(mirror_name, sizeof(mirror_name), "%s.git", h);
    path_join(mirror, repos, mirror_name);

    if (path_exists_nofollow(mirror)) {
        if (!is_real_dir(mirror)) {
            note("RECOVER", "%s mirror", d->name);
            remove_cache_entry(mirror);
        } else if (!ready_marker_matches(mirror, d->git)) {
            if (git_mirror_valid(mirror)) write_ready_marker(mirror, d->git);
            else {
                note("RECOVER", "%s mirror", d->name);
                remove_cache_entry(mirror);
            }
        }
    }

    if (!is_real_dir(mirror)) {
        char temp[PATH_MAX]; make_private_temp_dir(mirror, temp);
        note("FETCH", "%s", d->name);
        StrVec a = {0}; vec_push(&a, "git"); vec_push(&a, "clone"); vec_push(&a, "--mirror"); vec_push(&a, d->git); vec_push(&a, temp);
        int rc = run_process(&a, opt->verbose, NULL); vec_free(&a);
        if (rc != 0 || !git_mirror_valid(temp)) {
            (void)remove_tree(temp);
            die("failed to clone %s", d->git);
        }
        if (rename(temp, mirror) != 0) {
            int saved = errno;
            (void)remove_tree(temp);
            if (!is_real_dir(mirror) || !ready_marker_matches(mirror, d->git)) {
                errno = saved;
                die("cannot publish git mirror for %s: %s", d->name, strerror(errno));
            }
        } else {
            write_ready_marker(mirror, d->git);
        }
    }
}

static void resolve_dependency(const C_Dependency *d, const Options *opt, LockFile *lock, DepState *state, bool build_artifacts) {
    char mirror[PATH_MAX];
    ensure_git_mirror(d, opt, mirror);
    LockEntry *e = lock_for(lock, d);
    if (!e) {
        if (git_fetch_mirror(mirror, opt) != 0) die("failed to fetch %s", d->git);

        StrVec rev = {0}; vec_push(&rev, "git"); vec_push(&rev, "--git-dir"); vec_push(&rev, mirror); vec_push(&rev, "rev-parse");
        char refexpr[C_MAX_NAME + 16]; snprintf(refexpr, sizeof(refexpr), "%s^{commit}", d->ref); vec_push(&rev, refexpr);
        char *resolved = capture_process(&rev, NULL); vec_free(&rev);
        if (!resolved) {
            StrVec rev2 = {0}; vec_push(&rev2, "git"); vec_push(&rev2, "--git-dir"); vec_push(&rev2, mirror); vec_push(&rev2, "rev-parse");
            snprintf(refexpr, sizeof(refexpr), "origin/%s^{commit}", d->ref); vec_push(&rev2, refexpr);
            resolved = capture_process(&rev2, NULL); vec_free(&rev2);
        }
        if (!resolved) die("cannot resolve %s at ref %s", d->name, d->ref);
        if (lock->count >= C_MAX_DEPS) die("too many lockfile dependencies");
        e = &lock->entries[lock->count++]; memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", d->name);
        snprintf(e->url, sizeof(e->url), "%s", d->git);
        snprintf(e->requested, sizeof(e->requested), "%s", d->ref);
        snprintf(e->resolved, sizeof(e->resolved), "%s", resolved);
        free(resolved);
    }
    snprintf(state->resolved, sizeof(state->resolved), "%s", e->resolved);
    if (!git_mirror_has_commit(mirror, e->resolved)) {
        note("RECOVER", "%s mirror objects", d->name);
        if (git_fetch_mirror(mirror, opt) != 0 || !git_mirror_has_commit(mirror, e->resolved))
            die("cached mirror for %s does not contain locked commit %s", d->name, e->resolved);
    }

    char cache[PATH_MAX], srcroot[PATH_MAX], pkgroot[PATH_MAX], key_input[C_MAX_PATH + 256], key[17];
    cache_root(cache); path_join(srcroot, cache, "src"); path_join(pkgroot, cache, "pkg"); mkdir_p(srcroot); mkdir_p(pkgroot);
    snprintf(key_input, sizeof(key_input), "%s:%s", d->git, e->resolved);
    hash_hex(key_input, key);
    char src_name[C_MAX_NAME + 32]; snprintf(src_name, sizeof(src_name), "%s-%s", d->name, key);
    path_join(state->source, srcroot, src_name);

    uint64_t artifact_hash = 1469598103934665603ULL;
    artifact_hash = hash_update(artifact_hash, d->git, strlen(d->git));
    artifact_hash = hash_update(artifact_hash, e->resolved, strlen(e->resolved));
    artifact_hash = hash_update(artifact_hash, opt->cc, strlen(opt->cc));
    artifact_hash = hash_update(artifact_hash, &opt->release, sizeof(opt->release));
    artifact_hash = hash_update(artifact_hash, &d->kind, sizeof(d->kind));
    artifact_hash = hash_update(artifact_hash, d->subdir, strlen(d->subdir));
    for (size_t j = 0; j < d->links.count; ++j)
        artifact_hash = hash_update(artifact_hash, d->links.items[j], strlen(d->links.items[j]));
    for (size_t j = 0; j < d->include_dirs.count; ++j)
        artifact_hash = hash_update(artifact_hash, d->include_dirs.items[j], strlen(d->include_dirs.items[j]));
    for (size_t j = 0; j < d->source_patterns.count; ++j)
        artifact_hash = hash_update(artifact_hash, d->source_patterns.items[j], strlen(d->source_patterns.items[j]));
    for (size_t j = 0; j < d->cmake_options.count; ++j)
        artifact_hash = hash_update(artifact_hash, d->cmake_options.items[j], strlen(d->cmake_options.items[j]));
    char pkg_key[17]; hash_u64_hex(artifact_hash, pkg_key);
    char pkg_name[C_MAX_NAME + 32]; snprintf(pkg_name, sizeof(pkg_name), "%s-%s", d->name, pkg_key);
    path_join(state->package, pkgroot, pkg_name);
    if (path_exists_nofollow(state->source) && (!is_real_dir(state->source) || !ready_marker_matches(state->source, e->resolved))) {
        note("RECOVER", "%s checkout", d->name);
        remove_cache_entry(state->source);
    }
    if (!is_real_dir(state->source)) {
        char temp_source[PATH_MAX]; make_private_temp_dir(state->source, temp_source);
        StrVec co = {0}; vec_push(&co, "git"); vec_push(&co, "--git-dir"); vec_push(&co, mirror); vec_push(&co, "--work-tree"); vec_push(&co, temp_source); vec_push(&co, "checkout"); vec_push(&co, "-f"); vec_push(&co, e->resolved); vec_push(&co, "--"); vec_push(&co, ".");
        int rc = run_process(&co, opt->verbose, NULL); vec_free(&co);
        if (rc != 0) {
            (void)remove_tree(temp_source);
            if (!git_mirror_valid(mirror)) remove_ready_marker(mirror);
            die("failed to checkout %s", d->name);
        }
        if (rename(temp_source, state->source) != 0) {
            int saved = errno;
            (void)remove_tree(temp_source);
            if (!is_real_dir(state->source) || !ready_marker_matches(state->source, e->resolved)) {
                errno = saved;
                die("cannot publish checkout for %s: %s", d->name, strerror(errno));
            }
        } else {
            write_ready_marker(state->source, e->resolved);
        }
    }

    if (build_artifacts && d->kind == C_DEP_CMAKE) {
        char stamp[PATH_MAX]; path_join(stamp, state->package, ".c-built");
        bool package_ready = is_real_dir(state->package) && text_file_equals(stamp, e->resolved);
        if (!package_ready) {
            char buildroot[PATH_MAX], bdir[PATH_MAX], src[PATH_MAX];
            path_join(buildroot, cache, "dep-build"); mkdir_p(buildroot);
            char build_name[C_MAX_NAME + 32]; snprintf(build_name, sizeof(build_name), "%s-%s", d->name, pkg_key);
            path_join(bdir, buildroot, build_name);
            snprintf(src, PATH_MAX, "%s%s%s", state->source, d->subdir[0] ? "/" : "", d->subdir);
            if (path_exists_nofollow(bdir) && remove_tree(bdir) != 0) die("cannot clear dependency build directory: %s", bdir);
            if (path_exists_nofollow(state->package)) remove_cache_entry(state->package);
            mkdir_p(bdir); mkdir_p(state->package);
            note("DEP", "%s", d->name);
            StrVec cm = {0}; vec_push(&cm, "cmake"); vec_push(&cm, "-S"); vec_push(&cm, src); vec_push(&cm, "-B"); vec_push(&cm, bdir);
            char type[64]; snprintf(type, sizeof(type), "-DCMAKE_BUILD_TYPE=%s", opt->release ? "Release" : "Debug"); vec_push(&cm, type);
            char compiler[C_MAX_PATH + 64]; snprintf(compiler, sizeof(compiler), "-DCMAKE_C_COMPILER=%s", opt->cc); vec_push(&cm, compiler);
            char prefix[PATH_MAX + 64]; snprintf(prefix, sizeof(prefix), "-DCMAKE_INSTALL_PREFIX=%s", state->package); vec_push(&cm, prefix);
            vec_push(&cm, "-DBUILD_SHARED_LIBS=OFF");
            for (size_t j = 0; j < d->cmake_options.count; ++j) vec_push(&cm, d->cmake_options.items[j]);
            if (run_process(&cm, opt->verbose, NULL) != 0) { vec_free(&cm); (void)remove_tree(bdir); remove_cache_entry(state->package); die("cmake configure failed for %s", d->name); }
            vec_free(&cm);
            StrVec build = {0}; vec_push(&build, "cmake"); vec_push(&build, "--build"); vec_push(&build, bdir); vec_push(&build, "--config"); vec_push(&build, opt->release ? "Release" : "Debug");
            if (run_process(&build, opt->verbose, NULL) != 0) { vec_free(&build); (void)remove_tree(bdir); remove_cache_entry(state->package); die("cmake build failed for %s", d->name); }
            vec_free(&build);
            StrVec install = {0}; vec_push(&install, "cmake"); vec_push(&install, "--install"); vec_push(&install, bdir); vec_push(&install, "--config"); vec_push(&install, opt->release ? "Release" : "Debug");
            if (run_process(&install, opt->verbose, NULL) != 0) { vec_free(&install); (void)remove_tree(bdir); remove_cache_entry(state->package); die("cmake install failed for %s", d->name); }
            vec_free(&install);
            write_text_file_atomic(stamp, e->resolved);
        }
    }

}

static bool depfile_fresh(const char *obj, const char *depfile, const char *src) {
    time_t ot = mtime_of(obj);
    if (!ot || mtime_of(src) > ot || !file_exists(depfile)) return false;
    FILE *f = fopen(depfile, "r"); if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *buf = malloc((size_t)n + 1); if (!buf) { fclose(f); return false; }
    fread(buf, 1, (size_t)n, f); buf[n] = '\0'; fclose(f);
    for (char *p = buf; *p; ++p) if (*p == '\\' && p[1] == '\n') { *p = ' '; p[1] = ' '; }
    char *colon = strchr(buf, ':'); if (!colon) { free(buf); return false; }
    char *save = NULL;
    for (char *tok = strtok_r(colon + 1, " \t\r\n", &save); tok; tok = strtok_r(NULL, " \t\r\n", &save)) {
        if (mtime_of(tok) > ot) { free(buf); return false; }
    }
    free(buf); return true;
}

static void object_path(const char *objdir, const char *src, char out[PATH_MAX]) {
    char key[17];
    hash_hex(src, key);
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    char short_base[96];
    c__copy(short_base, sizeof(short_base), base);
    for (char *p = short_base; *p; ++p) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == ' ') *p = '_';
    }
    char name[160];
    snprintf(name, sizeof(name), "%s-%s.o", short_base, key);
    path_join(out, objdir, name);
}

static void expand_pattern(const char *pattern, StrVec *out) {
    glob_t g = {0};
    int rc = glob(pattern, 0, NULL, &g);
    if (rc == GLOB_NOMATCH) die("source pattern matched nothing: %s", pattern);
    if (rc != 0) die("glob failed: %s", pattern);
    for (size_t j = 0; j < g.gl_pathc; ++j) vec_push(out, g.gl_pathv[j]);
    globfree(&g);
}

static void expand_sources(const C_Target *t, C_Build *b, DepState states[], StrVec *out) {
    for (size_t i = 0; i < t->sources.count; ++i) expand_pattern(t->sources.items[i], out);
    for (size_t i = 0; i < t->dep_count; ++i) {
        C_Dependency *d = t->deps[i];
        if (d->kind != C_DEP_SOURCE) continue;
        ptrdiff_t dep_index = d - b->deps;
        if (dep_index < 0 || (size_t)dep_index >= b->dep_count) die("target %s has invalid dependency", t->name);
        const char *root = states[dep_index].source;
        char base[PATH_MAX];
        if (d->subdir[0]) path_join(base, root, d->subdir);
        else c__copy(base, sizeof(base), root);
        for (size_t j = 0; j < d->source_patterns.count; ++j) {
            char pattern[PATH_MAX];
            path_join(pattern, base, d->source_patterns.items[j]);
            expand_pattern(pattern, out);
        }
    }
}

static uint64_t target_signature(const C_Target *t, C_Build *b, DepState states[], const Options *opt, const StrVec *sources) {
    uint64_t h = 1469598103934665603ULL;
    h = hash_update(h, opt->cc, strlen(opt->cc));
    h = hash_update(h, &opt->release, sizeof(opt->release));
    h = hash_update(h, &t->kind, sizeof(t->kind));
    h = hash_update(h, t->name, strlen(t->name));
    const C_StringList *lists[] = {&t->includes, &t->defines, &t->cflags, &t->ldflags, &t->system_links, &t->frameworks};
    for (size_t l = 0; l < C_ARRAY_LEN(lists); ++l)
        for (size_t i = 0; i < lists[l]->count; ++i)
            h = hash_update(h, lists[l]->items[i], strlen(lists[l]->items[i]));
    for (size_t i = 0; i < sources->count; ++i) h = hash_update(h, sources->items[i], strlen(sources->items[i]));
    for (size_t i = 0; i < t->dep_count; ++i) {
        C_Dependency *d = t->deps[i];
        ptrdiff_t idx = d - b->deps;
        if (idx >= 0 && (size_t)idx < b->dep_count) {
            h = hash_update(h, states[idx].resolved, strlen(states[idx].resolved));
            h = hash_update(h, states[idx].package, strlen(states[idx].package));
        }
    }
    return h;
}

static void json_escape(FILE *f, const char *s) {
    for (; *s; ++s) {
        if (*s == '\\' || *s == '"') fputc('\\', f);
        if (*s == '\n') fputs("\\n", f); else fputc(*s, f);
    }
}

static void write_compile_command(FILE *db, bool *first, StrVec *cmd, const char *src, const char *cwd) {
    if (!*first) fprintf(db, ",\n");
    *first = false;
    fprintf(db, "  {\"directory\":\""); json_escape(db, cwd); fprintf(db, "\",\"file\":\""); json_escape(db, src); fprintf(db, "\",\"arguments\":[");
    for (size_t i = 0; i < cmd->count; ++i) { if (i) fputc(',', db); fputc('"', db); json_escape(db, cmd->items[i]); fputc('"', db); }
    fprintf(db, "]}");
}

static C_Target *select_target(C_Build *b, const Options *opt) {
    if (opt->target_name) {
        for (size_t i = 0; i < b->target_count; ++i) if (!strcmp(b->targets[i].name, opt->target_name)) return &b->targets[i];
        die("unknown target: %s", opt->target_name);
    }
    if (b->default_target >= 0 && (size_t)b->default_target < b->target_count) return &b->targets[b->default_target];
    return &b->targets[0];
}

static void append_target_compile_flags(StrVec *a, const C_Target *t, C_Build *b, DepState states[]) {
    for (size_t i = 0; i < t->includes.count; ++i) { char x[PATH_MAX + 3]; snprintf(x, sizeof(x), "-I%s", t->includes.items[i]); vec_push(a, x); }
    for (size_t i = 0; i < t->defines.count; ++i) { char x[PATH_MAX + 3]; snprintf(x, sizeof(x), "-D%s", t->defines.items[i]); vec_push(a, x); }
    for (size_t i = 0; i < t->cflags.count; ++i) vec_push(a, t->cflags.items[i]);
    for (size_t i = 0; i < t->dep_count; ++i) {
        C_Dependency *d = t->deps[i];
        ptrdiff_t dep_index = d - b->deps;
        if (dep_index < 0 || (size_t)dep_index >= b->dep_count) die("target %s has invalid dependency", t->name);
        DepState *s = &states[dep_index];
        char root[PATH_MAX];
        if (d->kind == C_DEP_CMAKE) path_join(root, s->package, "include");
        else if (d->subdir[0]) path_join(root, s->source, d->subdir);
        else c__copy(root, sizeof(root), s->source);
        if (d->include_dirs.count == 0) {
            char x[PATH_MAX + 3]; snprintf(x, sizeof(x), "-I%s", root); vec_push(a, x);
        } else {
            for (size_t j = 0; j < d->include_dirs.count; ++j) {
                char inc[PATH_MAX]; path_join(inc, root, d->include_dirs.items[j]);
                char x[PATH_MAX + 3]; snprintf(x, sizeof(x), "-I%s", inc); vec_push(a, x);
            }
        }
    }
}

static void append_link_flags(StrVec *a, const C_Target *t, C_Build *b, DepState states[]) {
    for (size_t i = 0; i < t->dep_count; ++i) {
        C_Dependency *d = t->deps[i];
        ptrdiff_t dep_index = d - b->deps;
        if (dep_index < 0 || (size_t)dep_index >= b->dep_count) die("target %s has invalid dependency", t->name);
        DepState *s = &states[dep_index];
        if (d->kind == C_DEP_CMAKE) {
            char lib[PATH_MAX]; path_join(lib, s->package, "lib"); char L[PATH_MAX + 3]; snprintf(L, sizeof(L), "-L%s", lib); vec_push(a, L);
            char lib64[PATH_MAX]; path_join(lib64, s->package, "lib64"); if (is_dir(lib64)) { snprintf(L, sizeof(L), "-L%s", lib64); vec_push(a, L); }
            for (size_t j = 0; j < d->links.count; ++j) { char x[C_MAX_NAME + 3]; snprintf(x, sizeof(x), "-l%s", d->links.items[j]); vec_push(a, x); }
        }
    }
    for (size_t i = 0; i < t->system_links.count; ++i) { char x[C_MAX_NAME + 3]; snprintf(x, sizeof(x), "-l%s", t->system_links.items[i]); vec_push(a, x); }
#ifdef __APPLE__
    for (size_t i = 0; i < t->frameworks.count; ++i) { vec_push(a, "-framework"); vec_push(a, t->frameworks.items[i]); }
#endif
    for (size_t i = 0; i < t->ldflags.count; ++i) vec_push(a, t->ldflags.items[i]);
}

static char *build_target(C_Build *b, C_Target *t, DepState states[], const Options *opt) {
    StrVec sources = {0}, objects = {0};
    expand_sources(t, b, states, &sources);
    if (sources.count == 0) die("target %s has no sources", t->name);
    uint64_t sig = target_signature(t, b, states, opt, &sources);
    char sighex[17]; hash_u64_hex(sig, sighex);
    char objdir[PATH_MAX]; path_join(objdir, "build/.objs", sighex); mkdir_p(objdir);
    char cwd[PATH_MAX]; if (!getcwd(cwd, sizeof(cwd))) die("getcwd failed");
    FILE *db = fopen("compile_commands.json", "w"); bool first = true; if (db) fprintf(db, "[\n");

    size_t compiled = 0;
    for (size_t i = 0; i < sources.count; ++i) {
        char obj[PATH_MAX], depf[PATH_MAX];
        object_path(objdir, sources.items[i], obj);
        if (strlen(obj) + 3 >= sizeof(depf)) die("object path too long: %s", obj);
        c__copy(depf, sizeof(depf), obj);
        strcat(depf, ".d");
        vec_push(&objects, obj);
        StrVec a = {0}; vec_push(&a, opt->cc); vec_push(&a, "-std=c11"); vec_push(&a, opt->release ? "-O2" : "-O0"); if (!opt->release) vec_push(&a, "-g");
        vec_push(&a, "-MMD"); vec_push(&a, "-MF"); vec_push(&a, depf); append_target_compile_flags(&a, t, b, states); vec_push(&a, "-c"); vec_push(&a, sources.items[i]); vec_push(&a, "-o"); vec_push(&a, obj);
        if (db) write_compile_command(db, &first, &a, sources.items[i], cwd);
        if (!depfile_fresh(obj, depf, sources.items[i])) { note("CC", "%s", sources.items[i]); if (run_process(&a, opt->verbose, NULL) != 0) die("compile failed"); compiled++; }
        vec_free(&a);
    }
    if (db) { fprintf(db, "\n]\n"); fclose(db); }

    char profile_dir[PATH_MAX]; path_join(profile_dir, "build", opt->release ? "release" : "debug"); mkdir_p(profile_dir);
    char *output = malloc(PATH_MAX); if (!output) die("out of memory");
    char outname[C_MAX_NAME + 4]; snprintf(outname, sizeof(outname), "%s%s", t->name, t->kind == C_TARGET_STATIC_LIBRARY ? ".a" : "");
    path_join(output, profile_dir, outname);
    bool relink = !file_exists(output) || !ready_marker_matches(output, "artifact");
    time_t outt = mtime_of(output);
    for (size_t i = 0; i < objects.count; ++i) if (mtime_of(objects.items[i]) > outt) relink = true;

    if (relink || compiled) {
        if (t->kind == C_TARGET_STATIC_LIBRARY) {
            StrVec a = {0}; vec_push(&a, "ar"); vec_push(&a, "rcs"); vec_push(&a, output); for (size_t i = 0; i < objects.count; ++i) vec_push(&a, objects.items[i]);
            note("AR", "%s", output); if (run_process_atomic_output(&a, opt->verbose, output) != 0) die("archive failed"); vec_free(&a);
        } else {
            StrVec a = {0}; vec_push(&a, opt->cc); for (size_t i = 0; i < objects.count; ++i) vec_push(&a, objects.items[i]); append_link_flags(&a, t, b, states); vec_push(&a, "-o"); vec_push(&a, output);
            note("LINK", "%s", output); if (run_process_atomic_output(&a, opt->verbose, output) != 0) die("link failed"); vec_free(&a);
        }
    } else note("CACHED", "%s", t->name);
    vec_free(&sources); vec_free(&objects); return output;
}

static void resolve_all(C_Build *b, const Options *opt, DepState states[], bool build_artifacts) {
    LockFile lock; load_lock(&lock);
    for (size_t i = 0; i < b->dep_count; ++i) resolve_dependency(&b->deps[i], opt, &lock, &states[i], build_artifacts);
    save_lock(&lock);
}

static void cmd_build_or_run(const Options *opt, bool run) {
    C_Build *b = alloc_build();
    load_build(opt, b);
    DepState states[C_MAX_DEPS] = {0};
    resolve_all(b, opt, states, true);
    C_Target *t = select_target(b, opt);
    char *output = build_target(b, t, states, opt);
    if (run) {
        if (t->kind != C_TARGET_EXECUTABLE && t->kind != C_TARGET_TEST) die("target %s is not executable", t->name);
        note("RUN", "%s", output);
        StrVec a = {0};
        char exec[PATH_MAX];
        if (output[0] == '/') snprintf(exec, sizeof(exec), "%s", output);
        else snprintf(exec, sizeof(exec), "./%s", output);
        vec_push(&a, exec);
        for (int i = 0; i < opt->run_argc; ++i) vec_push(&a, opt->run_argv[i]);
        int rc = run_process(&a, opt->verbose, NULL);
        vec_free(&a);
        free(output);
        free_build(b);
        exit(rc);
    }
    free(output);
    free_build(b);
}

static void cmd_fetch(const Options *opt) {
    C_Build *b = alloc_build();
    load_build(opt, b);
    DepState states[C_MAX_DEPS] = {0};
    resolve_all(b, opt, states, false);
    note("DONE", "%zu dependencies ready", b->dep_count);
    free_build(b);
}

static const char *dep_kind_name(C_DepKind kind) {
    return kind == C_DEP_CMAKE ? "cmake" : kind == C_DEP_SOURCE ? "source" : "header";
}

static void cmd_deps(const Options *opt) {
    C_Build *b = alloc_build();
    load_build(opt, b);
    if (opt->target_name && !strcmp(opt->target_name, "clean")) {
        char cache[PATH_MAX], path[PATH_MAX];
        cache_root(cache);
        static const char *dirs[] = {"git", "src", "pkg", "dep-build"};
        for (size_t i = 0; i < C_ARRAY_LEN(dirs); ++i) {
            path_join(path, cache, dirs[i]);
            if (is_dir(path) && remove_tree(path) != 0) die("cannot remove dependency cache: %s", path);
        }
        note("CLEAN", "dependency cache");
        free_build(b);
        return;
    }
    if (opt->target_name && strcmp(opt->target_name, "tree"))
        die("unknown deps action: %s (expected tree or clean)", opt->target_name);
    if (opt->target_name && !strcmp(opt->target_name, "tree")) {
        puts("Targets:");
        for (size_t i = 0; i < b->target_count; ++i) {
            C_Target *t = &b->targets[i];
            printf("  %s\n", t->name);
            for (size_t j = 0; j < t->target_dep_count; ++j)
                printf("    -> target %s\n", t->target_deps[j]->name);
            for (size_t j = 0; j < t->dep_count; ++j)
                printf("    -> dependency %s [%s]\n", t->deps[j]->name, dep_kind_name(t->deps[j]->kind));
        }
        if (b->dep_count) {
            puts("Dependencies:");
            for (size_t i = 0; i < b->dep_count; ++i)
                printf("  %s  %s  %s  [%s]\n", b->deps[i].name, b->deps[i].git, b->deps[i].ref, dep_kind_name(b->deps[i].kind));
        }
        free_build(b);
        return;
    }
    if (!b->dep_count) {
        puts("No dependencies.");
        free_build(b);
        return;
    }
    for (size_t i = 0; i < b->dep_count; ++i)
        printf("%s  %s  %s  [%s]\n", b->deps[i].name, b->deps[i].git, b->deps[i].ref, dep_kind_name(b->deps[i].kind));
    free_build(b);
}

static void cmd_update(const Options *opt) {
    C_Build *b = alloc_build();
    load_build(opt, b);
    LockFile lock;
    load_lock(&lock);
    const char *wanted = opt->target_name;
    size_t out = 0;
    bool matched = false;
    for (size_t i = 0; i < lock.count; ++i) {
        if (!wanted || !strcmp(lock.entries[i].name, wanted)) { matched = true; continue; }
        lock.entries[out++] = lock.entries[i];
    }
    lock.count = out;
    if (wanted && !matched) die("dependency not found in c.lock: %s", wanted);
    if (lock.count) save_lock(&lock);
    else unlink("c.lock");
    DepState states[C_MAX_DEPS] = {0};
    resolve_all(b, opt, states, false);
    note("UPDATE", "%s", wanted ? wanted : "all dependencies");
    free_build(b);
}

static void cmd_test(const Options *opt) {
    C_Build *b = alloc_build();
    load_build(opt, b);
    DepState states[C_MAX_DEPS] = {0};
    resolve_all(b, opt, states, true);
    size_t tests = 0;
    for (size_t i = 0; i < b->target_count; ++i) {
        C_Target *t = &b->targets[i];
        if (t->kind != C_TARGET_TEST) continue;
        if (opt->target_name && strcmp(t->name, opt->target_name)) continue;
        ++tests;
        char *output = build_target(b, t, states, opt);
        note("TEST", "%s", t->name);
        StrVec a = {0};
        char exec[PATH_MAX];
        snprintf(exec, sizeof(exec), "./%s", output);
        vec_push(&a, exec);
        int rc = run_process(&a, opt->verbose, NULL);
        vec_free(&a);
        free(output);
        if (rc != 0) die("test failed: %s", t->name);
    }
    if (!tests) die("no test targets defined; use c_test() in build.c");
    note("PASS", "%zu test target%s", tests, tests == 1 ? "" : "s");
    free_build(b);
}

static void cache_stats_walk(const char *path, unsigned long long *files, unsigned long long *bytes) {
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char child[PATH_MAX]; path_join(child, path, ent->d_name);
        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) cache_stats_walk(child, files, bytes);
        else if (S_ISREG(st.st_mode)) { ++*files; *bytes += (unsigned long long)st.st_size; }
    }
    closedir(d);
}

static void cmd_cache(const Options *opt) {
    char cache[PATH_MAX];
    cache_root(cache);
    if (opt->target_name && !strcmp(opt->target_name, "clean")) {
        if (is_dir(cache) && remove_tree(cache) != 0) die("cannot remove cache: %s", cache);
        note("CLEAN", "%s", cache);
        return;
    }
    if (opt->target_name && !strcmp(opt->target_name, "stats")) {
        unsigned long long files = 0, bytes = 0;
        if (is_dir(cache)) cache_stats_walk(cache, &files, &bytes);
        printf("Path   %s\nFiles  %llu\nBytes  %llu\n", cache, files, bytes);
        return;
    }
    if (opt->target_name) die("unknown cache action: %s (expected stats or clean)", opt->target_name);
    printf("%s\n", cache);
}

static void cmd_clean(void) {
    if (is_dir("build") && remove_tree("build") != 0) die("cannot remove build directory");
    if (file_exists("compile_commands.json")) unlink("compile_commands.json");
    note("CLEAN", "build");
}

static void write_text(const char *path, const char *text) {
    char dir[PATH_MAX]; snprintf(dir, sizeof(dir), "%s", path); char *slash = strrchr(dir, '/'); if (slash) { *slash = '\0'; mkdir_p(dir); }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644); if (fd < 0) die("cannot create %s: %s", path, strerror(errno));
    size_t n = strlen(text); if (write(fd, text, n) != (ssize_t)n) die("write failed: %s", path); close(fd);
}

static void cmd_init(void) {
    if (file_exists("build.c")) die("build.c already exists");
    write_text("build.c",
        "#include <cbuild.h>\n\n"
        "void build(C_Build *b) {\n"
        "    C_Target *app = c_executable(b, \"app\");\n"
        "    c_sources(app, \"src/*.c\");\n"
        "}\n");
    if (!file_exists("src/main.c")) write_text("src/main.c", "#include <stdio.h>\n\nint main(void) {\n    puts(\"Hello from C.\");\n    return 0;\n}\n");
    if (!file_exists(".gitignore")) write_text(".gitignore", "build/\ncompile_commands.json\n");
    note("INIT", "build.c + src/main.c");
}

static bool command_exists(const char *cmd) {
    StrVec a = {0}; vec_push(&a, "sh"); vec_push(&a, "-c"); char q[512]; snprintf(q, sizeof(q), "command -v %s >/dev/null 2>&1", cmd); vec_push(&a, q); int rc = run_process(&a, false, NULL); vec_free(&a); return rc == 0;
}

static void cmd_doctor(const Options *opt) {
    struct utsname u; uname(&u);
    printf("c %s\n\n", C_VERSION);
    printf("Platform   %s %s\n", u.sysname, u.machine);
    printf("Compiler   %s%s\n", opt->cc, command_exists(opt->cc) ? "" : "  [missing]");
    printf("Git        %s\n", command_exists("git") ? "ok" : "missing");
    printf("CMake      %s (only needed for CMake dependencies)\n", command_exists("cmake") ? "ok" : "missing");
    char cache[PATH_MAX]; cache_root(cache); printf("Cache      %s\n", cache);
}

static void usage(void) {
    puts("c - a build system and dependency manager for C\n\n"
         "usage:\n"
         "  c init\n"
         "  c build [target] [--release] [-v]\n"
         "  c run [target] [--release] [-v] [-- args...]\n"
         "  c fetch\n"
         "  c update [dependency]\n"
         "  c deps [tree|clean]\n"
         "  c test [target]\n"
         "  c clean\n"
         "  c cache [stats|clean]\n"
         "  c doctor\n"
         "  c --version\n\n"
         "environment:\n"
         "  CC              C compiler (default: cc)\n"
         "  C_CACHE_DIR     override global cache directory\n"
         "  C_INCLUDE_DIR   directory containing cbuild.h\n");
}

static Options parse_options(int argc, char **argv) {
    Options o = {0}; o.cc = getenv("CC"); if (!o.cc || !*o.cc) o.cc = "cc";
    if (argc < 2) { o.command = "help"; return o; }
    o.command = argv[1];
    bool args = false;
    for (int i = 2; i < argc; ++i) {
        if (args) { if (!o.run_argv) { o.run_argv = &argv[i]; o.run_argc = argc - i; } break; }
        if (!strcmp(argv[i], "--")) { args = true; continue; }
        if (!strcmp(argv[i], "--release") || !strcmp(argv[i], "-Drelease")) o.release = true;
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) o.verbose = true;
        else if (!strcmp(argv[i], "--cc") && i + 1 < argc) o.cc = argv[++i];
        else if (argv[i][0] != '-' && !o.target_name) o.target_name = argv[i];
        else die("unknown option: %s", argv[i]);
    }
    return o;
}

int main(int argc, char **argv) {
    Options opt = parse_options(argc, argv);
    if (!strcmp(opt.command, "--version") || !strcmp(opt.command, "version")) { puts(C_VERSION); return 0; }
    if (!strcmp(opt.command, "help") || !strcmp(opt.command, "--help") || !strcmp(opt.command, "-h")) { usage(); return 0; }
    if (!strcmp(opt.command, "init")) cmd_init();
    else if (!strcmp(opt.command, "build")) cmd_build_or_run(&opt, false);
    else if (!strcmp(opt.command, "run")) cmd_build_or_run(&opt, true);
    else if (!strcmp(opt.command, "fetch")) cmd_fetch(&opt);
    else if (!strcmp(opt.command, "update")) cmd_update(&opt);
    else if (!strcmp(opt.command, "deps")) cmd_deps(&opt);
    else if (!strcmp(opt.command, "test")) cmd_test(&opt);
    else if (!strcmp(opt.command, "clean")) cmd_clean();
    else if (!strcmp(opt.command, "cache")) cmd_cache(&opt);
    else if (!strcmp(opt.command, "doctor")) cmd_doctor(&opt);
    else die("unknown command: %s (try `c help`)", opt.command);
    return 0;
}
