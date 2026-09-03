#!/bin/sh
set -eu

C_BIN="$1"
INC="$2"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM HUP

export C_INCLUDE_DIR="$INC"
export C_CACHE_DIR="$TMP/cache"

mkdir -p "$TMP/dep/include" "$TMP/app/src"
cat >"$TMP/dep/include/value.h" <<'SRC'
#define OFFLINE_VALUE 42
SRC
(
    cd "$TMP/dep"
    git init -q -b main
    git config user.name test
    git config user.email test@example.invalid
    git add include/value.h
    git commit -qm initial
)
FIRST="$(git -C "$TMP/dep" rev-parse HEAD)"

cat >"$TMP/app/build.c" <<EOF
#include <cbuild.h>
void build(C_Build *b) {
    C_Target *app = c_executable(b, "app");
    c_sources(app, "src/main.c");
    C_Dependency *dep = c_git(b, "offline", "$TMP/dep", "main");
    c_dep_header_only(dep);
    c_dep_include(dep, "include");
    c_use(app, dep);
}
EOF
cat >"$TMP/app/src/main.c" <<'SRC'
#include <stdio.h>
#include <value.h>
int main(void) { printf("%d\n", OFFLINE_VALUE); return OFFLINE_VALUE == 42 ? 0 : 1; }
SRC

(
    cd "$TMP/app"
    "$C_BIN" build >/dev/null
    test "$(./build/debug/app)" = 42
    test -f c.lock
    grep -q "$FIRST" c.lock
)

src_cache="$(find "$C_CACHE_DIR/src" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
mirror_cache="$(find "$C_CACHE_DIR/git" -mindepth 1 -maxdepth 1 -type d -name '*.git' | head -n 1)"
test -n "$src_cache"
test -n "$mirror_cache"
test -f "$src_cache.c-ready"
test -f "$mirror_cache.c-ready"

# Advance the requested branch after the lock was written. A normal rebuild must
# remain pinned to the resolved commit in c.lock, even if the cached checkout is
# removed and has to be reconstructed from the mirror.
cat >"$TMP/dep/include/value.h" <<'SRC'
#define OFFLINE_VALUE 99
SRC
(
    cd "$TMP/dep"
    git add include/value.h
    git commit -qm second
)
SECOND="$(git -C "$TMP/dep" rev-parse HEAD)"
test "$FIRST" != "$SECOND"

rm -rf "$TMP/app/build" "$src_cache" "$src_cache.c-ready"
(
    cd "$TMP/app"
    "$C_BIN" build >/dev/null
    test "$(./build/debug/app)" = 42
    grep -q "$FIRST" c.lock
    if grep -q "$SECOND" c.lock; then
        echo "offline-dependency: ordinary rebuild drifted away from lock" >&2
        exit 1
    fi
)
test -f "$src_cache.c-ready"

# Now remove the origin entirely and force another source checkout recreation.
# The lockfile plus the cached mirror must be sufficient for an unchanged build.
rm -rf "$TMP/dep" "$TMP/app/build" "$src_cache" "$src_cache.c-ready"
(
    cd "$TMP/app"
    "$C_BIN" build >/dev/null
    test "$(./build/debug/app)" = 42
    grep -q "$FIRST" c.lock
)

# Fail closed when neither the origin nor the locked dependency cache exists.
# This guards against silently drifting to another revision or pretending an
# offline rebuild succeeded without the bytes required by c.lock.
rm -rf "$TMP/app/build" "$src_cache" "$src_cache.c-ready" "$mirror_cache" "$mirror_cache.c-ready"
if (
    cd "$TMP/app"
    "$C_BIN" build >/dev/null 2>&1
); then
    echo "offline-dependency: build unexpectedly succeeded without origin or locked cache" >&2
    exit 1
fi

echo "offline-dependency: ok"
