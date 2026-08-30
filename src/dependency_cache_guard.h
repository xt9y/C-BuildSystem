#ifndef C_DEPENDENCY_CACHE_GUARD_H
#define C_DEPENDENCY_CACHE_GUARD_H

/*
 * Transactional guards for the dependency cache used by src/main.c.
 *
 * The legacy dependency resolver historically used directory existence as its
 * completion signal. A failed mirror clone or source checkout could therefore
 * leave a partial directory that every later invocation treated as valid.
 * This guard keeps the core ABI untouched while adding explicit completion
 * markers and failure cleanup around the Git operations.
 *
 * It also makes c.lock and CMake's .c-built completion stamp atomic. A process
 * that is interrupted while writing either file leaves only a temporary file;
 * the previous lockfile/stamp remains intact.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* cache_io.h is force-included before this file. Chain through its wrappers. */
#ifdef fopen
#undef fopen
#endif
#ifdef execvp
#undef execvp
#endif

#define C_DEP_READY_SUFFIX ".c-ready"
#define C_DEP_ATOMIC_SLOTS 16

typedef struct C_DepAtomicPublish {
    FILE *file;
    char temp[PATH_MAX];
    char target[PATH_MAX];
} C_DepAtomicPublish;

static C_DepAtomicPublish c_dep_atomic[C_DEP_ATOMIC_SLOTS];

static int c_dep_cache_root(char out[PATH_MAX]) {
    const char *override = getenv("C_CACHE_DIR");
    int n;
    if (override && *override) {
        n = snprintf(out, PATH_MAX, "%s", override);
    }
#ifdef __APPLE__
    else {
        const char *home = getenv("HOME");
        if (!home || !*home) { errno = ENOENT; return 0; }
        n = snprintf(out, PATH_MAX, "%s/Library/Caches/c", home);
    }
#else
    else {
        const char *xdg = getenv("XDG_CACHE_HOME");
        if (xdg && *xdg) n = snprintf(out, PATH_MAX, "%s/c", xdg);
        else {
            const char *home = getenv("HOME");
            if (!home || !*home) { errno = ENOENT; return 0; }
            n = snprintf(out, PATH_MAX, "%s/.cache/c", home);
        }
    }
#endif
    if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return 0; }
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = '\0';
    return 1;
}

static int c_dep_is_direct_cache_entry(const char *path, const char *kind) {
    if (!path || !*path || !kind || !*kind) return 0;
    char root[PATH_MAX];
    if (!c_dep_cache_root(root)) return 0;
    char prefix[PATH_MAX];
    int n = snprintf(prefix, sizeof(prefix), "%s/%s/", root, kind);
    if (n < 0 || n >= (int)sizeof(prefix)) return 0;
    size_t plen = (size_t)n;
    if (strncmp(path, prefix, plen)) return 0;
    const char *tail = path + plen;
    return *tail && strchr(tail, '/') == NULL;
}

static int c_dep_marker_path(const char *entry, char out[PATH_MAX]) {
    int n = snprintf(out, PATH_MAX, "%s%s", entry, C_DEP_READY_SUFFIX);
    if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return 0; }
    return 1;
}

static int c_dep_marker_valid(const char *entry) {
    char marker[PATH_MAX];
    if (!c_dep_marker_path(entry, marker)) return 0;
    struct stat st;
    if (lstat(marker, &st) != 0) return 0;
    return S_ISREG(st.st_mode) && st.st_size > 0;
}

static void c_dep_unlink_marker(const char *entry) {
    char marker[PATH_MAX];
    if (c_dep_marker_path(entry, marker)) (void)unlink(marker);
}

static int c_dep_write_marker(const char *entry, const char *text) {
    char marker[PATH_MAX], temp[PATH_MAX];
    if (!c_dep_marker_path(entry, marker)) return 0;
    if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", marker, (long)getpid()) >= (int)sizeof(temp)) {
        errno = ENAMETOOLONG;
        return 0;
    }
    if (unlink(temp) != 0 && errno != ENOENT) return 0;
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(temp, flags, 0600);
    if (fd < 0) return 0;
    if (!text) text = "ready\n";
    size_t len = strlen(text);
    ssize_t wrote = write(fd, text, len);
    int saved = errno;
    if (wrote == (ssize_t)len && fsync(fd) == 0 && close(fd) == 0) {
        if (rename(temp, marker) == 0) return 1;
        saved = errno;
    } else {
        (void)close(fd);
    }
    (void)unlink(temp);
    errno = saved;
    return 0;
}

