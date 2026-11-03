using WinMouseFix.Settings.Resources;
using System;
using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.Versioning;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using WinMouseFix.Configuration;

namespace WinMouseFix.Settings.Services;

public sealed class HostRequestException(HostReply reply) : Exception(reply.Message)
{
    public HostReply Reply { get; } = reply;
}
public sealed class HostRestartedException() : IOException(UiText.Get("后台已重启，请重新读取设置后再修改。"));

[SupportedOSPlatform("windows")]
public sealed class HostClient : IHostSession
{
    private readonly SettingsEndpoint _endpoint;
    private readonly SemaphoreSlim _commandGate = new(1,1);
    private readonly CancellationTokenSource _lifetime = new();
    private NamedPipeClientStream? _command;
    private string _commandRun = "", _runId = "";
    private bool _launchAttempted;
    private volatile bool _closing, _recording;
    private Task? _subscription, _heartbeat;
    private string _window = "";
    private long _statusVersion;
    private readonly object _statusGate = new();
    private long _connectionSerial, _acceptedConnectionSerial;
    private int _disposed;
    public string HostRunId => _runId;
    public uint HostPid { get; private set; }
    public bool Closing => _closing;
    public event Action<HostStatus>? StatusChanged;
    public event Action<string>? Disconnected;
    public event Action? Restarted;
    public HostClient(SettingsEndpoint endpoint) { _endpoint = endpoint; }
    public async Task<HostReply> ConnectAsync(nint window, CancellationToken cancellation = default)
    {
        _window = window == 0 ? "" : ((ulong)window).ToString(System.Globalization.CultureInfo.InvariantCulture);
        var reply = await SendAsync("get-config", new { }, allowInitialLaunch: true, cancellation: cancellation).ConfigureAwait(false);
        _subscription ??= SubscribeAsync(); _heartbeat ??= HeartbeatAsync(); return reply;
    }
    public Task<HostReply> GetConfigurationAsync(CancellationToken cancellation = default) => SendAsync("get-config", new { }, cancellation: cancellation);
    public Task<HostReply> UpdateAsync(ProductConfiguration configuration, ulong expected, CancellationToken cancellation = default) =>
        SendAsync("update-config", new { config = configuration }, expected, cancellation: cancellation);
    public Task<HostReply> ImportAsync(ProductConfiguration configuration, ulong expected, CancellationToken cancellation = default) =>
        SendAsync("import-config", new { config = configuration }, expected, cancellation: cancellation);
    public Task<HostReply> ResetAsync(ulong expected, CancellationToken cancellation = default) =>
        SendAsync("reset-defaults", new { confirmed = true }, expected, cancellation: cancellation);
    public Task<HostReply> SetEnabledAsync(bool enabled, CancellationToken cancellation = default) =>
        SendAsync("set-enabled", new { enabled }, emergency: true, cancellation: cancellation);
    public Task<HostReply> SetStartupAsync(bool enabled, ulong expected, CancellationToken cancellation = default) => SendAsync("set-startup",new { enabled },expected,cancellation: cancellation);
    public Task<HostReply> StartTraceAsync(CancellationToken cancellation = default) => SendAsync("start-trace",new { },cancellation: cancellation);
    public Task<HostReply> CheckUpdatesAsync(CancellationToken cancellation = default) => SendAsync("check-updates",new { },cancellation: cancellation);
    public Task<HostReply> AcknowledgeUpdateAsync(ulong checkId,CancellationToken cancellation = default) => SendAsync("acknowledge-update",new { checkId },cancellation: cancellation);
    public Task<HostReply> RecordShellAsync(object observation,ulong expected,CancellationToken cancellation = default) => SendAsync("record-shell-observation",observation,expected,cancellation: cancellation);
    public async Task<HostReply> RecordAsync(bool enabled, CancellationToken cancellation = default)
    {
        var reply = await SendAsync(enabled ? "record-start" : "record-stop", new { }, cancellation: cancellation).ConfigureAwait(false);
        if (reply.Ok) _recording = enabled; return reply;
    }
    public Task<HostReply> ExportDiagnosticsAsync(CancellationToken cancellation = default) => SendAsync("export-diagnostics", new { }, cancellation: cancellation);
    public async Task ExitAsync(CancellationToken cancellation = default)
    {
        var reply = await SendAsync("exit", new { }, emergency: true, cancellation: cancellation).ConfigureAwait(false);
        if (!reply.Ok) throw new HostRequestException(reply);
        _closing = true;
        try { await SendAsync("exit-ack", new { }, emergency: true, cancellation: cancellation).ConfigureAwait(false); }
        catch (IOException) { } // The first acknowledged response already confirmed the explicit exit.
    }
    private async Task<HostReply> SendAsync(string command, object payload, ulong expected = 0, bool emergency = false,
        bool allowInitialLaunch = false, CancellationToken cancellation = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        if (_closing && command is not ("exit-ack" or "get-state" or "subscribe")) throw new IOException(UiText.Get("后台正在退出。"));
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellation, _lifetime.Token);
        linked.CancelAfter(TimeSpan.FromSeconds(3));
        if (emergency)
        {
            var channel = await OpenAsync(false, false, linked.Token).ConfigureAwait(false);
            await using var pipe = channel.Pipe;
            var request = Request(command, payload, expected, channel.Run);
            return await ExchangeAsync(pipe, request, linked.Token).ConfigureAwait(false);
        }
        await _commandGate.WaitAsync(linked.Token).ConfigureAwait(false);
        try
        {
            if (_command is null || !_command.IsConnected)
            {
                _command?.Dispose(); var opened = await OpenAsync(allowInitialLaunch, true, linked.Token).ConfigureAwait(false);
                _command = opened.Pipe; _commandRun = opened.Run;
            }
            var request = Request(command, payload, expected, _commandRun);
            try { return await ExchangeAsync(_command, request, linked.Token).ConfigureAwait(false); }
            catch (IOException) when (!linked.IsCancellationRequested && !_closing)
            {
                _command.Dispose(); _command = null;
                var opened = await OpenAsync(false, true, linked.Token).ConfigureAwait(false); _command = opened.Pipe; _commandRun = opened.Run;
                if (opened.Run != request.HostRunId) throw new HostRestartedException();
                return await ExchangeAsync(_command, request, linked.Token).ConfigureAwait(false); // Same request ID, same bytes and expected revision.
            }
        }
        catch (OperationCanceledException) { _command?.Dispose(); _command = null; throw; }
        finally { _commandGate.Release(); }
    }
    private HostRequest Request(string command, object payload, ulong expected, string run) => new()
    {
        Command = command, HostRunId = run, ExpectedRevision = expected,
        Payload = JsonSerializer.SerializeToElement(payload, ConfigurationJson.Options)
    };
    private async Task<(NamedPipeClientStream Pipe,string Run)> OpenAsync(bool allowLaunch, bool window, CancellationToken cancellation)
    {
        long serial = Interlocked.Increment(ref _connectionSerial);
        var pipe = new NamedPipeClientStream(".", _endpoint.PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
        try
        {
            try { await pipe.ConnectAsync(allowLaunch ? 150 : 1000, cancellation).ConfigureAwait(false); }
            catch (TimeoutException) when (allowLaunch && !_launchAttempted && !_closing && !_endpoint.Simulation)
            {
                _launchAttempted = true;
                if (!File.Exists(_endpoint.HostExecutable)) throw new FileNotFoundException(UiText.Get("找不到与设置窗口匹配的 WinMouseFix.Host.exe。"), _endpoint.HostExecutable);
                using var process = Process.Start(new ProcessStartInfo(_endpoint.HostExecutable,"--startup")
                {
                    UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = Path.GetDirectoryName(_endpoint.HostExecutable)!
                }) ?? throw new IOException(UiText.Get("无法启动后台。"));
                await pipe.ConnectAsync(2000, cancellation).ConfigureAwait(false);
            }
            uint pid = _endpoint.VerifyServer(pipe);
            var hello = Request("hello", window && !string.IsNullOrEmpty(_window) ? new { window = _window } : new { }, 0, "");
            var reply = await ExchangeAsync(pipe, hello, cancellation).ConfigureAwait(false);
            if (!reply.Ok) throw new HostRequestException(reply);
            if (!reply.Payload.TryGetProperty("hostPid", out var serverPid) || serverPid.GetUInt32() != pid ||
                reply.Payload.GetProperty("scope").GetString() != _endpoint.Scope || reply.Payload.GetProperty("simulation").GetBoolean() != _endpoint.Simulation)
                throw new IOException(UiText.Get("后台身份或运行模式不匹配。"));
            string previous;
            lock (_statusGate) {
                previous = _runId;
                if (previous != reply.HostRunId && serial < _acceptedConnectionSerial) throw new HostRestartedException();
                _acceptedConnectionSerial = Math.Max(_acceptedConnectionSerial, serial);
                _runId = reply.HostRunId; HostPid = pid;
                if (previous != reply.HostRunId) _statusVersion = 0;
            }
            if (previous != reply.HostRunId) {
                if (previous.Length != 0) Restarted?.Invoke();
            }
            ApplyStatus(reply);
            return (pipe, reply.HostRunId);
        }
        catch { await pipe.DisposeAsync().ConfigureAwait(false); throw; }
    }
    private async Task<HostReply> ExchangeAsync(NamedPipeClientStream pipe, HostRequest request, CancellationToken cancellation)
    {
        var data = JsonSerializer.SerializeToUtf8Bytes(request, ConfigurationJson.Options);
        if (data.Length is 0 or > ConfigurationJson.MaximumBytes) throw new IOException(UiText.Get("请求超过大小限制。"));
        var header = new byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(header, (uint)data.Length);
        await pipe.WriteAsync(header, cancellation).ConfigureAwait(false); await pipe.WriteAsync(data, cancellation).ConfigureAwait(false);
        await pipe.ReadExactlyAsync(header, cancellation).ConfigureAwait(false);
        uint length = BinaryPrimitives.ReadUInt32LittleEndian(header);
        if (length is 0 or > ConfigurationJson.MaximumBytes) throw new IOException(UiText.Get("后台响应超过大小限制。"));
        var bytes = new byte[(int)length]; await pipe.ReadExactlyAsync(bytes, cancellation).ConfigureAwait(false);
        HostReply reply;
        try {
            string text = new UTF8Encoding(false, true).GetString(bytes);
            reply = JsonSerializer.Deserialize<HostReply>(text, ConfigurationJson.Options) ?? throw new IOException(UiText.Get("后台响应为空。"));
        } catch (Exception error) when (error is JsonException or DecoderFallbackException) { throw new IOException(UiText.Get("后台响应格式无效。"), error); }
        if (reply.ProtocolVersion != 1 || reply.RequestId != request.RequestId || string.IsNullOrEmpty(reply.HostRunId))
            throw new IOException(UiText.Get("后台协议版本或请求标识不匹配。"));
        if (request.Command != "hello" && reply.HostRunId != request.HostRunId) throw new HostRestartedException();
        ApplyStatus(reply);
        return reply;
    }
    private void ApplyStatus(HostReply reply)
    {
        lock (_statusGate) {
        if (reply.HostRunId != _runId) return;
        if (reply.Payload.TryGetProperty("status", out var element))
        {
            var status = element.Deserialize<HostStatus>(ConfigurationJson.Options) ?? throw new IOException(UiText.Get("后台状态无效。"));
            if (status.Version == 0 || status.Version > long.MaxValue) throw new IOException(UiText.Get("后台状态序号无效。"));
            for (;;) {
                long previous = Interlocked.Read(ref _statusVersion);
                if ((long)status.Version <= previous) return;
                if (Interlocked.CompareExchange(ref _statusVersion, (long)status.Version, previous) == previous) break;
            }
            if (status.ExitRequested) _closing = true;
            if (!status.Recording) _recording = false;
            StatusChanged?.Invoke(status);
        }
        }
    }
    private async Task SubscribeAsync()
    {
        while (!_lifetime.IsCancellationRequested)
        {
            try
            {
                using var connect = CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token); connect.CancelAfter(3000);
                var channel = await OpenAsync(false, false, connect.Token).ConfigureAwait(false);
                await using var pipe = channel.Pipe;
                while (!_lifetime.IsCancellationRequested)
                {
                    using var timeout = CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token); timeout.CancelAfter(3000);
                    var reply = await ExchangeAsync(pipe, Request("subscribe", new { }, 0, channel.Run), timeout.Token).ConfigureAwait(false);
                    if (!reply.Ok) throw new HostRequestException(reply);
                }
            }
            catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { return; }
            catch (Exception error) when (error is IOException or TimeoutException or OperationCanceledException or JsonException or HostRequestException)
            {
                _recording = false; Disconnected?.Invoke(_closing ? UiText.Get("后台已退出。") : UiText.Get("与后台的连接已断开，正在尝试重新连接。"));
                if (_closing) return;
                try { await Task.Delay(1000, _lifetime.Token).ConfigureAwait(false); } catch (OperationCanceledException) { return; }
            }
        }
    }
    private async Task HeartbeatAsync()
    {
        while (!_lifetime.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(1000, _lifetime.Token).ConfigureAwait(false);
                if (_recording) await SendAsync("get-state", new { }).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { return; }
            catch (Exception error) when (error is IOException or TimeoutException or OperationCanceledException or HostRequestException) { _recording = false; }
        }
    }
    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        await _lifetime.CancelAsync().ConfigureAwait(false);
        _command?.Dispose();
        if (_subscription is not null) { try { await _subscription.ConfigureAwait(false); } catch (OperationCanceledException) {} }
        if (_heartbeat is not null) { try { await _heartbeat.ConfigureAwait(false); } catch (OperationCanceledException) {} }
        _lifetime.Dispose();
    }
}
