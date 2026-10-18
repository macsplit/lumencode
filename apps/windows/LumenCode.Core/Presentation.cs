using System.Text.Json;

namespace LumenCode.Core;

public static class Payload
{
    public static string Text(this JsonElement value, string name, string fallback = "") =>
        value.ValueKind == JsonValueKind.Object && value.TryGetProperty(name, out var field)
            ? field.ValueKind == JsonValueKind.String ? field.GetString() ?? fallback
              : field.ValueKind is JsonValueKind.Null or JsonValueKind.Undefined ? fallback : field.ToString()
            : fallback;

    public static int Number(this JsonElement value, string name) =>
        value.ValueKind == JsonValueKind.Object && value.TryGetProperty(name, out var field) && field.ValueKind == JsonValueKind.Number && field.TryGetInt32(out int number) ? number : 0;

    public static IEnumerable<JsonElement> Items(this JsonElement value, string name) =>
        value.ValueKind == JsonValueKind.Object && value.TryGetProperty(name, out var field) && field.ValueKind == JsonValueKind.Array
            ? field.EnumerateArray() : [];

    public static IEnumerable<JsonElement> Symbols(JsonElement value)
    {
        foreach (var symbol in value.Items("symbols"))
        {
            yield return symbol;
            foreach (var member in Members(symbol)) yield return member;
        }
    }

    private static IEnumerable<JsonElement> Members(JsonElement parent)
    {
        foreach (var member in parent.Items("members"))
        {
            yield return member;
            foreach (var nested in Members(member)) yield return nested;
        }
    }
}

public sealed record NavigationTarget(string Path, string Name = "", int Line = 0);
public sealed record DisplayItem(string Title, string Subtitle, JsonElement Data, string Section)
{
    public static DisplayItem From(JsonElement value, string section) => new(
        value.Text("name", value.Text("label", value.Text("message", value.Text("path", section)))),
        string.Join(" · ", new[] { value.Text("kind"), value.Text("detail"), value.Number("line") > 0 ? $"line {value.Number("line")}" : "" }.Where(s => s.Length > 0)),
        value, section);
}

public sealed record FileEntry(string Path, string Name, bool IsDirectory)
{
    public override string ToString() => Name;
}

public static class ProjectFiles
{
    // Presentation enumeration only. Language eligibility and analysis are owned by the engine.
    private static readonly HashSet<string> Excluded = new(StringComparer.OrdinalIgnoreCase)
        { ".git", ".svn", ".hg", "node_modules", "vendor", "build", "dist", ".build", ".venv", "venv", "__pycache__", "bin", "obj", ".vs" };

    public static IReadOnlyList<FileEntry> Children(string path, CancellationToken token)
    {
        var result = new List<FileEntry>();
        try
        {
            var options = new EnumerationOptions { IgnoreInaccessible = true, AttributesToSkip = FileAttributes.Hidden | FileAttributes.System | FileAttributes.ReparsePoint };
            foreach (var entry in new DirectoryInfo(path).EnumerateFileSystemInfos("*", options))
            {
                token.ThrowIfCancellationRequested();
                bool directory = entry.Attributes.HasFlag(FileAttributes.Directory);
                if (directory && Excluded.Contains(entry.Name)) continue;
                result.Add(new(entry.FullName, entry.Name, directory));
                if (result.Count >= 5000) break;
            }
        }
        catch (UnauthorizedAccessException) { }
        catch (DirectoryNotFoundException) { }
        return result.OrderByDescending(e => e.IsDirectory).ThenBy(e => e.Name, StringComparer.OrdinalIgnoreCase).ToArray();
    }
}
