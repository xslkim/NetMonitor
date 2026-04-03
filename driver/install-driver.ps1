param(
    [string] $DriverPath = (Join-Path $PSScriptRoot 'build\manual\NetMonitorDrv.sys'),
    [string] $ServiceName = 'NetMonitorDrv',
    [switch] $Start
)

$ErrorActionPreference = 'Stop'

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Please run this script from an elevated PowerShell session.'
}

if (-not (Test-Path $DriverPath)) {
    throw "Driver not found: $DriverPath"
}

$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($null -eq $service) {
    sc.exe create $ServiceName type= kernel start= demand binPath= "$DriverPath" | Out-Host
} else {
    Write-Host "Service $ServiceName already exists."
}

if ($Start) {
    sc.exe start $ServiceName | Out-Host
}
