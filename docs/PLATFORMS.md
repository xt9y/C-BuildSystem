# Supported platforms and compilers

C-BuildSystem 1.x is a POSIX build tool. Native Windows is not a supported host.

## Tier 1 — release-gated

Every 1.0 release must pass the complete correctness suite on these hosted-runner combinations:

| Host | Architecture | Compiler |
| --- | --- | --- |
| Ubuntu 22.04 | x86_64 | GCC 11+ |
| Ubuntu 24.04 | x86_64 | GCC 13+ |
| Ubuntu 24.04 | x86_64 | LLVM Clang 18+ |
| Ubuntu 24.04 | arm64 | GCC 13+ |
| macOS 15 | arm64 | Apple Clang 16+ |
| macOS 15 | x86_64 | Apple Clang 16+ |

The GitHub Actions runner labels used by the release gate are `ubuntu-22.04`, `ubuntu-24.04`, `ubuntu-24.04-arm`, `macos-15` and `macos-15-intel`.

## Tier 2 — distro regression

The full build/test suite also runs inside current pinned distro containers on an Ubuntu hosted runner:

- Debian 13 x86_64 with the distro GCC toolchain.
- Alpine 3.22 x86_64 with musl and the distro GCC toolchain.

These jobs specifically cover glibc-vs-musl and distro/userland differences. They are release-gated for 1.0.

## Compiler contract

- `build.c` is compiled as C11 or newer.
- GCC 11 and newer are supported on Linux.
- LLVM Clang 14 and newer are supported on Linux.
- Apple Clang 16 and newer is supported on macOS.
- C++ source targets require a matching C++ compiler/runtime available to the selected toolchain; `build.c` itself remains C.
- Cross-compilers can be supplied through the normal compiler environment, but a target/SDK combination not represented above is best-effort rather than release-gated.

## Required host behavior

The host must provide the POSIX facilities used by `c`, including a filesystem with normal rename/link semantics, `/bin/sh`, process creation, and a Git executable for Git dependencies.

## Not supported

- Native Windows (`cmd.exe`/Win32) is not supported in 1.x.
- MSVC is not supported in 1.x.
- Non-POSIX shells as the only command execution environment are not supported.

WSL is treated as Linux when its distro/compiler combination satisfies the Linux requirements, but WSL itself is not a separate release-gated platform.
