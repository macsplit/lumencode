using LumenCode.Core;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage.Pickers;

namespace LumenCode.Windows;

public sealed partial class MainWindow : Window
{
    private readonly EngineClient engine = new(EngineClient.FindExecutable());
    private CancellationTokenSource projectRequest = new();
    private CancellationTokenSource selectionRequest = new();
    private CancellationTokenSource searchRequest = new();
    private readonly List<NavigationTarget> history = [];
    private readonly Dictionary<string, (long Stamp, long Length, JsonElement Data)> cache = new(StringComparer.OrdinalIgnoreCase);
    private int historyIndex = -1, generation, pending;
    private string rootPath = "", selectedPath = "", previewText = "";
    private bool indexReady;
    private JsonElement analysis;
    private sealed record SelectionMeasurement(bool CacheHit, double PreviewReadyMs, double AnalysisReadyMs, double UiApplyMs, double CompletedMs);
    private SelectionMeasurement? lastSelectionMeasurement;
    private sealed record PreviewMeasurement(double SetTextMs, double DisplayMs, double TotalMs);
    private PreviewMeasurement? lastPreviewMeasurement;
    private PreviewDocument previewDocument = new("");
    private int previewPage, previewVersion;
    private string previewPath = "";
    private readonly Microsoft.UI.System.ThemeSettings themeSettings;
    private readonly string settingsFile = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LumenCode", "settings.json");

    public MainWindow()
    {
        InitializeComponent();
        // This is a source viewer. Retaining undo copies of every navigated file
        // wastes memory and adds work to large document replacements.
        Code.Document.UndoLimit = 0;
        themeSettings = Microsoft.UI.System.ThemeSettings.CreateForWindowId(AppWindow.Id);
        themeSettings.Changed += ThemeChanged;
        Title = "LumenCode";
        AppWindow.Resize(new global::Windows.Graphics.SizeInt32(1440, 920));
        AppWindow.Changed += (_, args) =>
        {
            if (args.DidSizeChange && (AppWindow.Size.Width < 1080 || AppWindow.Size.Height < 700))
                AppWindow.Resize(new global::Windows.Graphics.SizeInt32(Math.Max(1080, AppWindow.Size.Width), Math.Max(700, AppWindow.Size.Height)));
        };
        Closed += (_, _) => { themeSettings.Changed -= ThemeChanged; SaveSettings(); projectRequest.Cancel(); selectionRequest.Cancel(); searchRequest.Cancel(); };
        Root.Loaded += Loaded;
        WorkspaceGrid.SizeChanged += (_, _) =>
        {
            // A manually sized preview must still leave room for the overview
            // after shrinking the window. Star sizing handles the default case.
            if (PreviewRow.Height.IsAbsolute)
                PreviewRow.Height = new GridLength(Math.Clamp(PreviewRow.Height.Value, 140, Math.Max(140, WorkspaceGrid.ActualHeight - 226)));
        };
    }

    private async void Loaded(object sender, RoutedEventArgs e)
    {
        Root.Loaded -= Loaded;
        var args = Environment.GetCommandLineArgs();
        if (args.Contains("--gui-audit")) { await RunAuditAsync(args); return; }
        string? path = args.Skip(1).FirstOrDefault(a => Directory.Exists(a) || File.Exists(a));
        if (path is not null) { await LoadProjectAsync(Directory.Exists(path) ? path : Path.GetDirectoryName(path)!, File.Exists(path) ? path : null); return; }
        try
        {
            if (File.Exists(settingsFile))
            {
                using var saved = JsonDocument.Parse(await File.ReadAllTextAsync(settingsFile));
                string root = saved.RootElement.Text("root");
                if (Directory.Exists(root)) await LoadProjectAsync(root, saved.RootElement.Text("file"));
            }
        }
        catch (Exception error) { Status.Text = "Could not restore the previous workspace: " + error.Message; }
    }

