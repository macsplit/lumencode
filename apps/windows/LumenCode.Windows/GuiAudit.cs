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
            await SelectFileAsync(new(file));
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
            await File.WriteAllTextAsync(Path.Combine(output, "gui.json"), JsonSerializer.Serialize(new { passed = failure is null, checks, timings, failure, heartbeat = new { nominalIntervalMs = 16, samples = gaps.Count, maxGapMs = gaps.DefaultIfEmpty().Max(), p95GapMs = sorted.Length > 0 ? sorted[Math.Min(sorted.Length - 1, (int)(sorted.Length * .95))] : 0 }, limitations = "Programmatic workflow checks on this VM, not mouse/keyboard, accessibility, multiple-DPI or clean-machine acceptance." }, new JsonSerializerOptions { WriteIndented = true }));
            projectRequest.Cancel(); selectionRequest.Cancel(); searchRequest.Cancel();
            Application.Current.Exit();
        }
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
