#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "cbuild.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

typedef struct Options {
    const char *command;
    const char *target;
    const char *cc;
    bool release;
    bool verbose;
    int jobs;
    int run_argc;
    char **run_argv;
} Options;

typedef struct LockEntry {
    char name[C_MAX_NAME];
    char url[C_MAX_PATH];
    char requested[C_MAX_NAME];
    char resolved[96];
} LockEntry;

typedef struct LockFile {
    LockEntry entries[C_MAX_DEPS];
    size_t count;
} LockFile;

typedef struct DepState {
    char source[PATH_MAX];
    char package[PATH_MAX];
    char artifact[PATH_MAX];
    char resolved[96];
} DepState;

typedef struct Proc {
    HANDLE handle;
    DWORD pid;
} Proc;

static void die(const char *fmt, ...) {
    va_list ap;
    fputs("c: error: ", stderr);
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
    fflush(stdout);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) die("out of memory");
    memcpy(p, s, n);
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

static void slashify(char *s) {
    for (; *s; ++s) if (*s == '\\') *s = '/';
}

static bool is_absolute(const char *p) {
    return p && ((isalpha((unsigned char)p[0]) && p[1] == ':') || (p[0] == '/' && p[1] == '/'));
}

static void path_join(char out[PATH_MAX], const char *a, const char *b) {
    if (!a || !*a) snprintf(out, PATH_MAX, "%s", b ? b : "");
    else if (!b || !*b) snprintf(out, PATH_MAX, "%s", a);
    else if (is_absolute(b)) snprintf(out, PATH_MAX, "%s", b);
    else if (a[strlen(a) - 1] == '/' || a[strlen(a) - 1] == '\\') snprintf(out, PATH_MAX, "%s%s", a, b);
    else snprintf(out, PATH_MAX, "%s/%s", a, b);
    slashify(out);
}

static void path_dirname(char path[PATH_MAX]) {
    slashify(path);
    char *p = strrchr(path, '/');
    if (p) *p = '\0';
    else strcpy(path, ".");
}

static const char *path_basename(const char *path) {
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    const char *p = a;
    if (!p || (b && b > p)) p = b;
    return p ? p + 1 : path;
}

static bool file_exists(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool is_dir(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static uint64_t file_mtime(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return 0;
    ULARGE_INTEGER t;
    t.LowPart = d.ftLastWriteTime.dwLowDateTime;
    t.HighPart = d.ftLastWriteTime.dwHighDateTime;
    return t.QuadPart;
}

static void mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    slashify(tmp);
    size_t n = strlen(tmp);
    if (!n) return;
    while (n && tmp[n - 1] == '/') tmp[--n] = '\0';
    char *start = tmp;
    if (isalpha((unsigned char)tmp[0]) && tmp[1] == ':') start = tmp + 3;
    for (char *p = start; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (*tmp && !CreateDirectoryA(tmp, NULL)) {
            DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS) die("cannot create directory %s (win32 %lu)", tmp, (unsigned long)e);
        }
        *p = '/';
    }
    if (!CreateDirectoryA(tmp, NULL)) {
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) die("cannot create directory %s (win32 %lu)", tmp, (unsigned long)e);
    }
}

static int remove_tree(const char *path) {
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? 0 : -1;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        return DeleteFileA(path) ? 0 : -1;
    }
    char pattern[PATH_MAX];
    path_join(pattern, path, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char child[PATH_MAX];
            path_join(child, path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (remove_tree(child) != 0) { FindClose(h); return -1; }
            } else {
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileA(child)) { FindClose(h); return -1; }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryA(path) ? 0 : -1;
}

static void ensure_parent(const char *path) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    path_dirname(dir);
    if (strcmp(dir, ".")) mkdir_p(dir);
}

