$ErrorActionPreference = 'Stop'

function Invoke-Native([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$File $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
    }
}

$env:CC = 'clang'
$env:AR = 'llvm-ar'

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
    Invoke-Native 'c' @('build')

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

Write-Host 'Windows native smoke test passed.'
