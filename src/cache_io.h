#ifndef C_CACHE_IO_H
#define C_CACHE_IO_H

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif


/*
 * Native commands mutate project-local build state. Serialize those
 * commands per canonical working directory so two independent `c`
 * processes cannot publish the same object, depfile, link output or
 * compile_commands.json concurrently. Different projects use
 * different lock keys and remain fully parallel.
 *
 * flock() is intentionally used here: the lock is shared across the
 * cli fork, allowing `c run` to release it immediately before the
 * finished program is spawned instead of holding it for the lifetime
 * of the user's application.
 */
static int c_project_lock_fd = -1;
static int c_project_release_for_run = 0;

static int c_project_action_name(const char *arg) {
    static const char *const actions[] = {
        "init", "build", "run", "watch", "fetch", "update",
        "deps", "test", "clean", "cache"
    };
    if (!arg) return 0;
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i)
        if (!strcmp(arg, actions[i])) return 1;
    return 0;
}

static void c_project_consider_arg(const char *arg, int *need_lock, int *release_for_run) {
    if (!arg || !*arg) return;
    if (!strcmp(arg, "run")) *release_for_run = 1;
    if (c_project_action_name(arg)) *need_lock = 1;
}

static void c_project_command_flags(int *need_lock, int *release_for_run) {
    *need_lock = 0;
    *release_for_run = 0;
#ifdef __APPLE__
    int argc = *_NSGetArgc();
    char **argv = *_NSGetArgv();
    if (!argv || argc < 1) { *need_lock = 1; return; }
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--")) break;
        c_project_consider_arg(argv[i], need_lock, release_for_run);
    }
#else
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) { *need_lock = 1; return; }
    char buf[16384];
    ssize_t got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got <= 0) { *need_lock = 1; return; }
    buf[got] = '\0';
    size_t off = 0;
    int index = 0;
    while (off < (size_t)got) {
        size_t left = (size_t)got - off;
        size_t len = strnlen(buf + off, left);
        if (len == left) break;
        if (index > 0) {
            if (!strcmp(buf + off, "--")) break;
            c_project_consider_arg(buf + off, need_lock, release_for_run);
        }
        off += len + 1;
        ++index;
    }
#endif
}

static uint64_t c_project_path_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) { h ^= *p++; h *= 1099511628211ULL; }
    return h;
}

static void c_project_lock_release(void) {
    if (c_project_lock_fd >= 0) {
        (void)flock(c_project_lock_fd, LOCK_UN);
        close(c_project_lock_fd);
        c_project_lock_fd = -1;
    }
    (void)unsetenv("C_PROJECT_LOCK_KEY");
}

static void c_project_lock_fail(const char *what) {
    int saved = errno;
    fprintf(stderr, "c: error: project lock %s: %s\n", what, strerror(saved));
    _exit(1);
}

