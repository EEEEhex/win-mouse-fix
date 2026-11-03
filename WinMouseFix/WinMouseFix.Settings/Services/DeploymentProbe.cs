using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace WinMouseFix.Settings.Services;

// Read-only verification inside the actual Settings process.
internal static class DeploymentProbe
{
    [DllImport("kernel32.dll",CharSet = CharSet.Unicode)]
    private static extern int GetCurrentPackageFullName(ref uint length,nint name);
    public static string VerifyPortableIdentity() {
        uint length = 0;
        if (GetCurrentPackageFullName(ref length,0) != 15700)
            throw new InvalidOperationException("Settings unexpectedly has an installed package identity.");
        var metadata = FileVersionInfo.GetVersionInfo(Environment.ProcessPath!);
        if (metadata.CompanyName != "EEEEhex" || metadata.ProductName != "Win Mouse Fix")
            throw new InvalidOperationException("Settings executable must use the public project publisher/product metadata.");
        return metadata.CompanyName;
    }
}
