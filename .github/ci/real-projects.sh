#!/bin/sh
set -eu

C_BIN="$1"
INC="$2"
ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT INT TERM HUP

export C_INCLUDE_DIR="$INC"
export C_CACHE_DIR="$ROOT/cache"

checkout_at() {
    url="$1"
    revision="$2"
    destination="$3"
    mkdir -p "$destination"
    git -C "$destination" init -q
    git -C "$destination" remote add origin "$url"
    git -C "$destination" fetch -q --depth 1 origin "$revision"
    git -C "$destination" checkout -q --detach FETCH_HEAD
}

run_project() {
    name="$1"
    dir="$2"
    target="$3"
    echo "real-project: $name"
    (
        cd "$dir"
        "$C_BIN" build "$target" -j2
    )
}

# BGE is the long-lived first-party compatibility project. Pin the exact commit
# so changes in BGE cannot silently alter the C-BuildSystem release gate.
BGE_REV="1ce663622cbdeb1f094f0a84bd8ca96a4f55a165"
checkout_at "https://github.com/xt9y/BGE.git" "$BGE_REV" "$ROOT/BGE"
run_project "BGE@$BGE_REV" "$ROOT/BGE" bge

# Small external C project: one translation unit and a conventional public
# header. This catches simple source/include regressions independently of BGE.
checkout_at "https://github.com/DaveGamble/cJSON.git" "v1.7.19" "$ROOT/cjson"
cat >"$ROOT/cjson/build.c" <<'SRC'
#include <cbuild.h>
void build(C_Build *b) {
    C_Target *lib = c_static_library(b, "cjson");
    c_sources(lib, "cJSON.c");
    c_include(lib, ".");
    c_standard(lib, C_STANDARD_C11);
    c_warnings_strict(lib);
}
SRC
run_project "cJSON@v1.7.19" "$ROOT/cjson" cjson

# A second independent codebase keeps the real-project gate from becoming a
# BGE/cJSON-specific compatibility test.
checkout_at "https://github.com/benhoyt/inih.git" "r58" "$ROOT/inih"
cat >"$ROOT/inih/build.c" <<'SRC'
#include <cbuild.h>
void build(C_Build *b) {
    C_Target *lib = c_static_library(b, "inih");
    c_sources(lib, "ini.c");
    c_include(lib, ".");
    c_standard(lib, C_STANDARD_C11);
    c_warnings_strict(lib);
}
SRC
run_project "inih@r58" "$ROOT/inih" inih

echo "real-projects: ok"