__attribute__((constructor))
static void c_project_lock_acquire(void) {
    int need_lock = 0, release_for_run = 0;
    c_project_command_flags(&need_lock, &release_for_run);
    if (!need_lock) return;

    char cwd[PATH_MAX], canonical[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) c_project_lock_fail("getcwd");
    const char *identity = cwd;
    if (realpath(cwd, canonical)) identity = canonical;

    char key[17];
    if (snprintf(key, sizeof(key), "%016llx",
                 (unsigned long long)c_project_path_hash(identity)) >= (int)sizeof(key)) {
        errno = ENAMETOOLONG;
        c_project_lock_fail("key");
    }

    const char *held = getenv("C_PROJECT_LOCK_KEY");
    if (held && !strcmp(held, key)) {
        c_project_release_for_run = release_for_run;
        return;
    }

    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char root[PATH_MAX], lock_path[PATH_MAX];
    int n = snprintf(root, sizeof(root), "%s%sc-buildsystem-locks-%lu",
                     tmp, tmp[strlen(tmp) - 1] == '/' ? "" : "/",
                     (unsigned long)getuid());
    if (n < 0 || n >= (int)sizeof(root)) {
        errno = ENAMETOOLONG;
        c_project_lock_fail("directory path");
    }
    if (mkdir(root, 0700) != 0 && errno != EEXIST)
        c_project_lock_fail("directory create");
    struct stat st;
    if (lstat(root, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        errno = EPERM;
        c_project_lock_fail("directory ownership");
    }
    n = snprintf(lock_path, sizeof(lock_path), "%s/%s.lock", root, key);
    if (n < 0 || n >= (int)sizeof(lock_path)) {
        errno = ENAMETOOLONG;
        c_project_lock_fail("file path");
    }

    int flags = O_RDWR | O_CREAT;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(lock_path, flags, 0600);
    if (fd < 0) c_project_lock_fail("open");
    while (flock(fd, LOCK_EX) != 0) {
        if (errno == EINTR) continue;
        int saved = errno;
        close(fd);
        errno = saved;
        c_project_lock_fail("acquire");
    }
    c_project_lock_fd = fd;
    c_project_release_for_run = release_for_run;
    if (setenv("C_PROJECT_LOCK_KEY", key, 1) != 0) {
        int saved = errno;
        c_project_lock_release();
        errno = saved;
        c_project_lock_fail("environment");
    }
}

__attribute__((destructor))
static void c_project_lock_cleanup(void) {
    c_project_lock_release();
}

static int c_project_runtime_path(const char *path) {
    return path &&
        (!strncmp(path, "./build/debug/", 14) ||
         !strncmp(path, "./build/release/", 16));
}

static int c_project_posix_spawnp(pid_t *pid, const char *path,
                                  const posix_spawn_file_actions_t *file_actions,
                                  const posix_spawnattr_t *attrp,
                                  char *const argv[], char *const envp[]) {
    if (c_project_release_for_run && c_project_runtime_path(path))
        c_project_lock_release();
    return posix_spawnp(pid, path, file_actions, attrp, argv, envp);
}

/*
 * main.c historically copied cbuild.h to <cache>/scripts/cbuild.h and then
 * compiled build.c with that directory on the include path. Keep the old core
 * ABI untouched, but make that cached header purely virtual:
 *
 *   - writes to <cache>/scripts/cbuild.h are discarded;
 *   - reads of that path (used for the build-script cache hash) read the
 *     canonical cbuild.h instead;
 *   - the compiler invocation for build.c receives the canonical include dir.
 *
 * Therefore no cached cbuild.h file or symlink is created, while changes to
 * the real header still invalidate the compiled build.c module.
 *
 * This shim also protects the build system's atomic cache writers. Several
 * production paths use <destination>.tmp.<pid> followed by rename(). Opening
 * those predictable temporary names through stdio used to follow a planted
 * symlink. For .tmp. writes we unlink the name and recreate it with O_EXCL
 * (and O_NOFOLLOW where available), so a racing symlink can make the write
 * fail but cannot redirect it into another file.
 */

static int c_is_cached_build_header(const char *path) {
    static const char suffix[] = "/scripts/cbuild.h";
    if (!path) return 0;
    size_t n = strlen(path);
    size_t s = sizeof(suffix) - 1;
    return n >= s && !strcmp(path + n - s, suffix);
}

static int c_is_atomic_temp_write(const char *path, const char *mode) {
    if (!path || !mode) return 0;
    if (strcmp(mode, "w") && strcmp(mode, "wb")) return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strstr(base, ".tmp.") != NULL;
}

static FILE *c_exclusive_temp_fopen(const char *path, const char *mode) {
    if (unlink(path) != 0 && errno != ENOENT) return NULL;

    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif

    int fd = open(path, flags, 0600);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, mode);
    if (!f) {
        int saved = errno;
        close(fd);
        unlink(path);
        errno = saved;
        return NULL;
    }
    return f;
}

static int c_copy_readable_path(const char *path, char out[PATH_MAX]) {
    if (!path || !*path || access(path, R_OK) != 0) return 0;
    int n = snprintf(out, PATH_MAX, "%s", path);
    if (n < 0 || n >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return 0;
    }
    return 1;
}

