# Changelog

## 1.0.0 — 2026-09-04

First stable C-BuildSystem release.

### Stability contract

- Froze the intended `build.c` source API for the 1.x line and added compile-time API-surface regression coverage in C and C++.
- Added API version macros beginning at `1.0.0`.
- Defined supported platform/compiler tiers and made the release depend on their correctness jobs.
- Kept public struct layout outside the binary/source compatibility promise; supported build scripts use the documented API functions and enums.

### Correctness and compatibility

- Strengthened Git dependency testing with a locked offline rebuild regression.
- Locked rebuilds now exercise source-checkout rematerialization from the cached mirror, complete loss of the dependency origin, and fail-closed behavior when neither origin nor locked cache is available.
- Added pinned real-project validation for BGE, cJSON and inih.
- Expanded hosted CI across Ubuntu x86_64/arm64, GCC/Clang, macOS arm64/Intel, Debian and Alpine/musl.
- Existing sanitizer and fuzz gates remain part of the correctness chain.

### Benchmarks

- Standardized clean, no-op and incremental scenarios across cJSON, libcurl, SDL3 and Wireshark.
- Standardized measured run counts to 3 clean, 10 no-op and 5 incremental runs.
- Standardized median resource/timing summaries, raw samples, min/max, population standard deviation and coefficient of variation.
- Added a versioned `standard` benchmark result schema while retaining project-specific measurements.

### Release process

- `v1.0.0` is tagged and published by the single CI workflow only after every release-gated correctness/platform/real-project job succeeds on `main`.
- A failed or cancelled correctness gate cannot create the stable release.
