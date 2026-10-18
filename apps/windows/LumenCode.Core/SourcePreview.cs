using System.Text;
using System.Text.RegularExpressions;

namespace LumenCode.Core;

/// <summary>Display-only lexical colouring, using the macOS preview palette.
/// Never supplies language eligibility, symbols, fallbacks or engine facts.</summary>
public static class SourcePreview
{
    // Large previews use the base colour throughout. Importing thousands of RTF
    // colour runs into RichEdit is substantially slower than unformatted text.
    public const int ColourLimit = 64 * 1024;
    private const string Header = @"{\rtf1\ansi\ansicpg1252\uc1\deff0{\fonttbl{\f0\fmodern Consolas;}}{\colortbl;\red224\green230\blue237;\red252\green158\blue79;\red138\green194\blue250;\red143\green214\blue143;\red133\green145\blue161;\red199\green171\blue247;}\viewkind4\pard\f0\fs20\b0\cf1 ";
    private const string Strings = "(?<str>\"(?:\\\\[\\s\\S]|[^\"\\\\])*\"|'(?:\\\\[\\s\\S]|[^'\\\\])*'|`(?:\\\\[\\s\\S]|[^`\\\\])*`)";
    private const string Words = @"(?<keyword>\b(?:function|class|const|let|var|import|export|if|else|switch|case|for|while|return|async|await|public|private|protected|static|def|fn|struct|enum|impl|namespace|using|new|try|catch|actor|protocol|extension|func|guard|throws|throw|where|in|init|deinit|internal|fileprivate|open|true|false|null|nil|None|True|False)\b)|(?<type>\b[A-Z][A-Za-z0-9_]*\b)|(?<number>\b\d+(?:\.\d+)?\b)";
    private static Regex Rules(string comments, bool triple = false) => new(
        comments + "|" + (triple ? "(?<str>\"\"\"[\\s\\S]*?\"\"\"|'''[\\s\\S]*?''')|" : "") + Strings + "|" + Words,
        RegexOptions.CultureInvariant | RegexOptions.NonBacktracking, TimeSpan.FromMilliseconds(50));
    private static readonly Lazy<Regex> CStyle = new(() => Rules(@"(?<comment>//[^\r\n]*|/\*[\s\S]*?\*/)"));
    private static readonly Lazy<Regex> HashStyle = new(() => Rules(@"(?<comment>\#[^\r\n]*)", triple: true));
    private static readonly Lazy<Regex> PhpStyle = new(() => Rules(@"(?<comment>//[^\r\n]*|/\*[\s\S]*?\*/|\#[^\r\n]*)"));
    private static readonly Lazy<Regex> Markup = new(() => Rules(@"(?<comment><!--[\s\S]*?-->)"));
    private static readonly Lazy<Regex> Data = new(() => new Regex(Strings + @"|(?<keyword>\b(?:true|false|null)\b)|(?<number>\b\d+(?:\.\d+)?\b)", RegexOptions.CultureInvariant | RegexOptions.NonBacktracking, TimeSpan.FromMilliseconds(50)));

    public static string Format(string text, string path)
    {
        Regex? rules = text.Length > ColourLimit ? null : Path.GetExtension(path).ToLowerInvariant() switch
        {
            ".swift" or ".js" or ".mjs" or ".cjs" or ".jsx" or ".ts" or ".tsx" or ".c" or ".h" or ".cpp" or ".hpp" or ".cc" or ".m" or ".mm" or ".cs" or ".java" or ".kt" or ".kts" or ".go" or ".rs" or ".css" or ".scss" or ".less" or ".qml" => CStyle.Value,
            ".py" or ".rb" or ".sh" or ".bash" or ".zsh" or ".ps1" => HashStyle.Value,
            ".php" => PhpStyle.Value,
            ".html" or ".htm" or ".xml" or ".xaml" or ".svg" => Markup.Value,
            ".json" => Data.Value,
            _ => null
        };
        var result = new StringBuilder(Header, text.Length * 2 + Header.Length);
        int position = 0;
        if (rules is not null)
        {
            // On timeout, retain all remaining source verbatim in the base
            // colour rather than failing file navigation.
            try
            {
                foreach (Match match in rules.Matches(text))
                {
                    Append(result, text.AsSpan(position, match.Index - position));
                    int colour = match.Groups["comment"].Success ? 5 : match.Groups["str"].Success ? 4 : match.Groups["keyword"].Success ? 2 : match.Groups["type"].Success ? 3 : 6;
                    result.Append("\\cf").Append(colour).Append(' ');
                    Append(result, match.ValueSpan);
                    result.Append(@"\cf1 ");
                    position = match.Index + match.Length;
                }
            }
            catch (RegexMatchTimeoutException) { }
        }
        Append(result, text.AsSpan(position));
        return result.Append('}').ToString();
    }

    private static void Append(StringBuilder result, ReadOnlySpan<char> text)
    {
        for (int i = 0; i < text.Length; i++)
        {
            char c = text[i];
            switch (c)
            {
                case '\\': case '{': case '}': result.Append('\\').Append(c); break;
                case '\r':
                    if (i + 1 < text.Length && text[i + 1] == '\n') i++;
                    result.Append("\\par\n"); break;
                case '\n': result.Append("\\par\n"); break;
                case '\t': result.Append(@"\tab "); break;
                default:
                    if (c is >= ' ' and <= '~') result.Append(c);
                    else result.Append(@"\u").Append(unchecked((short)c)).Append('?');
                    break;
            }
        }
    }
}
