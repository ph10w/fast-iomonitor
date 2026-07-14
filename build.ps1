[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$ClientOnly,
    [ValidateSet('2022', 'Latest')]
    [string]$VisualStudio = '2022'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe was not found. Install Visual Studio with C++ desktop development.'
}

$installationPath = if ($VisualStudio -eq '2022') {
    & $vswhere -latest -products * -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
} else {
    & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $installationPath) {
    throw "No matching Visual Studio C++ installation was found (selection: $VisualStudio)."
}

$msbuild64 = Join-Path $installationPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'
$msbuild = if (Test-Path -LiteralPath $msbuild64) {
    $msbuild64
} else {
    Join-Path $installationPath 'MSBuild\Current\Bin\MSBuild.exe'
}
$target = if ($ClientOnly) {
    Join-Path $root 'client\IoMonitorClient.vcxproj'
} else {
    Join-Path $root 'FastIoMonitor.sln'
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $msbuild
$startInfo.UseShellExecute = $false
$startInfo.ArgumentList.Add($target)
$startInfo.ArgumentList.Add('/m')
$startInfo.ArgumentList.Add("/p:Configuration=$Configuration")
$startInfo.ArgumentList.Add('/p:Platform=x64')
$startInfo.ArgumentList.Add('/restore')

# ProcessStartInfo creates a normalized, case-insensitive environment block. This
# also avoids MSBuild failures if a parent process happens to expose both PATH and
# path entries.
$startInfo.Environment['PATH'] = $startInfo.Environment['PATH']
$process = [System.Diagnostics.Process]::Start($startInfo)
$process.WaitForExit()

if ($process.ExitCode -ne 0) {
    exit $process.ExitCode
}
