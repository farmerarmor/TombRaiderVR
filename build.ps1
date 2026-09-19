param([string]$DependencyRoot='')
$ErrorActionPreference='Stop'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if(!(Test-Path -LiteralPath $vswhere)){throw 'Install Visual Studio 2022 with Desktop development with C++.'}
$vs=& $vswhere -latest -version '[17.0,18.0)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Visual Studio 2022 C++ tools not found.'}
$vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
foreach($arch in @('x86','x64')) {
    $buildDir=Join-Path $PSScriptRoot "build/$arch"
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    $vcarch=if($arch -eq 'x86'){'x64_x86'}else{'x64'}
    $deps=''
    if($DependencyRoot){
        $deps=' -DFETCHCONTENT_SOURCE_DIR_OPENXR="'+[IO.Path]::GetFullPath((Join-Path $DependencyRoot 'openxr-src'))+'" -DFETCHCONTENT_SOURCE_DIR_MINHOOK="'+[IO.Path]::GetFullPath((Join-Path $DependencyRoot 'minhook-src'))+'"'
    }
    $script=Join-Path $buildDir 'build.cmd'
    @("@call `"$vcvars`" $vcarch >nul", '@if errorlevel 1 exit /b %errorlevel%', "@cmake -S `"$PSScriptRoot`" -B `"$buildDir`" -G `"NMake Makefiles`" -DCMAKE_BUILD_TYPE=Release$deps", '@if errorlevel 1 exit /b %errorlevel%', "@cmake --build `"$buildDir`"") | Set-Content -LiteralPath $script -Encoding ascii
    & cmd.exe /d /c $script
    if($LASTEXITCODE){throw "Build failed for $arch"}
}
$dist=Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Force -Path (Join-Path $dist 'TombRaiderVR') | Out-Null
foreach($dll in @('d3d11.dll','atidxx32.dll','atiadlxy.dll')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot "build/x86/bin/$dll") -Destination $dist -Force}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build/x64/bin/TombRaiderVRHost.exe') -Destination (Join-Path $dist 'TombRaiderVR') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'TombRaiderVR.ini') -Destination $dist -Force
Write-Output 'Build complete. Installable files are in dist/.'