static int c_dep_remove_tree_nofollow(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return unlink(path);

    DIR *dir = opendir(path);
    if (!dir) return -1;
    int rc = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char child[PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n < 0 || n >= (int)sizeof(child) || c_dep_remove_tree_nofollow(child) != 0) {
            rc = -1;
            break;
        }
    }
    int saved = errno;
    closedir(dir);
    errno = saved;
    if (rc == 0) rc = rmdir(path);
    return rc;
}

static int c_dep_remove_entry(const char *entry) {
    c_dep_unlink_marker(entry);
    return c_dep_remove_tree_nofollow(entry);
}

static int c_dep_wait(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static int c_dep_exec_real(const char *file, char *const argv[], int quiet) {
    pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        if (quiet) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                (void)dup2(devnull, STDOUT_FILENO);
                (void)dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) close(devnull);
            }
        }
        execvp(file, argv);
        _exit(127);
    }
    return c_dep_wait(pid);
}

static int c_dep_legacy_mirror_valid(const char *mirror) {
    char head[PATH_MAX], objects[PATH_MAX], config[PATH_MAX];
    if (snprintf(head, sizeof(head), "%s/HEAD", mirror) >= (int)sizeof(head) ||
        snprintf(objects, sizeof(objects), "%s/objects", mirror) >= (int)sizeof(objects) ||
        snprintf(config, sizeof(config), "%s/config", mirror) >= (int)sizeof(config)) return 0;
    struct stat st;
    if (lstat(head, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    if (lstat(objects, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return 0;
    if (lstat(config, &st) != 0 || !S_ISREG(st.st_mode)) return 0;

    char *argv[] = {"git", "--git-dir", (char *)mirror, "rev-parse", "--verify", "HEAD^{commit}", NULL};
    return c_dep_exec_real("git", argv, 1) == 0;
}

static int c_dep_valid_build_stamp(const char *path) {
    FILE *f = c_direct_header_fopen(path, "rb");
    if (!f) return 0;
    char buf[96];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    int bad = ferror(f);
    (void)fclose(f);
    if (bad) return 0;
    buf[n] = '\0';
    while (n && isspace((unsigned char)buf[n - 1])) buf[--n] = '\0';
    if (n != 40 && n != 64) return 0;
    for (size_t i = 0; i < n; ++i) if (!isxdigit((unsigned char)buf[i])) return 0;
    return 1;
}

static int c_dependency_stat(const char *path, struct stat *st) {
    int rc = stat(path, st);
    if (rc != 0) return rc;

    size_t len = path ? strlen(path) : 0;
    static const char built_suffix[] = "/.c-built";
    size_t blen = sizeof(built_suffix) - 1;
    if (len >= blen && !strcmp(path + len - blen, built_suffix) && !c_dep_valid_build_stamp(path)) {
        errno = ENOENT;
        return -1;
    }

    if (!S_ISDIR(st->st_mode)) return 0;
    if (!c_dep_is_direct_cache_entry(path, "src") && !c_dep_is_direct_cache_entry(path, "git")) return 0;

    struct stat lst;
    if (lstat(path, &lst) != 0 || S_ISLNK(lst.st_mode) || !S_ISDIR(lst.st_mode)) {
        errno = ENOENT;
        return -1;
    }

    if (c_dep_marker_valid(path)) return 0;

    if (c_dep_is_direct_cache_entry(path, "git") && c_dep_legacy_mirror_valid(path)) {
        (void)c_dep_write_marker(path, "mirror\n");
        return 0;
    }

    errno = ENOENT;
    return -1;
}

static int c_dep_is_atomic_target(const char *path, const char *mode) {
    if (!path || !mode || (strcmp(mode, "w") && strcmp(mode, "wb"))) return 0;
    if (!strcmp(path, "c.lock")) return 1;
    size_t len = strlen(path);
    static const char suffix[] = "/.c-built";
    size_t slen = sizeof(suffix) - 1;
    return len >= slen && !strcmp(path + len - slen, suffix);
}

static FILE *c_dep_open_atomic(const char *path, const char *mode) {
    size_t slot = C_DEP_ATOMIC_SLOTS;
    for (size_t i = 0; i < C_DEP_ATOMIC_SLOTS; ++i) {
        if (!c_dep_atomic[i].file) { slot = i; break; }
    }
    if (slot == C_DEP_ATOMIC_SLOTS) { errno = EMFILE; return NULL; }

    char temp[PATH_MAX];
    if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(temp)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    FILE *f = c_exclusive_temp_fopen(temp, mode);
    if (!f) return NULL;

    c_dep_atomic[slot].file = f;
    snprintf(c_dep_atomic[slot].temp, sizeof(c_dep_atomic[slot].temp), "%s", temp);
    snprintf(c_dep_atomic[slot].target, sizeof(c_dep_atomic[slot].target), "%s", path);
    return f;
}

static FILE *c_dependency_fopen(const char *path, const char *mode) {
    if (c_dep_is_atomic_target(path, mode)) return c_dep_open_atomic(path, mode);
    return c_direct_header_fopen(path, mode);
}

static int c_dependency_fclose(FILE *file) {
    if (!file) { errno = EINVAL; return EOF; }
    for (size_t i = 0; i < C_DEP_ATOMIC_SLOTS; ++i) {
        C_DepAtomicPublish *p = &c_dep_atomic[i];
        if (p->file != file) continue;

        int rc = 0;
        if (fflush(file) != 0) rc = EOF;
        if (rc == 0) {
            int fd = fileno(file);
            if (fd >= 0 && fsync(fd) != 0) rc = EOF;
        }
        if (fclose(file) != 0) rc = EOF;
        if (rc == 0 && rename(p->temp, p->target) != 0) rc = EOF;
        if (rc != 0) (void)unlink(p->temp);
        memset(p, 0, sizeof(*p));
        return rc;
    }
    return fclose(file);
}

static int c_dep_git_clone_mirror(char *const argv[], const char **dest) {
    if (!argv || !argv[0] || !argv[1] || !argv[2] || !argv[3] || !argv[4]) return 0;
    if (strcmp(argv[0], "git") || strcmp(argv[1], "clone") || strcmp(argv[2], "--mirror")) return 0;
    if (!c_dep_is_direct_cache_entry(argv[4], "git")) return 0;
    *dest = argv[4];
    return 1;
}

static int c_dep_git_checkout(char *const argv[], const char **mirror, const char **source, const char **resolved) {
    if (!argv || !argv[0]) return 0;
    if (strcmp(argv[0], "git")) return 0;
    const char *gitdir = NULL, *worktree = NULL, *commit = NULL;
    for (size_t i = 1; argv[i]; ++i) {
        if (!strcmp(argv[i], "--git-dir") && argv[i + 1]) gitdir = argv[++i];
        else if (!strcmp(argv[i], "--work-tree") && argv[i + 1]) worktree = argv[++i];
        else if (!strcmp(argv[i], "checkout")) {
            if (argv[i + 1] && !strcmp(argv[i + 1], "-f")) ++i;
            if (argv[i + 1]) commit = argv[i + 1];
            break;
        }
    }
    if (!gitdir || !worktree || !commit) return 0;
    if (!c_dep_is_direct_cache_entry(gitdir, "git") || !c_dep_is_direct_cache_entry(worktree, "src")) return 0;
    *mirror = gitdir;
    *source = worktree;
    *resolved = commit;
    return 1;
}

static int c_dependency_execvp(const char *file, char *const argv[]) {
    const char *dest = NULL;
    if (c_dep_git_clone_mirror(argv, &dest)) {
        if (c_dep_remove_entry(dest) != 0 && errno != ENOENT) _exit(127);
        int rc = c_dep_exec_real(file, argv, 0);
        if (rc == 0) {
            if (!c_dep_write_marker(dest, "mirror\n")) {
                (void)c_dep_remove_entry(dest);
                rc = 1;
            }
        } else {
            (void)c_dep_remove_entry(dest);
        }
        _exit(rc);
    }

    const char *mirror = NULL, *source = NULL, *resolved = NULL;
    if (c_dep_git_checkout(argv, &mirror, &source, &resolved)) {
        if (c_dep_remove_entry(source) != 0 && errno != ENOENT) _exit(127);
        if (mkdir(source, 0755) != 0 && errno != EEXIST) _exit(127);
        int rc = c_dep_exec_real(file, argv, 0);
        if (rc == 0) {
            char marker_text[96];
            int n = snprintf(marker_text, sizeof(marker_text), "%s\n", resolved);
            if (n < 0 || n >= (int)sizeof(marker_text) || !c_dep_write_marker(source, marker_text)) {
                (void)c_dep_remove_entry(source);
                rc = 1;
            } else {
                (void)c_dep_write_marker(mirror, "mirror\n");
            }
        } else {
            (void)c_dep_remove_entry(source);
            (void)c_dep_remove_entry(mirror);
        }
        _exit(rc);
    }

    return c_direct_header_execvp(file, argv);
}

#define stat(path, st) c_dependency_stat((path), (st))
#define fopen c_dependency_fopen
#define fclose c_dependency_fclose
#define execvp c_dependency_execvp

#endif
