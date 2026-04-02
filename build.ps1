# NetMonitor — configure, build, optional tests (MSVC multi-config)
# Usage: .\build.ps1 [-Config Debug|Release] [-Test] [-Clean]

param(
    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Release',
    [switch] $Test,
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $Root 'build'

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Removing $BuildDir ..."
    Remove-Item -Recurse -Force $BuildDir
}

Write-Host "Configuring CMake..."
cmake -S $Root -B $BuildDir @(
    '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
)

Write-Host "Building ($Config)..."
cmake --build $BuildDir --config $Config --parallel

if ($Test) {
    Write-Host "Running tests ($Config)..."
    ctest --test-dir $BuildDir -C $Config --output-on-failure
}

$Exe = Join-Path (Join-Path $BuildDir $Config) 'NetMonitor.exe'
Write-Host "Done: $Exe"
