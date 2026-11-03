using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.IO.Pipes;
using System.Runtime.Versioning;
using System.Text;
using System.Text.Json;
using WinMouseFix.Configuration;
using WinMouseFix.Settings.Services;
using WinMouseFix.Settings.ViewModels;

internal static class ClientChecks
{
    public static async Task<int> Models()
    {
        int checks = 0; void Check(bool value) { checks++; if (!value) throw new Exception($"ViewModel check {checks} failed"); }
        var fake = new FakeHostSession(); await using var model = new SettingsModel(fake, action => action());
        await model.ConnectAsync(0); Check(model.Connected && model.Committed.Revision == 1);
        model.Edit(c => c.Content.ThresholdCounts = 0); await Task.Delay(180); Check(model.Issue is not null && fake.Updates == 0);
        model.Edit(c => { c.Content.ThresholdCounts = 8; c.Content.MmPerCountX = 0.25; }); Check(await model.FlushAsync());
        Check(model.Committed.Content.MmPerCountX == 0.25 && fake.Updates == 1);
        fake.RejectNext = true; model.Edit(c => c.Content.ThresholdCounts = 9); Check(!await model.FlushAsync());
        Check(model.Committed.Content.ThresholdCounts == 8 && model.Draft.Content.ThresholdCounts == 9);
        await Task.Delay(180); Check(fake.Updates == 1 && model.Committed.Content.ThresholdCounts == 8);
        model.Edit(c => c.Content.ThresholdCounts = 10); Check(await model.FlushAsync());
        fake.ConflictNext = true; model.Edit(c => c.Content.MmPerCountX = 0.5); Check(!await model.FlushAsync());
        uint updates = fake.Updates; model.Edit(c => c.Content.MmPerCountX = 0.6); Check(!await model.FlushAsync() && fake.Updates == updates);
        await model.ReloadAsync(); Check(model.Draft.Content.MmPerCountX == 0.25);
        await model.RecordAsync(true); fake.Identify("forward",2); Check(model.Status.RecordedButton?.Button == "forward");
        await model.RecordAsync(false); Check(!fake.Status.Recording);
        await model.RecordShellAsync(new { action="task-view", trigger="pass", continuous="limited", hold="pass", reverse="pass", release="pass", notes="Fake managed observation" });
        Check(model.Status.Capabilities.Single(c => c.Name == "task-view").Continuous == "limited");
        await model.SetEnabledAsync(true); await model.DisposeAsync(); Check(fake.Status.Requested && fake.Disposed);
        var zoomFake = new FakeHostSession(); await using var zoomModel = new SettingsModel(zoomFake,action => action()); await zoomModel.ConnectAsync(0);
        Check(!await zoomModel.SetNativeZoomAsync("back") && zoomFake.Updates == 0);
        zoomFake.Status.Capabilities.Single(c => c.Name == "zoom").State = "supported"; await zoomModel.ReloadAsync();
        zoomModel.Edit(c => { c.Bindings.Add(new() {Id=5,Button="back",Trigger="wheel-up",Action="volume-up"}); c.Bindings.Add(new() {Id=6,Button="back",Trigger="wheel-down",Action="volume-down"}); });
        Check(await zoomModel.FlushAsync());
        zoomFake.RejectNext = true; Check(!await zoomModel.SetNativeZoomAsync("back"));
        Check(zoomModel.Committed.Bindings.Any(b => b.Action == "volume-up")); await zoomModel.ReloadAsync();
        Check(await zoomModel.SetNativeZoomAsync("back"));
        Check(zoomModel.Committed.Bindings.Count == 5 && zoomModel.Committed.Bindings.Count(b => b.Action == "native-zoom") == 1);
        Check(zoomModel.Committed.Bindings.Any(b => b.Button == "back" && b.Trigger == "drag") && zoomModel.Committed.Bindings.Any(b => b.Button == "forward" && b.Trigger == "click"));
        Check(zoomModel.Status.Capabilities.Single(c => c.Name == "wheel").State == "unverified");
        var zoomUpdates = zoomFake.Updates; Check(await zoomModel.SetNativeZoomAsync("back") && zoomFake.Updates == zoomUpdates);
        var bindingFake = new FakeHostSession(); await using var bindingModel = new SettingsModel(bindingFake,action => action()); await bindingModel.ConnectAsync(0);
        var originalBindings = ConfigurationJson.Serialize(bindingModel.Draft);
        Check(!await bindingModel.SaveBindingAsync(new() { Id=5,Button="back",Trigger="click",Action="back" }));
        Check(bindingFake.Updates == 0 && ConfigurationJson.Serialize(bindingModel.Draft) == originalBindings);
        var binding = new ButtonBinding { Id=5,Button="middle",Trigger="click",Modifiers=2,Action="shortcut",Keys=[16,75] };
        bindingFake.RejectNext = true; Check(!await bindingModel.SaveBindingAsync(binding));
        Check(ConfigurationJson.Serialize(bindingModel.Draft) == originalBindings && ConfigurationJson.Serialize(bindingModel.Committed) == originalBindings);
        Check(await bindingModel.FlushAsync() && bindingFake.Updates == 0);
        Check(await bindingModel.SaveBindingAsync(binding) && bindingFake.Updates == 1);
        binding.Keys.Clear(); Check(bindingModel.Committed.Bindings.Single(b => b.Id == 5).Keys.SequenceEqual(new uint[]{16,75}));
        var noticeFake = new FakeHostSession(); await using var noticeModel = new SettingsModel(noticeFake,action => action()); await noticeModel.ConnectAsync(0);
        Check(noticeModel.Committed.StartupNotification);
        noticeModel.Edit(c => c.StartupNotification = false); Check(await noticeModel.FlushAsync()); await noticeModel.ReloadAsync();
        Check(!noticeModel.Committed.StartupNotification && !noticeModel.Draft.StartupNotification);
        noticeFake.RejectNext = true; noticeModel.Edit(c => c.StartupNotification = true); Check(!await noticeModel.FlushAsync());
        Check(!noticeModel.Committed.StartupNotification); await noticeModel.ReloadAsync(); Check(!noticeModel.Draft.StartupNotification);
        noticeModel.Edit(c => { c.AutomaticUpdates = true; c.UpdateIntervalHours = 12; }); Check(await noticeModel.FlushAsync()); await noticeModel.ReloadAsync();
        Check(noticeModel.Committed.AutomaticUpdates && noticeModel.Committed.UpdateIntervalHours == 12);
        var updateReply = await noticeFake.CheckUpdatesAsync(); Check(updateReply.Ok && noticeFake.Status.Update.State == "current");
        noticeFake.NextUpdate = new() { State="available",Version="2.1.0",NotificationPending=true }; await noticeFake.CheckUpdatesAsync();
        Check(noticeFake.Status.Update.NotificationPending); await noticeFake.AcknowledgeUpdateAsync(noticeFake.Status.Update.CompletedId); Check(!noticeFake.Status.Update.NotificationPending);
        return checks;
    }
    [SupportedOSPlatform("windows")]
    public static async Task<int> Pipes(string output)
    {
        int checks = 0; void Check(bool value) { checks++; if (!value) throw new Exception($"Managed pipe check {checks} failed"); }
        string scope = "WinMouseFix.Client.simulation."+Guid.NewGuid().ToString("N");
        var endpoint = SettingsEndpoint.Isolated(scope,Environment.ProcessPath!,Path.Combine(output,"client-preferences"));
        await using var server = new Stub(scope);
        await using var client = new HostClient(endpoint);
        var initial = await client.ConnectAsync(0); Check(initial.Ok && client.HostPid == Environment.ProcessId);
        var config = ConfigurationJson.Parse(initial.Payload.GetProperty("config").GetRawText());
        server.DelayUpdate = true;
        config.Content.MmPerCountX = 0.2; var delayed = client.UpdateAsync(config,config.Revision);
        await server.SlowStarted.Task.WaitAsync(TimeSpan.FromSeconds(3));
        var before = System.Diagnostics.Stopwatch.StartNew(); var paused = await client.SetEnabledAsync(false);
        Check(paused.Ok && before.ElapsedMilliseconds < 400 && !delayed.IsCompleted);
        var saved = await delayed; Check(saved.Ok && !server.Enabled);
        config = ConfigurationJson.Parse(saved.Payload.GetProperty("config").GetRawText());
        server.DropAfterCommit = true; config.Content.MmPerCountY = 0.3;
        var retried = await client.UpdateAsync(config,config.Revision);
        Check(retried.Ok && server.Commits == 2 && server.Deduplicated == 1);
        server.RejectUpdate = true; config = ConfigurationJson.Parse(retried.Payload.GetProperty("config").GetRawText());
        Check(!(await client.UpdateAsync(config,config.Revision)).Ok); server.RejectUpdate = false;
        server.ChangeRun(); bool restarted = false;
        try { await client.UpdateAsync(config,config.Revision); } catch(HostRestartedException) { restarted = true; }
        Check(restarted && server.Commits == 2);
        var current = await client.GetConfigurationAsync(); Check(current.Ok && client.HostRunId == server.Run);
        Check((await client.RecordAsync(true)).Ok); Check(server.Recording);
        await client.RecordAsync(false); Check(!server.Recording);
        await client.ExitAsync(); Check(client.Closing && server.ExitAcknowledged);
        bool refused = false; try { await client.SetEnabledAsync(true); } catch(IOException) { refused = true; } Check(refused);
        Check(server.Commits == 2); return checks;
    }
    private sealed class Stub : IAsyncDisposable
    {
        private readonly string _scope;
        private readonly CancellationTokenSource _stop = new();
        private readonly ConcurrentDictionary<long,NamedPipeServerStream> _pipes = new();
        private readonly ConcurrentDictionary<string,HostReply> _cache = new();
        private readonly ConcurrentDictionary<string,string> _bytes = new();
        private readonly Task[] _listeners;
        private readonly object _state = new();
        private ProductConfiguration _config = new();
        private long _connection, _version, _leaseOwner;
        public string Run { get; private set; } = "managed-run-1";
        public bool Enabled { get; private set; } = true;
        public bool Recording { get; private set; }
        public bool ExitAcknowledged { get; private set; }
        public bool DelayUpdate,DropAfterCommit,RejectUpdate;
        public int Commits,Deduplicated;
        public TaskCompletionSource SlowStarted { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public Stub(string scope) { _scope = scope; _listeners = Enumerable.Range(0,4).Select(_ => Listen()).ToArray(); }
        public void ChangeRun() { lock(_state) { Run = "managed-run-2"; _version = 0; _cache.Clear(); _bytes.Clear(); } }
        private HostReply Reply(HostRequest request,bool ok = true,string code = "ok",bool configuration = true)
        {
            lock(_state) {
                var status = new HostStatus { Version = (ulong)++_version, State = ExitAcknowledged ? "stopped" : "paused", Requested = Enabled,
                    ConfigRevision = _config.Revision, Recording = Recording, ExitRequested = request.Command is "exit" or "exit-ack", RecordingRemainingMilliseconds = Recording ? 30000u : 0 };
                object payload = request.Command == "hello" ? new { status, scope = _scope, simulation = true, hostPid = (uint)Environment.ProcessId, productVersion = "test" } :
                    configuration ? new { status, config = _config } : new { status };
                return new() { ProtocolVersion = 1, RequestId = request.RequestId, HostRunId = Run, Ok = ok, Code = code, Revision = _config.Revision,
                    Payload = JsonSerializer.SerializeToElement(payload,ConfigurationJson.Options) };
            }
        }
        private async Task Listen()
        {
            while (!_stop.IsCancellationRequested) {
                await using var pipe = new NamedPipeServerStream(_scope+".Control",PipeDirection.InOut,4,PipeTransmissionMode.Byte,PipeOptions.Asynchronous|PipeOptions.CurrentUserOnly);
                long connection = Interlocked.Increment(ref _connection); _pipes[connection] = pipe;
                try {
                    await pipe.WaitForConnectionAsync(_stop.Token);
                    while (!_stop.IsCancellationRequested) {
                        byte[] header = new byte[4]; await pipe.ReadExactlyAsync(header,_stop.Token); uint length = BinaryPrimitives.ReadUInt32LittleEndian(header);
                        if (length is 0 or > 65536) throw new IOException();
                        byte[] data = new byte[(int)length]; await pipe.ReadExactlyAsync(data,_stop.Token);
                        var request = JsonSerializer.Deserialize<HostRequest>(data,ConfigurationJson.Options)!; string raw = Encoding.UTF8.GetString(data);
                        HostReply reply;
                        if (_cache.TryGetValue(request.RequestId,out var cached)) {
                            if (_bytes[request.RequestId] != raw) throw new IOException("A retried request changed bytes"); Interlocked.Increment(ref Deduplicated); reply = cached;
                        } else if (request.Command != "hello" && request.HostRunId != Run) reply = Reply(request,false,"host-restarted");
                        else {
                            if (request.Command == "update-config" && DelayUpdate) { DelayUpdate = false; SlowStarted.TrySetResult(); await Task.Delay(700,_stop.Token); }
                            if (request.Command == "subscribe") await Task.Delay(50,_stop.Token);
                            lock(_state) {
                                if (request.Command == "update-config") {
                                    if (RejectUpdate) reply = Reply(request,false,"configuration-rejected");
                                    else if (request.ExpectedRevision != _config.Revision) reply = Reply(request,false,"revision-conflict");
                                    else { var next = ConfigurationJson.Parse(request.Payload.GetProperty("config").GetRawText()); next.Revision = _config.Revision+1; _config = next; Commits++; reply = Reply(request); }
                                } else {
                                    if (request.Command == "set-enabled") Enabled = request.Payload.GetProperty("enabled").GetBoolean();
                                    if (request.Command == "record-start") { Recording = true; _leaseOwner = connection; }
                                    if (request.Command == "record-stop") Recording = false;
                                    if (request.Command == "exit-ack") ExitAcknowledged = true;
                                    reply = Reply(request);
                                }
                                _cache[request.RequestId] = reply; _bytes[request.RequestId] = raw;
                            }
                            if (request.Command == "update-config" && DropAfterCommit) { DropAfterCommit = false; break; }
                        }
                        data = JsonSerializer.SerializeToUtf8Bytes(reply,ConfigurationJson.Options); BinaryPrimitives.WriteUInt32LittleEndian(header,(uint)data.Length);
                        await pipe.WriteAsync(header,_stop.Token); await pipe.WriteAsync(data,_stop.Token);
                    }
                } catch(Exception error) when (error is IOException or OperationCanceledException or ObjectDisposedException) { }
                finally { _pipes.TryRemove(connection,out _); lock(_state) if (_leaseOwner == connection) { Recording = false; _leaseOwner = 0; } }
            }
        }
        public async ValueTask DisposeAsync() { await _stop.CancelAsync(); foreach(var pipe in _pipes.Values) pipe.Dispose(); await Task.WhenAll(_listeners); _stop.Dispose(); }
    }
}
