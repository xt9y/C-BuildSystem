#ifndef CBUILD_H
#define CBUILD_H

/*
 * cbuild.h - public build.c API for the `c` build system.
 *
 * build.c is compiled as C and loaded by the c executable. Keep this header
 * dependency-free. Beginning with 1.0, the documented names, signatures,
 * enum constants and build-description semantics form the source API for the
 * 1.x line. Compatible additions are allowed in 1.x; removals or incompatible
 * signature/semantic changes require a new major version.
 *
 * The public structs remain implementation-visible so build.c stays simple,
 * but their layout is not a binary ABI and direct field access is not part of
 * the supported source-compatibility contract.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define C_BUILD_API_VERSION_MAJOR 1
#define C_BUILD_API_VERSION_MINOR 0
#define C_BUILD_API_VERSION_PATCH 0

#define C_MAX_TARGETS 256
#define C_MAX_DEPS 256
#define C_MAX_ITEMS 16384
#define C_MAX_PATH 1024
#define C_MAX_NAME 128

typedef enum C_TargetKind {
    C_TARGET_EXECUTABLE = 0,
    C_TARGET_STATIC_LIBRARY = 1,
    C_TARGET_TEST = 2,
    C_TARGET_SHARED_LIBRARY = 3
} C_TargetKind;

typedef enum C_DepKind {
    C_DEP_HEADER_ONLY = 0,
    C_DEP_RESERVED = 1,
    C_DEP_SOURCE = 2,
    C_DEP_CBUILD = 3
} C_DepKind;

typedef enum C_Standard {
    C_STANDARD_C99 = 99,
    C_STANDARD_C11 = 11,
    C_STANDARD_C17 = 17,
    C_STANDARD_C23 = 23
} C_Standard;

typedef struct C_StringList {
    char **items;
    size_t count;
    size_t capacity;
} C_StringList;

typedef struct C_Dependency {
    char name[C_MAX_NAME];
    char git[C_MAX_PATH];
    char ref[C_MAX_NAME];
    char subdir[C_MAX_PATH];
    C_DepKind kind;
    C_StringList links;          /* reserved for source compatibility */
    C_StringList include_dirs;
    C_StringList source_patterns;
    C_StringList compile_flags;
    char build_target[C_MAX_NAME];
    C_TargetKind build_target_kind;
} C_Dependency;

typedef struct C_Target {
    char name[C_MAX_NAME];
    C_TargetKind kind;
    C_StringList sources;
    C_StringList includes;
    C_StringList defines;
    C_StringList cflags;
    C_StringList ldflags;
    C_StringList system_links;
    C_StringList frameworks;
    C_StringList generated_outputs;
    C_StringList generated_inputs;
    C_StringList generated_commands;
    C_Dependency *deps[C_MAX_DEPS];
    size_t dep_count;
    struct C_Target *target_deps[C_MAX_TARGETS];
    size_t target_dep_count;
    int unity_chunk;  /* 0=inherit CLI, 1=off, -1=auto, >=2=fixed chunk */
} C_Target;

typedef struct C_Build {
    C_Target targets[C_MAX_TARGETS];
    size_t target_count;
    C_Dependency deps[C_MAX_DEPS];
    size_t dep_count;
    int default_target;
} C_Build;

static inline void c__fatal(const char *message) {
    fprintf(stderr, "c: error: invalid build.c: %s\n", message ? message : "invalid build description");
    fflush(stderr);
    exit(2);
}

static inline void c__require_name(const char *name, const char *kind) {
    if (!name || !name[0]) {
        fprintf(stderr, "c: error: invalid build.c: %s name is empty\n", kind ? kind : "object");
        fflush(stderr);
        exit(2);
    }
    if (strlen(name) >= C_MAX_NAME) {
        fprintf(stderr, "c: error: invalid build.c: %s name exceeds C_MAX_NAME (%d)\n", kind ? kind : "object", C_MAX_NAME);
        fflush(stderr);
        exit(2);
    }
}

static inline void c__copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) c__fatal("attempted to copy into an invalid destination");
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) c__fatal("build description value exceeds a fixed API field limit");
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static inline void c__push(C_StringList *list, const char *value) {
    if (!list) c__fatal("attempted to append to a null list");
    if (!value) c__fatal("attempted to append a null string");
    if (list->count >= C_MAX_ITEMS) c__fatal("too many values in a build description list");
    if (list->count == list->capacity) {
        size_t next = list->capacity ? list->capacity * 2 : 8;
        if (next > C_MAX_ITEMS) next = C_MAX_ITEMS;
        char **items = (char **)realloc(list->items, next * sizeof(*items));
        if (!items) c__fatal("out of memory while extending build description");
        list->items = items;
        list->capacity = next;
    }
    size_t n = strlen(value);
    char *copy = (char *)malloc(n + 1);
    if (!copy) c__fatal("out of memory while copying build description value");
    memcpy(copy, value, n + 1);
    list->items[list->count++] = copy;
}

