using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
namespace WinMouseFix.Settings.Resources;
public static class UiText
{
    private static readonly Dictionary<string,string> English = Read("en");
    private static readonly Dictionary<string,string> Chinese = Read("zh-Hans");
    public static string Language { get; set; } = "zh-Hans";
    public static string Get(string key) => (Language == "zh-Hans" && Chinese.TryGetValue(key,out var chinese)) ? chinese : English.GetValueOrDefault(key,key);
    public static string Format(string key,params object[] args) => string.Format(CultureInfo.CurrentCulture,Get(key),args);
    public static IReadOnlyDictionary<string,string> FallbackStrings => English;
    private static Dictionary<string,string> Read(string language) {
        var assembly = typeof(UiText).Assembly; var name = assembly.GetManifestResourceNames().Single(n => n.EndsWith("Strings."+language+".json",StringComparison.Ordinal));
        using Stream stream = assembly.GetManifestResourceStream(name)!; return JsonSerializer.Deserialize<Dictionary<string,string>>(stream)!;
    }
}
