static void dep_root(const C_Dependency *d, const DepState *s, char out[PATH_MAX]) {
    if (d->subdir[0]) path_join(out, s->source, d->subdir); else snprintf(out, PATH_MAX, "%s", s->source);
}

static void artifact_name(char out[C_MAX_NAME + 32], const C_Target *t) {
    if (t->kind == C_TARGET_EXECUTABLE || t->kind == C_TARGET_TEST) snprintf(out, C_MAX_NAME + 32, "%s.exe", t->name);
    else if (t->kind == C_TARGET_STATIC_LIBRARY) snprintf(out, C_MAX_NAME + 32, "%s.a", t->name);
    else snprintf(out, C_MAX_NAME + 32, "%s.dll", t->name);
}

static void cbuild_dep_artifact_name(char out[C_MAX_NAME + 32], const C_Dependency *d) {
    if (d->build_target_kind == C_TARGET_STATIC_LIBRARY) snprintf(out, C_MAX_NAME + 32, "%s.a", d->build_target);
    else snprintf(out, C_MAX_NAME + 32, "%s.dll", d->build_target);
}

static void resolve_dependency(C_Dependency *d, const Options *opt, LockFile *lock, DepState *s) {
    char cache[PATH_MAX], gitroot[PATH_MAX], srcroot[PATH_MAX], pkgroot[PATH_MAX];
    cache_root(cache); path_join(gitroot, cache, "git"); path_join(srcroot, cache, "src"); path_join(pkgroot, cache, "pkg");
    mkdir_p(gitroot); mkdir_p(srcroot); mkdir_p(pkgroot);
    char urlkey[17]; hash_hex(hash_string(d->git), urlkey);
    char mirrorname[32]; snprintf(mirrorname, sizeof(mirrorname), "%s.git", urlkey);
    char mirror[PATH_MAX]; path_join(mirror, gitroot, mirrorname);
    if (!is_dir(mirror)) {
        note("FETCH", "%s", d->name);
        char *argv[] = {"git", "clone", "--mirror", "--depth=1", "--no-single-branch", d->git, mirror, NULL};
        if (run_argv(argv, opt->verbose, NULL) != 0) die("failed to clone %s", d->git);
    }
    LockEntry *e = find_lock(lock, d);
    if (!e) {
        char *fetch[] = {"git", "--git-dir", mirror, "fetch", "--depth=1", "--prune", "origin", NULL};
        if (run_argv(fetch, opt->verbose, NULL) != 0) die("failed to fetch %s", d->name);
        char expr[C_MAX_NAME + 32]; snprintf(expr, sizeof(expr), "%s^{commit}", d->ref);
        char *rev[] = {"git", "--git-dir", mirror, "rev-parse", expr, NULL};
        char *resolved = capture_argv(rev, NULL);
        if (!resolved) {
            snprintf(expr, sizeof(expr), "origin/%s^{commit}", d->ref);
            char *rev2[] = {"git", "--git-dir", mirror, "rev-parse", expr, NULL};
            resolved = capture_argv(rev2, NULL);
        }
        if (!resolved) die("cannot resolve %s at %s", d->name, d->ref);
        if (lock->count >= C_MAX_DEPS) die("too many dependencies");
        e = &lock->entries[lock->count++]; memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", d->name); snprintf(e->url, sizeof(e->url), "%s", d->git);
        snprintf(e->requested, sizeof(e->requested), "%s", d->ref); snprintf(e->resolved, sizeof(e->resolved), "%s", resolved); free(resolved);
    }
    snprintf(s->resolved, sizeof(s->resolved), "%s", e->resolved);
    char keyinput[C_MAX_PATH + 128]; snprintf(keyinput, sizeof(keyinput), "%s:%s", d->git, e->resolved);
    char key[17]; hash_hex(hash_string(keyinput), key);
    char srcname[C_MAX_NAME + 32]; snprintf(srcname, sizeof(srcname), "%s-%s", d->name, key); path_join(s->source, srcroot, srcname);
    if (!is_dir(s->source)) {
        mkdir_p(s->source);
        char *co[] = {"git", "--git-dir", mirror, "--work-tree", s->source, "checkout", "-f", e->resolved, "--", ".", NULL};
        if (run_argv(co, opt->verbose, NULL) != 0) { remove_tree(s->source); die("failed to checkout %s", d->name); }
    }
    char pkgname[C_MAX_NAME + 32]; snprintf(pkgname, sizeof(pkgname), "%s-%s", d->name, key); path_join(s->package, pkgroot, pkgname); mkdir_p(s->package);
}

