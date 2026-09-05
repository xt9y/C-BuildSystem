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

static void compiler_cbuild_resolve_dependency(const C_Dependency *d, const Options *opt,
                                               LockFile *lock, DepState *state, bool build_artifacts) {
    resolve_dependency(d, opt, lock, state, build_artifacts);
    if (d->kind != C_DEP_CBUILD) return;

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

#endif
