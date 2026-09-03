# Benchmark contract

C-BuildSystem benchmarks use one shared statistics/scenario contract so results from different projects can be compared and consumed consistently.

## Standard result schema

Each `results.json` contains `standard.schema_version = 1` and the following standard scenarios:

- `clean` — build from an empty output directory.
- `noop` — build with no source changes.
- `incremental` — the benchmark's representative change, with the invalidation/rebuild count recorded where applicable.

Both `c` and `cmake_ninja` are measured for the same scenario. Project-specific measurements may be present in addition to these standard scenarios.

## Standard run counts

- Clean: 3 measured runs.
- No-op: 10 measured runs.
- Incremental: 5 measured runs.
- Configuration/setup where measured: 3 runs.

## Statistics

Wall-clock time and resource metrics use the median as the primary reported value. Every standard timing summary retains all raw samples and reports:

- minimum wall time;
- median wall time;
- maximum wall time;
- population standard deviation;
- coefficient of variation;
- median user/system CPU time, RSS, page faults, context switches and filesystem I/O.

Wall time is stored in milliseconds. GNU `/usr/bin/time -v` supplies resource measurements. Object caching is disabled for the standard compile/build comparison.

## Project mappings

| Benchmark | Standard incremental scenario | Extra scenarios |
| --- | --- | --- |
| cJSON | one source file changed | fresh build-system/configuration setup |
| libcurl | `lib/curl_setup.h` fan-out edit | invocation-to-first-compiler latency |
| SDL3 | pinned real upstream update | configuration and archive-only work, controlled curves |
| Wireshark | one dissector source changed | ten-source stress point |

All benchmark source revisions are pinned. Benchmarks reject unfair comparisons when the generated C-BuildSystem target cannot preserve the source set or compile-affecting flags needed by the reference CMake/Ninja target.
