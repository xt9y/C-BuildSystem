#include "windows_platform.h"
#include "windows_build.h"

static int command_build(const Options *opt, bool run) {
    C_Build *b = load_build(opt); DepState states[C_MAX_DEPS] = {0}; prepare_dependencies(b, opt, states);
    unsigned char mark[C_MAX_TARGETS] = {0}; char *outputs[C_MAX_TARGETS] = {0}; C_Target *t = select_target(b, opt); char *output = build_target_graph(b, t, states, opt, mark, outputs);
    int rc = 0;
    if (run) {
        if (t->kind != C_TARGET_EXECUTABLE && t->kind != C_TARGET_TEST) die("target %s is not executable", t->name);
        note("RUN", "%s", output); StrVec a = {0}; vec_push(&a, output); for (int i = 0; i < opt->run_argc; ++i) vec_push(&a, opt->run_argv[i]); rc = run_vec(&a, opt->verbose, NULL); vec_free(&a);
    }
    free(output); free_outputs(b, outputs); free_build(b); return rc;
}

static int command_test(const Options *opt) {
    C_Build *b = load_build(opt); DepState states[C_MAX_DEPS] = {0}; prepare_dependencies(b, opt, states);
    unsigned char mark[C_MAX_TARGETS] = {0}; char *outputs[C_MAX_TARGETS] = {0}; size_t tests = 0;
    for (size_t i = 0; i < b->target_count; ++i) {
        C_Target *t = &b->targets[i]; if (t->kind != C_TARGET_TEST || (opt->target && strcmp(opt->target, t->name))) continue;
        char *out = build_target_graph(b, t, states, opt, mark, outputs); note("TEST", "%s", t->name); char *argv[] = {out, NULL}; int rc = run_argv(argv, opt->verbose, NULL); free(out); if (rc) die("test failed: %s", t->name); ++tests;
    }
    if (!tests) die("no test targets defined; use c_test() in build.c");
    note("PASS", "%zu test target%s", tests, tests == 1 ? "" : "s"); free_outputs(b, outputs); free_build(b); return 0;
}

static int command_fetch(const Options *opt) {
    C_Build *b = load_build(opt); DepState states[C_MAX_DEPS] = {0}; resolve_all(b, opt, states); note("DONE", "%zu dependencies ready", b->dep_count); free_build(b); return 0;
}

static int command_update(const Options *opt) {
    C_Build *b = load_build(opt); LockFile lock; load_lock(&lock); size_t w = 0; bool found = false;
    for (size_t i = 0; i < lock.count; ++i) { if (!opt->target || !strcmp(lock.entries[i].name, opt->target)) { found = true; continue; } lock.entries[w++] = lock.entries[i]; }
    if (opt->target && !found) die("dependency not found in c.lock: %s", opt->target); lock.count = w; if (lock.count) save_lock(&lock); else DeleteFileA("c.lock");
    DepState states[C_MAX_DEPS] = {0}; resolve_all(b, opt, states); note("UPDATE", "%s", opt->target ? opt->target : "all dependencies"); free_build(b); return 0;
}

static int command_deps(const Options *opt) {
    C_Build *b = load_build(opt);
    if (opt->target && !strcmp(opt->target, "clean")) { char c[PATH_MAX]; cache_root(c); char p[PATH_MAX]; const char *dirs[] = {"git","src","pkg"}; for (size_t i=0;i<C_ARRAY_LEN(dirs);++i){path_join(p,c,dirs[i]); remove_tree(p);} note("CLEAN","dependency cache"); free_build(b); return 0; }
    if (opt->target && strcmp(opt->target, "tree")) die("unknown deps action: %s", opt->target);
    if (opt->target) {
        puts("Targets:"); for (size_t i=0;i<b->target_count;++i){printf("  %s\n",b->targets[i].name); for(size_t j=0;j<b->targets[i].dep_count;++j) printf("    -> dependency %s\n",b->targets[i].deps[j]->name);}
    } else if (!b->dep_count) puts("No dependencies.");
    else for (size_t i=0;i<b->dep_count;++i) printf("%s  %s  %s\n",b->deps[i].name,b->deps[i].git,b->deps[i].ref);
    free_build(b); return 0;
}

