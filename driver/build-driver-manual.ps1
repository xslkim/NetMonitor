param(
    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Debug',
    [string] $OutputName = 'NetMonitorDrv.sys'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path $root "build\manual"
$outputPath = Join-Path $outDir $OutputName
$sdkVersion = '10.0.26100.0'
$kitsRoot = 'C:\Program Files (x86)\Windows Kits\10'
$vsVcVars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'

if (-not (Test-Path $vsVcVars)) {
    throw "vcvars64.bat not found: $vsVcVars"
}

$includeArgs = @(
    '/I.'
    '/I..\src'
    ('/I"{0}\Include\{1}\km"' -f $kitsRoot, $sdkVersion)
    ('/I"{0}\Include\{1}\shared"' -f $kitsRoot, $sdkVersion)
    ('/I"{0}\Include\{1}\ucrt"' -f $kitsRoot, $sdkVersion)
)

$compileFlags = @(
    '/nologo'
    '/c'
    '/kernel'
    '/GS-'
    '/W4'
    '/DUNICODE'
    '/D_UNICODE'
    '/D_AMD64_=1'
    '/DAMD64=1'
    '/DNDIS630=1'
    '/D_WIN32_WINNT=0x0A00'
    '/DNTDDI_VERSION=0x0A000000'
    '/Fobuild\manual\\'
)

$sources = @(
    'NetMonitorDrv.c'
    'RateLimitTable.c'
    'WfpCallout.c'
)

$linkFlags = @(
    '/nologo'
    '/driver'
    '/subsystem:native'
    '/entry:DriverEntry'
    '/machine:x64'
    '/debug'
    '/incremental:no'
    '/nodefaultlib:libcmt'
    ('/out:"{0}"' -f $outputPath)
    ('/libpath:"{0}\Lib\{1}\km\x64"' -f $kitsRoot, $sdkVersion)
    'ntoskrnl.lib'
    'hal.lib'
    'ndis.lib'
    'fwpkclnt.lib'
    'libcntpr.lib'
    'bufferoverflowfastfailk.lib'
    'ntstrsafe.lib'
)

if (-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}

$compileCommand = 'call "{0}" >nul && cd /d "{1}" && cl {2} {3} {4}' -f
    $vsVcVars,
    $root,
    ($compileFlags -join ' '),
    ($includeArgs -join ' '),
    ($sources -join ' ')

$linkCommand = 'call "{0}" >nul && cd /d "{1}" && link {2} {3}' -f
    $vsVcVars,
    $root,
    ($linkFlags -join ' '),
    ('build\manual\NetMonitorDrv.obj build\manual\RateLimitTable.obj build\manual\WfpCallout.obj')

cmd.exe /c $compileCommand
cmd.exe /c $linkCommand

Get-Item $outputPath | Format-List FullName,Length,LastWriteTime