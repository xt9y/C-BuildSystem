#!/bin/sh
set -eu

C_BIN="$1"
INC="$2"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM HUP

PROJECT="$TMP/project"
mkdir -p "$PROJECT/src"
cat >"$PROJECT/src/value.h" <<'SRC'
#define VALUE 1
SRC
cat >"$PROJECT/src/main.c" <<'SRC'
#include <stdio.h>
#include "value.h"
int main(void) { printf("%d\n", VALUE); return 0; }
SRC
cat >"$PROJECT/build.c" <<'SRC'
#include <cbuild.h>
void build(C_Build *b) {
    C_Target *app = c_executable(b, "app");
    c_sources(app, "src/*.c");
    c_include(app, "src");
}
SRC
(
    cd "$PROJECT"
    C_INCLUDE_DIR="$INC" "$C_BIN" build >/dev/null
    test "$(./build/debug/app)" = 1

    # Header changes must invalidate the dependent translation unit.
    printf '#define VALUE 2\n' > src/value.h
    C_INCLUDE_DIR="$INC" "$C_BIN" build >/dev/null
    test "$(./build/debug/app)" = 2

    # Adding a source through a glob must be noticed immediately.
    cat > src/new_file.c <<'SRC'
#error newly-added-source-was-compiled
SRC
    if C_INCLUDE_DIR="$INC" "$C_BIN" build >/dev/null 2>add.err; then
        echo "incremental-correctness: added source was ignored" >&2
        exit 1
    fi
    grep -q 'newly-added-source-was-compiled' add.err
    rm src/new_file.c
    C_INCLUDE_DIR="$INC" "$C_BIN" build >/dev/null

    # A removed source must not leave a stale object in the link set.
    cat > src/old.c <<'SRC'
int removed_symbol = 1;
SRC
    C_INCLUDE_DIR="$INC" "$C_BIN" build >/dev/null
    rm src/old.c
    cat > src/current.c <<'SRC'
int removed_symbol = 2;
SRC
    C_INCLUDE_DIR="$INC" "$C_BIN" build >/dev/null
    rm src/current.c

    # build.c/compile-flag changes must invalidate cached objects.
    cat > build.c <<'SRC'
#include <cbuild.h>
void build(C_Build *b) {
    C_Target *app = c_executable(b, "app");
    c_sources(app, "src/*.c");
    c_include(app, "src");
    c_define(app, "BUILD_FLAG=7");
}
SRC
    cat > src/main.c <<'SRC'
#include <stdio.h>
#ifndef BUILD_FLAG
#define BUILD_FLAG 0
#endif
int main(void) { printf("%d\n", BUILD_FLAG); return 0; }
SRC
    C_INCLUDE_DIR="$INC" "$C_BIN" build >/dev/null
    test "$(./build/debug/app)" = 7
    sed 's/BUILD_FLAG=7/BUILD_FLAG=9/' build.c > build.c.next
    mv build.c.next build.c
    C_INCLUDE_DIR="$INC" "$C_BIN" build >/dev/null
    test "$(./build/debug/app)" = 9
)

# Paths containing spaces must be ordinary supported project paths.
SPACE="$TMP/project with spaces"
mkdir -p "$SPACE/src"
cat >"$SPACE/src/main.c" <<'SRC'
#include <stdio.h>
int main(void) { puts("space-ok"); return 0; }
SRC
cat >"$SPACE/build.c" <<'SRC'
#include <cbuild.h>
void build(C_Build *b) {
    C_Target *app = c_executable(b, "app");
    c_sources(app, "src/*.c");
}
SRC
(
    cd "$SPACE"
    C_INCLUDE_DIR="$INC" "$C_BIN" build >/dev/null
    test "$(./build/debug/app)" = space-ok
)

# Multiple build processes sharing one project/cache must either all succeed or
# fail loudly; no process may observe a partial cache entry as valid.
CONCURRENT="$TMP/concurrent"
mkdir -p "$CONCURRENT/src"
cat >"$CONCURRENT/src/main.c" <<'SRC'
int main(void) { return 0; }
SRC
cat >"$CONCURRENT/build.c" <<'SRC'
#include <cbuild.h>
void build(C_Build *b) {
    C_Target *app = c_executable(b, "app");
    c_sources(app, "src/*.c");
}
SRC
(
    cd "$CONCURRENT"
    export C_INCLUDE_DIR="$INC"
    export C_CACHE_DIR="$TMP/shared-cache"
    "$C_BIN" build >one.log 2>&1 & p1=$!
    "$C_BIN" build >two.log 2>&1 & p2=$!
    "$C_BIN" build >three.log 2>&1 & p3=$!
    "$C_BIN" build >four.log 2>&1 & p4=$!

    failed=0
    wait "$p1" || failed=1
    wait "$p2" || failed=1
    wait "$p3" || failed=1
    wait "$p4" || failed=1
    if [ "$failed" -ne 0 ]; then
        echo "incremental-correctness: concurrent build failed" >&2
        for log in one.log two.log three.log four.log; do
            echo "--- $log ---" >&2
            cat "$log" >&2
        done
        exit 1
    fi
    test -x build/debug/app
    ./build/debug/app
)

echo "incremental-correctness: ok"
