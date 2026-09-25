$ErrorActionPreference = 'Stop'

function Invoke-Native([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$File $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
    }
}

# Regression guard: the Windows build must honor GNU Make's normal CC=cc
# default instead of hard-coding clang, and the runtime build backend must use
# plain ar instead of requiring llvm-ar. The shims below make cc/ar work while
# deliberately making direct clang/llvm-ar calls fail.
$realClang = (Get-Command clang.exe -ErrorAction Stop).Source
$realLlvmAr = (Get-Command llvm-ar.exe -ErrorAction Stop).Source
$toolShim = Join-Path $env:RUNNER_TEMP 'c-buildsystem-tool-shims'
Remove-Item -Recurse -Force $toolShim -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $toolShim | Out-Null

@"
@echo off
"$realClang" %*
"@ | Set-Content -Encoding Ascii (Join-Path $toolShim 'cc.cmd')

@"
@echo off
echo unexpected direct clang invocation 1>&2
exit /b 97
"@ | Set-Content -Encoding Ascii (Join-Path $toolShim 'clang.cmd')

@"
@echo off
"$realLlvmAr" %*
"@ | Set-Content -Encoding Ascii (Join-Path $toolShim 'ar.cmd')

@"
@echo off
echo unexpected direct llvm-ar invocation 1>&2
exit /b 98
"@ | Set-Content -Encoding Ascii (Join-Path $toolShim 'llvm-ar.cmd')

Remove-Item Env:CC -ErrorAction SilentlyContinue
Remove-Item Env:AR -ErrorAction SilentlyContinue
$env:Path = "$toolShim;$env:Path"

Invoke-Native 'make' @('clean')
Invoke-Native 'make' @()

if (-not (Test-Path 'build/c.exe')) { throw 'make did not produce build/c.exe' }
if (-not (Test-Path 'build/c-native.exe')) { throw 'make did not produce build/c-native.exe' }

Invoke-Native 'make' @('install')

$installRoot = Join-Path $env:LOCALAPPDATA 'Programs/C-BuildSystem'
$installBin = Join-Path $installRoot 'bin'
$installedC = Join-Path $installBin 'c.exe'
$installedNative = Join-Path $installRoot 'libexec/c-buildsystem/c-native.exe'
$installedHeader = Join-Path $installRoot 'include/cbuild.h'

if (-not (Test-Path $installedC)) { throw "missing installed c.exe: $installedC" }
if (-not (Test-Path $installedNative)) { throw "missing installed c-native.exe: $installedNative" }
if (-not (Test-Path $installedHeader)) { throw "missing installed cbuild.h: $installedHeader" }

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$pathEntries = @($userPath -split ';' | ForEach-Object { $_.TrimEnd('\\') })
if ($pathEntries -notcontains $installBin.TrimEnd('\\')) {
    throw "make install did not add $installBin to the user PATH"
}

# A child process cannot mutate its parent shell environment. Refresh the PATH
# exactly as a newly opened PowerShell window would see it, then require `c` to
# resolve by name from an unrelated working directory.
$env:Path = "$installBin;$env:Path"
$resolved = (Get-Command c -ErrorAction Stop).Source
if ([IO.Path]::GetFullPath($resolved) -ne [IO.Path]::GetFullPath($installedC)) {
    throw "c resolves to the wrong executable: $resolved"
}

$project = Join-Path $env:RUNNER_TEMP 'c buildsystem windows smoke'
Remove-Item -Recurse -Force $project -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $project | Out-Null
Push-Location $project
try {
    Invoke-Native 'c' @('init')

    $chainedOutput = (& c build run 2>$null) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "c build run failed with exit code $LASTEXITCODE" }
    if ($chainedOutput -notmatch 'Hello from C\.') { throw "unexpected c build run output: $chainedOutput" }

    $runOutput = (& c run 2>$null) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "c run failed with exit code $LASTEXITCODE" }
    if ($runOutput -notmatch 'Hello from C\.') { throw "unexpected c run output: $runOutput" }

    @'
#include <cbuild.h>