static bool move_replace(const char *from, const char *to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

static void copy_file_or_die(const char *from, const char *to) {
    ensure_parent(to);
    if (!CopyFileA(from, to, FALSE)) die("cannot copy %s to %s (win32 %lu)", from, to, (unsigned long)GetLastError());
}

static uint64_t hash_update(uint64_t h, const void *data, size_t len) {
    const unsigned char *p = data;
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static uint64_t hash_string(const char *s) { return hash_update(1469598103934665603ULL, s, strlen(s)); }

static uint64_t hash_file_seed(uint64_t h, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return hash_update(h, path, strlen(path));
    unsigned char buf[32768];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f))) h = hash_update(h, buf, n);
    fclose(f);
    return h;
}

static void hash_hex(uint64_t h, char out[17]) { snprintf(out, 17, "%016llx", (unsigned long long)h); }

static bool executable_path(char out[PATH_MAX]) {
    DWORD n = GetModuleFileNameA(NULL, out, PATH_MAX);
    if (!n || n >= PATH_MAX) return false;
    slashify(out);
    return true;
}

static const char *local_appdata(void) {
    const char *p = getenv("LOCALAPPDATA");
    if (p && *p) return p;
    p = getenv("USERPROFILE");
    if (p && *p) return p;
    return ".";
}

static void cache_root(char out[PATH_MAX]) {
    const char *p = getenv("C_CACHE_DIR");
    if (p && *p) { snprintf(out, PATH_MAX, "%s", p); slashify(out); return; }
    snprintf(out, PATH_MAX, "%s/C-BuildSystem/cache", local_appdata());
    slashify(out);
}

static void quote_arg(StrVec *parts, const char *arg) {
    bool need = !*arg || strpbrk(arg, " \t\n\v\"") != NULL;
    if (!need) { vec_push(parts, arg); return; }
    size_t cap = strlen(arg) * 2 + 3;
    char *q = malloc(cap);
    if (!q) die("out of memory");
    size_t n = 0;
    q[n++] = '"';
    size_t slashes = 0;
    for (const char *p = arg; ; ++p) {
        char c = *p;
        if (c == '\\') { ++slashes; continue; }
        if (c == '"') {
            while (slashes--) q[n++] = '\\', q[n++] = '\\';
            q[n++] = '\\'; q[n++] = '"';
            slashes = 0;
            continue;
        }
        if (!c) {
            while (slashes--) q[n++] = '\\', q[n++] = '\\';
            break;
        }
        while (slashes--) q[n++] = '\\';
        slashes = 0;
        q[n++] = c;
    }
    q[n++] = '"'; q[n] = '\0';
    vec_push(parts, q);
    free(q);
}

static char *command_line(char *const argv[]) {
    StrVec q = {0};
    size_t total = 1;
    for (size_t i = 0; argv[i]; ++i) { quote_arg(&q, argv[i]); total += strlen(q.items[i]) + 1; }
    char *line = calloc(total, 1);
    if (!line) die("out of memory");
    for (size_t i = 0; i < q.count; ++i) {
        if (i) strcat(line, " ");
        strcat(line, q.items[i]);
    }
    vec_free(&q);
    return line;
}

static void print_command(char *const argv[]) {
    fputs("  $", stderr);
    for (size_t i = 0; argv[i]; ++i) fprintf(stderr, " %s", argv[i]);
    fputc('\n', stderr);
}

static bool spawn_process(char *const argv[], const char *cwd, HANDLE stdout_handle, bool verbose, Proc *out) {
    if (verbose) print_command(argv);
    char *line = command_line(argv);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    BOOL inherit = FALSE;
    if (stdout_handle) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = stdout_handle;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        inherit = TRUE;
    }
    BOOL ok = CreateProcessA(NULL, line, NULL, NULL, inherit, 0, NULL, cwd && *cwd ? cwd : NULL, &si, &pi);
    free(line);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    out->handle = pi.hProcess;
    out->pid = pi.dwProcessId;
    return true;
}

