# NetMonitor Build Script
# Usage: .\build.ps1 [-Config Release|Debug] [-Clean]

param(
    [string]$Config = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

# Check prerequisites
Write-Host "=== Checking prerequisites ===" -ForegroundColor Cyan
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (!$cmake) {
    Write-Host "ERROR: CMake not found. Please install CMake 3.20+." -ForegroundColor Red
    exit 1
}
Write-Host "OK: CMake found" -ForegroundColor Green

# Clean if requested
if ($Clean) {
    Write-Host "=== Cleaning ===" -ForegroundColor Cyan
    if (Test-Path "build") {
        Remove-Item -Recurse -Force "build" -ErrorAction SilentlyContinue
        Write-Host "OK: Cleaned build/" -ForegroundColor Green
    }
}

# Create build directory
Write-Host "=== Configuring CMake ===" -ForegroundColor Cyan
if (!(Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}
Push-Location "build"

try {
    # Configure
    Write-Host "Running: cmake -G 'Visual Studio 17 2022' -A x64 .."
    & cmake -G "Visual Studio 17 2022" -A x64 ..
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: CMake configuration failed." -ForegroundColor Red
        exit 1
    }
    Write-Host "OK: CMake configured" -ForegroundColor Green

    # Build
    Write-Host ""
    Write-Host "=== Building ($Config) ===" -ForegroundColor Cyan
    & cmake --build . --config $Config
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Build failed." -ForegroundColor Red
        exit 1
    }
    Write-Host "OK: Build successful" -ForegroundColor Green

    # Run tests
    Write-Host ""
    Write-Host "=== Running tests ===" -ForegroundColor Cyan
    & ctest --output-on-failure -C $Config
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Tests failed." -ForegroundColor Red
        exit 1
    }
    Write-Host "OK: All tests passed" -ForegroundColor Green

    # Summary
    Write-Host ""
    Write-Host "=== Build Summary ===" -ForegroundColor Cyan
    $exePath = "$pwd/$Config/NetMonitor.exe"
    if (Test-Path $exePath) {
        $size = [math]::Round((Get-Item $exePath).Length / 1MB, 1)
        Write-Host "Executable: $exePath ($size MB)" -ForegroundColor Green
        Write-Host "To run: $exePath" -ForegroundColor Green
    }
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "SUCCESS: Build completed!" -ForegroundColor Green
Write-Host ""