void build(C_Build *b) {
    C_Target *lib = c_static_library(b, "maths");
    c_sources(lib, "src/maths.c");

    C_Target *shared = c_shared_library(b, "shared");
    c_sources(shared, "src/shared.c");

    C_Target *test = c_test(b, "smoke");
    c_sources(test, "src/test.c");
}
'@ | Set-Content -NoNewline build.c

    @'
int maths(void) { return 42; }
'@ | Set-Content -NoNewline src/maths.c

    @'
__declspec(dllexport) int shared_value(void) { return 7; }
'@ | Set-Content -NoNewline src/shared.c

    @'
int main(void) { return 0; }
'@ | Set-Content -NoNewline src/test.c

    Invoke-Native 'c' @('build', 'maths')
    Invoke-Native 'c' @('build', 'shared')
    Invoke-Native 'c' @('test')

    if (-not (Test-Path 'build/debug/maths.a')) { throw 'static library did not use the expected Windows artifact path' }
    if (-not (Test-Path 'build/debug/shared.dll')) { throw 'shared library did not use the expected Windows .dll artifact name' }
    if (-not (Test-Path 'build/debug/smoke.exe')) { throw 'test target did not use the expected Windows .exe artifact name' }

    Invoke-Native 'c' @('clean')
    if (Test-Path 'build') { throw 'c clean did not remove build/' }
}
finally {
    Pop-Location
}

# CMake dependencies must use the same compiler family as the consuming target,
# must not require install() rules, and must make their runtime DLL available to
# an executable launched by `c run`.
$depRepo = Join-Path $env:RUNNER_TEMP 'c-buildsystem-cmake-fixture-dep'
Remove-Item -Recurse -Force $depRepo -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $depRepo | Out-Null

@'
cmake_minimum_required(VERSION 3.20)
project(fixturedep C)
add_library(fixturedep SHARED dep.c)
'@ | Set-Content -NoNewline (Join-Path $depRepo 'CMakeLists.txt')

@'
#pragma once
int fixture_value(void);
'@ | Set-Content -NoNewline (Join-Path $depRepo 'dep.h')

@'
__declspec(dllexport) int fixture_value(void) { return 42; }
'@ | Set-Content -NoNewline (Join-Path $depRepo 'dep.c')

Push-Location $depRepo
try {
    Invoke-Native 'git' @('init', '-b', 'main')
    Invoke-Native 'git' @('config', 'user.email', 'ci@example.invalid')
    Invoke-Native 'git' @('config', 'user.name', 'C-BuildSystem CI')
    Invoke-Native 'git' @('add', '.')
    Invoke-Native 'git' @('commit', '-m', 'fixture')
}
finally {
    Pop-Location
}

$depUri = 'file:///' + (($depRepo -replace '\\', '/') -replace ' ', '%20')
$cmakeProject = Join-Path $env:RUNNER_TEMP 'c buildsystem windows cmake dependency'
Remove-Item -Recurse -Force $cmakeProject -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path (Join-Path $cmakeProject 'src') -Force | Out-Null

@"
#include <cbuild.h>

void build(C_Build *b) {
    C_Target *app = c_executable(b, "app");
    c_sources(app, "src/main.c");

    C_Dependency *dep = c_git(b, "fixturedep", "$depUri", "main");
    c_dep_cmake(dep);
    c_dep_include(dep, ".");
    c_dep_link(dep, "fixturedep");
    c_use(app, dep);
}
"@ | Set-Content -NoNewline (Join-Path $cmakeProject 'build.c')

@'
#include "dep.h"
#include <stdio.h>

int main(void) {
    int value = fixture_value();
    printf("fixture=%d\n", value);
    return value == 42 ? 0 : 1;
}
'@ | Set-Content -NoNewline (Join-Path $cmakeProject 'src/main.c')

Push-Location $cmakeProject
try {
    $cmakeRunOutput = (& c build run 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "CMake dependency build/run failed:`n$cmakeRunOutput" }
    if ($cmakeRunOutput -notmatch 'fixture=42') { throw "unexpected CMake dependency output: $cmakeRunOutput" }
    if (-not (Test-Path 'build/debug/fixturedep.dll')) { throw 'CMake dependency DLL was not copied beside the executable' }
}
finally {
    Pop-Location
}

Write-Host 'Windows native smoke test passed.'
