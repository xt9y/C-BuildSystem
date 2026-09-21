#ifndef C_CACHE_CLEANUP_H
#define C_CACHE_CLEANUP_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define C_CACHE_CLEANUP_MAX_DEPS 128
#define C_CACHE_CLEANUP_NAME_MAX 256
#define C_CACHE_CLEANUP_URL_MAX 2048

typedef struct C_CacheCleanupEntry {
    char name[C_CACHE_CLEANUP_NAME_MAX];
    char url[C_CACHE_CLEANUP_URL_MAX];
    char requested[C_CACHE_CLEANUP_NAME_MAX];
    char resolved[96];
} C_CacheCleanupEntry;

typedef struct C_CacheCleanupLock {
    C_CacheCleanupEntry entries[C_CACHE_CLEANUP_MAX_DEPS];
    size_t count;
} C_CacheCleanupLock;

static bool c_cache_cleanup_update_active = false;
static pid_t c_cache_cleanup_root_pid = 0;
static char c_cache_cleanup_wanted[C_CACHE_CLEANUP_NAME_MAX];
static char c_cache_cleanup_status_path[PATH_MAX];
static C_CacheCleanupLock c_cache_cleanup_before;

static void c_cache_cleanup_join(char out[PATH_MAX], const char *a, const char *b) {
    if (!a || !*a) snprintf(out, PATH_MAX, "%s", b ? b : "");
    else if (!b || !*b) snprintf(out, PATH_MAX, "%s", a);
    else if (a[strlen(a) - 1] == '/') snprintf(out, PATH_MAX, "%s%s", a, b);
    else snprintf(out, PATH_MAX, "%s/%s", a, b);
}

static const char *c_cache_cleanup_home(void) {
    const char *home = getenv("HOME");
    return home && *home ? home : NULL;
}

static bool c_cache_cleanup_root(char out[PATH_MAX]) {
    const char *override = getenv("C_CACHE_DIR");
    if (override && *override) {
        return snprintf(out, PATH_MAX, "%s", override) < PATH_MAX;
    }
    const char *home = c_cache_cleanup_home();
    if (!home) return false;
#ifdef __APPLE__
    return snprintf(out, PATH_MAX, "%s/Library/Caches/c", home) < PATH_MAX;
#else
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) return snprintf(out, PATH_MAX, "%s/c", xdg) < PATH_MAX;
    return snprintf(out, PATH_MAX, "%s/.cache/c", home) < PATH_MAX;
#endif
}

static int c_cache_cleanup_remove_tree(const char *path) {
    struct stat root;
    if (lstat(path, &root) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(root.st_mode) || S_ISLNK(root.st_mode)) return unlink(path);

    DIR *dir = opendir(path);
    if (!dir) return -1;
    int rc = 0;
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char child[PATH_MAX];
        c_cache_cleanup_join(child, path, ent->d_name);
        struct stat st;
        if (lstat(child, &st) != 0) { rc = -1; break; }
        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) rc = c_cache_cleanup_remove_tree(child);
        else rc = unlink(child);
        if (rc != 0) break;
    }
    int saved = errno;
    closedir(dir);
    errno = saved;
    if (rc == 0) rc = rmdir(path);
    return rc;
}

static int c_cache_cleanup_remove_entry(const char *path) {
    char marker[PATH_MAX];
    if (snprintf(marker, sizeof(marker), "%s.c-ready", path) >= (int)sizeof(marker)) return -1;
    if (unlink(marker) != 0 && errno != ENOENT) return -1;
    return c_cache_cleanup_remove_tree(path);
}

static uint64_t c_cache_cleanup_hash(const char *text) {
    uint64_t h = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) { h ^= *p++; h *= 1099511628211ULL; }
    return h;
}

static void c_cache_cleanup_hash_hex(const char *text, char out[17]) {
    snprintf(out, 17, "%016llx", (unsigned long long)c_cache_cleanup_hash(text));
}

static void c_cache_cleanup_lock_push(C_CacheCleanupLock *lock, const C_CacheCleanupEntry *entry) {
    if (lock->count < C_CACHE_CLEANUP_MAX_DEPS) lock->entries[lock->count++] = *entry;
}

