using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace WinMouseFix.Configuration;

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed class ProductConfiguration
{
    [JsonRequired] public uint SchemaVersion { get; set; } = 1;
    [JsonRequired] public ulong Revision { get; set; } = 1;
    [JsonRequired] public bool Enabled { get; set; }
    [JsonRequired] public bool Startup { get; set; }
    public bool StartupNotification { get; set; } = true;
    public bool AutomaticUpdates { get; set; }
    public uint UpdateIntervalHours { get; set; } = 6;
    [JsonRequired] public List<ButtonBinding> Bindings { get; set; } =
    [
        new() { Id = 1, Button = "back", Trigger = "click", Action = "back" },
        new() { Id = 2, Button = "back", Trigger = "drag", Action = "content-drag" },
        new() { Id = 3, Button = "forward", Trigger = "click", Action = "forward" },
        new() { Id = 4, Button = "forward", Trigger = "drag", Action = "shell-gestures" }
    ];
    [JsonRequired] public ContentOptions Content { get; set; } = new();
    [JsonRequired] public ShellOptions Shell { get; set; } = new();
    [JsonRequired] public WheelOptions Wheel { get; set; } = new();
    [JsonRequired] public ZoomOptions Zoom { get; set; } = new();
    [JsonRequired] public TimingOptions Timing { get; set; } = new();
    public ProductConfiguration Clone() => ConfigurationJson.Parse(ConfigurationJson.Serialize(this));
}
[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed class ButtonBinding
{
    [JsonRequired] public uint Id { get; set; }
    [JsonRequired] public string Button { get; set; } = "back";
    [JsonRequired] public string Trigger { get; set; } = "click";
    [JsonRequired] public uint Modifiers { get; set; }
    [JsonRequired] public string Action { get; set; } = "back";
    [JsonRequired] public List<uint> Keys { get; set; } = [];
}
[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed class ContentOptions
{
    [JsonRequired] public double ThresholdCounts { get; set; } = 8;
    [JsonRequired] public double MmPerCountX { get; set; } = 0.1;
    [JsonRequired] public double MmPerCountY { get; set; } = 0.1;
}
[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed class ShellOptions
{
    [JsonRequired] public uint Fingers { get; set; } = 4;
    [JsonRequired] public double MmPerCountX { get; set; } = 0.1;
    [JsonRequired] public double MmPerCountY { get; set; } = 0.1;
    [JsonRequired] public double DirectionRatio { get; set; } = 1.35;
}
[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed class WheelOptions
{
    [JsonRequired] public string Mode { get; set; } = "off";
    [JsonRequired] public double MmPerNotch { get; set; } = 15;
    [JsonRequired] public double RegularMilliseconds { get; set; } = 90;
    [JsonRequired] public double HighMilliseconds { get; set; } = 180;
}
[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed class ZoomOptions
{
    [JsonRequired] public double MmPerNotch { get; set; } = 2;
    [JsonRequired] public double InitialPinchMm { get; set; } = 10;
}
[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed class TimingOptions
{
    [JsonRequired] public uint DoubleClickMs { get; set; } = 500;
    [JsonRequired] public uint HoldMs { get; set; } = 500;
    [JsonRequired] public uint DoubleClickTolerancePixels { get; set; } = 4;
}
public sealed record ConfigurationIssue(string Field, string Message, uint BindingId = 0, uint ConflictingId = 0);
public sealed class ConfigurationException(ConfigurationIssue issue) : Exception(issue.Message)
{
    public ConfigurationIssue Issue { get; } = issue;
}
public static class ConfigurationJson
{
    public const int MaximumBytes = 65536;
    public static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false,
        MaxDepth = 16,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow
    };
    public static ProductConfiguration Parse(string json)
    {
        if (Encoding.UTF8.GetByteCount(json) > MaximumBytes) throw new JsonException("Configuration exceeds 64 KiB.");
        var config = JsonSerializer.Deserialize<ProductConfiguration>(json, Options) ?? throw new JsonException("Configuration must be an object.");
        var issue = Validate(config); if (issue is not null) throw new ConfigurationException(issue); return config;
    }
    public static string Serialize(ProductConfiguration config)
    {
        var issue = Validate(config); if (issue is not null) throw new ConfigurationException(issue);
        return JsonSerializer.Serialize(config, Options);
    }
    public static ConfigurationIssue? Validate(ProductConfiguration c)
    {
        bool Range(double n, double min, double max) => double.IsFinite(n) && n >= min && n <= max;
        bool Scale(double n) => Range(Math.Abs(n), 0.000001, 100);
        ConfigurationIssue Bad(string f) => new(f, "参数超出允许范围。");
        if (c.SchemaVersion != 1) return new("schemaVersion", "配置格式不兼容，原文件将被保留。");
        if (c.Revision is 0 or > 9007199254740991UL) return Bad("revision");
        if (c.UpdateIntervalHours is < 1 or > 168) return Bad("updateIntervalHours");
        if (c.Content is null || c.Shell is null || c.Wheel is null || c.Zoom is null || c.Timing is null || c.Bindings is null) return Bad("config");
        if (!Range(c.Content.ThresholdCounts, 0.000001, 1000000)) return Bad("content.thresholdCounts");
        if (!Scale(c.Content.MmPerCountX)) return Bad("content.mmPerCountX");
        if (!Scale(c.Content.MmPerCountY)) return Bad("content.mmPerCountY");
        if (c.Shell.Fingers is not (3 or 4)) return Bad("shell.fingers");
        if (!Scale(c.Shell.MmPerCountX)) return Bad("shell.mmPerCountX");
        if (!Scale(c.Shell.MmPerCountY)) return Bad("shell.mmPerCountY");
        if (!Range(c.Shell.DirectionRatio, 1.01, 8)) return Bad("shell.directionRatio");
        if (c.Wheel.Mode is not ("off" or "regular" or "high")) return Bad("wheel.mode");
        if (!Scale(c.Wheel.MmPerNotch)) return Bad("wheel.mmPerNotch");
        if (!Range(c.Wheel.RegularMilliseconds, 10, 500)) return Bad("wheel.regularMilliseconds");
        if (!Range(c.Wheel.HighMilliseconds, c.Wheel.RegularMilliseconds, 1000)) return Bad("wheel.highMilliseconds");
        if (!Scale(c.Zoom.MmPerNotch)) return Bad("zoom.mmPerNotch");
        if (!Range(c.Zoom.InitialPinchMm, 1, 100)) return Bad("zoom.initialPinchMm");
        if (c.Timing.DoubleClickMs is < 100 or > 2000) return Bad("timing.doubleClickMs");
        if (c.Timing.HoldMs is < 100 or > 5000) return Bad("timing.holdMs");
        if (c.Timing.DoubleClickTolerancePixels > 100) return Bad("timing.doubleClickTolerancePixels");
        if (c.Bindings.Count > 64) return Bad("bindings");
        for (int i = 0; i < c.Bindings.Count; ++i)
        {
            var b = c.Bindings[i];
            if (b is null || b.Id == 0 || b.Button is not ("middle" or "back" or "forward") || (b.Modifiers & ~15u) != 0 ||
                !ActionCatalog.Compatible(b.Trigger, b.Action)) return new("bindings", "按钮或动作无效。", b?.Id ?? 0);
            if (b.Keys is null || b.Keys.Count > 8 || (b.Action == "shortcut" ? b.Keys.Count == 0 : b.Keys.Count != 0) ||
                b.Keys.Any(k => k is < 8 or > 254) || b.Keys.Distinct().Count() != b.Keys.Count)
                return new("bindings.keys", "快捷键需要 1–8 个不同的有效按键。", b.Id);
            for (int j = 0; j < i; ++j)
            {
                var prior = c.Bindings[j];
                bool wheel(string t) => t is "wheel" or "wheel-up" or "wheel-down";
                if (prior.Id == b.Id || (prior.Button == b.Button && prior.Modifiers == b.Modifiers &&
                    (prior.Trigger == b.Trigger || (wheel(prior.Trigger) && wheel(b.Trigger) && (prior.Trigger == "wheel" || b.Trigger == "wheel")))))
                    return new("bindings", "该触发条件已绑定其他动作。", b.Id, prior.Id);
            }
        }
        return null;
    }
}
public static class ActionCatalog
{
    public static readonly string[] Discrete = ["back", "forward", "middle-click", "shortcut", "volume-up", "volume-down", "mute", "play-pause", "previous-track", "next-track"];
    public static bool Compatible(string trigger, string action) => trigger switch
    {
        "drag" => action is "content-drag" or "shell-gestures",
        "wheel" => action == "native-zoom",
        "click" or "double-click" or "hold" or "wheel-up" or "wheel-down" => Discrete.Contains(action),
        _ => false
    };
}