static void resolve_all(C_Build *b, const Options *opt, DepState states[]) {
    LockFile lock; load_lock(&lock);
    for (size_t i = 0; i < b->dep_count; ++i) resolve_dependency(&b->deps[i], opt, &lock, &states[i]);
    save_lock(&lock);
}

static void append_dep_compile_flags(StrVec *a, C_Target *t, C_Build *b, DepState states[]) {
    for (size_t i = 0; i < t->dep_count; ++i) {
        C_Dependency *d = t->deps[i]; ptrdiff_t idx = d - b->deps;
        if (idx < 0 || (size_t)idx >= b->dep_count) die("invalid dependency on target %s", t->name);
        char root[PATH_MAX]; dep_root(d, &states[idx], root);
        if (!d->include_dirs.count) { char x[PATH_MAX + 3]; snprintf(x, sizeof(x), "-I%s", root); vec_push(a, x); }
        else for (size_t j = 0; j < d->include_dirs.count; ++j) { char p[PATH_MAX], x[PATH_MAX + 3]; path_join(p, root, d->include_dirs.items[j]); snprintf(x, sizeof(x), "-I%s", p); vec_push(a, x); }
    }
}

static void object_path(const char *objdir, const char *src, char out[PATH_MAX]) {
    char key[17]; hash_hex(hash_string(src), key);
    char base[96]; snprintf(base, sizeof(base), "%s", path_basename(src));
    for (char *p = base; *p; ++p) if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') *p = '_';
    char name[160]; snprintf(name, sizeof(name), "%s-%s.o", base, key); path_join(out, objdir, name);
}

static bool object_fresh(const char *obj, const char *src) {
    uint64_t o = file_mtime(obj), s = file_mtime(src);
    return o && s && o >= s;
}

static void generated_sources(C_Target *t, const Options *opt) {
    if (t->generated_outputs.count != t->generated_inputs.count || t->generated_outputs.count != t->generated_commands.count)
        die("target %s has an invalid generated-source description", t->name);
    for (size_t i = 0; i < t->generated_outputs.count; ++i) {
        const char *out = t->generated_outputs.items[i], *in = t->generated_inputs.items[i], *cmd = t->generated_commands.items[i];
        bool need = !file_exists(out) || (in[0] && file_mtime(in) > file_mtime(out));
        if (!need) continue;
        ensure_parent(out); note("GEN", "%s", out);
        char *argv[] = {"powershell.exe", "-NoProfile", "-Command", (char *)cmd, NULL};
        if (run_argv(argv, opt->verbose, NULL) != 0) die("generator failed for %s", out);
        if (!file_exists(out)) die("generator did not produce %s", out);
    }
}

static void compiler_flags(StrVec *a, C_Target *t, C_Build *b, DepState states[], const Options *opt, const char *src) {
    vec_push(a, opt->cc);
    bool has_std = false;
    for (size_t i = 0; i < t->cflags.count; ++i) if (!strncmp(t->cflags.items[i], "-std=", 5)) has_std = true;
    if (!has_std) vec_push(a, strstr(src, ".cpp") || strstr(src, ".cc") || strstr(src, ".cxx") ? "-std=c++17" : "-std=c11");
    vec_push(a, opt->release ? "-O2" : "-O0");
    if (!opt->release) vec_push(a, "-g");
    for (size_t i = 0; i < t->includes.count; ++i) { char x[PATH_MAX + 3]; snprintf(x, sizeof(x), "-I%s", t->includes.items[i]); vec_push(a, x); }
    for (size_t i = 0; i < t->defines.count; ++i) { char x[PATH_MAX + 3]; snprintf(x, sizeof(x), "-D%s", t->defines.items[i]); vec_push(a, x); }
    for (size_t i = 0; i < t->cflags.count; ++i) vec_push(a, t->cflags.items[i]);
    append_dep_compile_flags(a, t, b, states);
}