static inline void c__check_target_name(const C_Build *b, const char *name) {
    c__require_name(name, "target");
    if (!b) c__fatal("null build passed while creating target");
    for (size_t i = 0; i < b->target_count; ++i) {
        if (!strcmp(b->targets[i].name, name)) {
            fprintf(stderr, "c: error: invalid build.c: duplicate target '%s'\n", name);
            fflush(stderr);
            exit(2);
        }
    }
}

static inline void c__check_dep_name(const C_Build *b, const char *name) {
    c__require_name(name, "dependency");
    if (!b) c__fatal("null build passed while creating dependency");
    for (size_t i = 0; i < b->dep_count; ++i) {
        if (!strcmp(b->deps[i].name, name)) {
            fprintf(stderr, "c: error: invalid build.c: duplicate dependency '%s'\n", name);
            fflush(stderr);
            exit(2);
        }
    }
}

static inline C_Target *c__new_target(C_Build *b, const char *name, C_TargetKind kind) {
    c__check_target_name(b, name);
    if (b->target_count >= C_MAX_TARGETS) c__fatal("too many targets; increase C_MAX_TARGETS or split the build");
    C_Target *t = &b->targets[b->target_count++];
    memset(t, 0, sizeof(*t));
    t->kind = kind;
    c__copy(t->name, sizeof(t->name), name);
    if (kind == C_TARGET_EXECUTABLE && b->default_target < 0) b->default_target = (int)(b->target_count - 1);
    return t;
}

static inline C_Target *c_executable(C_Build *b, const char *name) { return c__new_target(b, name, C_TARGET_EXECUTABLE); }
static inline C_Target *c_static_library(C_Build *b, const char *name) { return c__new_target(b, name, C_TARGET_STATIC_LIBRARY); }
static inline C_Target *c_shared_library(C_Build *b, const char *name) { return c__new_target(b, name, C_TARGET_SHARED_LIBRARY); }
static inline C_Target *c_test(C_Build *b, const char *name) { return c__new_target(b, name, C_TARGET_TEST); }

static inline void c_default_target(C_Build *b, C_Target *target) {
    if (!b || !target) c__fatal("c_default_target requires a build and target");
    ptrdiff_t idx = target - b->targets;
    if (idx < 0 || (size_t)idx >= b->target_count) c__fatal("default target does not belong to this build");
    b->default_target = (int)idx;
}

static inline void c_sources(C_Target *t, const char *pattern) { if (!t) c__fatal("c_sources received a null target"); c__push(&t->sources, pattern); }
static inline void c_include(C_Target *t, const char *path) { if (!t) c__fatal("c_include received a null target"); c__push(&t->includes, path); }
static inline void c_define(C_Target *t, const char *value) { if (!t) c__fatal("c_define received a null target"); c__push(&t->defines, value); }
static inline void c_flag(C_Target *t, const char *value) { if (!t) c__fatal("c_flag received a null target"); c__push(&t->cflags, value); }
static inline void c_link_flag(C_Target *t, const char *value) { if (!t) c__fatal("c_link_flag received a null target"); c__push(&t->ldflags, value); }
static inline void c_link_system(C_Target *t, const char *name) { if (!t) c__fatal("c_link_system received a null target"); c__push(&t->system_links, name); }
static inline void c_framework(C_Target *t, const char *name) { if (!t) c__fatal("c_framework received a null target"); c__push(&t->frameworks, name); }
static inline void c_unity(C_Target *t, int chunk_size) { if (!t) c__fatal("c_unity received a null target"); t->unity_chunk = chunk_size > 1 ? chunk_size : 1; }
static inline void c_unity_auto(C_Target *t) { if (!t) c__fatal("c_unity_auto received a null target"); t->unity_chunk = -1; }
static inline void c_no_unity(C_Target *t) { if (!t) c__fatal("c_no_unity received a null target"); t->unity_chunk = 1; }

static inline void c_standard(C_Target *t, C_Standard standard) {
    if (!t) c__fatal("c_standard received a null target");
    switch (standard) {
        case C_STANDARD_C99: c__push(&t->cflags, "-std=c99"); break;
        case C_STANDARD_C11: c__push(&t->cflags, "-std=c11"); break;
        case C_STANDARD_C17: c__push(&t->cflags, "-std=c17"); break;
        case C_STANDARD_C23: c__push(&t->cflags, "-std=c23"); break;
        default: c__fatal("unsupported C language standard");
    }
}

