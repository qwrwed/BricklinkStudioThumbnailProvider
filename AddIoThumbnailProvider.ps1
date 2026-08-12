# AddIoThumbnailProvider.ps1
#
# What it does:
#   Copies IoThumbnailProvider.dll to a fixed machine-wide location and
#   registers that copy, instead of registering the DLL in this repo folder
#   directly.
#
#   The copy is why this repo can be moved or renamed afterwards without
#   breaking the registration: DllRegisterServer bakes the DLL's own absolute
#   path into HKCR\CLSID\...\InProcServer32 at register time (via
#   GetModuleFileNameW), so whatever path is registered has to stay put. The
#   fixed copy is that stable path; this repo folder no longer needs to be.
#
# Why:
#   This repo was previously moved without re-registering, which left
#   InProcServer32 pointing at a deleted folder and silently broke .io
#   thumbnails for every file until the stale path was found and re-registered
#   by hand.
#
# What it touches (elevation required - writes to HKEY_CLASSES_ROOT):
#   C:\ProgramData\ThumbnailProviders\Io\IoThumbnailProvider.dll  (created/overwritten)
#   HKCR\CLSID\{E7E4677B-5BBF-4459-BDCF-A97AE3BE21C8}              (created by DllRegisterServer)
#   HKCR\.io\shellex\{e357fccd-a995-4576-b01f-234630154e96}        (created by DllRegisterServer)
#   HKCR\io_auto_file\shellex\{e357fccd-a995-4576-b01f-234630154e96} (created by this script -
#     DllRegisterServer only writes the .io key above, but Explorer resolves
#     the handler through the ProgId .io is associated with, io_auto_file, not
#     through .io directly)
#   Explorer's thumbnail cache is cleared and explorer.exe is restarted.
#
# Rerun this after rebuilding IoThumbnailProvider.dll, to refresh the
# installed copy.
#
# Reverses with: RemoveIoThumbnailProvider.ps1 in this folder.

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    throw "This script must be run from an elevated PowerShell prompt (writes to HKEY_CLASSES_ROOT)."
}

$here      = Split-Path -Parent $MyInvocation.MyCommand.Path
$source    = Join-Path $here 'IoThumbnailProvider.dll'
$installed = "$env:ProgramData\ThumbnailProviders\Io\IoThumbnailProvider.dll"

if (-not (Test-Path $source)) { throw "IoThumbnailProvider.dll not found in $here. Build it first." }

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $installed) | Out-Null
Copy-Item $source $installed -Force
"installed: $installed"

$regsvr32 = Join-Path $env:SystemRoot 'System32\regsvr32.exe'
& $regsvr32 /s $installed
if ($LASTEXITCODE -ne 0) { throw "regsvr32 failed with exit code $LASTEXITCODE" }
"registered: $installed"

reg add "HKCR\io_auto_file\shellex\{e357fccd-a995-4576-b01f-234630154e96}" /ve /t REG_SZ /d "{E7E4677B-5BBF-4459-BDCF-A97AE3BE21C8}" /f | Out-Null
"associated: io_auto_file -> IoThumbnailProvider"

taskkill /f /im explorer.exe | Out-Null
Remove-Item "$env:LocalAppData\Microsoft\Windows\Explorer\thumbcache_*.db" -Force -ErrorAction SilentlyContinue
Start-Process explorer
"explorer restarted, thumbnail cache cleared"