static int c_header_beside_binary(char out[PATH_MAX]) {
    char exe[PATH_MAX];
#ifdef __APPLE__
    uint32_t size = (uint32_t)sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0) return 0;
#else
    ssize_t exe_len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (exe_len <= 0) return 0;
    exe[exe_len] = '\0';
#endif

    char *slash = strrchr(exe, '/');
    if (!slash) return 0;
    *slash = '\0';

    char candidate[PATH_MAX];
    int n = snprintf(candidate, sizeof(candidate), "%s/../include/cbuild.h", exe);
    if (n < 0 || n >= (int)sizeof(candidate)) {
        errno = ENAMETOOLONG;
        return 0;
    }
    return c_copy_readable_path(candidate, out);
}

static int c_canonical_header(char out[PATH_MAX]) {
    /* The header installed/built beside the running c executable wins. */
    if (c_header_beside_binary(out)) return 1;

#ifdef CBUILD_HEADER_PATH
    if (c_copy_readable_path(CBUILD_HEADER_PATH, out)) return 1;
#endif

    /* Development fallback only. */
    const char *inc = getenv("C_INCLUDE_DIR");
    if (inc && *inc) {
        char candidate[PATH_MAX];
        int n = snprintf(candidate, sizeof(candidate), "%s/cbuild.h", inc);
        if (n >= 0 && n < (int)sizeof(candidate) &&
            c_copy_readable_path(candidate, out)) return 1;
    }

    errno = ENOENT;
    return 0;
}

static int c_canonical_include(char out[PATH_MAX]) {
    if (!c_canonical_header(out)) return 0;
    char *slash = strrchr(out, '/');
    if (!slash) {
        errno = EINVAL;
        return 0;
    }
    *slash = '\0';
    return 1;
}

static FILE *c_direct_header_fopen(const char *path, const char *mode) {
    if (!path || !mode) {
        errno = EINVAL;
        return NULL;
    }

    if (c_is_cached_build_header(path)) {
        if (!strcmp(mode, "wb")) {
            /* Remove leftovers from older c versions and discard the copy. */
            if (unlink(path) != 0 && errno != ENOENT) return NULL;
            return tmpfile();
        }

        if (!strcmp(mode, "rb")) {
            char canonical[PATH_MAX];
            if (!c_canonical_header(canonical)) return NULL;
            return fopen(canonical, mode);
        }
    }

    if (c_is_atomic_temp_write(path, mode)) return c_exclusive_temp_fopen(path, mode);
    return fopen(path, mode);
}

static int c_build_script_argv(char *const argv[]) {
    if (!argv) return 0;
    for (size_t i = 0; argv[i]; ++i) {
        if (!strcmp(argv[i], "build.c")) return 1;
    }
    return 0;
}

static int c_direct_header_execvp(const char *file, char *const argv[]) {
    if (!c_build_script_argv(argv)) return execvp(file, argv);

    char include_dir[PATH_MAX];
    if (!c_canonical_include(include_dir)) return execvp(file, argv);

    char include_arg[PATH_MAX + 3];
    int n = snprintf(include_arg, sizeof(include_arg), "-I%s", include_dir);
    if (n < 0 || n >= (int)sizeof(include_arg)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    char *rewritten[256];
    size_t count = 0;
    int replaced = 0;
    while (argv[count]) {
        if (count + 2 >= sizeof(rewritten) / sizeof(rewritten[0])) {
            errno = E2BIG;
            return -1;
        }
        rewritten[count] = argv[count];
        if (!replaced && argv[count][0] == '-' && argv[count][1] == 'I' &&
            strstr(argv[count] + 2, "/scripts")) {
            rewritten[count] = include_arg;
            replaced = 1;
        }
        ++count;
    }

    if (!replaced) rewritten[count++] = include_arg;
    rewritten[count] = NULL;
    return execvp(file, rewritten);
}

#define fopen c_direct_header_fopen
#define execvp c_direct_header_execvp
#define posix_spawnp c_project_posix_spawnp

#endif