# NetMonitor — configure, build, optional tests (MSVC multi-config)
# Usage: .\build.ps1 [-Config Debug|Release] [-Test] [-Clean]

param(
    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Release',
    [switch] $Test,
    [switch] $Clean,
    [switch] $DriverArtifacts,
    [switch] $BuildDriver
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $Root 'build'

function Get-VisualStudioMsBuildPath {
    param(
        [string] $VersionRange = ''
    )

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        return $null
    }

    $args = @('-latest', '-products', '*', '-requires', 'Microsoft.Component.MSBuild')
    if ($VersionRange) {
        $args += @('-version', $VersionRange)
    }
    $args += @('-property', 'installationPath')

    $installPath = & $vswhere @args
    if (-not $installPath) {
        return $null
    }

    $msbuild = Join-Path $installPath 'MSBuild\Current\Bin\MSBuild.exe'
    if (Test-Path $msbuild) {
        return $msbuild
    }

    return $null
}

function Test-WdkInstalled {
    $kitsRoot = 'C:\Program Files (x86)\Windows Kits\10'
    if (-not (Test-Path $kitsRoot)) {
        return $false
    }

    $kmHeader = Get-ChildItem (Join-Path $kitsRoot 'Include') -Filter ntddk.h -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\km\\' } |
        Select-Object -First 1
    return $null -ne $kmHeader
}

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

if ($DriverArtifacts) {
    $DriverDir = Join-Path $Root 'driver'
    if (Test-Path $DriverDir) {
        Write-Host "Driver sources are in: $DriverDir"
        Write-Host "Build the driver with WDK/Visual Studio, then install with:"
        Write-Host "  powershell -ExecutionPolicy Bypass -File `"$DriverDir\install-driver.ps1`""
    }
}

if ($BuildDriver) {
    $driverProject = Join-Path $Root 'driver\NetMonitorDrv.vcxproj'
    $msbuild = Get-VisualStudioMsBuildPath '[17.0,18.0)'
    if (-not $msbuild) {
        $msbuild = Get-VisualStudioMsBuildPath
    }

    if (-not (Test-Path $driverProject)) {
        throw "Driver project not found: $driverProject"
    }
    if (-not $msbuild) {
        throw 'MSBuild not found. Install Visual Studio Build Tools or the Desktop C++ workload.'
    }
    if (-not (Test-WdkInstalled)) {
        throw 'WDK is not installed. Install Windows Driver Kit so the project can import the WindowsKernelModeDriver10.0 toolset.'
    }

    Write-Host "Building driver ($Config, x64)..."
    & $msbuild $driverProject /t:Build /p:Configuration=$Config /p:Platform=x64 /m
}