static void c_cache_cleanup_load_lock(C_CacheCleanupLock *lock) {
    memset(lock, 0, sizeof(*lock));
    FILE *file = fopen("c.lock", "r");
    if (!file) return;

    char line[4096];
    C_CacheCleanupEntry current = {0};
    bool in_dependency = false;
    while (fgets(line, sizeof(line), file)) {
        if (!strncmp(line, "[[dependency]]", 14)) {
            if (in_dependency) c_cache_cleanup_lock_push(lock, &current);
            memset(&current, 0, sizeof(current));
            in_dependency = true;
            continue;
        }
        if (!in_dependency) continue;

        char key[64], value[C_CACHE_CLEANUP_URL_MAX];
        if (sscanf(line, "%63[^=] = \"%2047[^\"]\"", key, value) != 2) continue;
        size_t key_len = strlen(key);
        while (key_len && (key[key_len - 1] == ' ' || key[key_len - 1] == '\t')) key[--key_len] = '\0';
        if (!strcmp(key, "name")) snprintf(current.name, sizeof(current.name), "%s", value);
        else if (!strcmp(key, "url")) snprintf(current.url, sizeof(current.url), "%s", value);
        else if (!strcmp(key, "requested")) snprintf(current.requested, sizeof(current.requested), "%s", value);
        else if (!strcmp(key, "resolved")) snprintf(current.resolved, sizeof(current.resolved), "%s", value);
    }
    if (in_dependency) c_cache_cleanup_lock_push(lock, &current);
    fclose(file);
}

static const C_CacheCleanupEntry *c_cache_cleanup_find_name(const C_CacheCleanupLock *lock, const char *name) {
    for (size_t i = 0; i < lock->count; ++i)
        if (!strcmp(lock->entries[i].name, name)) return &lock->entries[i];
    return NULL;
}

static bool c_cache_cleanup_has_url(const C_CacheCleanupLock *lock, const char *url) {
    for (size_t i = 0; i < lock->count; ++i)
        if (!strcmp(lock->entries[i].url, url)) return true;
    return false;
}

static bool c_cache_cleanup_entry_changed(const C_CacheCleanupEntry *before, const C_CacheCleanupEntry *after) {
    if (!before || !after) return before != after;
    return strcmp(before->url, after->url) ||
           strcmp(before->requested, after->requested) ||
           strcmp(before->resolved, after->resolved);
}

static bool c_cache_cleanup_current_source(const C_CacheCleanupLock *after, const char *name, const char *filename) {
    for (size_t i = 0; i < after->count; ++i) {
        const C_CacheCleanupEntry *entry = &after->entries[i];
        if (strcmp(entry->name, name)) continue;
        char input[C_CACHE_CLEANUP_URL_MAX + 128], key[17], expected[C_CACHE_CLEANUP_NAME_MAX + 32];
        if (snprintf(input, sizeof(input), "%s:%s", entry->url, entry->resolved) >= (int)sizeof(input)) continue;
        c_cache_cleanup_hash_hex(input, key);
        if (snprintf(expected, sizeof(expected), "%s-%s", entry->name, key) >= (int)sizeof(expected)) continue;
        if (!strcmp(filename, expected)) return true;
    }
    return false;
}

static int c_cache_cleanup_prune_prefix(const char *cache, const char *subdir, const char *name,
                                        const C_CacheCleanupLock *after, bool preserve_current_source) {
    char root[PATH_MAX];
    c_cache_cleanup_join(root, cache, subdir);
    DIR *dir = opendir(root);
    if (!dir) return errno == ENOENT ? 0 : -1;

    char prefix[C_CACHE_CLEANUP_NAME_MAX + 2];
    if (snprintf(prefix, sizeof(prefix), "%s-", name) >= (int)sizeof(prefix)) { closedir(dir); return -1; }
    size_t prefix_len = strlen(prefix);
    int removed = 0;
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (strncmp(ent->d_name, prefix, prefix_len)) continue;
        struct stat st;
        char path[PATH_MAX];
        c_cache_cleanup_join(path, root, ent->d_name);
        if (lstat(path, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) continue;
        if (preserve_current_source && c_cache_cleanup_current_source(after, name, ent->d_name)) continue;
        if (c_cache_cleanup_remove_entry(path) != 0) { closedir(dir); return -1; }
        ++removed;
    }
    closedir(dir);
    return removed;
}

