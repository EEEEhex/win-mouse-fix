using System;
using System.IO;
using System.Text.Json;
using System.Runtime.InteropServices;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
namespace WinMouseFix.Settings.Services;
public sealed class UiPreferences
{
    public int X { get; set; }
    public int Y { get; set; }
    public int Width { get; set; } = 880;
    public int Height { get; set; } = 760;
    public bool Positioned { get; set; }
    public string Page { get; set; } = "general";
    public ElementTheme Theme { get; set; }
    public string Language { get; set; } = "zh-Hans";
    public bool AutomaticUpdates { get; set; }
    public bool UpdatePreferencesMigrated { get; set; }
    public static UiPreferences Load(string directory) {
        try { var file = Path.Combine(directory,"ui.json"); if (!File.Exists(file) || new FileInfo(file).Length > 8192) return new(); var result = JsonSerializer.Deserialize<UiPreferences>(File.ReadAllText(file)) ?? new(); if (!Enum.IsDefined(result.Theme)) result.Theme = ElementTheme.Default; return result; }
        catch { return new(); }
    }
    public void Apply(AppWindow window) {
        // UI state is user-writable; reject absurd rectangles before WinRT adds
        // their coordinates or converts sizes to physical monitor coordinates.
        Width = Math.Clamp(Width,640,16384); Height = Math.Clamp(Height,480,16384);
        X = Math.Clamp(X,-100000,100000); Y = Math.Clamp(Y,-100000,100000);
        var area = Positioned ? DisplayArea.GetFromRect(new(X,Y,Math.Max(1,Width),Math.Max(1,Height)),DisplayAreaFallback.Nearest) : DisplayArea.GetFromWindowId(window.Id,DisplayAreaFallback.Primary);
        double scale = Positioned ? 1 : Math.Max(1,GetDpiForWindow(Microsoft.UI.Win32Interop.GetWindowFromWindowId(window.Id))/96.0);
        var work = area.WorkArea; int width = Math.Min(work.Width,Math.Max(640,(int)(Width*scale))),height = Math.Min(work.Height,Math.Max(480,(int)(Height*scale)));
        int x = Positioned ? Math.Clamp(X,work.X,work.X+work.Width-width) : work.X+(work.Width-width)/2;
        int y = Positioned ? Math.Clamp(Y,work.Y,work.Y+work.Height-height) : work.Y+(work.Height-height)/2;
        window.MoveAndResize(new(x,y,width,height));
    }
    public void Save(string directory, AppWindow window) {
        try { X = window.Position.X; Y = window.Position.Y; Width = window.Size.Width; Height = window.Size.Height; Positioned = true;
            Directory.CreateDirectory(directory); string target = Path.Combine(directory,"ui.json"), temporary = target+".tmp";
            File.WriteAllText(temporary,JsonSerializer.Serialize(this)); File.Move(temporary,target,true);
        } catch { /* Window placement never prevents closing settings or affects input configuration. */ }
    }
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(nint window);
}
