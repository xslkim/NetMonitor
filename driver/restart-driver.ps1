param(
    [string] $ServiceName = 'NetMonitorDrv',
    [string] $DriverPath = (Join-Path $PSScriptRoot 'build\manual\NetMonitorDrv.sys'),
    [string] $StagedDriverPath = (Join-Path $PSScriptRoot 'build\manual\NetMonitorDrv.next.sys')
)

$ErrorActionPreference = 'Stop'

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Please run this script from an elevated PowerShell session.'
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

if (Test-Path $StagedDriverPath) {
    $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($null -ne $service -and $service.Status -ne 'Stopped') {
        sc.exe stop $ServiceName | Out-Host
        $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(15))
    }

    Copy-Item -Path $StagedDriverPath -Destination $DriverPath -Force
    Remove-Item $StagedDriverPath -Force
}

& (Join-Path $scriptRoot 'sign-driver.ps1') -DriverPath $DriverPath

$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($null -eq $service) {
    & (Join-Path $scriptRoot 'install-driver.ps1') -DriverPath $DriverPath -Start
} else {
    if ($service.Status -ne 'Stopped') {
        sc.exe stop $ServiceName | Out-Host
        $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(15))
    }
    sc.exe start $ServiceName | Out-Host
}

Get-Service -Name $ServiceName | Format-List Name,Status,StartType