param([Parameter(Mandatory=$true)][string]$GamePath)
$ErrorActionPreference='Stop'
$GamePath=[IO.Path]::GetFullPath($GamePath)
if(Get-Process TombRaider,TombRaiderVRHost -ErrorAction SilentlyContinue){throw 'Close Tomb Raider and its VR host first.'}
$backup=(Get-Content -LiteralPath (Join-Path $GamePath 'TombRaiderVR\last-install.txt') -Raw).Trim()
$allowed=@('d3d11.dll','atidxx32.dll','atiadlxy.dll','TombRaiderVR\TombRaiderVRHost.exe','TombRaiderVR.ini')
$backupRoot=Join-Path $GamePath 'TombRaiderVR\backups\'
if(!([IO.Path]::GetFullPath($backup)).StartsWith($backupRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid backup path.'}
$manifest=Get-Content -LiteralPath (Join-Path $backup 'manifest.json') -Raw | ConvertFrom-Json
foreach($item in $manifest){
    if($item.path -notin $allowed){throw 'Invalid manifest path.'}
    $target=Join-Path $GamePath $item.path
    if((Test-Path -LiteralPath $target) -and (Get-FileHash -LiteralPath $target).Hash -ne $item.installedHash){throw "File changed since installation: $target"}
}
foreach($item in $manifest){
    $target=Join-Path $GamePath $item.path
    if($item.existed){Copy-Item -LiteralPath (Join-Path $backup $item.path) -Destination $target -Force}
    elseif(Test-Path -LiteralPath $target){Remove-Item -LiteralPath $target}
}
Write-Output 'Restored the state before the latest installation. Logs and backups retained.'