static int wait_process(Proc *p) {
    if (!p->handle) return 1;
    WaitForSingleObject(p->handle, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(p->handle, &code);
    CloseHandle(p->handle);
    p->handle = NULL;
    return (int)code;
}

static int run_argv(char *const argv[], bool verbose, const char *cwd) {
    Proc p = {0};
    if (!spawn_process(argv, cwd, NULL, verbose, &p)) {
        fprintf(stderr, "c: error: cannot start %s (win32 %lu)\n", argv[0], (unsigned long)GetLastError());
        return 127;
    }
    return wait_process(&p);
}

static int run_vec(StrVec *v, bool verbose, const char *cwd) {
    if (!v->count) return 0;
    return run_argv(v->items, verbose, cwd);
}

static char *capture_argv(char *const argv[], const char *cwd) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE readh = NULL, writeh = NULL;
    if (!CreatePipe(&readh, &writeh, &sa, 0)) return NULL;
    SetHandleInformation(readh, HANDLE_FLAG_INHERIT, 0);
    Proc p = {0};
    if (!spawn_process(argv, cwd, writeh, false, &p)) { CloseHandle(readh); CloseHandle(writeh); return NULL; }
    CloseHandle(writeh);
    size_t cap = 512, len = 0;
    char *buf = malloc(cap);
    if (!buf) die("out of memory");
    for (;;) {
        char tmp[4096]; DWORD got = 0;
        if (!ReadFile(readh, tmp, sizeof(tmp), &got, NULL) || got == 0) break;
        if (len + got + 1 > cap) { while (len + got + 1 > cap) cap *= 2; buf = xrealloc(buf, cap); }
        memcpy(buf + len, tmp, got); len += got;
    }
    CloseHandle(readh);
    int rc = wait_process(&p);
    if (rc != 0) { free(buf); return NULL; }
    buf[len] = '\0';
    while (len && isspace((unsigned char)buf[len - 1])) buf[--len] = '\0';
    return buf;
}

static bool command_exists(const char *name) {
    char found[PATH_MAX];
    DWORD n = SearchPathA(NULL, name, NULL, PATH_MAX, found, NULL);
    if (n > 0 && n < PATH_MAX) return true;
    n = SearchPathA(NULL, name, ".exe", PATH_MAX, found, NULL);
    return n > 0 && n < PATH_MAX;
}

static int cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
    return n > 0 ? n : 1;
}

static int default_jobs(void) {
    int n = cpu_count() / 2;
    return n > 0 ? n : 1;
}

static bool wildcard(const char *p) { return p && (strchr(p, '*') || strchr(p, '?')); }

static void expand_pattern(const char *pattern, StrVec *out) {
    if (!wildcard(pattern)) {
        if (!file_exists(pattern)) die("source not found: %s", pattern);
        vec_push(out, pattern);
        return;
    }
    char normalized[PATH_MAX];
    snprintf(normalized, sizeof(normalized), "%s", pattern); slashify(normalized);
    char dir[PATH_MAX]; snprintf(dir, sizeof(dir), "%s", normalized); path_dirname(dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(normalized, &fd);
    if (h == INVALID_HANDLE_VALUE) die("source pattern matched nothing: %s", pattern);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char p[PATH_MAX]; path_join(p, dir, fd.cFileName); vec_push(out, p);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void write_file(const char *path, const char *text, bool exclusive) {
    ensure_parent(path);
    DWORD creation = exclusive ? CREATE_NEW : CREATE_ALWAYS;
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) die("cannot create %s (win32 %lu)", path, (unsigned long)GetLastError());
    DWORD n = (DWORD)strlen(text), wrote = 0;
    if (!WriteFile(h, text, n, &wrote, NULL) || wrote != n) { CloseHandle(h); die("cannot write %s", path); }
    FlushFileBuffers(h); CloseHandle(h);
}

static void atomic_write(const char *path, const char *text) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%lu", path, (unsigned long)GetCurrentProcessId());
    write_file(tmp, text, false);
    if (!move_replace(tmp, path)) { DeleteFileA(tmp); die("cannot publish %s", path); }
}

