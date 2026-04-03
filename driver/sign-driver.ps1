param(
    [string] $DriverPath = (Join-Path $PSScriptRoot 'build\manual\NetMonitorDrv.sys'),
    [string] $Subject = 'CN=NetMonitor Test Driver',
    [string] $CertFile = (Join-Path $PSScriptRoot 'build\manual\NetMonitorDrv.cer'),
    [string] $SignToolPath = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
)

$ErrorActionPreference = 'Stop'

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Please run this script from an elevated PowerShell session.'
}

if (-not (Test-Path $DriverPath)) {
    throw "Driver not found: $DriverPath"
}

if (-not (Test-Path $SignToolPath)) {
    throw "signtool.exe not found: $SignToolPath"
}

$driverDir = Split-Path -Parent $DriverPath
if (-not (Test-Path $driverDir)) {
    New-Item -ItemType Directory -Path $driverDir | Out-Null
}

$cert = Get-ChildItem Cert:\LocalMachine\My |
    Where-Object { $_.Subject -eq $Subject } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1

if ($null -eq $cert) {
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $Subject `
        -CertStoreLocation 'Cert:\LocalMachine\My' `
        -KeyAlgorithm RSA `
        -KeyLength 4096 `
        -HashAlgorithm SHA256 `
        -KeyExportPolicy Exportable `
        -NotAfter (Get-Date).AddYears(3)
}

Export-Certificate -Cert $cert -FilePath $CertFile -Force | Out-Null
Import-Certificate -FilePath $CertFile -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath $CertFile -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null

& $SignToolPath sign /v /fd SHA256 /sha1 $cert.Thumbprint /sm /s My $DriverPath

Write-Host 'Driver signed successfully:'
Get-Item $DriverPath | Format-List FullName,Length,LastWriteTime
Write-Host ''
Write-Host 'Certificate thumbprint:' $cert.Thumbprint