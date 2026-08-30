#!/bin/sh
set -eu

C_BIN="$1"
INC="$2"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM HUP

export C_INCLUDE_DIR="$INC"
export C_CACHE_DIR="$TMP/cache"

# Root cache paths must never be followed when cleaning. A symlinked
# cache root used to let remove_tree() recurse into the target.
mkdir -p "$TMP/victim"
printf 'keep\n' >"$TMP/victim/keep.txt"
ln -s "$TMP/victim" "$TMP/cache-link"
C_CACHE_DIR="$TMP/cache-link" "$C_BIN" cache clean >/dev/null
test -f "$TMP/victim/keep.txt"
test ! -e "$TMP/cache-link"

mkdir -p "$TMP/project/src" "$TMP/bin"
cat >"$TMP/project/src/main.c" <<'SRC'
#include <stdio.h>
int main(void) { puts("v1"); return 0; }
SRC
cat >"$TMP/project/build.c" <<'SRC'
#include <cbuild.h>
void build(C_Build *b) {
    C_Target *app = c_executable(b, "app");
    c_sources(app, "src/main.c");
}
SRC

REAL_CC="$(command -v cc)"
cat >"$TMP/bin/cc-wrapper" <<SH
#!/bin/sh
set -eu
real='$REAL_CC'
out=''
dep=''
compile=0
prev=''
for arg in "\$@"; do
    if [ "\$prev" = -o ]; then out="\$arg"; fi
    if [ "\$prev" = -MF ]; then dep="\$arg"; fi
    [ "\$arg" = -c ] && compile=1
    prev="\$arg"
done
if [ "\${C_FAIL_COMPILE:-0}" = 1 ] && [ "\$compile" = 1 ]; then
    [ -n "\$out" ] && printf 'partial-object\n' >"\$out"
    [ -n "\$dep" ] && printf '%s: src/main.c\n' "\$out" >"\$dep"
    exit 1
fi
if [ "\${C_FAIL_LINK:-0}" = 1 ] && [ "\$compile" = 0 ] && [ -n "\$out" ]; then
    printf 'partial-link\n' >"\$out"
    exit 1
fi
exec "\$real" "\$@"
SH
chmod +x "$TMP/bin/cc-wrapper"

(
    cd "$TMP/project"
    export CC="$TMP/bin/cc-wrapper"
    "$C_BIN" build >/dev/null
    test "$(./build/debug/app)" = v1

    sleep 1
    cat >src/main.c <<'SRC'
#include <stdio.h>
int main(void) { puts("v2"); return 0; }
SRC

    export C_FAIL_COMPILE=1
    if "$C_BIN" build >/dev/null 2>&1; then
        echo "cache-artifact-recovery: poisoned compile unexpectedly succeeded" >&2
        exit 1
    fi
    export C_FAIL_COMPILE=0
    "$C_BIN" build >/dev/null
    test "$(./build/debug/app)" = v2

    sleep 1
    cat >src/main.c <<'SRC'
#include <stdio.h>
int main(void) { puts("v3"); return 0; }
SRC
    export C_FAIL_LINK=1
    if "$C_BIN" build >/dev/null 2>&1; then
        echo "cache-artifact-recovery: poisoned link unexpectedly succeeded" >&2
        exit 1
    fi
    # The previous published executable must remain intact.
    test "$(./build/debug/app)" = v2
    export C_FAIL_LINK=0
    "$C_BIN" build >/dev/null
    test "$(./build/debug/app)" = v3
)

echo "cache-artifact-recovery: ok"