static bool bundled_header(char out[PATH_MAX]) {
    char exe[PATH_MAX];
    if (!executable_path(exe)) return false;
    char dir[PATH_MAX]; snprintf(dir, sizeof(dir), "%s", exe); path_dirname(dir);
    char candidate[PATH_MAX];
    path_join(candidate, dir, "../include/cbuild.h");
    if (file_exists(candidate)) { snprintf(out, PATH_MAX, "%s", candidate); return true; }
    path_join(candidate, dir, "../../include/cbuild.h");
    if (file_exists(candidate)) { snprintf(out, PATH_MAX, "%s", candidate); return true; }
#ifdef CBUILD_HEADER_PATH
    if (file_exists(CBUILD_HEADER_PATH)) { snprintf(out, PATH_MAX, "%s", CBUILD_HEADER_PATH); slashify(out); return true; }
#endif
    const char *inc = getenv("C_INCLUDE_DIR");
    if (inc && *inc) { path_join(candidate, inc, "cbuild.h"); if (file_exists(candidate)) { snprintf(out, PATH_MAX, "%s", candidate); return true; } }
    return false;
}

static void free_list(C_StringList *l) {
    for (size_t i = 0; i < l->count; ++i) free(l->items[i]);
    free(l->items); memset(l, 0, sizeof(*l));
}

static void free_build(C_Build *b) {
    if (!b) return;
    for (size_t i = 0; i < b->target_count; ++i) {
        C_Target *t = &b->targets[i];
        free_list(&t->sources); free_list(&t->includes); free_list(&t->defines); free_list(&t->cflags);
        free_list(&t->ldflags); free_list(&t->system_links); free_list(&t->frameworks);
        free_list(&t->generated_outputs); free_list(&t->generated_inputs); free_list(&t->generated_commands);
    }
    for (size_t i = 0; i < b->dep_count; ++i) {
        C_Dependency *d = &b->deps[i];
        free_list(&d->links); free_list(&d->include_dirs); free_list(&d->source_patterns); free_list(&d->compile_flags);
    }
    free(b);
}

static void compile_build_script(const Options *opt, char dll[PATH_MAX]) {
    if (!file_exists("build.c")) die("build.c not found (run `c init` first)");
    char cache[PATH_MAX], scripts[PATH_MAX], header[PATH_MAX];
    cache_root(cache); path_join(scripts, cache, "scripts"); mkdir_p(scripts);
    if (!bundled_header(header)) die("cannot locate cbuild.h; reinstall c or set C_INCLUDE_DIR");
    char cwd[PATH_MAX];
    if (!_getcwd(cwd, sizeof(cwd))) die("cannot determine working directory");
    slashify(cwd);
    uint64_t h = 1469598103934665603ULL;
    h = hash_update(h, cwd, strlen(cwd)); h = hash_update(h, C_VERSION, strlen(C_VERSION)); h = hash_update(h, opt->cc, strlen(opt->cc));
    h = hash_file_seed(h, "build.c"); h = hash_file_seed(h, header);
    char key[17]; hash_hex(h, key);
    char name[32]; snprintf(name, sizeof(name), "%s.dll", key); path_join(dll, scripts, name);
    if (file_exists(dll)) return;

    char incdir[PATH_MAX]; snprintf(incdir, sizeof(incdir), "%s", header); path_dirname(incdir);
    char inc[PATH_MAX + 3]; snprintf(inc, sizeof(inc), "-I%s", incdir);
    char tmp[PATH_MAX]; snprintf(tmp, sizeof(tmp), "%s.tmp.%lu.dll", dll, (unsigned long)GetCurrentProcessId());
    StrVec a = {0};
    vec_push(&a, opt->cc); vec_push(&a, "-std=c11"); vec_push(&a, "-O2"); vec_push(&a, "-shared");
    vec_push(&a, "-Dbuild=__declspec(dllexport)_cbuild_entry"); vec_push(&a, inc); vec_push(&a, "build.c"); vec_push(&a, "-o"); vec_push(&a, tmp);
    note("CONFIG", "build.c");
    int rc = run_vec(&a, opt->verbose, NULL); vec_free(&a);
    if (rc != 0) { DeleteFileA(tmp); die("failed to compile build.c"); }
    if (!move_replace(tmp, dll)) { DeleteFileA(tmp); die("cannot publish compiled build.c"); }
}