static inline void c_warnings_strict(C_Target *t) {
    if (!t) c__fatal("c_warnings_strict received a null target");
    c__push(&t->cflags, "-Wall");
    c__push(&t->cflags, "-Wextra");
    c__push(&t->cflags, "-Wpedantic");
}

/*
 * Declare a generated source/output. `command` is executed by /bin/sh when
 * output is missing, input is newer, or the command itself changed. Passing
 * NULL/empty input creates a command-only generated output. The output is
 * automatically added to the target's source list.
 */
static inline void c_generate(C_Target *t, const char *output, const char *input, const char *command) {
    if (!t) c__fatal("c_generate received a null target");
    if (!output || !output[0]) c__fatal("c_generate output is empty");
    if (!command || !command[0]) c__fatal("c_generate command is empty");
    c__push(&t->generated_outputs, output);
    c__push(&t->generated_inputs, input ? input : "");
    c__push(&t->generated_commands, command);
    c__push(&t->sources, output);
}

static inline void c_link_target(C_Target *t, C_Target *dependency) {
    if (!t || !dependency) c__fatal("c_link_target requires two targets");
    if (t == dependency) c__fatal("target cannot link to itself");
    for (size_t i = 0; i < t->target_dep_count; ++i) {
        if (t->target_deps[i] == dependency) {
            fprintf(stderr, "c: error: invalid build.c: target '%s' links target '%s' more than once\n", t->name, dependency->name);
            fflush(stderr);
            exit(2);
        }
    }
    if (t->target_dep_count >= C_MAX_TARGETS) c__fatal("too many target-to-target dependencies");
    t->target_deps[t->target_dep_count++] = dependency;
}

static inline C_Dependency *c_git(C_Build *b, const char *name, const char *git, const char *ref) {
    c__check_dep_name(b, name);
    if (!git || !git[0]) c__fatal("git dependency URL is empty");
    if (b->dep_count >= C_MAX_DEPS) c__fatal("too many dependencies; increase C_MAX_DEPS or split the build");
    C_Dependency *d = &b->deps[b->dep_count++];
    memset(d, 0, sizeof(*d));
    c__copy(d->name, sizeof(d->name), name);
    c__copy(d->git, sizeof(d->git), git);
    c__copy(d->ref, sizeof(d->ref), (ref && ref[0]) ? ref : "HEAD");
    d->kind = C_DEP_HEADER_ONLY;
    return d;
}

static inline void c_dep_header_only(C_Dependency *d) { if (!d) c__fatal("c_dep_header_only received a null dependency"); d->kind = C_DEP_HEADER_ONLY; }
static inline void c_dep_source(C_Dependency *d) { if (!d) c__fatal("c_dep_source received a null dependency"); d->kind = C_DEP_SOURCE; }
static inline void c_dep_cbuild(C_Dependency *d, const char *target, C_TargetKind kind) {
    if (!d) c__fatal("c_dep_cbuild received a null dependency");
    c__require_name(target, "dependency target");
    if (kind != C_TARGET_STATIC_LIBRARY && kind != C_TARGET_SHARED_LIBRARY)
        c__fatal("c_dep_cbuild currently supports static or shared library targets");
    d->kind = C_DEP_CBUILD;
    c__copy(d->build_target, sizeof(d->build_target), target);
    d->build_target_kind = kind;
}
static inline void c_dep_include(C_Dependency *d, const char *path) { if (!d) c__fatal("c_dep_include received a null dependency"); c__push(&d->include_dirs, path); }
static inline void c_dep_sources(C_Dependency *d, const char *pattern) { if (!d) c__fatal("c_dep_sources received a null dependency"); c__push(&d->source_patterns, pattern); }
static inline void c_dep_subdir(C_Dependency *d, const char *path) { if (!d) c__fatal("c_dep_subdir received a null dependency"); c__copy(d->subdir, sizeof(d->subdir), path); }
static inline void c_dep_flag(C_Dependency *d, const char *flag) { if (!d) c__fatal("c_dep_flag received a null dependency"); c__push(&d->compile_flags, flag); }

static inline void c_use(C_Target *t, C_Dependency *d) {
    if (!t || !d) c__fatal("c_use requires a target and dependency");
    for (size_t i = 0; i < t->dep_count; ++i) {
        if (t->deps[i] == d) {
            fprintf(stderr, "c: error: invalid build.c: dependency '%s' added to target '%s' more than once\n", d->name, t->name);
            fflush(stderr);
            exit(2);
        }
    }
    if (t->dep_count >= C_MAX_DEPS) c__fatal("too many dependencies attached to one target");
    t->deps[t->dep_count++] = d;
}

#ifdef __cplusplus
}
#endif

#endif
