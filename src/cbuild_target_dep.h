#ifndef C_CBUILD_TARGET_DEP_H
#define C_CBUILD_TARGET_DEP_H

/*
 * Adapter for consuming another C-BuildSystem project as a library dependency.
 * This header is included from perf_v2.h after src/main.c has provided the
 * dependency checkout/process helpers, and before src/cli.c resolves/links
 * targets. It deliberately wraps those existing primitives instead of adding
 * a second dependency pipeline.
 */

static void compiler_cbuild_artifact_name(char out[C_MAX_NAME + 32], const char *name, C_TargetKind kind) {
    if (kind == C_TARGET_STATIC_LIBRARY) {
        snprintf(out, C_MAX_NAME + 32, "%s.a", name);
        return;
    }
    if (kind == C_TARGET_SHARED_LIBRARY) {
#ifdef __APPLE__
        snprintf(out, C_MAX_NAME + 32, "lib%s.dylib", name);
#else
        snprintf(out, C_MAX_NAME + 32, "lib%s.so", name);
#endif
        return;
    }
    die("unsupported C-BuildSystem dependency target kind");
}

static void compiler_cbuild_project_root(const C_Dependency *d, const DepState *state, char out[PATH_MAX]) {
    if (d->subdir[0]) path_join(out, state->source, d->subdir);
    else c__copy(out, PATH_MAX, state->source);
}

static void compiler_cbuild_artifact_path(const C_Dependency *d, const DepState *state,
                                          const Options *opt, char out[PATH_MAX]) {
    char root[PATH_MAX], build_dir[PATH_MAX], profile_dir[PATH_MAX], name[C_MAX_NAME + 32];
    compiler_cbuild_project_root(d, state, root);
    path_join(build_dir, root, "build");
    path_join(profile_dir, build_dir, opt->release ? "release" : "debug");
    compiler_cbuild_artifact_name(name, d->build_target, d->build_target_kind);
    path_join(out, profile_dir, name);
}

static bool compiler_cbuild_list_contains(const C_StringList *list, const char *value) {
    if (!list || !value) return false;
    for (size_t i = 0; i < list->count; ++i)
        if (!strcmp(list->items[i], value)) return true;
    return false;
}

static C_Target *compiler_cbuild_find_target(C_Build *build, const char *name) {
    if (!build || !name) return NULL;
    for (size_t i = 0; i < build->target_count; ++i)
        if (!strcmp(build->targets[i].name, name)) return &build->targets[i];
    return NULL;
}

static void compiler_cbuild_import_target_includes(C_Dependency *d, const DepState *state, const Options *opt) {
    char root[PATH_MAX], previous_cwd[PATH_MAX];
    compiler_cbuild_project_root(d, state, root);
    if (!getcwd(previous_cwd, sizeof(previous_cwd))) die("getcwd failed while importing usage for %s", d->name);
    if (chdir(root) != 0) die("cannot enter cbuild dependency %s: %s", d->name, strerror(errno));

    C_Build *child = alloc_build();
    load_build(opt, child);
    C_Target *target = compiler_cbuild_find_target(child, d->build_target);
    if (!target) die("cbuild dependency %s does not define target %s", d->name, d->build_target);
    if (target->kind != d->build_target_kind)
        die("cbuild dependency %s target %s has an unexpected target kind", d->name, d->build_target);

    for (size_t i = 0; i < target->includes.count; ++i) {
        const char *include = target->includes.items[i];
        if (!compiler_cbuild_list_contains(&d->include_dirs, include)) c__push(&d->include_dirs, include);
    }

    free_build(child);
    if (chdir(previous_cwd) != 0) die("cannot restore working directory after importing usage for %s", d->name);
}

static bool compiler_cbuild_absolute_path(const char *path) {
    if (!path || !path[0]) return false;
    if (path[0] == '/') return true;
    return ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
           path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

static void compiler_cbuild_path_join(char out[PATH_MAX], const char *a, const char *b) {
    if (compiler_cbuild_absolute_path(b)) c__copy(out, PATH_MAX, b);
    else path_join(out, a, b);
}

static void compiler_asset_normalize_path(char path[PATH_MAX]) {
    for (char *p = path; *p; ++p) if (*p == '\\') *p = '/';
}

static void compiler_stage_dependency_assets(const C_Dependency *d, const DepState *state) {
    if (!d->links.count) return;
    if (d->links.count % 2 != 0) die("dependency %s has an invalid asset mapping", d->name);

    char root[PATH_MAX];
    compiler_cbuild_project_root(d, state, root);

    for (size_t i = 0; i < d->links.count; i += 2) {
        char relative_source[PATH_MAX], source[PATH_MAX], destination[PATH_MAX];
        c__copy(relative_source, sizeof(relative_source), d->links.items[i]);
        c__copy(destination, sizeof(destination), d->links.items[i + 1]);
        compiler_asset_normalize_path(relative_source);
        compiler_asset_normalize_path(destination);
        path_join(source, root, relative_source);

        if (!file_exists(source) || is_dir(source))
            die("dependency asset not found for %s: %s", d->name, relative_source);

        uint64_t source_hash = hash_file_seed(1469598103934665603ULL, source);
        if (file_exists(destination) && !is_dir(destination)) {
            uint64_t destination_hash = hash_file_seed(1469598103934665603ULL, destination);
            if (source_hash == destination_hash) continue;
        }

        char parent[PATH_MAX];
        c__copy(parent, sizeof(parent), destination);
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = '\0';
            if (parent[0]) mkdir_p(parent);
        }

        note("ASSET", "%s -> %s", d->name, destination);
        copy_file(source, destination);
        if (!file_exists(destination) || is_dir(destination))
            die("failed to stage dependency asset for %s: %s", d->name, destination);
        uint64_t staged_hash = hash_file_seed(1469598103934665603ULL, destination);
        if (source_hash != staged_hash)
            die("staged dependency asset differs from source for %s: %s", d->name, destination);
    }
}