static int command_cache(const Options *opt) {
    char root[PATH_MAX]; cache_root(root);
    if (opt->target && !strcmp(opt->target, "clean")) { remove_tree(root); note("CLEAN", "%s", root); return 0; }
    if (opt->target && strcmp(opt->target, "stats")) die("unknown cache action: %s", opt->target);
    if (!opt->target) puts(root); else { puts("Windows cache statistics are not yet collected; use `c cache` for the cache path."); }
    return 0;
}

static int command_init(void) {
    if (file_exists("build.c")) die("build.c already exists");
    write_file("build.c", "#include <cbuild.h>\n\nvoid build(C_Build *b) {\n    C_Target *app = c_executable(b, \"app\");\n    c_sources(app, \"src/*.c\");\n}\n", true);
    if (!file_exists("src/main.c")) write_file("src/main.c", "#include <stdio.h>\n\nint main(void) {\n    puts(\"Hello from C.\");\n    return 0;\n}\n", true);
    if (!file_exists(".gitignore")) write_file(".gitignore", "build/\ncompile_commands.json\n", true);
    note("INIT", "build.c + src/main.c"); return 0;
}

static int command_clean(void) { remove_tree("build"); DeleteFileA("compile_commands.json"); note("CLEAN", "build"); return 0; }

static int command_doctor(const Options *opt) {
    printf("c %s\n\n", C_VERSION); printf("Platform   Windows %s\n", sizeof(void*) == 8 ? "x86_64" : "x86");
    printf("Compiler   %s%s\n", opt->cc, command_exists(opt->cc) ? "" : "  [missing]");
    const char *ar = getenv("AR"); if (!ar || !*ar) ar = "llvm-ar"; printf("Archiver   %s%s\n", ar, command_exists(ar) ? "" : "  [missing]");
    printf("Git        %s\n", command_exists("git") ? "ok" : "missing"); printf("CPUs       %d\n", cpu_count()); printf("Jobs       %d\n", opt->jobs);
    char cache[PATH_MAX]; cache_root(cache); printf("Cache      %s\n", cache); return 0;
}

static void usage(void) {
    puts("c - a build system and dependency manager for C\n\n"
         "usage:\n"
         "  c init\n  c build [target] [--release] [-j N] [-v]\n  c run [target] [--release] [-j N] [-v] [-- args...]\n"
         "  c fetch\n  c update [dependency]\n  c deps [tree|clean]\n  c test [target]\n  c clean\n  c cache [clean]\n  c doctor\n  c --version\n\n"
         "environment:\n  CC              C compiler (default: clang)\n  AR              archiver (default: llvm-ar)\n  C_CACHE_DIR     override global cache directory\n  C_INCLUDE_DIR   directory containing cbuild.h\n");
}

static bool backend_file(const char *name) { return file_exists(name); }

static int wrapper_cmake(const Options *opt) {
    char bdir[PATH_MAX]; snprintf(bdir, sizeof(bdir), ".c-build/cmake/%s", opt->release ? "release" : "debug"); mkdir_p(bdir);
    char cache[PATH_MAX]; path_join(cache, bdir, "CMakeCache.txt");
    if (!file_exists(cache)) { char type[64]; snprintf(type,sizeof(type),"-DCMAKE_BUILD_TYPE=%s",opt->release?"Release":"Debug"); char *cfg[]={"cmake","-S",".","-B",bdir,type,"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",NULL}; if(run_argv(cfg,opt->verbose,NULL)) return 1; }
    if (!strcmp(opt->command,"clean")) { char *a[]={"cmake","--build",bdir,"--target","clean",NULL}; return run_argv(a,opt->verbose,NULL); }
    if (!strcmp(opt->command,"test")) { char *a[]={"ctest","--test-dir",bdir,"--output-on-failure","-C",opt->release?"Release":"Debug",NULL}; return run_argv(a,opt->verbose,NULL); }
    StrVec a={0}; vec_push(&a,"cmake"); vec_push(&a,"--build"); vec_push(&a,bdir); vec_push(&a,"--parallel"); char jobs[16]; snprintf(jobs,sizeof(jobs),"%d",opt->jobs); vec_push(&a,jobs); if(opt->target){vec_push(&a,"--target");vec_push(&a,opt->target);} int rc=run_vec(&a,opt->verbose,NULL); vec_free(&a); return rc;
}

