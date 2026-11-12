# Configure + build + test from a plain PowerShell (no Developer Prompt needed).
#   .\build.ps1              # debug
#   .\build.ps1 -Config release
#   .\build.ps1 -NoTests
param(
    [ValidateSet('debug', 'release')] [string]$Config = 'debug',
    [switch]$NoTests
)
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ workload was not found.' }

# Import the x64 MSVC environment into this PowerShell session.
# vcvars itself calls vswhere, so put the installer dir on PATH and keep its stderr quiet.
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = "$vs\VC\vcpkg" }

$preset = "windows-msvc-$Config"
Push-Location $PSScriptRoot
try {
    cmake --preset $preset
    if ($LASTEXITCODE) { throw 'configure failed' }
    cmake --build --preset $Config
    if ($LASTEXITCODE) { throw 'build failed' }
    if (-not $NoTests) {
        ctest --preset $Config
        if ($LASTEXITCODE) { throw 'tests failed' }
    }
} finally {
    Pop-Location
}