static C_Build *load_build(const Options *opt) {
    char dll[PATH_MAX]; compile_build_script(opt, dll);
    HMODULE h = LoadLibraryA(dll);
    if (!h) {
        DeleteFileA(dll); compile_build_script(opt, dll); h = LoadLibraryA(dll);
        if (!h) die("cannot load compiled build.c (win32 %lu)", (unsigned long)GetLastError());
    }
    typedef void (*BuildFn)(C_Build *);
    BuildFn fn = (BuildFn)(void *)GetProcAddress(h, "_cbuild_entry");
    if (!fn) { FreeLibrary(h); die("build.c must export `void build(C_Build *b)`"); }
    C_Build *b = calloc(1, sizeof(*b));
    if (!b) die("out of memory");
    b->default_target = -1;
    fn(b);
    FreeLibrary(h);
    return b;
}

static void load_lock(LockFile *lock) {
    memset(lock, 0, sizeof(*lock));
    FILE *f = fopen("c.lock", "r");
    if (!f) return;
    char line[4096]; LockEntry cur = {0}; bool in = false;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "[[dependency]]", 14)) {
            if (in && lock->count < C_MAX_DEPS) lock->entries[lock->count++] = cur;
            memset(&cur, 0, sizeof(cur)); in = true; continue;
        }
        char key[64], val[C_MAX_PATH];
        if (sscanf(line, "%63[^=] = \"%1023[^\"]\"", key, val) == 2) {
            size_t n = strlen(key); while (n && isspace((unsigned char)key[n - 1])) key[--n] = '\0';
            if (!strcmp(key, "name")) snprintf(cur.name, sizeof(cur.name), "%s", val);
            else if (!strcmp(key, "url")) snprintf(cur.url, sizeof(cur.url), "%s", val);
            else if (!strcmp(key, "requested")) snprintf(cur.requested, sizeof(cur.requested), "%s", val);
            else if (!strcmp(key, "resolved")) snprintf(cur.resolved, sizeof(cur.resolved), "%s", val);
        }
    }
    if (in && lock->count < C_MAX_DEPS) lock->entries[lock->count++] = cur;
    fclose(f);
}

static void save_lock(const LockFile *lock) {
    if (!lock->count) return;
    size_t cap = lock->count * (C_MAX_PATH + 512) + 128;
    char *text = calloc(cap, 1); if (!text) die("out of memory");
    strcat(text, "# Generated by c. Commit this file.\n\n");
    for (size_t i = 0; i < lock->count; ++i) {
        char item[C_MAX_PATH + 512]; const LockEntry *e = &lock->entries[i];
        snprintf(item, sizeof(item), "[[dependency]]\nname = \"%s\"\nurl = \"%s\"\nrequested = \"%s\"\nresolved = \"%s\"\n\n", e->name, e->url, e->requested, e->resolved);
        strcat(text, item);
    }
    atomic_write("c.lock", text); free(text);
}

static LockEntry *find_lock(LockFile *lock, const C_Dependency *d) {
    for (size_t i = 0; i < lock->count; ++i) {
        LockEntry *e = &lock->entries[i];
        if (!strcmp(e->name, d->name) && !strcmp(e->url, d->git) && !strcmp(e->requested, d->ref)) return e;
    }
    return NULL;
}
