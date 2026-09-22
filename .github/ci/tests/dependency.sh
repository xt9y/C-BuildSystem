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
printf 'font-v1\n' > font.png
git add answer.h font.png
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
    c_dep_asset(dep, "font.png", "Font/font.png");
    c_use(app, dep);
}
EOF2
cat > src/main.c <<'SRC'
#include <stdio.h>
#include <string.h>
#include <answer.h>
#include <casset.h>

int main(void) {
    const char *asset = c_asset("answer", "Font/font.png");
    if (!asset || c_asset("answer", "missing.file") != NULL) return 2;
    FILE *f = fopen(asset, "rb");
    if (!f) return 3;
    char value[32] = {0};
    if (!fgets(value, sizeof(value), f)) { fclose(f); return 4; }
    fclose(f);
    value[strcspn(value, "\r\n")] = '\0';
    printf("%d %s\n", ANSWER, value);
    return ANSWER == 42 && !strncmp(value, "font-v", 6) ? 0 : 5;
}
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
[ "$(git --git-dir="$mirror_cache" rev-parse --is-shallow-repository)" = "true" ]
[ "$(git --git-dir="$mirror_cache" rev-list --all --count)" -eq 1 ]
[ ! -e Font ]

# A failed checkout used to leave this directory behind and permanently poison
# the cache. Replace a known-good checkout with a partial one and require c to
# rebuild it automatically rather than trusting directory existence.
rm -rf "$src_cache" "$src_cache.c-ready"
mkdir -p "$src_cache"
printf '#define ANSWER 0\n' > "$src_cache/answer.h"
"$C_BIN" run | grep -q '^42 font-v1$'
[ -f "$src_cache.c-ready" ]
grep -q 'ANSWER 42' "$src_cache/answer.h"
[ ! -e Font ]
[ "$(cat "$src_cache/font.png")" = "font-v1" ]

# A failed `git clone --mirror` has the same failure mode. A malformed mirror
# directory must be discarded and cloned again on the next invocation.
rm -rf "$mirror_cache" "$mirror_cache.c-ready"
mkdir -p "$mirror_cache"
printf 'not-a-git-repository\n' > "$mirror_cache/HEAD"
"$C_BIN" fetch >/dev/null
[ -f "$mirror_cache.c-ready" ]
git --git-dir="$mirror_cache" rev-parse --verify 'HEAD^{commit}' >/dev/null
[ "$(git --git-dir="$mirror_cache" rev-parse --is-shallow-repository)" = "true" ]

"$C_BIN" run | grep -q '^42 font-v1$'
[ ! -e Font ]
old="$(grep resolved c.lock)"
locked_sha="$(sed -n 's/^resolved = "\(.*\)"/\1/p' c.lock)"
old_src="$src_cache"
old_mirror="$mirror_cache"

# Stale dependency build/package entries from older revisions must also be
# reclaimed by update, while unrelated dependency cache entries survive.
mkdir -p "$C_CACHE_DIR/pkg/answer-stale" "$C_CACHE_DIR/dep-build/answer-stale"
mkdir -p "$C_CACHE_DIR/pkg/other-keep" "$C_CACHE_DIR/dep-build/other-keep"

cd "$ROOT/headerdep"
cat > answer.h <<'HDR'
#define ANSWER 42
#define SECOND 1
HDR
printf 'font-v2\n' > font.png
git add answer.h font.png
git commit -qm second
cd "$ROOT/app"

# A cold depth-1 mirror now contains only the new upstream tip, while c.lock
# still points at the first commit. The locked commit must be fetched directly
# without unshallowing the dependency mirror.
rm -rf "$old_mirror" "$old_mirror.c-ready"
"$C_BIN" run | grep -q '^42 font-v1$'
[ -f "$old_mirror.c-ready" ]
[ "$(git --git-dir="$old_mirror" rev-parse --is-shallow-repository)" = "true" ]
git --git-dir="$old_mirror" cat-file -e "$locked_sha^{commit}"

"$C_BIN" update answer
new="$(grep resolved c.lock)"
[ "$old" != "$new" ]
[ ! -e "$old_src" ]
[ ! -e "$old_src.c-ready" ]
[ ! -e "$C_CACHE_DIR/pkg/answer-stale" ]
[ ! -e "$C_CACHE_DIR/dep-build/answer-stale" ]
[ -d "$C_CACHE_DIR/pkg/other-keep" ]
[ -d "$C_CACHE_DIR/dep-build/other-keep" ]
# The mirror is keyed by URL, so updating a ref on the same URL must reuse it.
[ -d "$old_mirror" ]
[ -f "$old_mirror.c-ready" ]
[ "$(git --git-dir="$old_mirror" rev-parse --is-shallow-repository)" = "true" ]
# An already-built binary must follow the new cached revision immediately;
# update must retarget the cache manifest before pruning the old checkout.
./build/debug/app | grep -q '^42 font-v2$'
"$C_BIN" run | grep -q '^42 font-v2$'
[ ! -e Font ]

# If a dependency changes URL, the old mirror is no longer useful to this
# project and must be reclaimed after the successful update.
cp -R "$ROOT/headerdep" "$ROOT/headerdep2"
git_url2="$ROOT/headerdep2"
sed "s|$git_url|$git_url2|" build.c > build.c.new
mv build.c.new build.c
"$C_BIN" update answer
[ ! -e "$old_mirror" ]
[ ! -e "$old_mirror.c-ready" ]
new_mirror="$(find "$C_CACHE_DIR/git" -mindepth 1 -maxdepth 1 -type d -name '*.git' | head -n 1)"
current_src="$(find "$C_CACHE_DIR/src" -mindepth 1 -maxdepth 1 -type d -name 'answer-*' | head -n 1)"
[ -n "$new_mirror" ]
[ -d "$new_mirror" ]
[ -n "$current_src" ]
[ -d "$current_src" ]
[ "$(git --git-dir="$new_mirror" rev-parse --is-shallow-repository)" = "true" ]
"$C_BIN" run | grep -q '^42 font-v2$'
[ ! -e Font ]

# Cleanup is post-success only. A failed update must not destroy the last
# usable checkout or mirror.
cp build.c build.c.good
sed "s|$git_url2|$ROOT/does-not-exist|" build.c.good > build.c
if "$C_BIN" update answer >/dev/null 2>&1; then
    echo "expected update failure" >&2
    exit 1
fi
[ -d "$new_mirror" ]
[ -f "$new_mirror.c-ready" ]
[ -d "$current_src" ]
[ -f "$current_src.c-ready" ]
[ ! -e Font ]
mv build.c.good build.c

[ "$($C_BIN cache)" = "$ROOT/cache" ]
mkdir -p "$C_CACHE_DIR/unrelated"
printf 'remove-me\n' > "$C_CACHE_DIR/unrelated/data"
# cclean is global and must work even outside a project directory.
cd "$ROOT"
"$C_BIN" cclean >/dev/null
[ ! -e "$ROOT/cache" ]
echo "dependency: ok"
