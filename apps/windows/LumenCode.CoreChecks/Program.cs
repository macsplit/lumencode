using LumenCode.Core;
using System.Diagnostics;
using System.Text.Json;

if (args.FirstOrDefault() == "--fake")
{
    switch (args[1])
    {
        case "echo": Console.WriteLine(JsonSerializer.Serialize(new { args = args.Skip(2).ToArray() })); return 0;
        case "invalid": Console.WriteLine("not JSON"); return 0;
        case "failure": Console.Error.WriteLine("Deliberate helper failure"); return 7;
        case "overflow":
            using (var stdout = Console.OpenStandardOutput()) { var chunk = new byte[65536]; for (int i = 0; i < 600; i++) stdout.Write(chunk); }
            return 0;
        case "sleep": await Task.Delay(TimeSpan.FromMinutes(1)); return 0;
        case "spawn":
            using (var child = Process.Start(new ProcessStartInfo(Environment.ProcessPath!) { ArgumentList = { "--fake", "sleep" }, UseShellExecute = false, CreateNoWindow = true }))
            { File.WriteAllText(args[2], JsonSerializer.Serialize(new { parent = Environment.ProcessId, child = child!.Id })); await Task.Delay(TimeSpan.FromMinutes(1)); }
            return 0;
    }
}

var results = new List<object>();
async Task Check(string name, Func<Task> action)
{
    try { await action(); results.Add(new { name, passed = true }); }
    catch (Exception error) { results.Add(new { name, passed = false, error = error.ToString() }); }
}
static async Task Expect<T>(Func<Task> action) where T : Exception
{
    try { await action(); } catch (T) { return; }
    throw new InvalidOperationException("Expected " + typeof(T).Name);
}
static bool Running(int pid)
{
    try { using var process = Process.GetProcessById(pid); return !process.HasExited; }
    catch (ArgumentException) { return false; }
}
var client = new EngineClient(Environment.ProcessPath!);
await Check("Preview pages preserve source and Unicode boundaries", () =>
{
    foreach (string text in new[] { "", "short\r\nsource", new string('x', PreviewDocument.PageLimit - 1) + "😀\r\nend", new string('x', PreviewDocument.PageLimit - 1) + "\r\nend", string.Concat(Enumerable.Repeat("café日本語😀\r\n", 26000)) })
    {
        var document = new PreviewDocument(text);
        var pages = Enumerable.Range(0, document.PageCount).Select(document.PageText).ToArray();
        if (string.Concat(pages) != text || pages.Any(p => p.Length > PreviewDocument.PageLimit)) throw new InvalidOperationException("Lost source or unbounded page");
        for (int page = 0; page + 1 < pages.Length; page++)
            if (char.IsHighSurrogate(pages[page][^1]) || (pages[page][^1] == '\r' && pages[page + 1][0] == '\n')) throw new InvalidOperationException("Split Unicode or CRLF");
        for (int line = 1; line <= 3; line++)
        {
            int offset = document.LineStart(line), page = document.PageAt(offset);
            if (offset < document.Start(page) || offset > document.End(page)) throw new InvalidOperationException("Line not on selected page");
        }
    }
    return Task.CompletedTask;
});
await Check("Unicode and quoted arguments preserved", async () =>
{
    string[] values = ["C:\\Project with spaces\\café\\日本語.cs", "a\"b", "trailing\\", ""];
    var response = await client.RunAsync(["--fake", "echo", .. values], TimeSpan.FromSeconds(5));
    if (!response.GetProperty("args").EnumerateArray().Select(v => v.GetString()).SequenceEqual(values)) throw new InvalidOperationException("Argument corruption");
});
await Check("Invalid JSON rejected", () => Expect<JsonException>(async () => await client.RunAsync(["--fake", "invalid"], TimeSpan.FromSeconds(5))));
await Check("Nonzero exit rejected", () => Expect<IOException>(async () => await client.RunAsync(["--fake", "failure"], TimeSpan.FromSeconds(5))));
await Check("Output bounded", () => Expect<InvalidDataException>(async () => await client.RunAsync(["--fake", "overflow"], TimeSpan.FromSeconds(5))));
await Check("Timeout terminates helper", () => Expect<TimeoutException>(async () => await client.RunAsync(["--fake", "sleep"], TimeSpan.FromMilliseconds(200))));
await Check("Missing engine reported", () => Expect<FileNotFoundException>(async () => await new EngineClient(Path.Combine(Path.GetTempPath(), Guid.NewGuid()+".exe")).RunAsync([], TimeSpan.FromSeconds(1))));
await Check("Cancellation terminates worker descendants", async () =>
{
    string record = Path.Combine(Path.GetTempPath(), "lumencode-processes-" + Guid.NewGuid() + ".json");
    using var cancellation = new CancellationTokenSource();
    try
    {
        var operation = client.RunAsync(["--fake", "spawn", record], TimeSpan.FromSeconds(10), cancellation.Token);
        var deadline = DateTime.UtcNow.AddSeconds(5);
        while (!File.Exists(record) && DateTime.UtcNow < deadline) await Task.Delay(20);
        if (!File.Exists(record)) throw new InvalidOperationException("Worker did not start");
        // The writer may have created the file but not finished its contents yet.
        await Task.Delay(50);
        using var data = JsonDocument.Parse(await File.ReadAllTextAsync(record));
        int parent = data.RootElement.Number("parent"), child = data.RootElement.Number("child");
        cancellation.Cancel();
        await Expect<OperationCanceledException>(async () => await operation);
        for (int i=0; i<25 && (Running(parent) || Running(child)); i++) await Task.Delay(20);
        if (Running(parent) || Running(child)) throw new InvalidOperationException("Orphaned helper process");
    }
    finally { cancellation.Cancel(); if (File.Exists(record)) File.Delete(record); }
});

