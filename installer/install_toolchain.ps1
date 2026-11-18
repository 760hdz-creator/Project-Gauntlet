# Project Gauntlet: install what building the game on this PC needs (run elevated by the installer).
# Everything comes from its maker's own servers and the player accepts their licenses:
#   - Microsoft Visual C++ Redistributable (the game)
#   - Visual Studio 2022 Build Tools, C++ workload (MSVC headers/libs, Windows SDK, CMake, Ninja)
#   - LLVM (clang-cl, the compiler the game code is tested with)
# Runs hidden; each installer shows its own progress window (passive mode). Skips anything already present. Log: %TEMP%\ProjectGauntlet_toolchain.log
# -ExcludeDir <folder>: also exclude that folder from Microsoft Defender (installer option; Defender's
#   machine-learning scan can flag the unsigned programs a game build makes). -NoTools: only that.
param([string]$ExcludeDir = '', [switch]$NoTools)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # PS 5.1 Invoke-WebRequest is very slow with the progress bar
$LlvmVersion = '23.1.4'
$log = Join-Path $env:TEMP 'ProjectGauntlet_toolchain.log'
Start-Transcript -Path $log -Append | Out-Null
$tmp = Join-Path $env:TEMP 'ProjectGauntlet_setup'
New-Item -ItemType Directory -Force $tmp | Out-Null
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if ($ExcludeDir) {
    try { Add-MpPreference -ExclusionPath $ExcludeDir; Write-Host "Defender exclusion added: $ExcludeDir" }
    catch { Write-Host "Could not add the Defender exclusion: $_" }
}
if ($NoTools) { Stop-Transcript | Out-Null; exit 0 }

function Get-File($url, $name) {
    $out = Join-Path $tmp $name
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing
    return $out
}

# 1. VC++ runtime (exit 1638 = newer already installed)
$redist = Get-File 'https://aka.ms/vs/17/release/vc_redist.x64.exe' 'vc_redist.x64.exe'
$p = Start-Process $redist -ArgumentList '/install', '/passive', '/norestart' -Wait -PassThru
Write-Host "vc_redist exit $($p.ExitCode)"

# 2. Build Tools with the C++ workload
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$haveVc = $false
if (Test-Path $vswhere) {
    $haveVc = [bool](& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath)
}
if (-not $haveVc) {
    $bt = Get-File 'https://aka.ms/vs/17/release/vs_buildtools.exe' 'vs_buildtools.exe'
    Write-Host 'Installing Visual Studio Build Tools (C++). This downloads about 2-3 GB and takes 5-15 minutes.'
    $p = Start-Process $bt -ArgumentList '--passive', '--wait', '--norestart', '--nocache',
        '--add', 'Microsoft.VisualStudio.Workload.VCTools', '--includeRecommended' -Wait -PassThru
    Write-Host "vs_buildtools exit $($p.ExitCode)"
    if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { throw "Build Tools install failed ($($p.ExitCode))" }
}

# 3. LLVM clang-cl
$clang = Join-Path $env:ProgramFiles 'LLVM\bin\clang-cl.exe'
if (-not (Test-Path $clang)) {
    $llvm = Get-File "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LlvmVersion/LLVM-$LlvmVersion-win64.msi" 'llvm.msi'
    Write-Host "Installing LLVM $LlvmVersion"
    $p = Start-Process msiexec.exe -ArgumentList '/i', "`"$llvm`"", '/passive', '/norestart' -Wait -PassThru
    if (-not (Test-Path $clang)) { throw "LLVM install failed ($($p.ExitCode))" }
}
Write-Host 'Build tools ready.'
Stop-Transcript | Out-Null
