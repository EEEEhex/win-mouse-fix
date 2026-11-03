using System.Text.Json;
using WinMouseFix.Configuration;

var repo = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../../../"));
if (args.Length == 1) repo = Path.GetFullPath(args[0]);
var shared = Path.Combine(repo, "WinMouseFix/Shared/Configuration");
var output = Path.Combine(repo, "artifacts/product-validation/config-goldens");
Directory.CreateDirectory(output);
int checks = 0;
void Check(bool value) { checks++; if (!value) throw new Exception($"Configuration check {checks} failed."); }
foreach(var entry in WinMouseFix.Settings.Resources.UiText.FallbackStrings) {
    WinMouseFix.Settings.Resources.UiText.Language = "zh-Hans"; Check(WinMouseFix.Settings.Resources.UiText.Get(entry.Key) == entry.Key);
    WinMouseFix.Settings.Resources.UiText.Language = "missing-language"; Check(WinMouseFix.Settings.Resources.UiText.Get(entry.Key) == entry.Value);
}
WinMouseFix.Settings.Resources.UiText.Language = "en"; Check(WinMouseFix.Settings.Resources.UiText.Get("保存") == "Save");
Check(WinMouseFix.Settings.Resources.UiText.Format("后台发生错误，已停止接管。错误 {0}，清理 {1}",5,6).Contains("error 5"));
WinMouseFix.Settings.Resources.UiText.Language = "zh-Hans";
foreach (var name in new[] { "default", "custom" })
{
    var json = File.ReadAllText(Path.Combine(shared, name+".json"));
    var c = ConfigurationJson.Parse(json);
    Check(c.StartupNotification);
    var encoded = ConfigurationJson.Serialize(c);
    Check(ConfigurationJson.Serialize(ConfigurationJson.Parse(encoded)) == encoded);
    File.WriteAllText(Path.Combine(output, "csharp-"+name+".json"), encoded);
    var native = Path.Combine(output, "cpp-"+name+".json");
    if (File.Exists(native)) Check(ConfigurationJson.Serialize(ConfigurationJson.Parse(File.ReadAllText(native))) == encoded);
    Check(name == "default" ? !c.Enabled && c.Bindings.Count == 4 : c.Revision == 42 && c.Content.MmPerCountX == -0.25 && c.Bindings[0].Keys[1] == 84);
}
var notificationConfig = new ProductConfiguration { StartupNotification = false };
Check(!ConfigurationJson.Parse(ConfigurationJson.Serialize(notificationConfig)).StartupNotification);
bool invalidNotification = false;
try { ConfigurationJson.Parse(ConfigurationJson.Serialize(notificationConfig).Replace("\"startupNotification\":false", "\"startupNotification\":\"false\"")); }
catch (JsonException) { invalidNotification = true; }
Check(invalidNotification);
var updateConfig = new ProductConfiguration { AutomaticUpdates = true, UpdateIntervalHours = 24 };
var updateRoundTrip = ConfigurationJson.Parse(ConfigurationJson.Serialize(updateConfig)); Check(updateRoundTrip.AutomaticUpdates && updateRoundTrip.UpdateIntervalHours == 24);
void Invalid(Action<ProductConfiguration> mutate, string? field = null)
{
    var c = new ProductConfiguration(); mutate(c); var issue = ConfigurationJson.Validate(c);
    Check(issue is not null && (field is null || field == issue.Field));
}
Invalid(c => c.Content.ThresholdCounts = 0, "content.thresholdCounts");
Invalid(c => c.Content.MmPerCountY = double.NaN, "content.mmPerCountY");
Invalid(c => c.Shell.Fingers = 5, "shell.fingers");
Invalid(c => c.Wheel.HighMilliseconds = 10, "wheel.highMilliseconds");
Invalid(c => c.Timing.HoldMs = 50, "timing.holdMs");
Invalid(c => c.UpdateIntervalHours = 0, "updateIntervalHours");
Invalid(c => c.UpdateIntervalHours = 169, "updateIntervalHours");
Invalid(c => c.Bindings[1].Trigger = "click");
Invalid(c => c.Bindings[1].Action = "launchpad");
Invalid(c => c.Bindings.Add(new() { Id = 5, Button = "middle", Action = "shortcut", Keys = [17,17] }), "bindings.keys");
Invalid(c => c.Revision = 9007199254740992UL, "revision");
foreach (var json in new[] { "null", "{}", "[]", "{\"schemaVersion\":2}", "{\"enabled\":\"false\"}", new string('x',65537) })
{
    bool rejected = false;
    try { ConfigurationJson.Parse(json); } catch (Exception e) when (e is JsonException or ConfigurationException) { rejected = true; }
    Check(rejected);
}
var nativeStatus = Path.Combine(output, "service-status.json");
if (File.Exists(nativeStatus))
{
    var status = JsonSerializer.Deserialize<HostStatus>(File.ReadAllText(nativeStatus), ConfigurationJson.Options)!;
    Check(status.ConfigRevision >= 4 && status.Capabilities.Count == 7 && status.RecordingRemainingMilliseconds == 0);
    Check(status.Capabilities.Single(c => c.Name == "task-view").Continuous == "limited");
    Check(status.Capabilities.Single(c => c.Name == "wheel").State == "unverified");
    var reply = JsonSerializer.Deserialize<HostReply>(File.ReadAllText(Path.Combine(output, "service-reply.json")), ConfigurationJson.Options)!;
    Check(reply.Ok && reply.ProtocolVersion == 1 && !string.IsNullOrEmpty(reply.HostRunId));
    Check(ConfigurationJson.Parse(reply.Payload.GetProperty("config").GetRawText()).Revision == reply.Revision);
}
checks += await ClientChecks.Models();
var logExport = Path.Combine(repo,"artifacts/product-validation/log-smoke/export.json");
if (File.Exists(logExport)) {
    using var metadata = JsonDocument.Parse(File.ReadAllText(logExport)); Check(metadata.RootElement.GetProperty("simulation").GetBoolean());
    using var archive = System.IO.Compression.ZipFile.OpenRead(metadata.RootElement.GetProperty("zip").GetString()!);
    Check(archive.Entries.Count is >= 3 and <= 11);
    foreach (var entry in archive.Entries) {
        Check(!entry.FullName.Contains('/') && !entry.FullName.Contains('\\') && entry.Length <= 2*1024*1024);
        using var reader = new StreamReader(entry.Open(),new System.Text.UTF8Encoding(false,true)); var text = reader.ReadToEnd();
        if (entry.FullName.EndsWith(".json")) { using var json = JsonDocument.Parse(text); Check(json.RootElement.ValueKind == JsonValueKind.Object); }
        else foreach (var line in text.Split('\n',StringSplitOptions.RemoveEmptyEntries)) { using var json = JsonDocument.Parse(line); Check(json.RootElement.ValueKind == JsonValueKind.Object); }
    }
}
if (OperatingSystem.IsWindows()) checks += await ClientChecks.Pipes(output);
Console.WriteLine($"{checks} managed configuration/client/model checks passed; isolated pipe stubs only; no real Host or desktop input started.");