    private void SaveSettings()
    {
        if (Environment.GetCommandLineArgs().Contains("--gui-audit")) return;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(settingsFile)!);
            File.WriteAllText(settingsFile, JsonSerializer.Serialize(new { root = rootPath, file = selectedPath }));
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    private void BusyStarted() { pending++; Busy.IsActive = true; }
    private void BusyFinished() { pending = Math.Max(0, pending - 1); Busy.IsActive = pending > 0; }
    private void ShowError(Exception error) => Status.Text = error.Message;

    private async Task LoadProjectAsync(string path, string? initialFile = null)
    {
        projectRequest.Cancel(); selectionRequest.Cancel(); searchRequest.Cancel();
        projectRequest.Dispose(); projectRequest = new();
        int current = ++generation;
        var token = projectRequest.Token;
        rootPath = Path.GetFullPath(path);
        indexReady = false; cache.Clear(); history.Clear(); historyIndex = -1;
        selectedPath = ""; analysis = default;
        Files.RootNodes.Clear(); Overview.ItemsSource = null; SearchBox.Text = "";
        DetailSections.Children.Clear(); DetailTitle.Text = "Details"; DetailSubtitle.Text = ""; Signature.Text = "";
        FileTitle.Text = Path.GetFileName(rootPath); AnalysisSummary.Text = "Select a file to explore its structure.";
        SetPreview("", "Source preview");
        ProjectTitle.Text = rootPath; Title = $"LumenCode — {Path.GetFileName(rootPath)}";
        UpdateHistoryButtons();
        IndexStatus.Text = "Loading project files…";
        BusyStarted();
        try
        {
            var entries = await Task.Run(() => ProjectFiles.Children(rootPath, token), token);
            if (current != generation) return;
            foreach (var entry in entries) Files.RootNodes.Add(Node(entry));
            IndexStatus.Text = "Building project relationships…";
            Status.Text = "Project open. File analysis is available while indexing runs.";
            _ = IndexProjectAsync(current, token);
            if (initialFile is { Length: > 0 } && File.Exists(initialFile)) await SelectFileAsync(new(initialFile));
        }
        catch (OperationCanceledException) { }
        catch (Exception error) { if (current == generation) ShowError(error); }
        finally { BusyFinished(); }
    }

    private async Task IndexProjectAsync(int current, CancellationToken token)
    {
        BusyStarted();
        try
        {
            var stats = await engine.BuildIndexAsync(rootPath, token);
            if (current != generation) return;
            indexReady = true;
            IndexStatus.Text = $"Project index ready · {stats.Text("fileCount", stats.Text("files", ""))} files";
            Status.Text = "Project relationships ready.";
            cache.Clear();
            if (selectedPath.Length > 0) await SelectFileAsync(new(selectedPath), recordHistory: false);
            if (SearchBox.Text.Length > 0) await SearchAsync();
        }
        catch (OperationCanceledException) { }
        catch (Exception error) { if (current == generation) { IndexStatus.Text = "Index unavailable · file analysis still works"; ShowError(error); } }
        finally { BusyFinished(); }
    }

    private static TreeViewNode Node(FileEntry entry) => new() { Content = entry, HasUnrealizedChildren = entry.IsDirectory };

    private async void FolderExpanding(TreeView sender, TreeViewExpandingEventArgs args)
    {
        if (!args.Node.HasUnrealizedChildren || args.Node.Content is not FileEntry entry) return;
        int current = generation;
        var token = projectRequest.Token;
        try
        {
            var entries = await Task.Run(() => ProjectFiles.Children(entry.Path, token), token);
            if (current != generation) return;
            args.Node.Children.Clear();
            foreach (var child in entries) args.Node.Children.Add(Node(child));
            args.Node.HasUnrealizedChildren = false;
            if (entries.Count >= 5000) Status.Text = "Showing the first 5,000 entries in this folder.";
        }
        catch (OperationCanceledException) { }
        catch (Exception error) { ShowError(error); }
    }

    private async void FileInvoked(TreeView sender, TreeViewItemInvokedEventArgs args)
    {
        var node = args.InvokedItem as TreeViewNode;
        var entry = node?.Content as FileEntry ?? args.InvokedItem as FileEntry;
        if (entry is { IsDirectory: false }) await SelectFileAsync(new(entry.Path));
    }

