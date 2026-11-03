using System;
using System.ComponentModel;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using WinMouseFix.Settings.Resources;
using WinMouseFix.Configuration;
using WinMouseFix.Settings.Services;
namespace WinMouseFix.Settings.ViewModels;

public sealed class SettingsModel : INotifyPropertyChanged, IAsyncDisposable
{
    private readonly IHostSession _client;
    private readonly Action<Action> _post;
    private readonly SemaphoreSlim _saveGate = new(1,1);
    private CancellationTokenSource? _debounce;
    private long _editVersion, _savedVersion;
    private long _rejectedVersion = -1;
    private bool _conflicted, _disposed;
    private bool _hasConnected, _reloadPending, _hostChanged;
    public ProductConfiguration Committed { get; private set; } = new();
    public ProductConfiguration Draft { get; private set; } = new();
    public HostStatus Status { get; private set; } = new();
    public bool Connected { get; private set; }
    public bool Saving { get; private set; }
    public string Message { get; private set; } = UiText.Get("正在连接后台…");
    public ConfigurationIssue? Issue { get; private set; }
    public event PropertyChangedEventHandler? PropertyChanged;
    public event Action? ConfigurationChanged;
    public SettingsModel(IHostSession client, Action<Action> post)
    {
        _client = client; _post = post;
        client.StatusChanged += status => _post(() => {
            if (_disposed) return;
            if (status.Version <= Status.Version && Status.Version != 0) return;
            Status = status; Changed();
            if (!Connected && _hasConnected && _editVersion == _savedVersion && !_reloadPending) _ = ReconnectedAsync();
        });
        client.Disconnected += message => _post(() => { if (_disposed) return; Connected = false; Message = message; Changed(); });
        client.Restarted += () => _post(() => { if (_disposed) return; Status = new(); Connected = false; _conflicted = true; _hostChanged = true; Message = UiText.Get("后台已重启，请重新读取设置。"); Changed(); });
    }
    public async Task ConnectAsync(nint window)
    {
        try { Accept(await _client.ConnectAsync(window), true); }
        catch (Exception e) { Offline(e); }
    }
    public async Task ReloadAsync()
    {
        _debounce?.Cancel();
        try { Accept(await _client.GetConfigurationAsync(), true); _conflicted = false; }
        catch (Exception e) { Offline(e); }
    }
    private async Task ReconnectedAsync() { _reloadPending = true; try { await ReloadAsync(); } finally { _reloadPending = false; } }
    public void Edit(Action<ProductConfiguration> edit)
    {
        if (!Connected || _disposed) return;
        edit(Draft); ++_editVersion; Issue = ConfigurationJson.Validate(Draft);
        if (Issue is not null) { Message = Issue.Message; Changed(); return; }
        if (_conflicted) { Message = UiText.Get("草稿尚未保存，请先重新读取当前配置。"); Changed(); return; }
        Message = UiText.Get("更改待保存…"); Changed();
        _debounce?.Cancel(); _debounce?.Dispose(); _debounce = new();
        _ = DebouncedAsync(_debounce.Token);
    }
    private async Task DebouncedAsync(CancellationToken token)
    {
        try { await Task.Delay(150, token); await FlushAsync(token); }
        catch (OperationCanceledException) { }
    }
    public async Task<bool> FlushAsync(CancellationToken cancellation = default)
    {
        if (_disposed || !Connected || _conflicted || _editVersion == _rejectedVersion || ConfigurationJson.Validate(Draft) is not null) return false;
        await _saveGate.WaitAsync(cancellation);
        try
        {
            if (_savedVersion == _editVersion) return true;
            long version = _editVersion; var snapshot = Draft.Clone(); snapshot.Revision = Committed.Revision;
            Saving = true; Message = UiText.Get("正在保存…"); Changed();
            var response = await _client.UpdateAsync(snapshot, Committed.Revision);
            if (!response.Ok) { _rejectedVersion = version; Reject(response); return false; }
            Accept(response, false); _savedVersion = version;
            if (version == _editVersion) { Draft = Committed.Clone(); ConfigurationChanged?.Invoke(); }
            Message = Status.ActiveRevision != 0 && Status.ActiveRevision != Committed.Revision ? UiText.Get("已保存，当前手势结束后生效。") : UiText.Get("已保存。");
            if (response.Code == "saved-with-startup-error") Message = UiText.Get("设置已保存；登录启动登记失败，请重新切换该选项。");
            return true;
        }
        catch (Exception e) { Offline(e); return false; }
        finally { Saving = false; _saveGate.Release(); Changed(); }
    }
    public async Task<bool> SetNativeZoomAsync(string button)
    {
        if (!Connected || button is not ("back" or "forward" or "middle") || !Status.Capabilities.Any(c => c.Name == "zoom" && c.State == "supported")) return false;
        bool Combo(ButtonBinding b) => b.Button == button && b.Modifiers == 0 && b.Trigger is "wheel" or "wheel-up" or "wheel-down";
        if (Draft.Bindings.Count(Combo) == 1 && Draft.Bindings.Any(b => Combo(b) && b.Trigger == "wheel" && b.Action == "native-zoom")) return true;
        Edit(c => {
            c.Bindings.RemoveAll(b => Combo(b));
            uint id = 1; while (c.Bindings.Any(b => b.Id == id)) ++id;
            c.Bindings.Add(new() { Id = id, Button = button, Trigger = "wheel", Action = "native-zoom" });
        });
        return await FlushAsync();
    }
    public async Task<bool> SaveBindingAsync(ButtonBinding binding)
    {
        if (!Connected || !await FlushAsync()) return false;
        await _saveGate.WaitAsync();
        try {
            var candidate = Draft.Clone(); candidate.Bindings.RemoveAll(b => b.Id == binding.Id);
            candidate.Bindings.Add(JsonSerializer.Deserialize<ButtonBinding>(JsonSerializer.Serialize(binding,ConfigurationJson.Options),ConfigurationJson.Options)!);
            var issue = ConfigurationJson.Validate(candidate);
            if (issue is not null) { Issue = issue; Message = issue.Message; Changed(); return false; }
            Saving = true; Message = UiText.Get("正在保存…"); Changed();
            var reply = await _client.UpdateAsync(candidate,Committed.Revision);
            if (!reply.Ok) { Reject(reply); return false; }
            Accept(reply,true); Message = Status.ActiveRevision != 0 && Status.ActiveRevision != Committed.Revision ? UiText.Get("已保存，当前手势结束后生效。") : UiText.Get("已保存。"); return true;
        }
        catch(Exception error) { Offline(error); return false; }
        finally { Saving = false; _saveGate.Release(); Changed(); }
    }
    public async Task SetEnabledAsync(bool enabled)
    {
        try { var reply = await _client.SetEnabledAsync(enabled); if (reply.Ok) { Accept(reply, _editVersion == _savedVersion); Draft.Enabled = Committed.Enabled; } else Reject(reply); }
        catch (Exception e) { Offline(e); }
    }
    public async Task SetStartupAsync(bool enabled) {
        await _saveGate.WaitAsync();
        try { var reply = await _client.SetStartupAsync(enabled,Committed.Revision); if (reply.Ok) { Accept(reply,_editVersion == _savedVersion); Draft.Startup = Committed.Startup; if (reply.NativeError != 0) Message = UiText.Get("登录启动登记失败，请重试；后台输入不受影响。"); } else Reject(reply); }
        catch (Exception e) { Offline(e); } finally { _saveGate.Release(); Changed(); }
    }
    public async Task StartTraceAsync() { try { var reply = await _client.StartTraceAsync(); if (!reply.Ok) Reject(reply); else { Message = UiText.Get("详细诊断已开启，最多 60 秒并有容量上限。"); Changed(); } } catch (Exception e) { Offline(e); } }
    public async Task<bool> ReplaceAsync(ProductConfiguration config, bool defaults = false)
    {
        _debounce?.Cancel();
        try {
            var issue = ConfigurationJson.Validate(config); if (issue is not null) { Issue = issue; Message = issue.Message; Changed(); return false; }
            var reply = defaults ? await _client.ResetAsync(Committed.Revision) : await _client.ImportAsync(config, Committed.Revision);
            if (!reply.Ok) { Reject(reply); return false; }
            Accept(reply, true); return true;
        } catch (Exception e) { Offline(e); return false; }
    }
    public async Task RecordAsync(bool enabled)
    {
        try { var reply = await _client.RecordAsync(enabled); if (!reply.Ok) Reject(reply); }
        catch (Exception e) { Offline(e); }
    }
    public async Task<string?> ExportDiagnosticsAsync()
    {
        try { var reply = await _client.ExportDiagnosticsAsync(); if (!reply.Ok) { Reject(reply); return null; } return reply.Payload.GetProperty("path").GetString(); }
        catch (Exception e) { Offline(e); return null; }
    }
    public Task ExitHostAsync() => _client.ExitAsync();
    public async Task RecordShellAsync(object observation) {
        if (!await FlushAsync()) return;
        try { var reply = await _client.RecordShellAsync(observation,Committed.Revision); if (reply.Ok) Accept(reply,false); else Reject(reply); }
        catch (Exception error) { Offline(error); }
    }
    private void Accept(HostReply reply, bool replaceDraft)
    {
        if (!reply.Ok) { Reject(reply); return; }
        if (reply.Payload.TryGetProperty("config", out var config)) {
            var incoming = ConfigurationJson.Parse(config.GetRawText());
            if (_hostChanged || incoming.Revision >= Committed.Revision) Committed = incoming;
        }
        if (replaceDraft) { Draft = Committed.Clone(); _savedVersion = ++_editVersion; _conflicted = false; }
        Connected = true; _hasConnected = true; _hostChanged = false; Issue = null; Message = UiText.Get("已连接，设置会自动保存。");
        ConfigurationChanged?.Invoke(); Changed();
    }
    private void Reject(HostReply reply)
    {
        if (reply.Payload.TryGetProperty("config", out var config)) {
            var incoming = ConfigurationJson.Parse(config.GetRawText());
            if (_hostChanged || incoming.Revision >= Committed.Revision) Committed = incoming;
        }
        _conflicted = reply.Code == "revision-conflict";
        Issue = new(reply.Field, reply.Message, reply.BindingId, reply.ConflictingId);
        Message = _conflicted ? UiText.Get("配置已发生变化，草稿未覆盖后台。请重新读取后再修改。") : reply.Message; Changed();
    }
    private void Offline(Exception e) { Connected = false; Message = e is OperationCanceledException ? UiText.Get("后台请求超时，请重新读取确认结果。") : e.Message; Changed(); }
    private void Changed([CallerMemberName] string? property = null) { PropertyChanged?.Invoke(this, new(property)); PropertyChanged?.Invoke(this, new("State")); }
    public async ValueTask DisposeAsync() { if (_disposed) return; _disposed = true; _debounce?.Cancel(); _debounce?.Dispose(); await _client.DisposeAsync(); }
}
