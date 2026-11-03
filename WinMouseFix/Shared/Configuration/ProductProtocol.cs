using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace WinMouseFix.Configuration;

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed class HostRequest
{
    public uint ProtocolVersion { get; set; } = 1;
    public string RequestId { get; set; } = Guid.NewGuid().ToString("N");
    public string HostRunId { get; set; } = "";
    public string Command { get; set; } = "hello";
    public ulong ExpectedRevision { get; set; }
    public JsonElement Payload { get; set; } = JsonSerializer.SerializeToElement(new { });
}
[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed class HostReply
{
    [JsonRequired] public uint ProtocolVersion { get; set; }
    [JsonRequired] public string RequestId { get; set; } = "";
    [JsonRequired] public string HostRunId { get; set; } = "";
    [JsonRequired] public bool Ok { get; set; }
    [JsonRequired] public string Code { get; set; } = "";
    [JsonRequired] public string Message { get; set; } = "";
    [JsonRequired] public string Field { get; set; } = "";
    [JsonRequired] public ulong Revision { get; set; }
    [JsonRequired] public ulong ActiveRevision { get; set; }
    [JsonRequired] public uint BindingId { get; set; }
    [JsonRequired] public uint ConflictingId { get; set; }
    [JsonRequired] public uint NativeError { get; set; }
    [JsonRequired] public uint CleanupError { get; set; }
    [JsonRequired] public JsonElement Payload { get; set; }
}
public sealed class HostStatus
{
    public string ProductVersion { get; set; } = "";
    public uint AbiVersion { get; set; }
    public string Architecture { get; set; } = "";
    public string NativeSdk { get; set; } = "";
    public OperatingSystemInfo Os { get; set; } = new();
    public ulong Version { get; set; }
    public string State { get; set; } = "offline";
    public bool Requested { get; set; }
    public ulong ConfigRevision { get; set; }
    public ulong ActiveRevision { get; set; }
    public bool Recording { get; set; }
    public uint RecordingRemainingMilliseconds { get; set; }
    public bool StartupRegistered { get; set; }
    public uint StartupError { get; set; }
    public bool RecoveryRequired { get; set; }
    public string RecoveryReason { get; set; } = "";
    public ulong LostTraceRecords { get; set; }
    public uint LogError { get; set; }
    public bool ExitRequested { get; set; }
    public UpdateStatus Update { get; set; } = new();
    public FailureInfo Failure { get; set; } = new();
    public CaptureInfo Capture { get; set; } = new();
    public List<CapabilityInfo> Capabilities { get; set; } = [];
    public RecordedButton? RecordedButton { get; set; }
}
public sealed class UpdateStatus
{
    public string State { get; set; } = "idle";
    public string CurrentVersion { get; set; } = "2.0.0";
    public ulong CheckId { get; set; }
    public ulong CompletedId { get; set; }
    public bool NotificationPending { get; set; }
    public string Version { get; set; } = "";
    public string Title { get; set; } = "";
    public string Notes { get; set; } = "";
    public string Url { get; set; } = "";
    public string Error { get; set; } = "";
    public string CheckedUtc { get; set; } = "";
}
public sealed class OperatingSystemInfo
{
    public uint Major { get; set; }
    public uint Minor { get; set; }
    public uint Build { get; set; }
    public uint Revision { get; set; }
    public bool RevisionKnown { get; set; }
}
public sealed class FailureInfo
{
    public uint Status { get; set; }
    public uint NativeError { get; set; }
    public uint CleanupError { get; set; }
}
public sealed class CaptureInfo
{
    public ulong RawPackets { get; set; }
    public ulong RawWheels { get; set; }
    public ulong BlockedWheels { get; set; }
    public ulong InjectedWheelHooks { get; set; }
    public ulong KeyboardTakeovers { get; set; }
    public ulong HookCalls { get; set; }
    public ulong HookTicks { get; set; }
    public ulong HookMaxTicks { get; set; }
}
public sealed class CapabilityInfo
{
    public string Name { get; set; } = "";
    public string State { get; set; } = "unverified"; // supported/unavailable/unverified/development
    public string Reason { get; set; } = "";
    public string Trigger { get; set; } = "not-run";
    public string Continuous { get; set; } = "not-run";
    public string Hold { get; set; } = "not-run";
    public string Reverse { get; set; } = "not-run";
    public string Release { get; set; } = "not-run";
    public string Evidence { get; set; } = "";
}
public sealed class RecordedButton
{
    public string Button { get; set; } = "";
    public uint Modifiers { get; set; }
    public ulong Serial { get; set; }
}
