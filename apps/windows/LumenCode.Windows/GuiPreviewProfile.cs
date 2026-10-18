using LumenCode.Core;
using System.Diagnostics;
using System.Text.Json;

namespace LumenCode.Windows;

public sealed partial class MainWindow
{
    private async Task CheckPreviewPagingAsync(Action<string, bool> check)
    {
        string source = string.Join("\r\n", Enumerable.Range(1, 600).Select(n => $"const line{n} = 'café 日本語 😀';"));
        SetPreview(source, "Paged source");
        check("Large preview has bounded pages and complete copy source", previewDocument.PageCount > 1 && previewText == source && PreviewNavigation.Visibility == Microsoft.UI.Xaml.Visibility.Visible);
        SelectPreviewLine(599);
        Code.Document.Selection.GetText(Microsoft.UI.Text.TextGetOptions.None, out string selected);
        check("Line navigation opens later page with CRLF and Unicode", previewPage > 0 && selected == "const line599 = 'café 日本語 😀';");
        await ChangePreviewPageAsync(0);
        check("Previous source page restores beginning", previewPage == 0 && !PreviousPreview.IsEnabled && NextPreview.IsEnabled);
        await ChangePreviewPageAsync(previewDocument.PageCount - 1);
        check("Last source page disables Next", PreviousPreview.IsEnabled && !NextPreview.IsEnabled);
        Task stale = ChangePreviewPageAsync(0);
        SetPreview("new selection", "Replacement source");
        await stale;
        Code.Document.GetText(Microsoft.UI.Text.TextGetOptions.None, out string actual);
        check("Pending page cannot overwrite a newer selection", Normalize(actual) == "new selection" && PreviewNavigation.Visibility == Microsoft.UI.Xaml.Visibility.Collapsed);
    }

    private async Task ProfilePreviewsAsync(string output, Action<string, bool> check)
    {
        var samples = new List<object>();
        // Repeated assignment-only samples separate RichEdit work from engine
        // startup. The queued probe also includes dispatcher/layout delay; it is
        // not a measurement of pixels reaching the monitor.
        foreach (var item in new[] { ("coloured", 32 * 1024), ("plain-128k", 128 * 1024), ("plain-256k", 256 * 1024), ("long-line", 128 * 1024) })
        {
            string unit = item.Item1 == "long-line" ? "café日本語😀 " : "const sample = 'café日本語😀'; // source\r\n";
            string text = string.Concat(Enumerable.Repeat(unit, item.Item2 / unit.Length));
            var document = new PreviewDocument(text);
            string rtf = await Task.Run(() => SourcePreview.Format(document.PageText(0), "profile.js"));
            for (int repeat = 0; repeat < 3; repeat++)
            {
                SetPreview("", "Profiling");
                await Task.Delay(100);
                var watch = Stopwatch.StartNew();
                var dispatched = new TaskCompletionSource<double>();
                DispatcherQueue.TryEnqueue(() => dispatched.TrySetResult(watch.Elapsed.TotalMilliseconds));
                SetPreview(text, item.Item1, rtf, document);
                var assignment = lastPreviewMeasurement;
                double dispatcherDelayMs = await dispatched.Task;
                await Task.Delay(100);
                Code.Document.GetText(Microsoft.UI.Text.TextGetOptions.None, out string actual);
                check($"Profile {item.Item1} round trip {repeat}", Normalize(actual) == Normalize(document.PageText(0)) && previewText == text);
                check($"Profile {item.Item1} read-only {repeat}", Code.IsReadOnly && !Code.Document.CanUndo());
                samples.Add(new { workload = item.Item1, chars = text.Length, repeat, assignment, dispatcherDelayMs });
            }
            // Walk real controls through every page, including Unicode/CRLF
            // boundaries and the final page, rather than checking slices alone.
            for (int page = 0; page < document.PageCount; page++)
            {
                await ChangePreviewPageAsync(page);
                Code.Document.GetText(Microsoft.UI.Text.TextGetOptions.None, out string actual);
                check($"Profile {item.Item1} page {page + 1}", Normalize(actual) == Normalize(document.PageText(page)) && Code.IsReadOnly);
                samples.Add(new { workload = item.Item1, operation = "page", page, assignment = lastPreviewMeasurement });
                await Task.Delay(30);
            }
            SelectPreviewLine(2);
            check($"Profile {item.Item1} line navigation", previewPage == document.PageAt(document.LineStart(2)));
            await File.WriteAllTextAsync(Path.Combine(output, "preview-profile.json"), JsonSerializer.Serialize(new { samples, pageLimit = PreviewDocument.PageLimit, limitations = "Three assignment samples per workload on this VM; dispatcher probe includes queued work, not rendered-frame or physical-input latency." }, new JsonSerializerOptions { WriteIndented = true }));
        }
    }

    private static string Normalize(string text) => text.TrimEnd('\r', '\n').Replace("\r\n", "\n").Replace('\r', '\n');
}
