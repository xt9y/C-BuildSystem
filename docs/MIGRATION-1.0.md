# Migrating to 1.0

Most pre-1.0 `build.c` files require no mechanical rewrite. The stable release formalizes the API already used by current projects.

## Required checks

1. Build with the documented functions from `cbuild.h` rather than reading or writing `C_Build`, `C_Target`, `C_Dependency` or `C_StringList` fields directly.
2. Do not call internal `c__*` helpers.
3. Commit `c.lock` when the project has Git dependencies. Normal builds are expected to honor the resolved commit recorded there; use the explicit dependency-update command when you intend to move it.
4. For reproducible/offline work, preserve the configured C-BuildSystem cache. A lockfile identifies the dependency revision but does not contain the dependency bytes.
5. Use a supported POSIX host/compiler combination from `PLATFORMS.md` for release-critical builds.

## API version checks

Build scripts that need to detect the stable API can use:

```c
#if C_BUILD_API_VERSION_MAJOR != 1
#error "This build.c requires C-BuildSystem 1.x"
#endif
```

The 1.x contract allows compatible additions. Existing documented names/signatures and established build-description meanings will not be removed or incompatibly changed until a new major release.

## Dependencies

A generated `c.lock` is authoritative for ordinary builds. Updating the requested Git branch/tag in `build.c` alone does not silently move an already locked dependency. Use `c update` (or the dependency-specific update form) when changing the resolved revision is intentional.

Locked offline rebuilds are a permanent correctness regression: after a successful fetch/build, a rebuild must work from `c.lock` plus the cached Git mirror even when the origin is unavailable.

## Benchmarks

Consumers of benchmark JSON should prefer `standard` for cross-project comparisons. Existing project-specific fields remain present in 1.0, while `standard.schema_version` identifies the normalized schema.
