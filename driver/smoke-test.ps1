param(
    [string] $DriverServiceName = 'NetMonitorDrv',
    [string] $DriverPath = 'C:\d\NetMonitor\driver\build\manual\NetMonitorDrv.sys',
    [string] $AppPath = 'C:\d\NetMonitor\build\Debug\NetMonitor.exe'
)

$ErrorActionPreference = 'Stop'

Write-Host "Driver binary:"
Get-Item $DriverPath | Format-List FullName,Length,LastWriteTime

Write-Host "Application binary:"
Get-Item $AppPath | Format-List FullName,Length,LastWriteTime

Write-Host "Driver service status:"
$service = Get-Service -Name $DriverServiceName -ErrorAction SilentlyContinue
if ($null -eq $service) {
    Write-Host "Not installed"
} else {
    $service | Format-List Name,Status,StartType
}