    private async Task SelectFileAsync(NavigationTarget target, bool recordHistory = true)
    {
        var watch = Stopwatch.StartNew();
        if (!File.Exists(target.Path)) { Status.Text = "This file is no longer available."; return; }
        selectionRequest.Cancel(); selectionRequest.Dispose();
        selectionRequest = CancellationTokenSource.CreateLinkedTokenSource(projectRequest.Token);
        var token = selectionRequest.Token;
        int current = generation;
        selectedPath = Path.GetFullPath(target.Path);
        string requested = selectedPath;
        if (recordHistory)
        {
            if (historyIndex < 0 || history[historyIndex] != target)
            {
                if (historyIndex + 1 < history.Count) history.RemoveRange(historyIndex + 1, history.Count - historyIndex - 1);
                history.Add(target); if (history.Count > 50) history.RemoveAt(0);
                historyIndex = history.Count - 1;
            }
            UpdateHistoryButtons();
        }
        FileTitle.Text = Path.GetFileName(requested);
        AnalysisSummary.Text = "Analysing…"; Status.Text = "Analysing " + Path.GetRelativePath(rootPath, requested);
        Overview.ItemsSource = null; DetailSections.Children.Clear(); DetailTitle.Text = "Details"; DetailSubtitle.Text = ""; Signature.Text = "";
        SetPreview("", requested);
        BusyStarted();
        try
        {
            // Show source independently of process startup, indexing and analysis.
            // File opening/decoding also belongs off the dispatcher (network roots
            // and large previews can otherwise block even before the first await).
            var source = await Task.Run(async () =>
            {
                var info = new FileInfo(requested);
                string text = await ReadPreviewAsync(requested, token);
                var document = new PreviewDocument(text);
                return (Text: text, Document: document, Rtf: SourcePreview.Format(document.PageText(0), requested), Stamp: info.LastWriteTimeUtc.Ticks, Length: info.Length);
            }, token);
            if (token.IsCancellationRequested || current != generation || requested != selectedPath) return;
            double applyStart = watch.Elapsed.TotalMilliseconds;
            SetPreview(source.Text, Path.GetRelativePath(rootPath, requested), source.Rtf, source.Document);
            double previewReadyMs = watch.Elapsed.TotalMilliseconds;
            double uiApplyMs = previewReadyMs - applyStart;
            JsonElement data;
            bool cacheHit = cache.TryGetValue(requested, out var saved) && saved.Stamp == source.Stamp && saved.Length == source.Length;
            if (cacheHit) data = saved.Data;
            else data = await engine.AnalyzeAsync(requested, indexReady ? rootPath : null, token);
            double analysisReadyMs = watch.Elapsed.TotalMilliseconds;
            if (token.IsCancellationRequested || current != generation || requested != selectedPath) return;
            // Flatten/format JSON and inspect its size on a worker, not the UI.
            var presentation = await Task.Run(() =>
            {
                var symbols = Payload.Symbols(data).ToArray();
                var rows = symbols.Select(s => DisplayItem.From(s, "Symbols")).ToList();
                foreach (var section in new[] { ("dependencies", "Dependencies"), ("routes", "Routes"), ("quickLinks", "Links"), ("relatedFiles", "Related files"), ("analysisNotices", "Analysis notices") })
                    rows.AddRange(data.Items(section.Item1).Select(s => DisplayItem.From(s, section.Item2)));
                return (Rows: rows, SymbolCount: symbols.Length, Cacheable: cacheHit || data.GetRawText().Length <= 512 * 1024);
            }, token);
            if (token.IsCancellationRequested || current != generation || requested != selectedPath) return;
            applyStart = watch.Elapsed.TotalMilliseconds;
            // A cancelled selection from an older project must never populate
            // the new project's cache with stale relationship data.
            if (!cacheHit && presentation.Cacheable)
            {
                if (cache.Count >= 16) cache.Remove(cache.Keys.First());
                cache[requested] = (source.Stamp, source.Length, data);
            }
            analysis = data;
            Overview.ItemsSource = presentation.Rows;
            AnalysisSummary.Text = string.Join(" · ", new[] { data.Text("language"), data.Text("analysisSourceMode"), "confidence " + data.Text("analysisConfidence"), $"{presentation.SymbolCount} symbols" });
            ShowFileDetails(data);
            Status.Text = indexReady ? "Ready" : "File ready · project indexing continues";
            if (target.Name.Length > 0 || target.Line > 0)
            {
                var symbol = Payload.Symbols(data).FirstOrDefault(s => target.Name.Length > 0 ? s.Text("name") == target.Name : s.Number("line") == target.Line);
                if (symbol.ValueKind == JsonValueKind.Object) Inspect(DisplayItem.From(symbol, "Symbols"));
                else if (target.Line > 0) SelectPreviewLine(target.Line);
            }
            lastSelectionMeasurement = new(cacheHit, previewReadyMs, analysisReadyMs, uiApplyMs + watch.Elapsed.TotalMilliseconds - applyStart, watch.Elapsed.TotalMilliseconds);
        }
        catch (OperationCanceledException) { }
        catch (Exception error) { if (!token.IsCancellationRequested && current == generation) { AnalysisSummary.Text = "Analysis unavailable"; ShowError(error); } }
        finally { BusyFinished(); }
    }

