# C-BuildSystem 1.x build.c API

C-BuildSystem 1.0 freezes the intended `build.c` source API for the 1.x release line.

## Compatibility contract

A `build.c` that uses only the documented 1.x API and does not access public struct fields directly is expected to remain source-compatible with later 1.x releases.

The contract covers:

- API version macros: `C_BUILD_API_VERSION_MAJOR`, `C_BUILD_API_VERSION_MINOR`, `C_BUILD_API_VERSION_PATCH`.
- Target kinds: `C_TARGET_EXECUTABLE`, `C_TARGET_STATIC_LIBRARY`, `C_TARGET_TEST`, `C_TARGET_SHARED_LIBRARY`.
- Dependency kinds: `C_DEP_HEADER_ONLY`, `C_DEP_RESERVED`, `C_DEP_SOURCE`.
- C standards: `C_STANDARD_C99`, `C_STANDARD_C11`, `C_STANDARD_C17`, `C_STANDARD_C23`.
- Target creation: `c_executable`, `c_static_library`, `c_shared_library`, `c_test`, `c_default_target`.
- Target configuration: `c_sources`, `c_include`, `c_define`, `c_flag`, `c_link_flag`, `c_link_system`, `c_framework`.
- Unity and language helpers: `c_unity`, `c_unity_auto`, `c_no_unity`, `c_standard`, `c_warnings_strict`.
- Generated/build graph APIs: `c_generate`, `c_link_target`.
- Git dependencies: `c_git`, `c_dep_header_only`, `c_dep_source`, `c_dep_include`, `c_dep_sources`, `c_dep_subdir`, `c_dep_flag`, `c_use`.

CI compiles exact function-pointer signatures for this surface as both C11 and C++17. Removing a listed API, changing its signature incompatibly, or changing an established build-description meaning incompatibly requires a new major release.

## What is not frozen

`C_Build`, `C_Target`, `C_Dependency` and `C_StringList` remain visible because `build.c` is intentionally small and header-only at the description layer. Their field layout is not a binary ABI and direct field access is not part of the supported 1.x source contract.

Internal `c__*` helpers are implementation details and are not covered by compatibility promises.

Compatible additions may be made during 1.x. The version macros identify the API level available to a build script.
