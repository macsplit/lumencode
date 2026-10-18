namespace LumenCode.Core;

/// <summary>Bounds work handed to the native text control, without changing source.</summary>
public sealed class PreviewDocument
{
    public const int PageLimit = 8192;
    public string Text { get; }
    private readonly List<int> starts = [0];
    public int PageCount => starts.Count;

    public PreviewDocument(string text)
    {
        Text = text;
        int start = 0;
        while (text.Length - start > PageLimit)
        {
            int end = start + PageLimit;
            // Prefer whole lines, but still bound minified/generated lines.
            int newline = text.LastIndexOf('\n', end - 1, PageLimit / 2);
            if (newline >= start + PageLimit / 2) end = newline + 1;
            if (end > start && (char.IsHighSurrogate(text[end - 1]) || (text[end - 1] == '\r' && text[end] == '\n'))) end--;
            starts.Add(end);
            start = end;
        }
    }

    public int Start(int page) => starts[page];
    public int End(int page) => page + 1 < starts.Count ? starts[page + 1] : Text.Length;
    public string PageText(int page) => Text[Start(page)..End(page)];
    public int PageAt(int offset)
    {
        int index = starts.BinarySearch(Math.Clamp(offset, 0, Text.Length));
        return index >= 0 ? index : Math.Max(0, ~index - 1);
    }

    public int LineStart(int line)
    {
        int offset = 0;
        for (int current = 1; current < line && offset < Text.Length; current++)
        {
            int end = Text.IndexOfAny(['\r', '\n'], offset);
            if (end < 0) return Text.Length;
            offset = end + 1;
            if (Text[end] == '\r' && offset < Text.Length && Text[offset] == '\n') offset++;
        }
        return offset;
    }
}