if (args.Length > 0 && File.Exists(args[0]))
{
    string helper = args[0];
    await Check("Shared engine Unicode CRLF indexing", async () =>
    {
        string root = Path.Combine(Path.GetTempPath(), "LumenCode space café 日本語 " + Guid.NewGuid());
        Directory.CreateDirectory(root);
        try
        {
            string file = Path.Combine(root, "helper.js");
            await File.WriteAllTextAsync(file, "function helper() { return 42; }\r\nfunction wrapper() { return helper(); }\r\nmodule.exports = { wrapper };\r\n");
            await File.WriteAllTextAsync(Path.Combine(root, "main.js"), "const { wrapper } = require('./helper');\r\nfunction run() { return wrapper(); }\r\n");
            var native = new EngineClient(helper, Path.Combine(root, "cache"));
            var stats = await native.BuildIndexAsync(root);
            if (stats.Number("failed") != 0 || stats.Number("files") < 2 || stats.Number("crossFileEdges") < 1) throw new InvalidOperationException(stats.GetRawText());
            var analysis = await native.AnalyzeAsync(file, root);
            var wrapper = Payload.Symbols(analysis).First(s => s.Text("name") == "wrapper");
            if (!wrapper.Items("calls").Any(c => c.Text("name") == "helper")) throw new InvalidOperationException("Missing same-file call");
            if (!wrapper.Items("calledBy").Any(c => c.Text("name") == "run")) throw new InvalidOperationException("Missing cross-file caller");
            var search = await native.SearchAsync(root, "wrapper");
            if (!search.Items("results").Any(r => r.Text("name").Contains("wrapper"))) throw new InvalidOperationException("Missing indexed symbol");
        }
        finally { Directory.Delete(root, true); }
    });
}
string json = JsonSerializer.Serialize(new { passed = results.All(r => JsonSerializer.SerializeToElement(r).GetProperty("passed").GetBoolean()), results }, new JsonSerializerOptions { WriteIndented = true });
Console.WriteLine(json);
if (args.Length > 1) await File.WriteAllTextAsync(args[1], json);
return results.All(r => JsonSerializer.SerializeToElement(r).GetProperty("passed").GetBoolean()) ? 0 : 1;
