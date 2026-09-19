param([Parameter(Mandatory=$true)][string]$GamePath)
$ErrorActionPreference='Stop'
$GamePath=[IO.Path]::GetFullPath($GamePath)
if(Get-Process TombRaider -ErrorAction SilentlyContinue){throw 'Close Tomb Raider before installing.'}
$expected='F36B8DD2BD74D48C14BF910AD9BD4AC9F4024433523FFC7E46D5C85C3DD618F5'
if((Get-FileHash -LiteralPath (Join-Path $GamePath 'TombRaider.exe') -Algorithm SHA256).Hash -ne $expected){throw 'Unsupported TombRaider.exe; installation cancelled.'}
$files=@('d3d11.dll','atidxx32.dll','atiadlxy.dll','TombRaiderVR\TombRaiderVRHost.exe')
if(!(Test-Path -LiteralPath (Join-Path $GamePath 'TombRaiderVR.ini'))){$files+='TombRaiderVR.ini'}
$payload=Join-Path $PSScriptRoot 'dist'
foreach($file in $files){if(!(Test-Path -LiteralPath (Join-Path $payload $file))){throw "Missing payload: $file"}}
$backup=Join-Path $GamePath ('TombRaiderVR\backups\'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $backup -Force | Out-Null
$manifest=@()
foreach($file in $files){
    $destination=Join-Path $GamePath $file
    $existed=Test-Path -LiteralPath $destination
    if($existed){$saved=Join-Path $backup $file;New-Item -ItemType Directory -Path (Split-Path $saved) -Force | Out-Null;Copy-Item -LiteralPath $destination -Destination $saved}
    $manifest+=@{path=$file;existed=$existed;installedHash=(Get-FileHash -LiteralPath (Join-Path $payload $file)).Hash}
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'manifest.json')
foreach($file in $files){$destination=Join-Path $GamePath $file;New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null;Copy-Item -LiteralPath (Join-Path $payload $file) -Destination $destination -Force}
Set-Content -LiteralPath (Join-Path $GamePath 'TombRaiderVR\last-install.txt') -Value $backup
Write-Output "Installed stereo diagnostic build. Rollback manifest: $backup\manifest.json"
