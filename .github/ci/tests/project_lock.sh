#!/bin/sh
set -eu
C_BIN="$1"
INC="$2"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM HUP
PROJECT="$TMP/project"
TRACK="$TMP/track"
mkdir -p "$PROJECT/src" "$TRACK"
cat >"$PROJECT/src/main.c" <<'SRC'
int main(void) { return 0; }
SRC
cat >"$PROJECT/build.c" <<'SRC'
#include <cbuild.h>
void build(C_Build *b) {
    C_Target *app = c_executable(b, "app");
    c_sources(app, "src/main.c");
}
SRC
cat >"$PROJECT/cc-wrap.sh" <<'CC'
#!/bin/sh
set -eu
target=0
for arg in "$@"; do
    case "$arg" in src/main.c|*/src/main.c) target=1 ;; esac
done
if [ "$target" -eq 0 ]; then exec "$REAL_CC" "$@"; fi
active="$C_LOCK_TEST_DIR/active"
if ! mkdir "$active" 2>/dev/null; then
    : > "$C_LOCK_TEST_DIR/overlap"
    while ! mkdir "$active" 2>/dev/null; do sleep 0.01; done
fi
cleanup() { rmdir "$active" 2>/dev/null || true; }
trap cleanup EXIT INT TERM HUP
sleep 1
set +e
"$REAL_CC" "$@"
rc=$?
set -e
cleanup
trap - EXIT INT TERM HUP
exit "$rc"
CC
chmod +x "$PROJECT/cc-wrap.sh"
(
    cd "$PROJECT"
    export C_INCLUDE_DIR="$INC" C_CACHE_DIR="$TMP/cache" C_LOCK_TEST_DIR="$TRACK"
    export REAL_CC="${CC:-cc}" CC="$PROJECT/cc-wrap.sh"
    "$C_BIN" deps >/dev/null
    rm -f "$TRACK/overlap"
    "$C_BIN" build >one.log 2>&1 & p1=$!
    "$C_BIN" build >two.log 2>&1 & p2=$!
    "$C_BIN" build >three.log 2>&1 & p3=$!
    "$C_BIN" build >four.log 2>&1 & p4=$!
    failed=0
    wait "$p1" || failed=1; wait "$p2" || failed=1
    wait "$p3" || failed=1; wait "$p4" || failed=1
    if [ "$failed" -ne 0 ]; then
        echo "project-lock: concurrent build failed" >&2
        cat one.log two.log three.log four.log >&2
        exit 1
    fi
    if [ -e "$TRACK/overlap" ]; then
        echo "project-lock: same-project compiler overlap" >&2
        exit 1
    fi
    test -x build/debug/app
    ./build/debug/app
)
echo "project-lock: ok"
