#!/bin/sh
set -eu

INC="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM HUP

cat >"$TMP/api_1x_surface.c" <<'SRC'
#include <cbuild.h>

#if C_BUILD_API_VERSION_MAJOR != 1
#error "C-BuildSystem 1.x API major changed"
#endif
#if C_BUILD_API_VERSION_MINOR != 0
#error "C-BuildSystem 1.0 API minor baseline changed"
#endif
#if C_BUILD_API_VERSION_PATCH != 0
#error "C-BuildSystem 1.0 API patch baseline changed"
#endif

static void verify_surface(void) {
    C_Target *(*p_executable)(C_Build *, const char *) = c_executable;
    C_Target *(*p_static_library)(C_Build *, const char *) = c_static_library;
    C_Target *(*p_shared_library)(C_Build *, const char *) = c_shared_library;
    C_Target *(*p_test)(C_Build *, const char *) = c_test;
    void (*p_default_target)(C_Build *, C_Target *) = c_default_target;
    void (*p_sources)(C_Target *, const char *) = c_sources;
    void (*p_include)(C_Target *, const char *) = c_include;
    void (*p_define)(C_Target *, const char *) = c_define;
    void (*p_flag)(C_Target *, const char *) = c_flag;
    void (*p_link_flag)(C_Target *, const char *) = c_link_flag;
    void (*p_link_system)(C_Target *, const char *) = c_link_system;
    void (*p_framework)(C_Target *, const char *) = c_framework;
    void (*p_unity)(C_Target *, int) = c_unity;
    void (*p_unity_auto)(C_Target *) = c_unity_auto;
    void (*p_no_unity)(C_Target *) = c_no_unity;
    void (*p_standard)(C_Target *, C_Standard) = c_standard;
    void (*p_warnings_strict)(C_Target *) = c_warnings_strict;
    void (*p_generate)(C_Target *, const char *, const char *, const char *) = c_generate;
    void (*p_link_target)(C_Target *, C_Target *) = c_link_target;
    C_Dependency *(*p_git)(C_Build *, const char *, const char *, const char *) = c_git;
    void (*p_dep_header_only)(C_Dependency *) = c_dep_header_only;
    void (*p_dep_source)(C_Dependency *) = c_dep_source;
    void (*p_dep_cbuild)(C_Dependency *, const char *, C_TargetKind) = c_dep_cbuild;
    void (*p_dep_include)(C_Dependency *, const char *) = c_dep_include;
    void (*p_dep_sources)(C_Dependency *, const char *) = c_dep_sources;
    void (*p_dep_subdir)(C_Dependency *, const char *) = c_dep_subdir;
    void (*p_dep_flag)(C_Dependency *, const char *) = c_dep_flag;
    void (*p_use)(C_Target *, C_Dependency *) = c_use;

    C_TargetKind target_kinds[] = {
        C_TARGET_EXECUTABLE, C_TARGET_STATIC_LIBRARY, C_TARGET_TEST, C_TARGET_SHARED_LIBRARY
    };
    C_DepKind dep_kinds[] = { C_DEP_HEADER_ONLY, C_DEP_RESERVED, C_DEP_SOURCE, C_DEP_CBUILD };
    C_Standard standards[] = {
        C_STANDARD_C99, C_STANDARD_C11, C_STANDARD_C17, C_STANDARD_C23
    };

    (void)p_executable; (void)p_static_library; (void)p_shared_library; (void)p_test;
    (void)p_default_target; (void)p_sources; (void)p_include; (void)p_define;
    (void)p_flag; (void)p_link_flag; (void)p_link_system; (void)p_framework;
    (void)p_unity; (void)p_unity_auto; (void)p_no_unity; (void)p_standard;
    (void)p_warnings_strict; (void)p_generate; (void)p_link_target; (void)p_git;
    (void)p_dep_header_only; (void)p_dep_source; (void)p_dep_cbuild; (void)p_dep_include;
    (void)p_dep_sources; (void)p_dep_subdir; (void)p_dep_flag; (void)p_use;
    (void)target_kinds; (void)dep_kinds; (void)standards;
}

int main(void) {
    verify_surface();
    return 0;
}
SRC

${CC:-cc} -std=c11 -Wall -Wextra -Werror -I"$INC" -fsyntax-only "$TMP/api_1x_surface.c"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -I"$INC" -x c++ -fsyntax-only "$TMP/api_1x_surface.c"

echo "api-1x-surface: ok"
