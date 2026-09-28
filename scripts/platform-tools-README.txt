DeskXR portable ADB folder

Extract Google's Android SDK Platform-Tools package so this directory contains:

  adb.exe
  AdbWinApi.dll
  AdbWinUsbApi.dll

Expected layout:

  DeskXR-Windows-x64\
    DeskXR.ps1
    adb-common.ps1
    platform-tools\
      adb.exe
      AdbWinApi.dll
      AdbWinUsbApi.dll

No Windows PATH or Android SDK environment-variable setup is required in this layout.