static void build_source_dependency(C_Dependency *d, const Options *opt, DepState *s) {
    if (d->kind != C_DEP_SOURCE) return;
    if (!d->source_patterns.count) die("source dependency %s has no sources", d->name);
    char lib[PATH_MAX]; path_join(lib, s->package, "dependency.a"); snprintf(s->artifact, PATH_MAX, "%s", lib);
    if (file_exists(lib)) return;
    char root[PATH_MAX]; dep_root(d, s, root); char objdir[PATH_MAX]; path_join(objdir, s->package, ".objs"); mkdir_p(objdir);
    StrVec objects = {0};
    for (size_t pi = 0; pi < d->source_patterns.count; ++pi) {
        char pattern[PATH_MAX]; path_join(pattern, root, d->source_patterns.items[pi]); StrVec srcs = {0}; expand_pattern(pattern, &srcs);
        for (size_t i = 0; i < srcs.count; ++i) {
            char obj[PATH_MAX]; object_path(objdir, srcs.items[i], obj); vec_push(&objects, obj);
            StrVec a = {0}; vec_push(&a, opt->cc); vec_push(&a, "-std=c11"); vec_push(&a, opt->release ? "-O2" : "-O0");
            char rootinc[PATH_MAX + 3]; snprintf(rootinc, sizeof(rootinc), "-I%s", root); vec_push(&a, rootinc);
            for (size_t j = 0; j < d->include_dirs.count; ++j) { char p[PATH_MAX], x[PATH_MAX + 3]; path_join(p, root, d->include_dirs.items[j]); snprintf(x, sizeof(x), "-I%s", p); vec_push(&a, x); }
            for (size_t j = 0; j < d->compile_flags.count; ++j) vec_push(&a, d->compile_flags.items[j]);
            vec_push(&a, "-c"); vec_push(&a, srcs.items[i]); vec_push(&a, "-o"); vec_push(&a, obj);
            note("CC", "%s", srcs.items[i]); if (run_vec(&a, opt->verbose, NULL) != 0) die("dependency compile failed: %s", d->name); vec_free(&a);
        }
        vec_free(&srcs);
    }
    StrVec ar = {0}; const char *arcmd = getenv("AR"); if (!arcmd || !*arcmd) arcmd = "ar"; vec_push(&ar, arcmd); vec_push(&ar, "rcs"); vec_push(&ar, lib);
    for (size_t i = 0; i < objects.count; ++i) vec_push(&ar, objects.items[i]); note("AR", "%s", lib); if (run_vec(&ar, opt->verbose, NULL) != 0) die("dependency archive failed: %s", d->name);
    vec_free(&ar); vec_free(&objects);
}

static const char *cmake_cxx_compiler(const Options *opt, char out[PATH_MAX]) {
    const char *env = getenv("CXX");
    if (env && *env) return env;

    char cc[PATH_MAX]; snprintf(cc, sizeof(cc), "%s", opt->cc); slashify(cc);
    const char *base = path_basename(cc);
    const char *candidate = NULL;
    if (!_stricmp(base, "cc.exe") || !_stricmp(base, "cc")) candidate = "c++.exe";
    else if (!_stricmp(base, "gcc.exe") || !_stricmp(base, "gcc")) candidate = "g++.exe";
    else if (!_stricmp(base, "clang.exe") || !_stricmp(base, "clang")) candidate = "clang++.exe";

    if (candidate && (strchr(cc, '/') || strchr(cc, '\\'))) {
        char dir[PATH_MAX]; snprintf(dir, sizeof(dir), "%s", cc); path_dirname(dir); path_join(out, dir, candidate);
        if (file_exists(out)) return out;
    }
    if (command_exists("c++")) return "c++";
    if (command_exists("g++")) return "g++";
    if (command_exists("clang++")) return "clang++";
    return NULL;
}

