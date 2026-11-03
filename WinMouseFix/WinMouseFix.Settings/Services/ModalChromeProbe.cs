using System;
using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;

namespace WinMouseFix.Settings.Services;

// Read-only measurements for the isolated UI smoke; never sends window input.
internal static class ModalChromeProbe
{
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left,Top,Right,Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct Point { public int X,Y; }
    [DllImport("user32.dll")] [return:MarshalAs(UnmanagedType.Bool)] private static extern bool GetWindowRect(nint window,out Rect rectangle);
    [DllImport("user32.dll")] [return:MarshalAs(UnmanagedType.Bool)] private static extern bool ClientToScreen(nint window,ref Point point);
    [DllImport("dwmapi.dll")] private static extern int DwmGetWindowAttribute(nint window,uint attribute,out Rect value,uint size);
    public static object Read(Window window,FrameworkElement root,FrameworkElement dialog,double titleBarHeight) {
        var handle = WinRT.Interop.WindowNative.GetWindowHandle(window); var client = new Point();
        bool gotBounds = GetWindowRect(handle,out var bounds),gotClient = ClientToScreen(handle,ref client); int result = DwmGetWindowAttribute(handle,5,out var caption,16);
        if (!gotBounds || !gotClient || result != 0 || caption.Bottom <= caption.Top)
            throw new InvalidOperationException($"Cannot measure native caption: window={gotBounds},client={gotClient},dwm={result},rect={caption.Left},{caption.Top},{caption.Right},{caption.Bottom},windowTop={bounds.Top},clientTop={client.Y}.");
        var position = dialog.TransformToVisual(root).TransformPoint(new(0,0));
        double dialogTop = client.Y+position.Y*root.XamlRoot.RasterizationScale;
        int captionBottom = bounds.Top+caption.Bottom;
        double titleBarBottom = client.Y+titleBarHeight*root.XamlRoot.RasterizationScale;
        bool overlap = dialogTop < Math.Max(captionBottom,titleBarBottom)-2;
        if (!window.ExtendsContentIntoTitleBar || overlap) throw new InvalidOperationException("Native caption buttons overlap the binding editor or the titlebar mode changed.");
        return new { windowWidth=bounds.Right-bounds.Left,windowHeight=bounds.Bottom-bounds.Top,clientTop=client.Y,captionBottom,titleBarBottom,captionBoundsWidth=caption.Right-caption.Left,dialogTop,overlap,titleBarModeUnchanged=true };
    }
}
