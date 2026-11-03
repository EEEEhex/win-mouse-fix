using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Threading;
using Microsoft.UI.Xaml;
using WinMouseFix.Settings.Services;

namespace WinMouseFix.Settings;
public partial class App : Application
{
    private MainWindow? _window;
    private Mutex? _instance;
    private EventWaitHandle? _activation;
    private RegisteredWaitHandle? _activationWait;
    private string? _smokeOutput;
    public App() { InitializeComponent(); UnhandledException += (_,e) => { if (_smokeOutput is not null) { e.Handled = true; Failure(e.Exception); } }; }
    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        try {
            var arguments = Environment.GetCommandLineArgs().Skip(1).ToArray(); bool smoke = arguments.Contains("--ui-smoke");
            if (Environment.OSVersion.Version.Build < 22000) throw new PlatformNotSupportedException("WinMouseFix 需要 Windows 11。");
            SettingsEndpoint endpoint; IHostSession session;
            if (smoke) {
                int index = Array.IndexOf(arguments,"--output"); if (index < 0 || index+1 >= arguments.Length) throw new ArgumentException("UI smoke requires --output <isolated-directory>.");
                _smokeOutput = Path.GetFullPath(arguments[index+1]); Directory.CreateDirectory(_smokeOutput);
                int scopeIndex = Array.IndexOf(arguments,"--simulation-scope"); string tag = scopeIndex >= 0 && scopeIndex+1 < arguments.Length ? arguments[scopeIndex+1] : Guid.NewGuid().ToString("N");
                if (tag.Length > 64 || tag.Any(c => !char.IsAsciiLetterOrDigit(c) && c != '-')) throw new ArgumentException("Invalid simulation scope.");
                endpoint = SettingsEndpoint.Isolated("WinMouseFix.UI.simulation."+tag,Environment.ProcessPath!,Path.Combine(_smokeOutput,"preferences")); session = new FakeHostSession();
            } else { endpoint = SettingsEndpoint.Production(); session = new HostClient(endpoint); }
            _instance = new(false,endpoint.SettingsMutex,out bool created);
            _activation = new(false,EventResetMode.AutoReset,endpoint.ActivationEvent);
            if (!created) {
                if (!smoke) foreach(var process in Process.GetProcessesByName("WinMouseFix.Settings")) {
                    using(process) { try { if (process.Id != Environment.ProcessId && process.SessionId == Process.GetCurrentProcess().SessionId && string.Equals(process.MainModule?.FileName,Environment.ProcessPath,StringComparison.OrdinalIgnoreCase)) AllowSetForegroundWindow((uint)process.Id); } catch { } }
                }
                _activation.Set(); await session.DisposeAsync();
                if (smoke) File.WriteAllText(Path.Combine(_smokeOutput!,"result.json"),JsonSerializer.Serialize(new { simulation = true, ok = true, duplicate = true, newWindow = false, nativeCapture = false }));
                Exit(); return;
            }
            _window = new(endpoint,session,smoke);
            _activationWait = ThreadPool.RegisterWaitForSingleObject(_activation,(_,_) => _window.DispatcherQueue.TryEnqueue(() => { try { if (!_window.Model.Connected) return; if (smoke) _window.AppWindow.Show(false); else _window.Activate(); } catch { } }),null,Timeout.Infinite,false);
            _window.Closed += (_,_) => { _activationWait?.Unregister(null); _activation?.Dispose(); _instance?.Dispose(); };
            await _window.ConnectAsync();
            if (!_window.Model.Connected) throw new IOException(_window.Model.Message);
            if (smoke) _window.AppWindow.Show(false); else _window.Activate();
            if (smoke) {
                File.WriteAllText(Path.Combine(_smokeOutput!,"ready.json"),"{\"simulation\":true,\"ready\":true}");
                await _window.RunSmokeAsync(_smokeOutput!);
                int holdIndex = Array.IndexOf(arguments,"--smoke-hold-ms"); if (holdIndex >= 0 && holdIndex+1 < arguments.Length) { int milliseconds = int.Parse(arguments[holdIndex+1]); if (milliseconds < 0 || milliseconds > 8000) throw new ArgumentException("Invalid smoke hold."); await System.Threading.Tasks.Task.Delay(milliseconds); }
                _window.Close();
            }
        } catch(Exception e) { Failure(e); }
    }
    private void Failure(Exception error) {
        Environment.ExitCode = 1;
        if (_smokeOutput is not null) File.WriteAllText(Path.Combine(_smokeOutput,"result.json"),JsonSerializer.Serialize(new { simulation = true, ok = false, error = error.ToString() }));
        else MessageBoxW(0,error.Message,"WinMouseFix",0x10);
        Exit();
    }
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool AllowSetForegroundWindow(uint pid);
    [DllImport("user32.dll",CharSet = CharSet.Unicode)] private static extern int MessageBoxW(nint window,string message,string title,uint flags);
}
