using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation.Peers;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Windows.Storage.Pickers;

namespace LumenCode.Windows;

public sealed partial class MainWindow
{
    private async Task CheckWindowsBehaviourAsync(string output, Action<string, bool> check)
    {
        foreach (var control in new (FrameworkElement Element, string Name)[]
        {
            (Code, "Source preview"), (SearchBox, "Search files and symbols"),
            (FilesDivider, "Resize project pane"), (OverviewDivider, "Resize overview pane"), (PreviewDivider, "Resize code preview")
        })
        {
            var peer = FrameworkElementAutomationPeer.CreatePeerForElement(control.Element);
            check("Automation name: " + control.Name, peer is not null && peer.GetName() == control.Name);
            if (control.Element is PaneSplitter)
                check("Automation role and keyboard help: " + control.Name, peer!.GetAutomationControlType() == AutomationControlType.Thumb && peer.GetHelpText().Contains("arrow keys"));
        }
        var sourcePeer = FrameworkElementAutomationPeer.CreatePeerForElement(Code);
        // RichEdit's text provider is bridged through its native child. The
        // managed peer alone does not expose that external-client pattern.
        check("Source exposes named read-only edit control", sourcePeer?.GetAutomationControlType() == AutomationControlType.Edit && Code.IsReadOnly);

        string savedRoot = rootPath, savedFile = selectedPath;
        var picker = new FolderPicker();
        picker.FileTypeFilter.Add("*");
        nint owner = WinRT.Interop.WindowNative.GetWindowHandle(this);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, owner);
        var operation = picker.PickSingleFolderAsync();
        var task = operation.AsTask();
        nint dialog = 0;
        try
        {
            for (int attempt = 0; attempt < 100 && dialog == 0 && !task.IsCompleted; attempt++)
            {
                await Task.Delay(50);
                dialog = AuditNative.FindOwnedDialog(owner);
            }
            check("Native folder picker opens as an owned dialog", dialog != 0);
        }
        finally
        {
            // WM_CLOSE targets only the observed modal dialog owned by this
            // audit window. Never send Escape to whichever app has focus.
            if (dialog != 0) AuditNative.PostMessage(dialog, 0x10, 0, 0);
            else operation.Cancel();
        }
        var folder = await task.WaitAsync(TimeSpan.FromSeconds(10));
        check("Cancelling folder picker preserves workspace", folder is null && rootPath == savedRoot && selectedPath == savedFile);

        var original = AuditNative.GetContrast();
        try
        {
            AuditNative.SetContrast(original with { Flags = original.Flags | 1 });
            await WaitContrastAsync(true);
            check("Real Windows high contrast is detected", themeSettings.HighContrast);
            SetPreview("const sample = 'café 日本語 😀'; // comment", "Contrast audit");
            var expected = new global::Windows.UI.ViewManagement.UISettings().UIElementColor(global::Windows.UI.ViewManagement.UIElementType.WindowText);
            var first = Code.Document.GetRange(0, 5).CharacterFormat.ForegroundColor;
            var last = Code.Document.GetRange(30, 35).CharacterFormat.ForegroundColor;
            check("High contrast source uses system text colour", first == expected && last == expected);
            await Task.Delay(300);
            await CaptureAsync(output, "windows-high-contrast.png");
        }
        finally
        {
            AuditNative.SetContrast(original);
            await WaitContrastAsync((original.Flags & 1) != 0);
        }
        check("Original Windows contrast setting restored", themeSettings.HighContrast == ((original.Flags & 1) != 0));
        await File.WriteAllTextAsync(Path.Combine(output, "windows-behaviour.json"), JsonSerializer.Serialize(new { passed = true, displayScale = Root.XamlRoot.RasterizationScale, folderPicker = "Owned native dialog opened and cancelled", highContrast = "System setting enabled, source colour checked, original setting restored", limitations = "Automation peers and one real contrast theme on this VM; not a human Narrator review, multi-monitor or multiple-DPI acceptance." }, new JsonSerializerOptions { WriteIndented = true }));
    }

    private async Task WaitContrastAsync(bool enabled)
    {
        for (int attempt = 0; attempt < 100 && themeSettings.HighContrast != enabled; attempt++) await Task.Delay(50);
        if (themeSettings.HighContrast != enabled) throw new InvalidOperationException("Windows contrast change was not observed.");
        await Task.Delay(300);
    }

    private static class AuditNative
    {
        internal sealed record Contrast(uint Flags, string? Scheme);
        [StructLayout(LayoutKind.Sequential)] private struct HighContrast { public uint Size, Flags; public nint Scheme; }
        private delegate bool EnumWindow(nint handle, nint parameter);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool SystemParametersInfo(uint action, uint size, ref HighContrast data, uint flags);
        [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindow callback, nint parameter);
        [DllImport("user32.dll")] private static extern nint GetWindow(nint handle, uint command);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(nint handle, StringBuilder name, int count);
        [DllImport("user32.dll")] private static extern bool IsWindowVisible(nint handle);
        [DllImport("user32.dll")] internal static extern bool PostMessage(nint handle, uint message, nint wParam, nint lParam);
        internal static nint FindOwnedDialog(nint owner)
        {
            nint result = 0;
            EnumWindows((handle, _) =>
            {
                var name = new StringBuilder(64);
                GetClassName(handle, name, name.Capacity);
                if (GetWindow(handle, 4) == owner && IsWindowVisible(handle) && name.ToString() == "#32770") { result = handle; return false; }
                return true;
            }, 0);
            return result;
        }
        internal static Contrast GetContrast()
        {
            var data = new HighContrast { Size = (uint)Marshal.SizeOf<HighContrast>() };
            if (!SystemParametersInfo(0x42, data.Size, ref data, 0)) throw new System.ComponentModel.Win32Exception();
            return new(data.Flags, Marshal.PtrToStringUni(data.Scheme));
        }
        internal static void SetContrast(Contrast value)
        {
            var data = new HighContrast { Size = (uint)Marshal.SizeOf<HighContrast>(), Flags = value.Flags, Scheme = value.Scheme is null ? 0 : Marshal.StringToHGlobalUni(value.Scheme) };
            try { if (!SystemParametersInfo(0x43, data.Size, ref data, 2)) throw new System.ComponentModel.Win32Exception(); }
            finally { if (data.Scheme != 0) Marshal.FreeHGlobal(data.Scheme); }
        }
    }
}
