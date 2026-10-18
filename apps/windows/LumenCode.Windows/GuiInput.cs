using Microsoft.UI.Xaml;
using System.Runtime.InteropServices;

namespace LumenCode.Windows;

/// <summary>OS input for the explicit interactive GUI audit only.</summary>
internal sealed class GuiInput : IDisposable
{
    private readonly nint window;
    private readonly Point savedCursor;
    public string LastDrag { get; private set; } = "";
    public GuiInput(nint window)
    {
        this.window = window;
        if (!GetCursorPos(out savedCursor)) throw new InvalidOperationException("Cannot read desktop cursor position.");
        SetForegroundWindow(window);
    }
    private void RequireForeground()
    {
        if (GetForegroundWindow() != window) throw new InvalidOperationException($"Audit window is not foreground (own {window}, foreground {GetForegroundWindow()}); input stopped.");
    }
    public async Task WaitForForegroundAsync()
    {
        // Scheduled tasks may not steal foreground focus. Give a person at the
        // VM time to activate the audit window; never send input elsewhere.
        var deadline = DateTime.UtcNow.AddSeconds(30);
        while (GetForegroundWindow() != window && DateTime.UtcNow < deadline) await Task.Delay(100);
        RequireForeground();
    }
    public async Task KeysAsync(params ushort[] keys)
    {
        RequireForeground();
        var inputs = keys.Select(k => Key(k, 0, 0)).Concat(keys.Reverse().Select(k => Key(k, 0, 2))).ToArray();
        Send(inputs);
        await Task.Delay(100);
    }
    public async Task TextAsync(string text)
    {
        RequireForeground();
        Send(text.SelectMany(c => new[] { Key(0, c, 4), Key(0, c, 6) }).ToArray());
        await Task.Delay(100);
    }
    public async Task DragAsync(FrameworkElement element, FrameworkElement root, int dx, int dy)
    {
        RequireForeground();
        var center = element.TransformToVisual(root).TransformPoint(new global::Windows.Foundation.Point(element.ActualWidth / 2, element.ActualHeight / 2));
        var origin = new Point();
        if (!ClientToScreen(window, ref origin)) throw new InvalidOperationException("Cannot locate audit window.");
        double scale = root.XamlRoot.RasterizationScale;
        int x = origin.X + (int)Math.Round(center.X * scale), y = origin.Y + (int)Math.Round(center.Y * scale);
        LastDrag = $"root {root.ActualWidth}x{root.ActualHeight}, scale {scale}, center {center}, client {origin.X},{origin.Y}, screen {x},{y}";
        if (GetAncestor(WindowFromPoint(new Point { X = x, Y = y }), 2) != window)
            throw new InvalidOperationException("Audit pointer target is outside its own window; input stopped.");
        MovePointer(x, y);
        await Task.Delay(50);
        RequireForeground();
        Send([new Input { Type = 0, Data = new Union { Mouse = new Mouse { Flags = 2 } } }]);
        try
        {
            for (int step = 1; step <= 4; step++)
            {
                RequireForeground();
                MovePointer(x + (int)(dx * scale * step / 4), y + (int)(dy * scale * step / 4));
                await Task.Delay(30);
            }
        }
        finally { Send([new Input { Type = 0, Data = new Union { Mouse = new Mouse { Flags = 4 } } }]); }
        await Task.Delay(100);
    }
    private static void MovePointer(int x, int y)
    {
        int left = GetSystemMetrics(76), top = GetSystemMetrics(77);
        int width = GetSystemMetrics(78), height = GetSystemMetrics(79);
        if (width <= 1 || height <= 1) throw new InvalidOperationException("Interactive desktop bounds unavailable.");
        // Cursor repositioning alone did not deliver captured WinUI moves on
        // the VM. Send actual absolute mouse input over the virtual desktop.
        Send([new Input { Type = 0, Data = new Union { Mouse = new Mouse {
            X = (int)Math.Round((x - left) * 65535.0 / (width - 1)),
            Y = (int)Math.Round((y - top) * 65535.0 / (height - 1)), Flags = 0xC001
        } } }]);
    }
    private static Input Key(ushort key, ushort scan, uint flags) => new() { Type = 1, Data = new Union { Keyboard = new Keyboard { Key = key, Scan = scan, Flags = flags | (key is >= 0x21 and <= 0x2e ? 1u : 0u) } } };
    private static void Send(Input[] inputs)
    {
        if (SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<Input>()) != inputs.Length)
            throw new InvalidOperationException("Windows rejected GUI audit input.");
    }
    public void Dispose() => SetCursorPos(savedCursor.X, savedCursor.Y);
    [StructLayout(LayoutKind.Sequential)] private struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct Mouse { public int X, Y; public uint Data, Flags, Time; public nuint Extra; }
    [StructLayout(LayoutKind.Sequential)] private struct Keyboard { public ushort Key, Scan; public uint Flags, Time; public nuint Extra; }
    [StructLayout(LayoutKind.Explicit)] private struct Union { [FieldOffset(0)] public Mouse Mouse; [FieldOffset(0)] public Keyboard Keyboard; }
    [StructLayout(LayoutKind.Sequential)] private struct Input { public uint Type; public Union Data; }
    [DllImport("user32.dll", SetLastError = true)] private static extern uint SendInput(uint count, Input[] inputs, int size);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(nint window);
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] private static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] private static extern bool ClientToScreen(nint window, ref Point point);
    [DllImport("user32.dll")] private static extern nint WindowFromPoint(Point point);
    [DllImport("user32.dll")] private static extern nint GetAncestor(nint window, uint flags);
    [DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);
}
