#ifndef C_DEPENDENCY_CACHE_CLEAN_GUARD_H
#define C_DEPENDENCY_CACHE_CLEAN_GUARD_H

/*
 * dependency_cache_guard.h stores readiness metadata beside each cached Git
 * mirror/source directory. The legacy remove_tree() implementation may visit
 * the marker before the directory, so deleting it immediately would make the
 * directory appear invalid and skip its removal. Defer those marker unlinks
 * until the containing cache bucket is being removed.
 */

static int c_dep_is_ready_marker_path(const char *path) {
    if (!path) return 0;
    size_t len = strlen(path);
    size_t suffix = sizeof(C_DEP_READY_SUFFIX) - 1;
    if (len <= suffix || strcmp(path + len - suffix, C_DEP_READY_SUFFIX)) return 0;

    char entry[PATH_MAX];
    if (len - suffix >= sizeof(entry)) return 0;
    memcpy(entry, path, len - suffix);
    entry[len - suffix] = '\0';
    return c_dep_is_direct_cache_entry(entry, "src") ||
           c_dep_is_direct_cache_entry(entry, "git");
}

static int c_dep_is_cache_bucket(const char *path, const char *kind) {
    char root[PATH_MAX], bucket[PATH_MAX];
    if (!c_dep_cache_root(root)) return 0;
    int n = snprintf(bucket, sizeof(bucket), "%s/%s", root, kind);
    return n >= 0 && n < (int)sizeof(bucket) && !strcmp(path, bucket);
}

static int c_dependency_unlink(const char *path) {
    if (c_dep_is_ready_marker_path(path)) return 0;
    return unlink(path);
}

static void c_dep_remove_bucket_markers(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        size_t len = strlen(ent->d_name);
        size_t suffix = sizeof(C_DEP_READY_SUFFIX) - 1;
        if (len <= suffix || strcmp(ent->d_name + len - suffix, C_DEP_READY_SUFFIX)) continue;

        char child[PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n >= 0 && n < (int)sizeof(child)) (void)unlink(child);
    }
    closedir(dir);
}

static int c_dependency_rmdir(const char *path) {
    if (c_dep_is_cache_bucket(path, "src") || c_dep_is_cache_bucket(path, "git"))
        c_dep_remove_bucket_markers(path);
    return rmdir(path);
}

#define unlink(path) c_dependency_unlink((path))
#define rmdir(path) c_dependency_rmdir((path))

#endif
