# RemoveIoThumbnailProvider.ps1
#
# What it does:
#   Unregisters the installed copy of IoThumbnailProvider.dll, removes the
#   .io file-association key this pair added, and deletes the deployed copy.
#
# What it touches (elevation required - writes to HKEY_CLASSES_ROOT):
#   HKCR\CLSID\{E7E4677B-5BBF-4459-BDCF-A97AE3BE21C8}                (deleted by DllUnregisterServer)
#   HKCR\.io\shellex\{e357fccd-a995-4576-b01f-234630154e96}          (deleted by DllUnregisterServer)
#   HKCR\io_auto_file\shellex\{e357fccd-a995-4576-b01f-234630154e96} (deleted by this script)
#   C:\ProgramData\ThumbnailProviders\Io\IoThumbnailProvider.dll     (deleted)
#
# Reverses: AddIoThumbnailProvider.ps1 in this folder.

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    throw "This script must be run from an elevated PowerShell prompt (writes to HKEY_CLASSES_ROOT)."
}

$installed = "$env:ProgramData\ThumbnailProviders\Io\IoThumbnailProvider.dll"
$regsvr32  = Join-Path $env:SystemRoot 'System32\regsvr32.exe'

if (Test-Path $installed) {
    & $regsvr32 /u /s $installed
    "unregistered: $installed"
} else {
    "not installed at $installed, skipping unregister"
}

reg delete "HKCR\io_auto_file\shellex\{e357fccd-a995-4576-b01f-234630154e96}" /f 2>$null | Out-Null
"removed: io_auto_file association"

Remove-Item (Split-Path -Parent $installed) -Recurse -Force -ErrorAction SilentlyContinue
"deleted: $(Split-Path -Parent $installed)"