static void compiler_cbuild_resolve_dependency(const C_Dependency *d, const Options *opt,
                                               LockFile *lock, DepState *state, bool build_artifacts) {
    resolve_dependency(d, opt, lock, state, build_artifacts);
    compiler_stage_dependency_assets(d, state);
    if (d->kind != C_DEP_CBUILD) return;

    C_Dependency *mutable_dependency = (C_Dependency *)d;
    compiler_cbuild_import_target_includes(mutable_dependency, state, opt);

    char root[PATH_MAX], artifact[PATH_MAX], executable[PATH_MAX];
    compiler_cbuild_project_root(d, state, root);
    compiler_cbuild_artifact_path(d, state, opt, artifact);

    if (!executable_path(executable)) die("cannot resolve current c executable");

    StrVec command = {0};
    vec_push(&command, executable);
    vec_push(&command, "build");
    vec_push(&command, d->build_target);
    if (opt->release) vec_push(&command, "--release");
    vec_push(&command, "--cc");
    vec_push(&command, opt->cc);

    note("DEP", "%s", d->name);
    int rc = run_process(&command, opt->verbose, root);
    vec_free(&command);
    if (rc != 0) die("cbuild target build failed for %s", d->name);
    if (!file_exists(artifact)) die("cbuild target %s did not produce %s", d->build_target, artifact);

    /*
     * target_signature() already hashes DepState::package and ::resolved.
     * Point package at the concrete imported artifact and fold its content
     * into resolved so relinking cannot stay stale when the nested build
     * changes while its Git revision remains the same.
     */
    c__copy(state->package, sizeof(state->package), artifact);
    uint64_t artifact_hash = hash_file_seed(1469598103934665603ULL, artifact);
    char artifact_hex[17], resolved[sizeof(state->resolved)];
    hash_u64_hex(artifact_hash, artifact_hex);
    int n = snprintf(resolved, sizeof(resolved), "%s:%s", state->resolved, artifact_hex);
    if (n < 0 || n >= (int)sizeof(resolved)) die("dependency signature too long for %s", d->name);
    c__copy(state->resolved, sizeof(state->resolved), resolved);
}

static void compiler_cbuild_append_link_flags(StrVec *a, const C_Target *t, C_Build *b, DepState states[]) {
    for (size_t i = 0; i < t->dep_count; ++i) {
        C_Dependency *d = t->deps[i];
        if (d->kind != C_DEP_CBUILD) continue;

        ptrdiff_t dep_index = d - b->deps;
        if (dep_index < 0 || (size_t)dep_index >= b->dep_count)
            die("target %s has invalid dependency", t->name);

        const char *artifact = states[dep_index].package;
        if (!artifact[0] || !file_exists(artifact))
            die("cbuild target artifact missing for %s", d->name);

        vec_push(a, artifact);
        if (d->build_target_kind == C_TARGET_SHARED_LIBRARY) {
            char directory[PATH_MAX], rpath[PATH_MAX + 32];
            c__copy(directory, sizeof(directory), artifact);
            char *slash = strrchr(directory, '/');
            if (!slash) die("invalid cbuild target artifact path for %s", d->name);
            *slash = '\0';
            int n = snprintf(rpath, sizeof(rpath), "-Wl,-rpath,%s", directory);
            if (n < 0 || n >= (int)sizeof(rpath)) die("rpath too long for dependency %s", d->name);
            vec_push(a, rpath);
        }
    }

    append_link_flags(a, t, b, states);
}

/* Affect only the compiler pipeline that appears after perf_v2.h in cli.c. */
#define resolve_dependency compiler_cbuild_resolve_dependency
#define append_link_flags compiler_cbuild_append_link_flags
#define path_join compiler_cbuild_path_join

#endif
