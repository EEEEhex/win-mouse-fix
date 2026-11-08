using System;
using System.Linq;
using System.IO;
using System.Text.Json;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using WinMouseFix.Configuration;
using WinMouseFix.Settings.Resources;
using Windows.System;

namespace WinMouseFix.Settings;
public sealed partial class MainWindow
{
    private static string CurrentProductVersion => typeof(MainWindow).Assembly.GetName().Version!.ToString(3);
    private ContentDialog? _updateDialog;
    private ProgressBar? _updateProgress;
    private TextBlock? _updateProgressText;
    private bool UpdateBusy => Model.Status.Update.State is "downloading" or "verifying" or "ready" or "installing";
    private async Task CheckForUpdatesAsync() => await ShowUpdateAsync(null);
    private void TryShowUpdateReminder() {
        if (!Model.Connected || _closing || Model.Status.FullscreenDetected || _updateDialog is not null || _bindingDialog is not null || Root.XamlRoot is null || !Model.Status.Update.NotificationPending) return;
        if (VisualTreeHelper.GetOpenPopupsForXamlRoot(Root.XamlRoot).Any(p => p.IsOpen)) return;
        _ = ShowUpdateAsync(Model.Status.Update);
    }
    private async Task ShowUpdateAsync(UpdateStatus? automatic) {
        if (_updateDialog is not null || _closing || (automatic is not null && Model.Status.FullscreenDetected)) return;
        var dialog = _updateDialog = new ContentDialog { XamlRoot = Root.XamlRoot, Title = UiText.Get("检查更新"), CloseButtonText = UiText.Get("取消"), DefaultButton = ContentDialogButton.Close };
        bool closed = false; bool acting = false; dialog.Closed += (_,_) => closed = true;
        dialog.Content = Stack(new ProgressRing { IsActive = true, Width = 28, Height = 28 },Text(UiText.Get("正在检查 GitHub Releases…")));
        dialog.PrimaryButtonClick += async (_,e) => {
            if (acting || dialog.Tag is not UpdateStatus result) { e.Cancel = true; return; }
            var deferral = e.GetDeferral();
            try {
                acting = true;
                if (!result.CanInstall && ValidReleaseUrl(result.Url)) { e.Cancel = !await Launcher.LaunchUriAsync(new Uri(result.Url)); return; }
                e.Cancel = true;
                if (!await Model.FlushAsync()) throw new InvalidOperationException(UiText.Get("请先保存设置后再更新。"));
                var reply = await _session.DownloadUpdateAsync(result.CompletedId);
                if (!reply.Ok) throw new InvalidOperationException(UiText.Get("无法开始更新，请重新检查版本后重试。"));
                FillUpdateDialog(dialog,Model.Status.Update);
            } catch (Exception error) { e.Cancel = true; dialog.Content = Text(error.Message); }
            finally { acting = false; deferral.Complete(); }
        };
        dialog.CloseButtonClick += async (_,e) => {
            if (dialog.Tag is not UpdateStatus update) return;
            if (update.State == "installing") { e.Cancel = true; return; }
            if (update.State is "downloading" or "verifying" or "ready") {
                var deferral = e.GetDeferral();
                try { var reply = await _session.CancelUpdateAsync(); e.Cancel = !reply.Ok; }
                catch { e.Cancel = true; }
                finally { deferral.Complete(); }
            }
        };
        dialog.Opened += async (_,_) => {
            try {
                var result = automatic;
                if (result is null && Model.Status.Update.State is "downloading" or "verifying" or "ready" or "installing" or "download-error") result = Model.Status.Update;
                if (result is null) {
                    var reply = await _session.CheckUpdatesAsync(); if (!reply.Ok) throw new InvalidOperationException(reply.Message);
                    result = JsonSerializer.Deserialize<HostStatus>(reply.Payload.GetProperty("status").GetRawText(),ConfigurationJson.Options)!.Update;
                    var id = result.CheckId; var started = DateTime.UtcNow;
                    while (result.CompletedId < id || result.State == "checking") {
                        if (closed || _closing) return;
                        if (!Model.Connected) throw new InvalidOperationException(UiText.Get("后台已断开，请重新打开设置后检查。"));
                        if (DateTime.UtcNow-started > TimeSpan.FromMinutes(4)) throw new TimeoutException(UiText.Get("检查更新超时，请稍后重试。"));
                        await Task.Delay(100); result = Model.Status.Update;
                    }
                }
                if (closed || _closing) return;
                FillUpdateDialog(dialog,result);
                if (result.State == "available") await _session.AcknowledgeUpdateAsync(result.CompletedId);
                var lastState = result.State;
                while (!closed && !_closing) {
                    await Task.Delay(150);
                    if (closed || _closing || !Model.Connected) return;
                    var current = Model.Status.Update;
                    if (current.State != lastState) { FillUpdateDialog(dialog,current); lastState = current.State; }
                    else if (current.State == "downloading") UpdateDownloadProgress(current);
                }
            } catch (Exception error) {
                if (!closed && !_closing) { dialog.Title = UiText.Get("检查更新失败"); dialog.Content = Text(error.Message); dialog.CloseButtonText = UiText.Get("知道了"); }
            }
        };
        try { await dialog.ShowAsync(); }
        catch (Exception error) { _uiError = error.Message; }
        finally { _updateDialog = null; }
    }
    private static bool ValidReleaseUrl(string url) => Uri.TryCreate(url,UriKind.Absolute,out var value) && value.Scheme == "https" && value.Host.Equals("github.com",StringComparison.OrdinalIgnoreCase) && value.AbsolutePath.StartsWith("/EEEEhex/win-mouse-fix/releases/tag/",StringComparison.Ordinal);
    private void FillUpdateDialog(ContentDialog dialog,UpdateStatus update) {
        dialog.Tag = update; dialog.CloseButtonText = UiText.Get("知道了"); dialog.PrimaryButtonText = ""; _updateProgress = null; _updateProgressText = null;
        if (update.State is "downloading" or "verifying" or "ready" or "installing") {
            dialog.Title = UiText.Get(update.State == "downloading" ? "正在下载更新" : update.State == "installing" ? "正在安装更新" : "正在校验更新");
            _updateProgress = new ProgressBar { Minimum = 0, Maximum = 100, IsIndeterminate = update.State != "downloading" || update.TotalBytes == 0, HorizontalAlignment = HorizontalAlignment.Stretch };
            _updateProgressText = Secondary("");
            dialog.Content = Stack(Text(UiText.Format("新版本：v{0}",update.Version)),_updateProgress,_updateProgressText,Secondary(UiText.Get("下载完成后将自动安装并重启 Win Mouse Fix。")));
            dialog.CloseButtonText = update.State == "installing" ? "" : UiText.Get("取消更新"); UpdateDownloadProgress(update); return;
        }
        if (update.State is "download-error" or "cancelled") {
            dialog.Title = UiText.Get(update.State == "cancelled" ? "更新已取消" : "更新未完成");
            var reason = update.State == "cancelled" ? UiText.Get("当前版本可继续使用。") : UpdateError(update.Error);
            dialog.Content = Stack(Text(reason),Secondary(UiText.Get("当前版本和配置保持不变，可以稍后重试。")));
            dialog.PrimaryButtonText = UiText.Get("重试"); dialog.IsPrimaryButtonEnabled = update.CanInstall; return;
        }
        if (update.State == "available") {
            dialog.Title = UiText.Format("发现新版本 v{0}",update.Version);
            var notes = Text(string.IsNullOrWhiteSpace(update.Notes) ? UiText.Get("此版本未提供更新日志。") : update.Notes); notes.IsTextSelectionEnabled = true;
            var content = Stack(Secondary(UiText.Format("当前版本：v{0}",CurrentProductVersion)));
            if (update.Title.Trim() != "v"+update.Version && update.Title.Trim() != update.Version) content.Children.Add(Text(update.Title));
            content.Children.Add(new ScrollViewer { Content = notes, MaxHeight = 340, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, MaxWidth = 560 }); dialog.Content = content;
            content.Children.Add(Secondary(UiText.Get(update.CanInstall ? "下载完成后将自动安装并重启 Win Mouse Fix。" : "此版本未提供可自动安装的完整更新包，请前往发布页查看。")));
            dialog.PrimaryButtonText = UiText.Get(update.CanInstall ? "立即更新" : "前往发布页"); dialog.IsPrimaryButtonEnabled = update.CanInstall || ValidReleaseUrl(update.Url); dialog.CloseButtonText = UiText.Get("稍后");
            dialog.DefaultButton = ContentDialogButton.Primary;
        } else if (update.State == "current") {
            dialog.Title = UiText.Get("已是最新版本"); dialog.Content = Text(UiText.Format("当前版本：v{0}",CurrentProductVersion));
        } else if (update.State == "no-releases") {
            dialog.Title = UiText.Get("暂无可比较的版本"); dialog.Content = Text(UiText.Get("尚未找到标题含有效版本号的正式 Release。"));
        } else {
            dialog.Title = UiText.Get("检查更新失败");
            dialog.Content = Text(UiText.Get(update.Error == "rate-limit" ? "GitHub 请求受到限制，请稍后重试。" : update.Error == "http-404" ? "无法访问公开 Releases，请确认仓库地址正确且仓库已公开。" : "无法获取更新信息。请检查网络及系统代理设置，稍后重试。"));
        }
    }
    private void UpdateDownloadProgress(UpdateStatus update) {
        if (_updateProgress is null || _updateProgressText is null) return;
        if (update.State == "downloading") {
            _updateProgress.IsIndeterminate = update.TotalBytes == 0;
            _updateProgress.Value = update.TotalBytes == 0 ? 0 : Math.Clamp(100d*update.ReceivedBytes/update.TotalBytes,0,100);
            _updateProgressText.Text = $"{update.ReceivedBytes/1048576d:F1} / {update.TotalBytes/1048576d:F1} MB · {update.BytesPerSecond/1048576d:F1} MB/s";
        } else _updateProgressText.Text = UiText.Get(update.State == "installing" ? "正在安全退出后台和设置窗口…" : "正在检查文件完整性并准备安装…");
    }
    private static string UpdateError(string error) => UiText.Get(error switch {
        "package-integrity" or "invalid-zip" or "invalid-manifest" or "version-mismatch" or "invalid-checksum" or "invalid-package" => "更新包校验失败，请重新下载。",
        "missing-install-manifest" => "当前目录缺少发布清单，请先使用完整发布包手动覆盖一次。",
        "write-failed" or "configuration-save-failed" or "insufficient-space" => "无法写入更新文件，请检查程序目录权限和磁盘空间。",
        "updater-start-failed" or "updater-handoff-failed" => "无法启动更新器，当前版本将继续运行。",
        "update-already-pending" => "存在未完成的更新，请重启程序以恢复。",
        _ => "更新失败，请检查网络、系统代理和程序目录写入权限后重试。"
    });
    private async Task RunUpdateSmokeAsync(string directory) {
        var fake = (Services.FakeHostSession)_session;
        foreach (var state in new[]{"available","current","no-releases","error"}) {
            fake.NextUpdate = new() { State=state,Version="2.2.0",Title="v2.2.0",Notes="# 更新日志\n\n- 改善平滑滚动\n- 修复设置保存",Url=RepositoryUrl+"/releases/tag/v2.2.0",Error="http-404" };
            var pending = CheckForUpdatesAsync(); await Task.Delay(250);
            var dialog = _updateDialog ?? throw new InvalidOperationException("Update dialog was not shown.");
            if (dialog.Tag is not UpdateStatus shown || shown.State != state) throw new InvalidOperationException("Update result did not reach the dialog.");
            if (state == "available" && (dialog.PrimaryButtonText != UiText.Get("前往发布页") || !dialog.IsPrimaryButtonEnabled || fake.Status.Update.NotificationPending)) throw new InvalidOperationException("Update release link/acknowledgement failed.");
            await CaptureAsync(FindVisual<FrameworkElement>(dialog,item => item.Name == "BackgroundElement") ?? dialog,Path.Combine(directory,"update-"+state+".png"));
            dialog.Hide(); await pending;
        }
        fake.NextUpdate = new() { State="available",CanInstall=true,Version="2.2.0",Title="v2.2.0",Notes="- Improve scrolling\n- Fix settings",Url=RepositoryUrl+"/releases/tag/v2.2.0" };
        var deliveryDialog = CheckForUpdatesAsync(); await Task.Delay(250);
        if (_updateDialog is null || _updateDialog.PrimaryButtonText != UiText.Get("立即更新")) throw new InvalidOperationException("Automatic installation action missing.");
        await CaptureAsync(FindVisual<FrameworkElement>(_updateDialog,item => item.Name == "BackgroundElement") ?? _updateDialog,Path.Combine(directory,"update-install-available.png"));
        var installButton = FindVisual<Button>(_updateDialog,item => item.Name == "PrimaryButton") ?? throw new InvalidOperationException("Install button missing.");
        var installPeer = new Microsoft.UI.Xaml.Automation.Peers.ButtonAutomationPeer(installButton);
        ((Microsoft.UI.Xaml.Automation.Provider.IInvokeProvider)installPeer.GetPattern(Microsoft.UI.Xaml.Automation.Peers.PatternInterface.Invoke)).Invoke(); await Task.Delay(400);
        if (_updateProgress is null || Math.Abs(_updateProgress.Value-37)>0.1 || _updateDialog.CloseButtonText != UiText.Get("取消更新")) throw new InvalidOperationException("Download progress or cancellation UI missing.");
        await CaptureAsync(FindVisual<FrameworkElement>(_updateDialog,item => item.Name == "BackgroundElement") ?? _updateDialog,Path.Combine(directory,"update-downloading.png"));
        await fake.CancelUpdateAsync(); await Task.Delay(300);
        if (_updateDialog.Title.ToString() != UiText.Get("更新已取消")) throw new InvalidOperationException("Cancelled update UI missing.");
        _updateDialog.Hide(); await deliveryDialog;
        var notified = new UpdateStatus { State="available",CheckId=100,CompletedId=100,NotificationPending=true,Version="2.3.0",Title="v2.3.0",Notes="Automatic update reminder",Url=RepositoryUrl+"/releases/tag/v2.3.0" };
        fake.SetFullscreen(true); fake.AnnounceUpdate(notified); await Task.Delay(250);
        if (_updateDialog is not null || !fake.Status.Update.NotificationPending) throw new InvalidOperationException("Fullscreen consumed or displayed the automatic update reminder.");
        fake.SetFullscreen(false); await Task.Delay(250);
        if (_updateDialog is null || fake.Status.Update.NotificationPending) throw new InvalidOperationException("Automatic update reminder was not shown and acknowledged.");
        await CaptureAsync(FindVisual<FrameworkElement>(_updateDialog,item => item.Name == "BackgroundElement") ?? _updateDialog,Path.Combine(directory,"update-automatic.png"));
        var closingReminder = _updateDialog; closingReminder.Hide();
        var closeDeadline = DateTime.UtcNow.AddSeconds(3);
        while (ReferenceEquals(_updateDialog,closingReminder) && DateTime.UtcNow < closeDeadline) await Task.Delay(25);
        if (_updateDialog is not null) throw new InvalidOperationException("Automatic reminder failed to close or reopened after acknowledgement.");
        fake.NextUpdate = new() { State="current",Version=CurrentProductVersion };
    }
}
