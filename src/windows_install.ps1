[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Prefix,
    [string]$Target = 'build/c.exe',
    [string]$Native = 'build/c-native.exe',
    [string]$Header = 'include/cbuild.h',
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

function Normalize-Path([string]$Path) {
    try {
        return [IO.Path]::GetFullPath($Path).TrimEnd('\')
    }
    catch {
        return $Path.TrimEnd('\')
    }
}

$root = [IO.Path]::GetFullPath($Prefix)
$bin = Join-Path $root 'bin'
$include = Join-Path $root 'include'
$libexec = Join-Path $root 'libexec/c-buildsystem'

if ($Uninstall) {
    $installedC = Join-Path $bin 'c.exe'
    $installedNative = Join-Path $libexec 'c-native.exe'
    $installedHeader = Join-Path $include 'cbuild.h'

    foreach ($path in @($installedC, $installedNative, $installedHeader)) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -Force -LiteralPath $path
        }
    }

    if (Test-Path -LiteralPath $libexec) {
        Remove-Item -Recurse -Force -LiteralPath $libexec
    }

    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if ($userPath) {
        $binKey = Normalize-Path $bin
        $kept = @(
            $userPath -split ';' |
                Where-Object { $_ -and (Normalize-Path $_) -ne $binKey }
        )
        [Environment]::SetEnvironmentVariable('Path', ($kept -join ';'), 'User')
    }

    Write-Host "Uninstalled c from $root"
    exit 0
}

foreach ($source in @($Target, $Native, $Header)) {
    if (-not (Test-Path -LiteralPath $source)) {
        throw "required install input does not exist: $source"
    }
}

New-Item -ItemType Directory -Force -Path $bin, $include, $libexec | Out-Null
Copy-Item -Force -LiteralPath $Target -Destination (Join-Path $bin 'c.exe')
Copy-Item -Force -LiteralPath $Native -Destination (Join-Path $libexec 'c-native.exe')
Copy-Item -Force -LiteralPath $Header -Destination (Join-Path $include 'cbuild.h')

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$entries = @()
if ($userPath) {
    $entries = @($userPath -split ';' | Where-Object { $_ })
}

$binKey = Normalize-Path $bin
$hasBin = $false
foreach ($entry in $entries) {
    if ((Normalize-Path $entry) -eq $binKey) {
        $hasBin = $true
        break
    }
}

if (-not $hasBin) {
    [Environment]::SetEnvironmentVariable('Path', (($entries + $bin) -join ';'), 'User')
    Write-Host "Added $bin to the user PATH. Open a new PowerShell window to use c everywhere."
}

Write-Host "Installed c to $(Join-Path $bin 'c.exe')"
