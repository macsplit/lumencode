using LumenCode.Core;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media.Imaging;
using System.Diagnostics;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Text.Json;
using Windows.Graphics.Imaging;
using Windows.Storage;

namespace LumenCode.Windows;

public sealed partial class MainWindow
{
    private async Task RunAuditAsync(string[] args)
    {
        int position = Array.IndexOf(args, "--gui-audit");
        if (args.Length <= position + 2) { Status.Text = "Usage: --gui-audit <repository> <output>"; return; }
        string repo = Path.GetFullPath(args[position + 1]), output = Path.GetFullPath(args[position + 2]);
        Directory.CreateDirectory(output);
        var checks = new List<object>();
        var timings = new List<object>();
        var inputDiagnostics = new List<object>();
        var gaps = new List<double>();
        var timer = new Microsoft.UI.Xaml.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(16) };
        var clock = Stopwatch.StartNew(); double previous = clock.Elapsed.TotalMilliseconds;
        timer.Tick += (_, _) => { double now = clock.Elapsed.TotalMilliseconds; gaps.Add(now - previous); previous = now; };
        void Check(string name, bool passed) { checks.Add(new { name, passed }); if (!passed) throw new InvalidOperationException("GUI assertion failed: " + name); }
        string? failure = null;
        try
        {
            timer.Start();
            await LoadProjectAsync(Path.Combine(repo, "tests", "fixtures", "baseline"));
            Check("Project tree populated", Files.RootNodes.Count > 0);
            Check("Dark theme", Root.RequestedTheme == ElementTheme.Dark);
            string file = Path.Combine(rootPath, "js_commonjs", "helper.js");
            await SelectFileAsync(new(file));
            // Index completion may supersede this request with an augmented
            // analysis of the same file. Await that real refresh as well.
            var initialDeadline = DateTime.UtcNow.AddMinutes(2);
            while (pending > 0 && DateTime.UtcNow < initialDeadline) await Task.Delay(20);
            Check("Real engine analysis visible", Payload.Symbols(analysis).Any(s => s.Text("name") == "wrapper"));
            Check("Source preview populated", previewText.Contains("function"));
            Check("Source read-only", Code.IsReadOnly);
            Check("Source viewer does not retain file undo history", !Code.Document.CanUndo());
            var deadline = DateTime.UtcNow.AddMinutes(2);
            while (!indexReady && DateTime.UtcNow < deadline) await Task.Delay(50);
            Check("Project index built", indexReady);
            // Index completion can refresh the selected file asynchronously.
            while (pending > 0 && DateTime.UtcNow < deadline) await Task.Delay(50);
            using var manifest = JsonDocument.Parse(await File.ReadAllTextAsync(Path.Combine(rootPath, "manifest.json")));
            var files = manifest.RootElement.GetProperty("cases").EnumerateArray()
                .Select(c => Path.Combine(repo, c.Text("root"), c.Text("file"))).Distinct().Take(12).ToArray();
            foreach (string path in files)
            {
                var watch = Stopwatch.StartNew();
                await SelectFileAsync(new(path));
                Check("Selected " + Path.GetFileName(path), analysis.Text("path").Replace('/', '\\').Equals(path.Replace('/', '\\'), StringComparison.OrdinalIgnoreCase));
                timings.Add(new { operation = "select", path = Path.GetRelativePath(repo, path), elapsedMs = watch.Elapsed.TotalMilliseconds, stages = lastSelectionMeasurement });
            }
            // Start competing selections without waiting for the first one. A
            // cancelled result must not overwrite the latest source or analysis.
            cache.Remove(files[0]);
            var superseded = SelectFileAsync(new(files[0]));
            while (previewText.Length == 0 && !superseded.IsCompleted) await Task.Delay(1);
            var latest = SelectFileAsync(new(files[^1]));
            await Task.WhenAll(superseded, latest);
            Check("Rapid navigation keeps latest analysis", analysis.Text("path").Replace('/', '\\').Equals(files[^1].Replace('/', '\\'), StringComparison.OrdinalIgnoreCase));
            Check("Rapid navigation keeps latest preview", previewText == await ReadPreviewAsync(files[^1], CancellationToken.None));
            Check("Revisit uses analysis cache", lastSelectionMeasurement?.CacheHit == true);
            cache.Clear();
            var selecting = SelectFileAsync(new(file));
            while (previewText.Length == 0 && !selecting.IsCompleted) await Task.Delay(1);
            bool sourceWhileAnalysing = previewText.Contains("function") && AnalysisSummary.Text == "Analysing…" && !selecting.IsCompleted;
            await selecting;
            Check("Source available while engine analysis is pending", sourceWhileAnalysing);
            string largeFile = Path.Combine(output, "large-preview.js");
            await File.WriteAllTextAsync(largeFile, string.Join("\n", Enumerable.Range(0, 2000).Select(n => $"export function sample{n}(value) {{ return value + {n}; }}")));
            try
            {
                await SelectFileAsync(new(largeFile));
                Check("Large file exposes all 2000 engine symbols", Payload.Symbols(analysis).Count() == 2000 && Overview.Items.Count == 2000);
                Check("Large source preview complete and read-only", previewText.Contains("sample1999") && Code.IsReadOnly);
                timings.Add(new { operation = "select-large", symbols = 2000, stages = lastSelectionMeasurement });
            }
            finally { File.Delete(largeFile); cache.Remove(largeFile); }
            CheckPreviewFormatting(Check);
            await SelectFileAsync(new(file));
            if (args.Contains("--desktop-input")) await CheckDesktopInputAsync(file, files[^1], Check, inputDiagnostics);
            var wrapper = Payload.Symbols(analysis).First(s => s.Text("name") == "wrapper");
            Inspect(DisplayItem.From(wrapper, "Symbols"));
            Check("Symbol details and calls", DetailTitle.Text == "wrapper" && wrapper.Items("calls").Any(s => s.Text("name") == "helper"));
            Check("Navigation history", history.Count > 1 && BackButton.IsEnabled);
            SearchBox.Text = "wrapper";
            // TextChanged is dispatched by XAML. Drive that real event path and
            // wait for its asynchronous result, rather than starting a second
            // competing request that can cancel the one being awaited.
            var searchDeadline = DateTime.UtcNow.AddSeconds(10);
            while (SearchResults.Items.Count == 0 && DateTime.UtcNow < searchDeadline) await Task.Delay(20);
            Check("Engine search results: " + Status.Text, SearchResults.Items.Count > 0);
            SearchBox.Text = "";
            var clearDeadline = DateTime.UtcNow.AddSeconds(2);
            while (Files.Visibility != Visibility.Visible && DateTime.UtcNow < clearDeadline) await Task.Delay(20);
            Check("Clearing search restores tree", Files.Visibility == Visibility.Visible);
            Check("Clearing search restores status", Status.Text == "Ready");
            Code.Document.Selection.SetRange(0, Math.Min(10, previewText.Length));
            Code.Document.Selection.GetText(Microsoft.UI.Text.TextGetOptions.None, out string selected);
            Check("Selectable source text", selected.Length > 0);
            await CaptureAsync(output, "windows-dark.png");
            AppWindow.Resize(new global::Windows.Graphics.SizeInt32(1080, 700));
            await Task.Delay(300);
            Check("Minimum pane dimensions", FilesColumn.ActualWidth >= 220 && OverviewColumn.ActualWidth >= 260 && DetailColumn.ActualWidth >= 260);
            Check("Minimum rows fit without clipping", ContentRow.ActualHeight + PreviewRow.ActualHeight + 6 <= WorkspaceGrid.ActualHeight + 1);
            await CaptureAsync(output, "windows-minimum.png");
        }
        catch (Exception error) { failure = error.ToString(); }
        finally
        {
            timer.Stop();
            var sorted = gaps.Order().ToArray();
            bool desktopInput = args.Contains("--desktop-input");
            await File.WriteAllTextAsync(Path.Combine(output, "gui.json"), JsonSerializer.Serialize(new { passed = failure is null, desktopInput, displayScale = Root.XamlRoot.RasterizationScale, checks, timings, inputDiagnostics, failure, heartbeat = new { nominalIntervalMs = 16, samples = gaps.Count, maxGapMs = gaps.DefaultIfEmpty().Max(), p95GapMs = sorted.Length > 0 ? sorted[Math.Min(sorted.Length - 1, (int)(sorted.Length * .95))] : 0 }, limitations = desktopInput ? "Programmatic workflows plus foreground-guarded OS-injected keyboard/pointer checks on this VM. Not human-input, accessibility, multiple-DPI or clean-machine acceptance." : "Programmatic workflows and text/colour checks. Desktop input was not requested; not mouse/keyboard, accessibility, multiple-DPI or clean-machine acceptance." }, new JsonSerializerOptions { WriteIndented = true }));
            projectRequest.Cancel(); selectionRequest.Cancel(); searchRequest.Cancel();
            Application.Current.Exit();
        }
    }

    private void CheckPreviewFormatting(Action<string, bool> check)
    {
        string source = "const text = \"{\\rtf1} café 日本語 😀 https://example/#tag\";\r\n// class \"comment\"\r\nfunction finalLine() { return 42; }";
        SetPreview(source, "Formatting audit", SourcePreview.Format(source, "example.js"));
        Code.Document.GetText(Microsoft.UI.Text.TextGetOptions.None, out string actual);
        check("RTF preserves Unicode, braces, backslashes and CRLF text", actual.TrimEnd('\r', '\n').Replace('\r', '\n') == source.Replace("\r\n", "\n"));
        var keyword = Code.Document.GetRange(0, 5).CharacterFormat.ForegroundColor;
        var literal = Code.Document.GetRange(actual.IndexOf("https://", StringComparison.Ordinal), actual.IndexOf("https://", StringComparison.Ordinal) + 5).CharacterFormat.ForegroundColor;
        var comment = Code.Document.GetRange(actual.IndexOf("class", StringComparison.Ordinal), actual.IndexOf("class", StringComparison.Ordinal) + 5).CharacterFormat.ForegroundColor;
        check("Dark palette keyword colour", keyword.R == 252 && keyword.G == 158 && keyword.B == 79);
        check("URL inside string retains string colour", literal.R == 143 && literal.G == 214 && literal.B == 143);
        check("Keyword inside comment retains comment colour", comment.R == 133 && comment.G == 145 && comment.B == 161);
        SelectPreviewLine(3);
        Code.Document.Selection.GetText(Microsoft.UI.Text.TextGetOptions.None, out string selected);
        check("Line navigation handles CRLF and astral Unicode", selected == "function finalLine() { return 42; }");
        foreach (var sample in new[]
        {
            (Path: "example.py", Text: "# class comment\nvalue = \"\"\"multiline\n日本語 😀\"\"\"\nreturn value"),
            (Path: "example.swift", Text: "// func comment\nstruct Example { let text = \"café\" }"),
            (Path: "example.json", Text: "{\"unicode\":\"日本語 😀\",\"enabled\":true,\"value\":42}"),
            (Path: "example.txt", Text: "{\\rtf1\\b literal} café 日本語 😀\tend")
        })
        {
            SetPreview(sample.Text, sample.Path, SourcePreview.Format(sample.Text, sample.Path));
            Code.Document.GetText(Microsoft.UI.Text.TextGetOptions.None, out actual);
            check("Preview round trip " + Path.GetExtension(sample.Path), actual.TrimEnd('\r', '\n').Replace('\r', '\n') == sample.Text);
        }
    }

    private async Task CheckDesktopInputAsync(string file, string other, Action<string, bool> check, List<object> inputDiagnostics)
    {
        file = Path.GetFullPath(file);
        other = Path.GetFullPath(other);
        Activate();
        using var input = new GuiInput(WinRT.Interop.WindowNative.GetWindowHandle(this));
        await input.WaitForForegroundAsync();
        await input.KeysAsync(0x11, 0x46); // Ctrl+F
        check("Ctrl+F focuses project search", ReferenceEquals(Microsoft.UI.Xaml.Input.FocusManager.GetFocusedElement(Root.XamlRoot), SearchBox));
        await input.TextAsync("wrapper");
        var deadline = DateTime.UtcNow.AddSeconds(10);
        while (SearchResults.Items.Count == 0 && DateTime.UtcNow < deadline) await Task.Delay(20);
        check("Keyboard typing reaches engine search", SearchResults.Items.Count > 0);
        await input.KeysAsync(0x11, 0x41); // Ctrl+A
        await input.KeysAsync(0x08); // Backspace
        check("Keyboard clears search and restores files", SearchBox.Text.Length == 0 && Files.Visibility == Visibility.Visible);
        await input.KeysAsync(0x09); // Tab
        check("Tab leaves search for another control", !ReferenceEquals(Microsoft.UI.Xaml.Input.FocusManager.GetFocusedElement(Root.XamlRoot), SearchBox));
        await SelectFileAsync(new(file));
        await SelectFileAsync(new(other));
        await input.KeysAsync(0x12, 0x25); // Alt+Left
        deadline = DateTime.UtcNow.AddSeconds(10);
        while ((pending > 0 || selectedPath != file) && DateTime.UtcNow < deadline) await Task.Delay(20);
        check("Alt+Left navigates back", selectedPath == file && analysis.Text("path").Replace('/', '\\').Equals(file.Replace('/', '\\'), StringComparison.OrdinalIgnoreCase));
        await input.KeysAsync(0x12, 0x27); // Alt+Right
        deadline = DateTime.UtcNow.AddSeconds(10);
        while ((pending > 0 || selectedPath != other) && DateTime.UtcNow < deadline) await Task.Delay(20);
        check("Alt+Right navigates forward", selectedPath == other && analysis.Text("path").Replace('/', '\\').Equals(other.Replace('/', '\\'), StringComparison.OrdinalIgnoreCase));
        Code.Focus(FocusState.Keyboard);
        check("Source preview receives keyboard focus", ReferenceEquals(Microsoft.UI.Xaml.Input.FocusManager.GetFocusedElement(Root.XamlRoot), Code));
        Code.Document.GetText(Microsoft.UI.Text.TextGetOptions.None, out string beforeTyping);
        await input.TextAsync("must not edit source");
        await input.KeysAsync(0x11, 0x5a); // Ctrl+Z
        Code.Document.GetText(Microsoft.UI.Text.TextGetOptions.None, out string afterTyping);
        check("Read-only source rejects typing and Undo", Code.IsReadOnly && beforeTyping == afterTyping && !Code.Document.CanUndo());
        await input.KeysAsync(0x11, 0x41); // Ctrl+A
        Code.Document.Selection.GetText(Microsoft.UI.Text.TextGetOptions.None, out string keyboardSelection);
        check("Keyboard selects source text", keyboardSelection.TrimEnd('\r', '\n') == beforeTyping.TrimEnd('\r', '\n'));
        double filesWidth = FilesColumn.ActualWidth, previewHeight = PreviewRow.ActualHeight;
        FilesDivider.Focus(FocusState.Keyboard);
        await input.KeysAsync(0x27); // Right
        check("Project divider resizes with Right arrow", FilesColumn.ActualWidth > filesWidth + 8);
        double resized = FilesColumn.ActualWidth;
        await input.KeysAsync(0x28); // Down must not resize a vertical divider
        check("Vertical divider ignores perpendicular arrows", Math.Abs(FilesColumn.ActualWidth - resized) < 1);
        OverviewDivider.Focus(FocusState.Keyboard);
        double overviewBefore = OverviewColumn.ActualWidth;
        await input.KeysAsync(0x27);
        check("Overview divider resizes with Right arrow", OverviewColumn.ActualWidth > overviewBefore + 8);
        PreviewDivider.Focus(FocusState.Keyboard);
        await input.KeysAsync(0x26); // Up
        check("Preview divider resizes with Up arrow", PreviewRow.ActualHeight > previewHeight + 8);
        resized = PreviewRow.ActualHeight;
        await input.KeysAsync(0x27);
        check("Horizontal divider ignores perpendicular arrows", Math.Abs(PreviewRow.ActualHeight - resized) < 1);
        filesWidth = FilesColumn.ActualWidth;
        var pointerTrace = new List<string>();
        Root.AddHandler(Microsoft.UI.Xaml.UIElement.PointerPressedEvent, new Microsoft.UI.Xaml.Input.PointerEventHandler((_, e) => pointerTrace.Add("root pressed " + e.GetCurrentPoint(Root).Position + " source=" + e.OriginalSource.GetType().Name)), true);
        FilesDivider.AddHandler(Microsoft.UI.Xaml.UIElement.PointerPressedEvent, new Microsoft.UI.Xaml.Input.PointerEventHandler((_, e) => pointerTrace.Add("pressed " + e.GetCurrentPoint(Root).Position + " left=" + e.GetCurrentPoint(Root).Properties.IsLeftButtonPressed)), true);
        FilesDivider.AddHandler(Microsoft.UI.Xaml.UIElement.PointerMovedEvent, new Microsoft.UI.Xaml.Input.PointerEventHandler((_, e) => pointerTrace.Add("moved " + e.GetCurrentPoint(Root).Position + " left=" + e.GetCurrentPoint(Root).Properties.IsLeftButtonPressed)), true);
        await input.DragAsync(FilesDivider, Root, 32, 0);
        inputDiagnostics.Add(new { operation = "project-pane-drag", before = filesWidth, after = FilesColumn.ActualWidth, coordinates = input.LastDrag, events = pointerTrace.ToArray() });
        check("Pointer drag resizes project pane", FilesColumn.ActualWidth > filesWidth + 16);
        check("Pointer press focuses divider", ReferenceEquals(Microsoft.UI.Xaml.Input.FocusManager.GetFocusedElement(Root.XamlRoot), FilesDivider));
        overviewBefore = OverviewColumn.ActualWidth;
        await input.DragAsync(OverviewDivider, Root, 32, 0);
        inputDiagnostics.Add(new { operation = "overview-pane-drag", before = overviewBefore, after = OverviewColumn.ActualWidth, coordinates = input.LastDrag });
        check("Pointer drag resizes overview pane", OverviewColumn.ActualWidth > overviewBefore + 16);
        previewHeight = PreviewRow.ActualHeight;
        await input.DragAsync(PreviewDivider, Root, 0, -32);
        inputDiagnostics.Add(new { operation = "preview-pane-drag", before = previewHeight, after = PreviewRow.ActualHeight, coordinates = input.LastDrag });
        check("Pointer drag resizes preview pane", PreviewRow.ActualHeight > previewHeight + 16);
        // Restore the default layout before normal/minimum screenshots.
        FilesColumn.Width = new GridLength(290);
        OverviewColumn.Width = new GridLength(1, GridUnitType.Star);
        PreviewRow.Height = new GridLength(1, GridUnitType.Star);
        await SelectFileAsync(new(file));
    }

    private async Task CaptureAsync(string directory, string name)
    {
        var bitmap = new RenderTargetBitmap(); await bitmap.RenderAsync(Root);
        byte[] pixels = (await bitmap.GetPixelsAsync()).ToArray();
        var folder = await StorageFolder.GetFolderFromPathAsync(directory);
        var file = await folder.CreateFileAsync(name, CreationCollisionOption.ReplaceExisting);
        using var stream = await file.OpenAsync(FileAccessMode.ReadWrite);
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
        encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied, (uint)bitmap.PixelWidth, (uint)bitmap.PixelHeight, 96, 96, pixels);
        await encoder.FlushAsync();
    }
}
