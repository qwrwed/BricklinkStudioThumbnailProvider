# .io (_BrickLink Studio_) Windows Thumbnail Provider

A Windows shell extension that generates thumbnail previews for [BrickLink Studio](https://www.bricklink.com/v3/studio/download.page) (`.io`) model files in Explorer.

A `.io` file is a ZIP archive; Studio stores a rendered `thumbnail.png` at the archive root. This handler extracts that PNG and hands it to Explorer.

## Contents

- [.io (_BrickLink Studio_) Windows Thumbnail Provider](#io-bricklink-studio-windows-thumbnail-provider)
  - [Contents](#contents)
  - [Requirements](#requirements)
  - [Build](#build)
  - [Install](#install)
  - [Uninstall](#uninstall)

## Requirements

- [_Visual Studio Build Tools_](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2026) with "_Desktop development with C++_" workload

## Build

From an _x64 Native Tools Command Prompt_:

```cmd
cl /LD /EHsc /O2 IoThumbnailProvider.cpp third_party\miniz\miniz.c /link /DEF:IoThumbnailProvider.def ole32.lib oleaut32.lib shlwapi.lib gdiplus.lib advapi32.lib shell32.lib /OUT:IoThumbnailProvider.dll
```

## Install

1. Register the DLL from an elevated prompt:

```cmd
regsvr32 path\to\IoThumbnailProvider.dll
```

2. Associate the handler with the `.io` file type:

```cmd
reg add "HKCR\io_auto_file\shellex\{e357fccd-a995-4576-b01f-234630154e96}" /ve /t REG_SZ /d "{E7E4677B-5BBF-4459-BDCF-A97AE3BE21C8}" /f
```

3. Clear the thumbnail cache and restart Explorer:

PowerShell:

```powershell
taskkill /f /im explorer.exe
Remove-Item "$env:LocalAppData\Microsoft\Windows\Explorer\thumbcache_*.db" -Force
Start-Process explorer
```

cmd:

```cmd
taskkill /f /im explorer.exe
del /f "%LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db"
start explorer
```

## Uninstall

```cmd
regsvr32 /u IoThumbnailProvider.dll
reg delete "HKCR\io_auto_file\shellex\{e357fccd-a995-4576-b01f-234630154e96}" /f
```
