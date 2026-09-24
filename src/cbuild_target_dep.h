#ifndef C_CBUILD_TARGET_DEP_H
#define C_CBUILD_TARGET_DEP_H

/*
 * Adapter for consuming another C-BuildSystem project as a library dependency.
 * This header is included from perf_v2.h after src/main.c has provided the
 * dependency checkout/process helpers, and before src/cli.c resolves/links
 * targets. It deliberately wraps those existing primitives instead of adding
 * a second dependency pipeline.
 */

#define COMPILER_DEP_CMAKE ((C_DepKind)1)

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

static bool compiler_asset_files_equal(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb");
    if (!fa) return false;
    FILE *fb = fopen(b, "rb");
    if (!fb) { fclose(fa); return false; }
    unsigned char ba[8192], bb[8192];
    bool equal = true;
    for (;;) {
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || (na && memcmp(ba, bb, na))) { equal = false; break; }
        if (!na) { if (ferror(fa) || ferror(fb)) equal = false; break; }
    }
    fclose(fa);
    fclose(fb);
    return equal;
}

static void compiler_asset_publish_temp(const char *temp, const char *final) {
    if (compiler_asset_files_equal(temp, final)) {
        if (unlink(temp) != 0) die("cannot remove temporary asset cache file %s: %s", temp, strerror(errno));
        return;
    }
    if (rename(temp, final) != 0) {
        int saved = errno;
        unlink(temp);
        errno = saved;
        die("cannot publish asset cache file %s: %s", final, strerror(errno));
    }
}

static void compiler_asset_project_cache(char out[PATH_MAX]) {
    char cache[PATH_MAX], assets[PATH_MAX], cwd[PATH_MAX], key[17];
    if (!getcwd(cwd, sizeof(cwd))) die("cannot determine project directory for dependency assets");
    cache_root(cache);
    path_join(assets, cache, "assets");
    hash_hex(cwd, key);
    path_join(out, assets, key);
    mkdir_p(out);
}

static void compiler_asset_manifest_path(const char *project_cache, const char *dependency, char out[PATH_MAX]) {
    char key[17], name[32];
    hash_hex(dependency, key);
    if (snprintf(name, sizeof(name), "%s.bin", key) >= (int)sizeof(name))
        die("asset manifest name too long for %s", dependency);
    path_join(out, project_cache, name);
}

static void compiler_asset_write_c_string(FILE *file, const char *value) {
    fputc('"', file);
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '\\' || *p == '"') fputc('\\', file);
        if (*p == '\n') fputs("\\n", file);
        else if (*p == '\r') fputs("\\r", file);
        else if (*p == '\t') fputs("\\t", file);
        else fputc(*p, file);
    }
    fputc('"', file);
}