    private static async Task<string> ReadPreviewAsync(string path, CancellationToken token)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 4096, FileOptions.Asynchronous);
        using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
        var buffer = new char[256 * 1024];
        int count = await reader.ReadBlockAsync(buffer.AsMemory(), token);
        string text = new(buffer, 0, count);
        if (text.Contains('\0')) return "Binary file · source preview unavailable.";
        return count == buffer.Length ? text + "\n\n[Preview limited to 256 KiB of text]" : text;
    }

    private void SetPreview(string text, string title, string? rtf = null, PreviewDocument? document = null)
    {
        previewVersion++;
        previewText = text;
        previewDocument = document ?? new(text);
        previewPath = selectedPath;
        PreviewTitle.Text = title;
        // A caller's full-document RTF cannot be used for a single page.
        ApplyPreviewPage(0, document is null && previewDocument.PageCount > 1 ? null : rtf);
    }

    private void ApplyPreviewPage(int page, string? rtf = null)
    {
        var measurement = Stopwatch.StartNew();
        previewPage = page;
        string text = previewDocument.PageText(page);
        PreviewNavigation.Visibility = previewDocument.PageCount > 1 ? Visibility.Visible : Visibility.Collapsed;
        PreviousPreview.IsEnabled = page > 0;
        NextPreview.IsEnabled = page + 1 < previewDocument.PageCount;
        PreviewPosition.Text = $"{page + 1} / {previewDocument.PageCount}";
        Code.IsReadOnly = false;
        Code.Document.BatchDisplayUpdates();
        try
        {
            if (text.Length == 0) Code.Document.SetText(Microsoft.UI.Text.TextSetOptions.None, "");
            else
            {
                // Discard the previous page's runs before importing another
                // RTF page; replacing a populated rich document costs more.
                Code.Document.SetText(Microsoft.UI.Text.TextSetOptions.None, "");
                if (themeSettings.HighContrast)
                {
                    Code.Document.SetText(Microsoft.UI.Text.TextSetOptions.None, text);
                    var range = Code.Document.GetRange(0, 0);
                    range.Expand(Microsoft.UI.Text.TextRangeUnit.Story);
                    range.CharacterFormat.ForegroundColor = new global::Windows.UI.ViewManagement.UISettings().UIElementColor(global::Windows.UI.ViewManagement.UIElementType.WindowText);
                }
                else Code.Document.SetText(Microsoft.UI.Text.TextSetOptions.FormatRtf, rtf ?? SourcePreview.Format(text, previewPath));
            }
        }
        finally
        {
            double textMs = measurement.Elapsed.TotalMilliseconds;
            double displayStart = measurement.Elapsed.TotalMilliseconds;
            try { Code.Document.ApplyDisplayUpdates(); } finally { Code.IsReadOnly = true; }
            lastPreviewMeasurement = new(textMs, measurement.Elapsed.TotalMilliseconds - displayStart, measurement.Elapsed.TotalMilliseconds);
        }
    }

    private void ThemeChanged(Microsoft.UI.System.ThemeSettings sender, object args) => DispatcherQueue.TryEnqueue(() =>
    {
        previewVersion++;
        ApplyPreviewPage(previewPage);
    });

    private async Task ChangePreviewPageAsync(int page)
    {
        if (page < 0 || page >= previewDocument.PageCount) return;
        int version = ++previewVersion;
        var document = previewDocument;
        string path = previewPath;
        string rtf = await Task.Run(() => SourcePreview.Format(document.PageText(page), path));
        if (version != previewVersion || document != previewDocument) return;
        ApplyPreviewPage(page, rtf);
    }

    private async void PreviousPreviewClick(object sender, RoutedEventArgs e) => await ChangePreviewPageAsync(previewPage - 1);
    private async void NextPreviewClick(object sender, RoutedEventArgs e) => await ChangePreviewPageAsync(previewPage + 1);

    private void SelectPreviewLine(int line)
    {
        int offset = previewDocument.LineStart(line);
        int page = previewDocument.PageAt(offset);
        previewVersion++;
        if (page != previewPage) ApplyPreviewPage(page);
        // Work in the displayed page; RichEdit uses one position per paragraph
        // ending whereas the source may use a two-character CRLF pair.
        Code.Document.GetText(Microsoft.UI.Text.TextGetOptions.None, out string documentText);
        // CRLF pairs in the source occupy one RichEdit paragraph position.
        string prefix = previewDocument.Text[previewDocument.Start(page)..Math.Max(previewDocument.Start(page), offset)];
        int start = prefix.Replace("\r\n", "\n").Length;
        int end = documentText.IndexOfAny(['\r', '\n'], start); if (end < 0) end = documentText.Length;
        Code.Document.Selection.SetRange(start, end);
        Code.Document.Selection.ScrollIntoView(Microsoft.UI.Text.PointOptions.Start);
    }

    private void ShowFileDetails(JsonElement data)
    {
        DetailTitle.Text = "File overview"; DetailSubtitle.Text = data.Text("path"); Signature.Text = "";
        DetailSections.Children.Clear();
        AddText("Analysis", $"{data.Text("analysisSourceMode")} · confidence {data.Text("analysisConfidence")}");
        AddLinks("Analysis notices", data.Items("analysisNotices"));
        if (data.TryGetProperty("packageSummary", out var package) && package.ValueKind == JsonValueKind.Object)
        {
            if (package.Text("name").Length > 0) AddText("Package", package.Text("name") + " " + package.Text("version"));
            if (package.TryGetProperty("scripts", out var scripts) && scripts.ValueKind == JsonValueKind.Object)
                foreach (var script in scripts.EnumerateObject()) AddText(script.Name, script.Value.ToString());
        }
        if (data.TryGetProperty("cssSummary", out var css) && css.ValueKind == JsonValueKind.Object)
            foreach (string key in new[] { "matchedClasses", "missingClasses", "unusedClasses", "scriptAppliedClasses" }) AddLinks(key, css.Items(key));
    }

    private void Inspect(DisplayItem item)
    {
        var data = item.Data;
        DetailTitle.Text = item.Title; DetailSubtitle.Text = item.Subtitle; DetailSections.Children.Clear();
        string parameters = string.Join(", ", data.Items("parameters").Select(p => p.Text("text", p.Text("name") + (p.Text("type").Length > 0 ? ": " + p.Text("type") : "")) + (p.Text("default").Length > 0 ? " = " + p.Text("default") : "")));
        string returns = string.Join(" | ", data.Items("returns").Select(r => r.Text("text")));
        Signature.Text = item.Section == "Symbols" ? $"{item.Title}({parameters})" + (returns.Length > 0 ? " → " + returns : "") : "";
        string confidence = data.Text("confidence"), mode = data.Text("sourceMode");
        if (confidence.Length + mode.Length > 0) AddText("Analysis", $"{mode} · confidence {confidence}");
        foreach (var group in new[] { ("members", "Members"), ("calls", "Calls"), ("calledBy", "Called by"), ("declaredIn", "Declared in") }) AddLinks(group.Item2, data.Items(group.Item1));
        if (data.TryGetProperty("definition", out var definition) && definition.ValueKind == JsonValueKind.Object) AddLinks("Definition", [definition]);
        string snippet = data.Text("snippet");
        if (snippet.Length > 0) SetPreview(snippet, item.Title);
        else if (data.Number("line") > 0) SelectPreviewLine(data.Number("line"));
        var destination = Target(data);
        if (destination is not null && (destination.Path != selectedPath || item.Section != "Symbols"))
        {
            var button = new Button { Content = "Navigate to source", HorizontalAlignment = HorizontalAlignment.Left };
            button.Click += async (_, _) => await SelectFileAsync(destination);
            DetailSections.Children.Add(button);
        }
    }

    private NavigationTarget? Target(JsonElement data)
    {
        string path = data.Text("targetPath", data.Text("path", data.Text("sourcePath", selectedPath)));
        if (path.Length == 0 || Uri.TryCreate(path, UriKind.Absolute, out var uri) && !uri.IsFile && !Path.IsPathRooted(path)) return null;
        try { path = Path.GetFullPath(path, rootPath); }
        catch (ArgumentException) { return null; }
        return File.Exists(path) ? new(path, data.Text("targetName", data.Text("name")), data.Number("line")) : null;
    }

    private void AddText(string heading, string text)
    {
        DetailSections.Children.Add(new TextBlock { Text = heading, Style = (Style)Root.Resources["SectionHeading"] });
        DetailSections.Children.Add(new TextBlock { Text = text, IsTextSelectionEnabled = true, TextWrapping = TextWrapping.Wrap });
    }

    private void AddLinks(string heading, IEnumerable<JsonElement> values)
    {
        var entries = values.Take(200).ToArray(); if (entries.Length == 0) return;
        DetailSections.Children.Add(new TextBlock { Text = heading, Style = (Style)Root.Resources["SectionHeading"] });
        foreach (var value in entries)
        {
            if (value.ValueKind != JsonValueKind.Object) { DetailSections.Children.Add(new TextBlock { Text = value.ToString() }); continue; }
            var item = DisplayItem.From(value, heading);
            var button = new Button { Content = new TextBlock { Text = item.Title + (item.Subtitle.Length > 0 ? "\n" + item.Subtitle : ""), TextWrapping = TextWrapping.Wrap }, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Left, Margin = new Thickness(0, 2, 0, 2) };
            button.Click += async (_, _) =>
            {
                if (heading == "Members") Inspect(item with { Section = "Symbols" });
                else if (Target(value) is { } target) await SelectFileAsync(target);
                else Inspect(item);
            };
            DetailSections.Children.Add(button);
        }
    }

    private async Task SearchAsync()
    {
        searchRequest.Cancel(); searchRequest.Dispose(); searchRequest = CancellationTokenSource.CreateLinkedTokenSource(projectRequest.Token);
        var token = searchRequest.Token; int current = generation; string query = SearchBox.Text.Trim();
        Files.Visibility = query.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
        SearchResults.Visibility = query.Length == 0 ? Visibility.Collapsed : Visibility.Visible;
        SearchResults.ItemsSource = null;
        if (query.Length == 0)
        {
            Status.Text = indexReady || rootPath.Length == 0 ? "Ready" : "Project indexing continues";
            return;
        }
        if (!indexReady) { Status.Text = "Search will be available when project indexing finishes."; return; }
        try
        {
            await Task.Delay(180, token);
            var result = await engine.SearchAsync(rootPath, query, token: token);
            if (token.IsCancellationRequested || current != generation) return;
            var rows = result.Items("results").Select(r => DisplayItem.From(r, "Search")).ToArray();
            SearchResults.ItemsSource = rows;
            Status.Text = $"{rows.Length} search results";
        }
        catch (OperationCanceledException) { }
        catch (Exception error) { if (!token.IsCancellationRequested && current == generation) ShowError(error); }
    }

    private async void SearchChanged(object sender, TextChangedEventArgs e) { if (SearchResults is not null) await SearchAsync(); }
    private async void SearchInvoked(object sender, ItemClickEventArgs e) { if (e.ClickedItem is DisplayItem item && Target(item.Data) is { } target) await SelectFileAsync(target); }
    private void OverviewInvoked(object sender, ItemClickEventArgs e) { if (e.ClickedItem is DisplayItem item) Inspect(item); }
    private void UpdateHistoryButtons() { BackButton.IsEnabled = historyIndex > 0; ForwardButton.IsEnabled = historyIndex >= 0 && historyIndex < history.Count - 1; }
    private async void BackClick(object sender, RoutedEventArgs e) { if (historyIndex > 0) { historyIndex--; UpdateHistoryButtons(); await SelectFileAsync(history[historyIndex], false); } }
    private async void ForwardClick(object sender, RoutedEventArgs e) { if (historyIndex + 1 < history.Count) { historyIndex++; UpdateHistoryButtons(); await SelectFileAsync(history[historyIndex], false); } }
    private async void RefreshClick(object sender, RoutedEventArgs e) { if (rootPath.Length > 0) await LoadProjectAsync(rootPath, selectedPath); }
    private async void OpenClick(object sender, RoutedEventArgs e)
    {
        try
        {
            var picker = new FolderPicker(); picker.FileTypeFilter.Add("*");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));
            var folder = await picker.PickSingleFolderAsync();
            if (folder is not null) await LoadProjectAsync(folder.Path);
        }
        catch (Exception error) { ShowError(error); }
    }
    private void EditorClick(object sender, RoutedEventArgs e) { if (File.Exists(selectedPath)) { try { Process.Start(new ProcessStartInfo(selectedPath) { UseShellExecute = true }); } catch (Exception error) { ShowError(error); } } }
    private void ExplorerClick(object sender, RoutedEventArgs e)
    {
        try
        {
            if (File.Exists(selectedPath)) Process.Start(new ProcessStartInfo("explorer.exe", "/select,\"" + selectedPath + "\"") { UseShellExecute = true });
            else if (Directory.Exists(rootPath)) Process.Start(new ProcessStartInfo(rootPath) { UseShellExecute = true });
        }
        catch (Exception error) { ShowError(error); }
    }
    private void CopyClick(object sender, RoutedEventArgs e) { var data = new DataPackage(); data.SetText(previewText); Clipboard.SetContent(data); }
    private void OpenShortcut(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs e) { e.Handled = true; OpenClick(this, new()); }
    private void FindShortcut(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs e) { e.Handled = true; SearchBox.Focus(FocusState.Keyboard); SearchBox.SelectAll(); }
    private void RefreshShortcut(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs e) { e.Handled = true; RefreshClick(this, new()); }
    private void BackShortcut(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs e) { e.Handled = true; BackClick(this, new()); }
    private void ForwardShortcut(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs e) { e.Handled = true; ForwardClick(this, new()); }
    private void FilesSplit(PaneSplitter sender, double delta) => FilesColumn.Width = new GridLength(Math.Clamp(FilesColumn.ActualWidth + delta, 220, WorkspaceGrid.ActualWidth - 550));
    private void OverviewSplit(PaneSplitter sender, double delta) { double width = Math.Clamp(OverviewColumn.ActualWidth + delta, 260, WorkspaceGrid.ActualWidth - FilesColumn.ActualWidth - 280); OverviewColumn.Width = new GridLength(width); DetailColumn.Width = new GridLength(1, GridUnitType.Star); }
    private void PreviewSplit(PaneSplitter sender, double delta) => PreviewRow.Height = new GridLength(Math.Clamp(PreviewRow.ActualHeight - delta, 140, WorkspaceGrid.ActualHeight - 230));
}
