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
    private async Task CheckForUpdatesAsync() => await ShowUpdateAsync(null);
    private void TryShowUpdateReminder() {
        if (!Model.Connected || _closing || _updateDialog is not null || _bindingDialog is not null || Root.XamlRoot is null || !Model.Status.Update.NotificationPending) return;
        if (VisualTreeHelper.GetOpenPopupsForXamlRoot(Root.XamlRoot).Any(p => p.IsOpen)) return;
        _ = ShowUpdateAsync(Model.Status.Update);
    }
    private async Task ShowUpdateAsync(UpdateStatus? automatic) {
        if (_updateDialog is not null || _closing) return;
        var dialog = _updateDialog = new ContentDialog { XamlRoot = Root.XamlRoot, Title = UiText.Get("检查更新"), CloseButtonText = UiText.Get("取消"), DefaultButton = ContentDialogButton.Close };
        bool closed = false; dialog.Closed += (_,_) => closed = true;
        dialog.Content = Stack(new ProgressRing { IsActive = true, Width = 28, Height = 28 },Text(UiText.Get("正在检查 GitHub Releases…")));
        dialog.PrimaryButtonClick += async (_,e) => {
            if (dialog.Tag is not UpdateStatus result || !ValidReleaseUrl(result.Url)) return;
            var deferral = e.GetDeferral();
            try { e.Cancel = !await Launcher.LaunchUriAsync(new Uri(result.Url)); }
            catch { e.Cancel = true; }
            finally { deferral.Complete(); }
        };
        dialog.Opened += async (_,_) => {
            try {
                var result = automatic;
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
        dialog.Tag = update; dialog.CloseButtonText = UiText.Get("知道了");
        if (update.State == "available") {
            dialog.Title = UiText.Format("发现新版本 v{0}",update.Version);
            var notes = Text(string.IsNullOrWhiteSpace(update.Notes) ? UiText.Get("此版本未提供更新日志。") : update.Notes); notes.IsTextSelectionEnabled = true;
            var content = Stack(Secondary(UiText.Format("当前版本：v{0}",CurrentProductVersion)));
            if (update.Title.Trim() != "v"+update.Version && update.Title.Trim() != update.Version) content.Children.Add(Text(update.Title));
            content.Children.Add(new ScrollViewer { Content = notes, MaxHeight = 340, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, MaxWidth = 560 }); dialog.Content = content;
            dialog.PrimaryButtonText = UiText.Get("前往发布页"); dialog.IsPrimaryButtonEnabled = ValidReleaseUrl(update.Url); dialog.CloseButtonText = UiText.Get("稍后");
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
    private async Task RunUpdateSmokeAsync(string directory) {
        var fake = (Services.FakeHostSession)_session;
        foreach (var state in new[]{"available","current","no-releases","error"}) {
            fake.NextUpdate = new() { State=state,Version="2.1.0",Title="v2.1.0",Notes="# 更新日志\n\n- 改善平滑滚动\n- 修复设置保存",Url=RepositoryUrl+"/releases/tag/v2.1.0",Error="http-404" };
            var pending = CheckForUpdatesAsync(); await Task.Delay(250);
            var dialog = _updateDialog ?? throw new InvalidOperationException("Update dialog was not shown.");
            if (dialog.Tag is not UpdateStatus shown || shown.State != state) throw new InvalidOperationException("Update result did not reach the dialog.");
            if (state == "available" && (dialog.PrimaryButtonText != UiText.Get("前往发布页") || !dialog.IsPrimaryButtonEnabled || fake.Status.Update.NotificationPending)) throw new InvalidOperationException("Update release link/acknowledgement failed.");
            await CaptureAsync(FindVisual<FrameworkElement>(dialog,item => item.Name == "BackgroundElement") ?? dialog,Path.Combine(directory,"update-"+state+".png"));
            dialog.Hide(); await pending;
        }
        var notified = new UpdateStatus { State="available",CheckId=100,CompletedId=100,NotificationPending=true,Version="2.2.0",Title="v2.2.0",Notes="Automatic update reminder",Url=RepositoryUrl+"/releases/tag/v2.2.0" };
        fake.AnnounceUpdate(notified); await Task.Delay(250);
        if (_updateDialog is null || fake.Status.Update.NotificationPending) throw new InvalidOperationException("Automatic update reminder was not shown and acknowledged.");
        await CaptureAsync(FindVisual<FrameworkElement>(_updateDialog,item => item.Name == "BackgroundElement") ?? _updateDialog,Path.Combine(directory,"update-automatic.png"));
        var closingReminder = _updateDialog; closingReminder.Hide();
        var closeDeadline = DateTime.UtcNow.AddSeconds(3);
        while (ReferenceEquals(_updateDialog,closingReminder) && DateTime.UtcNow < closeDeadline) await Task.Delay(25);
        if (_updateDialog is not null) throw new InvalidOperationException("Automatic reminder failed to close or reopened after acknowledgement.");
        fake.NextUpdate = new() { State="current",Version="2.0.0" };
    }
}
