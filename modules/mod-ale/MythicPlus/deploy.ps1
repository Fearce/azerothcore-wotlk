# =============================================================================
# MythicPlus deploy — copies the Lua (AIO + MythicPlus) into the worldserver's
# ALE.ScriptPath ("lua_scripts"), installs the AIO client addon, and ensures the
# ALE config + logger are present. Run after a build, or after a clean checkout.
#
#   .\deploy.ps1 -Bin "D:\WowPs\server-ac-build\bin\RelWithDebInfo" `
#                -ClientAddons "D:\WowPs\ChromieCraft_3.3.5a\Interface\AddOns"
#
# SQL (run once, see Data/SQL) and the client MPQ texture patch (see
# Data/Client/README.md) are separate manual steps — this only does the Lua.
# =============================================================================
param(
    [string]$Bin = "D:\WowPs\server-ac-build\bin\RelWithDebInfo",
    [string]$ClientAddons = "D:\WowPs\ChromieCraft_3.3.5a\Interface\AddOns"
)
$ErrorActionPreference = "Stop"
$src = $PSScriptRoot
$lua = Join-Path $Bin "lua_scripts"

New-Item -ItemType Directory -Force $lua | Out-Null

# AIO server framework -> lua_scripts/AIO_Server
$dstAIO = Join-Path $lua "AIO_Server"
if (Test-Path $dstAIO) { Remove-Item -Recurse -Force $dstAIO }
Copy-Item -Recurse (Join-Path $src "AIO_Server") $dstAIO
Write-Host "  [ok] AIO_Server -> $dstAIO"

# MythicPlus server/client/locale Lua -> lua_scripts/MythicPlus
$dstMP = Join-Path $lua "MythicPlus"
if (Test-Path $dstMP) { Remove-Item -Recurse -Force $dstMP }
Copy-Item -Recurse (Join-Path $src "lua") $dstMP
Write-Host "  [ok] MythicPlus -> $dstMP"

# AIO client addon -> client Interface/AddOns/AIO_Client
if (Test-Path $ClientAddons) {
    $dstClient = Join-Path $ClientAddons "AIO_Client"
    if (Test-Path $dstClient) { Remove-Item -Recurse -Force $dstClient }
    Copy-Item -Recurse (Join-Path $src "AIO_Client") $dstClient
    Write-Host "  [ok] AIO_Client -> $dstClient"
} else {
    Write-Host "  [skip] client AddOns dir not found: $ClientAddons"
}

# mod_ale.conf from .dist if missing
$conf = Join-Path $Bin "configs\modules\mod_ale.conf"
$confDist = "$conf.dist"
if ((Test-Path $confDist) -and -not (Test-Path $conf)) {
    Copy-Item $confDist $conf
    Write-Host "  [ok] mod_ale.conf created from .dist"
}

# Surface ALE load logs at Info
$wsConf = Join-Path $Bin "configs\worldserver.conf"
if ((Test-Path $wsConf) -and -not (Select-String -Path $wsConf -Pattern "^Logger.ALE=" -Quiet)) {
    Add-Content $wsConf "`nLogger.ALE=4,Console Server"
    Write-Host "  [ok] Logger.ALE=4 added to worldserver.conf"
}

Write-Host "`nDeploy done. Remember (one-time): apply Data/SQL/* to the world+characters DBs,"
Write-Host "and build the client MPQ texture patch per Data/Client/README.md."