static int c_cache_cleanup_prune_mirror(const char *cache, const char *url) {
    char git_root[PATH_MAX], key[17], name[32], path[PATH_MAX];
    c_cache_cleanup_join(git_root, cache, "git");
    c_cache_cleanup_hash_hex(url, key);
    if (snprintf(name, sizeof(name), "%s.git", key) >= (int)sizeof(name)) return -1;
    c_cache_cleanup_join(path, git_root, name);
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    return c_cache_cleanup_remove_entry(path) == 0 ? 1 : -1;
}

static bool c_cache_cleanup_name_seen(const char names[][C_CACHE_CLEANUP_NAME_MAX], size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(names[i], name)) return true;
    return false;
}

static void c_cache_cleanup_prune_after_update(void) {
    C_CacheCleanupLock after;
    c_cache_cleanup_load_lock(&after);
    char cache[PATH_MAX];
    if (!c_cache_cleanup_root(cache)) return;

    char names[C_CACHE_CLEANUP_MAX_DEPS][C_CACHE_CLEANUP_NAME_MAX];
    size_t name_count = 0;
    for (size_t i = 0; i < c_cache_cleanup_before.count; ++i) {
        const C_CacheCleanupEntry *old = &c_cache_cleanup_before.entries[i];
        if (c_cache_cleanup_wanted[0] && strcmp(old->name, c_cache_cleanup_wanted)) continue;
        if (c_cache_cleanup_name_seen(names, name_count, old->name)) continue;
        if (name_count < C_CACHE_CLEANUP_MAX_DEPS)
            snprintf(names[name_count++], C_CACHE_CLEANUP_NAME_MAX, "%s", old->name);
    }

    for (size_t i = 0; i < name_count; ++i) {
        const char *name = names[i];
        const C_CacheCleanupEntry *old = c_cache_cleanup_find_name(&c_cache_cleanup_before, name);
        const C_CacheCleanupEntry *now = c_cache_cleanup_find_name(&after, name);
        bool changed = c_cache_cleanup_entry_changed(old, now);

        if (c_cache_cleanup_prune_prefix(cache, "src", name, &after, true) < 0) return;
        if (changed) {
            if (c_cache_cleanup_prune_prefix(cache, "pkg", name, &after, false) < 0) return;
            if (c_cache_cleanup_prune_prefix(cache, "dep-build", name, &after, false) < 0) return;
        }
    }

    for (size_t i = 0; i < c_cache_cleanup_before.count; ++i) {
        const C_CacheCleanupEntry *old = &c_cache_cleanup_before.entries[i];
        if (c_cache_cleanup_wanted[0] && strcmp(old->name, c_cache_cleanup_wanted)) continue;
        if (!old->url[0] || c_cache_cleanup_has_url(&after, old->url)) continue;
        if (c_cache_cleanup_prune_mirror(cache, old->url) < 0) return;
    }
}

