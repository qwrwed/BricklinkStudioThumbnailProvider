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

From an elevated PowerShell prompt, in this folder:

```powershell
.\AddIoThumbnailProvider.ps1
```

This copies the built DLL to `C:\ProgramData\ThumbnailProviders\Io\`, registers that copy, associates the handler with the `.io` file type, and clears Explorer's thumbnail cache. Registering the fixed copy rather than the DLL in this folder means this repo can be moved or renamed afterwards without breaking the registration - rerun the script (to refresh the installed copy) only after rebuilding the DLL, not after moving this folder.

## Uninstall

From an elevated PowerShell prompt, in this folder:

```powershell
.\RemoveIoThumbnailProvider.ps1
```
