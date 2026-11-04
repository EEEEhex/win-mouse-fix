<p align="center">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

<br>

<div align="center">
  <a href="https://github.com/EEEEhex/win-mouse-fix">
    <img src="WinMouseFix/WinMouseFix.Settings/Assets/ProductIcon.png" width="160" height="160" alt="Win Mouse Fix 图标">
  </a>
  <h1>Win Mouse Fix</h1>
  <p>让你的鼠标，比windows触控板还好用!</p>
  <p>
    <a href="https://github.com/EEEEhex/win-mouse-fix/releases">下载</a>
    &nbsp;·&nbsp;
    <a href="https://github.com/EEEEhex/win-mouse-fix/issues">帮助与反馈</a>
    &nbsp;·&nbsp;
    <a href="https://github.com/noah-nuebling/mac-mouse-fix">Mac Mouse Fix</a>
  </p>
</div>

<br>

Win Mouse Fix 的灵感来自 [Mac Mouse Fix](https://github.com/noah-nuebling/mac-mouse-fix)，是在 Windows 上对 Mac Mouse Fix 的界面设计和鼠标交互体验的独立复刻。通过侧键手势拖动内容、打开任务视图和切换桌面，让普通鼠标也能使用触控板式手势。

## 安装

需要 **Windows 11 x64**。

1. 前往 [Releases](https://github.com/EEEEhex/win-mouse-fix/releases)，下载 Windows x64 程序压缩包。请选择程序包，而非 GitHub 自动生成的 **Source code** 源码包。
2. 将压缩包完整解压到准备长期存放的位置，保留随附文件和 `Settings` 文件夹的目录结构。
3. 运行 `WinMouseFix.Host.exe`。单击系统托盘图标打开设置，在 **通用 → 启动 Win Mouse Fix** 中开启鼠标增强。
4. 如果希望登录 Windows 后自动运行，可开启 **通用 → 登录 Windows 时启动**。

配置默认保存在 `%LOCALAPPDATA%\WinMouseFix`。在 **通用 → 更多设置 → 配置保存位置** 中，可选择 **保存到程序所在目录**，使用 `WinMouseFix.Host.exe` 同级的 `Data` 文件夹。切换时自动迁移数据并重启；移动或升级便携版时，请保留 `Data` 和 `WinMouseFix.storage.json`。

## 更新

在 **通用 → 检查更新** 中查看新版本和更新日志，点击 **立即更新** 后即可下载、查看进度、安装并自动重启。两种保存位置的配置都会保留。开启 **自动更新** 后，程序会在启动时及设定间隔检查更新；安装仍需您确认。网络请求自动使用 Windows 系统代理设置。

## 卸载

1. 打开设置；如果启用了 **通用 → 登录 Windows 时启动**，先将其关闭。
2. 右键单击系统托盘图标，选择 **退出**，等待托盘图标消失。
3. 删除之前解压的 Win Mouse Fix 程序文件夹。

如需同时清除设置、日志和本机验收记录，可在文件资源管理器地址栏输入 `%LOCALAPPDATA%`，删除其中的 `WinMouseFix` 文件夹。跳过这一步可以保留设置，供下次安装时使用。
