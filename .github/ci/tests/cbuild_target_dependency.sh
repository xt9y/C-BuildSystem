#!/bin/sh
set -eu

C_BIN="$1"
INC="$2"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM HUP

LIB="$TMP/libproject"
APP="$TMP/app"
mkdir -p "$LIB/include" "$LIB/src" "$APP/src"

cat >"$LIB/include/value.h" <<'SRC'
#ifndef VALUE_H
#define VALUE_H
int dependency_value(void);
#endif
SRC

cat >"$LIB/src/value.c" <<'SRC'
#include "value.h"
int dependency_value(void) { return 42; }
SRC

cat >"$LIB/build.c" <<'SRC'
#include <cbuild.h>
void build(C_Build *b) {
    C_Target *lib = c_shared_library(b, "dependency");
    c_sources(lib, "src/value.c");
    c_include(lib, "include");
    c_default_target(b, lib);
}
SRC

(
    cd "$LIB"
    git init -q
    git config user.name test
    git config user.email test@example.invalid
    git add .
    git commit -qm initial
    git branch -M main
)

cat >"$APP/src/main.c" <<'SRC'
#include <stdio.h>
#include "value.h"
int main(void) {
    printf("%d\n", dependency_value());
    return 0;
}
SRC

cat >"$APP/build.c" <<SRC
#include <cbuild.h>
void build(C_Build *b) {
    C_Target *app = c_executable(b, "app");
    c_sources(app, "src/main.c");

    C_Dependency *dep = c_git(b, "dependency", "$LIB", "main");
    c_dep_cbuild(dep, "dependency", C_TARGET_SHARED_LIBRARY);
    c_dep_include(dep, "include");
    c_use(app, dep);

    c_default_target(b, app);
}
SRC

(
    cd "$APP"
    C_INCLUDE_DIR="$INC" "$C_BIN" build
    test "$(./build/debug/app)" = 42

    if [ "$(uname -s)" = Darwin ]; then
        otool -L ./build/debug/app | grep -q 'libdependency.dylib'
    else
        ldd ./build/debug/app | grep -q 'libdependency.so'
    fi
)

echo "cbuild-target-dependency: ok"
