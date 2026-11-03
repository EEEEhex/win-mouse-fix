using WinMouseFix.Settings.Resources;
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace WinMouseFix.Settings.Services;

[SupportedOSPlatform("windows")]
public sealed class SettingsEndpoint
{
    public string Scope { get; }
    public string PipeName => Scope+".Control";
    public string SettingsMutex => "Local\\"+Scope+".Settings";
    public string ActivationEvent => "Local\\"+Scope+".ActivateSettings";
    public string HostExecutable { get; }
    public string PreferencesDirectory { get; }
    public bool Simulation { get; }
    private SettingsEndpoint(string scope, string host, string preferences, bool simulation)
    {
        Scope = scope; HostExecutable = Path.GetFullPath(host); PreferencesDirectory = Path.GetFullPath(preferences); Simulation = simulation;
    }
    public static SettingsEndpoint Production()
    {
        var sid = WindowsIdentity.GetCurrent().User?.Value ?? throw new InvalidOperationException(UiText.Get("无法确定当前用户。"));
        var desktop = new StringBuilder(256);
        if (!Native.GetUserObjectInformationW(Native.GetThreadDesktop(Native.GetCurrentThreadId()), 2, desktop, 512, out _))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        ulong hash = 14695981039346656037UL;
        unchecked { foreach (char c in desktop.ToString()) { hash = (hash ^ (byte)c)*1099511628211UL; hash = (hash ^ (byte)(c >> 8))*1099511628211UL; } }
        var scope = $"WinMouseFix.v1.{sid}.{Process.GetCurrentProcess().SessionId}.{hash:x16}";
        return new(scope, LocateHost(), Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WinMouseFix/Settings"), false);
    }
    public static SettingsEndpoint Isolated(string scope, string host, string preferences)
    {
        if (!scope.Contains(".simulation.", StringComparison.Ordinal)) throw new ArgumentException(UiText.Get("隔离设置需要 simulation 命名空间。"));
        var productionData = Path.GetFullPath(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WinMouseFix"));
        var root = Path.GetFullPath(preferences);
        if (root.Equals(productionData, StringComparison.OrdinalIgnoreCase) || root.StartsWith(productionData+Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException(UiText.Get("隔离设置不能使用真实用户数据目录。"));
        return new(scope, host, root, true);
    }
    private static string LocateHost()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        var staged = Path.GetFullPath(Path.Combine(directory.FullName, "../WinMouseFix.Host.exe"));
        if (File.Exists(staged)) return staged;
        var configuration = Assembly.GetExecutingAssembly().GetCustomAttribute<AssemblyConfigurationAttribute>()?.Configuration == "Debug" ? "Debug" : "Release";
        for (int i = 0; directory is not null && i < 10; ++i, directory = directory.Parent)
            if (File.Exists(Path.Combine(directory.FullName, "WinMouseFix.sln")))
                return Path.Combine(directory.FullName, "x64", configuration, "WinMouseFix.Host.exe");
        return staged; // A missing deployment is reported explicitly by the connection layer.
    }
    public uint VerifyServer(NamedPipeClientStream pipe)
    {
        if (!Native.GetNamedPipeServerProcessId(pipe.SafePipeHandle, out uint pid) ||
            !Native.ProcessIdToSessionId(pid, out uint session) || session != Process.GetCurrentProcess().SessionId)
            throw new IOException(UiText.Get("后台连接不属于当前 Windows 会话。"));
        using var process = Native.OpenProcess(0x1000, false, pid);
        var image = new StringBuilder(32768); uint size = 32768;
        if (process.IsInvalid || !Native.QueryFullProcessImageNameW(process, 0, image, ref size)) throw new IOException(UiText.Get("无法验证后台程序路径。"));
        if (!Canonical(image.ToString()).Equals(Canonical(HostExecutable), StringComparison.OrdinalIgnoreCase))
            throw new IOException(UiText.Get("后台程序与当前设置窗口不在同一套安装目录中。"));
        return pid;
    }
    private static string Canonical(string path)
    {
        using var file = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        var text = new StringBuilder(32768);
        uint count = Native.GetFinalPathNameByHandleW(file, text, (uint)text.Capacity, 0);
        if (count == 0 || count >= text.Capacity) throw new IOException(UiText.Get("无法解析后台可执行文件路径。"));
        return text.ToString();
    }
    private static class Native
    {
        [DllImport("kernel32.dll")] internal static extern uint GetCurrentThreadId();
        [DllImport("user32.dll")] internal static extern nint GetThreadDesktop(uint id);
        [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)] [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetUserObjectInformationW(nint handle, int index, StringBuilder value, uint bytes, out uint needed);
        [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetNamedPipeServerProcessId(SafePipeHandle pipe, out uint pid);
        [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool ProcessIdToSessionId(uint pid, out uint session);
        [DllImport("kernel32.dll", SetLastError = true)] internal static extern SafeProcessHandle OpenProcess(uint access, bool inherit, uint pid);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool QueryFullProcessImageNameW(SafeProcessHandle process, uint flags, StringBuilder path, ref uint count);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        internal static extern uint GetFinalPathNameByHandleW(SafeFileHandle file, StringBuilder path, uint count, uint flags);
    }
}
