using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Automation.Peers;

namespace LumenCode.Windows;

/// <summary>A small keyboard-accessible divider; owns no application layout state.</summary>
public sealed class PaneSplitter : UserControl
{
    public bool IsVertical { get; set; }
    public event Action<PaneSplitter, double>? Dragged;
    protected override AutomationPeer OnCreateAutomationPeer() => new SplitterPeer(this);
    private sealed class SplitterPeer(PaneSplitter owner) : FrameworkElementAutomationPeer(owner)
    {
        protected override string GetClassNameCore() => nameof(PaneSplitter);
        protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Thumb;
        protected override string GetHelpTextCore() => owner.IsVertical ? "Use Left and Right arrow keys to resize." : "Use Up and Down arrow keys to resize.";
    }
    private global::Windows.Foundation.Point? last;
    public PaneSplitter()
    {
        IsTabStop = true;
        Content = new Border { Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        UseSystemFocusVisuals = true;
        HorizontalAlignment = HorizontalAlignment.Stretch;
        VerticalAlignment = VerticalAlignment.Stretch;
        Loaded += (_, _) => ProtectedCursor = InputSystemCursor.Create(IsVertical ? InputSystemCursorShape.SizeWestEast : InputSystemCursorShape.SizeNorthSouth);
        PointerPressed += (_, e) =>
        {
            if (e.Pointer.PointerDeviceType == global::Microsoft.UI.Input.PointerDeviceType.Mouse && !e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
            Focus(FocusState.Pointer);
            if (CapturePointer(e.Pointer)) last = e.GetCurrentPoint(Parent as UIElement).Position;
            e.Handled = true;
        };
        PointerMoved += (_, e) =>
        {
            if (last is not { } previous) return;
            var point = e.GetCurrentPoint(Parent as UIElement).Position;
            Dragged?.Invoke(this, IsVertical ? point.X - previous.X : point.Y - previous.Y);
            last = point;
        };
        PointerReleased += (_, e) => { last = null; ReleasePointerCapture(e.Pointer); };
        PointerCaptureLost += (_, _) => last = null;
        KeyDown += (_, e) =>
        {
            double delta = IsVertical
                ? e.Key == global::Windows.System.VirtualKey.Left ? -16 : e.Key == global::Windows.System.VirtualKey.Right ? 16 : 0
                : e.Key == global::Windows.System.VirtualKey.Up ? -16 : e.Key == global::Windows.System.VirtualKey.Down ? 16 : 0;
            if (delta != 0) { Dragged?.Invoke(this, delta); e.Handled = true; }
        };
    }
}