static uint64_t cmake_dependency_signature(C_Dependency *d, const Options *opt, const char *cxx) {
    uint64_t h = hash_string(opt->cc);
    h = hash_update(h, &opt->release, sizeof(opt->release));
    if (cxx) h = hash_update(h, cxx, strlen(cxx));
    for (size_t i = 0; i < d->compile_flags.count; ++i) h = hash_update(h, d->compile_flags.items[i], strlen(d->compile_flags.items[i]));
    return h;
}

static bool find_named_file_recursive(const char *root, const char *name, char out[PATH_MAX]) {
    if (!is_dir(root)) return false;
    char direct[PATH_MAX]; path_join(direct, root, name);
    if (file_exists(direct)) { snprintf(out, PATH_MAX, "%s", direct); return true; }

    char pattern[PATH_MAX]; path_join(pattern, root, "*");
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char child[PATH_MAX]; path_join(child, root, fd.cFileName);
        if (find_named_file_recursive(child, name, out)) { found = true; break; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

static bool find_cmake_link_artifact(const DepState *s, const char *name, char out[PATH_MAX]) {
    char names[5][C_MAX_NAME + 32];
    snprintf(names[0], sizeof(names[0]), "lib%s.dll.a", name);
    snprintf(names[1], sizeof(names[1]), "lib%s.a", name);
    snprintf(names[2], sizeof(names[2]), "%s.dll.a", name);
    snprintf(names[3], sizeof(names[3]), "%s.a", name);
    snprintf(names[4], sizeof(names[4]), "%s.lib", name);

    char lib[PATH_MAX]; path_join(lib, s->package, "lib");
    for (size_t i = 0; i < C_ARRAY_LEN(names); ++i) {
        char p[PATH_MAX]; path_join(p, lib, names[i]);
        if (file_exists(p)) { snprintf(out, PATH_MAX, "%s", p); return true; }
    }
    if (s->artifact[0]) for (size_t i = 0; i < C_ARRAY_LEN(names); ++i) if (find_named_file_recursive(s->artifact, names[i], out)) return true;
    return false;
}

static void build_cmake_dependency(C_Dependency *d, const Options *opt, DepState *s) {
    if (d->kind != C_DEP_CMAKE) return;

    char cxx_buf[PATH_MAX]; const char *cxx = cmake_cxx_compiler(opt, cxx_buf);
    uint64_t sig = cmake_dependency_signature(d, opt, cxx); char key[17]; hash_hex(sig, key);
    char buildname[64], stampname[64];
    snprintf(buildname, sizeof(buildname), ".build-%s", key);
    snprintf(stampname, sizeof(stampname), ".c-built-%s", key);

    char root[PATH_MAX], builddir[PATH_MAX], stamp[PATH_MAX], libdir[PATH_MAX], bindir[PATH_MAX];
    dep_root(d, s, root); path_join(builddir, s->package, buildname); path_join(stamp, s->package, stampname);
    path_join(libdir, s->package, "lib"); path_join(bindir, s->package, "bin");
    mkdir_p(builddir); mkdir_p(libdir); mkdir_p(bindir); snprintf(s->artifact, PATH_MAX, "%s", builddir);

    if (!file_exists(stamp)) {
        StrVec cm = {0}; vec_push(&cm, "cmake"); vec_push(&cm, "-S"); vec_push(&cm, root); vec_push(&cm, "-B"); vec_push(&cm, builddir); vec_push(&cm, "-G"); vec_push(&cm, "Ninja");
        char type[64]; snprintf(type, sizeof(type), "-DCMAKE_BUILD_TYPE=%s", opt->release ? "Release" : "Debug"); vec_push(&cm, type);
        char pref[PATH_MAX + 64], ccopt[PATH_MAX + 64], cxxopt[PATH_MAX + 64], archiveopt[PATH_MAX + 64], libopt[PATH_MAX + 64], runtimeopt[PATH_MAX + 64];
        snprintf(pref, sizeof(pref), "-DCMAKE_INSTALL_PREFIX=%s", s->package); vec_push(&cm, pref);
        snprintf(ccopt, sizeof(ccopt), "-DCMAKE_C_COMPILER=%s", opt->cc); vec_push(&cm, ccopt);
        if (cxx) { snprintf(cxxopt, sizeof(cxxopt), "-DCMAKE_CXX_COMPILER=%s", cxx); vec_push(&cm, cxxopt); }
        snprintf(archiveopt, sizeof(archiveopt), "-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=%s", libdir); vec_push(&cm, archiveopt);
        snprintf(libopt, sizeof(libopt), "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=%s", libdir); vec_push(&cm, libopt);
        snprintf(runtimeopt, sizeof(runtimeopt), "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=%s", bindir); vec_push(&cm, runtimeopt);
        vec_push(&cm, "-DBUILD_SHARED_LIBS=OFF");
        for (size_t i = 0; i < d->compile_flags.count; ++i) vec_push(&cm, d->compile_flags.items[i]);
        note("DEP", "%s", d->name); if (run_vec(&cm, opt->verbose, NULL) != 0) die("cmake configure failed for %s", d->name); vec_free(&cm);
        char *build[] = {"cmake", "--build", builddir, "--config", opt->release ? "Release" : "Debug", NULL}; if (run_argv(build, opt->verbose, NULL) != 0) die("cmake build failed for %s", d->name);
        char *install[] = {"cmake", "--install", builddir, "--config", opt->release ? "Release" : "Debug", NULL}; if (run_argv(install, opt->verbose, NULL) != 0) die("cmake install failed for %s", d->name);
        write_file(stamp, s->resolved, false);
    }
}

static void build_cbuild_dependency(C_Dependency *d, const Options *opt, DepState *s) {
    if (d->kind != C_DEP_CBUILD) return;
    char root[PATH_MAX]; dep_root(d, s, root); char exe[PATH_MAX]; if (!executable_path(exe)) die("cannot find current c executable");
    StrVec a = {0}; vec_push(&a, exe); vec_push(&a, "build"); vec_push(&a, d->build_target); if (opt->release) vec_push(&a, "--release"); vec_push(&a, "--cc"); vec_push(&a, opt->cc);
    note("DEP", "%s", d->name); if (run_vec(&a, opt->verbose, root) != 0) die("cbuild target build failed for %s", d->name); vec_free(&a);
    char dir[PATH_MAX], name[C_MAX_NAME + 32]; path_join(dir, root, opt->release ? "build/release" : "build/debug"); cbuild_dep_artifact_name(name, d); path_join(s->artifact, dir, name);
    if (!file_exists(s->artifact)) die("cbuild dependency did not produce %s", s->artifact);
}

static void prepare_dependencies(C_Build *b, const Options *opt, DepState states[]) {
    resolve_all(b, opt, states);
    for (size_t i = 0; i < b->dep_count; ++i) {
        build_source_dependency(&b->deps[i], opt, &states[i]);
        build_cmake_dependency(&b->deps[i], opt, &states[i]);
        build_cbuild_dependency(&b->deps[i], opt, &states[i]);
    }
}

static void append_link_flags(StrVec *a, C_Target *t, C_Build *b, DepState states[]) {
    for (size_t i = 0; i < t->dep_count; ++i) {
        C_Dependency *d = t->deps[i]; ptrdiff_t idx = d - b->deps; if (idx < 0 || (size_t)idx >= b->dep_count) die("invalid dependency");
        DepState *s = &states[idx];
        if (d->kind == C_DEP_SOURCE || d->kind == C_DEP_CBUILD) { if (s->artifact[0]) vec_push(a, s->artifact); }
        else if (d->kind == C_DEP_CMAKE) {
            for (size_t j = 0; j < d->source_patterns.count; ++j) {
                char artifact[PATH_MAX];
                if (!find_cmake_link_artifact(s, d->source_patterns.items[j], artifact)) die("cmake dependency %s did not produce link library %s", d->name, d->source_patterns.items[j]);
                vec_push(a, artifact);
            }
        }
    }
    for (size_t i = 0; i < t->system_links.count; ++i) { char x[C_MAX_NAME + 3]; snprintf(x, sizeof(x), "-l%s", t->system_links.items[i]); vec_push(a, x); }
    for (size_t i = 0; i < t->ldflags.count; ++i) vec_push(a, t->ldflags.items[i]);
}

static void copy_cmake_runtime_dependencies(C_Target *t, C_Build *b, DepState states[], const char *output) {
    char outdir[PATH_MAX]; snprintf(outdir, sizeof(outdir), "%s", output); path_dirname(outdir);
    for (size_t i = 0; i < t->dep_count; ++i) {
        C_Dependency *d = t->deps[i]; if (d->kind != C_DEP_CMAKE) continue;
        ptrdiff_t idx = d - b->deps; if (idx < 0 || (size_t)idx >= b->dep_count) die("invalid dependency");
        char bindir[PATH_MAX]; path_join(bindir, states[idx].package, "bin"); if (!is_dir(bindir)) continue;
        char pattern[PATH_MAX]; path_join(pattern, bindir, "*.dll"); WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pattern, &fd); if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char src[PATH_MAX], dst[PATH_MAX]; path_join(src, bindir, fd.cFileName); path_join(dst, outdir, fd.cFileName);
            if (_stricmp(src, dst)) copy_file_or_die(src, dst);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

static C_Target *select_target(C_Build *b, const Options *opt) {
    if (opt->target) for (size_t i = 0; i < b->target_count; ++i) if (!strcmp(b->targets[i].name, opt->target)) return &b->targets[i];
    if (opt->target) die("unknown target: %s", opt->target);
    if (b->default_target >= 0 && (size_t)b->default_target < b->target_count) return &b->targets[b->default_target];
    if (!b->target_count) die("build.c defines no targets");
    return &b->targets[0];
}

static char *build_target_graph(C_Build *b, C_Target *t, DepState states[], const Options *opt, unsigned char mark[], char *outputs[]);

static char *build_one(C_Build *b, C_Target *t, DepState states[], const Options *opt, char *outputs[]) {
    generated_sources(t, opt);
    StrVec sources = {0}, objects = {0};
    for (size_t i = 0; i < t->sources.count; ++i) expand_pattern(t->sources.items[i], &sources);
    if (!sources.count) die("target %s has no sources", t->name);
    uint64_t sig = hash_string(t->name); sig = hash_update(sig, &t->kind, sizeof(t->kind)); sig = hash_update(sig, opt->cc, strlen(opt->cc));
    for (size_t i = 0; i < sources.count; ++i) sig = hash_update(sig, sources.items[i], strlen(sources.items[i]));
    char key[17]; hash_hex(sig, key); char objdir[PATH_MAX]; snprintf(objdir, sizeof(objdir), "build/.objs/%s", key); mkdir_p(objdir);

    typedef struct Task { Proc p; char *src; char *obj; StrVec cmd; bool active; } Task;
    Task *tasks = calloc(sources.count, sizeof(*tasks)); if (!tasks) die("out of memory"); size_t task_count = 0;
    for (size_t i = 0; i < sources.count; ++i) {
        char obj[PATH_MAX]; object_path(objdir, sources.items[i], obj); vec_push(&objects, obj);
        if (object_fresh(obj, sources.items[i])) continue;
        Task *task = &tasks[task_count++]; task->src = xstrdup(sources.items[i]); task->obj = xstrdup(obj);
        compiler_flags(&task->cmd, t, b, states, opt, sources.items[i]); vec_push(&task->cmd, "-c"); vec_push(&task->cmd, sources.items[i]); vec_push(&task->cmd, "-o"); vec_push(&task->cmd, obj);
    }
    size_t next = 0, running = 0, finished = 0; size_t limit = (size_t)(opt->jobs > 0 ? opt->jobs : 1); if (limit > task_count) limit = task_count;
    while (finished < task_count) {
        while (next < task_count && running < limit) {
            Task *task = &tasks[next]; note("CC", "%s", task->src); if (!spawn_process(task->cmd.items, NULL, NULL, opt->verbose, &task->p)) die("cannot start compiler for %s", task->src); task->active = true; ++next; ++running;
        }
        HANDLE hs[MAXIMUM_WAIT_OBJECTS]; size_t slots[MAXIMUM_WAIT_OBJECTS]; DWORD n = 0;
        for (size_t i = 0; i < task_count && n < MAXIMUM_WAIT_OBJECTS; ++i) if (tasks[i].active) { hs[n] = tasks[i].p.handle; slots[n] = i; ++n; }
        DWORD w = WaitForMultipleObjects(n, hs, FALSE, INFINITE); if (w < WAIT_OBJECT_0 || w >= WAIT_OBJECT_0 + n) die("wait for compiler failed");
        Task *task = &tasks[slots[w - WAIT_OBJECT_0]]; int rc = wait_process(&task->p); task->active = false; --running; ++finished; if (rc != 0) die("compile failed: %s", task->src);
    }
    for (size_t i = 0; i < task_count; ++i) { vec_free(&tasks[i].cmd); free(tasks[i].src); free(tasks[i].obj); } free(tasks);

    char profile[PATH_MAX]; snprintf(profile, sizeof(profile), "build/%s", opt->release ? "release" : "debug"); mkdir_p(profile);
    char name[C_MAX_NAME + 32]; artifact_name(name, t); char *output = malloc(PATH_MAX); if (!output) die("out of memory"); path_join(output, profile, name);
    bool relink = !file_exists(output) || task_count > 0; uint64_t ot = file_mtime(output); for (size_t i = 0; i < objects.count; ++i) if (file_mtime(objects.items[i]) > ot) relink = true;
    if (relink) {
        if (t->kind == C_TARGET_STATIC_LIBRARY) {
            StrVec a = {0}; const char *ar = getenv("AR"); if (!ar || !*ar) ar = "ar"; vec_push(&a, ar); vec_push(&a, "rcs"); vec_push(&a, output); for (size_t i = 0; i < objects.count; ++i) vec_push(&a, objects.items[i]);
            note("AR", "%s", output); if (run_vec(&a, opt->verbose, NULL) != 0) die("archive failed"); vec_free(&a);
        } else {
            StrVec a = {0}; vec_push(&a, opt->cc); if (t->kind == C_TARGET_SHARED_LIBRARY) vec_push(&a, "-shared"); for (size_t i = 0; i < objects.count; ++i) vec_push(&a, objects.items[i]);
            for (size_t i = 0; i < t->target_dep_count; ++i) { ptrdiff_t idx = t->target_deps[i] - b->targets; if (idx >= 0 && (size_t)idx < b->target_count && outputs[idx]) vec_push(&a, outputs[idx]); }
            append_link_flags(&a, t, b, states); vec_push(&a, "-o"); vec_push(&a, output); note("LINK", "%s", output); if (run_vec(&a, opt->verbose, NULL) != 0) die("link failed"); vec_free(&a);
        }
    } else note("CACHED", "%s", t->name);
    if (t->kind == C_TARGET_EXECUTABLE || t->kind == C_TARGET_TEST || t->kind == C_TARGET_SHARED_LIBRARY) copy_cmake_runtime_dependencies(t, b, states, output);
    vec_free(&sources); vec_free(&objects); return output;
}

static char *build_target_graph(C_Build *b, C_Target *t, DepState states[], const Options *opt, unsigned char mark[], char *outputs[]) {
    ptrdiff_t idx = t - b->targets; if (idx < 0 || (size_t)idx >= b->target_count) die("invalid target graph");
    if (mark[idx] == 1) die("cyclic target dependency involving %s", t->name);
    if (mark[idx] == 2) return xstrdup(outputs[idx]);
    mark[idx] = 1;
    for (size_t i = 0; i < t->target_dep_count; ++i) { char *d = build_target_graph(b, t->target_deps[i], states, opt, mark, outputs); free(d); }
    outputs[idx] = build_one(b, t, states, opt, outputs); mark[idx] = 2; return xstrdup(outputs[idx]);
}

static void free_outputs(C_Build *b, char *outputs[]) { for (size_t i = 0; i < b->target_count; ++i) free(outputs[i]); }
