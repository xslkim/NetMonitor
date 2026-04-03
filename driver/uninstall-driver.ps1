param(
    [string] $ServiceName = 'NetMonitorDrv'
)

$ErrorActionPreference = 'Stop'

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Please run this script from an elevated PowerShell session.'
}

$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($null -eq $service) {
    Write-Host "Service $ServiceName is not installed."
    return
}

if ($service.Status -ne 'Stopped') {
    sc.exe stop $ServiceName | Out-Host
}

sc.exe delete $ServiceName | Out-Host