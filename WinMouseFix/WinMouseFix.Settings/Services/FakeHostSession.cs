using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using WinMouseFix.Configuration;
namespace WinMouseFix.Settings.Services;

// Explicit UI/model smoke backend. It has no native DLL, process launch or input API.
public sealed class FakeHostSession : IHostSession
{
    private ProductConfiguration _config = new();
    public HostStatus Status { get; } = new() { State = "paused", ConfigRevision = 1, Capabilities = [
        new() { Name = "content-drag", State = "unverified" },
        new() { Name = "task-view", State = "unverified", Continuous = "limited" },
        new() { Name = "show-desktop", State = "unverified" },
        new() { Name = "desktop-left", State = "unverified" },
        new() { Name = "desktop-right", State = "unverified" },
        new() { Name = "wheel", State = "unverified" },
        new() { Name = "zoom", State = "unverified" }
    ] };
    public bool RejectNext { get; set; }
    public Task<HostReply> SetDataLocationAsync(string location,ulong expected,CancellationToken cancellation = default) {
        if (RejectNext) { RejectNext = false; return Task.FromResult(Reply(false,"storage-error","storage","模拟迁移失败")); }
        if (expected != _config.Revision) return Task.FromResult(Reply(false,"revision-conflict"));
        if (location is not ("appdata" or "program")) return Task.FromResult(Reply(false,"bad-request"));
        Status.Storage.Location = location; Status.Storage.NativeError = 0; return Task.FromResult(Reply());
    }
    public UpdateStatus NextUpdate { get; set; } = new() { State = "current", Version = "2.1.0" };
    public Task<HostReply> ReadyAsync(CancellationToken cancellation = default) => Task.FromResult(Reply());
    public Task<HostReply> CheckUpdatesAsync(CancellationToken cancellation = default) {
        var id = Status.Update.CheckId+1;
        Status.Update = JsonSerializer.Deserialize<UpdateStatus>(JsonSerializer.Serialize(NextUpdate,ConfigurationJson.Options),ConfigurationJson.Options)!;
        Status.Update.CheckId = Status.Update.CompletedId = id; return Task.FromResult(Reply());
    }
    public Task<HostReply> DownloadUpdateAsync(ulong checkId,CancellationToken cancellation = default) {
        if (checkId != Status.Update.CompletedId || !Status.Update.CanInstall) return Task.FromResult(Reply(false,"not-ready"));
        Status.Update.State="downloading"; Status.Update.NotificationPending=false; Status.Update.TotalBytes=100*1024*1024; Status.Update.ReceivedBytes=37*1024*1024; Status.Update.BytesPerSecond=2*1024*1024; return Task.FromResult(Reply());
    }
    public Task<HostReply> CancelUpdateAsync(CancellationToken cancellation = default) { Status.Update.State="cancelled"; return Task.FromResult(Reply()); }
    public Task<HostReply> InstallUpdateAsync(CancellationToken cancellation = default) { if (Status.Update.State != "ready") return Task.FromResult(Reply(false,"not-ready")); Status.Update.State="installing"; return Task.FromResult(Reply()); }
    public Task<HostReply> AcknowledgeUpdateAsync(ulong checkId,CancellationToken cancellation = default) {
        if (checkId == Status.Update.CompletedId) Status.Update.NotificationPending = false; return Task.FromResult(Reply());
    }
    public bool ConflictNext { get; set; }
    public bool Offline { get; set; }
    public uint Updates { get; private set; }
    public bool Disposed { get; private set; }
    public event Action<HostStatus>? StatusChanged;
    public event Action<string>? Disconnected;
    public event Action? Restarted;
    private void Emit() { Status.Version++; Status.ConfigRevision = _config.Revision; StatusChanged?.Invoke(JsonSerializer.Deserialize<HostStatus>(JsonSerializer.Serialize(Status, ConfigurationJson.Options), ConfigurationJson.Options)!); }
    private HostReply Reply(bool ok = true, string code = "ok", string field = "", string message = "") {
        if (Offline) throw new System.IO.IOException("模拟连接已断开。");
        Emit(); return new() { ProtocolVersion = 1, RequestId = "simulation", HostRunId = "ui-simulation", Ok = ok, Code = code,
            Field = field, Message = message, Revision = _config.Revision, ActiveRevision = Status.ActiveRevision,
            Payload = JsonSerializer.SerializeToElement(new { config = _config, status = Status }, ConfigurationJson.Options) };
    }
    public Task<HostReply> ConnectAsync(nint window, CancellationToken cancellation = default) => Task.FromResult(Reply());
    public Task<HostReply> GetConfigurationAsync(CancellationToken cancellation = default) => Task.FromResult(Reply());
    public Task<HostReply> UpdateAsync(ProductConfiguration config, ulong expected, CancellationToken cancellation = default) {
        if (ConflictNext) { ConflictNext = false; _config.Revision++; }
        if (expected != _config.Revision) return Task.FromResult(Reply(false,"revision-conflict","revision","配置已更新。"));
        if (RejectNext) { RejectNext = false; return Task.FromResult(Reply(false,"configuration-rejected","content.thresholdCounts","模拟拒绝，不改变后台配置。")); }
        var issue = ConfigurationJson.Validate(config);
        if (issue is not null) return Task.FromResult(Reply(false,"configuration-rejected",issue.Field,issue.Message));
        _config = config.Clone(); _config.Revision = expected+1; Updates++; return Task.FromResult(Reply());
    }
    public Task<HostReply> ImportAsync(ProductConfiguration config, ulong expected, CancellationToken cancellation = default) => UpdateAsync(config,expected,cancellation);
    public Task<HostReply> ResetAsync(ulong expected, CancellationToken cancellation = default) => UpdateAsync(new(),expected,cancellation);
    public Task<HostReply> SetEnabledAsync(bool enabled, CancellationToken cancellation = default) {
        _config.Enabled = enabled; _config.Revision++; Status.Requested = enabled; Status.State = enabled ? "enabled" : "paused"; return Task.FromResult(Reply());
    }
    public Task<HostReply> RecordAsync(bool enabled, CancellationToken cancellation = default) {
        Status.Recording = enabled; Status.RecordingRemainingMilliseconds = enabled ? 30000u : 0; Status.RecordedButton = null; return Task.FromResult(Reply());
    }
    public Task<HostReply> SetStartupAsync(bool enabled, ulong expected, CancellationToken cancellation = default) { _config.Startup = enabled; _config.Revision++; Status.StartupRegistered = enabled; return Task.FromResult(Reply()); }
    public Task<HostReply> StartTraceAsync(CancellationToken cancellation = default) => Task.FromResult(Reply());
    public Task<HostReply> RecordShellAsync(object observation,ulong expected,CancellationToken cancellation = default) {
        if (expected != _config.Revision) return Task.FromResult(Reply(false,"revision-conflict"));
        var item = JsonSerializer.SerializeToElement(observation); var name = item.GetProperty("action").GetString();
        var capability = Status.Capabilities.Find(c => c.Name == name)!;
        capability.Trigger = item.GetProperty("trigger").GetString()!; capability.Continuous = item.GetProperty("continuous").GetString()!;
        capability.Hold = item.GetProperty("hold").GetString()!; capability.Reverse = item.GetProperty("reverse").GetString()!; capability.Release = item.GetProperty("release").GetString()!;
        capability.State = capability.Trigger == "pass" ? "supported" : "unverified"; return Task.FromResult(Reply());
    }
    public void Identify(string button, uint modifiers = 0) { if (!Status.Recording) throw new InvalidOperationException("No identification lease"); Status.RecordedButton = new() { Button = button, Modifiers = modifiers, Serial = Status.Version+1 }; Emit(); }
    public void Disconnect() { Offline = true; Status.Recording = false; Disconnected?.Invoke("模拟断线，后台继续运行。"); }
    public void Restart() { Status.Version = 0; Offline = false; Restarted?.Invoke(); Emit(); }
    public void AnnounceUpdate(UpdateStatus update) { Status.Update = update; Emit(); }
    public Task<HostReply> ExportDiagnosticsAsync(CancellationToken cancellation = default) => Task.FromResult(Reply(false,"simulation","","模拟模式不导出真实日志。"));
    public Task ExitAsync(CancellationToken cancellation = default) { Status.ExitRequested = true; Status.State = "stopped"; Emit(); return Task.CompletedTask; }
    public ValueTask DisposeAsync() { Disposed = true; Status.Recording = false; return ValueTask.CompletedTask; }
}
