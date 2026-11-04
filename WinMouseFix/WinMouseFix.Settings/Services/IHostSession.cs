using System;
using System.Threading;
using System.Threading.Tasks;
using WinMouseFix.Configuration;
namespace WinMouseFix.Settings.Services;
public interface IHostSession : IAsyncDisposable
{
    event Action<HostStatus>? StatusChanged;
    event Action<string>? Disconnected;
    event Action? Restarted;
    Task<HostReply> ConnectAsync(nint window, CancellationToken cancellation = default);
    Task<HostReply> GetConfigurationAsync(CancellationToken cancellation = default);
    Task<HostReply> UpdateAsync(ProductConfiguration config, ulong expected, CancellationToken cancellation = default);
    Task<HostReply> ImportAsync(ProductConfiguration config, ulong expected, CancellationToken cancellation = default);
    Task<HostReply> ResetAsync(ulong expected, CancellationToken cancellation = default);
    Task<HostReply> SetEnabledAsync(bool enabled, CancellationToken cancellation = default);
    Task<HostReply> SetStartupAsync(bool enabled, ulong expected, CancellationToken cancellation = default);
    Task<HostReply> SetDataLocationAsync(string location,ulong expected,CancellationToken cancellation = default);
    Task<HostReply> StartTraceAsync(CancellationToken cancellation = default);
    Task<HostReply> CheckUpdatesAsync(CancellationToken cancellation = default);
    Task<HostReply> ReadyAsync(CancellationToken cancellation = default);
    Task<HostReply> DownloadUpdateAsync(ulong checkId,CancellationToken cancellation = default);
    Task<HostReply> CancelUpdateAsync(CancellationToken cancellation = default);
    Task<HostReply> InstallUpdateAsync(CancellationToken cancellation = default);
    Task<HostReply> AcknowledgeUpdateAsync(ulong checkId,CancellationToken cancellation = default);
    Task<HostReply> RecordShellAsync(object observation,ulong expected,CancellationToken cancellation = default);
    Task<HostReply> RecordAsync(bool enabled, CancellationToken cancellation = default);
    Task<HostReply> ExportDiagnosticsAsync(CancellationToken cancellation = default);
    Task ExitAsync(CancellationToken cancellation = default);
}
