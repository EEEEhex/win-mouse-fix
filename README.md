<p align="center">
  <strong>English</strong> · <a href="README.zh-CN.md">简体中文</a>
</p>

<br>

<div align="center">
  <a href="https://github.com/EEEEhex/win-mouse-fix">
    <img src="WinMouseFix/WinMouseFix.Settings/Assets/ProductIcon.png" width="160" height="160" alt="Win Mouse Fix icon">
  </a>
  <h1>Win Mouse Fix</h1>
  <p>Make your mouse better than a Windows touchpad!</p>
  <p>
    <a href="https://github.com/EEEEhex/win-mouse-fix/releases">Download</a>
    &nbsp;·&nbsp;
    <a href="https://github.com/EEEEhex/win-mouse-fix/issues">Help &amp; Feedback</a>
    &nbsp;·&nbsp;
    <a href="https://github.com/noah-nuebling/mac-mouse-fix">Mac Mouse Fix</a>
  </p>
</div>

<br>

Win Mouse Fix is an independent Windows re-creation of [Mac Mouse Fix](https://github.com/noah-nuebling/mac-mouse-fix), inspired by its design and gesture-driven mouse experience. Use side-button gestures to drag content, open Task View, and switch desktops on Windows 11.

## Installation

Requires **Windows 11 x64**.

1. Download the Windows x64 release archive from [Releases](https://github.com/EEEEhex/win-mouse-fix/releases). Choose the application package rather than GitHub’s **Source code** archives.
2. Extract the entire archive to a folder you want to keep. Keep the included files and the `Settings` folder together.
3. Run `WinMouseFix.Host.exe`. Click its system tray icon to open Settings, then turn on **General → Enable Win Mouse Fix**.
4. Optionally enable **General → Start when you sign in** to launch the app when you sign in to Windows.

No installer is required. Closing Settings keeps Win Mouse Fix running in the system tray.

## Uninstallation

1. Open Settings and turn off **General → Start when you sign in** if enabled.
2. Right-click the system tray icon and choose **Exit (退出)**. Wait for the tray icon to disappear.
3. Delete the folder where you extracted Win Mouse Fix.

To also remove saved settings, logs, and local validation records, open `%LOCALAPPDATA%` in File Explorer and delete the `WinMouseFix` folder. Skip this step to keep your settings for a future installation.
