#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
C_BIN="${1:-$ROOT/build/c}"
INC="${2:-$ROOT/include}"
RUNTIME_TESTS="$ROOT/tests"
RUNTIME_BENCHMARKS="$ROOT/benchmarks"

if [ -e "$RUNTIME_TESTS" ] || [ -L "$RUNTIME_TESTS" ]; then
    echo "c: refusing to replace existing $RUNTIME_TESTS" >&2
    exit 1
fi
if [ -e "$RUNTIME_BENCHMARKS" ] || [ -L "$RUNTIME_BENCHMARKS" ]; then
    echo "c: refusing to replace existing $RUNTIME_BENCHMARKS" >&2
    exit 1
fi

cp -R "$ROOT/.github/ci/tests" "$RUNTIME_TESTS"
cp -R "$ROOT/.github/ci/benchmarks" "$RUNTIME_BENCHMARKS"
cleanup() {
    rm -rf "$RUNTIME_TESTS" "$RUNTIME_BENCHMARKS"
}
trap cleanup EXIT INT TERM HUP

sh "$RUNTIME_TESTS/smoke.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/wrapper_backends.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/dependency.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/offline_dependency.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/malformed_lockfile.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/compiler_only.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/source_dependency.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/mixed_language.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/api_baseline.sh" "$INC"
sh "$RUNTIME_TESTS/api_1x_surface.sh" "$INC"
sh "$RUNTIME_TESTS/api_guards.sh" "$INC"
python3 "$RUNTIME_TESTS/benchmark_contract.py"
sh "$RUNTIME_TESTS/direct_header.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/test_command.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/profiles.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/incremental_correctness.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/interrupted_compile.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/cache_artifact_recovery.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/filesystem_failure.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/target_graph.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/generated_source.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/cli_inspection.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/performance.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/advanced_performance.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/parallel_deps.sh" "$C_BIN" "$INC"
sh "$RUNTIME_TESTS/install_layout.sh" "$ROOT"
