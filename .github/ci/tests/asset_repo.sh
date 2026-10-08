#!/bin/sh
set -eu
C_BIN="$1"
INC="$2"
ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT INT TERM
export C_INCLUDE_DIR="$INC"
export C_CACHE_DIR="$ROOT/cache"

mkdir -p "$ROOT/models/nested" "$ROOT/app/src"
cd "$ROOT/models"
git init -q
git config user.name test
git config user.email test@example.invalid
printf 'first\n' > first.glb
printf 'second\n' > second.glb
printf 'nested\n' > nested/new.glb
git add .
git commit -qm initial
url="file://$ROOT/models"

cd "$ROOT/app"
cat > build.c <<EOF
#include <cbuild.h>
void build(C_Build *b) {
    C_Target *app = c_executable(b, "app");
    c_sources(app, "src/main.c");
    C_Dependency *models = c_git(b, "models", "$url", "master");
    c_dep_assets(models);
    c_use(app, models);
}
EOF
cat > src/main.c <<'EOF'
#include <casset.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "nested/new.glb";
    const char *path = c_asset("models", name);
    if (!path || c_asset("models", "../first.glb") != NULL ||
        c_asset("models", "/etc/passwd") != NULL) return 2;
    FILE *file = fopen(path, "rb");
    if (!file) return 3;
    char buf[40] = {0};
    int ok = fgets(buf, sizeof(buf), file) != NULL;
    fclose(file);
    if (!ok) return 4;
    printf("ASSET=%s\n", path);
    return 0;
}
EOF

# Selecting a runtime filename must materialize that file, not its siblings.
"$C_BIN" run -- first.glb > "$ROOT/one.log"
first_path="$(sed -n 's/^ASSET=//p' "$ROOT/one.log" | tail -n 1)"
[ -f "$first_path" ]
first_dir="$(dirname "$first_path")"
[ ! -e "$first_dir/second.glb" ]
[ ! -e "$first_dir/nested/new.glb" ]

# The default without a filename still exposes the full repository by path.
"$C_BIN" run > "$ROOT/all.log"
all_path="$(sed -n 's/^ASSET=//p' "$ROOT/all.log" | tail -n 1)"
[ -f "$all_path" ]
all_dir="$(dirname "$(dirname "$all_path")")"
[ -f "$all_dir/first.glb" ] && [ -f "$all_dir/second.glb" ]

# Explicit mapping narrows both the accessible names and the checkout.
python3 - <<'PY'
from pathlib import Path
p=Path("build.c")
s=p.read_text().replace("    c_dep_assets(models);",
"    c_dep_asset_only(models, \"second.glb\");")
p.write_text(s)
p=Path("src/main.c")
s=p.read_text().replace('"nested/new.glb"', '"second.glb"')
p.write_text(s)
PY
"$C_BIN" run > "$ROOT/mapped.log"
mapped_path="$(sed -n 's/^ASSET=//p' "$ROOT/mapped.log" | tail -n 1)"
mapped_dir="$(dirname "$mapped_path")"
[ -f "$mapped_dir/second.glb" ]
[ ! -e "$mapped_dir/first.glb" ]
[ ! -e "$mapped_dir/nested" ]

echo "asset repo auto/selected: ok"