static void compiler_asset_runtime_header(const char *project_cache, char include_dir[PATH_MAX]) {
    char header[PATH_MAX], temp[PATH_MAX], root[PATH_MAX];
    path_join(include_dir, project_cache, "include");
    mkdir_p(include_dir);
    path_join(header, include_dir, "casset.h");
    if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", header, (long)getpid()) >= (int)sizeof(temp))
        die("asset runtime header path too long");

    c__copy(root, sizeof(root), project_cache);
    compiler_asset_normalize_path(root);

    FILE *file = fopen(temp, "wb");
    if (!file) die("cannot create asset runtime header %s: %s", temp, strerror(errno));
    fputs(
        "#ifndef C_ASSET_RUNTIME_H\n"
        "#define C_ASSET_RUNTIME_H\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "#include <stdio.h>\n"
        "#include <string.h>\n"
        "#ifndef C_ASSET_PATH_MAX\n"
        "#define C_ASSET_PATH_MAX 4096\n"
        "#endif\n"
        "#if defined(__cplusplus)\n"
        "#define C_ASSET_THREAD_LOCAL thread_local\n"
        "#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L\n"
        "#define C_ASSET_THREAD_LOCAL _Thread_local\n"
        "#else\n"
        "#define C_ASSET_THREAD_LOCAL\n"
        "#endif\n"
        "#define C_ASSET_MANIFEST_ROOT ", file);
    compiler_asset_write_c_string(file, root);
    fputs(
        "\n"
        "static inline uint64_t c_asset__hash(const char *text) {\n"
        "    uint64_t h = 1469598103934665603ULL;\n"
        "    const unsigned char *p = (const unsigned char *)text;\n"
        "    while (*p) { h ^= *p++; h *= 1099511628211ULL; }\n"
        "    return h;\n"
        "}\n"
        "static inline int c_asset__read0(FILE *file, char *out, size_t cap) {\n"
        "    size_t n = 0; int ch;\n"
        "    while ((ch = fgetc(file)) != EOF) {\n"
        "        if (ch == 0) { if (cap) out[n < cap ? n : cap - 1] = '\\0'; return n < cap ? 1 : -1; }\n"
        "        if (n + 1 < cap) out[n] = (char)ch;\n"
        "        ++n;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "static inline const char *c_asset(const char *dependency, const char *logical_path) {\n"
        "    if (!dependency || !logical_path) return NULL;\n"
        "    C_ASSET_THREAD_LOCAL static char physical[C_ASSET_PATH_MAX];\n"
        "    char manifest[C_ASSET_PATH_MAX], logical[C_ASSET_PATH_MAX];\n"
        "    int n = snprintf(manifest, sizeof(manifest), \"%s/%016llx.bin\", C_ASSET_MANIFEST_ROOT,\n"
        "                     (unsigned long long)c_asset__hash(dependency));\n"
        "    if (n < 0 || n >= (int)sizeof(manifest)) return NULL;\n"
        "    FILE *file = fopen(manifest, \"rb\");\n"
        "    if (!file) return NULL;\n"
        "    for (;;) {\n"
        "        int left = c_asset__read0(file, logical, sizeof(logical));\n"
        "        if (left <= 0) break;\n"
        "        int right = c_asset__read0(file, physical, sizeof(physical));\n"
        "        if (right <= 0) break;\n"
        "        if (!strcmp(logical, logical_path)) { fclose(file); return physical; }\n"
        "    }\n"
        "    fclose(file);\n"
        "    return NULL;\n"
        "}\n"
        "#undef C_ASSET_MANIFEST_ROOT\n"
        "#undef C_ASSET_THREAD_LOCAL\n"
        "#endif\n", file);
    bool ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    if (!ok) { unlink(temp); die("cannot finish asset runtime header %s", temp); }
    compiler_asset_publish_temp(temp, header);
}

static void compiler_publish_dependency_assets(C_Dependency *d, const DepState *state) {
    if (!d->links.count) return;
    if (d->links.count % 2 != 0) die("dependency %s has an invalid asset mapping", d->name);

    char project_cache[PATH_MAX], include_dir[PATH_MAX], manifest[PATH_MAX], temp[PATH_MAX], root[PATH_MAX];
    compiler_asset_project_cache(project_cache);
    compiler_asset_runtime_header(project_cache, include_dir);
    compiler_asset_manifest_path(project_cache, d->name, manifest);
    if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", manifest, (long)getpid()) >= (int)sizeof(temp))
        die("asset manifest path too long for %s", d->name);
    compiler_cbuild_project_root(d, state, root);

    FILE *file = fopen(temp, "wb");
    if (!file) die("cannot create asset manifest for %s: %s", d->name, strerror(errno));
    for (size_t i = 0; i < d->links.count; i += 2) {
        char relative_source[PATH_MAX], logical[PATH_MAX], physical[PATH_MAX];
        c__copy(relative_source, sizeof(relative_source), d->links.items[i]);
        c__copy(logical, sizeof(logical), d->links.items[i + 1]);
        compiler_asset_normalize_path(relative_source);
        compiler_asset_normalize_path(logical);
        path_join(physical, root, relative_source);
        compiler_asset_normalize_path(physical);

        if (!file_exists(physical) || is_dir(physical)) {
            fclose(file);
            unlink(temp);
            die("dependency asset not found for %s: %s", d->name, relative_source);
        }
        if (strchr(logical, '\n') || strchr(logical, '\r') || strchr(logical, '\t')) {
            fclose(file);
            unlink(temp);
            die("dependency asset logical path contains unsupported control characters for %s", d->name);
        }
        if (fwrite(logical, 1, strlen(logical) + 1, file) != strlen(logical) + 1 ||
            fwrite(physical, 1, strlen(physical) + 1, file) != strlen(physical) + 1) {
            fclose(file);
            unlink(temp);
            die("cannot write asset manifest for %s", d->name);
        }
    }
    bool ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    if (!ok) { unlink(temp); die("cannot finish asset manifest for %s", d->name); }
    compiler_asset_publish_temp(temp, manifest);

    /* Adding an absolute generated include must not replace the dependency's
       normal root include when no explicit include directories were declared. */
    if (!d->include_dirs.count) c__push(&d->include_dirs, ".");
    if (!compiler_cbuild_list_contains(&d->include_dirs, include_dir)) c__push(&d->include_dirs, include_dir);
}

static void compiler_cbuild_resolve_dependency(const C_Dependency *d, const Options *opt,
                                               LockFile *lock, DepState *state, bool build_artifacts) {
    const bool cmake_dependency = d->kind == COMPILER_DEP_CMAKE;
    resolve_dependency(d, opt, lock, state, build_artifacts || cmake_dependency);
    compiler_publish_dependency_assets((C_Dependency *)d, state);
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

static void compiler_cmake_append_library_dir(StrVec *a, const char *dir) {
    if (!is_dir(dir)) return;
    char flag[PATH_MAX + 32];
    int n = snprintf(flag, sizeof(flag), "-L%s", dir);
    if (n < 0 || n >= (int)sizeof(flag)) die("CMake dependency library path too long");
    vec_push(a, flag);
    n = snprintf(flag, sizeof(flag), "-Wl,-rpath,%s", dir);
    if (n < 0 || n >= (int)sizeof(flag)) die("CMake dependency rpath too long");
    vec_push(a, flag);
}

static void compiler_cmake_append_link_flags(StrVec *a, const C_Dependency *d,
                                             const DepState *state) {
    char lib[PATH_MAX], lib64[PATH_MAX], root[PATH_MAX], bin[PATH_MAX];
    path_join(lib, state->package, "lib");
    path_join(lib64, state->package, "lib64");
    compiler_cbuild_project_root(d, state, root);
    path_join(bin, root, "_Bin");

    compiler_cmake_append_library_dir(a, lib);
    compiler_cmake_append_library_dir(a, lib64);
    compiler_cmake_append_library_dir(a, bin);

    for (size_t i = 0; i < d->source_patterns.count; ++i) {
        char flag[C_MAX_NAME + 3];
        int n = snprintf(flag, sizeof(flag), "-l%s", d->source_patterns.items[i]);
        if (n < 0 || n >= (int)sizeof(flag)) die("CMake dependency library name too long for %s", d->name);
        vec_push(a, flag);
    }
}

static void compiler_cbuild_append_link_flags(StrVec *a, const C_Target *t, C_Build *b, DepState states[]) {
    for (size_t i = 0; i < t->dep_count; ++i) {
        C_Dependency *d = t->deps[i];
        ptrdiff_t dep_index = d - b->deps;
        if (dep_index < 0 || (size_t)dep_index >= b->dep_count)
            die("target %s has invalid dependency", t->name);

        if (d->kind == COMPILER_DEP_CMAKE) {
            compiler_cmake_append_link_flags(a, d, &states[dep_index]);
            continue;
        }
        if (d->kind != C_DEP_CBUILD) continue;

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

    for (size_t i = 0; i < t->system_links.count; ++i) {
        char flag[C_MAX_NAME + 3];
        snprintf(flag, sizeof(flag), "-l%s", t->system_links.items[i]);
        vec_push(a, flag);
    }
#ifdef __APPLE__
    for (size_t i = 0; i < t->frameworks.count; ++i) {
        vec_push(a, "-framework");
        vec_push(a, t->frameworks.items[i]);
    }
#endif
    for (size_t i = 0; i < t->ldflags.count; ++i) vec_push(a, t->ldflags.items[i]);
}

/* Affect only the compiler pipeline that appears after perf_v2.h in cli.c. */
#define resolve_dependency compiler_cbuild_resolve_dependency
#define append_link_flags compiler_cbuild_append_link_flags
#define path_join compiler_cbuild_path_join

/* The public 1.x reserved slot is now the CMake dependency kind. The legacy
 * rejection in cli.c appears after this header, so hide only that sentinel. */
#define C_DEP_RESERVED ((C_DepKind)-1)

#endif