static bool c_cache_cleanup_is_command(const char *arg) {
    static const char *commands[] = {
        "init", "build", "run", "watch", "fetch", "update", "deps", "test",
        "clean", "cache", "cclean", "doctor", "help", "version", "--help", "--version"
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        if (!strcmp(arg, commands[i])) return true;
    return false;
}

typedef struct C_CacheCleanupArgs {
    int argc;
    char *argv[128];
#ifndef __APPLE__
    char storage[16384];
#endif
} C_CacheCleanupArgs;

static bool c_cache_cleanup_args(C_CacheCleanupArgs *out) {
    memset(out, 0, sizeof(*out));
#ifdef __APPLE__
    int argc = *_NSGetArgc();
    char **argv = *_NSGetArgv();
    if (!argv || argc < 1) return false;
    if (argc > (int)(sizeof(out->argv) / sizeof(out->argv[0]))) argc = (int)(sizeof(out->argv) / sizeof(out->argv[0]));
    out->argc = argc;
    for (int i = 0; i < argc; ++i) out->argv[i] = argv[i];
    return true;
#else
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return false;
    ssize_t got = read(fd, out->storage, sizeof(out->storage) - 1);
    close(fd);
    if (got <= 0) return false;
    out->storage[got] = '\0';
    size_t offset = 0;
    while (offset < (size_t)got && out->argc < (int)(sizeof(out->argv) / sizeof(out->argv[0]))) {
        size_t left = (size_t)got - offset;
        size_t len = strnlen(out->storage + offset, left);
        if (len == left) break;
        out->argv[out->argc++] = out->storage + offset;
        offset += len + 1;
    }
    return out->argc > 0;
#endif
}

static void c_cache_cleanup_status_write(int status) {
    if (!c_cache_cleanup_update_active || !c_cache_cleanup_status_path[0]) return;
    int fd = open(c_cache_cleanup_status_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    char value = status == 0 ? '0' : '1';
    (void)write(fd, &value, 1);
    close(fd);
}

static bool c_cache_cleanup_status_success(void) {
    int fd = open(c_cache_cleanup_status_path, O_RDONLY);
    if (fd < 0) return false;
    char value = 0;
    ssize_t got = read(fd, &value, 1);
    close(fd);
    return got == 1 && value == '0';
}

static void c_cache_cleanup_setup_status(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(c_cache_cleanup_status_path, sizeof(c_cache_cleanup_status_path),
             "%s%s.c-buildsystem-update-%ld.status", tmp,
             tmp[strlen(tmp) - 1] == '/' ? "" : "/", (long)c_cache_cleanup_root_pid);
    (void)unlink(c_cache_cleanup_status_path);
}

static bool c_cache_cleanup_standalone_update(const C_CacheCleanupArgs *args) {
    c_cache_cleanup_wanted[0] = '\0';
    for (int i = 2; i < args->argc; ++i) {
        const char *arg = args->argv[i];
        if (!strcmp(arg, "--")) return false;
        if (c_cache_cleanup_is_command(arg)) return false;
        if (!strcmp(arg, "--cc")) { if (++i >= args->argc) return false; continue; }
        if (!strcmp(arg, "--release") || !strcmp(arg, "-Drelease") ||
            !strcmp(arg, "-v") || !strcmp(arg, "--verbose")) continue;
        if (arg[0] == '-') continue;
        if (c_cache_cleanup_wanted[0]) return false;
        snprintf(c_cache_cleanup_wanted, sizeof(c_cache_cleanup_wanted), "%s", arg);
    }
    return true;
}

static void c_cache_cleanup_full_cache(void) {
    char cache[PATH_MAX];
    if (!c_cache_cleanup_root(cache)) {
        fputs("c: error: cannot determine cache directory\n", stderr);
        fflush(NULL);
        _exit(1);
    }
    if (c_cache_cleanup_remove_tree(cache) != 0 && errno != ENOENT) {
        fprintf(stderr, "c: error: cannot remove cache %s: %s\n", cache, strerror(errno));
        fflush(NULL);
        _exit(1);
    }
    printf("  CLEAN   %s\n", cache);
    fflush(NULL);
    _exit(0);
}

__attribute__((constructor))
static void c_cache_cleanup_start(void) {
    C_CacheCleanupArgs args;
    if (!c_cache_cleanup_args(&args) || args.argc < 2) return;
    if (!strcmp(args.argv[1], "cclean")) c_cache_cleanup_full_cache();
    if (strcmp(args.argv[1], "update") || !c_cache_cleanup_standalone_update(&args)) return;

    c_cache_cleanup_update_active = true;
    c_cache_cleanup_root_pid = getpid();
    c_cache_cleanup_load_lock(&c_cache_cleanup_before);
    c_cache_cleanup_setup_status();
}

__attribute__((destructor))
static void c_cache_cleanup_finish(void) {
    if (!c_cache_cleanup_update_active || getpid() != c_cache_cleanup_root_pid) return;
    bool success = c_cache_cleanup_status_success();
    (void)unlink(c_cache_cleanup_status_path);
    if (success) c_cache_cleanup_prune_after_update();
}

static _Noreturn void c_cache_cleanup_exit(int status) {
    c_cache_cleanup_status_write(status);
    exit(status);
}

static _Noreturn void c_cache_cleanup__exit(int status) {
    c_cache_cleanup_status_write(status);
    _exit(status);
}

#define exit c_cache_cleanup_exit
#define _exit c_cache_cleanup__exit

#endif
