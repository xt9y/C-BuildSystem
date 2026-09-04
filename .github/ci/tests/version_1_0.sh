#!/bin/sh
set -eu
C_BIN="$1"
test "$("$C_BIN" --version)" = "1.0.0"
"$C_BIN" doctor | grep -q '^c 1\.0\.0$'
echo "version-1.0: ok"
