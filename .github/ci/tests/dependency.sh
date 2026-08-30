#!/bin/sh
set -eu
C_BIN="$1"
INC="$2"
ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT INT TERM
export C_INCLUDE_DIR="$INC"
export C_CACHE_DIR="$ROOT/cache"

mkdir -p "$ROOT/headerdep"
cd "$ROOT/headerdep"
git init -q
git config user.name test
git config user.email test@example.invalid
cat > answer.h <<'HDR'
#define ANSWER 42
HDR
git add answer.h
git commit -qm initial

git_url="$ROOT/headerdep"
mkdir -p "$ROOT/app/src"
cd "$ROOT/app"
cat > build.c <<EOF2
#include <cbuild.h>
void build(C_Build *b) {
    C_Target *app = c_executable(b, "app");
    c_sources(app, "src/*.c");
    C_Dependency *dep = c_git(b, "answer", "$git_url", "master");
    c_dep_header_only(dep);
    c_use(app, dep);
}
EOF2
cat > src/main.c <<'SRC'
#include <stdio.h>
#include <answer.h>
int main(void) { printf("%d\n", ANSWER); return ANSWER == 42 ? 0 : 1; }
SRC
"$C_BIN" fetch
[ -f c.lock ]
"$C_BIN" deps | grep -q 'answer'

src_cache="$(find "$C_CACHE_DIR/src" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
mirror_cache="$(find "$C_CACHE_DIR/git" -mindepth 1 -maxdepth 1 -type d -name '*.git' | head -n 1)"
[ -n "$src_cache" ]
[ -n "$mirror_cache" ]
[ -f "$src_cache.c-ready" ]
[ -f "$mirror_cache.c-ready" ]

# A failed checkout used to leave this directory behind and permanently poison
# the cache. Replace a known-good checkout with a partial one and require c to
# rebuild it automatically rather than trusting directory existence.
rm -rf "$src_cache" "$src_cache.c-ready"
mkdir -p "$src_cache"
printf '#define ANSWER 0\n' > "$src_cache/answer.h"
"$C_BIN" run | grep -q '^42$'
[ -f "$src_cache.c-ready" ]
grep -q 'ANSWER 42' "$src_cache/answer.h"

# A failed `git clone --mirror` has the same failure mode. A malformed mirror
# directory must be discarded and cloned again on the next invocation.
rm -rf "$mirror_cache" "$mirror_cache.c-ready"
mkdir -p "$mirror_cache"
printf 'not-a-git-repository\n' > "$mirror_cache/HEAD"
"$C_BIN" fetch >/dev/null
[ -f "$mirror_cache.c-ready" ]
git --git-dir="$mirror_cache" rev-parse --verify 'HEAD^{commit}' >/dev/null

"$C_BIN" run | grep -q '^42$'
old="$(grep resolved c.lock)"

cd "$ROOT/headerdep"
cat > answer.h <<'HDR'
#define ANSWER 42
#define SECOND 1
HDR
git add answer.h
git commit -qm second
cd "$ROOT/app"
"$C_BIN" update answer
new="$(grep resolved c.lock)"
[ "$old" != "$new" ]
"$C_BIN" run >/dev/null
[ "$($C_BIN cache)" = "$ROOT/cache" ]
"$C_BIN" cache clean >/dev/null
[ ! -d "$ROOT/cache" ]
echo "dependency: ok"