static int wrapper_make(const Options *opt) {
    StrVec a={0}; vec_push(&a,"make"); char jobs[16]; snprintf(jobs,sizeof(jobs),"-j%d",opt->jobs); vec_push(&a,jobs);
    if (!strcmp(opt->command,"clean")) vec_push(&a,"clean"); else if(!strcmp(opt->command,"test")) vec_push(&a,"test"); else if(opt->target) vec_push(&a,opt->target);
    int rc=run_vec(&a,opt->verbose,NULL); vec_free(&a); return rc;
}

static Options parse_options(int argc, char **argv) {
    Options o = {0}; o.command = argc > 1 ? argv[1] : "help"; o.cc = getenv("CC"); if (!o.cc || !*o.cc) o.cc = "clang"; o.jobs = default_jobs();
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--")) { o.run_argc = argc - i - 1; o.run_argv = &argv[i + 1]; break; }
        if (!strcmp(argv[i], "--release") || !strcmp(argv[i], "-Drelease")) o.release = true;
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) o.verbose = true;
        else if (!strcmp(argv[i], "--cc") && i + 1 < argc) o.cc = argv[++i];
        else if ((!strcmp(argv[i], "-j") || !strcmp(argv[i], "--jobs")) && i + 1 < argc) o.jobs = atoi(argv[++i]);
        else if (!strncmp(argv[i], "-j", 2) && argv[i][2]) o.jobs = atoi(argv[i] + 2);
        else if (!strncmp(argv[i], "--jobs=", 7)) o.jobs = atoi(argv[i] + 7);
        else if (argv[i][0] != '-' && !o.target) o.target = argv[i];
        else if (!strncmp(argv[i], "--unity", 7) || !strcmp(argv[i], "--no-unity") || !strcmp(argv[i], "--profile") || !strcmp(argv[i], "--explain") || !strcmp(argv[i], "--fast-debug") || !strcmp(argv[i], "--adaptive-jobs") || !strcmp(argv[i], "--no-adaptive-jobs") || !strcmp(argv[i], "--object-cache") || !strcmp(argv[i], "--no-object-cache") || !strncmp(argv[i], "--linker", 8)) { /* accepted for source compatibility; Windows backend currently ignores tuning-only switches */ }
        else die("unknown option: %s", argv[i]);
    }
    if (o.jobs < 1) o.jobs = 1; if (o.jobs > 64) o.jobs = 64; return o;
}

int main(int argc, char **argv) {
    Options opt = parse_options(argc, argv);
    if (!strcmp(opt.command,"--version") || !strcmp(opt.command,"version")) { puts(C_VERSION); return 0; }
    if (!strcmp(opt.command,"help") || !strcmp(opt.command,"--help") || !strcmp(opt.command,"-h")) { usage(); return 0; }

    if (!backend_file("build.c") && backend_file("CMakeLists.txt") && (!strcmp(opt.command,"build") || !strcmp(opt.command,"clean") || !strcmp(opt.command,"test"))) return wrapper_cmake(&opt);
    if (!backend_file("build.c") && (backend_file("Makefile") || backend_file("GNUmakefile") || backend_file("makefile")) && (!strcmp(opt.command,"build") || !strcmp(opt.command,"clean") || !strcmp(opt.command,"test"))) return wrapper_make(&opt);

    if (!strcmp(opt.command,"init")) return command_init();
    if (!strcmp(opt.command,"build")) return command_build(&opt,false);
    if (!strcmp(opt.command,"run")) return command_build(&opt,true);
    if (!strcmp(opt.command,"test")) return command_test(&opt);
    if (!strcmp(opt.command,"fetch")) return command_fetch(&opt);
    if (!strcmp(opt.command,"update")) return command_update(&opt);
    if (!strcmp(opt.command,"deps")) return command_deps(&opt);
    if (!strcmp(opt.command,"clean")) return command_clean();
    if (!strcmp(opt.command,"cache")) return command_cache(&opt);
    if (!strcmp(opt.command,"doctor")) return command_doctor(&opt);
    die("unknown command: %s (try `c help`)", opt.command);
    return 1;
}
