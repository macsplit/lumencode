#include "symbolparser.h"
#include "httpclients.h"
#include "weblinks.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <cctype>
#include <optional>
#include <climits>
#include <cstring>
#include <algorithm>
#include <functional>
#include <tree_sitter/api.h>

extern "C" {
const TSLanguage *tree_sitter_swift(void);
const TSLanguage *tree_sitter_javascript(void);
const TSLanguage *tree_sitter_typescript(void);
const TSLanguage *tree_sitter_tsx(void);
const TSLanguage *tree_sitter_php(void);
const TSLanguage *tree_sitter_css(void);
const TSLanguage *tree_sitter_rust(void);
const TSLanguage *tree_sitter_python(void);
const TSLanguage *tree_sitter_java(void);
const TSLanguage *tree_sitter_c_sharp(void);
const TSLanguage *tree_sitter_go(void);
const TSLanguage *tree_sitter_cpp(void);
}

static QVariantList toVariantList(const QStringList &values)
{
    QVariantList result;
    for (const QString &value : values) {
        result.append(value);
    }
    return result;
}

constexpr qint64 kMaxParsableFileBytes = 2 * 1024 * 1024;
constexpr qint64 kMaxAuxiliaryFileBytes = 512 * 1024;
constexpr qint64 kMinifiedDetectionBytes = 16 * 1024;

static bool shouldSkipFileBySize(const QFileInfo &info, qint64 limit)
{
    return info.exists() && info.isFile() && info.size() > limit;
}

static bool languageSupportsMinifiedSkip(const QString &language)
{
    return language == QStringLiteral("script")
        || language == QStringLiteral("jsx")
        || language == QStringLiteral("ts")
        || language == QStringLiteral("tsx")
        || language == QStringLiteral("css");
}

static bool looksLikeMinifiedSource(const QString &path, const QString &language, const QString &text)
{
    if (!languageSupportsMinifiedSkip(language) || text.size() < kMinifiedDetectionBytes) {
        return false;
    }

    const QString fileName = QFileInfo(path).fileName().toLower();
    if (fileName.contains(QStringLiteral(".min.")) || fileName.endsWith(QStringLiteral("-min.js"))
        || fileName.endsWith(QStringLiteral("-min.css"))) {
        return true;
    }

    const QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.isEmpty()) {
        return false;
    }

    int longestLine = 0;
    int nonEmptyLines = 0;
    int totalNonEmptyChars = 0;
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        ++nonEmptyLines;
        longestLine = qMax(longestLine, trimmed.size());
        totalNonEmptyChars += trimmed.size();
    }

    if (nonEmptyLines == 0) {
        return false;
    }

    const double averageLineLength = static_cast<double>(totalNonEmptyChars) / static_cast<double>(nonEmptyLines);
    return longestLine >= 4000
        || (longestLine >= 2000 && averageLineLength >= 350.0)
        || (nonEmptyLines <= 8 && averageLineLength >= 1200.0);
}

static QString tsType(TSNode node)
{
    // ts_node_type() dereferences the node's subtree, so a null node (e.g. a
    // missing optional field such as the value of `let x: T`) would crash.
    if (ts_node_is_null(node)) {
        return {};
    }
    return QString::fromUtf8(ts_node_type(node));
}

static QString nodeText(TSNode node, const QByteArray &source)
{
    if (ts_node_is_null(node)) {
        return {};
    }
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (end <= start || static_cast<int>(end) > source.size()) {
        return {};
    }
    return QString::fromUtf8(source.constData() + start, static_cast<int>(end - start));
}

static int nodeLine(TSNode node)
{
    return static_cast<int>(ts_node_start_point(node).row) + 1;
}

static TSNode fieldNode(TSNode node, const char *fieldName)
{
    return ts_node_child_by_field_name(node, fieldName, strlen(fieldName));
}

static TSLanguage *languageForName(const QString &language)
{
    if (language == QStringLiteral("php")) {
        return const_cast<TSLanguage *>(tree_sitter_php());
    }
    if (language == QStringLiteral("swift")) {
        return const_cast<TSLanguage *>(tree_sitter_swift());
    }
    if (language == QStringLiteral("css")) {
        return const_cast<TSLanguage *>(tree_sitter_css());
    }
    if (language == QStringLiteral("tsx")) {
        return const_cast<TSLanguage *>(tree_sitter_tsx());
    }
    if (language == QStringLiteral("rust")) {
        return const_cast<TSLanguage *>(tree_sitter_rust());
    }
    if (language == QStringLiteral("python")) {
        return const_cast<TSLanguage *>(tree_sitter_python());
    }
    if (language == QStringLiteral("java")) {
        return const_cast<TSLanguage *>(tree_sitter_java());
    }
    if (language == QStringLiteral("csharp")) {
        return const_cast<TSLanguage *>(tree_sitter_c_sharp());
    }
    if (language == QStringLiteral("jsx") || language == QStringLiteral("script")) {
        return const_cast<TSLanguage *>(tree_sitter_javascript());
    }
    if (language == QStringLiteral("ts")) {
        return const_cast<TSLanguage *>(tree_sitter_typescript());
    }
    if (language == QStringLiteral("go")) {
        return const_cast<TSLanguage *>(tree_sitter_go());
    }
    if (language == QStringLiteral("cpp")) {
        return const_cast<TSLanguage *>(tree_sitter_cpp());
    }
    return nullptr;
}

static QString nodeSnippet(TSNode node, const QByteArray &source, int maxLines = 10)
{
    if (ts_node_is_null(node)) {
        return {};
    }
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (end <= start || static_cast<int>(end) > source.size()) {
        return {};
    }
    const QString text = QString::fromUtf8(source.constData() + start, static_cast<int>(end - start));
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.size() <= maxLines) {
        return text;
    }
    return lines.mid(0, maxLines).join(QLatin1Char('\n')) + QStringLiteral("\n...");
}

// ---------------------------------------------------------------------------
// Branch-scoped AST repair
//
// Tree-sitter recovers from a syntax error by wrapping the damaged region in an
// ERROR node, and on real files that region often swallows every declaration
// after the error (one half-typed line can hide the rest of a class). Instead of
// giving up on the AST for the whole file, the repair pass finds the lines the
// parser flags, blanks them (byte-for-byte, keeping newlines so every offset and
// line number stays identical) and re-parses, greedily, while that strictly
// shrinks the error. The resulting tree stays authoritative for everything
// except the blanked lines, so the damage is limited to the branch that
// contains them.
//
// The AST parse functions read the repaired bytes through a thread-local
// override, while snippets keep coming from the original text (offsets match).
// ---------------------------------------------------------------------------

static thread_local const QByteArray *t_astSourceOverride = nullptr;

class ScopedAstSourceOverride
{
public:
    explicit ScopedAstSourceOverride(const QByteArray &source)
        : m_previous(t_astSourceOverride)
    {
        t_astSourceOverride = &source;
    }
    ~ScopedAstSourceOverride() { t_astSourceOverride = m_previous; }
    ScopedAstSourceOverride(const ScopedAstSourceOverride &) = delete;
    ScopedAstSourceOverride &operator=(const ScopedAstSourceOverride &) = delete;

private:
    const QByteArray *m_previous;
};

// Parse the file currently being analysed. Honours the repair override when it
// is active and describes the same bytes (same length); auxiliary parses of
// *other* files must call ts_parser_parse_string directly.
static TSTree *parseAnalysedSource(TSParser *parser, const QByteArray &source)
{
    const QByteArray *effective = &source;
    if (t_astSourceOverride && t_astSourceOverride->size() == source.size()) {
        effective = t_astSourceOverride;
    }
    return ts_parser_parse_string(parser, nullptr, effective->constData(), effective->size());
}

struct AstErrorScore
{
    int errorNodes = 0;
    qint64 errorBytes = 0;
    int declarations = 0; // declaration-like nodes the tree still forms
    bool operator<(const AstErrorScore &other) const
    {
        if (errorBytes != other.errorBytes) {
            return errorBytes < other.errorBytes;
        }
        return errorNodes < other.errorNodes;
    }
    bool isClean() const { return errorNodes == 0; }
};

// A repair candidate must shrink the error; among those, the one that keeps
// the most declaration structure wins. That stops the greedy pass from
// "fixing" a file by blanking an intact `class Foo {` header, which also makes
// the ERROR smaller but dissolves the type.
static bool isBetterRepair(const AstErrorScore &candidate, const AstErrorScore &best, bool bestIsCurrent)
{
    if (bestIsCurrent) {
        return candidate < best;
    }
    if (candidate.declarations != best.declarations) {
        return candidate.declarations > best.declarations;
    }
    return candidate < best;
}

static bool isDeclarationLikeType(const char *type)
{
    return std::strstr(type, "declaration") || std::strstr(type, "definition")
        || std::strstr(type, "_item") || std::strstr(type, "method") || std::strstr(type, "function")
        || std::strstr(type, "class");
}

// Per-grammar lookup table: is node symbol N declaration-like? Built once per
// language so repair trials do not string-compare every node.
static const QVector<bool> &declarationSymbolTable(const TSLanguage *language)
{
    static QMutex mutex;
    static QHash<const TSLanguage *, QVector<bool>> tables;
    QMutexLocker locker(&mutex);
    auto it = tables.find(language);
    if (it == tables.end()) {
        const uint32_t symbolCount = ts_language_symbol_count(language);
        QVector<bool> table(static_cast<int>(symbolCount), false);
        for (uint32_t symbol = 0; symbol < symbolCount; ++symbol) {
            const char *name = ts_language_symbol_name(language, static_cast<TSSymbol>(symbol));
            table[static_cast<int>(symbol)] = name && isDeclarationLikeType(name);
        }
        it = tables.insert(language, table);
    }
    return *it;
}

static int countDeclarationNodes(TSNode node, const QVector<bool> &table)
{
    int total = 0;
    const TSSymbol symbol = ts_node_symbol(node);
    if (symbol < table.size() && table.at(symbol) && ts_node_is_named(node)) {
        total = 1;
    }
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t index = 0; index < count; ++index) {
        total += countDeclarationNodes(ts_node_named_child(node, index), table);
    }
    return total;
}

// Candidate rows are split in two tiers. Primary: rows of stray tokens inside
// an ERROR node (unnamed leaves, nested ERROR/MISSING nodes) other than the
// ERROR node's own first row, plus MISSING positions - these are where the
// parse actually went wrong. Secondary: the first row of each ERROR node,
// which is frequently an intact declaration header (e.g. `class Foo {`) whose
// node could not be completed; blanking it is a last resort.
struct AstRepairCandidates
{
    QList<int> primary;
    QList<int> secondary;
    QList<QPair<int, int>> errorSpans; // rows covered by ERROR nodes
};

static void collectAstErrors(TSNode node, AstErrorScore &score, AstRepairCandidates &candidates)
{
    if (ts_node_is_null(node) || !ts_node_has_error(node)) {
        return;
    }
    if (ts_node_is_missing(node)) {
        ++score.errorNodes;
        score.errorBytes += 1;
        const int row = static_cast<int>(ts_node_start_point(node).row);
        candidates.primary.append(row);
        if (row > 0) {
            candidates.primary.append(row - 1);
        }
        return;
    }
    if (ts_node_is_error(node)) {
        ++score.errorNodes;
        score.errorBytes += qMax<qint64>(1, ts_node_end_byte(node) - ts_node_start_byte(node));
        const int startRow = static_cast<int>(ts_node_start_point(node).row);
        candidates.secondary.append(startRow);
        candidates.errorSpans.append({startRow, static_cast<int>(ts_node_end_point(node).row)});
        const uint32_t childCount = ts_node_child_count(node);
        for (uint32_t index = 0; index < childCount; ++index) {
            TSNode child = ts_node_child(node, index);
            const int row = static_cast<int>(ts_node_start_point(child).row);
            if (ts_node_is_error(child) || ts_node_is_missing(child) || !ts_node_is_named(child)
                || ts_node_child_count(child) == 0) {
                candidates.primary.append(row);
            }
        }
    }
    const uint32_t childCount = ts_node_child_count(node);
    for (uint32_t index = 0; index < childCount; ++index) {
        collectAstErrors(ts_node_child(node, index), score, candidates);
    }
}

static AstErrorScore scoreAstSource(TSParser *parser, const QByteArray &source, AstRepairCandidates *candidateRows)
{
    AstErrorScore score;
    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    if (!tree) {
        score.errorNodes = 1;
        score.errorBytes = source.size();
        return score;
    }
    AstRepairCandidates rows;
    collectAstErrors(ts_tree_root_node(tree), score, rows);
    score.declarations = countDeclarationNodes(ts_tree_root_node(tree), declarationSymbolTable(ts_tree_language(tree)));
    ts_tree_delete(tree);
    if (candidateRows) {
        *candidateRows = rows;
    }
    return score;
}

static QVector<int> lineStartOffsets(const QByteArray &source)
{
    QVector<int> starts{0};
    for (int index = 0; index < source.size(); ++index) {
        if (source.at(index) == '\n') {
            starts.append(index + 1);
        }
    }
    return starts;
}

static QByteArray blankSourceRow(QByteArray source, const QVector<int> &starts, int row)
{
    if (row < 0 || row >= starts.size()) {
        return source;
    }
    const int begin = starts.at(row);
    const int end = row + 1 < starts.size() ? starts.at(row + 1) - 1 : source.size();
    for (int index = begin; index < end; ++index) {
        const char ch = source.at(index);
        if (ch != '\n' && ch != '\r') {
            source[index] = ' ';
        }
    }
    return source;
}

static int rowIndentWidth(const QByteArray &source, const QVector<int> &starts, int row, bool *blank)
{
    const int begin = starts.at(row);
    const int end = row + 1 < starts.size() ? starts.at(row + 1) - 1 : source.size();
    int width = 0;
    for (int index = begin; index < end; ++index) {
        const char ch = source.at(index);
        if (ch == ' ') {
            ++width;
        } else if (ch == '\t') {
            width += 8 - (width % 8);
        } else if (ch == '\r') {
            continue;
        } else {
            *blank = false;
            return width;
        }
    }
    *blank = true;
    return width;
}

// For indentation-scoped languages the "branch" owned by a line is the line
// plus every following line indented deeper than it. Blanking only a damaged
// header would otherwise re-home its body under the previous declaration.
static QList<int> indentedBlockRows(const QByteArray &source, const QVector<int> &starts, int row)
{
    QList<int> rows{row};
    bool blank = false;
    const int headerIndent = rowIndentWidth(source, starts, row, &blank);
    if (blank) {
        return rows;
    }
    for (int next = row + 1; next < starts.size(); ++next) {
        bool nextBlank = false;
        const int indent = rowIndentWidth(source, starts, next, &nextBlank);
        if (nextBlank) {
            rows.append(next);
            continue;
        }
        if (indent <= headerIndent) {
            break;
        }
        rows.append(next);
    }
    while (rows.size() > 1) {
        bool trailingBlank = false;
        rowIndentWidth(source, starts, rows.constLast(), &trailingBlank);
        if (!trailingBlank) {
            break;
        }
        rows.removeLast();
    }
    return rows;
}

// Lines that look damaged regardless of what the parser reports: tree-sitter
// often blames an intact enclosing line (a `describe('x', () => {` header)
// for an unclosed call or string further down. Suspects are
//  - a line opening more brackets than it closes whose next non-blank line
//    is not indented deeper (a real multi-line construct indents), and
//  - a line with an unterminated ' or " string.
static QSet<int> textuallySuspectRows(const QByteArray &source, const QVector<int> &starts)
{
    QSet<int> suspects;
    for (int row = 0; row < starts.size(); ++row) {
        const int begin = starts.at(row);
        const int end = row + 1 < starts.size() ? starts.at(row + 1) - 1 : source.size();
        const QByteArray line = source.mid(begin, end - begin);
        if (line.contains("\"\"\"") || line.contains("'''") || line.contains('`')) {
            continue; // multi-line string syntax: not judged per line
        }
        int net = 0;
        char quote = 0;
        for (int index = 0; index < line.size(); ++index) {
            const char ch = line.at(index);
            if (quote) {
                if (ch == '\\') {
                    ++index;
                } else if (ch == quote) {
                    quote = 0;
                }
                continue;
            }
            if (ch == '/' && index + 1 < line.size() && (line.at(index + 1) == '/' || line.at(index + 1) == '*')) {
                break;
            }
            if (ch == '#' && (index == 0 || line.at(index - 1) == ' ')) {
                break; // Python/shell comment (also C preprocessor lines, harmlessly)
            }
            if (ch == '"' || ch == '\'') {
                // an apostrophe after a letter (Rust lifetimes, English text) is not a quote
                const bool apostrophe = ch == '\'' && index > 0 && std::isalnum(static_cast<unsigned char>(line.at(index - 1)));
                if (!apostrophe) {
                    quote = ch;
                }
            } else if (ch == '(' || ch == '[' || ch == '{') {
                ++net;
            } else if (ch == ')' || ch == ']' || ch == '}') {
                --net;
            }
        }
        if (quote != 0) {
            suspects.insert(row);
            continue;
        }
        if (net <= 0) {
            continue;
        }
        bool blank = false;
        const int indent = rowIndentWidth(source, starts, row, &blank);
        for (int next = row + 1; next < starts.size(); ++next) {
            bool nextBlank = false;
            const int nextIndent = rowIndentWidth(source, starts, next, &nextBlank);
            if (nextBlank) {
                continue;
            }
            if (nextIndent <= indent) {
                suspects.insert(row);
            }
            break;
        }
    }
    return suspects;
}

// C/C++ macro pre-pass (same length, so offsets and line numbers hold): the
// grammar cannot expand macros, and export / attribute / Qt macros are what
// trips it on most real files. Rewritten before parsing:
//  - Qt: Q_OBJECT, Q_PROPERTY(...), QML_ELEMENT, Q_INVOKABLE, ... (blanked,
//    with their argument list); `signals:` / `Q_SIGNALS:` -> `public`, bare
//    `slots` / `Q_SLOTS` blanked; `emit` / `Q_EMIT` blanked; value macros
//    (Q_NULLPTR, Q_FUNC_INFO) -> `0`;
//  - export / attribute macros (FOO_EXPORT, FMT_API, FMT_CONSTEXPR20,
//    FOO_DEPRECATED(...), ...) blanked;
//  - wrapping export macros (CJSON_PUBLIC(type) name(...)) unwrapped;
//  - *_BEGIN_NAMESPACE / *_END_NAMESPACE lines blanked.
static QByteArray blankCppMacros(const QByteArray &source)
{
    QByteArray out = source;
    const int size = out.size();
    auto isIdentChar = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; };
    auto blank = [&](int from, int to) {
        for (int index = from; index < to && index < size; ++index) {
            if (out.at(index) != '\n') {
                out[index] = ' ';
            }
        }
    };
    auto put = [&](int at, const char *text) {
        for (int index = 0; text[index] && at + index < size; ++index) {
            out[at + index] = text[index];
        }
    };
    auto matchingParen = [&](int open) {
        int depth = 0;
        for (int index = open; index < size && index < open + 4000; ++index) {
            if (out.at(index) == '(') {
                ++depth;
            } else if (out.at(index) == ')' && --depth == 0) {
                return index;
            }
        }
        return -1;
    };
    static const QRegularExpression attributeSuffix(QStringLiteral(
        R"(^[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)*_(?:EXPORT|NO_EXPORT|API|DECL|INLINE|FORCE_INLINE|ALWAYS_INLINE|NOINLINE|CONSTEXPR\d*|CONSTEVAL|NOEXCEPT|NODISCARD|DEPRECATED|DEPRECATED_EXPORT|NORETURN|VISIBILITY|HIDDEN|LOCAL|CDECL|STDCALL|UNUSED|MAYBE_UNUSED|FALLTHROUGH|NO_UNIQUE_ADDRESS)$)"));
    static const QRegularExpression wrapperSuffix(QStringLiteral(R"(^[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)*_(?:PUBLIC|API|EXPORT|EXTERN|DECLSPEC)$)"));
    static const QRegularExpression qtMacro(QStringLiteral(R"(^(?:Q|QML|K)_[A-Z0-9_]+$)"));
    static const QRegularExpression namespaceMacro(QStringLiteral(R"(^[A-Z0-9_]*_(?:BEGIN|END)_NAMESPACE[A-Z0-9_]*$|^QT_(?:BEGIN|END)_[A-Z_]+$)"));
    static const QSet<QByteArray> valueMacros = {"Q_NULLPTR", "Q_FUNC_INFO", "Q_INT64_C", "Q_UINT64_C", "QT_VERSION_CHECK"};

    bool lineStart = true;
    int index = 0;
    while (index < size) {
        const char ch = out.at(index);
        // Preprocessor lines, comments and literals are left alone.
        if (lineStart && (ch == ' ' || ch == '\t')) {
            ++index;
            continue;
        }
        if (lineStart && ch == '#') {
            while (index < size && out.at(index) != '\n') {
                if (out.at(index) == '\\' && index + 1 < size && out.at(index + 1) == '\n') {
                    ++index;
                }
                ++index;
            }
            continue;
        }
        lineStart = ch == '\n';
        if (ch == '/' && index + 1 < size && out.at(index + 1) == '/') {
            while (index < size && out.at(index) != '\n') {
                ++index;
            }
            continue;
        }
        if (ch == '/' && index + 1 < size && out.at(index + 1) == '*') {
            const int end = out.indexOf("*/", index + 2);
            index = end < 0 ? size : end + 2;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            ++index;
            while (index < size && out.at(index) != ch && out.at(index) != '\n') {
                index += out.at(index) == '\\' ? 2 : 1;
            }
            ++index;
            continue;
        }
        if (!isIdentChar(ch) || (index > 0 && isIdentChar(out.at(index - 1)))) {
            ++index;
            continue;
        }
        int end = index;
        while (end < size && isIdentChar(out.at(end))) {
            ++end;
        }
        const QByteArray word = out.mid(index, end - index);
        const QString name = QString::fromLatin1(word);
        int after = end;
        while (after < size && (out.at(after) == ' ' || out.at(after) == '\t')) {
            ++after;
        }
        const bool hasArgs = after < size && out.at(after) == '(';

        if (valueMacros.contains(word)) {
            const int close = hasArgs ? matchingParen(after) : end - 1;
            if (close >= 0) {
                blank(index, close + 1);
                put(index, "0");
                index = close + 1;
                continue;
            }
        }
        if (word == "signals" || word == "Q_SIGNALS") {
            if (after < size && out.at(after) == ':') {
                blank(index, end);
                put(index, "public");
            } else {
                blank(index, end); // `public Q_SIGNALS:`
            }
            index = end;
            continue;
        }
        if (word == "slots" || word == "Q_SLOTS" || word == "emit" || word == "Q_EMIT") {
            // `public slots:` / `emit changed();` - only in those positions.
            const bool accessLabel = after < size && out.at(after) == ':';
            const bool emitStatement = (word == "emit" || word == "Q_EMIT") && after < size && isIdentChar(out.at(after));
            if (accessLabel || emitStatement) {
                blank(index, end);
            }
            index = end;
            continue;
        }
        if (namespaceMacro.match(name).hasMatch()) {
            const int close = hasArgs ? matchingParen(after) : end - 1;
            blank(index, (close >= 0 ? close : end - 1) + 1);
            index = end;
            continue;
        }
        if (qtMacro.match(name).hasMatch() || attributeSuffix.match(name).hasMatch()) {
            const int close = hasArgs ? matchingParen(after) : -1;
            blank(index, close >= 0 ? close + 1 : end);
            index = close >= 0 ? close + 1 : end;
            continue;
        }
        if (hasArgs && wrapperSuffix.match(name).hasMatch()) {
            // CJSON_PUBLIC(cJSON *) cJSON_Parse(...) -> cJSON * cJSON_Parse(...)
            const int close = matchingParen(after);
            int next = close + 1;
            while (next < size && (out.at(next) == ' ' || out.at(next) == '\t')) {
                ++next;
            }
            if (close > after && next < size && (isIdentChar(out.at(next)) || out.at(next) == '*' || out.at(next) == '&')
                && !out.mid(after, close - after).contains(';')) {
                blank(index, after + 1);
                out[close] = ' ';
                index = after + 1;
                continue;
            }
        }
        index = end;
    }
    return out;
}

// A block whose closer was deleted shows up as an indentation drop: a line,
// not itself starting with a closer, indented no deeper than the line that
// opened a still-open block. The closers that block needs (the reverse of
// its unmatched openers, e.g. `})` for `it('x', () => {`) are written over
// the end of that line's indentation - same length, so offsets hold.
struct AstInsertion
{
    int row = 0; // the row that receives the closers
    int offset = -1; // byte where they are written; -1 when they do not fit
    QByteArray closers;
    int openRow = 0; // the row that opened the unclosed block
};

// The ways to write an insertion's closers back: as they are, and followed
// by `;` where the grammar needs a separator before the next declaration on
// the same line (Swift, Go). Each variant must fit in the indentation.
static QList<QByteArray> closerWriteVariants(const QByteArray &source, const AstInsertion &insertion)
{
    QList<QByteArray> variants;
    if (insertion.offset < 0) {
        return variants;
    }
    for (const QByteArray &closers : {insertion.closers, insertion.closers.endsWith(';') ? QByteArray() : insertion.closers + ';'}) {
        if (closers.isEmpty()) {
            continue;
        }
        const int at = insertion.offset + insertion.closers.size() - closers.size();
        if (at < 0) {
            continue;
        }
        QByteArray trial = source;
        bool room = true;
        for (int index = 0; index < closers.size() && room; ++index) {
            room = trial.at(at + index) == ' ';
            trial[at + index] = closers.at(index);
        }
        const int lineStart = source.lastIndexOf('\n', insertion.offset) + 1;
        if (room && at >= lineStart) {
            variants.append(trial);
        }
    }
    return variants;
}

static QList<AstInsertion> closerInsertionCandidates(const QByteArray &source, const QVector<int> &starts)
{
    struct Open { int row; int indent; QByteArray openers; };
    QList<AstInsertion> candidates;
    QVector<Open> stack;
    auto closerFor = [](char opener) { return opener == '(' ? ')' : opener == '[' ? ']' : '}'; };
    for (int row = 0; row < starts.size() && candidates.size() < 16; ++row) {
        const int begin = starts.at(row);
        const int end = row + 1 < starts.size() ? starts.at(row + 1) - 1 : source.size();
        bool blank = false;
        const int indent = rowIndentWidth(source, starts, row, &blank);
        if (blank) {
            continue;
        }
        int first = begin;
        while (first < end && (source.at(first) == ' ' || source.at(first) == '\t')) {
            ++first;
        }
        const char lead = source.at(first);
        if (lead == '#') {
            continue; // preprocessor
        }
        const bool startsWithCloser = lead == '}' || lead == ')' || lead == ']';
        // Indentation drop past a still-open block: that block lost its closer.
        if (!startsWithCloser && !stack.isEmpty() && indent <= stack.constLast().indent
            && stack.constLast().row < row) {
            QByteArray closers;
            for (int index = stack.constLast().openers.size() - 1; index >= 0; --index) {
                closers.append(closerFor(stack.constLast().openers.at(index)));
            }
            // Closers ending in `)` / `]` close an expression, which needs a
            // separator before the next statement on the same line.
            if (closers.endsWith(')') || closers.endsWith(']')) {
                closers.append(';');
            }
            const int room = first - begin; // indentation bytes available
            const bool fits = !closers.isEmpty() && room >= closers.size() && !source.mid(begin, room).contains('\t');
            candidates.append({row, fits ? first - closers.size() : -1, closers, stack.constLast().row});
            stack.removeLast(); // assume it closed here, keep scanning
        }
        // Track this line's brackets (strings and // comments skipped).
        QByteArray lineOpeners;
        char quote = 0;
        for (int index = first; index < end; ++index) {
            const char ch = source.at(index);
            if (quote) {
                if (ch == '\\') {
                    ++index;
                } else if (ch == quote) {
                    quote = 0;
                }
                continue;
            }
            if (ch == '/' && index + 1 < end && (source.at(index + 1) == '/' || source.at(index + 1) == '*')) {
                break;
            }
            if (ch == '"' || ch == '\'' || ch == '`') {
                quote = ch;
            } else if (ch == '(' || ch == '[' || ch == '{') {
                lineOpeners.append(ch);
            } else if (ch == ')' || ch == ']' || ch == '}') {
                if (!lineOpeners.isEmpty()) {
                    lineOpeners.chop(1);
                } else if (!stack.isEmpty()) {
                    // Closes (part of) an earlier line's openers.
                    stack.last().openers.chop(1);
                    if (stack.constLast().openers.isEmpty()) {
                        stack.removeLast();
                    }
                }
            }
        }
        if (!lineOpeners.isEmpty()) {
            stack.append({row, indent, lineOpeners});
        }
    }
    return candidates;
}

struct AstRepairResult
{
    QByteArray source;
    QList<int> blankedLines; // 1-based
    bool clean = false;
};

static AstRepairResult repairSourceForAst(const QByteArray &original, TSLanguage *language,
                                          bool indentationBlocks = false)
{
    constexpr int kMaxBlankedLines = 12;
    constexpr int kMaxCandidatesPerRound = 10;
    // Deterministic budget (parse attempts), so results do not depend on
    // machine load; the time cap is only a safety net for pathological input.
    // Each trial re-parses the whole file, so large files get fewer trials
    // (80 up to ~48 KB, down to 8 for ~480 KB and above).
    const int kMaxTrialParses = qBound(8, static_cast<int>(80LL * 48 * 1024 / qMax<qsizetype>(1, original.size())), 80);
    constexpr qint64 kBudgetMs = 1500;
    int trialParses = 0;

    AstRepairResult result;
    result.source = original;
    if (!language) {
        return result;
    }
    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, language)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return result;
    }

    QElapsedTimer timer;
    timer.start();
    const QVector<int> starts = lineStartOffsets(original);
    const QSet<int> suspectRows = textuallySuspectRows(original, starts);
    const QList<AstInsertion> insertions = closerInsertionCandidates(original, starts);
    constexpr int kMaxInsertionTrials = 12;
    int insertionTrialsUsed = 0;
    QSet<int> insertedRows;
    constexpr int kMaxSuspectTrials = 12;
    int suspectTrialsUsed = 0;
    AstRepairCandidates candidates;
    AstErrorScore current = scoreAstSource(parser, result.source, &candidates);
    QSet<int> blanked;
    // Many scattered errors mean grammar noise (unexpanded macros and the
    // like), not a local edit: repair would not converge, so leave the file
    // to the AST + heuristic merge instead of paying for trial parses.
    constexpr int kMaxInitialErrorNodes = 25;
    if (current.errorNodes > kMaxInitialErrorNodes) {
        ts_parser_delete(parser);
        return result;
    }
    int stagnantRounds = 0;

    auto normalizeRows = [&](const QList<int> &rows) {
        QList<int> ordered;
        for (int row : rows) {
            if (row >= 0 && row < starts.size() && !blanked.contains(row) && !ordered.contains(row)) {
                ordered.append(row);
            }
        }
        std::sort(ordered.begin(), ordered.end());
        return ordered;
    };

    while (!current.isClean() && blanked.size() < kMaxBlankedLines && timer.elapsed() < kBudgetMs
           && trialParses < kMaxTrialParses) {
        QList<int> bestRows;
        AstErrorScore bestScore = current;
        AstRepairCandidates bestCandidates;
        QByteArray bestSource;

        auto tryTier = [&](const QList<int> &tierRows, bool decisiveOnly = false) {
            QList<int> ordered = normalizeRows(tierRows);
            if (ordered.size() > kMaxCandidatesPerRound) {
                ordered = ordered.mid(0, kMaxCandidatesPerRound);
            }
            for (int row : std::as_const(ordered)) {
                const QList<int> rows = indentationBlocks ? indentedBlockRows(result.source, starts, row)
                                                          : QList<int>{row};
                QByteArray trial = result.source;
                for (int blankRow : rows) {
                    trial = blankSourceRow(trial, starts, blankRow);
                }
                if (trial == result.source) {
                    continue;
                }
                if (++trialParses > kMaxTrialParses) {
                    return;
                }
                AstRepairCandidates trialCandidates;
                const AstErrorScore trialScore = scoreAstSource(parser, trial, &trialCandidates);
                // Textual suspects must be decisive (a clean parse); a partial
                // gain would steer the greedy pass away from the parser's own,
                // better candidates.
                const bool decisive = trialScore.isClean();
                if ((!decisiveOnly || decisive) && trialScore < current
                    && isBetterRepair(trialScore, bestScore, bestRows.isEmpty())) {
                    bestScore = trialScore;
                    bestRows = rows;
                    bestCandidates = trialCandidates;
                    bestSource = trial;
                }
                if (timer.elapsed() >= kBudgetMs) {
                    return;
                }
            }
        };

        // A lost closer (indentation drop past an open block): writing the
        // closers back counts only if the file then parses cleanly.
        for (const AstInsertion &insertion : insertions) {
            if (insertionTrialsUsed >= kMaxInsertionTrials || !bestRows.isEmpty()) {
                break;
            }
            if (insertedRows.contains(insertion.row) || blanked.contains(insertion.row)) {
                continue;
            }
            // Either write the closers back, or - when they do not fit -
            // dissolve the unclosed block by blanking the line that opened it
            // (its body then belongs to the enclosing block; only the damaged
            // block itself is lost).
            QList<QPair<QByteArray, int>> trials; // source, row reported as damaged
            for (const QByteArray &variant : closerWriteVariants(result.source, insertion)) {
                trials.append({variant, insertion.row});
            }
            if (!blanked.contains(insertion.openRow)) {
                // Indentation-scoped languages lose the header's whole block, or
                // its body would be re-homed under the previous declaration.
                QByteArray trial = result.source;
                const QList<int> rows = indentationBlocks ? indentedBlockRows(result.source, starts, insertion.openRow)
                                                          : QList<int>{insertion.openRow};
                for (int row : rows) {
                    trial = blankSourceRow(trial, starts, row);
                }
                trials.append({trial, insertion.openRow});
            }
            for (const auto &trial : std::as_const(trials)) {
                if (insertionTrialsUsed >= kMaxInsertionTrials) {
                    break;
                }
                ++insertionTrialsUsed;
                ++trialParses;
                AstRepairCandidates trialCandidates;
                const AstErrorScore trialScore = scoreAstSource(parser, trial.first, &trialCandidates);
                if (trialScore.isClean()) {
                    bestScore = trialScore;
                    bestRows = {trial.second};
                    bestCandidates = trialCandidates;
                    bestSource = trial.first;
                    insertedRows.insert(insertion.row);
                    break;
                }
            }
        }
        // Textual suspects first, but only one that makes the file parse
        // cleanly counts (tree-sitter often blames an intact enclosing header
        // for an unclosed call or string further down); otherwise the parser's
        // own candidates as before.
        {
            int firstError = INT_MAX;
            int lastError = -1;
            for (const auto &span : std::as_const(candidates.errorSpans)) {
                firstError = qMin(firstError, span.first);
                lastError = qMax(lastError, span.second);
            }
            // The parser notices damage after it happens: look inside the
            // error spans and up to 30 rows before the first one, nearest first.
            QList<int> suspectsInErrors;
            for (int row : std::as_const(suspectRows)) {
                if (!blanked.contains(row) && row >= firstError - 30 && row <= lastError) {
                    suspectsInErrors.append(row);
                }
            }
            std::sort(suspectsInErrors.begin(), suspectsInErrors.end(), [&](int left, int right) {
                const int leftDistance = qAbs(left - firstError);
                const int rightDistance = qAbs(right - firstError);
                return leftDistance != rightDistance ? leftDistance < rightDistance : left < right;
            });
            const int suspectTrials = qMin(6, kMaxSuspectTrials - suspectTrialsUsed);
            if (bestRows.isEmpty() && suspectTrials > 0 && !suspectsInErrors.isEmpty()) {
                const QList<int> tier = suspectsInErrors.mid(0, suspectTrials);
                suspectTrialsUsed += tier.size();
                tryTier(tier, true);
            }
        }
        if (bestRows.isEmpty()) {
            tryTier(candidates.primary + candidates.secondary);
        }
        if (bestRows.isEmpty()) {
            break;
        }
        for (int row : std::as_const(bestRows)) {
            blanked.insert(row);
        }
        // Stop when rounds keep shaving only a sliver off the error.
        if (bestScore.errorBytes * 20 > current.errorBytes * 19 && !bestScore.isClean()) {
            if (++stagnantRounds >= 3) {
                result.source = bestSource;
                current = bestScore;
                break;
            }
        } else {
            stagnantRounds = 0;
        }
        result.source = bestSource;
        current = bestScore;
        candidates = bestCandidates;
    }

    ts_parser_delete(parser);
    for (int row : std::as_const(blanked)) {
        result.blankedLines.append(row + 1);
    }
    std::sort(result.blankedLines.begin(), result.blankedLines.end());
    result.clean = current.isClean();
    if (!result.clean && result.blankedLines.isEmpty()) {
        result.source = original;
    }
    return result;
}

// A clean parse can still be wrong: grammars that allow nested functions and
// types (Swift, Go closures, Rust items) quietly nest every following
// declaration inside a block whose `}` was lost. The tell is indentation: a
// declaration nested in another declaration but starting at the same (or a
// shallower) column than it.
static int countMisnestedDeclarations(TSNode node, const QVector<bool> &table, int ancestorColumn, int ancestorRow)
{
    int total = 0;
    const TSSymbol symbol = ts_node_symbol(node);
    const bool declaration = symbol < table.size() && table.at(symbol) && ts_node_is_named(node);
    int column = ancestorColumn;
    int row = ancestorRow;
    if (declaration) {
        const TSPoint start = ts_node_start_point(node);
        if (ancestorColumn >= 0 && static_cast<int>(start.row) != ancestorRow
            && static_cast<int>(start.column) <= ancestorColumn) {
            ++total;
        }
        column = static_cast<int>(start.column);
        row = static_cast<int>(start.row);
    }
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t index = 0; index < count; ++index) {
        total += countMisnestedDeclarations(ts_node_named_child(node, index), table, column, row);
    }
    return total;
}

static int misnestedDeclarations(TSParser *parser, const QByteArray &source, bool *clean)
{
    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    if (!tree) {
        *clean = false;
        return INT_MAX;
    }
    const TSNode root = ts_tree_root_node(tree);
    *clean = !ts_node_has_error(root);
    const int count = countMisnestedDeclarations(root, declarationSymbolTable(ts_tree_language(tree)), -1, -1);
    ts_tree_delete(tree);
    return count;
}

// For a clean parse with misnested declarations: write back lost closers
// (see closerInsertionCandidates) and keep the first that still parses
// cleanly and removes the misnesting.
static AstRepairResult repairMisnestingForAst(const QByteArray &original, TSLanguage *language)
{
    AstRepairResult result;
    result.source = original;
    result.clean = true;
    if (!language) {
        return result;
    }
    const QVector<int> starts = lineStartOffsets(original);
    const QList<AstInsertion> insertions = closerInsertionCandidates(original, starts);
    if (insertions.isEmpty()) {
        return result;
    }
    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, language)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return result;
    }
    bool clean = false;
    int current = misnestedDeclarations(parser, original, &clean);
    int trials = 0;
    QByteArray source = original;
    for (const AstInsertion &insertion : insertions) {
        if (current == 0 || trials >= 6) {
            break;
        }
        for (const QByteArray &trial : closerWriteVariants(source, insertion)) {
            ++trials;
            bool trialClean = false;
            const int misnested = misnestedDeclarations(parser, trial, &trialClean);
            if (trialClean && misnested < current) {
                source = trial;
                current = misnested;
                result.blankedLines.append(insertion.row + 1);
                break;
            }
        }
    }
    ts_parser_delete(parser);
    if (!result.blankedLines.isEmpty()) {
        result.source = source;
    }
    return result;
}

static QString rustDependencyLabel(const QString &target)
{
    const QString trimmed = target.trimmed();
    if (trimmed.isEmpty()) {
        return trimmed;
    }

    const int groupIndex = trimmed.indexOf(QStringLiteral("::{"));
    QString prefix = groupIndex >= 0 ? trimmed.left(groupIndex) : trimmed;
    const int aliasIndex = prefix.indexOf(QStringLiteral(" as "));
    if (aliasIndex >= 0) {
        prefix = prefix.mid(aliasIndex + 4).trimmed();
    }
    const QStringList parts = prefix.split(QStringLiteral("::"), Qt::SkipEmptyParts);
    return parts.isEmpty() ? prefix : parts.constFirst().trimmed();
}

static TSNode firstAncestorOfType(TSNode node, std::initializer_list<const char *> types)
{
    TSNode current = node;
    while (!ts_node_is_null(current)) {
        const QString currentType = tsType(current);
        for (const char *type : types) {
            if (currentType == QLatin1String(type)) {
                return current;
            }
        }
        current = ts_node_parent(current);
    }
    return TSNode{};
}

static QString nodeValueText(TSNode node, const QByteArray &source)
{
    const QString raw = nodeText(node, source).trimmed();
    if (raw.size() >= 2) {
        const QChar first = raw.front();
        const QChar last = raw.back();
        if ((first == QLatin1Char('"') && last == QLatin1Char('"'))
            || (first == QLatin1Char('\'') && last == QLatin1Char('\''))
            || (first == QLatin1Char('`') && last == QLatin1Char('`'))) {
            return raw.mid(1, raw.size() - 2);
        }
    }
    return raw;
}

static QString firstIdentifier(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral(R"(([A-Za-z_][A-Za-z0-9_]*))"));
    const QRegularExpressionMatch match = pattern.match(text);
    return match.hasMatch() ? match.captured(1) : QString();
}

static QString firstVariableName(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral(R"(\$([A-Za-z_][A-Za-z0-9_]*))"));
    const QRegularExpressionMatch match = pattern.match(text);
    return match.hasMatch() ? QStringLiteral("$") + match.captured(1) : QString();
}

static QString lastIdentifier(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral(R"(([A-Za-z_][A-Za-z0-9_]*)\s*$)"));
    const QRegularExpressionMatch match = pattern.match(text.trimmed());
    return match.hasMatch() ? match.captured(1) : QString();
}

static QString symbolKey(const QVariantMap &symbol)
{
    return QStringLiteral("%1|%2|%3")
        .arg(symbol.value(QStringLiteral("kind")).toString(),
             symbol.value(QStringLiteral("name")).toString())
        .arg(symbol.value(QStringLiteral("line")).toInt());
}

static QVariantMap relationFromSymbol(const QVariantMap &symbol)
{
    QVariantMap relation;
    relation.insert(QStringLiteral("kind"), symbol.value(QStringLiteral("kind")).toString());
    relation.insert(QStringLiteral("name"), symbol.value(QStringLiteral("name")).toString());
    relation.insert(QStringLiteral("line"), symbol.value(QStringLiteral("line")).toInt());
    relation.insert(QStringLiteral("detail"), symbol.value(QStringLiteral("detail")).toString());
    relation.insert(QStringLiteral("snippet"), symbol.value(QStringLiteral("snippet")).toString());
    relation.insert(QStringLiteral("sourceMode"), symbol.value(QStringLiteral("sourceMode")).toString());
    relation.insert(QStringLiteral("confidence"), symbol.value(QStringLiteral("confidence")).toString());
    relation.insert(QStringLiteral("calls"), QVariantList{});
    relation.insert(QStringLiteral("calledBy"), QVariantList{});
    return relation;
}

static QVariantMap withProvenance(QVariantMap item, const QString &sourceMode, const QString &confidence)
{
    if (item.value(QStringLiteral("sourceMode")).toString().isEmpty()) {
        item.insert(QStringLiteral("sourceMode"), sourceMode);
    }
    if (item.value(QStringLiteral("confidence")).toString().isEmpty()) {
        item.insert(QStringLiteral("confidence"), confidence);
    }
    return item;
}

static QVariantMap makeAnalysisNotice(const QString &severity, const QString &message)
{
    QVariantMap notice;
    notice.insert(QStringLiteral("severity"), severity);
    notice.insert(QStringLiteral("message"), message);
    return notice;
}

static void appendParserAnalysisNotice(QVariantMap &analysis,
                                       const QString &severity,
                                       const QString &message,
                                       bool partial = true)
{
    QVariantList notices = analysis.value(QStringLiteral("analysisNotices")).toList();
    for (const QVariant &entry : std::as_const(notices)) {
        const QVariantMap existing = entry.toMap();
        if (existing.value(QStringLiteral("severity")).toString() == severity
            && existing.value(QStringLiteral("message")).toString() == message) {
            if (partial) {
                analysis.insert(QStringLiteral("analysisPartial"), true);
            }
            return;
        }
    }

    notices.append(makeAnalysisNotice(severity, message));
    analysis.insert(QStringLiteral("analysisNotices"), notices);
    if (partial) {
        analysis.insert(QStringLiteral("analysisPartial"), true);
    }
}

static QVariantList annotateFlatEntriesWithProvenance(const QVariantList &entries,
                                                      const QString &sourceMode,
                                                      const QString &confidence)
{
    QVariantList annotated;
    annotated.reserve(entries.size());
    for (const QVariant &entry : entries) {
        annotated.append(withProvenance(entry.toMap(), sourceMode, confidence));
    }
    return annotated;
}

static QVariantList annotateSymbolsWithProvenance(const QVariantList &symbols,
                                                  const QString &sourceMode,
                                                  const QString &confidence)
{
    QVariantList annotated;
    annotated.reserve(symbols.size());
    for (const QVariant &entry : symbols) {
        QVariantMap symbol = withProvenance(entry.toMap(), sourceMode, confidence);
        symbol.insert(QStringLiteral("members"),
                      annotateSymbolsWithProvenance(symbol.value(QStringLiteral("members")).toList(),
                                                    sourceMode, confidence));

        auto annotateRelations = [&](const QString &field) {
            QVariantList relations;
            for (const QVariant &relationEntry : symbol.value(field).toList()) {
                relations.append(withProvenance(relationEntry.toMap(), sourceMode, confidence));
            }
            symbol.insert(field, relations);
        };
        annotateRelations(QStringLiteral("calls"));
        annotateRelations(QStringLiteral("calledBy"));
        annotated.append(symbol);
    }
    return annotated;
}

static QVariantMap annotateAnalysisWithProvenance(QVariantMap result,
                                                  const QString &sourceMode,
                                                  const QString &baseConfidence)
{
    const bool astErrors = result.value(QStringLiteral("analysisHasAstErrors")).toBool();
    QString confidence = baseConfidence;
    if (confidence.isEmpty()) {
        if (sourceMode == QStringLiteral("ast")) {
            confidence = astErrors ? QStringLiteral("medium") : QStringLiteral("high");
        } else if (sourceMode == QStringLiteral("heuristic")) {
            confidence = QStringLiteral("medium");
        } else if (sourceMode == QStringLiteral("recovered")) {
            confidence = QStringLiteral("low");
        } else {
            confidence = QStringLiteral("low");
        }
    }

    result.insert(QStringLiteral("analysisSourceMode"), sourceMode);
    result.insert(QStringLiteral("analysisConfidence"), confidence);
    result.insert(QStringLiteral("symbols"),
                  annotateSymbolsWithProvenance(result.value(QStringLiteral("symbols")).toList(),
                                               sourceMode, confidence));
    result.insert(QStringLiteral("dependencies"),
                  annotateFlatEntriesWithProvenance(result.value(QStringLiteral("dependencies")).toList(),
                                                    sourceMode, confidence));
    result.insert(QStringLiteral("routes"),
                  annotateFlatEntriesWithProvenance(result.value(QStringLiteral("routes")).toList(),
                                                    sourceMode, confidence));
    result.insert(QStringLiteral("quickLinks"),
                  annotateFlatEntriesWithProvenance(result.value(QStringLiteral("quickLinks")).toList(),
                                                    sourceMode, confidence));
    result.insert(QStringLiteral("relatedFiles"),
                  annotateFlatEntriesWithProvenance(result.value(QStringLiteral("relatedFiles")).toList(),
                                                    sourceMode, confidence));
    result.remove(QStringLiteral("_recoveryLineRanges"));
    return result;
}

static int relationKindPriority(const QString &kind)
{
    if (kind == QStringLiteral("function")
        || kind == QStringLiteral("method")
        || kind == QStringLiteral("constructor")
        || kind == QStringLiteral("initializer")
        || kind == QStringLiteral("hook")
        || kind == QStringLiteral("component")) {
        return 0;
    }
    if (kind == QStringLiteral("class")
        || kind == QStringLiteral("struct")
        || kind == QStringLiteral("enum")
        || kind == QStringLiteral("protocol")
        || kind == QStringLiteral("trait")
        || kind == QStringLiteral("interface")
        || kind == QStringLiteral("type")) {
        return 1;
    }
    if (kind == QStringLiteral("module")) {
        return 2;
    }
    if (kind == QStringLiteral("variable")) {
        return 3;
    }
    if (kind == QStringLiteral("property")) {
        return 4;
    }
    return 5;
}

static QString bestRelationTargetKey(const QStringList &candidateKeys,
                                     const QHash<QString, QVariantMap> &byKey)
{
    QString bestKey;
    int bestPriority = std::numeric_limits<int>::max();

    for (const QString &candidateKey : candidateKeys) {
        const QVariantMap candidate = byKey.value(candidateKey);
        const int priority = relationKindPriority(candidate.value(QStringLiteral("kind")).toString());
        if (priority < bestPriority) {
            bestPriority = priority;
            bestKey = candidateKey;
        }
    }

    return bestKey;
}

static void collectSymbolsByKey(const QVariantList &symbols,
                                QHash<QString, QVariantMap> &byKey,
                                QHash<QString, QStringList> &keysByName)
{
    for (const QVariant &entry : symbols) {
        const QVariantMap symbol = entry.toMap();
        const QString key = symbolKey(symbol);
        const QString name = symbol.value(QStringLiteral("name")).toString();
        if (!key.isEmpty() && !name.isEmpty()) {
            byKey.insert(key, symbol);
            keysByName[name].append(key);
        }
        collectSymbolsByKey(symbol.value(QStringLiteral("members")).toList(), byKey, keysByName);
    }
}

static bool snippetsLookCompatible(const QString &primarySnippet, const QString &secondarySnippet);

static void appendUniqueRelation(QHash<QString, QVariantList> &edgeMap,
                                 const QString &ownerKey,
                                 const QVariantMap &relation,
                                 const QString &detail)
{
    QVariantMap adjusted = relation;
    adjusted.insert(QStringLiteral("detail"), detail);
    QVariantList list = edgeMap.value(ownerKey);
    for (const QVariant &entry : std::as_const(list)) {
        const QVariantMap existing = entry.toMap();
        if (existing.value(QStringLiteral("kind")).toString() == adjusted.value(QStringLiteral("kind")).toString()
            && existing.value(QStringLiteral("name")).toString() == adjusted.value(QStringLiteral("name")).toString()) {
            const int existingLine = existing.value(QStringLiteral("line")).toInt();
            const int adjustedLine = adjusted.value(QStringLiteral("line")).toInt();
            if (existingLine > 0 && adjustedLine > 0 && qAbs(existingLine - adjustedLine) <= 1) {
                return;
            }
            if (snippetsLookCompatible(existing.value(QStringLiteral("snippet")).toString(),
                                       adjusted.value(QStringLiteral("snippet")).toString())) {
                return;
            }
        }
    }
    list.append(adjusted);
    edgeMap.insert(ownerKey, list);
}

static QVariantList applyRelationsToSymbols(const QVariantList &symbols,
                                           const QHash<QString, QVariantList> &callsByKey,
                                           const QHash<QString, QVariantList> &calledByByKey)
{
    QVariantList result;
    result.reserve(symbols.size());
    for (const QVariant &entry : symbols) {
        QVariantMap symbol = entry.toMap();
        const QString key = symbolKey(symbol);
        symbol.insert(QStringLiteral("members"),
                      applyRelationsToSymbols(symbol.value(QStringLiteral("members")).toList(),
                                              callsByKey,
                                              calledByByKey));
        if (callsByKey.contains(key)) {
            symbol.insert(QStringLiteral("calls"), callsByKey.value(key));
        }
        if (calledByByKey.contains(key)) {
            symbol.insert(QStringLiteral("calledBy"), calledByByKey.value(key));
        }
        result.append(symbol);
    }
    return result;
}

static bool snippetsLookCompatible(const QString &primarySnippet, const QString &secondarySnippet)
{
    const QString left = primarySnippet.trimmed();
    const QString right = secondarySnippet.trimmed();
    if (left.isEmpty() || right.isEmpty()) {
        return true;
    }
    return left == right || left.contains(right) || right.contains(left);
}

static QVariantList mergeUniqueRelations(const QVariantList &primary, const QVariantList &secondary)
{
    QHash<QString, QVariantList> edgeMap;
    const QString ownerKey = QStringLiteral("__merge__");
    for (const QVariant &entry : primary) {
        const QVariantMap relation = entry.toMap();
        appendUniqueRelation(edgeMap, ownerKey, relation, relation.value(QStringLiteral("detail")).toString());
    }
    for (const QVariant &entry : secondary) {
        const QVariantMap relation = entry.toMap();
        appendUniqueRelation(edgeMap, ownerKey, relation, relation.value(QStringLiteral("detail")).toString());
    }
    return edgeMap.value(ownerKey);
}

static QString flatEntryMergeKey(const QVariantMap &entry)
{
    const QString target = entry.value(QStringLiteral("target")).toString();
    const QString type = entry.value(QStringLiteral("type")).toString();
    const QString path = entry.value(QStringLiteral("path")).toString();
    const QString label = entry.value(QStringLiteral("label")).toString();
    const QString name = entry.value(QStringLiteral("name")).toString();
    const QString method = entry.value(QStringLiteral("method")).toString();
    const int line = entry.value(QStringLiteral("line")).toInt();
    return QStringLiteral("%1|%2|%3|%4|%5|%6")
        .arg(type, target, path, label, name.isEmpty() ? method : name, QString::number(line));
}

static QVariantList mergeUniqueFlatEntries(const QVariantList &primary, const QVariantList &secondary)
{
    QVariantList merged = primary;
    QSet<QString> seen;
    for (const QVariant &entry : std::as_const(merged)) {
        seen.insert(flatEntryMergeKey(entry.toMap()));
    }
    for (const QVariant &entry : secondary) {
        const QString key = flatEntryMergeKey(entry.toMap());
        if (!seen.contains(key)) {
            merged.append(entry);
            seen.insert(key);
        }
    }
    return merged;
}

static QVariantMap mergeSymbolData(QVariantMap primary, const QVariantMap &secondary);

static int compatibleSymbolIndex(const QVariantList &symbols, const QVariantMap &candidate)
{
    const QString candidateKey = symbolKey(candidate);
    const QString candidateKind = candidate.value(QStringLiteral("kind")).toString();
    const QString candidateName = candidate.value(QStringLiteral("name")).toString();
    const int candidateLine = candidate.value(QStringLiteral("line")).toInt();
    const QString candidateSnippet = candidate.value(QStringLiteral("snippet")).toString();

    for (int i = 0; i < symbols.size(); ++i) {
        const QVariantMap existing = symbols.at(i).toMap();
        if (symbolKey(existing) == candidateKey && !candidateKey.isEmpty()) {
            return i;
        }

        if (existing.value(QStringLiteral("kind")).toString() != candidateKind
            || existing.value(QStringLiteral("name")).toString() != candidateName
            || candidateKind.isEmpty()
            || candidateName.isEmpty()) {
            continue;
        }

        const int existingLine = existing.value(QStringLiteral("line")).toInt();
        if (existingLine > 0 && candidateLine > 0) {
            const int lineDelta = qAbs(existingLine - candidateLine);
            if (lineDelta <= 1) {
                return i;
            }
            if (lineDelta > 3) {
                continue;
            }
        }

        if (existingLine <= 0 || candidateLine <= 0) {
            if (snippetsLookCompatible(existing.value(QStringLiteral("snippet")).toString(), candidateSnippet)) {
                return i;
            }
            continue;
        }

        if (snippetsLookCompatible(existing.value(QStringLiteral("snippet")).toString(), candidateSnippet)) {
            return i;
        }
    }

    return -1;
}

static QVariantList mergeSymbolLists(const QVariantList &primary, const QVariantList &secondary)
{
    QVariantList merged = primary;

    for (const QVariant &entry : secondary) {
        const QVariantMap symbol = entry.toMap();
        const int index = compatibleSymbolIndex(merged, symbol);
        if (index >= 0) {
            merged[index] = mergeSymbolData(merged.at(index).toMap(), symbol);
        } else {
            merged.append(symbol);
        }
    }

    return merged;
}

static QVariantMap mergeSymbolData(QVariantMap primary, const QVariantMap &secondary)
{
    const QStringList scalarFields = {
        QStringLiteral("kind"),
        QStringLiteral("name"),
        QStringLiteral("detail"),
        QStringLiteral("snippet"),
        QStringLiteral("sourceMode"),
        QStringLiteral("confidence")
    };
    for (const QString &field : scalarFields) {
        if (primary.value(field).toString().isEmpty() && !secondary.value(field).toString().isEmpty()) {
            primary.insert(field, secondary.value(field));
        }
    }

    if (primary.value(QStringLiteral("line")).toInt() <= 0 && secondary.value(QStringLiteral("line")).toInt() > 0) {
        primary.insert(QStringLiteral("line"), secondary.value(QStringLiteral("line")));
    }

    const QVariantList mergedMembers = mergeSymbolLists(primary.value(QStringLiteral("members")).toList(),
                                                        secondary.value(QStringLiteral("members")).toList());
    primary.insert(QStringLiteral("members"), mergedMembers);

    if (primary.value(QStringLiteral("parameters")).toList().isEmpty()
        && !secondary.value(QStringLiteral("parameters")).toList().isEmpty()) {
        primary.insert(QStringLiteral("parameters"), secondary.value(QStringLiteral("parameters")).toList());
    }
    if (primary.value(QStringLiteral("returns")).toList().isEmpty()
        && !secondary.value(QStringLiteral("returns")).toList().isEmpty()) {
        primary.insert(QStringLiteral("returns"), secondary.value(QStringLiteral("returns")).toList());
    }

    primary.insert(QStringLiteral("calls"),
                   mergeUniqueRelations(primary.value(QStringLiteral("calls")).toList(),
                                        secondary.value(QStringLiteral("calls")).toList()));
    primary.insert(QStringLiteral("calledBy"),
                   mergeUniqueRelations(primary.value(QStringLiteral("calledBy")).toList(),
                                        secondary.value(QStringLiteral("calledBy")).toList()));
    return primary;
}

static QVariantMap canonicalRelationForSymbol(const QVariantMap &relation, const QVariantList &canonicalSymbols)
{
    const int index = compatibleSymbolIndex(canonicalSymbols, relation);
    if (index < 0) {
        return relation;
    }

    QVariantMap normalized = relationFromSymbol(canonicalSymbols.at(index).toMap());
    const QString detail = relation.value(QStringLiteral("detail")).toString();
    if (!detail.isEmpty()) {
        normalized.insert(QStringLiteral("detail"), detail);
    }
    return normalized;
}

static QVariantList normalizeRelationsAgainstSymbols(const QVariantList &relations,
                                                     const QVariantList &canonicalSymbols)
{
    QVariantList normalized;
    QHash<QString, QVariantList> deduped;
    const QString ownerKey = QStringLiteral("__normalized__");
    for (const QVariant &entry : relations) {
        const QVariantMap relation = canonicalRelationForSymbol(entry.toMap(), canonicalSymbols);
        appendUniqueRelation(deduped, ownerKey, relation, relation.value(QStringLiteral("detail")).toString());
    }
    normalized = deduped.value(ownerKey);
    return normalized;
}

static QVariantList normalizeSymbolTree(const QVariantList &symbols, const QVariantList &canonicalSymbols)
{
    QVariantList normalized;
    for (const QVariant &entry : symbols) {
        QVariantMap symbol = entry.toMap();
        QVariantList members = normalizeSymbolTree(symbol.value(QStringLiteral("members")).toList(), canonicalSymbols);
        members = mergeSymbolLists(QVariantList{}, members);
        symbol.insert(QStringLiteral("members"), members);
        symbol.insert(QStringLiteral("calls"),
                      normalizeRelationsAgainstSymbols(symbol.value(QStringLiteral("calls")).toList(),
                                                       canonicalSymbols));
        symbol.insert(QStringLiteral("calledBy"),
                      normalizeRelationsAgainstSymbols(symbol.value(QStringLiteral("calledBy")).toList(),
                                                       canonicalSymbols));
        normalized.append(symbol);
    }
    return mergeSymbolLists(QVariantList{}, normalized);
}

static bool analysisHasMeaningfulContent(const QVariantMap &analysis)
{
    return !analysis.value(QStringLiteral("symbols")).toList().isEmpty()
        || !analysis.value(QStringLiteral("dependencies")).toList().isEmpty()
        || !analysis.value(QStringLiteral("routes")).toList().isEmpty()
        || !analysis.value(QStringLiteral("quickLinks")).toList().isEmpty()
        || !analysis.value(QStringLiteral("relatedFiles")).toList().isEmpty()
        || !analysis.value(QStringLiteral("packageSummary")).toMap().isEmpty()
        || !analysis.value(QStringLiteral("cssSummary")).toMap().isEmpty()
        || !analysis.value(QStringLiteral("summary")).toString().trimmed().isEmpty();
}

static QVariantMap mergeRecoveredAnalysis(QVariantMap primary,
                                          const QVariantMap &recovery,
                                          const QString &message)
{
    primary.insert(QStringLiteral("symbols"),
                   mergeSymbolLists(primary.value(QStringLiteral("symbols")).toList(),
                                    recovery.value(QStringLiteral("symbols")).toList()));
    const QVariantList canonicalSymbols = primary.value(QStringLiteral("symbols")).toList();
    primary.insert(QStringLiteral("symbols"),
                   normalizeSymbolTree(primary.value(QStringLiteral("symbols")).toList(),
                                       canonicalSymbols));
    {
        // The AST and heuristic parsers describe the same #include / import
        // with different labels and types: one dependency per target and line.
        QVariantList dependencies = primary.value(QStringLiteral("dependencies")).toList();
        QSet<QString> seen;
        for (const QVariant &entry : std::as_const(dependencies)) {
            const QVariantMap item = entry.toMap();
            seen.insert(item.value(QStringLiteral("target")).toString() + QLatin1Char('|') + item.value(QStringLiteral("line")).toString());
        }
        for (const QVariant &entry : recovery.value(QStringLiteral("dependencies")).toList()) {
            const QVariantMap item = entry.toMap();
            const QString key = item.value(QStringLiteral("target")).toString() + QLatin1Char('|') + item.value(QStringLiteral("line")).toString();
            if (!seen.contains(key)) {
                seen.insert(key);
                dependencies.append(entry);
            }
        }
        primary.insert(QStringLiteral("dependencies"), dependencies);
    }
    primary.insert(QStringLiteral("routes"),
                   mergeUniqueFlatEntries(primary.value(QStringLiteral("routes")).toList(),
                                          recovery.value(QStringLiteral("routes")).toList()));
    primary.insert(QStringLiteral("quickLinks"),
                   mergeUniqueFlatEntries(primary.value(QStringLiteral("quickLinks")).toList(),
                                          recovery.value(QStringLiteral("quickLinks")).toList()));
    primary.insert(QStringLiteral("relatedFiles"),
                   mergeUniqueFlatEntries(primary.value(QStringLiteral("relatedFiles")).toList(),
                                          recovery.value(QStringLiteral("relatedFiles")).toList()));

    if (primary.value(QStringLiteral("packageSummary")).toMap().isEmpty()
        && !recovery.value(QStringLiteral("packageSummary")).toMap().isEmpty()) {
        primary.insert(QStringLiteral("packageSummary"), recovery.value(QStringLiteral("packageSummary")).toMap());
    }
    if (primary.value(QStringLiteral("cssSummary")).toMap().isEmpty()
        && !recovery.value(QStringLiteral("cssSummary")).toMap().isEmpty()) {
        primary.insert(QStringLiteral("cssSummary"), recovery.value(QStringLiteral("cssSummary")).toMap());
    }
    if (primary.value(QStringLiteral("summary")).toString().trimmed().isEmpty()
        && !recovery.value(QStringLiteral("summary")).toString().trimmed().isEmpty()) {
        primary.insert(QStringLiteral("summary"), recovery.value(QStringLiteral("summary")).toString());
    }

    primary.insert(QStringLiteral("analysisHasAstErrors"),
                   primary.value(QStringLiteral("analysisHasAstErrors")).toBool()
                       || recovery.value(QStringLiteral("analysisHasAstErrors")).toBool());
    appendParserAnalysisNotice(primary, QStringLiteral("warning"), message, true);
    return primary;
}

// Heuristic symbols that start on lines the AST repair had to blank. These are
// the only places where the heuristic parser is allowed to add structure once
// the repaired tree is clean: a damaged header line keeps its declaration, but
// everything else stays AST-derived.
static QVariantList heuristicSymbolsOnLines(const QVariantList &heuristicSymbols,
                                            const QSet<int> &lines,
                                            QVariantList astSymbols,
                                            QVariantList *mergedAstSymbols)
{
    QVariantList admitted;
    for (const QVariant &entry : heuristicSymbols) {
        QVariantMap symbol = entry.toMap();
        const int line = symbol.value(QStringLiteral("line")).toInt();
        if (lines.contains(line)) {
            symbol.insert(QStringLiteral("calledBy"), QVariantList{});
            admitted.append(symbol);
            continue;
        }
        // A damaged member header inside an intact type: graft the member onto
        // the matching AST type instead of duplicating the type.
        QVariantList damagedMembers;
        for (const QVariant &memberEntry : symbol.value(QStringLiteral("members")).toList()) {
            QVariantMap member = memberEntry.toMap();
            if (lines.contains(member.value(QStringLiteral("line")).toInt())) {
                member.insert(QStringLiteral("calls"), QVariantList{});
                member.insert(QStringLiteral("calledBy"), QVariantList{});
                member.insert(QStringLiteral("sourceMode"), QStringLiteral("heuristic"));
                member.insert(QStringLiteral("confidence"), QStringLiteral("medium"));
                damagedMembers.append(member);
            }
        }
        if (damagedMembers.isEmpty()) {
            continue;
        }
        for (int index = 0; index < astSymbols.size(); ++index) {
            QVariantMap astSymbol = astSymbols.at(index).toMap();
            if (astSymbol.value(QStringLiteral("name")).toString() != symbol.value(QStringLiteral("name")).toString()) {
                continue;
            }
            QVariantList members = astSymbol.value(QStringLiteral("members")).toList();
            for (const QVariant &memberEntry : std::as_const(damagedMembers)) {
                const QString memberName = memberEntry.toMap().value(QStringLiteral("name")).toString();
                const bool present = std::any_of(members.cbegin(), members.cend(), [&](const QVariant &existing) {
                    return existing.toMap().value(QStringLiteral("name")).toString() == memberName;
                });
                if (!present) {
                    members.append(memberEntry);
                }
            }
            astSymbol.insert(QStringLiteral("members"), members);
            astSymbols[index] = astSymbol;
            break;
        }
    }
    if (mergedAstSymbols) {
        *mergedAstSymbols = astSymbols;
    }
    return admitted;
}

// Keep an admitted heuristic symbol's outgoing calls only where the target is a
// real symbol of the merged tree, and give each target the reverse edge.
static QVariantList linkAdmittedHeuristicCalls(QVariantList symbols, const QVariantList &admitted)
{
    auto identity = [](const QVariantMap &item) {
        return QStringLiteral("%1|%2|%3").arg(item.value(QStringLiteral("kind")).toString(),
                                              item.value(QStringLiteral("name")).toString(),
                                              QString::number(item.value(QStringLiteral("line")).toInt()));
    };
    QSet<QString> admittedIds;
    for (const QVariant &entry : admitted) {
        admittedIds.insert(identity(entry.toMap()));
    }
    QHash<QString, QVariantList> reverseEdges;
    QSet<QString> knownIds;
    std::function<void(const QVariantList &)> collect = [&](const QVariantList &items) {
        for (const QVariant &entry : items) {
            const QVariantMap item = entry.toMap();
            knownIds.insert(identity(item));
            collect(item.value(QStringLiteral("members")).toList());
        }
    };
    collect(symbols);

    for (int index = 0; index < symbols.size(); ++index) {
        QVariantMap symbol = symbols.at(index).toMap();
        if (!admittedIds.contains(identity(symbol))) {
            continue;
        }
        QVariantList kept;
        for (const QVariant &callEntry : symbol.value(QStringLiteral("calls")).toList()) {
            const QVariantMap call = callEntry.toMap();
            if (knownIds.contains(identity(call))) {
                kept.append(call);
                reverseEdges[identity(call)].append(relationFromSymbol(symbol));
            }
        }
        symbol.insert(QStringLiteral("calls"), kept);
        symbols[index] = symbol;
    }
    if (reverseEdges.isEmpty()) {
        return symbols;
    }
    std::function<QVariantList(QVariantList)> apply = [&](QVariantList items) {
        for (int index = 0; index < items.size(); ++index) {
            QVariantMap item = items.at(index).toMap();
            const auto edges = reverseEdges.constFind(identity(item));
            if (edges != reverseEdges.constEnd()) {
                item.insert(QStringLiteral("calledBy"),
                            mergeUniqueRelations(item.value(QStringLiteral("calledBy")).toList(), *edges));
            }
            item.insert(QStringLiteral("members"), apply(item.value(QStringLiteral("members")).toList()));
            items[index] = item;
        }
        return items;
    };
    return apply(symbols);
}

static int countSymbolTree(const QVariantList &symbols)
{
    int total = 0;
    for (const QVariant &entry : symbols) {
        total += 1 + countSymbolTree(entry.toMap().value(QStringLiteral("members")).toList());
    }
    return total;
}

static QString describeLineList(const QList<int> &lines)
{
    QStringList parts;
    for (int line : lines) {
        parts.append(QString::number(line));
    }
    return parts.join(QStringLiteral(", "));
}

static QVariantMap makeLineRange(int startLine, int endLine)
{
    QVariantMap range;
    range.insert(QStringLiteral("startLine"), startLine);
    range.insert(QStringLiteral("endLine"), qMax(startLine, endLine));
    return range;
}

static QVariantList normalizeLineRanges(QVariantList ranges)
{
    if (ranges.isEmpty()) {
        return ranges;
    }

    std::sort(ranges.begin(), ranges.end(), [](const QVariant &leftValue, const QVariant &rightValue) {
        const QVariantMap left = leftValue.toMap();
        const QVariantMap right = rightValue.toMap();
        const int leftStart = left.value(QStringLiteral("startLine")).toInt();
        const int rightStart = right.value(QStringLiteral("startLine")).toInt();
        if (leftStart != rightStart) {
            return leftStart < rightStart;
        }
        return left.value(QStringLiteral("endLine")).toInt() < right.value(QStringLiteral("endLine")).toInt();
    });

    QVariantList normalized;
    QVariantMap current = ranges.constFirst().toMap();
    for (int index = 1; index < ranges.size(); ++index) {
        const QVariantMap candidate = ranges.at(index).toMap();
        const int currentEnd = current.value(QStringLiteral("endLine")).toInt();
        const int candidateStart = candidate.value(QStringLiteral("startLine")).toInt();
        if (candidateStart <= currentEnd + 1) {
            current.insert(QStringLiteral("endLine"),
                           qMax(currentEnd, candidate.value(QStringLiteral("endLine")).toInt()));
            continue;
        }
        normalized.append(current);
        current = candidate;
    }
    normalized.append(current);
    return normalized;
}

static int symbolEndLine(const QVariantMap &symbol)
{
    const int startLine = symbol.value(QStringLiteral("line")).toInt();
    if (startLine <= 0) {
        return startLine;
    }

    const QString snippet = symbol.value(QStringLiteral("snippet")).toString();
    const int lineCount = qMax(1, snippet.count(QLatin1Char('\n')) + 1);
    return startLine + lineCount - 1;
}

static bool lineSpanIntersectsRanges(int startLine,
                                     int endLine,
                                     const QVariantList &ranges)
{
    if (startLine <= 0 || endLine <= 0 || ranges.isEmpty()) {
        return false;
    }

    for (const QVariant &entry : ranges) {
        const QVariantMap range = entry.toMap();
        const int rangeStart = range.value(QStringLiteral("startLine")).toInt();
        const int rangeEnd = range.value(QStringLiteral("endLine")).toInt();
        if (startLine <= rangeEnd && endLine >= rangeStart) {
            return true;
        }
    }
    return false;
}

static QVariantList collectSymbolOwnedRanges(const QVariantList &symbols)
{
    QVariantList ranges;
    std::function<void(const QVariantList &)> visit = [&](const QVariantList &items) {
        for (const QVariant &entry : items) {
            const QVariantMap symbol = entry.toMap();
            const int startLine = symbol.value(QStringLiteral("line")).toInt();
            const int endLine = symbolEndLine(symbol);
            if (startLine > 0 && endLine >= startLine) {
                ranges.append(makeLineRange(startLine, endLine));
            }
            visit(symbol.value(QStringLiteral("members")).toList());
        }
    };

    visit(symbols);
    return normalizeLineRanges(ranges);
}

static QVariantList filterRecoveryFlatEntries(const QVariantList &entries,
                                              const QVariantList &ranges)
{
    if (ranges.isEmpty()) {
        return entries;
    }

    QVariantList filtered;
    for (const QVariant &entry : entries) {
        const QVariantMap item = entry.toMap();
        const int line = item.value(QStringLiteral("line")).toInt();
        if (line > 0 && !lineSpanIntersectsRanges(line, line, ranges)) {
            filtered.append(item);
        }
    }
    return filtered;
}

static QVariantList filterRecoveryRelations(const QVariantList &relations,
                                            const QVariantList &ownedRanges)
{
    if (ownedRanges.isEmpty()) {
        return relations;
    }

    QVariantList filtered;
    for (const QVariant &entry : relations) {
        const QVariantMap relation = entry.toMap();
        const int line = relation.value(QStringLiteral("line")).toInt();
        if (line <= 0 || !lineSpanIntersectsRanges(line, line, ownedRanges)) {
            filtered.append(relation);
        }
    }
    return filtered;
}

static QVariantList filterRecoverySymbols(const QVariantList &symbols,
                                          const QVariantList &ranges,
                                          const QVariantList &canonicalAstSymbols)
{
    if (ranges.isEmpty()) {
        return symbols;
    }

    QVariantList filtered;
    for (const QVariant &entry : symbols) {
        QVariantMap symbol = entry.toMap();
        const QVariantList filteredMembers = filterRecoverySymbols(symbol.value(QStringLiteral("members")).toList(),
                                                                  ranges,
                                                                  canonicalAstSymbols);
        const int startLine = symbol.value(QStringLiteral("line")).toInt();
        const int endLine = symbolEndLine(symbol);
        const bool intersectsOwnedAst = lineSpanIntersectsRanges(startLine, endLine, ranges);
        const bool overlapsAst = compatibleSymbolIndex(canonicalAstSymbols, symbol) >= 0;
        const QVariantList filteredCalls = filterRecoveryRelations(symbol.value(QStringLiteral("calls")).toList(), ranges);
        const QVariantList filteredCalledBy = filterRecoveryRelations(symbol.value(QStringLiteral("calledBy")).toList(), ranges);

        if (intersectsOwnedAst && filteredMembers.isEmpty()
            && filteredCalls.isEmpty() && filteredCalledBy.isEmpty()) {
            continue;
        }

        symbol.insert(QStringLiteral("members"), filteredMembers);
        if (intersectsOwnedAst) {
            symbol.insert(QStringLiteral("calls"), filteredCalls);
            symbol.insert(QStringLiteral("calledBy"), filteredCalledBy);
        }

        if (!overlapsAst || !intersectsOwnedAst || !filteredMembers.isEmpty()) {
            filtered.append(symbol);
        } else if (!filteredCalls.isEmpty() || !filteredCalledBy.isEmpty()) {
            filtered.append(symbol);
        }
    }

    return filtered;
}

static QVariantMap limitRecoveryAnalysisToRanges(QVariantMap recovery,
                                                 const QVariantMap &astAnalysis)
{
    const QVariantList ranges = collectSymbolOwnedRanges(astAnalysis.value(QStringLiteral("symbols")).toList());
    if (ranges.isEmpty()) {
        return recovery;
    }

    const QVariantList astSymbols = astAnalysis.value(QStringLiteral("symbols")).toList();
    recovery.insert(QStringLiteral("symbols"),
                    filterRecoverySymbols(recovery.value(QStringLiteral("symbols")).toList(),
                                          ranges,
                                          astSymbols));
    recovery.insert(QStringLiteral("dependencies"),
                    filterRecoveryFlatEntries(recovery.value(QStringLiteral("dependencies")).toList(), ranges));
    recovery.insert(QStringLiteral("routes"),
                    filterRecoveryFlatEntries(recovery.value(QStringLiteral("routes")).toList(), ranges));
    recovery.insert(QStringLiteral("quickLinks"), QVariantList{});
    recovery.insert(QStringLiteral("relatedFiles"), QVariantList{});
    return recovery;
}

static QString swiftDeclarationKind(TSNode node, const QByteArray &source)
{
    const QString type = tsType(node);
    if (type == QStringLiteral("protocol_declaration")) {
        return QStringLiteral("protocol");
    }
    if (type == QStringLiteral("typealias_declaration")) {
        return QStringLiteral("typealias");
    }
    if (type == QStringLiteral("associatedtype_declaration")) {
        return QStringLiteral("associatedtype");
    }
    if (type == QStringLiteral("init_declaration")) {
        return QStringLiteral("initializer");
    }
    if (type == QStringLiteral("deinit_declaration")) {
        return QStringLiteral("deinitializer");
    }
    if (type == QStringLiteral("function_declaration") || type == QStringLiteral("protocol_function_declaration")) {
        return QStringLiteral("function");
    }
    if (type == QStringLiteral("property_declaration") || type == QStringLiteral("protocol_property_declaration")) {
        return QStringLiteral("property");
    }
    if (type == QStringLiteral("class_declaration")) {
        const QString declarationKind = nodeText(fieldNode(node, "declaration_kind"), source).trimmed().toLower();
        return declarationKind.isEmpty() ? QStringLiteral("type") : declarationKind;
    }
    return QStringLiteral("symbol");
}

static QString swiftDeclarationName(TSNode node, const QByteArray &source)
{
    const QString type = tsType(node);
    if (type == QStringLiteral("init_declaration")) {
        return QStringLiteral("init");
    }
    if (type == QStringLiteral("deinit_declaration")) {
        return QStringLiteral("deinit");
    }

    const QString nameText = nodeText(fieldNode(node, "name"), source).trimmed();
    if (!nameText.isEmpty()) {
        if (type == QStringLiteral("property_declaration") || type == QStringLiteral("protocol_property_declaration")) {
            const QString variableName = firstVariableName(nameText);
            return variableName.isEmpty() ? firstIdentifier(nameText) : variableName;
        }
        return firstIdentifier(nameText);
    }

    const QString snippet = nodeText(node, source);
    if (type == QStringLiteral("property_declaration") || type == QStringLiteral("protocol_property_declaration")) {
        const QString variableName = firstVariableName(snippet);
        return variableName.isEmpty() ? firstIdentifier(snippet) : variableName;
    }
    return firstIdentifier(snippet);
}

static QString swiftCallableKeyForNode(TSNode node,
                                       const QByteArray &source,
                                       const QHash<QString, QVariantMap> &byKey)
{
    const QString type = tsType(node);
    if (type != QStringLiteral("function_declaration")
        && type != QStringLiteral("protocol_function_declaration")
        && type != QStringLiteral("init_declaration")
        && type != QStringLiteral("deinit_declaration")
        && type != QStringLiteral("method_declaration")) {
        return QString();
    }

    QVariantMap probe;
    probe.insert(QStringLiteral("kind"), type == QStringLiteral("method_declaration")
                                       ? QStringLiteral("method")
                                       : swiftDeclarationKind(node, source));
    probe.insert(QStringLiteral("name"),
                 type == QStringLiteral("method_declaration")
                     ? firstIdentifier(nodeText(fieldNode(node, "name"), source).trimmed())
                     : swiftDeclarationName(node, source));
    probe.insert(QStringLiteral("line"), nodeLine(node));
    const QString key = symbolKey(probe);
    return byKey.contains(key) ? key : QString();
}

static QString swiftCallTargetName(TSNode node, const QByteArray &source)
{
    if (tsType(node) != QStringLiteral("call_expression")) {
        return QString();
    }

    QString raw = nodeText(fieldNode(node, "called_expression"), source).trimmed();
    if (raw.isEmpty()) {
        raw = nodeText(fieldNode(node, "function"), source).trimmed();
    }
    if (raw.isEmpty() && ts_node_named_child_count(node) > 0) {
        raw = nodeText(ts_node_named_child(node, 0), source).trimmed();
    }
    return lastIdentifier(raw);
}

static QString phpCallableKeyForNode(TSNode node,
                                     const QByteArray &source,
                                     const QHash<QString, QVariantMap> &byKey)
{
    const QString type = tsType(node);
    if (type != QStringLiteral("function_definition") && type != QStringLiteral("method_declaration")) {
        return QString();
    }

    QVariantMap probe;
    probe.insert(QStringLiteral("kind"), type == QStringLiteral("method_declaration")
                                       ? QStringLiteral("method")
                                       : QStringLiteral("function"));
    probe.insert(QStringLiteral("name"), firstIdentifier(nodeText(fieldNode(node, "name"), source).trimmed()));
    probe.insert(QStringLiteral("line"), nodeLine(node));
    const QString key = symbolKey(probe);
    return byKey.contains(key) ? key : QString();
}

static QString phpCallTargetName(TSNode node, const QByteArray &source)
{
    const QString type = tsType(node);
    if (type != QStringLiteral("function_call_expression")
        && type != QStringLiteral("member_call_expression")
        && type != QStringLiteral("scoped_call_expression")) {
        return QString();
    }

    QString raw = nodeText(fieldNode(node, "function"), source).trimmed();
    if (raw.isEmpty()) {
        raw = nodeText(fieldNode(node, "name"), source).trimmed();
    }
    if (raw.isEmpty() && ts_node_named_child_count(node) > 0) {
        raw = nodeText(ts_node_named_child(node, 0), source).trimmed();
    }
    return lastIdentifier(raw);
}

static QSet<QString> phpCallbackTargetNames(const QString &snippet,
                                            const QHash<QString, QStringList> &keysByName)
{
    QSet<QString> names;

    for (auto it = keysByName.cbegin(); it != keysByName.cend(); ++it) {
        const QString &candidate = it.key();
        if (candidate.isEmpty()) {
            continue;
        }

        const QString singleQuotedThis = QStringLiteral("[$this, '%1']").arg(candidate);
        const QString doubleQuotedThis = QStringLiteral("[$this, \"%1\"]").arg(candidate);
        const QString singleQuotedBare = QStringLiteral("'%1'").arg(candidate);
        const QString doubleQuotedBare = QStringLiteral("\"%1\"").arg(candidate);

        if (snippet.contains(singleQuotedThis) || snippet.contains(doubleQuotedThis)) {
            names.insert(candidate);
            continue;
        }

        if ((snippet.contains(QStringLiteral("add_action("))
             || snippet.contains(QStringLiteral("add_filter("))
             || snippet.contains(QStringLiteral("add_menu_page("))
             || snippet.contains(QStringLiteral("add_submenu_page("))
             || snippet.contains(QStringLiteral("register_rest_route(")))
            && (snippet.contains(singleQuotedBare) || snippet.contains(doubleQuotedBare))) {
            names.insert(candidate);
        }
    }

    return names;
}

static QVariantList applyPhpCallbackRelations(const QVariantList &symbols)
{
    if (symbols.isEmpty()) {
        return symbols;
    }

    QHash<QString, QVariantMap> byKey;
    QHash<QString, QStringList> keysByName;
    QHash<QString, QVariantList> callsByKey;
    QHash<QString, QVariantList> calledByByKey;
    collectSymbolsByKey(symbols, byKey, keysByName);

    std::function<void(const QVariantList &)> collectExistingRelations = [&](const QVariantList &items) {
        for (const QVariant &entry : items) {
            const QVariantMap symbol = entry.toMap();
            const QString ownerKey = symbolKey(symbol);
            if (!ownerKey.isEmpty()) {
                for (const QVariant &callEntry : symbol.value(QStringLiteral("calls")).toList()) {
                    appendUniqueRelation(callsByKey, ownerKey, callEntry.toMap(), callEntry.toMap().value(QStringLiteral("detail")).toString());
                }
                for (const QVariant &callerEntry : symbol.value(QStringLiteral("calledBy")).toList()) {
                    appendUniqueRelation(calledByByKey, ownerKey, callerEntry.toMap(), callerEntry.toMap().value(QStringLiteral("detail")).toString());
                }
            }
            collectExistingRelations(symbol.value(QStringLiteral("members")).toList());
        }
    };

    collectExistingRelations(symbols);

    std::function<void(const QVariantList &)> collectRelations = [&](const QVariantList &items) {
        for (const QVariant &entry : items) {
            const QVariantMap symbol = entry.toMap();
            const QString ownerKey = symbolKey(symbol);
            const QString ownerKind = symbol.value(QStringLiteral("kind")).toString();
            const bool canOwn = ownerKind == QStringLiteral("function")
                || ownerKind == QStringLiteral("method")
                || ownerKind == QStringLiteral("constructor")
                || ownerKind == QStringLiteral("initializer")
                || ownerKind == QStringLiteral("deinitializer")
                || ownerKind == QStringLiteral("hook")
                || ownerKind == QStringLiteral("component");
            if (!ownerKey.isEmpty() && canOwn) {
                const QSet<QString> targetNames = phpCallbackTargetNames(symbol.value(QStringLiteral("snippet")).toString(),
                                                                        keysByName);
                for (const QString &targetName : targetNames) {
                    const QStringList candidateKeys = keysByName.value(targetName);
                    if (candidateKeys.isEmpty()) {
                        continue;
                    }
                    const QString targetKey = bestRelationTargetKey(candidateKeys, byKey);
                    if (targetKey.isEmpty() || targetKey == ownerKey || !byKey.contains(targetKey)) {
                        continue;
                    }
                    appendUniqueRelation(callsByKey, ownerKey, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
                    appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(ownerKey)), QStringLiteral("called by"));
                }
            }
            collectRelations(symbol.value(QStringLiteral("members")).toList());
        }
    };

    collectRelations(symbols);
    return applyRelationsToSymbols(symbols, callsByKey, calledByByKey);
}

static QString pythonCallableKeyForNode(TSNode node,
                                        const QByteArray &source,
                                        const QHash<QString, QVariantMap> &byKey)
{
    if (tsType(node) != QStringLiteral("function_definition")) {
        return QString();
    }

    const QString name = firstIdentifier(nodeText(fieldNode(node, "name"), source).trimmed());
    const int line = nodeLine(node);
    const TSNode classAncestor = firstAncestorOfType(ts_node_parent(node), {"class_definition"});
    const TSNode decoratedAncestor = firstAncestorOfType(ts_node_parent(node), {"decorated_definition"});

    QStringList candidateKinds;
    if (!ts_node_is_null(classAncestor)) {
        if (!ts_node_is_null(decoratedAncestor)
            && nodeText(decoratedAncestor, source).contains(QStringLiteral("@property"))) {
            candidateKinds.append(QStringLiteral("property"));
        }
        candidateKinds.append(QStringLiteral("method"));
    }
    candidateKinds.append(QStringLiteral("function"));

    for (const QString &kind : std::as_const(candidateKinds)) {
        QVariantMap probe;
        probe.insert(QStringLiteral("kind"), kind);
        probe.insert(QStringLiteral("name"), name);
        probe.insert(QStringLiteral("line"), line);
        const QString key = symbolKey(probe);
        if (byKey.contains(key)) {
            return key;
        }
    }

    return QString();
}

static QString pythonCallTargetName(TSNode node, const QByteArray &source)
{
    if (tsType(node) != QStringLiteral("call")) {
        return QString();
    }

    QString raw = nodeText(fieldNode(node, "function"), source).trimmed();
    if (raw.isEmpty() && ts_node_named_child_count(node) > 0) {
        raw = nodeText(ts_node_named_child(node, 0), source).trimmed();
    }
    return lastIdentifier(raw);
}

static QString javaCallableKeyForNode(TSNode node,
                                      const QByteArray &source,
                                      const QHash<QString, QVariantMap> &byKey)
{
    QVariantMap probe;
    const QString type = tsType(node);
    if (type == QStringLiteral("method_declaration")) {
        probe.insert(QStringLiteral("kind"), QStringLiteral("method"));
        probe.insert(QStringLiteral("name"), nodeText(fieldNode(node, "name"), source).trimmed());
        probe.insert(QStringLiteral("line"), nodeLine(node));
    } else if (type == QStringLiteral("constructor_declaration")) {
        probe.insert(QStringLiteral("kind"), QStringLiteral("constructor"));
        probe.insert(QStringLiteral("name"), nodeText(fieldNode(node, "name"), source).trimmed());
        probe.insert(QStringLiteral("line"), nodeLine(node));
    }

    const QString key = symbolKey(probe);
    return byKey.contains(key) ? key : QString();
}

static QString javaCallTargetName(TSNode node, const QByteArray &source)
{
    const QString type = tsType(node);
    QString raw;
    if (type == QStringLiteral("method_invocation")) {
        raw = nodeText(fieldNode(node, "name"), source).trimmed();
        if (raw.isEmpty()) {
            raw = nodeText(fieldNode(node, "object"), source).trimmed();
        }
    } else if (type == QStringLiteral("object_creation_expression")) {
        raw = nodeText(fieldNode(node, "type"), source).trimmed();
    }

    if (raw.isEmpty() && ts_node_named_child_count(node) > 0) {
        raw = nodeText(ts_node_named_child(node, 0), source).trimmed();
    }
    return lastIdentifier(raw);
}

static QString csharpCallableKeyForNode(TSNode node,
                                        const QByteArray &source,
                                        const QHash<QString, QVariantMap> &byKey)
{
    QVariantMap probe;
    const QString type = tsType(node);
    if (type == QStringLiteral("method_declaration")) {
        probe.insert(QStringLiteral("kind"), QStringLiteral("method"));
        probe.insert(QStringLiteral("name"), nodeText(fieldNode(node, "name"), source).trimmed());
        probe.insert(QStringLiteral("line"), nodeLine(node));
    } else if (type == QStringLiteral("constructor_declaration")) {
        probe.insert(QStringLiteral("kind"), QStringLiteral("constructor"));
        probe.insert(QStringLiteral("name"), nodeText(fieldNode(node, "name"), source).trimmed());
        probe.insert(QStringLiteral("line"), nodeLine(node));
    }

    const QString key = symbolKey(probe);
    return byKey.contains(key) ? key : QString();
}

static QString csharpCallTargetName(TSNode node, const QByteArray &source)
{
    const QString type = tsType(node);
    QString raw;
    if (type == QStringLiteral("invocation_expression")) {
        raw = nodeText(fieldNode(node, "function"), source).trimmed();
        if (raw.isEmpty()) {
            raw = nodeText(fieldNode(node, "expression"), source).trimmed();
        }
    } else if (type == QStringLiteral("object_creation_expression")) {
        raw = nodeText(fieldNode(node, "type"), source).trimmed();
    }

    if (raw.isEmpty() && ts_node_named_child_count(node) > 0) {
        raw = nodeText(ts_node_named_child(node, 0), source).trimmed();
    }
    return lastIdentifier(raw);
}

static QString rustCallableKeyForNode(TSNode node,
                                      const QByteArray &source,
                                      const QHash<QString, QVariantMap> &byKey)
{
    if (tsType(node) != QStringLiteral("function_item")) {
        return QString();
    }

    const QString name = nodeText(fieldNode(node, "name"), source).trimmed();
    const int line = nodeLine(node);

    QStringList candidateKinds;
    const TSNode parent = ts_node_parent(node);
    const QString parentType = tsType(parent);
    if (parentType == QStringLiteral("declaration_list")) {
        const TSNode grandParent = ts_node_parent(parent);
        const QString grandParentType = tsType(grandParent);
        if (grandParentType == QStringLiteral("impl_item")
            || grandParentType == QStringLiteral("trait_item")
            || grandParentType == QStringLiteral("mod_item")) {
            candidateKinds.append(QStringLiteral("method"));
        }
    }
    candidateKinds.append(QStringLiteral("function"));

    for (const QString &kind : std::as_const(candidateKinds)) {
        QVariantMap probe;
        probe.insert(QStringLiteral("kind"), kind);
        probe.insert(QStringLiteral("name"), name);
        probe.insert(QStringLiteral("line"), line);
        const QString key = symbolKey(probe);
        if (byKey.contains(key)) {
            return key;
        }
    }

    return QString();
}

static QString rustCallTargetName(TSNode node, const QByteArray &source)
{
    if (tsType(node) != QStringLiteral("call_expression")) {
        return QString();
    }

    QString raw = nodeText(fieldNode(node, "function"), source).trimmed();
    if (raw.isEmpty() && ts_node_named_child_count(node) > 0) {
        raw = nodeText(ts_node_named_child(node, 0), source).trimmed();
    }
    return lastIdentifier(raw);
}

static QString jsCallableKeyForNode(TSNode node,
                                    const QByteArray &source,
                                    const QHash<QString, QVariantMap> &byKey,
                                    bool reactMode)
{
    if (ts_node_is_null(node)) {
        return {};
    }
    const QString type = tsType(node);
    QVariantMap probe;

    if (type == QStringLiteral("function_declaration")
        || type == QStringLiteral("generator_function_declaration")) {
        const QString name = firstIdentifier(nodeText(fieldNode(node, "name"), source).trimmed());
        probe.insert(QStringLiteral("kind"),
                     (!name.isEmpty() && name.startsWith(QStringLiteral("use")))
                         ? QStringLiteral("hook")
                         : (reactMode && !name.isEmpty() && name.at(0).isUpper()
                                ? QStringLiteral("component")
                                : QStringLiteral("function")));
        probe.insert(QStringLiteral("name"), name);
        probe.insert(QStringLiteral("line"), nodeLine(node));
    } else if (type == QStringLiteral("method_definition")) {
        probe.insert(QStringLiteral("kind"), QStringLiteral("method"));
        probe.insert(QStringLiteral("name"), firstIdentifier(nodeText(fieldNode(node, "name"), source).trimmed()));
        probe.insert(QStringLiteral("line"), nodeLine(node));
    } else if (type == QStringLiteral("variable_declarator")) {
        const QString name = firstIdentifier(nodeText(fieldNode(node, "name"), source).trimmed());
        const TSNode valueNode = fieldNode(node, "value");
        if (ts_node_is_null(valueNode)) {
            return {};
        }
        const QString valueType = tsType(valueNode);
        if (valueType == QStringLiteral("arrow_function")
            || valueType == QStringLiteral("function")
            || valueType == QStringLiteral("function_expression")) {
            probe.insert(QStringLiteral("kind"),
                         (!name.isEmpty() && name.startsWith(QStringLiteral("use")))
                             ? QStringLiteral("hook")
                             : (reactMode && !name.isEmpty() && name.at(0).isUpper()
                                    ? QStringLiteral("component")
                                    : QStringLiteral("function")));
            probe.insert(QStringLiteral("name"), name);
            probe.insert(QStringLiteral("line"), nodeLine(node));
        }
    } else if (type == QStringLiteral("pair")) {
        const TSNode keyNode = fieldNode(node, "key");
        const TSNode valueNode = fieldNode(node, "value");
        if (ts_node_is_null(valueNode)) {
            return {};
        }
        const QString valueType = tsType(valueNode);
        if (valueType == QStringLiteral("arrow_function")
            || valueType == QStringLiteral("function")
            || valueType == QStringLiteral("function_expression")) {
            probe.insert(QStringLiteral("kind"), QStringLiteral("function"));
            probe.insert(QStringLiteral("name"), firstIdentifier(nodeText(keyNode, source).trimmed()));
            probe.insert(QStringLiteral("line"), nodeLine(node));
        }
    } else if (type == QStringLiteral("assignment_expression")) {
        const QString left = nodeText(fieldNode(node, "left"), source).trimmed();
        const TSNode valueNode = fieldNode(node, "right");
        if (ts_node_is_null(valueNode)) {
            return {};
        }
        const QString valueType = tsType(valueNode);
        if ((left.startsWith(QStringLiteral("module.exports."))
             || left.startsWith(QStringLiteral("exports.")))
            && (valueType == QStringLiteral("arrow_function")
                || valueType == QStringLiteral("function")
                || valueType == QStringLiteral("function_expression"))) {
            probe.insert(QStringLiteral("kind"), QStringLiteral("function"));
            probe.insert(QStringLiteral("name"), left.section(QLatin1Char('.'), -1));
            probe.insert(QStringLiteral("line"), nodeLine(node));
        }
    }

    const QString key = symbolKey(probe);
    return byKey.contains(key) ? key : QString();
}

static QString jsCallTargetName(TSNode node, const QByteArray &source)
{
    const QString type = tsType(node);
    if (type != QStringLiteral("call_expression") && type != QStringLiteral("new_expression")) {
        return QString();
    }

    QString raw = nodeText(fieldNode(node, "function"), source).trimmed();
    if (raw.isEmpty()) {
        raw = nodeText(fieldNode(node, "constructor"), source).trimmed();
    }
    if (raw.isEmpty() && ts_node_named_child_count(node) > 0) {
        raw = nodeText(ts_node_named_child(node, 0), source).trimmed();
    }
    return lastIdentifier(raw);
}

static QSet<QString> scriptCallNamesFromSnippet(const QString &snippet)
{
    static const QVector<QRegularExpression> patterns = {
        QRegularExpression(QStringLiteral(R"(\b([A-Za-z_][A-Za-z0-9_]*)\s*\()")),
        QRegularExpression(QStringLiteral(R"(\.\s*([A-Za-z_][A-Za-z0-9_]*)\s*\()")),
        QRegularExpression(QStringLiteral(R"(\[\s*\$this\s*,\s*['"]([A-Za-z_][A-Za-z0-9_]*)['"]\s*\])"))
    };

    QSet<QString> names;
    for (const QRegularExpression &pattern : patterns) {
        auto it = pattern.globalMatch(snippet);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            names.insert(match.captured(1));
        }
    }
    return names;
}

static QVariantMap makeImportBinding(const QString &localName, const QString &importedName)
{
    QVariantMap binding;
    binding.insert(QStringLiteral("local"), localName.trimmed());
    binding.insert(QStringLiteral("imported"), importedName.trimmed());
    return binding;
}

static QVariantList parseNamedImportBindings(const QString &clause)
{
    QVariantList bindings;
    QString inner = clause.trimmed();
    if (inner.startsWith(QLatin1Char('{'))) {
        inner.remove(0, 1);
    }
    if (inner.endsWith(QLatin1Char('}'))) {
        inner.chop(1);
    }

    const QStringList parts = inner.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &rawPart : parts) {
        const QString part = rawPart.trimmed();
        if (part.isEmpty()) {
            continue;
        }
        const QStringList aliasParts = part.split(QRegularExpression(QStringLiteral(R"(\s+as\s+)")),
                                                  Qt::SkipEmptyParts);
        if (aliasParts.size() >= 2) {
            bindings.append(makeImportBinding(aliasParts.at(1), aliasParts.at(0)));
        } else {
            bindings.append(makeImportBinding(part, part));
        }
    }

    return bindings;
}

static QVariantList parseScriptImportBindingsFromStatement(const QString &statement)
{
    QVariantList bindings;
    QRegularExpression importClausePattern(QStringLiteral(R"(^\s*import\s+(.+?)\s+from\s+['"])"),
                                           QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch clauseMatch = importClausePattern.match(statement);
    if (!clauseMatch.hasMatch()) {
        return bindings;
    }

    const QString clause = clauseMatch.captured(1).trimmed();
    if (clause.isEmpty()) {
        return bindings;
    }

    int braceStart = clause.indexOf(QLatin1Char('{'));
    int braceEnd = clause.lastIndexOf(QLatin1Char('}'));
    QString beforeBraces = clause;
    if (braceStart >= 0 && braceEnd > braceStart) {
        beforeBraces = clause.left(braceStart).trimmed();
        const QVariantList named = parseNamedImportBindings(clause.mid(braceStart, braceEnd - braceStart + 1));
        for (const QVariant &binding : named) {
            bindings.append(binding);
        }
    }

    QString prefix = beforeBraces;
    if (prefix.endsWith(QLatin1Char(','))) {
        prefix.chop(1);
        prefix = prefix.trimmed();
    }

    if (prefix.startsWith(QStringLiteral("* as "))) {
        bindings.append(makeImportBinding(prefix.mid(5).trimmed(), QStringLiteral("*")));
    } else if (!prefix.isEmpty()) {
        const QStringList prefixParts = prefix.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &part : prefixParts) {
            const QString local = part.trimmed();
            if (!local.isEmpty()) {
                bindings.append(makeImportBinding(local, QStringLiteral("default")));
            }
        }
    }

    return bindings;
}

static QVariantList parseRequireBindingsFromStatement(const QString &statement)
{
    QVariantList bindings;
    QRegularExpression destructuredPattern(QStringLiteral(R"((?:const|let|var)\s*\{([^}]+)\}\s*=\s*require\s*\()"));
    const QRegularExpressionMatch destructuredMatch = destructuredPattern.match(statement);
    if (destructuredMatch.hasMatch()) {
        const QStringList parts = destructuredMatch.captured(1).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &rawPart : parts) {
            const QString part = rawPart.trimmed();
            if (part.isEmpty()) {
                continue;
            }
            const QStringList aliasParts = part.split(QLatin1Char(':'), Qt::SkipEmptyParts);
            if (aliasParts.size() >= 2) {
                bindings.append(makeImportBinding(aliasParts.at(1), aliasParts.at(0)));
            } else {
                bindings.append(makeImportBinding(part, part));
            }
        }
        return bindings;
    }

    QRegularExpression directPattern(QStringLiteral(R"((?:const|let|var)\s+([A-Za-z_]\w*)\s*=\s*require\s*\()"));
    const QRegularExpressionMatch directMatch = directPattern.match(statement);
    if (directMatch.hasMatch()) {
        bindings.append(makeImportBinding(directMatch.captured(1), QStringLiteral("default")));
    }
    return bindings;
}

static bool canOwnCallRelations(const QString &kind)
{
    return kind == QStringLiteral("function")
        || kind == QStringLiteral("method")
        || kind == QStringLiteral("constructor")
        || kind == QStringLiteral("initializer")
        || kind == QStringLiteral("deinitializer")
        || kind == QStringLiteral("hook")
        || kind == QStringLiteral("component");
}

static QVariantList applySnippetCallRelations(const QVariantList &symbols)
{
    if (symbols.isEmpty()) {
        return symbols;
    }

    QHash<QString, QVariantMap> byKey;
    QHash<QString, QStringList> keysByName;
    QHash<QString, QVariantList> callsByKey;
    QHash<QString, QVariantList> calledByByKey;
    collectSymbolsByKey(symbols, byKey, keysByName);

    std::function<void(const QVariantList &)> collectExistingRelations = [&](const QVariantList &items) {
        for (const QVariant &entry : items) {
            const QVariantMap symbol = entry.toMap();
            const QString ownerKey = symbolKey(symbol);
            if (!ownerKey.isEmpty()) {
                for (const QVariant &callEntry : symbol.value(QStringLiteral("calls")).toList()) {
                    appendUniqueRelation(callsByKey, ownerKey, callEntry.toMap(), callEntry.toMap().value(QStringLiteral("detail")).toString());
                }
                for (const QVariant &callerEntry : symbol.value(QStringLiteral("calledBy")).toList()) {
                    appendUniqueRelation(calledByByKey, ownerKey, callerEntry.toMap(), callerEntry.toMap().value(QStringLiteral("detail")).toString());
                }
            }
            collectExistingRelations(symbol.value(QStringLiteral("members")).toList());
        }
    };

    collectExistingRelations(symbols);

    std::function<void(const QVariantList &)> collectRelations = [&](const QVariantList &items) {
        for (const QVariant &entry : items) {
            const QVariantMap symbol = entry.toMap();
            const QString ownerKey = symbolKey(symbol);
            const QString ownerKind = symbol.value(QStringLiteral("kind")).toString();
            if (!ownerKey.isEmpty() && canOwnCallRelations(ownerKind)) {
                const QSet<QString> relationNames = scriptCallNamesFromSnippet(symbol.value(QStringLiteral("snippet")).toString());
                for (const QString &targetName : relationNames) {
                    const QStringList candidateKeys = keysByName.value(targetName);
                    if (candidateKeys.isEmpty()) {
                        continue;
                    }
                    const QString targetKey = bestRelationTargetKey(candidateKeys, byKey);
                    if (targetKey.isEmpty() || targetKey == ownerKey || !byKey.contains(targetKey)) {
                        continue;
                    }
                    appendUniqueRelation(callsByKey, ownerKey, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
                    appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(ownerKey)), QStringLiteral("called by"));
                }
            }
            collectRelations(symbol.value(QStringLiteral("members")).toList());
        }
    };

    collectRelations(symbols);
    return applyRelationsToSymbols(symbols, callsByKey, calledByByKey);
}

static QVariantMap makeCssClassSummaryEntry(const QString &name, bool matched,
                                            const QString &path = QString(),
                                            int line = 0,
                                            const QString &snippet = QString())
{
    QVariantMap entry;
    entry.insert(QStringLiteral("name"), name);
    entry.insert(QStringLiteral("kind"), QStringLiteral("class"));
    entry.insert(QStringLiteral("matched"), matched);
    entry.insert(QStringLiteral("path"), path);
    entry.insert(QStringLiteral("line"), line);
    entry.insert(QStringLiteral("snippet"),
                 !snippet.isEmpty() ? snippet : QStringLiteral(".%1 { ... }").arg(name));
    return entry;
}

static QString normalizeCssSelectorText(const QString &text)
{
    QString cleaned = text;
    cleaned.remove(QRegularExpression(QStringLiteral(R"(/\*[\s\S]*?\*/)")));
    return cleaned;
}

// Class name of a CSS class_selector node: `li.completed` and `.a.b` parse as
// class_selector nodes whose text includes the tag / outer class, so read the
// class_name child rather than the node text.
static QString cssClassSelectorName(TSNode node, const QByteArray &source)
{
    QString name;
    const uint32_t childCount = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < childCount; ++i) {
        TSNode child = ts_node_named_child(node, i);
        if (tsType(child) == QStringLiteral("class_name")) {
            name = nodeText(child, source).trimmed();
        }
    }
    if (name.isEmpty()) {
        name = nodeText(node, source).trimmed().section(QLatin1Char('.'), -1);
    }
    return name;
}

static QStringList extractCssClassesTreeSitter(const QString &text)
{
    QStringList classes;
    const QByteArray source = text.toUtf8();
    TSLanguage *language = languageForName(QStringLiteral("css"));
    if (!language) {
        return classes;
    }

    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, language)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return classes;
    }

    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    if (!tree) {
        ts_parser_delete(parser);
        return classes;
    }

    TSNode root = ts_tree_root_node(tree);
    QSet<QString> seen;
    std::function<void(TSNode)> visit = [&](TSNode node) {
        if (ts_node_is_null(node)) {
            return;
        }

        const QString type = tsType(node);
        if (type == QStringLiteral("class_selector")) {
            const QString name = cssClassSelectorName(node, source);
            if (!name.isEmpty() && !seen.contains(name)) {
                seen.insert(name);
                classes.append(name);
            }
        }

        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            visit(ts_node_named_child(node, i));
        }
    };

    visit(root);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    classes.sort(Qt::CaseInsensitive);
    return classes;
}

static QVariantMap findCssClassSummaryEntryTreeSitter(const QString &cssPath, const QString &cssText, const QString &name)
{
    const QByteArray source = cssText.toUtf8();
    TSLanguage *language = languageForName(QStringLiteral("css"));
    if (!language) {
        return {};
    }

    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, language)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return {};
    }

    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    if (!tree) {
        ts_parser_delete(parser);
        return {};
    }

    TSNode root = ts_tree_root_node(tree);
    QVariantMap entry;
    bool found = false;

    std::function<void(TSNode)> visit = [&](TSNode node) {
        if (found || ts_node_is_null(node)) {
            return;
        }

        const QString type = tsType(node);
        if (type == QStringLiteral("class_selector")) {
            const QString className = cssClassSelectorName(node, source);
            if (className == name) {
                const TSNode snippetNode = firstAncestorOfType(node, {"rule_set", "block"});
                const TSNode effectiveNode = ts_node_is_null(snippetNode) ? node : snippetNode;
                entry = makeCssClassSummaryEntry(name, true, cssPath,
                                                 nodeLine(effectiveNode),
                                                 nodeSnippet(effectiveNode, source));
                found = true;
                return;
            }
        }

        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            visit(ts_node_named_child(node, i));
            if (found) {
                return;
            }
        }
    };

    visit(root);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    return entry;
}

static QVariantMap findCssClassSummaryEntry(const QString &cssPath, const QString &cssText, const QString &name)
{
    QVariantMap astEntry = findCssClassSummaryEntryTreeSitter(cssPath, cssText, name);
    if (!astEntry.isEmpty()) {
        return astEntry;
    }

    const QString escapedName = QRegularExpression::escape(name);
    const QString cleanedText = normalizeCssSelectorText(cssText);
    QRegularExpression selectorPattern(QStringLiteral("\\.%1\\b[^\\{\\n]*\\{").arg(escapedName));
    const QRegularExpressionMatch match = selectorPattern.match(cleanedText);
    if (!match.hasMatch()) {
        return makeCssClassSummaryEntry(name, true, cssPath, 0, QStringLiteral(".%1 { ... }").arg(name));
    }

    const int start = match.capturedStart(0);
    const int line = cleanedText.left(start).count(QLatin1Char('\n')) + 1;
    const QString snippet = cleanedText.mid(start, 100).split(QLatin1Char('\n')).at(0).trimmed()
        + QStringLiteral(" ...");
    return makeCssClassSummaryEntry(name, true, cssPath, line, snippet);
}

// Single-pass CSS class index builder. Parses cssText once and returns name → entry for
// all class selectors. For minified files (detected by filename or line structure) it falls
// back to a fast regex scan with stub entries so the caller never runs N sequential parses.
static QMap<QString, QVariantMap> buildCssClassIndex(const QString &cssPath, const QString &cssText)
{
    QMap<QString, QVariantMap> index;

    if (looksLikeMinifiedSource(cssPath, QStringLiteral("css"), cssText)) {
        const QString cleaned = normalizeCssSelectorText(cssText);
        QRegularExpression pattern(QStringLiteral(R"(\.([A-Za-z_-][\w-]*))"));
        auto it = pattern.globalMatch(cleaned);
        while (it.hasNext()) {
            const QString name = it.next().captured(1);
            if (!index.contains(name)) {
                index.insert(name, makeCssClassSummaryEntry(name, true, cssPath, 0,
                    QStringLiteral(".%1 { ... }").arg(name)));
            }
        }
        return index;
    }

    const QByteArray source = cssText.toUtf8();
    TSLanguage *language = languageForName(QStringLiteral("css"));
    if (!language) {
        return index;
    }
    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, language)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return index;
    }
    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    if (!tree) {
        ts_parser_delete(parser);
        return index;
    }
    TSNode root = ts_tree_root_node(tree);
    std::function<void(TSNode)> visit = [&](TSNode node) {
        if (ts_node_is_null(node)) {
            return;
        }
        const QString type = tsType(node);
        if (type == QStringLiteral("class_selector")) {
            const QString name = cssClassSelectorName(node, source);
            if (!name.isEmpty() && !index.contains(name)) {
                const TSNode snippetNode = firstAncestorOfType(node, {"rule_set", "block"});
                const TSNode effectiveNode = ts_node_is_null(snippetNode) ? node : snippetNode;
                index.insert(name, makeCssClassSummaryEntry(name, true, cssPath,
                                                             nodeLine(effectiveNode),
                                                             nodeSnippet(effectiveNode, source)));
            }
        }
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            visit(ts_node_named_child(node, i));
        }
    };
    visit(root);
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return index;
}

static QStringList extractHtmlLinkedAssets(const QString &htmlText, const QString &assetType);
static QStringList extractHtmlLinkedAssets(const QString &htmlText, const QString &assetType);

static int lineNumberAtOffset(const QString &text, int offset)
{
    return text.left(qMax(0, offset)).count(QLatin1Char('\n')) + 1;
}

static QString snippetFromLine(const QString &text, int lineNumber, int contextLines = 0)
{
    if (text.isEmpty() || lineNumber <= 0) {
        return {};
    }

    const QStringList lines = text.split(QLatin1Char('\n'));
    if (lineNumber > lines.size()) {
        return {};
    }

    const int start = qMax(0, lineNumber - 1 - contextLines);
    const int end = qMin(lines.size(), lineNumber + contextLines);
    QString snippet = lines.mid(start, end - start).join(QLatin1Char('\n')).trimmed();
    if (snippet.size() > 220) {
        snippet = snippet.left(220).trimmed() + QStringLiteral(" ...");
    }
    return snippet;
}

static QString snippetFromBraceBlock(const QString &text, int startOffset, int maxLines = 18)
{
    if (text.isEmpty() || startOffset < 0 || startOffset >= text.size()) {
        return {};
    }

    const int openBrace = text.indexOf(QLatin1Char('{'), startOffset);
    if (openBrace < 0) {
        return snippetFromLine(text, lineNumberAtOffset(text, startOffset), 2);
    }

    int depth = 0;
    bool inDoubleQuote = false;
    bool inSingleQuote = false;
    bool escaping = false;
    bool inLineComment = false;
    bool inBlockComment = false;
    int endOffset = -1;

    for (int i = openBrace; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        const QChar next = i + 1 < text.size() ? text.at(i + 1) : QChar();

        if (inLineComment) {
            if (ch == QLatin1Char('\n')) {
                inLineComment = false;
            }
            continue;
        }

        if (inBlockComment) {
            if (ch == QLatin1Char('*') && next == QLatin1Char('/')) {
                inBlockComment = false;
                ++i;
            }
            continue;
        }

        if (inDoubleQuote) {
            if (!escaping && ch == QLatin1Char('"')) {
                inDoubleQuote = false;
            }
            escaping = !escaping && ch == QLatin1Char('\\');
            continue;
        }

        if (inSingleQuote) {
            if (!escaping && ch == QLatin1Char('\'')) {
                inSingleQuote = false;
            }
            escaping = !escaping && ch == QLatin1Char('\\');
            continue;
        }

        escaping = false;

        if (ch == QLatin1Char('/') && next == QLatin1Char('/')) {
            inLineComment = true;
            ++i;
            continue;
        }
        if (ch == QLatin1Char('/') && next == QLatin1Char('*')) {
            inBlockComment = true;
            ++i;
            continue;
        }
        if (ch == QLatin1Char('"')) {
            inDoubleQuote = true;
            continue;
        }
        if (ch == QLatin1Char('\'')) {
            inSingleQuote = true;
            continue;
        }

        if (ch == QLatin1Char('{')) {
            ++depth;
        } else if (ch == QLatin1Char('}')) {
            --depth;
            if (depth == 0) {
                endOffset = i + 1;
                break;
            }
        }
    }

    if (endOffset <= startOffset) {
        return snippetFromLine(text, lineNumberAtOffset(text, startOffset), 2);
    }

    QString snippet = text.mid(startOffset, endOffset - startOffset).trimmed();
    QStringList lines = snippet.split(QLatin1Char('\n'));
    while (!lines.isEmpty()) {
        const QString first = lines.first().trimmed();
        if (first.isEmpty() || first == QStringLiteral("}") || first == QStringLiteral("};")
            || first == QStringLiteral("*/") || first == QStringLiteral("public:")
            || first == QStringLiteral("private:") || first == QStringLiteral("protected:")
            || first == QStringLiteral("signals:") || first == QStringLiteral("public slots:")
            || first == QStringLiteral("private slots:") || first == QStringLiteral("protected slots:")) {
            lines.removeFirst();
            continue;
        }
        break;
    }
    snippet = lines.join(QLatin1Char('\n')).trimmed();
    lines = snippet.split(QLatin1Char('\n'));
    if (lines.size() <= maxLines) {
        return snippet;
    }
    return lines.mid(0, maxLines).join(QLatin1Char('\n')) + QStringLiteral("\n...");
}

static bool isControlKeywordName(const QString &name)
{
    static const QSet<QString> keywords = {
        QStringLiteral("if"),
        QStringLiteral("for"),
        QStringLiteral("while"),
        QStringLiteral("switch"),
        QStringLiteral("catch"),
        QStringLiteral("function"),
        QStringLiteral("return"),
        QStringLiteral("else"),
        QStringLiteral("do"),
        QStringLiteral("try")
    };
    return keywords.contains(name);
}

static QVariantMap makeSourceContextItem(const QString &sourcePath,
                                         const QString &sourceLanguage,
                                         int line,
                                         const QString &snippet,
                                         const QString &detail = QString(),
                                         const QString &snippetKind = QStringLiteral("line_excerpt"),
                                         const QString &diagnosticsMode = QStringLiteral("none"))
{
    QString effectiveSnippetKind = snippetKind;
    if (effectiveSnippetKind == QStringLiteral("line_excerpt") && snippet.contains(QLatin1Char('\n'))) {
        effectiveSnippetKind = QStringLiteral("block_excerpt");
    }
    QVariantMap item;
    item.insert(QStringLiteral("sourcePath"), sourcePath);
    item.insert(QStringLiteral("sourceLanguage"), sourceLanguage);
    item.insert(QStringLiteral("line"), line);
    item.insert(QStringLiteral("snippet"), snippet);
    item.insert(QStringLiteral("snippetKind"), effectiveSnippetKind);
    item.insert(QStringLiteral("diagnosticsMode"), diagnosticsMode);
    if (!detail.isEmpty()) {
        item.insert(QStringLiteral("detail"), detail);
    }
    return item;
}

static QString normalizeSignatureLanguage(const QString &language)
{
    if (language == QStringLiteral("script")) {
        return QStringLiteral("js");
    }
    if (language == QStringLiteral("jsx")) {
        return QStringLiteral("jsx");
    }
    if (language == QStringLiteral("tsx")) {
        return QStringLiteral("tsx");
    }
    if (language == QStringLiteral("ts")) {
        return QStringLiteral("ts");
    }
    return language;
}

static bool isCallableSymbolKind(const QString &kind)
{
    return kind == QStringLiteral("function")
        || kind == QStringLiteral("method")
        || kind == QStringLiteral("constructor")
        || kind == QStringLiteral("hook");
}

static QString signatureHeadForSnippet(const QString &text)
{
    QString head = text.trimmed();
    const int brace = head.indexOf(QLatin1Char('{'));
    if (brace >= 0) {
        head = head.left(brace).trimmed();
    }
    const int newline = head.indexOf(QLatin1Char('\n'));
    if (newline >= 0) {
        head = head.left(newline).trimmed();
    }
    return head;
}

static QStringList splitTopLevelSignatureParts(const QString &text)
{
    QStringList parts;
    QString current;
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;
    int angleDepth = 0;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        const QChar prev = i > 0 ? text.at(i - 1) : QChar();
        if (ch == QLatin1Char('\'') && !inDoubleQuote && prev != QLatin1Char('\\')) {
            inSingleQuote = !inSingleQuote;
        } else if (ch == QLatin1Char('"') && !inSingleQuote && prev != QLatin1Char('\\')) {
            inDoubleQuote = !inDoubleQuote;
        }
        if (!inSingleQuote && !inDoubleQuote) {
            if (ch == QLatin1Char('(')) ++parenDepth;
            else if (ch == QLatin1Char(')')) parenDepth = qMax(0, parenDepth - 1);
            else if (ch == QLatin1Char('[')) ++bracketDepth;
            else if (ch == QLatin1Char(']')) bracketDepth = qMax(0, bracketDepth - 1);
            else if (ch == QLatin1Char('{')) ++braceDepth;
            else if (ch == QLatin1Char('}')) braceDepth = qMax(0, braceDepth - 1);
            else if (ch == QLatin1Char('<')) ++angleDepth;
            else if (ch == QLatin1Char('>')) angleDepth = qMax(0, angleDepth - 1);
            else if (ch == QLatin1Char(',') && parenDepth == 0 && bracketDepth == 0
                     && braceDepth == 0 && angleDepth == 0) {
                const QString part = current.trimmed();
                if (!part.isEmpty()) {
                    parts.append(part);
                }
                current.clear();
                continue;
            }
        }
        current += ch;
    }
    const QString part = current.trimmed();
    if (!part.isEmpty()) {
        parts.append(part);
    }
    return parts;
}

static QVariantMap makeSignatureParameter(const QString &name, const QString &type)
{
    QVariantMap item;
    item.insert(QStringLiteral("name"), name.trimmed());
    item.insert(QStringLiteral("type"), type.trimmed());
    return item;
}

static QVariantMap parseSignatureParameter(QString raw, const QString &language)
{
    raw = raw.trimmed();
    if (raw.isEmpty() || raw == QStringLiteral("void")) {
        return {};
    }
    const int equals = raw.indexOf(QLatin1Char('='));
    if (equals >= 0) {
        raw = raw.left(equals).trimmed();
    }
    const int colon = raw.indexOf(QLatin1Char(':'));
    if ((language == QStringLiteral("ts") || language == QStringLiteral("tsx")
         || language == QStringLiteral("qml")) && colon >= 0) {
        QString name = raw.left(colon).trimmed();
        name.remove(QRegularExpression(QStringLiteral(R"(\?)")));
        if (name.contains(QLatin1Char(' '))) {
            name = name.section(QLatin1Char(' '), -1);
        }
        return makeSignatureParameter(name, raw.mid(colon + 1).trimmed());
    }
    if (language == QStringLiteral("php")) {
        const int dollar = raw.lastIndexOf(QLatin1Char('$'));
        if (dollar >= 0) {
            return makeSignatureParameter(raw.mid(dollar).trimmed(), raw.left(dollar).trimmed());
        }
    }
    if (language == QStringLiteral("python")) {
        if (raw == QStringLiteral("self") || raw == QStringLiteral("cls")) {
            return {};
        }
        if (colon >= 0) {
            return makeSignatureParameter(raw.left(colon).trimmed(), raw.mid(colon + 1).trimmed());
        }
        return makeSignatureParameter(raw, QString());
    }
    if (language == QStringLiteral("swift")) {
        const int swiftColon = raw.indexOf(QLatin1Char(':'));
        if (swiftColon >= 0) {
            const QStringList names = raw.left(swiftColon).trimmed().split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
            QString name = names.isEmpty() ? raw.left(swiftColon).trimmed() : names.last();
            if (name == QStringLiteral("_") && !names.isEmpty()) {
                name = names.first();
            }
            return makeSignatureParameter(name, raw.mid(swiftColon + 1).trimmed());
        }
    }
    if (language == QStringLiteral("rust")) {
        if (raw == QStringLiteral("self") || raw == QStringLiteral("&self")
            || raw == QStringLiteral("&mut self") || raw == QStringLiteral("mut self")) {
            return {};
        }
        if (colon >= 0) {
            return makeSignatureParameter(raw.left(colon).trimmed(), raw.mid(colon + 1).trimmed());
        }
    }
    if (language == QStringLiteral("objc")) {
        const QString name = raw.section(QLatin1Char(' '), -1).remove(QRegularExpression(QStringLiteral(R"([*&]+)")));
        return makeSignatureParameter(name, raw.left(raw.lastIndexOf(raw.section(QLatin1Char(' '), -1))).trimmed());
    }

    const QRegularExpression trailingNamePattern(QStringLiteral(R"(([A-Za-z_]\w*)\s*$)"));
    const QRegularExpressionMatch trailingNameMatch = trailingNamePattern.match(raw);
    if (!trailingNameMatch.hasMatch()) {
        return {};
    }
    const QString name = trailingNameMatch.captured(1).trimmed();
    const QString type = raw.left(trailingNameMatch.capturedStart(1)).trimmed();
    return makeSignatureParameter(name, type);
}

static void appendReturnDetail(QVariantList &returns, const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    for (const QVariant &entry : std::as_const(returns)) {
        if (entry.toMap().value(QStringLiteral("text")).toString() == trimmed) {
            return;
        }
    }
    QVariantMap item;
    item.insert(QStringLiteral("text"), trimmed);
    returns.append(item);
}

static QVariantMap enrichCallableSignature(QVariantMap symbol, const QString &language)
{
    if (!isCallableSymbolKind(symbol.value(QStringLiteral("kind")).toString())) {
        return symbol;
    }
    if (!symbol.value(QStringLiteral("signatureSource")).toString().isEmpty()) {
        return symbol; // grammar-derived; the snippet heuristics below would only degrade it
    }

    const QString snippet = symbol.value(QStringLiteral("snippet")).toString();
    if (snippet.trimmed().isEmpty()) {
        if (!symbol.contains(QStringLiteral("parameters"))) {
            symbol.insert(QStringLiteral("parameters"), QVariantList{});
        }
        if (!symbol.contains(QStringLiteral("returns"))) {
            QVariantList noReturn;
            noReturn.append(QVariantMap{{QStringLiteral("text"), QStringLiteral("none")}});
            symbol.insert(QStringLiteral("returns"), noReturn);
        }
        return symbol;
    }

    const QString normalizedLanguage = normalizeSignatureLanguage(language);
    const QString symbolName = symbol.value(QStringLiteral("name")).toString();
    QString parameterText;
    QString explicitReturn;
    const QString head = signatureHeadForSnippet(snippet);
    const QString kind = symbol.value(QStringLiteral("kind")).toString();

    if (head.startsWith(QLatin1Char(':'))) {
        if (!symbol.contains(QStringLiteral("parameters"))) {
            symbol.insert(QStringLiteral("parameters"), QVariantList{});
        }
        if (!symbol.contains(QStringLiteral("returns"))) {
            QVariantList noReturn;
            noReturn.append(QVariantMap{{QStringLiteral("text"), QStringLiteral("none")}});
            symbol.insert(QStringLiteral("returns"), noReturn);
        }
        return symbol;
    }

    auto capture = [&](const QRegularExpression &pattern, int paramsIndex, int returnIndex) {
        const auto match = pattern.match(head);
        if (!match.hasMatch()) {
            return false;
        }
        parameterText = match.captured(paramsIndex).trimmed();
        explicitReturn = returnIndex > 0 ? match.captured(returnIndex).trimmed() : QString();
        return true;
    };

    if (normalizedLanguage == QStringLiteral("python")) {
        capture(QRegularExpression(QStringLiteral(R"(^\s*def\s+[A-Za-z_]\w*\s*\(([^)]*)\)\s*(?:->\s*([^:]+))?)")), 1, 2);
    } else if (normalizedLanguage == QStringLiteral("php")) {
        capture(QRegularExpression(QStringLiteral(R"(\bfunction\s+[A-Za-z_]\w*\s*\(([^)]*)\)\s*(?::\s*([^\s{]+))?)")), 1, 2);
    } else if (normalizedLanguage == QStringLiteral("swift")) {
        if (!capture(QRegularExpression(QStringLiteral(R"(\bfunc\s+[A-Za-z_]\w*\s*\(([^)]*)\)\s*(?:->\s*([^{]+))?)")), 1, 2)) {
            capture(QRegularExpression(QStringLiteral(R"(\binit\s*\(([^)]*)\))")), 1, -1);
        }
    } else if (normalizedLanguage == QStringLiteral("rust")) {
        capture(QRegularExpression(QStringLiteral(R"(\bfn\s+[A-Za-z_]\w*\s*\(([^)]*)\)\s*(?:->\s*([^{]+))?)")), 1, 2);
    } else if (normalizedLanguage == QStringLiteral("objc")) {
        const auto match = QRegularExpression(QStringLiteral(R"(^\s*[-+]\s*\(([^)]+)\)\s*([A-Za-z_]\w*(?::[A-Za-z_]\w*)*))")).match(head);
        if (match.hasMatch()) {
            explicitReturn = match.captured(1).trimmed();
            auto paramIt = QRegularExpression(QStringLiteral(R"(:\s*\(([^)]+)\)\s*([A-Za-z_]\w*))")).globalMatch(head);
            QStringList objcParams;
            while (paramIt.hasNext()) {
                const auto paramMatch = paramIt.next();
                objcParams.append(paramMatch.captured(2) + QStringLiteral(" : ") + paramMatch.captured(1));
            }
            parameterText = objcParams.join(QStringLiteral(", "));
        }
    } else {
        bool matched = false;
        if (!symbolName.isEmpty()) {
            const QString escapedName = QRegularExpression::escape(symbolName);
            const auto cStyle = QRegularExpression(
                                    QStringLiteral(R"(^\s*(.+?)\b%1\s*\(([^)]*)\)\s*(?:const\b)?\s*(?:->\s*([^{]+))?)")
                                        .arg(escapedName))
                                    .match(head);
            if (cStyle.hasMatch()) {
                parameterText = cStyle.captured(2).trimmed();
                explicitReturn = cStyle.captured(3).trimmed();
                if (explicitReturn.isEmpty()) {
                    QString prefix = cStyle.captured(1).trimmed();
                    const QStringList dropTokens = {
                        QStringLiteral("public"), QStringLiteral("private"), QStringLiteral("protected"),
                        QStringLiteral("static"), QStringLiteral("virtual"), QStringLiteral("inline"),
                        QStringLiteral("constexpr"), QStringLiteral("final"), QStringLiteral("override"),
                        QStringLiteral("abstract"), QStringLiteral("async"), QStringLiteral("synchronized"),
                        QStringLiteral("extern"), QStringLiteral("sealed"), QStringLiteral("function")
                    };
                    QStringList prefixTokens = prefix.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
                    while (!prefixTokens.isEmpty() && dropTokens.contains(prefixTokens.first())) {
                        prefixTokens.removeFirst();
                    }
                    explicitReturn = prefixTokens.join(QLatin1Char(' ')).trimmed();
                }
                matched = true;
            }
        }
        if (!matched) {
            matched = capture(QRegularExpression(QStringLiteral(R"(\bfunction\s+[A-Za-z_]\w*\s*\(([^)]*)\)\s*(?::\s*([^{=]+))?)")), 1, 2);
        }
        if (!matched) {
            capture(QRegularExpression(QStringLiteral(R"(\b(?:constructor|[A-Za-z_]\w*)\s*\(([^)]*)\)\s*(?::\s*([^{=]+))?)")), 1, 2);
        }
    }

    QVariantList parameters;
    for (const QString &part : splitTopLevelSignatureParts(parameterText)) {
        const QVariantMap parameter = parseSignatureParameter(part, normalizedLanguage);
        if (!parameter.isEmpty()) {
            parameters.append(parameter);
        }
    }

    QVariantList returns;
    if (!explicitReturn.isEmpty()) {
        appendReturnDetail(returns, explicitReturn);
    } else if (kind == QStringLiteral("constructor")) {
        appendReturnDetail(returns, QStringLiteral("none"));
    } else {
        auto returnIt = QRegularExpression(QStringLiteral(R"(\breturn\b\s*([^;\n]+))")).globalMatch(snippet);
        while (returnIt.hasNext() && returns.size() < 3) {
            const QString captured = returnIt.next().captured(1).trimmed();
            // Skip bare open-delimiters from multi-line object/array/call literals
            if (captured != QStringLiteral("{") && captured != QStringLiteral("(")
                    && captured != QStringLiteral("[")) {
                appendReturnDetail(returns, captured);
            }
        }
        if (returns.isEmpty()) {
            appendReturnDetail(returns, QStringLiteral("none"));
        }
    }

    symbol.insert(QStringLiteral("parameters"), parameters);
    symbol.insert(QStringLiteral("returns"), returns);
    return symbol;
}

static QVariantList enrichSymbolSignatures(const QVariantList &symbols, const QString &language)
{
    QVariantList enriched;
    enriched.reserve(symbols.size());
    for (const QVariant &entry : symbols) {
        QVariantMap symbol = entry.toMap();
        symbol.insert(QStringLiteral("members"),
                      enrichSymbolSignatures(symbol.value(QStringLiteral("members")).toList(), language));
        enriched.append(enrichCallableSignature(symbol, language));
    }
    return enriched;
}

static QVariantMap enrichAnalysisSignatures(QVariantMap result)
{
    result.insert(QStringLiteral("symbols"),
                  enrichSymbolSignatures(result.value(QStringLiteral("symbols")).toList(),
                                         result.value(QStringLiteral("language")).toString()));
    return result;
}

static QString detectLanguageForWebLinks(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("ts") || suffix == QStringLiteral("mts") || suffix == QStringLiteral("cts")) {
        return QStringLiteral("ts");
    }
    if (suffix == QStringLiteral("tsx") || suffix == QStringLiteral("jsx")) {
        return suffix;
    }
    return QStringLiteral("script");
}

// ---------------------------------------------------------------------------
// Web links: HTML <-> CSS <-> JavaScript (see weblinks.h)
// ---------------------------------------------------------------------------

static QMap<QString, QVariantMap> cachedCssClassIndex(const QString &cssPath)
{
    struct Entry
    {
        qint64 size = -1;
        QDateTime modified;
        QMap<QString, QVariantMap> index;
    };
    static QMutex mutex;
    static QHash<QString, Entry> cache;
    const QFileInfo info(cssPath);
    if (!info.exists() || shouldSkipFileBySize(info, kMaxAuxiliaryFileBytes)) {
        return {};
    }
    {
        QMutexLocker locker(&mutex);
        const auto it = cache.constFind(info.absoluteFilePath());
        if (it != cache.constEnd() && it->size == info.size() && it->modified == info.lastModified()) {
            return it->index;
        }
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    Entry entry;
    entry.size = info.size();
    entry.modified = info.lastModified();
    entry.index = buildCssClassIndex(info.absoluteFilePath(), QString::fromUtf8(file.readAll()));
    QMutexLocker locker(&mutex);
    cache.insert(info.absoluteFilePath(), entry);
    return entry.index;
}

// Text with the same line structure as the host file, containing only `content`
// starting at `startLine`, so an embedded block parses with host line numbers.
static QString linePaddedBlock(const QString &content, int startLine)
{
    return QString(qMax(0, startLine - 1), QLatin1Char('\n')) + content;
}

// CSS classes (name -> entry with path/line/snippet) available to a page: its
// linked local stylesheets and its inline <style> blocks.
static QMap<QString, QVariantMap> cssClassesForPage(const WebLinks::HtmlPage &page)
{
    QMap<QString, QVariantMap> classes;
    for (const WebLinks::HtmlAsset &asset : page.assets) {
        if (asset.kind != QStringLiteral("stylesheet") || !asset.local || !asset.exists) {
            continue;
        }
        const QMap<QString, QVariantMap> index = cachedCssClassIndex(asset.resolvedPath);
        for (auto it = index.constBegin(); it != index.constEnd(); ++it) {
            if (!classes.contains(it.key())) {
                classes.insert(it.key(), it.value());
            }
        }
    }
    for (const WebLinks::HtmlInlineBlock &block : page.inlineBlocks) {
        if (block.kind != QStringLiteral("style")) {
            continue;
        }
        const QMap<QString, QVariantMap> index = buildCssClassIndex(page.path, linePaddedBlock(block.content, block.startLine));
        for (auto it = index.constBegin(); it != index.constEnd(); ++it) {
            if (!classes.contains(it.key())) {
                classes.insert(it.key(), it.value());
            }
        }
    }
    return classes;
}

// Text of a linked local script worth scanning for DOM references (not huge,
// not a minified bundle); empty otherwise.
static QString readScanableScript(const QString &scriptPath)
{
    const QFileInfo info(scriptPath);
    if (!info.exists() || shouldSkipFileBySize(info, kMaxAuxiliaryFileBytes)) {
        return {};
    }
    QFile file(scriptPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    const QString text = QString::fromUtf8(file.readAll());
    if (looksLikeMinifiedSource(scriptPath, QStringLiteral("script"), text)) {
        return {};
    }
    return text;
}

static QVariantMap makeWebLinkItem(const QString &type,
                                   const QString &label,
                                   const QString &targetPath,
                                   const QString &targetLanguage,
                                   int line,
                                   const QString &snippet,
                                   const QString &detail,
                                   bool exists = true)
{
    QVariantMap item = makeSourceContextItem(targetPath, targetLanguage, line, snippet, detail);
    item.insert(QStringLiteral("label"), label);
    item.insert(QStringLiteral("target"), QFileInfo(targetPath).fileName());
    item.insert(QStringLiteral("type"), type);
    item.insert(QStringLiteral("path"), targetPath);
    item.insert(QStringLiteral("targetPath"), targetPath);
    item.insert(QStringLiteral("language"), targetLanguage);
    item.insert(QStringLiteral("exists"), exists);
    item.insert(QStringLiteral("sourceMode"), QStringLiteral("ast"));
    item.insert(QStringLiteral("confidence"), exists ? QStringLiteral("high") : QStringLiteral("medium"));
    return item;
}

static QString fileLineLabel(const QString &path, int line)
{
    return line > 0 ? QStringLiteral("%1:%2").arg(QFileInfo(path).fileName()).arg(line)
                    : QFileInfo(path).fileName();
}

static QVariantMap makeCrossFileRelation(const QString &kind, const QString &name, const QString &path,
                                         const QString &language, int line, const QString &snippet,
                                         const QString &detail)
{
    QVariantMap relation;
    relation.insert(QStringLiteral("kind"), kind);
    relation.insert(QStringLiteral("name"), name);
    relation.insert(QStringLiteral("line"), line);
    relation.insert(QStringLiteral("detail"), detail);
    relation.insert(QStringLiteral("snippet"), snippet);
    relation.insert(QStringLiteral("path"), path);
    relation.insert(QStringLiteral("sourcePath"), path);
    relation.insert(QStringLiteral("language"), language);
    relation.insert(QStringLiteral("sourceLanguage"), language);
    relation.insert(QStringLiteral("sourceMode"), QStringLiteral("ast"));
    relation.insert(QStringLiteral("confidence"), QStringLiteral("high"));
    relation.insert(QStringLiteral("calls"), QVariantList{});
    relation.insert(QStringLiteral("calledBy"), QVariantList{});
    return relation;
}

static QString handlerSymbolName(const WebLinks::HtmlHandler &handler)
{
    return QStringLiteral("<%1 %2>").arg(handler.tag, handler.attribute);
}

// JS/TS file: consumer pages, DOM references resolved to HTML elements and
// CSS rules, and `calledBy` edges from inline HTML event handlers.
static void enrichScriptWithWebLinks(QVariantMap &result, const QString &path, const QString &text)
{
    const QList<WebLinks::HtmlPage> pages = WebLinks::pagesReferencingAsset(path, QStringLiteral("script"));
    QVariantList quickLinks;
    for (const QVariant &entry : result.value(QStringLiteral("quickLinks")).toList()) {
        if (entry.toMap().value(QStringLiteral("type")).toString() != QStringLiteral("consumer")) {
            quickLinks.append(entry);
        }
    }
    const QString absolute = QFileInfo(path).absoluteFilePath();
    for (const WebLinks::HtmlPage &page : pages) {
        int line = 1;
        QString snippet;
        for (const WebLinks::HtmlAsset &asset : page.assets) {
            if (asset.kind == QStringLiteral("script") && asset.resolvedPath == absolute) {
                line = asset.line;
                snippet = asset.snippet;
                break;
            }
        }
        quickLinks.append(makeWebLinkItem(QStringLiteral("consumer"), QFileInfo(page.path).fileName(), page.path,
                                          QStringLiteral("html"), line, snippet,
                                          QStringLiteral("referenced by HTML file")));
    }

    if (!pages.isEmpty()) {
        QHash<QString, QMap<QString, QVariantMap>> cssByPage;
        for (const WebLinks::HtmlPage &page : pages) {
            cssByPage.insert(page.path, cssClassesForPage(page));
        }
        QSet<QString> seenLinks;
        int domLinks = 0;
        for (const WebLinks::DomReference &ref : WebLinks::extractDomReferences(text)) {
            if (domLinks >= 80) {
                break;
            }
            const QString key = ref.kind + QLatin1Char('|') + ref.name;
            if (seenLinks.contains(key)) {
                continue;
            }
            seenLinks.insert(key);
            const QString via = QStringLiteral("%1 at line %2").arg(ref.via).arg(ref.line);
            bool resolved = false;
            if (ref.kind == QStringLiteral("id")) {
                for (const WebLinks::HtmlPage &page : pages) {
                    if (const WebLinks::HtmlElement *element = page.elementWithId(ref.name)) {
                        quickLinks.append(makeWebLinkItem(QStringLiteral("dom-id"),
                                                          QStringLiteral("#%1 → %2").arg(ref.name, fileLineLabel(page.path, element->line)),
                                                          page.path, QStringLiteral("html"), element->line, element->snippet, via));
                        resolved = true;
                        break;
                    }
                }
                if (!resolved) {
                    quickLinks.append(makeWebLinkItem(QStringLiteral("dom-id-missing"),
                                                      QStringLiteral("#%1 — no such element in %2").arg(ref.name, QFileInfo(pages.first().path).fileName()),
                                                      path, detectLanguageForWebLinks(path), ref.line, ref.snippet, via, false));
                }
            } else if (ref.kind == QStringLiteral("class")) {
                for (const WebLinks::HtmlPage &page : pages) {
                    const QMap<QString, QVariantMap> &css = cssByPage[page.path];
                    if (css.contains(ref.name)) {
                        const QVariantMap rule = css.value(ref.name);
                        const QString rulePath = rule.value(QStringLiteral("path")).toString();
                        const int ruleLine = rule.value(QStringLiteral("line")).toInt();
                        quickLinks.append(makeWebLinkItem(QStringLiteral("css-class"),
                                                          QStringLiteral(".%1 → %2").arg(ref.name, fileLineLabel(rulePath, ruleLine)),
                                                          rulePath, QStringLiteral("css"), ruleLine,
                                                          rule.value(QStringLiteral("snippet")).toString(), via));
                        resolved = true;
                        break;
                    }
                    const QList<const WebLinks::HtmlElement *> elements = page.elementsWithClass(ref.name);
                    if (!elements.isEmpty()) {
                        quickLinks.append(makeWebLinkItem(QStringLiteral("dom-class"),
                                                          QStringLiteral(".%1 → %2").arg(ref.name, fileLineLabel(page.path, elements.first()->line)),
                                                          page.path, QStringLiteral("html"), elements.first()->line,
                                                          elements.first()->snippet, via));
                        resolved = true;
                        break;
                    }
                }
                if (!resolved) {
                    quickLinks.append(makeWebLinkItem(QStringLiteral("css-class-missing"),
                                                      QStringLiteral(".%1 — no rule or element in linked pages").arg(ref.name),
                                                      path, detectLanguageForWebLinks(path), ref.line, ref.snippet, via, false));
                }
            }
            ++domLinks;
        }

        // Inline event handlers in consumer pages that call this file's functions.
        QVariantList symbols = result.value(QStringLiteral("symbols")).toList();
        bool changed = false;
        for (int index = 0; index < symbols.size(); ++index) {
            QVariantMap symbol = symbols.at(index).toMap();
            const QString name = symbol.value(QStringLiteral("name")).toString();
            if (!isCallableSymbolKind(symbol.value(QStringLiteral("kind")).toString())
                && symbol.value(QStringLiteral("kind")).toString() != QStringLiteral("component")) {
                continue;
            }
            QVariantList calledBy = symbol.value(QStringLiteral("calledBy")).toList();
            for (const WebLinks::HtmlPage &page : pages) {
                for (const WebLinks::HtmlHandler &handler : page.handlers) {
                    if (handler.calledNames.contains(name)) {
                        calledBy.append(makeCrossFileRelation(QStringLiteral("handler"), handlerSymbolName(handler),
                                                              page.path, QStringLiteral("html"), handler.line,
                                                              handler.snippet, handler.code));
                        changed = true;
                    }
                }
            }
            symbol.insert(QStringLiteral("calledBy"), calledBy);
            symbols[index] = symbol;
        }
        if (changed) {
            result.insert(QStringLiteral("symbols"), symbols);
        }
    }

    // Custom elements this file defines, and the pages that use them.
    const QHash<QString, QString> definitions = WebLinks::extractCustomElementDefinitions(text);
    if (!definitions.isEmpty()) {
        const QList<WebLinks::HtmlPage> usingPages = pages.isEmpty()
            ? WebLinks::pagesReferencingAsset(path, QStringLiteral("script"))
            : pages;
        for (auto it = definitions.constBegin(); it != definitions.constEnd(); ++it) {
            for (const WebLinks::HtmlPage &page : usingPages) {
                for (const WebLinks::HtmlElement &element : page.customElements) {
                    if (element.tag == it.key()) {
                        quickLinks.append(makeWebLinkItem(QStringLiteral("custom-element"),
                                                          QStringLiteral("<%1> (%2) used in %3").arg(it.key(), it.value(), fileLineLabel(page.path, element.line)),
                                                          page.path, QStringLiteral("html"), element.line, element.snippet,
                                                          QStringLiteral("custom element")));
                        break;
                    }
                }
            }
        }
    }
    result.insert(QStringLiteral("quickLinks"), quickLinks);
}


// ---------------------------------------------------------------------------
// PHP links: use/require/include dependencies, framework routes, template assets
// ---------------------------------------------------------------------------

struct PhpAutoloadRoot
{
    QString prefix; // namespace prefix with trailing backslash
    QString directory; // absolute
};

// PSR-4 roots from the nearest composer.json above the file (cached).
static QList<PhpAutoloadRoot> phpAutoloadRoots(const QString &filePath)
{
    static QMutex mutex;
    static QHash<QString, QList<PhpAutoloadRoot>> cache;
    QDir dir = QFileInfo(filePath).dir();
    for (int level = 0; level < 8; ++level) {
        const QString composerPath = dir.absoluteFilePath(QStringLiteral("composer.json"));
        if (QFileInfo::exists(composerPath)) {
            {
                QMutexLocker locker(&mutex);
                if (cache.contains(composerPath)) {
                    return cache.value(composerPath);
                }
            }
            QList<PhpAutoloadRoot> roots;
            QFile file(composerPath);
            if (file.open(QIODevice::ReadOnly)) {
                const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
                for (const QString &section : {QStringLiteral("autoload"), QStringLiteral("autoload-dev")}) {
                    const QJsonObject psr4 = root.value(section).toObject().value(QStringLiteral("psr-4")).toObject();
                    for (auto it = psr4.constBegin(); it != psr4.constEnd(); ++it) {
                        QStringList directories;
                        if (it.value().isArray()) {
                            for (const QJsonValue &value : it.value().toArray()) {
                                directories.append(value.toString());
                            }
                        } else {
                            directories.append(it.value().toString());
                        }
                        for (const QString &directory : std::as_const(directories)) {
                            roots.append({it.key(), QDir::cleanPath(dir.absoluteFilePath(directory))});
                        }
                    }
                }
            }
            std::sort(roots.begin(), roots.end(), [](const PhpAutoloadRoot &a, const PhpAutoloadRoot &b) {
                return a.prefix.size() > b.prefix.size();
            });
            QMutexLocker locker(&mutex);
            cache.insert(composerPath, roots);
            return roots;
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return {};
}

static QString resolvePhpClassPath(const QString &filePath, const QString &className)
{
    const QString normalized = className.startsWith(QLatin1Char('\\')) ? className.mid(1) : className;
    for (const PhpAutoloadRoot &root : phpAutoloadRoots(filePath)) {
        if (!normalized.startsWith(root.prefix)) {
            continue;
        }
        QString relative = normalized.mid(root.prefix.size());
        relative.replace(QLatin1Char('\\'), QLatin1Char('/'));
        const QString candidate = QDir(root.directory).absoluteFilePath(relative + QStringLiteral(".php"));
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

// require 'x.php' / __DIR__ . '/x.php' / CONSTANT . 'core/x.php': resolve the
// literal part relative to the file, then to its ancestors (constants usually
// name a project root).
static QString resolvePhpIncludePath(const QString &filePath, const QString &literal)
{
    QString relative = literal.trimmed();
    if (relative.isEmpty()) {
        return {};
    }
    QDir dir = QFileInfo(filePath).dir();
    const QString direct = QDir::cleanPath(dir.absoluteFilePath(relative.startsWith(QLatin1Char('/')) ? relative.mid(1) : relative));
    if (QFileInfo::exists(direct)) {
        return direct;
    }
    for (int level = 0; level < 6 && dir.cdUp(); ++level) {
        const QString candidate = QDir::cleanPath(dir.absoluteFilePath(relative.startsWith(QLatin1Char('/')) ? relative.mid(1) : relative));
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

// Java imports resolved to source files: `package a.b;` gives the source
// root, `import a.b.C;` is root/a/b/C.java (also in sibling source sets,
// e.g. src/test/java -> src/main/java); static imports bind the member.
static void resolveJavaImports(QVariantMap &analysis, const QString &path, const QString &text)
{
    static const QRegularExpression packagePattern(QStringLiteral(R"(^\s*package\s+([A-Za-z_][\w.]*)\s*;)"),
                                                   QRegularExpression::MultilineOption);
    const QString package = packagePattern.match(text).captured(1);
    QDir root = QFileInfo(path).absoluteDir();
    for (int up = 0; up < package.count(QLatin1Char('.')) + (package.isEmpty() ? 0 : 1); ++up) {
        if (!root.cdUp()) {
            return;
        }
    }
    QStringList roots{root.absolutePath()};
    QDir sourceSets(root.absolutePath());
    const QString flavour = sourceSets.dirName(); // java / kotlin
    if (sourceSets.cdUp() && sourceSets.cdUp() && sourceSets.dirName() == QStringLiteral("src")) {
        for (const QString &set : sourceSets.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString candidate = sourceSets.filePath(set + QLatin1Char('/') + flavour);
            if (!roots.contains(candidate) && QFileInfo(candidate).isDir()) {
                roots.append(candidate);
            }
        }
    }
    static const QRegularExpression staticPrefix(QStringLiteral(R"(^static\s+)"));
    QVariantList dependencies = analysis.value(QStringLiteral("dependencies")).toList();
    for (int index = 0; index < dependencies.size(); ++index) {
        QVariantMap item = dependencies.at(index).toMap();
        QString target = item.value(QStringLiteral("target")).toString().trimmed();
        const bool isStatic = staticPrefix.match(target).hasMatch()
            || item.value(QStringLiteral("snippet")).toString().contains(QStringLiteral("import static"));
        target.remove(staticPrefix);
        if (target.endsWith(QStringLiteral(".*")) || !item.value(QStringLiteral("path")).toString().isEmpty()) {
            continue;
        }
        QStringList parts = target.split(QLatin1Char('.'), Qt::SkipEmptyParts);
        QString member;
        if (isStatic && parts.size() > 1) {
            member = parts.takeLast();
        }
        if (parts.isEmpty()) {
            continue;
        }
        const QString local = member.isEmpty() ? parts.last() : member;
        const QVariantList bindings{QVariantMap{{QStringLiteral("local"), local}, {QStringLiteral("imported"), local}}};
        bool found = false;
        // a.b.C, then a.b.C.Nested -> C.java
        for (int length = parts.size(); length > 0 && !found; --length) {
            for (const QString &sourceRoot : std::as_const(roots)) {
                const QString candidate = sourceRoot + QLatin1Char('/') + parts.mid(0, length).join(QLatin1Char('/'))
                    + QStringLiteral(".java");
                if (QFileInfo(candidate).isFile()) {
                    item.insert(QStringLiteral("path"), QFileInfo(candidate).absoluteFilePath());
                    item.insert(QStringLiteral("bindings"), bindings);
                    found = true;
                    break;
                }
            }
            if (length < parts.size() - 1) {
                break; // at most one level of nesting
            }
        }
        // Outside the project's own package family: a library import, bound
        // so its names are never linked to project code.
        const QString family = package.section(QLatin1Char('.'), 0, 1);
        if (!found && !family.isEmpty() && !target.startsWith(family + QLatin1Char('.'))) {
            item.insert(QStringLiteral("bindings"), bindings);
        }
        dependencies[index] = item;
    }
    analysis.insert(QStringLiteral("dependencies"), dependencies);
}

static void enrichPhpAnalysis(QVariantMap &result, const QString &path, const QString &text)
{
    QVariantList dependencies = result.value(QStringLiteral("dependencies")).toList();
    QSet<QString> seen;
    for (const QVariant &entry : std::as_const(dependencies)) {
        seen.insert(entry.toMap().value(QStringLiteral("target")).toString());
    }
    auto addDependency = [&](const QString &target, const QString &type, int offset, const QString &resolved,
                             const QString &label = QString(), const QString &localName = QString()) {
        if (target.isEmpty() || seen.contains(type + target) || dependencies.size() >= 120) {
            return;
        }
        seen.insert(type + target);
        const int line = lineNumberAtOffset(text, offset);
        QVariantMap item = makeSourceContextItem(path, QStringLiteral("php"), line, snippetFromLine(text, line, 0),
                                                 QStringLiteral("%1 dependency").arg(type));
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("label"), label.isEmpty() ? target : label);
        item.insert(QStringLiteral("path"), resolved);
        item.insert(QStringLiteral("exists"), resolved.isEmpty() ? true : QFileInfo::exists(resolved));
        if (type == QStringLiteral("use") && !resolved.isEmpty()) {
            // `use App\Mail\Mailer [as M]` binds the short (or alias) name to the class.
            const QString imported = target.section(QLatin1Char('\\'), -1);
            item.insert(QStringLiteral("bindings"), QVariantList{QVariantMap{
                {QStringLiteral("local"), localName.isEmpty() ? imported : localName},
                {QStringLiteral("imported"), imported}}});
        }
        dependencies.append(item);
    };

    // use Foo\Bar; use Foo\{A, B as C}; use function Foo\bar;
    static const QRegularExpression usePattern(
        QStringLiteral(R"(^[ \t]*use\s+(?:function\s+|const\s+)?([\\A-Za-z_][\\\w]*)(?:\s*\\\s*\{([^}]*)\})?[^;]*;)"),
        QRegularExpression::MultilineOption);
    auto useIt = usePattern.globalMatch(text);
    while (useIt.hasNext()) {
        const auto match = useIt.next();
        QString base = match.captured(1);
        const QString group = match.captured(2);
        static const QRegularExpression aliasPattern(QStringLiteral(R"(\s+as\s+([A-Za-z_]\w*))"));
        QStringList names;
        QStringList aliases;
        if (!group.isEmpty()) {
            if (base.endsWith(QLatin1Char('\\'))) {
                base.chop(1);
            }
            for (QString part : group.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const QString alias = aliasPattern.match(part).captured(1);
                part = part.trimmed().section(QRegularExpression(QStringLiteral(R"(\s+as\s+)")), 0, 0).trimmed();
                if (!part.isEmpty()) {
                    names.append(base + QLatin1Char('\\') + part);
                    aliases.append(alias);
                }
            }
        } else {
            names.append(base);
            aliases.append(aliasPattern.match(match.captured(0)).captured(1));
        }
        for (int index = 0; index < names.size(); ++index) {
            const QString &name = names.at(index);
            addDependency(name, QStringLiteral("use"), match.capturedStart(0), resolvePhpClassPath(path, name),
                          name.section(QLatin1Char('\\'), -1), aliases.at(index));
        }
    }

    // require / include (_once) with a string literal part.
    static const QRegularExpression includePattern(
        QStringLiteral(R"(\b(require|include)(_once)?\s*\(?\s*([^;]*?)\s*\)?\s*;)"));
    static const QRegularExpression literalPattern(QStringLiteral(R"(['"]([^'"]+\.(?:php|inc|phtml|html))['"])"));
    auto includeIt = includePattern.globalMatch(text);
    while (includeIt.hasNext()) {
        const auto match = includeIt.next();
        const QString expression = match.captured(3);
        const auto literal = literalPattern.match(expression);
        if (!literal.hasMatch()) {
            continue;
        }
        const QString target = literal.captured(1);
        addDependency(target, match.captured(1) + match.captured(2), match.capturedStart(0),
                      resolvePhpIncludePath(path, target), QFileInfo(target).fileName());
    }
    result.insert(QStringLiteral("dependencies"), dependencies);

    // Routes: Slim/Lumen $app->get('/x', ...), $group->post(...), ->map([...], '/x'),
    // Laravel Route::get('/x', ...), CodeIgniter $route['x'] = 'controller/method'.
    QVariantList routes = result.value(QStringLiteral("routes")).toList();
    static const QRegularExpression routeCall(
        QStringLiteral(R"((\$\w+|Route)\s*(?:->|::)\s*(get|post|put|patch|delete|options|any|map|match)\s*\(\s*(?:(\[[^\]]*\])\s*,\s*)?['"]([^'"]*)['"])"),
        QRegularExpression::CaseInsensitiveOption);
    auto routeIt = routeCall.globalMatch(text);
    while (routeIt.hasNext() && routes.size() < 200) {
        const auto match = routeIt.next();
        const QString routePath = match.captured(4);
        QString method = match.captured(2).toUpper();
        if (!match.captured(3).isEmpty()) {
            QString methods = match.captured(3);
            methods.remove(QRegularExpression(QStringLiteral(R"([\[\]'"\s])")));
            method = methods.contains(QLatin1Char('$')) ? QStringLiteral("ANY")
                                                          : methods.toUpper().replace(QLatin1Char(','), QLatin1Char('|'));
        }
        if (!(routePath.startsWith(QLatin1Char('/')) || routePath.startsWith(QLatin1Char('{')) || match.captured(1) == QStringLiteral("Route"))) {
            continue;
        }
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        QVariantMap route = makeSourceContextItem(path, QStringLiteral("php"), line, snippetFromLine(text, line, 1),
                                                  QStringLiteral("route"));
        route.insert(QStringLiteral("owner"), match.captured(1));
        route.insert(QStringLiteral("method"), method);
        route.insert(QStringLiteral("path"), routePath);
        route.insert(QStringLiteral("label"), method + QStringLiteral(" ") + routePath);
        routes.append(route);
    }
    static const QRegularExpression ciRoute(QStringLiteral(R"(^\s*\$route\[\s*['"]([^'"]+)['"]\s*\]\s*=\s*['"]([^'"]*)['"])"),
                                            QRegularExpression::MultilineOption);
    auto ciIt = ciRoute.globalMatch(text);
    while (ciIt.hasNext() && routes.size() < 200) {
        const auto match = ciIt.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        QVariantMap route = makeSourceContextItem(path, QStringLiteral("php"), line, snippetFromLine(text, line, 0),
                                                  QStringLiteral("route"));
        route.insert(QStringLiteral("owner"), QStringLiteral("$route"));
        route.insert(QStringLiteral("method"), QStringLiteral("ANY"));
        route.insert(QStringLiteral("path"), match.captured(1));
        route.insert(QStringLiteral("handler"), match.captured(2));
        route.insert(QStringLiteral("label"), QStringLiteral("%1 → %2").arg(match.captured(1), match.captured(2)));
        routes.append(route);
    }
    result.insert(QStringLiteral("routes"), routes);

    // Templates: a PHP file that renders HTML links assets like a page does.
    if (text.contains(QStringLiteral("<script")) || text.contains(QStringLiteral("<link"))
        || text.contains(QStringLiteral("<form"))) {
        // Blank the PHP regions (keeping newlines) so the HTML grammar sees
        // only the template markup, with unchanged line numbers.
        QString markup = text;
        int searchFrom = 0;
        while (true) {
            const int open = markup.indexOf(QStringLiteral("<?"), searchFrom);
            if (open < 0) {
                break;
            }
            int close = markup.indexOf(QStringLiteral("?>"), open + 2);
            const int end = close < 0 ? markup.size() : close + 2;
            for (int index = open; index < end; ++index) {
                if (markup.at(index) != QLatin1Char('\n')) {
                    markup[index] = QLatin1Char(' ');
                }
            }
            searchFrom = end;
        }
        const WebLinks::HtmlPage page = WebLinks::parseHtmlPage(path, markup);
        QVariantList quickLinks = result.value(QStringLiteral("quickLinks")).toList();
        for (const WebLinks::HtmlAsset &asset : page.assets) {
            if (asset.kind == QStringLiteral("page") || !asset.local) {
                continue;
            }
            QVariantMap item = makeSourceContextItem(path, QStringLiteral("php"), asset.line, asset.snippet,
                                                     QStringLiteral("%1 link").arg(asset.kind));
            item.insert(QStringLiteral("label"), QFileInfo(asset.target).fileName().isEmpty() ? asset.target : QFileInfo(asset.target).fileName());
            item.insert(QStringLiteral("target"), asset.target);
            item.insert(QStringLiteral("type"), asset.kind);
            item.insert(QStringLiteral("path"), asset.resolvedPath);
            item.insert(QStringLiteral("targetPath"), asset.resolvedPath);
            item.insert(QStringLiteral("exists"), asset.exists);
            quickLinks.append(item);
        }
        result.insert(QStringLiteral("quickLinks"), quickLinks);
    }
    if (result.value(QStringLiteral("relatedFiles")).toList().isEmpty()) {
        result.insert(QStringLiteral("relatedFiles"), SymbolParser::findRelatedFilesPublic(path));
    }
}


// Swift: `import Module` / `@testable import Module` / `import struct Module.Type`.
static void enrichSwiftDependencies(QVariantMap &result, const QString &path, const QString &text)
{
    QVariantList dependencies = result.value(QStringLiteral("dependencies")).toList();
    if (!dependencies.isEmpty()) {
        return;
    }
    static const QRegularExpression importPattern(
        QStringLiteral(R"(^[ \t]*(@testable\s+)?import\s+(?:(?:typealias|struct|class|enum|protocol|let|var|func)\s+)?([A-Za-z_][\w.]*))"),
        QRegularExpression::MultilineOption);
    auto it = importPattern.globalMatch(text);
    QSet<QString> seen;
    while (it.hasNext()) {
        const auto match = it.next();
        const QString module = match.captured(2);
        if (seen.contains(module)) {
            continue;
        }
        seen.insert(module);
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        QVariantMap item = makeSourceContextItem(path, QStringLiteral("swift"), line, snippetFromLine(text, line, 0),
                                                 QStringLiteral("import dependency"));
        item.insert(QStringLiteral("target"), module);
        item.insert(QStringLiteral("type"), match.captured(1).isEmpty() ? QStringLiteral("import") : QStringLiteral("testable import"));
        item.insert(QStringLiteral("label"), module);
        item.insert(QStringLiteral("path"), QString());
        item.insert(QStringLiteral("exists"), true);
        dependencies.append(item);
    }
    result.insert(QStringLiteral("dependencies"), dependencies);
}

// Java web routes: Spring (@RequestMapping / @GetMapping ...) and JAX-RS
// (@Path + @GET ...), combining a class-level prefix with method mappings.
static void enrichJavaRoutes(QVariantMap &result, const QString &path, const QString &text)
{
    if (!text.contains(QStringLiteral("Mapping")) && !text.contains(QStringLiteral("@Path"))) {
        return;
    }
    QVariantList routes = result.value(QStringLiteral("routes")).toList();
    static const QRegularExpression annotationPattern(
        QStringLiteral(R"(@(RequestMapping|GetMapping|PostMapping|PutMapping|DeleteMapping|PatchMapping|Path|GET|POST|PUT|DELETE|PATCH|HEAD|OPTIONS)\b(\s*\(([^)]*)\))?)"));
    static const QRegularExpression pathValue(QStringLiteral(R"re((?:value\s*=\s*|path\s*=\s*)?\{?\s*"([^"]*)")re"));
    static const QRegularExpression methodValue(QStringLiteral(R"(RequestMethod\.([A-Z]+))"));
    static const QRegularExpression classDecl(QStringLiteral(R"(\b(class|interface)\s+\w+)"));

    const auto firstClass = classDecl.match(text);
    const int classOffset = firstClass.hasMatch() ? firstClass.capturedStart(0) : -1;

    struct Annotation
    {
        QString name;
        QString value;
        QString args;
        int offset = 0;
        int line = 0;
    };
    QList<Annotation> methodLevel;
    QString prefix;
    auto it = annotationPattern.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        Annotation annotation{match.captured(1), pathValue.match(match.captured(3)).captured(1), match.captured(3),
                              static_cast<int>(match.capturedStart(0)), lineNumberAtOffset(text, match.capturedStart(0))};
        if (classOffset >= 0 && annotation.offset < classOffset) {
            if (annotation.name == QStringLiteral("RequestMapping") || annotation.name == QStringLiteral("Path")) {
                prefix = annotation.value;
            }
            continue;
        }
        methodLevel.append(annotation);
    }

    auto addRoute = [&](const QString &method, const QString &routePath, int line) {
        QString full = prefix;
        if (!routePath.isEmpty()) {
            if (!full.endsWith(QLatin1Char('/')) && !routePath.startsWith(QLatin1Char('/'))) {
                full += QLatin1Char('/');
            }
            full += routePath;
        }
        if (full.isEmpty()) {
            full = QStringLiteral("/");
        }
        QVariantMap route = makeSourceContextItem(path, QStringLiteral("java"), line, snippetFromLine(text, line, 2),
                                                  QStringLiteral("route"));
        route.insert(QStringLiteral("method"), method);
        route.insert(QStringLiteral("path"), full);
        route.insert(QStringLiteral("label"), method + QStringLiteral(" ") + full);
        routes.append(route);
    };

    static const QSet<QString> jaxRsVerbs = {
        QStringLiteral("GET"), QStringLiteral("POST"), QStringLiteral("PUT"), QStringLiteral("DELETE"),
        QStringLiteral("PATCH"), QStringLiteral("HEAD"), QStringLiteral("OPTIONS"),
    };
    for (const Annotation &annotation : std::as_const(methodLevel)) {
        if (routes.size() >= 200) {
            break;
        }
        if (annotation.name == QStringLiteral("RequestMapping")) {
            const auto method = methodValue.match(annotation.args);
            addRoute(method.hasMatch() ? method.captured(1) : QStringLiteral("ANY"), annotation.value, annotation.line);
        } else if (annotation.name.endsWith(QStringLiteral("Mapping"))) {
            addRoute(annotation.name.left(annotation.name.size() - 7).toUpper(), annotation.value, annotation.line);
        } else if (jaxRsVerbs.contains(annotation.name)) {
            // Pair the verb with a method-level @Path in the same annotation block.
            QString subPath;
            for (const Annotation &other : std::as_const(methodLevel)) {
                if (other.name == QStringLiteral("Path") && qAbs(other.line - annotation.line) <= 3) {
                    subPath = other.value;
                    break;
                }
            }
            addRoute(annotation.name, subPath, annotation.line);
        }
    }
    result.insert(QStringLiteral("routes"), routes);
}


// ---------------------------------------------------------------------------
// Signatures from the syntax tree for TS/JS, C#, Java and PHP
//
// A post-pass over an AST analysis: re-parse, index every callable node by
// its start line (and its declarator's line, for `const f = () => {}`), and
// give each callable symbol parameters and returns read from the grammar
// (types, defaults, modifiers). Where no return type is declared (JS, untyped
// PHP), return paths are classified as for Python.
// ---------------------------------------------------------------------------

namespace {

bool isAstCallableNode(const char *type)
{
    if (!type) {
        return false;
    }
    static const QSet<QByteArray> types = {
        "function_declaration", "generator_function_declaration", "method_definition", "method_signature",
        "abstract_method_signature", "function_signature", "function_expression", "function", "arrow_function",
        "generator_function", "method_declaration", "constructor_declaration", "local_function_statement",
        "operator_declaration", "delegate_declaration", "destructor_declaration", "function_definition",
        "anonymous_function", "anonymous_function_creation_expression", "arrow_function",
        "function_item", "function_signature_item", "init_declaration", "protocol_function_declaration",
    };
    return types.contains(QByteArray(type));
}

QString stripTypeAnnotation(QString text)
{
    text = text.simplified();
    if (text.startsWith(QLatin1Char(':'))) {
        text = text.mid(1).trimmed();
    }
    return text;
}

QString astCallableName(TSNode node, const QByteArray &source)
{
    QString name = nodeText(fieldNode(node, "name"), source).trimmed();
    if (!name.isEmpty()) {
        return name;
    }
    TSNode parent = ts_node_parent(node);
    const QString parentType = tsType(parent);
    if (parentType == QStringLiteral("variable_declarator") || parentType == QStringLiteral("public_field_definition")
        || parentType == QStringLiteral("field_definition")) {
        return nodeText(fieldNode(parent, "name"), source).trimmed();
    }
    if (parentType == QStringLiteral("pair")) {
        return nodeText(fieldNode(parent, "key"), source).trimmed();
    }
    if (parentType == QStringLiteral("assignment_expression")) {
        return nodeText(fieldNode(parent, "left"), source).trimmed().section(QLatin1Char('.'), -1);
    }
    return {};
}

QVariantList astParameters(TSNode function, const QByteArray &source, const QString &language)
{
    QVariantList parameters;
    if (language == QStringLiteral("swift")) {
        // Parameters are direct children; a default value follows its parameter
        // as a `default_value` field of the declaration.
        const uint32_t childCount = ts_node_child_count(function);
        for (uint32_t index = 0; index < childCount; ++index) {
            TSNode child = ts_node_child(function, index);
            const char *field = ts_node_field_name_for_child(function, index);
            if (field && std::strcmp(field, "default_value") == 0 && !parameters.isEmpty()) {
                QVariantMap last = parameters.last().toMap();
                last.insert(QStringLiteral("default"), nodeText(child, source).simplified().left(60));
                parameters.last() = last;
                continue;
            }
            if (tsType(child) != QStringLiteral("parameter")) {
                continue;
            }
            const QString external = nodeText(fieldNode(child, "external_name"), source).trimmed();
            const QString name = nodeText(fieldNode(child, "name"), source).trimmed();
            // The full annotation after the name (a function type such as
            // `@escaping (Request) -> Void` spans several type-field nodes).
            QString type;
            TSNode nameNode = fieldNode(child, "name");
            if (!ts_node_is_null(nameNode)) {
                const uint32_t from = ts_node_end_byte(nameNode);
                const uint32_t to = ts_node_end_byte(child);
                if (to > from) {
                    type = QString::fromUtf8(source.constData() + from, static_cast<int>(to - from)).simplified();
                }
                if (type.startsWith(QLatin1Char(':'))) {
                    type = type.mid(1).trimmed();
                }
            }
            if (type.isEmpty()) {
                type = nodeText(fieldNode(child, "type"), source).simplified();
            }
            if (nodeText(child, source).contains(QStringLiteral("...")) && !type.endsWith(QStringLiteral("..."))) {
                type += QStringLiteral("...");
            }
            for (uint32_t k = 0; k < ts_node_named_child_count(child); ++k) {
                if (tsType(ts_node_named_child(child, k)) == QStringLiteral("parameter_modifiers")) {
                    const QString modifiers = nodeText(ts_node_named_child(child, k), source).simplified();
                    if (!type.contains(modifiers)) {
                        type = modifiers + QLatin1Char(' ') + type;
                    }
                }
            }
            const QString display = (!external.isEmpty() && external != name) ? external + QLatin1Char(' ') + name : name;
            parameters.append(makeSignatureParameter(display, type));
        }
        return parameters;
    }
    if (language == QStringLiteral("rust")) {
        TSNode list = fieldNode(function, "parameters");
        for (uint32_t index = 0; index < ts_node_named_child_count(list); ++index) {
            TSNode param = ts_node_named_child(list, index);
            const QString type = tsType(param);
            if (type == QStringLiteral("parameter")) {
                parameters.append(makeSignatureParameter(nodeText(fieldNode(param, "pattern"), source).simplified(),
                                                         nodeText(fieldNode(param, "type"), source).simplified()));
            } else if (type == QStringLiteral("variadic_parameter")) {
                parameters.append(makeSignatureParameter(QStringLiteral("..."), QString()));
            }
            // self_parameter is the receiver, not an argument (as for Python's self).
        }
        return parameters;
    }
    TSNode list = fieldNode(function, "parameters");
    if (ts_node_is_null(list)) {
        TSNode single = fieldNode(function, "parameter"); // x => ...
        if (!ts_node_is_null(single)) {
            parameters.append(makeSignatureParameter(nodeText(single, source), QString()));
        }
        return parameters;
    }
    const uint32_t count = ts_node_named_child_count(list);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode param = ts_node_named_child(list, index);
        const QString type = tsType(param);
        QString name;
        QString paramType;
        QString defaultValue;
        QString prefix;
        if (type == QStringLiteral("comment") || type == QStringLiteral("attribute_list")) {
            continue;
        }
        if (language == QStringLiteral("csharp")) {
            if (type != QStringLiteral("parameter")) {
                continue;
            }
            name = nodeText(fieldNode(param, "name"), source);
            paramType = nodeText(fieldNode(param, "type"), source).simplified();
            for (uint32_t k = 0; k < ts_node_named_child_count(param); ++k) {
                TSNode child = ts_node_named_child(param, k);
                const QString childType = tsType(child);
                if (childType == QStringLiteral("modifier")) {
                    prefix += nodeText(child, source) + QLatin1Char(' ');
                } else if (childType != QStringLiteral("identifier") && childType != QStringLiteral("attribute_list")
                           && ts_node_start_byte(child) > ts_node_start_byte(fieldNode(param, "name"))) {
                    defaultValue = nodeText(child, source).simplified();
                    if (defaultValue.startsWith(QLatin1Char('='))) {
                        defaultValue = defaultValue.mid(1).trimmed();
                    }
                }
            }
        } else if (language == QStringLiteral("java")) {
            if (type == QStringLiteral("formal_parameter")) {
                name = nodeText(fieldNode(param, "name"), source);
                paramType = nodeText(fieldNode(param, "type"), source).simplified() + nodeText(fieldNode(param, "dimensions"), source);
            } else if (type == QStringLiteral("spread_parameter")) {
                for (uint32_t k = 0; k < ts_node_named_child_count(param); ++k) {
                    TSNode child = ts_node_named_child(param, k);
                    if (tsType(child) == QStringLiteral("variable_declarator")) {
                        name = nodeText(fieldNode(child, "name"), source);
                    } else if (tsType(child) != QStringLiteral("modifiers")) {
                        paramType = nodeText(child, source).simplified() + QStringLiteral("...");
                    }
                }
            } else {
                continue;
            }
        } else if (language == QStringLiteral("php")) {
            if (type != QStringLiteral("simple_parameter") && type != QStringLiteral("variadic_parameter")
                && type != QStringLiteral("property_promotion_parameter")) {
                continue;
            }
            name = nodeText(fieldNode(param, "name"), source);
            paramType = nodeText(fieldNode(param, "type"), source).simplified();
            defaultValue = nodeText(fieldNode(param, "default_value"), source).simplified();
            if (type == QStringLiteral("variadic_parameter")) {
                name = QStringLiteral("...") + name;
            } else if (type == QStringLiteral("property_promotion_parameter")) {
                prefix = nodeText(fieldNode(param, "visibility"), source) + QLatin1Char(' ');
            }
        } else { // ts / tsx / js
            if (type == QStringLiteral("required_parameter") || type == QStringLiteral("optional_parameter")) {
                TSNode pattern = fieldNode(param, "pattern");
                if (ts_node_is_null(pattern)) {
                    pattern = fieldNode(param, "name");
                }
                name = nodeText(pattern, source).simplified();
                if (type == QStringLiteral("optional_parameter")) {
                    name += QLatin1Char('?');
                }
                paramType = stripTypeAnnotation(nodeText(fieldNode(param, "type"), source));
                defaultValue = nodeText(fieldNode(param, "value"), source).simplified();
                for (uint32_t k = 0; k < ts_node_named_child_count(param); ++k) {
                    TSNode child = ts_node_named_child(param, k);
                    if (tsType(child) == QStringLiteral("accessibility_modifier")) {
                        prefix = nodeText(child, source) + QLatin1Char(' ');
                    }
                }
            } else if (type == QStringLiteral("assignment_pattern")) {
                name = nodeText(fieldNode(param, "left"), source).simplified();
                defaultValue = nodeText(fieldNode(param, "right"), source).simplified();
            } else if (type == QStringLiteral("identifier") || type == QStringLiteral("rest_pattern")
                       || type == QStringLiteral("object_pattern") || type == QStringLiteral("array_pattern")) {
                name = nodeText(param, source).simplified();
            } else {
                continue;
            }
        }
        if (name.isEmpty()) {
            continue;
        }
        QVariantMap parameter = makeSignatureParameter(name, (prefix + paramType).trimmed());
        if (!defaultValue.isEmpty()) {
            parameter.insert(QStringLiteral("default"), defaultValue.left(60));
        }
        parameters.append(parameter);
    }
    return parameters;
}

QString classifyReturnExpression(TSNode expression, const QByteArray &source, const QString &language)
{
    const QString type = tsType(expression);
    const bool php = language == QStringLiteral("php");
    if (type.isEmpty()) {
        return php ? QStringLiteral("null") : QStringLiteral("undefined");
    }
    if (type == QStringLiteral("parenthesized_expression") && ts_node_named_child_count(expression) > 0) {
        return classifyReturnExpression(ts_node_named_child(expression, 0), source, language);
    }
    if (type == QStringLiteral("string") || type == QStringLiteral("template_string") || type == QStringLiteral("encapsed_string")) {
        return QStringLiteral("string");
    }
    if (type == QStringLiteral("number")) return QStringLiteral("number");
    if (type == QStringLiteral("integer")) return QStringLiteral("int");
    if (type == QStringLiteral("float")) return QStringLiteral("float");
    if (type == QStringLiteral("true") || type == QStringLiteral("false") || type == QStringLiteral("boolean")) {
        return php ? QStringLiteral("bool") : QStringLiteral("boolean");
    }
    if (type == QStringLiteral("null")) return QStringLiteral("null");
    if (type == QStringLiteral("undefined")) return QStringLiteral("undefined");
    if (type == QStringLiteral("object")) return QStringLiteral("object");
    if (type == QStringLiteral("array") || type == QStringLiteral("array_creation_expression")) return QStringLiteral("array");
    if (type == QStringLiteral("arrow_function") || type == QStringLiteral("function_expression")
        || type == QStringLiteral("anonymous_function") || type == QStringLiteral("anonymous_function_creation_expression")) {
        return QStringLiteral("function");
    }
    if (type == QStringLiteral("new_expression")) {
        return nodeText(fieldNode(expression, "constructor"), source).simplified();
    }
    if (type == QStringLiteral("object_creation_expression")) {
        for (uint32_t k = 0; k < ts_node_named_child_count(expression); ++k) {
            const QString childType = tsType(ts_node_named_child(expression, k));
            if (childType == QStringLiteral("name") || childType == QStringLiteral("qualified_name")) {
                return nodeText(ts_node_named_child(expression, k), source);
            }
        }
        return QStringLiteral("object");
    }
    if (type == QStringLiteral("await_expression") && ts_node_named_child_count(expression) > 0) {
        return classifyReturnExpression(ts_node_named_child(expression, 0), source, language);
    }
    if (type == QStringLiteral("unary_expression") || type == QStringLiteral("unary_op_expression")) {
        if (nodeText(expression, source).trimmed().startsWith(QLatin1Char('!'))) {
            return php ? QStringLiteral("bool") : QStringLiteral("boolean");
        }
    }
    if (type == QStringLiteral("binary_expression")) {
        static const QSet<QString> comparisons = {
            QStringLiteral("==="), QStringLiteral("!=="), QStringLiteral("=="), QStringLiteral("!="),
            QStringLiteral("<"), QStringLiteral(">"), QStringLiteral("<="), QStringLiteral(">="),
            QStringLiteral("instanceof"), QStringLiteral("in"), QStringLiteral("<>"),
        };
        if (comparisons.contains(nodeText(fieldNode(expression, "operator"), source).trimmed())) {
            return php ? QStringLiteral("bool") : QStringLiteral("boolean");
        }
    }
    if (type == QStringLiteral("call_expression") || type == QStringLiteral("function_call_expression")
        || type == QStringLiteral("member_call_expression")) {
        QString callee = nodeText(fieldNode(expression, "function"), source).simplified();
        if (callee.isEmpty()) {
            callee = nodeText(fieldNode(expression, "name"), source).simplified();
        }
        return QStringLiteral("result of %1()").arg(callee.size() > 40 ? callee.section(QLatin1Char('.'), -1) : callee);
    }
    if (type == QStringLiteral("identifier") || type == QStringLiteral("variable_name") || type == QStringLiteral("member_expression")
        || type == QStringLiteral("member_access_expression")) {
        const QString text = nodeText(expression, source).simplified();
        if (text == QStringLiteral("this") || text == QStringLiteral("$this")) {
            return QStringLiteral("this");
        }
        return QStringLiteral("value of %1").arg(text.left(40));
    }
    if (type == QStringLiteral("this")) return QStringLiteral("this");
    return QStringLiteral("expression");
}

void collectReturnStatements(TSNode node, QList<TSNode> &returns, bool root = true)
{
    if (ts_node_is_null(node)) {
        return; // abstract / interface members have no body
    }
    if (!root && isAstCallableNode(ts_node_type(node))) {
        return;
    }
    const QString type = tsType(node);
    if (!root && (type == QStringLiteral("class_declaration") || type == QStringLiteral("class"))) {
        return;
    }
    if (type == QStringLiteral("return_statement")) {
        returns.append(node);
    }
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t index = 0; index < count; ++index) {
        collectReturnStatements(ts_node_named_child(node, index), returns, false);
    }
}

QVariantList astReturns(TSNode function, const QByteArray &source, const QString &language, bool *declared)
{
    QVariantList returns;
    const QString functionType = tsType(function);
    *declared = false;
    if (functionType == QStringLiteral("constructor_declaration") || functionType == QStringLiteral("destructor_declaration")) {
        *declared = true;
        returns.append(QVariantMap{{QStringLiteral("text"), QStringLiteral("none")}});
        return returns;
    }
    QString declaredType;
    if (language == QStringLiteral("swift") || language == QStringLiteral("rust")) {
        *declared = true;
        if (functionType == QStringLiteral("init_declaration")) {
            returns.append(QVariantMap{{QStringLiteral("text"), QStringLiteral("none")}});
            return returns;
        }
        QString text = nodeText(fieldNode(function, "return_type"), source).simplified();
        if (text.isEmpty() || text == QStringLiteral("()") || text == QStringLiteral("Void")) {
            text = QStringLiteral("none");
        }
        if (language == QStringLiteral("swift")) {
            QStringList effects;
            for (uint32_t k = 0; k < ts_node_child_count(function); ++k) {
                const QString childType = tsType(ts_node_child(function, k));
                if (childType == QStringLiteral("async") || childType == QStringLiteral("throws")) {
                    effects.append(nodeText(ts_node_child(function, k), source).simplified());
                }
            }
            if (!effects.isEmpty()) {
                text += QStringLiteral(" (%1)").arg(effects.join(QLatin1Char(' ')));
            }
        }
        returns.append(QVariantMap{{QStringLiteral("text"), text}, {QStringLiteral("source"), QStringLiteral("declared")}});
        return returns;
    }
    if (language == QStringLiteral("csharp")) {
        declaredType = nodeText(fieldNode(function, "returns"), source).simplified();
        if (declaredType.isEmpty()) {
            declaredType = nodeText(fieldNode(function, "type"), source).simplified();
        }
    } else if (language == QStringLiteral("java")) {
        declaredType = nodeText(fieldNode(function, "type"), source).simplified() + nodeText(fieldNode(function, "dimensions"), source);
    } else {
        declaredType = stripTypeAnnotation(nodeText(fieldNode(function, "return_type"), source));
    }
    if (!declaredType.isEmpty()) {
        *declared = true;
        returns.append(QVariantMap{{QStringLiteral("text"), declaredType == QStringLiteral("void") ? QStringLiteral("none") : declaredType},
                                   {QStringLiteral("source"), QStringLiteral("declared")}});
        return returns;
    }
    if (language == QStringLiteral("csharp") || language == QStringLiteral("java")) {
        returns.append(QVariantMap{{QStringLiteral("text"), QStringLiteral("none")}});
        *declared = true;
        return returns;
    }
    // Arrow function with an expression body returns that expression.
    TSNode body = fieldNode(function, "body");
    const bool asyncFunction = nodeText(function, source).trimmed().startsWith(QStringLiteral("async"));
    QStringList types;
    int paths = 0;
    if (!ts_node_is_null(body) && tsType(body) != QStringLiteral("statement_block") && tsType(body) != QStringLiteral("compound_statement")) {
        types.append(classifyReturnExpression(body, source, language));
        paths = 1;
    } else {
        QList<TSNode> statements;
        collectReturnStatements(body, statements);
        for (const TSNode &statement : std::as_const(statements)) {
            const QString type = ts_node_named_child_count(statement) > 0
                ? classifyReturnExpression(ts_node_named_child(statement, 0), source, language)
                : (language == QStringLiteral("php") ? QStringLiteral("null") : QStringLiteral("undefined"));
            ++paths;
            if (!types.contains(type)) {
                types.append(type);
            }
        }
    }
    QString text;
    if (types.isEmpty()) {
        text = QStringLiteral("none");
    } else {
        text = types.join(QStringLiteral(" | "));
        if (paths > 1) {
            text += QStringLiteral(" (%1 return paths)").arg(paths);
        }
    }
    if (asyncFunction && language != QStringLiteral("php")) {
        text = QStringLiteral("Promise<%1>").arg(types.isEmpty() ? QStringLiteral("void") : types.join(QStringLiteral(" | ")));
    }
    returns.append(QVariantMap{{QStringLiteral("text"), text}, {QStringLiteral("source"), QStringLiteral("inferred")}});
    return returns;
}

} // namespace

static QVariantList applyAstSignatures(const QVariantList &symbols, const QByteArray &source, const QString &language)
{
    const QString signatureLanguage = (language == QStringLiteral("tsx") || language == QStringLiteral("ts")
                                       || language == QStringLiteral("script") || language == QStringLiteral("jsx"))
        ? QStringLiteral("ts") : language;
    TSLanguage *tsLanguage = languageForName(language);
    if (!tsLanguage || symbols.isEmpty()) {
        return symbols;
    }
    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, tsLanguage)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return symbols;
    }
    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    QMultiHash<int, TSNode> byLine;
    std::function<void(TSNode)> index = [&](TSNode node) {
        if (isAstCallableNode(ts_node_type(node))) {
            byLine.insert(nodeLine(node), node);
            TSNode parent = ts_node_parent(node);
            if (!ts_node_is_null(parent) && nodeLine(parent) != nodeLine(node)) {
                byLine.insert(nodeLine(parent), node);
            }
        }
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            index(ts_node_named_child(node, i));
        }
    };
    index(ts_tree_root_node(tree));

    std::function<QVariantList(QVariantList)> apply = [&](QVariantList items) {
        for (int i = 0; i < items.size(); ++i) {
            QVariantMap symbol = items.at(i).toMap();
            symbol.insert(QStringLiteral("members"), apply(symbol.value(QStringLiteral("members")).toList()));
            if (isCallableSymbolKind(symbol.value(QStringLiteral("kind")).toString())
                || symbol.value(QStringLiteral("kind")).toString() == QStringLiteral("component")) {
                const int line = symbol.value(QStringLiteral("line")).toInt();
                const QString name = symbol.value(QStringLiteral("name")).toString();
                TSNode match{};
                bool found = false;
                const QList<TSNode> candidates = byLine.values(line);
                for (const TSNode &candidate : candidates) {
                    const QString candidateName = astCallableName(candidate, source);
                    if (candidateName == name || candidateName.section(QLatin1Char('.'), -1) == name
                        || (candidates.size() == 1 && candidateName.isEmpty())) {
                        match = candidate;
                        found = true;
                        break;
                    }
                }
                if (found) {
                    bool declared = false;
                    symbol.insert(QStringLiteral("parameters"), astParameters(match, source, signatureLanguage));
                    symbol.insert(QStringLiteral("returns"), astReturns(match, source, signatureLanguage, &declared));
                    symbol.insert(QStringLiteral("signatureSource"), QStringLiteral("ast"));
                }
            }
            items[i] = symbol;
        }
        return items;
    };
    const QVariantList result = apply(symbols);
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return result;
}


// ---------------------------------------------------------------------------
// Call sites (for the project index)
//
// Every call made inside each callable symbol - callee name, receiver /
// qualifier text and line - read from the syntax tree, whether or not the
// callee is defined in this file. Same-file edges are already resolved into
// calls / calledBy; the call sites are what the project index resolves
// across files. Calls at file level go to `moduleCallSites`.
// ---------------------------------------------------------------------------

namespace {

struct CallSiteInfo
{
    QString name;
    QString qualifier;
};

QString lastSegment(QString text)
{
    text = text.simplified();
    for (const QString &separator : {QStringLiteral("::"), QStringLiteral("->"), QStringLiteral("?."), QStringLiteral(".")}) {
        const int at = text.lastIndexOf(separator);
        if (at >= 0) {
            text = text.mid(at + separator.size());
        }
    }
    text.remove(QRegularExpression(QStringLiteral(R"(<.*$)")));
    return text.trimmed();
}

CallSiteInfo callSiteFor(TSNode node, const QByteArray &source, const char *type)
{
    CallSiteInfo info;
    auto qualifierOf = [&](TSNode object) {
        QString text = nodeText(object, source).simplified();
        return text.size() > 60 ? QString() : text;
    };
    if (std::strcmp(type, "call_expression") == 0 || std::strcmp(type, "call") == 0
        || std::strcmp(type, "invocation_expression") == 0) {
        TSNode function = fieldNode(node, "function");
        if (ts_node_is_null(function) && ts_node_named_child_count(node) > 0) {
            function = ts_node_named_child(node, 0); // Swift
        }
        const QString functionType = tsType(function);
        if (functionType == QStringLiteral("member_expression")) {
            info.name = nodeText(fieldNode(function, "property"), source);
            info.qualifier = qualifierOf(fieldNode(function, "object"));
        } else if (functionType == QStringLiteral("attribute")) {
            info.name = nodeText(fieldNode(function, "attribute"), source);
            info.qualifier = qualifierOf(fieldNode(function, "object"));
        } else if (functionType == QStringLiteral("selector_expression")) {
            info.name = nodeText(fieldNode(function, "field"), source);
            info.qualifier = qualifierOf(fieldNode(function, "operand"));
        } else if (functionType == QStringLiteral("field_expression")) {
            info.name = nodeText(fieldNode(function, "field"), source);
            TSNode value = fieldNode(function, "value");
            info.qualifier = qualifierOf(ts_node_is_null(value) ? fieldNode(function, "argument") : value);
        } else if (functionType == QStringLiteral("member_access_expression")) {
            info.name = lastSegment(nodeText(fieldNode(function, "name"), source));
            info.qualifier = qualifierOf(fieldNode(function, "expression"));
        } else if (functionType == QStringLiteral("scoped_identifier") || functionType == QStringLiteral("qualified_identifier")) {
            info.name = nodeText(fieldNode(function, "name"), source).section(QStringLiteral("::"), -1);
            TSNode scope = fieldNode(function, "path");
            info.qualifier = qualifierOf(ts_node_is_null(scope) ? fieldNode(function, "scope") : scope);
        } else if (functionType == QStringLiteral("navigation_expression")) {
            info.name = lastSegment(nodeText(fieldNode(function, "suffix"), source));
            info.qualifier = qualifierOf(fieldNode(function, "target"));
        } else {
            info.name = lastSegment(nodeText(function, source));
        }
    } else if (std::strcmp(type, "new_expression") == 0) {
        info.name = lastSegment(nodeText(fieldNode(node, "constructor"), source));
        info.qualifier = QStringLiteral("new");
    } else if (std::strcmp(type, "method_invocation") == 0) {
        info.name = nodeText(fieldNode(node, "name"), source);
        info.qualifier = qualifierOf(fieldNode(node, "object"));
    } else if (std::strcmp(type, "object_creation_expression") == 0) {
        TSNode typeNode = fieldNode(node, "type");
        if (ts_node_is_null(typeNode)) {
            for (uint32_t k = 0; k < ts_node_named_child_count(node); ++k) {
                const QString childType = tsType(ts_node_named_child(node, k));
                if (childType == QStringLiteral("name") || childType == QStringLiteral("qualified_name")) {
                    typeNode = ts_node_named_child(node, k);
                    break;
                }
            }
        }
        info.name = lastSegment(nodeText(typeNode, source));
        info.qualifier = QStringLiteral("new");
    } else if (std::strcmp(type, "function_call_expression") == 0) {
        info.name = lastSegment(nodeText(fieldNode(node, "function"), source).remove(QLatin1Char('\\')));
    } else if (std::strcmp(type, "member_call_expression") == 0 || std::strcmp(type, "nullsafe_member_call_expression") == 0) {
        info.name = nodeText(fieldNode(node, "name"), source);
        info.qualifier = qualifierOf(fieldNode(node, "object"));
    } else if (std::strcmp(type, "scoped_call_expression") == 0) {
        info.name = nodeText(fieldNode(node, "name"), source);
        info.qualifier = qualifierOf(fieldNode(node, "scope"));
    }
    static const QRegularExpression identifier(QStringLiteral(R"(^[A-Za-z_$][\w$]*$)"));
    if (!identifier.match(info.name).hasMatch()) {
        info.name.clear();
    }
    return info;
}

bool isCallSiteNode(const char *type)
{
    static const QSet<QByteArray> types = {
        "call_expression", "call", "invocation_expression", "new_expression", "method_invocation",
        "object_creation_expression", "function_call_expression", "member_call_expression",
        "nullsafe_member_call_expression", "scoped_call_expression",
    };
    return types.contains(QByteArray(type));
}

} // namespace

namespace {
// Call sites for the structural (non-AST) languages; defined further down.
QVariantMap applyTextCallSites(QVariantMap analysis, const QString &text, const QString &language);
} // namespace

static QVariantMap applyAstCallSites(QVariantMap analysis, const QByteArray &source, const QString &language)
{
    QVariantList symbols = analysis.value(QStringLiteral("symbols")).toList();
    TSLanguage *tsLanguage = languageForName(language);
    if (!tsLanguage) {
        return analysis;
    }
    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, tsLanguage)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return analysis;
    }
    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());

    // Callable symbols by line (a function's declaration line), with names.
    QMultiHash<int, QPair<QString, QString>> callableByLine; // line -> (name, key)
    std::function<void(const QVariantList &)> collect = [&](const QVariantList &items) {
        for (const QVariant &entry : items) {
            const QVariantMap symbol = entry.toMap();
            const QString kind = symbol.value(QStringLiteral("kind")).toString();
            if (isCallableSymbolKind(kind) || kind == QStringLiteral("component") || kind == QStringLiteral("handler")
                || kind == QStringLiteral("test") || kind == QStringLiteral("test hook")) {
                callableByLine.insert(symbol.value(QStringLiteral("line")).toInt(),
                                      {symbol.value(QStringLiteral("name")).toString(), symbolKey(symbol)});
            }
            collect(symbol.value(QStringLiteral("members")).toList());
        }
    };
    collect(symbols);

    QHash<QString, QVariantList> sitesByKey;
    QHash<QString, QSet<QString>> seenByKey;
    QVariantList moduleSites;
    QSet<QString> moduleSeen;
    std::function<void(TSNode, const QString &)> visit = [&](TSNode node, const QString &owner) {
        QString current = owner;
        const char *type = ts_node_type(node);
        if (isAstCallableNode(type) || std::strcmp(type, "method_declaration") == 0
            || std::strcmp(type, "function_definition") == 0) {
            const QList<QPair<QString, QString>> candidates = callableByLine.values(nodeLine(node));
            const QString nodeName = astCallableName(node, source);
            for (const auto &candidate : candidates) {
                if (candidates.size() == 1 || candidate.first == nodeName
                    || candidate.first.section(QLatin1Char('.'), -1) == nodeName) {
                    current = candidate.second;
                    break;
                }
            }
        }
        if (isCallSiteNode(type)) {
            const CallSiteInfo info = callSiteFor(node, source, type);
            if (!info.name.isEmpty()) {
                const QString dedupe = info.qualifier + QLatin1Char('|') + info.name;
                QVariantMap site{{QStringLiteral("name"), info.name}, {QStringLiteral("line"), nodeLine(node)}};
                if (!info.qualifier.isEmpty()) {
                    site.insert(QStringLiteral("qualifier"), info.qualifier);
                }
                if (current.isEmpty()) {
                    if (!moduleSeen.contains(dedupe) && moduleSites.size() < 300) {
                        moduleSeen.insert(dedupe);
                        moduleSites.append(site);
                    }
                } else if (!seenByKey[current].contains(dedupe) && sitesByKey[current].size() < 200) {
                    seenByKey[current].insert(dedupe);
                    sitesByKey[current].append(site);
                }
            }
        }
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t index = 0; index < count; ++index) {
            visit(ts_node_named_child(node, index), current);
        }
    };
    visit(ts_tree_root_node(tree), QString());
    ts_tree_delete(tree);
    ts_parser_delete(parser);

    std::function<QVariantList(QVariantList)> apply = [&](QVariantList items) {
        for (int index = 0; index < items.size(); ++index) {
            QVariantMap symbol = items.at(index).toMap();
            symbol.insert(QStringLiteral("members"), apply(symbol.value(QStringLiteral("members")).toList()));
            const auto sites = sitesByKey.constFind(symbolKey(symbol));
            if (sites != sitesByKey.constEnd()) {
                symbol.insert(QStringLiteral("callSites"), *sites);
            }
            items[index] = symbol;
        }
        return items;
    };
    analysis.insert(QStringLiteral("symbols"), apply(symbols));
    if (!moduleSites.isEmpty()) {
        analysis.insert(QStringLiteral("moduleCallSites"), moduleSites);
    }
    return analysis;
}

QVariantMap SymbolParser::makeSymbol(const QString &kind, const QString &name, int line,
                                     const QString &detail, const QVariantList &members,
                                     const QString &snippet)
{
    QVariantMap result;
    result.insert(QStringLiteral("kind"), kind);
    result.insert(QStringLiteral("name"), name);
    result.insert(QStringLiteral("line"), line);
    result.insert(QStringLiteral("detail"), detail);
    result.insert(QStringLiteral("members"), members);
    result.insert(QStringLiteral("snippet"), snippet);
    result.insert(QStringLiteral("calls"), QVariantList{});
    result.insert(QStringLiteral("calledBy"), QVariantList{});
    return result;
}

QVariantMap SymbolParser::makeResultSkeleton(const QString &path, const QString &fileName, const QString &language)
{
    QVariantMap result;
    result.insert(QStringLiteral("path"), path);
    result.insert(QStringLiteral("fileName"), fileName);
    result.insert(QStringLiteral("language"), language);
    result.insert(QStringLiteral("symbols"), QVariantList{});
    result.insert(QStringLiteral("quickLinks"), QVariantList{});
    result.insert(QStringLiteral("dependencies"), QVariantList{});
    result.insert(QStringLiteral("routes"), QVariantList{});
    result.insert(QStringLiteral("relatedFiles"), QVariantList{});
    result.insert(QStringLiteral("packageSummary"), QVariantMap{});
    result.insert(QStringLiteral("cssSummary"), QVariantMap{});
    result.insert(QStringLiteral("summary"), QString());
    result.insert(QStringLiteral("analysisSourceMode"), QString());
    result.insert(QStringLiteral("analysisConfidence"), QString());
    result.insert(QStringLiteral("analysisHasAstErrors"), false);
    result.insert(QStringLiteral("analysisPartial"), false);
    result.insert(QStringLiteral("analysisNotices"), QVariantList{});
    return result;
}

QString SymbolParser::formatByteSize(qint64 bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(qMax<qint64>(0, bytes));
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < 3) {
        value /= 1024.0;
        ++unitIndex;
    }

    const int decimals = unitIndex == 0 ? 0 : (value >= 10.0 ? 0 : 1);
    return QStringLiteral("%1 %2").arg(QString::number(value, 'f', decimals), QString::fromLatin1(units[unitIndex]));
}

QVariantMap SymbolParser::makeOversizedFileResult(const QString &path, const QString &language,
                                                  qint64 size, qint64 limit)
{
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), language);
    result.insert(QStringLiteral("summary"),
                  QStringLiteral("Analysis skipped: file is too large (%1, limit %2)")
                      .arg(formatByteSize(size), formatByteSize(limit)));
    return result;
}

static QVariantMap makeMinifiedFileResult(const QString &path,
                                          const QString &language,
                                          qint64 size)
{
    QVariantMap result = SymbolParser::makeResultSkeleton(path, QFileInfo(path).fileName(), language);
    result.insert(QStringLiteral("summary"),
                  QStringLiteral("Analysis skipped: file appears to be minified or bundled (%1 bytes)")
                      .arg(QString::number(size)));
    appendParserAnalysisNotice(result,
                               QStringLiteral("warning"),
                               QStringLiteral("Structural analysis was skipped because this file appears to be minified or bundled source."));
    return result;
}

SymbolParser::SymbolParser(QObject *parent)
    : QObject(parent)
{
}

QVariantMap SymbolParser::parseFile(const QString &path) const
{
    QVariantMap analysis = parseFileAnalysis(path);
    // Browser-side HTTP calls (fetch, axios, jQuery, XHR, forms), matched to
    // backend routes by the project index.
    static const QSet<QString> httpClientLanguages = {
        QStringLiteral("script"), QStringLiteral("ts"), QStringLiteral("tsx"), QStringLiteral("jsx"),
        QStringLiteral("html"), QStringLiteral("php"),
    };
    const QString language = analysis.value(QStringLiteral("language")).toString();
    if (httpClientLanguages.contains(language)
        && !analysis.value(QStringLiteral("summary")).toString().startsWith(QStringLiteral("Analysis skipped"))) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            analysis = HttpClients::applyHttpClientCalls(analysis, QString::fromUtf8(file.readAll()), language);
        }
    }
    return analysis;
}

QVariantMap SymbolParser::parseFileAnalysis(const QString &path) const
{
    QFile file(path);
    const QFileInfo info(path);
    const QString language = detectLanguage(path);
    QVariantMap result = makeResultSkeleton(path, info.fileName(), language);
    auto finalizeResult = [](QVariantMap analysis, const QString &sourceMode, const QString &confidence = QString()) {
        analysis = enrichAnalysisSignatures(analysis);
        return annotateAnalysisWithProvenance(analysis, sourceMode, confidence);
    };
    auto finalizeRecoveredResult = [&](const QVariantMap &astAnalysis,
                                       const QVariantMap &heuristicAnalysis,
                                       const QString &message) {
        const QVariantMap limitedHeuristic = limitRecoveryAnalysisToRanges(heuristicAnalysis, astAnalysis);
        const QVariantMap finalizedAst = finalizeResult(astAnalysis, QStringLiteral("ast"));
        const QVariantMap finalizedHeuristic = finalizeResult(limitedHeuristic, QStringLiteral("heuristic"));
        return annotateAnalysisWithProvenance(
            mergeRecoveredAnalysis(finalizedAst, finalizedHeuristic, message),
            QStringLiteral("recovered"),
            QStringLiteral("medium"));
    };

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.insert(QStringLiteral("summary"), QStringLiteral("Unable to read file"));
        return finalizeResult(result, QStringLiteral("heuristic"), QStringLiteral("low"));
    }

    if (shouldSkipFileBySize(info, kMaxParsableFileBytes)) {
        return finalizeResult(makeOversizedFileResult(path, language, info.size(), kMaxParsableFileBytes),
                              QStringLiteral("heuristic"),
                              QStringLiteral("low"));
    }

    const QString text = QString::fromUtf8(file.readAll());

    if (looksLikeMinifiedSource(path, language, text)) {
        return finalizeResult(makeMinifiedFileResult(path, language, info.size()),
                              QStringLiteral("heuristic"),
                              QStringLiteral("low"));
    }

    // AST-first analysis shared by every Tree-sitter language:
    //  1. parse; a clean tree is authoritative;
    //  2. on syntax errors, blank the offending lines and re-parse (branch-scoped
    //     repair), so the damage stays local to those lines;
    //  3. only if the tree is still broken, supplement with the heuristic parser,
    //     restricted to AST-uncovered ranges.
    const QString recoveryMessage = QStringLiteral("Heuristic recovery supplemented AST-uncovered ranges only.");
    auto analyseWithAst = [&](const QString &astLanguage,
                              const std::function<QVariantMap()> &astParse,
                              const std::function<QVariantMap()> &heuristicParse,
                              bool requireSymbols) -> QVariantMap {
        auto hasContent = [&](const QVariantMap &analysis) {
            return requireSymbols ? !analysis.value(QStringLiteral("symbols")).toList().isEmpty()
                                  : analysisHasMeaningfulContent(analysis);
        };

        static const QSet<QString> astSignatureLanguages = {
            QStringLiteral("ts"), QStringLiteral("tsx"), QStringLiteral("script"), QStringLiteral("jsx"),
            QStringLiteral("csharp"), QStringLiteral("java"), QStringLiteral("php"),
            QStringLiteral("rust"), QStringLiteral("swift"),
        };
        static const QSet<QString> callSiteLanguages = {
            QStringLiteral("ts"), QStringLiteral("tsx"), QStringLiteral("script"), QStringLiteral("jsx"),
            QStringLiteral("csharp"), QStringLiteral("java"), QStringLiteral("php"), QStringLiteral("rust"),
            QStringLiteral("swift"), QStringLiteral("python"), QStringLiteral("go"), QStringLiteral("cpp"),
        };
        auto withAstSignatures = [&](QVariantMap analysis, const QByteArray &bytes) {
            if (astSignatureLanguages.contains(astLanguage)) {
                analysis.insert(QStringLiteral("symbols"),
                                applyAstSignatures(analysis.value(QStringLiteral("symbols")).toList(), bytes, astLanguage));
            }
            if (callSiteLanguages.contains(astLanguage)) {
                analysis = applyAstCallSites(analysis, bytes, astLanguage);
            }
            return analysis;
        };
        // C/C++: the grammar sees the macro-blanked bytes (same length, so
        // offsets, lines and snippets from the original text still hold).
        const QByteArray originalBytes = text.toUtf8();
        QByteArray astBytes = originalBytes;
        if (astLanguage == QStringLiteral("cpp")) {
            // Keep the pre-pass only where it helps: a rewrite can also walk
            // into a grammar gap the macro happened to steer around (fmt's
            // `FMT_DEPRECATED operator const string_view&()` cascades into a
            // whole-file ERROR without the macro).
            const QByteArray prepassed = blankCppMacros(originalBytes);
            if (prepassed != originalBytes) {
                TSParser *scoreParser = ts_parser_new();
                if (scoreParser && ts_parser_set_language(scoreParser, languageForName(astLanguage))) {
                    const AstErrorScore before = scoreAstSource(scoreParser, originalBytes, nullptr);
                    const AstErrorScore after = scoreAstSource(scoreParser, prepassed, nullptr);
                    if (after.errorBytes < before.errorBytes
                        || (after.errorBytes == before.errorBytes && after.declarations >= before.declarations)) {
                        astBytes = prepassed;
                    }
                }
                if (scoreParser) {
                    ts_parser_delete(scoreParser);
                }
            }
        }
        std::optional<ScopedAstSourceOverride> macroGuard;
        if (astBytes != originalBytes) {
            macroGuard.emplace(astBytes);
        }
        QVariantMap astResult = withAstSignatures(astParse(), astBytes);
        if (!astResult.value(QStringLiteral("analysisHasAstErrors")).toBool()) {
            // Brace languages: a lost `}` can parse cleanly while nesting the
            // rest of the file; indentation gives it away.
            static const QSet<QString> misnestLanguages = {
                QStringLiteral("swift"), QStringLiteral("go"), QStringLiteral("rust"), QStringLiteral("ts"),
                QStringLiteral("tsx"), QStringLiteral("script"), QStringLiteral("jsx"), QStringLiteral("java"),
                QStringLiteral("csharp"), QStringLiteral("php"), QStringLiteral("cpp"),
            };
            if (misnestLanguages.contains(astLanguage) && hasContent(astResult)) {
                const AstRepairResult renest = repairMisnestingForAst(astBytes, languageForName(astLanguage));
                if (!renest.blankedLines.isEmpty()) {
                    ScopedAstSourceOverride guard(renest.source);
                    QVariantMap repaired = withAstSignatures(astParse(), renest.source);
                    if (!repaired.value(QStringLiteral("analysisHasAstErrors")).toBool() && hasContent(repaired)) {
                        QVariantList damaged;
                        for (int line : renest.blankedLines) {
                            damaged.append(line);
                        }
                        repaired.insert(QStringLiteral("analysisDamagedLines"), damaged);
                        repaired.insert(QStringLiteral("analysisHasAstErrors"), true);
                        return finalizeResult(repaired, QStringLiteral("ast"), QStringLiteral("high"));
                    }
                }
            }
            if (hasContent(astResult)) {
                return finalizeResult(astResult, QStringLiteral("ast"));
            }
            if (heuristicParse) {
                return finalizeResult(heuristicParse(), QStringLiteral("heuristic"));
            }
            return finalizeResult(astResult, QStringLiteral("ast"));
        }

        const AstRepairResult repair = repairSourceForAst(astBytes, languageForName(astLanguage),
                                                          astLanguage == QStringLiteral("python"));
        bool repairAccepted = false;
        if (!repair.blankedLines.isEmpty()) {
            ScopedAstSourceOverride guard(repair.source);
            const QVariantMap repaired = withAstSignatures(astParse(), repair.source);
            // Blanking must never cost declarations the unrepaired tree already
            // had (grammar gaps can flag valid code); otherwise keep the original.
            // It must also stay in the same league as what the heuristic parser
            // sees: blanking that merely dissolves an enclosing type (e.g. to
            // "balance" a missing brace) is worse than the AST+heuristic merge.
            const int repairedCount = countSymbolTree(repaired.value(QStringLiteral("symbols")).toList());
            const int heuristicCount = heuristicParse
                ? countSymbolTree(heuristicParse().value(QStringLiteral("symbols")).toList())
                : 0;
            if (repairedCount >= countSymbolTree(astResult.value(QStringLiteral("symbols")).toList())
                && repairedCount * 10 >= heuristicCount * 6) {
                astResult = repaired;
                repairAccepted = true;
            }
        }
        const bool stillBroken = !repairAccepted
            || astResult.value(QStringLiteral("analysisHasAstErrors")).toBool();
        astResult.insert(QStringLiteral("analysisHasAstErrors"), true);
        if (repairAccepted) {
            QVariantList damaged;
            for (int line : repair.blankedLines) {
                damaged.append(line);
            }
            astResult.insert(QStringLiteral("analysisDamagedLines"), damaged);
        }

        if (!stillBroken && hasContent(astResult)) {
            QVariantMap finalized = finalizeResult(astResult, QStringLiteral("ast"), QStringLiteral("high"));
            QString fileConfidence = QStringLiteral("high");
            if (heuristicParse) {
                QSet<int> blankedSet(repair.blankedLines.cbegin(), repair.blankedLines.cend());
                QVariantList astSymbols;
                const QVariantMap heuristic = finalizeResult(heuristicParse(), QStringLiteral("heuristic"));
                const QVariantList admitted = heuristicSymbolsOnLines(heuristic.value(QStringLiteral("symbols")).toList(),
                                                                      blankedSet,
                                                                      finalized.value(QStringLiteral("symbols")).toList(),
                                                                      &astSymbols);
                if (!admitted.isEmpty() || astSymbols != finalized.value(QStringLiteral("symbols")).toList()) {
                    QVariantList merged = astSymbols;
                    merged.append(admitted);
                    merged = linkAdmittedHeuristicCalls(merged, admitted);
                    std::stable_sort(merged.begin(), merged.end(), [](const QVariant &left, const QVariant &right) {
                        return left.toMap().value(QStringLiteral("line")).toInt()
                            < right.toMap().value(QStringLiteral("line")).toInt();
                    });
                    finalized.insert(QStringLiteral("symbols"), merged);
                    fileConfidence = QStringLiteral("medium");
                }
            }
            appendParserAnalysisNotice(finalized, QStringLiteral("warning"),
                                       QStringLiteral("Syntax errors on line(s) %1 were excluded; the rest of the file was analysed from the syntax tree.")
                                           .arg(describeLineList(repair.blankedLines)),
                                       true);
            finalized.insert(QStringLiteral("analysisSourceMode"), QStringLiteral("recovered"));
            finalized.insert(QStringLiteral("analysisConfidence"), fileConfidence);
            return finalized;
        }
        if (heuristicParse) {
            QVariantMap recovered = finalizeRecoveredResult(astResult, heuristicParse(), recoveryMessage);
            if (repairAccepted) {
                appendParserAnalysisNotice(recovered, QStringLiteral("warning"),
                                           QStringLiteral("Syntax errors on line(s) %1 were excluded before AST analysis.")
                                               .arg(describeLineList(repair.blankedLines)),
                                           true);
            }
            return recovered;
        }
        if (hasContent(astResult)) {
            QVariantMap finalized = finalizeResult(astResult, QStringLiteral("ast"));
            appendParserAnalysisNotice(finalized, QStringLiteral("warning"),
                                       QStringLiteral("The syntax tree contains errors; some declarations may be missing."),
                                       true);
            return finalized;
        }
        return finalizeResult(astResult, QStringLiteral("ast"), QStringLiteral("low"));
    };

    if (language == QStringLiteral("php")) {
        QVariantMap analysis = analyseWithAst(language,
                                              [&] { return parsePhpTreeSitter(path, text); },
                                              [&] { return parsePhp(path, text); },
                                              false);
        enrichPhpAnalysis(analysis, path, text);
        return annotateAnalysisWithProvenance(analysis, analysis.value(QStringLiteral("analysisSourceMode")).toString(),
                                              analysis.value(QStringLiteral("analysisConfidence")).toString());
    }
    if (language == QStringLiteral("swift")) {
        QVariantMap analysis = analyseWithAst(language,
                                              [&] { return parseSwiftTreeSitter(path, text); },
                                              {},
                                              true);
        enrichSwiftDependencies(analysis, path, text);
        analysis = annotateAnalysisWithProvenance(analysis, analysis.value(QStringLiteral("analysisSourceMode")).toString(),
                                                  analysis.value(QStringLiteral("analysisConfidence")).toString());
        if (analysis.value(QStringLiteral("symbols")).toList().isEmpty()) {
            QVariantMap fallback = makeResultSkeleton(path, info.fileName(), language);
            fallback.insert(QStringLiteral("summary"), QStringLiteral("No Swift symbols extracted"));
            return finalizeResult(fallback, QStringLiteral("heuristic"), QStringLiteral("low"));
        }
        return analysis;
    }
    if (language == QStringLiteral("html")) {
        return finalizeResult(parseHtml(path, text), QStringLiteral("ast"));
    }
    if (language == QStringLiteral("qml")) {
        return finalizeResult(parseQml(path, text), QStringLiteral("heuristic"));
    }
    if (language == QStringLiteral("css")) {
        return analyseWithAst(language,
                              [&] { return parseCssTreeSitter(path, text); },
                              [&] { return parseCss(path, text); },
                              true);
    }
    if (language == QStringLiteral("tsx")
        || language == QStringLiteral("ts")
        || language == QStringLiteral("script")
        || language == QStringLiteral("jsx")) {
        QVariantMap analysis = analyseWithAst(language,
                              [&] { return parseScriptLikeTreeSitter(path, text, language); },
                              [&] { return parseScriptLike(path, text, language == QStringLiteral("tsx")
                                                                             || language == QStringLiteral("jsx")); },
                              false);
        enrichScriptWithWebLinks(analysis, path, text);
        return analysis;
    }
    if (language == QStringLiteral("json")) {
        return finalizeResult(parseJson(path, text), QStringLiteral("heuristic"));
    }
    if (language == QStringLiteral("python")) {
        return analyseWithAst(language,
                              [&] { return parsePythonTreeSitter(path, text); },
                              [&] { return parsePython(path, text); },
                              false);
    }
    if (language == QStringLiteral("cpp")) {
        return analyseWithAst(language,
                              [&] { return parseCppTreeSitter(path, text); },
                              [&] { return parseCppLike(path, text, language); },
                              true);
    }
    if (language == QStringLiteral("java")) {
        QVariantMap analysis = analyseWithAst(language,
                                              [&] { return parseJavaTreeSitter(path, text); },
                                              [&] { return parseJava(path, text); },
                                              false);
        enrichJavaRoutes(analysis, path, text);
        resolveJavaImports(analysis, path, text);
        return annotateAnalysisWithProvenance(analysis, analysis.value(QStringLiteral("analysisSourceMode")).toString(),
                                              analysis.value(QStringLiteral("analysisConfidence")).toString());
    }
    if (language == QStringLiteral("csharp")) {
        return analyseWithAst(language,
                              [&] { return parseCSharpTreeSitter(path, text); },
                              [&] { return parseCSharp(path, text); },
                              false);
    }
    if (language == QStringLiteral("rust")) {
        return analyseWithAst(language,
                              [&] { return parseRustTreeSitter(path, text); },
                              [&] { return parseRust(path, text); },
                              false);
    }
    if (language == QStringLiteral("objc")) {
        return applyTextCallSites(finalizeResult(parseObjectiveC(path, text, language), QStringLiteral("heuristic")), text, language);
    }
    if (language == QStringLiteral("go")) {
        return analyseWithAst(language,
                              [&] { return parseGoTreeSitter(path, text); },
                              {},
                              false);
    }
    if (language == QStringLiteral("sql")) {
        return finalizeResult(parseSql(path, text), QStringLiteral("heuristic"), QStringLiteral("high"));
    }
    if (language == QStringLiteral("shell")) {
        return applyTextCallSites(finalizeResult(parseShell(path, text), QStringLiteral("heuristic")), text, language);
    }
    if (language == QStringLiteral("kotlin")) {
        return finalizeResult(parseKotlin(path, text), QStringLiteral("heuristic"), QStringLiteral("high"));
    }
    if (language == QStringLiteral("vbnet")) {
        return applyTextCallSites(finalizeResult(parseVbNet(path, text), QStringLiteral("heuristic"), QStringLiteral("high")), text, language);
    }
    return finalizeResult(result, QStringLiteral("heuristic"), QStringLiteral("low"));
}

QByteArray SymbolParser::cppPrepassed(const QByteArray &source)
{
    return blankCppMacros(source);
}

QVariantMap SymbolParser::debugAst(const QString &path)
{
    QVariantMap report;
    const QString language = detectLanguage(path);
    report.insert(QStringLiteral("language"), language);
    QFile file(path);
    TSLanguage *tsLanguage = languageForName(language);
    if (!file.open(QIODevice::ReadOnly) || !tsLanguage) {
        report.insert(QStringLiteral("error"), QStringLiteral("unreadable file or no Tree-sitter grammar"));
        return report;
    }
    const QByteArray raw = file.readAll();
    // What the analysis parses: C/C++ after the macro pre-pass.
    const QByteArray source = language == QStringLiteral("cpp") ? blankCppMacros(raw) : raw;
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, tsLanguage);
    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    QVariantList errors;
    const QList<QByteArray> sourceLines = source.split('\n');
    std::function<void(TSNode, int)> visit = [&](TSNode node, int depth) {
        if (!ts_node_has_error(node) || errors.size() >= 40) {
            return;
        }
        if (ts_node_is_error(node) || ts_node_is_missing(node)) {
            QVariantMap entry;
            entry.insert(QStringLiteral("kind"), ts_node_is_missing(node) ? QStringLiteral("MISSING") : QStringLiteral("ERROR"));
            entry.insert(QStringLiteral("type"), tsType(node));
            entry.insert(QStringLiteral("depth"), depth);
            entry.insert(QStringLiteral("startLine"), static_cast<int>(ts_node_start_point(node).row) + 1);
            entry.insert(QStringLiteral("endLine"), static_cast<int>(ts_node_end_point(node).row) + 1);
            entry.insert(QStringLiteral("text"), QString::fromUtf8(sourceLines.value(static_cast<int>(ts_node_start_point(node).row))).trimmed());
            TSNode parent = ts_node_parent(node);
            entry.insert(QStringLiteral("parent"), tsType(parent));
            QStringList children;
            const uint32_t count = ts_node_child_count(node);
            for (uint32_t index = 0; index < count && index < 12; ++index) {
                TSNode child = ts_node_child(node, index);
                children.append(QStringLiteral("%1@%2").arg(tsType(child)).arg(ts_node_start_point(child).row + 1));
            }
            entry.insert(QStringLiteral("children"), children);
            errors.append(entry);
        }
        const uint32_t count = ts_node_child_count(node);
        for (uint32_t index = 0; index < count; ++index) {
            visit(ts_node_child(node, index), depth + 1);
        }
    };
    visit(ts_tree_root_node(tree), 0);
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    report.insert(QStringLiteral("errors"), errors);

    const AstRepairResult repair = repairSourceForAst(source, tsLanguage, language == QStringLiteral("python"));
    QVariantList blanked;
    for (int line : repair.blankedLines) {
        blanked.append(line);
    }
    report.insert(QStringLiteral("repairBlankedLines"), blanked);
    report.insert(QStringLiteral("repairClean"), repair.clean);
    return report;
}


// ---------------------------------------------------------------------------
// VB.NET: structural line parser
//
// There is no maintained Tree-sitter grammar for VB.NET, but its declarations
// are keyword-delimited blocks (Class ... End Class, Sub ... End Sub), which a
// line parser can follow precisely. Damage stays local by construction: a
// missing `End Sub` is closed implicitly by the next member declaration or by
// the enclosing `End Class`, so one broken member never swallows its
// siblings. Types are declared (`As T`), so signatures are exact.
// ---------------------------------------------------------------------------

namespace {

struct VbLogicalLine
{
    QString text; // comment-stripped, continuations joined
    int line = 0; // first physical line (1-based)
    int lastLine = 0;
};

QString stripVbComment(const QString &line)
{
    bool inString = false;
    for (int index = 0; index < line.size(); ++index) {
        const QChar ch = line.at(index);
        if (ch == QLatin1Char('"')) {
            inString = !inString;
        } else if (!inString && (ch == QLatin1Char('\'') || ch == QChar(0x2018) || ch == QChar(0x2019))) {
            return line.left(index);
        }
    }
    const QString trimmed = line.trimmed();
    if (trimmed.startsWith(QStringLiteral("REM "), Qt::CaseInsensitive) || trimmed.compare(QStringLiteral("REM"), Qt::CaseInsensitive) == 0) {
        return {};
    }
    return line;
}

QList<VbLogicalLine> vbLogicalLines(const QString &text)
{
    QList<VbLogicalLine> lines;
    const QStringList physical = text.split(QLatin1Char('\n'));
    VbLogicalLine current;
    for (int index = 0; index < physical.size(); ++index) {
        QString line = stripVbComment(physical.at(index));
        line.remove(QLatin1Char('\r'));
        if (index == 0 && line.startsWith(QChar(0xFEFF))) {
            line.remove(0, 1);
        }
        const QString trimmed = line.trimmed();
        if (current.text.isEmpty()) {
            current.line = index + 1;
        }
        current.lastLine = index + 1;
        // Explicit ` _` continuation, or implicit continuation after , ( { operators.
        const bool explicitContinuation = trimmed.endsWith(QStringLiteral(" _")) || trimmed == QStringLiteral("_");
        QString piece = explicitContinuation ? trimmed.left(trimmed.size() - 1).trimmed() : trimmed;
        current.text += (current.text.isEmpty() ? QString() : QStringLiteral(" ")) + piece;
        const bool implicitContinuation = !piece.isEmpty()
            && (piece.endsWith(QLatin1Char(',')) || piece.endsWith(QLatin1Char('(')) || piece.endsWith(QLatin1Char('{'))
                || piece.endsWith(QLatin1Char('&')) || piece.endsWith(QLatin1Char('=')));
        if (explicitContinuation || implicitContinuation) {
            continue;
        }
        if (!current.text.trimmed().isEmpty()) {
            lines.append(current);
        }
        current = VbLogicalLine();
    }
    if (!current.text.trimmed().isEmpty()) {
        lines.append(current);
    }
    return lines;
}

const QString &vbModifiersPattern()
{
    static const QString pattern = QStringLiteral(
        R"((?:(?:Public|Private|Protected|Friend|Shared|Shadows|Overloads|Overrides|Overridable|NotOverridable|MustOverride|MustInherit|NotInheritable|Partial|ReadOnly|WriteOnly|Default|Static|Async|Iterator|WithEvents|Widening|Narrowing|Const|Dim)\s+)*)");
    return pattern;
}

QVariantList parseVbParameters(const QString &parameterText)
{
    QVariantList parameters;
    for (QString part : splitTopLevelSignatureParts(parameterText)) {
        part = part.trimmed();
        if (part.isEmpty()) {
            continue;
        }
        QString defaultValue;
        const int equals = part.indexOf(QLatin1Char('='));
        if (equals >= 0) {
            defaultValue = part.mid(equals + 1).trimmed();
            part = part.left(equals).trimmed();
        }
        static const QRegularExpression paramPattern(QStringLiteral(
            R"(^(?:(?:ByVal|ByRef|Optional|ParamArray)\s+)*([A-Za-z_]\w*)(\(\s*\))?(?:\s+As\s+(?:New\s+)?(.+))?$)"),
            QRegularExpression::CaseInsensitiveOption);
        const auto match = paramPattern.match(part);
        if (!match.hasMatch()) {
            continue;
        }
        QString type = match.captured(3).trimmed();
        if (!match.captured(2).isEmpty()) {
            type = type.isEmpty() ? QStringLiteral("Object()") : type + QStringLiteral("()");
        }
        QVariantMap parameter = makeSignatureParameter(match.captured(1), type);
        if (part.contains(QStringLiteral("ByRef"), Qt::CaseInsensitive)) {
            parameter.insert(QStringLiteral("passing"), QStringLiteral("ByRef"));
        }
        if (!defaultValue.isEmpty()) {
            parameter.insert(QStringLiteral("default"), defaultValue);
        }
        parameters.append(parameter);
    }
    return parameters;
}

// Text inside the first balanced (...) after `start`.
QString balancedParenthesis(const QString &text, int start, int *endOut = nullptr)
{
    const int open = text.indexOf(QLatin1Char('('), start);
    if (open < 0) {
        return {};
    }
    int depth = 0;
    bool inString = false;
    for (int index = open; index < text.size(); ++index) {
        const QChar ch = text.at(index);
        if (ch == QLatin1Char('"')) {
            inString = !inString;
        } else if (!inString && ch == QLatin1Char('(')) {
            ++depth;
        } else if (!inString && ch == QLatin1Char(')')) {
            if (--depth == 0) {
                if (endOut) {
                    *endOut = index + 1;
                }
                return text.mid(open + 1, index - open - 1);
            }
        }
    }
    return {};
}

} // namespace

QVariantMap SymbolParser::parseVbNet(const QString &path, const QString &text) const
{
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("vbnet"));
    const QList<VbLogicalLine> lines = vbLogicalLines(text);
    const QString modifiers = vbModifiersPattern();

    static const QRegularExpression typeOpen(
        QStringLiteral(R"(^%1(Namespace|Class|Module|Structure|Interface|Enum)\s+([A-Za-z_][\w.]*))").arg(vbModifiersPattern()),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression memberOpen(
        QStringLiteral(R"(^%1(?:(Sub|Function|Operator)\s+([A-Za-z_]\w*|New)|(Property)\s+([A-Za-z_]\w*)|(Event)\s+([A-Za-z_]\w*)|Custom\s+Event\s+([A-Za-z_]\w*)|Declare\s+(?:Auto\s+|Ansi\s+|Unicode\s+)?(Sub|Function)\s+([A-Za-z_]\w*)))").arg(vbModifiersPattern()),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression fieldDecl(
        QStringLiteral(R"(^(?:(?:Public|Private|Protected|Friend|Shared|Shadows|ReadOnly|WithEvents|Dim|Const|Static)\s+)+([A-Za-z_]\w*)(?:\([^)]*\))*\s*(?:As\s+(?:New\s+)?([^=]+))?(?:=.*)?$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression endPattern(
        QStringLiteral(R"(^End\s+(Namespace|Class|Module|Structure|Interface|Enum|Sub|Function|Property|Operator|Event|Get|Set)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression importsPattern(
        QStringLiteral(R"(^Imports\s+(?:([A-Za-z_]\w*)\s*=\s*)?([A-Za-z_][\w.]*))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression attributeRoute(
        QStringLiteral(R"re(<(Http(Get|Post|Put|Delete|Patch)|Route)(?:Attribute)?\s*\(\s*"([^"]*)")re"),
        QRegularExpression::CaseInsensitiveOption);

    struct Frame
    {
        QVariantMap symbol;
        QString blockKeyword; // Class, Sub, ... (the word after End)
        int startIndex = 0; // logical line index
        bool isType = false;
    };
    QList<Frame> stack;
    QVariantList topLevel;
    QVariantList dependencies;
    QVariantList routes;
    QString pendingRoutePrefix;
    QList<QPair<QString, QString>> pendingMethodRoutes; // method, path
    struct BodyRange
    {
        QString key;
        int firstLine;
        int lastLine;
    };
    QList<BodyRange> bodies;
    const QStringList physicalLines = text.split(QLatin1Char('\n'));

    auto snippetFor = [&](int startLine, int endLine) {
        QStringList out;
        for (int line = startLine; line <= endLine && line <= physicalLines.size() && out.size() < 10; ++line) {
            QString physical = physicalLines.at(line - 1);
            physical.remove(QLatin1Char('\r'));
            if (line == 1 && physical.startsWith(QChar(0xFEFF))) {
                physical.remove(0, 1);
            }
            out.append(physical);
        }
        if (endLine - startLine + 1 > 10) {
            out.append(QStringLiteral("..."));
        }
        return out.join(QLatin1Char('\n'));
    };

    auto attach = [&](const QVariantMap &symbol) {
        for (int index = stack.size() - 1; index >= 0; --index) {
            if (stack.at(index).isType) {
                QVariantList members = stack[index].symbol.value(QStringLiteral("members")).toList();
                members.append(symbol);
                stack[index].symbol.insert(QStringLiteral("members"), members);
                return;
            }
        }
        topLevel.append(symbol);
    };

    auto closeFrame = [&](int endLine) {
        Frame frame = stack.takeLast();
        const int startLine = frame.symbol.value(QStringLiteral("line")).toInt();
        frame.symbol.insert(QStringLiteral("endLine"), endLine);
        frame.symbol.insert(QStringLiteral("snippet"), snippetFor(startLine, endLine));
        if (!frame.isType) {
            bodies.append({symbolKey(frame.symbol), startLine + 1, endLine});
        }
        // Namespaces are transparent: their types are listed directly.
        if (frame.blockKeyword.compare(QStringLiteral("Namespace"), Qt::CaseInsensitive) == 0) {
            const QVariantList members = frame.symbol.value(QStringLiteral("members")).toList();
            for (const QVariant &member : members) {
                QVariantMap type = member.toMap();
                const QString ns = frame.symbol.value(QStringLiteral("name")).toString();
                const QString detail = type.value(QStringLiteral("detail")).toString();
                type.insert(QStringLiteral("detail"), detail.isEmpty() ? QStringLiteral("in %1").arg(ns)
                                                                       : detail + QStringLiteral(", in %1").arg(ns));
                attach(type);
            }
            return;
        }
        attach(frame.symbol);
    };

    // Close open members (not types) - used when a new member starts or a type ends
    // without the member's End statement: the damage stays with that member.
    QStringList unterminated;
    auto noteUnterminated = [&](const Frame &frame) {
        unterminated.append(QStringLiteral("%1 %2 (line %3) has no End %1")
                                .arg(frame.blockKeyword, frame.symbol.value(QStringLiteral("name")).toString())
                                .arg(frame.symbol.value(QStringLiteral("line")).toInt()));
    };
    auto closeOpenMembers = [&](int endLine) {
        while (!stack.isEmpty() && !stack.constLast().isType) {
            noteUnterminated(stack.constLast());
            closeFrame(endLine);
        }
    };

    for (int index = 0; index < lines.size(); ++index) {
        const VbLogicalLine &logical = lines.at(index);
        QString statement = logical.text.trimmed();
        // Attributes: <HttpGet("x")> Public Function ...
        auto routeIt = attributeRoute.globalMatch(statement);
        while (routeIt.hasNext()) {
            const auto match = routeIt.next();
            if (match.captured(1).compare(QStringLiteral("Route"), Qt::CaseInsensitive) == 0) {
                pendingMethodRoutes.append({QStringLiteral("ROUTE"), match.captured(3)});
            } else {
                pendingMethodRoutes.append({match.captured(2).toUpper(), match.captured(3)});
            }
        }
        if (statement.startsWith(QLatin1Char('<'))) {
            int depth = 0;
            int cut = 0;
            for (int i = 0; i < statement.size(); ++i) {
                if (statement.at(i) == QLatin1Char('<')) ++depth;
                else if (statement.at(i) == QLatin1Char('>') && --depth == 0) { cut = i + 1; if (i + 1 < statement.size() && statement.at(i + 1) != QLatin1Char('<')) break; }
            }
            statement = statement.mid(cut).trimmed();
            if (statement.isEmpty()) {
                continue;
            }
        }

        const auto importMatch = importsPattern.match(statement);
        if (importMatch.hasMatch()) {
            QVariantMap item = makeSourceContextItem(path, QStringLiteral("vbnet"), logical.line,
                                                     snippetFor(logical.line, logical.line), QStringLiteral("imports dependency"));
            item.insert(QStringLiteral("target"), importMatch.captured(2));
            item.insert(QStringLiteral("type"), QStringLiteral("imports"));
            item.insert(QStringLiteral("label"), importMatch.captured(1).isEmpty()
                                                     ? importMatch.captured(2)
                                                     : QStringLiteral("%1 = %2").arg(importMatch.captured(1), importMatch.captured(2)));
            item.insert(QStringLiteral("path"), QString());
            item.insert(QStringLiteral("exists"), true);
            dependencies.append(item);
            continue;
        }

        const auto endMatch = endPattern.match(statement);
        if (endMatch.hasMatch()) {
            const QString keyword = endMatch.captured(1);
            if (keyword.compare(QStringLiteral("Get"), Qt::CaseInsensitive) == 0
                || keyword.compare(QStringLiteral("Set"), Qt::CaseInsensitive) == 0) {
                continue;
            }
            // Find the innermost frame this End closes; frames above it were
            // left open by damage and are closed here too.
            int target = -1;
            for (int i = stack.size() - 1; i >= 0; --i) {
                if (stack.at(i).blockKeyword.compare(keyword, Qt::CaseInsensitive) == 0) {
                    target = i;
                    break;
                }
            }
            if (target < 0) {
                continue; // stray End: ignore rather than unwind everything
            }
            while (stack.size() > target + 1) {
                noteUnterminated(stack.constLast());
                closeFrame(logical.lastLine - 1);
            }
            closeFrame(logical.lastLine);
            if (keyword.compare(QStringLiteral("Class"), Qt::CaseInsensitive) == 0
                || keyword.compare(QStringLiteral("Module"), Qt::CaseInsensitive) == 0) {
                pendingRoutePrefix.clear();
            }
            continue;
        }

        const auto typeMatch = typeOpen.match(statement);
        if (typeMatch.hasMatch()) {
            closeOpenMembers(logical.line - 1);
            QString keyword = typeMatch.captured(1);
            keyword = keyword.left(1).toUpper() + keyword.mid(1).toLower();
            QString kind = keyword.toLower();
            if (kind == QStringLiteral("structure")) {
                kind = QStringLiteral("struct");
            }
            Frame frame;
            frame.blockKeyword = keyword;
            frame.isType = true;
            frame.startIndex = index;
            QString detail;
            if (statement.contains(QRegularExpression(QStringLiteral(R"(\bPartial\b)"), QRegularExpression::CaseInsensitiveOption))) {
                detail = QStringLiteral("partial");
            }
            // Inherits / Implements on following lines.
            for (int look = index + 1; look < lines.size() && look <= index + 3; ++look) {
                const QString next = lines.at(look).text.trimmed();
                static const QRegularExpression inherits(QStringLiteral(R"(^(Inherits|Implements)\s+(.+)$)"),
                                                         QRegularExpression::CaseInsensitiveOption);
                const auto inheritsMatch = inherits.match(next);
                if (!inheritsMatch.hasMatch()) {
                    break;
                }
                const QString part = inheritsMatch.captured(1).toLower() + QLatin1Char(' ') + inheritsMatch.captured(2).simplified();
                detail = detail.isEmpty() ? part : detail + QStringLiteral(", ") + part;
            }
            frame.symbol = makeSymbol(kind, typeMatch.captured(2), logical.line, detail, {}, QString());
            for (const auto &route : std::as_const(pendingMethodRoutes)) {
                if (route.first == QStringLiteral("ROUTE")) {
                    pendingRoutePrefix = route.second;
                }
            }
            pendingMethodRoutes.clear();
            stack.append(frame);
            continue;
        }

        // Enum members.
        if (!stack.isEmpty() && stack.constLast().blockKeyword == QStringLiteral("Enum")) {
            static const QRegularExpression enumMember(QStringLiteral(R"(^([A-Za-z_]\w*)\s*(?:=\s*(.+))?$)"));
            const auto match = enumMember.match(statement);
            if (match.hasMatch()) {
                QVariantList members = stack.last().symbol.value(QStringLiteral("members")).toList();
                members.append(makeSymbol(QStringLiteral("enum member"), match.captured(1), logical.line,
                                          match.captured(2).trimmed(), {}, snippetFor(logical.line, logical.line)));
                stack.last().symbol.insert(QStringLiteral("members"), members);
            }
            continue;
        }

        const auto memberMatch = memberOpen.match(statement);
        const bool insideType = !stack.isEmpty() && std::any_of(stack.cbegin(), stack.cend(), [](const Frame &f) { return f.isType; });
        if (memberMatch.hasMatch()) {
            QString keyword;
            QString name;
            bool declared = false;
            if (!memberMatch.captured(1).isEmpty()) {
                keyword = memberMatch.captured(1);
                name = memberMatch.captured(2);
            } else if (!memberMatch.captured(3).isEmpty()) {
                keyword = QStringLiteral("Property");
                name = memberMatch.captured(4);
            } else if (!memberMatch.captured(5).isEmpty()) {
                keyword = QStringLiteral("EventDecl");
                name = memberMatch.captured(6);
            } else if (!memberMatch.captured(7).isEmpty()) {
                keyword = QStringLiteral("Event");
                name = memberMatch.captured(7);
            } else {
                keyword = memberMatch.captured(8);
                name = memberMatch.captured(9);
                declared = true;
            }
            keyword = keyword.left(1).toUpper() + keyword.mid(1).toLower();
            if (keyword == QStringLiteral("Eventdecl")) {
                keyword = QStringLiteral("EventDecl");
            }
            // A member starting while another member is still open: the
            // previous one lost its End statement. Close it here.
            closeOpenMembers(logical.line - 1);

            const bool inInterface = !stack.isEmpty() && stack.constLast().blockKeyword == QStringLiteral("Interface");
            const bool mustOverride = statement.contains(QRegularExpression(QStringLiteral(R"(\bMustOverride\b)"), QRegularExpression::CaseInsensitiveOption));
            bool hasBody = !(inInterface || mustOverride || declared || keyword == QStringLiteral("EventDecl"));
            if (keyword == QStringLiteral("Property") && hasBody) {
                // Auto-property unless Get/Set/End Property follows before the next declaration.
                hasBody = false;
                for (int look = index + 1; look < lines.size(); ++look) {
                    const QString next = lines.at(look).text.trimmed();
                    static const QRegularExpression accessor(QStringLiteral(R"(^(?:(?:Public|Private|Protected|Friend)\s+)?(Get|Set)\b|^End\s+Property\b)"),
                                                             QRegularExpression::CaseInsensitiveOption);
                    if (accessor.match(next).hasMatch()) {
                        hasBody = true;
                        break;
                    }
                    if (memberOpen.match(next).hasMatch() || typeOpen.match(next).hasMatch() || endPattern.match(next).hasMatch()
                        || fieldDecl.match(next).hasMatch()) {
                        break;
                    }
                }
            }

            QString kind = QStringLiteral("method");
            if (name.compare(QStringLiteral("New"), Qt::CaseInsensitive) == 0) {
                kind = QStringLiteral("constructor");
            } else if (keyword == QStringLiteral("Property")) {
                kind = QStringLiteral("property");
            } else if (keyword == QStringLiteral("Event") || keyword == QStringLiteral("EventDecl")) {
                kind = QStringLiteral("event");
            } else if (keyword == QStringLiteral("Operator")) {
                kind = QStringLiteral("operator");
            } else if (!insideType) {
                kind = QStringLiteral("function");
            }
            int afterParams = 0;
            const int nameAt = statement.indexOf(name, memberMatch.capturedStart(0) + (memberMatch.capturedLength(0) - name.size()));
            const QString parameterText = balancedParenthesis(statement, qMax(0, nameAt), &afterParams);
            QString returnType;
            if (keyword == QStringLiteral("Function") || keyword == QStringLiteral("Property") || keyword == QStringLiteral("Operator")) {
                static const QRegularExpression asPattern(QStringLiteral(R"(^\s*As\s+(?:New\s+)?([^=]+?)(?:\s+(?:Implements|Handles)\b.*)?\s*$)"),
                                                          QRegularExpression::CaseInsensitiveOption);
                const QString tail = afterParams > 0 ? statement.mid(afterParams)
                                                     : statement.mid(memberMatch.capturedEnd(0));
                returnType = asPattern.match(tail).captured(1).trimmed();
            }
            QString detail;
            static const QRegularExpression handles(QStringLiteral(R"(\bHandles\s+([\w.]+(?:\s*,\s*[\w.]+)*))"),
                                                    QRegularExpression::CaseInsensitiveOption);
            const auto handlesMatch = handles.match(statement);
            if (handlesMatch.hasMatch()) {
                detail = QStringLiteral("handles ") + handlesMatch.captured(1);
            }
            if (mustOverride) {
                detail = detail.isEmpty() ? QStringLiteral("MustOverride") : detail + QStringLiteral(", MustOverride");
            }
            QVariantMap symbol = makeSymbol(kind, name, logical.line, detail, {}, snippetFor(logical.line, logical.lastLine));
            if (kind != QStringLiteral("event")) {
                symbol.insert(QStringLiteral("parameters"), parseVbParameters(parameterText));
                QVariantList returns;
                returns.append(QVariantMap{{QStringLiteral("text"),
                                            returnType.isEmpty() ? (keyword == QStringLiteral("Sub") ? QStringLiteral("none")
                                                                                                      : QStringLiteral("Object"))
                                                                 : returnType}});
                symbol.insert(QStringLiteral("returns"), returns);
                symbol.insert(QStringLiteral("signatureSource"), QStringLiteral("parser"));
            }
            for (const auto &route : std::as_const(pendingMethodRoutes)) {
                QString full = pendingRoutePrefix;
                if (!route.second.isEmpty()) {
                    full = full.isEmpty() ? route.second : full + QLatin1Char('/') + route.second;
                }
                QVariantMap item = makeSourceContextItem(path, QStringLiteral("vbnet"), logical.line,
                                                         snippetFor(logical.line, logical.line), QStringLiteral("route"));
                const QString method = route.first == QStringLiteral("ROUTE") ? QStringLiteral("ANY") : route.first;
                item.insert(QStringLiteral("method"), method);
                item.insert(QStringLiteral("path"), full);
                item.insert(QStringLiteral("label"), method + QStringLiteral(" ") + full);
                routes.append(item);
            }
            pendingMethodRoutes.clear();

            if (hasBody) {
                Frame frame;
                frame.symbol = symbol;
                frame.blockKeyword = keyword == QStringLiteral("EventDecl") ? QStringLiteral("Event") : keyword;
                frame.startIndex = index;
                stack.append(frame);
            } else {
                attach(symbol);
            }
            continue;
        }

        // Fields at type level.
        if (!stack.isEmpty() && stack.constLast().isType) {
            const auto fieldMatch = fieldDecl.match(statement);
            if (fieldMatch.hasMatch()) {
                const bool isConst = statement.contains(QRegularExpression(QStringLiteral(R"(\bConst\b)"), QRegularExpression::CaseInsensitiveOption));
                attach(makeSymbol(isConst ? QStringLiteral("constant") : QStringLiteral("field"), fieldMatch.captured(1),
                                  logical.line, fieldMatch.captured(2).trimmed(), {},
                                  snippetFor(logical.line, logical.lastLine)));
            }
        }
    }
    const int lastLine = physicalLines.size();
    while (!stack.isEmpty()) {
        noteUnterminated(stack.constLast());
        closeFrame(lastLine); // unterminated blocks run to end of file
    }

    // Calls: callable names referenced from each member body.
    QHash<QString, QVariantMap> byKey;
    QHash<QString, QStringList> keysByName;
    collectSymbolsByKey(topLevel, byKey, keysByName);
    QHash<QString, QStringList> keysByLowerName;
    for (auto it = keysByName.constBegin(); it != keysByName.constEnd(); ++it) {
        keysByLowerName[it.key().toLower()].append(it.value());
    }
    QHash<QString, QVariantList> callsByKey;
    QHash<QString, QVariantList> calledByByKey;
    static const QRegularExpression identifier(QStringLiteral(R"((?:^|[^\w."])(?:Me\.|MyBase\.|MyClass\.)?([A-Za-z_]\w*)\b)"));
    static const QSet<QString> keywords = {
        QStringLiteral("if"), QStringLiteral("then"), QStringLiteral("else"), QStringLiteral("end"), QStringLiteral("dim"),
        QStringLiteral("as"), QStringLiteral("new"), QStringLiteral("return"), QStringLiteral("for"), QStringLiteral("next"),
        QStringLiteral("call"), QStringLiteral("raiseevent"), QStringLiteral("not"), QStringLiteral("and"), QStringLiteral("or"),
    };
    for (const BodyRange &body : std::as_const(bodies)) {
        const QString ownerKey = body.key;
        for (int line = body.firstLine; line < body.lastLine && line <= physicalLines.size(); ++line) {
            const QString code = stripVbComment(physicalLines.at(line - 1));
            // Remove string literals.
            QString cleaned = code;
            cleaned.remove(QRegularExpression(QStringLiteral(R"("[^"]*")")));
            auto it = identifier.globalMatch(cleaned);
            while (it.hasNext()) {
                const QString name = it.next().captured(1).toLower();
                if (keywords.contains(name)) {
                    continue;
                }
                const QStringList candidates = keysByLowerName.value(name);
                if (candidates.isEmpty()) {
                    continue;
                }
                const QString targetKey = bestRelationTargetKey(candidates, byKey);
                if (targetKey.isEmpty() || targetKey == ownerKey || !byKey.contains(targetKey)) {
                    continue;
                }
                const QString targetKind = byKey.value(targetKey).value(QStringLiteral("kind")).toString();
                if (!isCallableSymbolKind(targetKind) && targetKind != QStringLiteral("event")) {
                    continue;
                }
                appendUniqueRelation(callsByKey, ownerKey, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
                appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(ownerKey)), QStringLiteral("called by"));
            }
        }
    }
    topLevel = applyRelationsToSymbols(topLevel, callsByKey, calledByByKey);

    result.insert(QStringLiteral("symbols"), topLevel);
    result.insert(QStringLiteral("dependencies"), dependencies);
    result.insert(QStringLiteral("routes"), routes);
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(topLevel.size()));
    if (!unterminated.isEmpty()) {
        result.insert(QStringLiteral("analysisHasAstErrors"), true);
        appendParserAnalysisNotice(result, QStringLiteral("warning"),
                                   QStringLiteral("Unterminated block(s), closed at the next declaration: %1.")
                                       .arg(unterminated.mid(0, 5).join(QStringLiteral("; "))),
                                   true);
    }
    return result;
}


// ---------------------------------------------------------------------------
// SQL (MySQL / MariaDB and SQL Server T-SQL): statement-aware parser
//
// Comments and string literals are blanked first (newlines kept, so every
// offset still maps to its line). Objects are found at statement starts;
// a routine body ends at the next statement-level CREATE/ALTER, a T-SQL `GO`
// line or the MySQL custom DELIMITER, so a broken body never swallows the
// next object. Relations between objects defined in the file:
//   table  -> table      references (foreign keys)
//   view / routine / trigger -> table   reads / writes
//   routine -> routine   executes (CALL / EXEC / function calls)
// ---------------------------------------------------------------------------

namespace {

QString blankSqlCommentsAndStrings(const QString &text)
{
    QString out = text;
    const int size = out.size();
    int index = 0;
    auto blank = [&](int from, int to) {
        for (int i = from; i < to && i < size; ++i) {
            if (out.at(i) != QLatin1Char('\n')) {
                out[i] = QLatin1Char(' ');
            }
        }
    };
    while (index < size) {
        const QChar ch = out.at(index);
        const QChar next = index + 1 < size ? out.at(index + 1) : QChar();
        if ((ch == QLatin1Char('-') && next == QLatin1Char('-')) || ch == QLatin1Char('#')) {
            int end = out.indexOf(QLatin1Char('\n'), index);
            if (end < 0) end = size;
            blank(index, end);
            index = end;
        } else if (ch == QLatin1Char('/') && next == QLatin1Char('*')) {
            int end = out.indexOf(QStringLiteral("*/"), index + 2);
            end = end < 0 ? size : end + 2;
            blank(index, end);
            index = end;
        } else if (ch == QLatin1Char('\'')) {
            int end = index + 1;
            while (end < size) {
                if (out.at(end) == QLatin1Char('\n')) {
                    // An unterminated string must not run across a batch or
                    // delimiter boundary (a GO / DELIMITER line).
                    static const QRegularExpression batchLine(QStringLiteral(R"(^[ \t]*(GO|DELIMITER)\b)"),
                                                              QRegularExpression::CaseInsensitiveOption);
                    const int lineEnd = out.indexOf(QLatin1Char('\n'), end + 1);
                    if (batchLine.match(out.mid(end + 1, (lineEnd < 0 ? size : lineEnd) - end - 1)).hasMatch()) {
                        break;
                    }
                }
                if (out.at(end) == QLatin1Char('\'')) {
                    if (end + 1 < size && out.at(end + 1) == QLatin1Char('\'')) { end += 2; continue; }
                    break;
                }
                if (out.at(end) == QLatin1Char('\\')) { ++end; }
                ++end;
            }
            blank(index + 1, end); // keep the quotes
            index = end + 1;
        } else {
            ++index;
        }
    }
    return out;
}

QString sqlUnquote(QString name)
{
    name = name.trimmed();
    QStringList parts;
    for (QString part : name.split(QLatin1Char('.'))) {
        part = part.trimmed();
        if ((part.startsWith(QLatin1Char('`')) && part.endsWith(QLatin1Char('`')))
            || (part.startsWith(QLatin1Char('[')) && part.endsWith(QLatin1Char(']')))
            || (part.startsWith(QLatin1Char('"')) && part.endsWith(QLatin1Char('"')))) {
            part = part.mid(1, part.size() - 2);
        }
        parts.append(part);
    }
    return parts.join(QLatin1Char('.'));
}

QString sqlBaseName(const QString &qualified)
{
    return qualified.section(QLatin1Char('.'), -1).toLower();
}

const QString &sqlNamePattern()
{
    static const QString pattern = QStringLiteral(
        R"((?:(?:`[^`]+`|\[[^\]]+\]|"[^"]+"|[A-Za-z_@#][\w$#@]*)\s*\.\s*)*(?:`[^`]+`|\[[^\]]+\]|"[^"]+"|[A-Za-z_@#][\w$#@]*))");
    return pattern;
}

} // namespace

QVariantMap SymbolParser::parseSql(const QString &path, const QString &text) const
{
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("sql"));
    const QString clean = blankSqlCommentsAndStrings(text);
    const QStringList physicalLines = text.split(QLatin1Char('\n'));

    // Statement boundaries that always end an object: GO lines, DELIMITER lines,
    // and the start of the next top-level CREATE / ALTER / DROP statement.
    static const QRegularExpression boundaryPattern(
        QStringLiteral(R"(^[ \t]*(?:GO\b|DELIMITER\s|(?:CREATE|ALTER|DROP)\s))"),
        QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    QList<int> boundaries;
    auto boundaryIt = boundaryPattern.globalMatch(clean);
    while (boundaryIt.hasNext()) {
        boundaries.append(boundaryIt.next().capturedStart(0));
    }
    boundaries.append(clean.size());

    static const QRegularExpression createPattern(
        QStringLiteral(R"(^[ \t]*CREATE\s+(?:OR\s+(?:REPLACE|ALTER)\s+)?(?:DEFINER\s*=\s*\S+\s+)?(?:ALGORITHM\s*=\s*\w+\s+)?(?:SQL\s+SECURITY\s+\w+\s+)?(?:TEMPORARY\s+|TEMP\s+)?(?:UNIQUE\s+|CLUSTERED\s+|NONCLUSTERED\s+)*(TABLE|VIEW|PROCEDURE|PROC|FUNCTION|TRIGGER|INDEX|TYPE|SEQUENCE|SCHEMA|DATABASE)\s+(?:IF\s+NOT\s+EXISTS\s+)?(%1))").arg(sqlNamePattern()),
        QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);

    struct SqlObject
    {
        QVariantMap symbol;
        int start = 0;
        int end = 0;
        QString kind;
    };
    QList<SqlObject> objects;
    auto snippetFor = [&](int startLine, int endLine) {
        QStringList out;
        for (int line = startLine; line <= endLine && line <= physicalLines.size() && out.size() < 10; ++line) {
            QString physical = physicalLines.at(line - 1);
            physical.remove(QLatin1Char('\r'));
            out.append(physical);
        }
        if (endLine - startLine + 1 > 10) {
            out.append(QStringLiteral("..."));
        }
        return out.join(QLatin1Char('\n'));
    };

    auto createIt = createPattern.globalMatch(clean);
    while (createIt.hasNext()) {
        const auto match = createIt.next();
        const int start = match.capturedStart(0);
        int end = clean.size();
        for (int boundary : std::as_const(boundaries)) {
            if (boundary > start + 1) {
                end = boundary;
                break;
            }
        }
        QString kind = match.captured(1).toLower();
        if (kind == QStringLiteral("proc")) {
            kind = QStringLiteral("procedure");
        }
        const QString name = sqlUnquote(match.captured(2));
        const QString body = clean.mid(start, end - start);
        const int startLine = lineNumberAtOffset(clean, start);
        int endLine = lineNumberAtOffset(clean, qMax(start, end - 1));
        // Trim trailing blank lines from the extent.
        while (endLine > startLine && endLine <= physicalLines.size() && physicalLines.at(endLine - 1).trimmed().isEmpty()) {
            --endLine;
        }

        QVariantMap symbol = makeSymbol(kind, name, startLine, QString(), {}, snippetFor(startLine, endLine));
        symbol.insert(QStringLiteral("endLine"), endLine);
        QVariantList members;

        if (kind == QStringLiteral("table")) {
            // Column list: the first balanced (...) after the name.
            const int open = body.indexOf(QLatin1Char('('), match.capturedEnd(0) - start);
            int depth = 0;
            int itemStart = open + 1;
            auto handleItem = [&](int from, int to) {
                const QString item = body.mid(from, to - from).simplified();
                if (item.isEmpty()) {
                    return;
                }
                const int line = lineNumberAtOffset(clean, start + from + (body.mid(from, to - from).size() - body.mid(from, to - from).trimmed().size() > 0
                                                                               ? body.mid(from, to - from).indexOf(body.mid(from, to - from).trimmed().left(1))
                                                                               : 0));
                static const QRegularExpression constraintStart(
                    QStringLiteral(R"(^(?:CONSTRAINT\s+\S+\s+)?(PRIMARY\s+KEY|FOREIGN\s+KEY|UNIQUE(?:\s+(?:KEY|INDEX))?|KEY|INDEX|CHECK|FULLTEXT|SPATIAL)\b)"),
                    QRegularExpression::CaseInsensitiveOption);
                const auto constraint = constraintStart.match(item);
                if (constraint.hasMatch()) {
                    QString constraintKind = constraint.captured(1).simplified().toLower();
                    static const QRegularExpression references(
                        QStringLiteral(R"(REFERENCES\s+(%1))").arg(sqlNamePattern()), QRegularExpression::CaseInsensitiveOption);
                    const auto ref = references.match(item);
                    QVariantMap member = makeSymbol(constraintKind == QStringLiteral("foreign key") ? QStringLiteral("foreign key")
                                                                                                    : QStringLiteral("key"),
                                                    item.left(80), line,
                                                    ref.hasMatch() ? QStringLiteral("references %1").arg(sqlUnquote(ref.captured(1))) : constraintKind,
                                                    {}, snippetFor(line, line));
                    if (ref.hasMatch()) {
                        member.insert(QStringLiteral("references"), sqlUnquote(ref.captured(1)));
                    }
                    members.append(member);
                    return;
                }
                static const QRegularExpression columnPattern(
                    QStringLiteral(R"(^(%1)\s+(.+)$)").arg(sqlNamePattern()), QRegularExpression::CaseInsensitiveOption);
                const auto column = columnPattern.match(item);
                if (column.hasMatch()) {
                    QVariantMap member = makeSymbol(QStringLiteral("column"), sqlUnquote(column.captured(1)), line,
                                                    column.captured(2).left(80), {}, snippetFor(line, line));
                    static const QRegularExpression inlineRef(
                        QStringLiteral(R"(REFERENCES\s+(%1))").arg(sqlNamePattern()), QRegularExpression::CaseInsensitiveOption);
                    const auto ref = inlineRef.match(column.captured(2));
                    if (ref.hasMatch()) {
                        member.insert(QStringLiteral("references"), sqlUnquote(ref.captured(1)));
                    }
                    members.append(member);
                }
            };
            if (open >= 0) {
                for (int i = open; i < body.size(); ++i) {
                    const QChar ch = body.at(i);
                    if (ch == QLatin1Char('(')) {
                        ++depth;
                    } else if (ch == QLatin1Char(')')) {
                        if (--depth == 0) {
                            handleItem(itemStart, i);
                            break;
                        }
                    } else if (ch == QLatin1Char(',') && depth == 1) {
                        handleItem(itemStart, i);
                        itemStart = i + 1;
                    }
                }
            }
        } else if (kind == QStringLiteral("procedure") || kind == QStringLiteral("function")) {
            // Parameters: (...) after the name, or T-SQL @params up to AS / RETURNS.
            QString parameterText;
            const int afterName = match.capturedEnd(0) - start;
            int cursor = afterName;
            while (cursor < body.size() && body.at(cursor).isSpace()) {
                ++cursor;
            }
            int paramsEnd = cursor;
            if (cursor < body.size() && body.at(cursor) == QLatin1Char('(')) {
                int depth = 0;
                for (int i = cursor; i < body.size(); ++i) {
                    if (body.at(i) == QLatin1Char('(')) ++depth;
                    else if (body.at(i) == QLatin1Char(')') && --depth == 0) {
                        parameterText = body.mid(cursor + 1, i - cursor - 1);
                        paramsEnd = i + 1;
                        break;
                    }
                }
            } else {
                static const QRegularExpression tsqlEnd(QStringLiteral(R"(\b(AS|RETURNS|WITH)\b)"), QRegularExpression::CaseInsensitiveOption);
                const auto endMatch = tsqlEnd.match(body, cursor);
                if (endMatch.hasMatch()) {
                    parameterText = body.mid(cursor, endMatch.capturedStart(0) - cursor);
                    paramsEnd = endMatch.capturedStart(0);
                }
            }
            QVariantList parameters;
            for (QString part : splitTopLevelSignatureParts(parameterText.simplified())) {
                part = part.trimmed();
                static const QRegularExpression paramPattern(
                    QStringLiteral(R"(^(?:(IN|OUT|INOUT)\s+)?(@?[A-Za-z_][\w@$#]*)\s+(?:AS\s+)?(.+?)(?:\s*=\s*(\S.*?))?(?:\s+(OUTPUT|OUT|READONLY))?$)"),
                    QRegularExpression::CaseInsensitiveOption);
                const auto param = paramPattern.match(part);
                if (!param.hasMatch()) {
                    continue;
                }
                QVariantMap parameter = makeSignatureParameter(param.captured(2), param.captured(3).trimmed());
                const QString direction = !param.captured(1).isEmpty() ? param.captured(1).toUpper()
                                                                         : (param.captured(5).isEmpty() ? QString() : QStringLiteral("OUT"));
                if (!direction.isEmpty()) {
                    parameter.insert(QStringLiteral("direction"), direction);
                }
                if (!param.captured(4).isEmpty()) {
                    parameter.insert(QStringLiteral("default"), param.captured(4).trimmed());
                }
                parameters.append(parameter);
            }
            symbol.insert(QStringLiteral("parameters"), parameters);
            QVariantList returns;
            static const QRegularExpression returnsPattern(QStringLiteral(R"(\bRETURNS\s+(@\w+\s+)?(TABLE\b|[A-Za-z_][\w]*(?:\s*\([^)]*\))?))"),
                                                           QRegularExpression::CaseInsensitiveOption);
            const auto returnsMatch = returnsPattern.match(body, paramsEnd);
            if (kind == QStringLiteral("function") && returnsMatch.hasMatch()) {
                returns.append(QVariantMap{{QStringLiteral("text"), returnsMatch.captured(2).simplified()}});
            } else {
                returns.append(QVariantMap{{QStringLiteral("text"),
                                            kind == QStringLiteral("function") ? QStringLiteral("value") : QStringLiteral("result sets / OUT parameters")}});
            }
            symbol.insert(QStringLiteral("returns"), returns);
            symbol.insert(QStringLiteral("signatureSource"), QStringLiteral("parser"));
        } else if (kind == QStringLiteral("trigger")) {
            static const QRegularExpression triggerPattern(
                QStringLiteral(R"(\b(BEFORE|AFTER|INSTEAD\s+OF|FOR)\s+(INSERT|UPDATE|DELETE)(?:\s*,\s*(?:INSERT|UPDATE|DELETE))*\s+ON\s+(%1))").arg(sqlNamePattern()),
                QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression tsqlTrigger(
                QStringLiteral(R"(\bON\s+(%1)\s+(?:AFTER|FOR|INSTEAD\s+OF)\s+([\w\s,]+?)\s+AS\b)").arg(sqlNamePattern()),
                QRegularExpression::CaseInsensitiveOption);
            const auto trigger = triggerPattern.match(body);
            if (trigger.hasMatch()) {
                symbol.insert(QStringLiteral("detail"), QStringLiteral("%1 %2 on %3").arg(trigger.captured(1).toUpper(), trigger.captured(2).toUpper(), sqlUnquote(trigger.captured(3))));
                symbol.insert(QStringLiteral("triggerTable"), sqlUnquote(trigger.captured(3)));
            } else {
                const auto tsql = tsqlTrigger.match(body);
                if (tsql.hasMatch()) {
                    symbol.insert(QStringLiteral("detail"), QStringLiteral("%1 on %2").arg(tsql.captured(2).simplified().toUpper(), sqlUnquote(tsql.captured(1))));
                    symbol.insert(QStringLiteral("triggerTable"), sqlUnquote(tsql.captured(1)));
                }
            }
        } else if (kind == QStringLiteral("index")) {
            static const QRegularExpression onTable(QStringLiteral(R"(\bON\s+(%1))").arg(sqlNamePattern()), QRegularExpression::CaseInsensitiveOption);
            const auto on = onTable.match(body);
            if (on.hasMatch()) {
                symbol.insert(QStringLiteral("detail"), QStringLiteral("on %1").arg(sqlUnquote(on.captured(1))));
                symbol.insert(QStringLiteral("indexTable"), sqlUnquote(on.captured(1)));
            }
        }
        symbol.insert(QStringLiteral("members"), members);
        objects.append({symbol, start, end, kind});
    }

    // Relations between objects of this file.
    QHash<QString, int> tableIndex;   // base name -> object index
    QHash<QString, int> routineIndex; // base name -> object index
    for (int i = 0; i < objects.size(); ++i) {
        const QString base = sqlBaseName(objects.at(i).symbol.value(QStringLiteral("name")).toString());
        if (objects.at(i).kind == QStringLiteral("table") || objects.at(i).kind == QStringLiteral("view")) {
            tableIndex.insert(base, i);
        } else if (objects.at(i).kind == QStringLiteral("procedure") || objects.at(i).kind == QStringLiteral("function")) {
            routineIndex.insert(base, i);
        }
    }
    QHash<QString, QVariantList> callsByKey;
    QHash<QString, QVariantList> calledByByKey;
    auto link = [&](int from, int to, const QString &forward, const QString &backward) {
        if (from == to || from < 0 || to < 0) {
            return;
        }
        const QVariantMap &source = objects.at(from).symbol;
        const QVariantMap &target = objects.at(to).symbol;
        appendUniqueRelation(callsByKey, symbolKey(source), relationFromSymbol(target), forward);
        appendUniqueRelation(calledByByKey, symbolKey(target), relationFromSymbol(source), backward);
    };
    static const QRegularExpression readPattern(QStringLiteral(R"(\b(?:FROM|JOIN)\s+(%1))").arg(sqlNamePattern()),
                                                QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression writePattern(QStringLiteral(R"(\b(?:INSERT\s+(?:IGNORE\s+)?(?:INTO\s+)?|UPDATE\s+|DELETE\s+FROM\s+|MERGE\s+(?:INTO\s+)?|REPLACE\s+INTO\s+|TRUNCATE\s+TABLE\s+)(%1))").arg(sqlNamePattern()),
                                                 QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression execPattern(QStringLiteral(R"(\b(?:CALL|EXEC|EXECUTE)\s+(?:@\w+\s*=\s*)?(%1))").arg(sqlNamePattern()),
                                                QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression callPattern(QStringLiteral(R"((%1)\s*\()").arg(sqlNamePattern()));
    for (int i = 0; i < objects.size(); ++i) {
        const SqlObject &object = objects.at(i);
        if (object.kind == QStringLiteral("table")) {
            for (const QVariant &memberValue : object.symbol.value(QStringLiteral("members")).toList()) {
                const QString referenced = memberValue.toMap().value(QStringLiteral("references")).toString();
                if (!referenced.isEmpty()) {
                    link(i, tableIndex.value(sqlBaseName(referenced), -1), QStringLiteral("references"), QStringLiteral("referenced by"));
                }
            }
            continue;
        }
        if (object.kind == QStringLiteral("trigger")) {
            link(i, tableIndex.value(sqlBaseName(object.symbol.value(QStringLiteral("triggerTable")).toString()), -1),
                 QStringLiteral("fires on"), QStringLiteral("trigger"));
        }
        if (object.kind == QStringLiteral("index")) {
            link(i, tableIndex.value(sqlBaseName(object.symbol.value(QStringLiteral("indexTable")).toString()), -1),
                 QStringLiteral("indexes"), QStringLiteral("indexed by"));
            continue;
        }
        const QString body = clean.mid(object.start, object.end - object.start);
        // Writes first: a table that is both written and read (DELETE FROM x,
        // UPDATE x ... FROM x) is reported as written.
        auto writeIt = writePattern.globalMatch(body);
        while (writeIt.hasNext()) {
            link(i, tableIndex.value(sqlBaseName(sqlUnquote(writeIt.next().captured(1))), -1), QStringLiteral("writes"), QStringLiteral("written by"));
        }
        auto readIt = readPattern.globalMatch(body);
        while (readIt.hasNext()) {
            link(i, tableIndex.value(sqlBaseName(sqlUnquote(readIt.next().captured(1))), -1), QStringLiteral("reads"), QStringLiteral("read by"));
        }
        auto execIt = execPattern.globalMatch(body);
        while (execIt.hasNext()) {
            link(i, routineIndex.value(sqlBaseName(sqlUnquote(execIt.next().captured(1))), -1), QStringLiteral("executes"), QStringLiteral("executed by"));
        }
        // Function calls in the body (skip the object's own header).
        const int headerEnd = body.indexOf(QLatin1Char('('));
        auto callIt = callPattern.globalMatch(body, headerEnd >= 0 ? headerEnd + 1 : 0);
        while (callIt.hasNext()) {
            const QString called = sqlBaseName(sqlUnquote(callIt.next().captured(1)));
            const int target = routineIndex.value(called, -1);
            if (target >= 0 && objects.at(target).kind == QStringLiteral("function")) {
                link(i, target, QStringLiteral("calls"), QStringLiteral("called by"));
            }
        }
    }

    QVariantList symbols;
    for (const SqlObject &object : std::as_const(objects)) {
        symbols.append(object.symbol);
    }
    symbols = applyRelationsToSymbols(symbols, callsByKey, calledByByKey);

    // Dependencies: USE db, SOURCE file (mysql), :r file (sqlcmd).
    QVariantList dependencies;
    static const QRegularExpression usePattern(QStringLiteral(R"(^[ \t]*(USE|SOURCE|\\\.|:r)\s+([^\s;]+))"),
                                               QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    auto useIt = usePattern.globalMatch(clean);
    while (useIt.hasNext()) {
        const auto match = useIt.next();
        const QString verb = match.captured(1).toUpper();
        QString target = match.captured(2);
        const int line = lineNumberAtOffset(clean, match.capturedStart(0));
        const bool isFile = verb != QStringLiteral("USE");
        if (isFile) {
            // the blanked text lost quoted file names; read from the original
            const QString original = physicalLines.value(line - 1);
            static const QRegularExpression fileName(QStringLiteral(R"((?:SOURCE|\\\.|:r)\s+['"]?([^'";\s]+))"), QRegularExpression::CaseInsensitiveOption);
            target = fileName.match(original).captured(1);
        }
        QVariantMap item = makeSourceContextItem(path, QStringLiteral("sql"), line, snippetFor(line, line),
                                                 isFile ? QStringLiteral("script dependency") : QStringLiteral("database"));
        item.insert(QStringLiteral("target"), sqlUnquote(target));
        item.insert(QStringLiteral("type"), isFile ? QStringLiteral("include") : QStringLiteral("use"));
        item.insert(QStringLiteral("label"), sqlUnquote(target));
        const QString resolved = isFile ? QDir::cleanPath(QFileInfo(path).dir().absoluteFilePath(target)) : QString();
        item.insert(QStringLiteral("path"), resolved);
        item.insert(QStringLiteral("exists"), isFile ? QFileInfo::exists(resolved) : true);
        dependencies.append(item);
    }

    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), dependencies);
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    int tables = 0;
    int routines = 0;
    for (const SqlObject &object : std::as_const(objects)) {
        tables += object.kind == QStringLiteral("table") ? 1 : 0;
        routines += (object.kind == QStringLiteral("procedure") || object.kind == QStringLiteral("function")) ? 1 : 0;
    }
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 objects (%2 tables, %3 routines)").arg(objects.size()).arg(tables).arg(routines));
    return result;
}


// All children of `node` stored under field `fieldName` (e.g. every name of
// a Go `a, b int` parameter declaration).
static QList<TSNode> fieldNodes(TSNode node, const char *fieldName)
{
    QList<TSNode> nodes;
    const uint32_t count = ts_node_child_count(node);
    for (uint32_t index = 0; index < count; ++index) {
        const char *name = ts_node_field_name_for_child(node, index);
        if (name && std::strcmp(name, fieldName) == 0) {
            nodes.append(ts_node_child(node, index));
        }
    }
    return nodes;
}

static QVariantMap makeSymbolStatic(const QString &kind, const QString &name, int line, const QString &detail,
                                    const QVariantList &members, const QString &snippet)
{
    return SymbolParser::makeSymbolPublic(kind, name, line, detail, members, snippet);
}

// Call relations for AST languages whose callable symbols were registered by
// node position: walk the tree, attribute each call to the innermost
// registered owner, and link it to same-file callables by name.
static QVariantList applyPositionalCallRelations(const QVariantList &symbols,
                                                 TSNode root,
                                                 const QByteArray &source,
                                                 const QHash<uint32_t, QString> &ownerKeyByStart,
                                                 const QSet<QString> &callNodeTypes,
                                                 const std::function<QString(TSNode)> &calleeName)
{
    QHash<QString, QVariantMap> byKey;
    QHash<QString, QStringList> keysByName;
    collectSymbolsByKey(symbols, byKey, keysByName);
    QHash<QString, QVariantList> callsByKey;
    QHash<QString, QVariantList> calledByByKey;
    std::function<void(TSNode, const QString &)> visit = [&](TSNode node, const QString &currentKey) {
        QString activeKey = currentKey;
        const auto owner = ownerKeyByStart.constFind(ts_node_start_byte(node));
        if (owner != ownerKeyByStart.constEnd() && byKey.contains(*owner)) {
            activeKey = *owner;
        }
        if (!activeKey.isEmpty() && callNodeTypes.contains(QLatin1String(ts_node_type(node)))) {
            const QString target = calleeName(node);
            const QStringList candidates = keysByName.value(target);
            if (!target.isEmpty() && !candidates.isEmpty()) {
                const QString targetKey = bestRelationTargetKey(candidates, byKey);
                if (!targetKey.isEmpty() && targetKey != activeKey && byKey.contains(targetKey)
                    && isCallableSymbolKind(byKey.value(targetKey).value(QStringLiteral("kind")).toString())) {
                    appendUniqueRelation(callsByKey, activeKey, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
                    appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(activeKey)), QStringLiteral("called by"));
                }
            }
        }
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t index = 0; index < count; ++index) {
            visit(ts_node_named_child(node, index), activeKey);
        }
    };
    visit(root, QString());
    Q_UNUSED(source);
    return applyRelationsToSymbols(symbols, callsByKey, calledByByKey);
}

// Attach `member` to the top-level symbol named `ownerName`, creating a
// grouping symbol when the owner is declared elsewhere (another file of the
// package, or a class declared in a header).
static void attachToOwner(QVariantList &symbols, const QString &ownerName, const QVariantMap &member,
                          const QString &kindIfNew, const QString &detailIfNew)
{
    for (int index = 0; index < symbols.size(); ++index) {
        QVariantMap owner = symbols.at(index).toMap();
        if (owner.value(QStringLiteral("name")).toString() == ownerName
            && owner.value(QStringLiteral("kind")).toString() != QStringLiteral("function")) {
            QVariantList members = owner.value(QStringLiteral("members")).toList();
            members.append(member);
            owner.insert(QStringLiteral("members"), members);
            symbols[index] = owner;
            return;
        }
    }
    symbols.append(makeSymbolStatic(kindIfNew, ownerName, member.value(QStringLiteral("line")).toInt(), detailIfNew, {member},
                                    member.value(QStringLiteral("snippet")).toString()));
}

QVariantMap SymbolParser::parseGoTreeSitter(const QString &path, const QString &text) const
{
    const QByteArray source = text.toUtf8();
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("go"));
    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, languageForName(QStringLiteral("go")))) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return result;
    }
    TSTree *tree = parseAnalysedSource(parser, source);
    TSNode root = ts_tree_root_node(tree);

    QVariantList symbols;
    QVariantList dependencies;
    QVariantList routes;
    QHash<uint32_t, QString> ownerKeyByStart;
    QString packageName;

    auto parametersOf = [&](TSNode list) {
        QVariantList parameters;
        const uint32_t count = ts_node_named_child_count(list);
        for (uint32_t index = 0; index < count; ++index) {
            TSNode declaration = ts_node_named_child(list, index);
            const QString declarationType = tsType(declaration);
            if (declarationType != QStringLiteral("parameter_declaration")
                && declarationType != QStringLiteral("variadic_parameter_declaration")) {
                continue;
            }
            QString type = nodeText(fieldNode(declaration, "type"), source).simplified();
            if (declarationType == QStringLiteral("variadic_parameter_declaration")) {
                type = QStringLiteral("...") + type;
            }
            const QList<TSNode> names = fieldNodes(declaration, "name");
            if (names.isEmpty()) {
                parameters.append(makeSignatureParameter(QStringLiteral("_"), type));
            }
            for (const TSNode &name : names) {
                parameters.append(makeSignatureParameter(nodeText(name, source), type));
            }
        }
        return parameters;
    };
    auto returnsOf = [&](TSNode result) {
        QVariantList returns;
        const QString text = nodeText(result, source).simplified();
        returns.append(QVariantMap{{QStringLiteral("text"), text.isEmpty() ? QStringLiteral("none") : text}});
        return returns;
    };
    auto makeCallable = [&](const QString &kind, const QString &name, TSNode node, const QString &detail) {
        QVariantMap symbol = makeSymbol(kind, name, nodeLine(node), detail, {}, nodeSnippet(node, source));
        symbol.insert(QStringLiteral("endLine"), static_cast<int>(ts_node_end_point(node).row) + 1);
        symbol.insert(QStringLiteral("parameters"), parametersOf(fieldNode(node, "parameters")));
        symbol.insert(QStringLiteral("returns"), returnsOf(fieldNode(node, "result")));
        symbol.insert(QStringLiteral("signatureSource"), QStringLiteral("ast"));
        return symbol;
    };

    const uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode node = ts_node_named_child(root, i);
        const QString type = tsType(node);
        if (type == QStringLiteral("package_clause")) {
            packageName = nodeText(ts_node_named_child(node, 0), source);
        } else if (type == QStringLiteral("import_declaration")) {
            std::function<void(TSNode)> collect = [&](TSNode n) {
                if (tsType(n) == QStringLiteral("import_spec")) {
                    QString target = nodeText(fieldNode(n, "path"), source);
                    target = target.mid(1, target.size() - 2);
                    const QString alias = nodeText(fieldNode(n, "name"), source);
                    QVariantMap item = makeSourceContextItem(path, QStringLiteral("go"), nodeLine(n), nodeSnippet(n, source, 1),
                                                             QStringLiteral("import dependency"));
                    item.insert(QStringLiteral("target"), target);
                    item.insert(QStringLiteral("type"), QStringLiteral("import"));
                    item.insert(QStringLiteral("label"), alias.isEmpty() ? target : QStringLiteral("%1 %2").arg(alias, target));
                    item.insert(QStringLiteral("path"), QString());
                    item.insert(QStringLiteral("exists"), true);
                    dependencies.append(item);
                    return;
                }
                const uint32_t c = ts_node_named_child_count(n);
                for (uint32_t k = 0; k < c; ++k) {
                    collect(ts_node_named_child(n, k));
                }
            };
            collect(node);
        } else if (type == QStringLiteral("function_declaration")) {
            const QString name = nodeText(fieldNode(node, "name"), source);
            const QVariantMap symbol = makeCallable(QStringLiteral("function"), name, node, QString());
            ownerKeyByStart.insert(ts_node_start_byte(node), symbolKey(symbol));
            symbols.append(symbol);
        } else if (type == QStringLiteral("method_declaration")) {
            const QString name = nodeText(fieldNode(node, "name"), source);
            TSNode receiver = ts_node_named_child(fieldNode(node, "receiver"), 0);
            QString receiverType = nodeText(fieldNode(receiver, "type"), source).trimmed();
            receiverType.remove(QLatin1Char('*'));
            receiverType = receiverType.section(QLatin1Char('['), 0, 0).trimmed(); // generic receivers
            const QVariantMap symbol = makeCallable(QStringLiteral("method"), name, node,
                                                    QStringLiteral("(%1)").arg(nodeText(receiver, source).simplified()));
            ownerKeyByStart.insert(ts_node_start_byte(node), symbolKey(symbol));
            attachToOwner(symbols, receiverType, symbol, QStringLiteral("type"),
                          QStringLiteral("methods; type declared in another file"));
        } else if (type == QStringLiteral("type_declaration")) {
            const uint32_t specs = ts_node_named_child_count(node);
            for (uint32_t k = 0; k < specs; ++k) {
                TSNode spec = ts_node_named_child(node, k);
                const QString specType = tsType(spec);
                if (specType != QStringLiteral("type_spec") && specType != QStringLiteral("type_alias")) {
                    continue;
                }
                const QString name = nodeText(fieldNode(spec, "name"), source);
                TSNode typeNode = fieldNode(spec, "type");
                const QString typeKind = tsType(typeNode);
                QString kind = QStringLiteral("type");
                QVariantList members;
                if (typeKind == QStringLiteral("struct_type")) {
                    kind = QStringLiteral("struct");
                    std::function<void(TSNode)> fields = [&](TSNode n) {
                        if (tsType(n) == QStringLiteral("field_declaration")) {
                            const QString fieldType = nodeText(fieldNode(n, "type"), source).simplified();
                            const QList<TSNode> names = fieldNodes(n, "name");
                            if (names.isEmpty()) {
                                members.append(makeSymbol(QStringLiteral("field"), fieldType, nodeLine(n), QStringLiteral("embedded"), {}, nodeSnippet(n, source, 1)));
                            }
                            for (const TSNode &fieldName : names) {
                                members.append(makeSymbol(QStringLiteral("field"), nodeText(fieldName, source), nodeLine(n), fieldType, {}, nodeSnippet(n, source, 1)));
                            }
                            return;
                        }
                        const uint32_t c = ts_node_named_child_count(n);
                        for (uint32_t m = 0; m < c; ++m) {
                            fields(ts_node_named_child(n, m));
                        }
                    };
                    fields(typeNode);
                } else if (typeKind == QStringLiteral("interface_type")) {
                    kind = QStringLiteral("interface");
                    const uint32_t c = ts_node_named_child_count(typeNode);
                    for (uint32_t m = 0; m < c; ++m) {
                        TSNode element = ts_node_named_child(typeNode, m);
                        if (tsType(element) == QStringLiteral("method_elem")) {
                            members.append(makeCallable(QStringLiteral("method"), nodeText(fieldNode(element, "name"), source), element,
                                                        QStringLiteral("interface method")));
                        } else {
                            members.append(makeSymbol(QStringLiteral("embedded"), nodeText(element, source).simplified(), nodeLine(element),
                                                      QString(), {}, nodeSnippet(element, source, 1)));
                        }
                    }
                }
                QVariantMap symbol = makeSymbol(kind, name, nodeLine(spec),
                                                specType == QStringLiteral("type_alias") ? QStringLiteral("alias of %1").arg(nodeText(typeNode, source).simplified())
                                                : (kind == QStringLiteral("type") ? nodeText(typeNode, source).simplified().left(60) : QString()),
                                                members, nodeSnippet(spec, source));
                symbol.insert(QStringLiteral("endLine"), static_cast<int>(ts_node_end_point(spec).row) + 1);
                // Methods may already have been grouped under a placeholder.
                bool merged = false;
                for (int index = 0; index < symbols.size(); ++index) {
                    QVariantMap existing = symbols.at(index).toMap();
                    if (existing.value(QStringLiteral("name")).toString() == name
                        && existing.value(QStringLiteral("kind")).toString() == QStringLiteral("type")
                        && existing.value(QStringLiteral("detail")).toString().startsWith(QStringLiteral("methods;"))) {
                        QVariantList combined = members;
                        combined.append(existing.value(QStringLiteral("members")).toList());
                        symbol.insert(QStringLiteral("members"), combined);
                        symbols[index] = symbol;
                        merged = true;
                        break;
                    }
                }
                if (!merged) {
                    symbols.append(symbol);
                }
            }
        } else if (type == QStringLiteral("const_declaration") || type == QStringLiteral("var_declaration")) {
            std::function<void(TSNode)> specs = [&](TSNode n) {
                const QString specType = tsType(n);
                if (specType == QStringLiteral("const_spec") || specType == QStringLiteral("var_spec")) {
                    for (const TSNode &name : fieldNodes(n, "name")) {
                        symbols.append(makeSymbol(specType == QStringLiteral("const_spec") ? QStringLiteral("constant") : QStringLiteral("variable"),
                                                  nodeText(name, source), nodeLine(n),
                                                  nodeText(fieldNode(n, "type"), source).simplified(), {}, nodeSnippet(n, source, 2)));
                    }
                    return;
                }
                const uint32_t c = ts_node_named_child_count(n);
                for (uint32_t m = 0; m < c; ++m) {
                    specs(ts_node_named_child(n, m));
                }
            };
            specs(node);
        }
    }

    // Methods of types declared later in the file were attached to a
    // placeholder; merge happens above. Now calls and routes.
    auto calleeName = [&](TSNode call) {
        TSNode function = fieldNode(call, "function");
        if (tsType(function) == QStringLiteral("selector_expression")) {
            return nodeText(fieldNode(function, "field"), source);
        }
        if (tsType(function) == QStringLiteral("identifier")) {
            return nodeText(function, source);
        }
        return QString();
    };
    symbols = applyPositionalCallRelations(symbols, root, source, ownerKeyByStart, {QStringLiteral("call_expression")}, calleeName);

    static const QSet<QString> routeMethods = {
        QStringLiteral("GET"), QStringLiteral("POST"), QStringLiteral("PUT"), QStringLiteral("DELETE"), QStringLiteral("PATCH"),
        QStringLiteral("HEAD"), QStringLiteral("OPTIONS"), QStringLiteral("Any"), QStringLiteral("Get"), QStringLiteral("Post"),
        QStringLiteral("Put"), QStringLiteral("Delete"), QStringLiteral("Patch"), QStringLiteral("Handle"), QStringLiteral("HandleFunc"),
    };
    std::function<void(TSNode)> scanRoutes = [&](TSNode node) {
        if (tsType(node) == QStringLiteral("call_expression")) {
            TSNode function = fieldNode(node, "function");
            if (tsType(function) == QStringLiteral("selector_expression")) {
                const QString method = nodeText(fieldNode(function, "field"), source);
                TSNode arguments = fieldNode(node, "arguments");
                TSNode first = ts_node_named_child(arguments, 0);
                const QString firstType = tsType(first);
                if (routeMethods.contains(method) && (firstType == QStringLiteral("interpreted_string_literal") || firstType == QStringLiteral("raw_string_literal"))) {
                    QString routePath = nodeText(first, source);
                    routePath = routePath.mid(1, routePath.size() - 2);
                    // net/http 1.22 patterns: "GET /x"
                    QString verb = method.startsWith(QStringLiteral("Handle")) ? QStringLiteral("ANY") : method.toUpper();
                    static const QRegularExpression verbPrefix(QStringLiteral(R"(^([A-Z]+)\s+(/.*)$)"));
                    const auto prefixed = verbPrefix.match(routePath);
                    if (prefixed.hasMatch()) {
                        verb = prefixed.captured(1);
                        routePath = prefixed.captured(2);
                    }
                    if (routePath.startsWith(QLatin1Char('/'))) {
                        QVariantMap route = makeSourceContextItem(path, QStringLiteral("go"), nodeLine(node), nodeSnippet(node, source, 2),
                                                                  QStringLiteral("route"));
                        route.insert(QStringLiteral("owner"), nodeText(fieldNode(function, "operand"), source).left(40));
                        route.insert(QStringLiteral("method"), verb);
                        route.insert(QStringLiteral("path"), routePath);
                        route.insert(QStringLiteral("label"), verb + QStringLiteral(" ") + routePath);
                        routes.append(route);
                    }
                }
            }
        }
        const uint32_t c = ts_node_named_child_count(node);
        for (uint32_t k = 0; k < c && routes.size() < 200; ++k) {
            scanRoutes(ts_node_named_child(node, k));
        }
    };
    scanRoutes(root);

    result.insert(QStringLiteral("analysisHasAstErrors"), ts_node_has_error(root));
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), dependencies);
    result.insert(QStringLiteral("routes"), routes);
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("package %1: %2 top-level symbols").arg(packageName).arg(symbols.size()));
    return result;
}


// ---------------------------------------------------------------------------
// C / C++ from the syntax tree (tree-sitter-cpp, used for C as well)
// ---------------------------------------------------------------------------

namespace {

struct CppDeclaratorInfo
{
    TSNode function = TSNode{}; // function_declarator, if any
    QString suffix; // pointer / reference markers collected on the way down
    TSNode nameNode = TSNode{};
};

CppDeclaratorInfo cppUnwrapDeclarator(TSNode node, const QByteArray &source)
{
    CppDeclaratorInfo info;
    for (int guard = 0; guard < 12 && !ts_node_is_null(node); ++guard) {
        const QString type = tsType(node);
        if (type == QStringLiteral("function_declarator")) {
            info.function = node;
            info.nameNode = fieldNode(node, "declarator");
            return info;
        }
        if (type == QStringLiteral("pointer_declarator")) {
            info.suffix += QLatin1Char('*');
        } else if (type == QStringLiteral("reference_declarator")) {
            info.suffix += nodeText(node, source).trimmed().startsWith(QStringLiteral("&&")) ? QStringLiteral("&&") : QStringLiteral("&");
        }
        TSNode next = fieldNode(node, "declarator");
        if (ts_node_is_null(next) && ts_node_named_child_count(node) > 0) {
            next = ts_node_named_child(node, ts_node_named_child_count(node) - 1);
            if (type != QStringLiteral("pointer_declarator") && type != QStringLiteral("reference_declarator")
                && type != QStringLiteral("parenthesized_declarator")) {
                info.nameNode = node;
                return info;
            }
        }
        if (ts_node_is_null(next)) {
            info.nameNode = node;
            return info;
        }
        node = next;
    }
    info.nameNode = node;
    return info;
}

QString cppDeclaratorName(TSNode node, const QByteArray &source)
{
    // Innermost identifier of a (parameter / field) declarator.
    for (int guard = 0; guard < 12 && !ts_node_is_null(node); ++guard) {
        const QString type = tsType(node);
        if (type == QStringLiteral("identifier") || type == QStringLiteral("field_identifier")
            || type == QStringLiteral("type_identifier")) {
            return nodeText(node, source);
        }
        TSNode next = fieldNode(node, "declarator");
        if (ts_node_is_null(next)) {
            if (ts_node_named_child_count(node) == 0) {
                return {};
            }
            next = ts_node_named_child(node, ts_node_named_child_count(node) - 1);
        }
        node = next;
    }
    return {};
}

} // namespace

QVariantMap SymbolParser::parseCppTreeSitter(const QString &path, const QString &text) const
{
    const QByteArray source = text.toUtf8();
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("cpp"));
    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, languageForName(QStringLiteral("cpp")))) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return result;
    }
    TSTree *tree = parseAnalysedSource(parser, source);
    TSNode root = ts_tree_root_node(tree);

    QVariantList symbols;
    QVariantList dependencies;
    QHash<uint32_t, QString> ownerKeyByStart;

    auto parametersOf = [&](TSNode list) {
        QVariantList parameters;
        const uint32_t count = ts_node_named_child_count(list);
        for (uint32_t index = 0; index < count; ++index) {
            TSNode param = ts_node_named_child(list, index);
            const QString paramType = tsType(param);
            if (paramType == QStringLiteral("variadic_parameter")) {
                parameters.append(makeSignatureParameter(QStringLiteral("..."), QString()));
                continue;
            }
            if (paramType != QStringLiteral("parameter_declaration") && paramType != QStringLiteral("optional_parameter_declaration")
                && paramType != QStringLiteral("variadic_parameter_declaration")) {
                continue;
            }
            QString type = nodeText(fieldNode(param, "type"), source).simplified();
            // const / volatile qualifiers before the type
            const uint32_t childCount = ts_node_named_child_count(param);
            for (uint32_t k = 0; k < childCount; ++k) {
                TSNode child = ts_node_named_child(param, k);
                if (tsType(child) == QStringLiteral("type_qualifier") && ts_node_start_byte(child) < ts_node_start_byte(fieldNode(param, "type"))) {
                    type = nodeText(child, source) + QLatin1Char(' ') + type;
                }
            }
            TSNode declarator = fieldNode(param, "declarator");
            const CppDeclaratorInfo info = cppUnwrapDeclarator(declarator, source);
            const QString name = cppDeclaratorName(declarator, source);
            if (type == QStringLiteral("void") && name.isEmpty() && info.suffix.isEmpty()) {
                continue; // f(void)
            }
            QVariantMap parameter = makeSignatureParameter(name.isEmpty() ? QStringLiteral("_") : name, type + info.suffix);
            const QString defaultValue = nodeText(fieldNode(param, "default_value"), source).simplified();
            if (!defaultValue.isEmpty()) {
                parameter.insert(QStringLiteral("default"), defaultValue);
            }
            parameters.append(parameter);
        }
        return parameters;
    };

    auto makeFunction = [&](TSNode node, TSNode typeNode, const CppDeclaratorInfo &info, const QString &kindHint,
                            const QString &detail, bool isDefinition) {
        QString name = nodeText(info.nameNode, source).simplified();
        QString owner;
        const QString nameType = tsType(info.nameNode);
        if (nameType == QStringLiteral("qualified_identifier")) {
            owner = nodeText(fieldNode(info.nameNode, "scope"), source).simplified();
            name = nodeText(fieldNode(info.nameNode, "name"), source).simplified();
            // nested qualifiers: A::B::c -> scope "A", name "B::c"
            if (name.contains(QStringLiteral("::"))) {
                owner = owner + QStringLiteral("::") + name.section(QStringLiteral("::"), 0, -2);
                name = name.section(QStringLiteral("::"), -1);
            }
        }
        const QString ownerBase = owner.section(QStringLiteral("::"), -1).section(QLatin1Char('<'), 0, 0);
        QString kind = kindHint;
        const bool destructor = name.startsWith(QLatin1Char('~'));
        const bool constructor = !ownerBase.isEmpty() && name == ownerBase;
        if (destructor) {
            kind = QStringLiteral("destructor");
        } else if (constructor || (kindHint == QStringLiteral("method") && typeNode.id == nullptr && !name.startsWith(QStringLiteral("operator")))) {
            kind = QStringLiteral("constructor");
        } else if (!owner.isEmpty() && kind == QStringLiteral("function")) {
            kind = QStringLiteral("method");
        }
        QVariantMap symbol = makeSymbol(kind, name, nodeLine(node), detail, {}, nodeSnippet(node, source));
        symbol.insert(QStringLiteral("endLine"), static_cast<int>(ts_node_end_point(node).row) + 1);
        symbol.insert(QStringLiteral("parameters"), parametersOf(fieldNode(info.function, "parameters")));
        QString returnType = nodeText(typeNode, source).simplified();
        if (!returnType.isEmpty()) {
            // const / volatile written before the type belong to it.
            QStringList qualifiers;
            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t k = 0; k < childCount; ++k) {
                TSNode child = ts_node_named_child(node, k);
                if (tsType(child) == QStringLiteral("type_qualifier") && ts_node_start_byte(child) < ts_node_start_byte(typeNode)) {
                    qualifiers.append(nodeText(child, source));
                }
            }
            if (!qualifiers.isEmpty()) {
                returnType = qualifiers.join(QLatin1Char(' ')) + QLatin1Char(' ') + returnType;
            }
            returnType += info.suffix;
        }
        QVariantList returns;
        returns.append(QVariantMap{{QStringLiteral("text"),
                                    (kind == QStringLiteral("constructor") || kind == QStringLiteral("destructor") || returnType.isEmpty()
                                     || returnType == QStringLiteral("void"))
                                        ? QStringLiteral("none")
                                        : returnType}});
        symbol.insert(QStringLiteral("returns"), returns);
        symbol.insert(QStringLiteral("signatureSource"), QStringLiteral("ast"));
        symbol.insert(QStringLiteral("_owner"), owner);
        symbol.insert(QStringLiteral("_definition"), isDefinition);
        return symbol;
    };

    std::function<QVariantMap(TSNode, const QString &)> parseRecord;
    // Add or replace (a definition replaces an earlier prototype of the same name).
    auto addTopLevel = [&](QVariantMap symbol) {
        const QString owner = symbol.take(QStringLiteral("_owner")).toString();
        const bool definition = symbol.take(QStringLiteral("_definition")).toBool();
        if (!owner.isEmpty()) {
            // Out-of-line member definition: attach to its class (replace the in-class declaration).
            const QString ownerBase = owner.section(QStringLiteral("::"), -1).section(QLatin1Char('<'), 0, 0);
            for (int index = 0; index < symbols.size(); ++index) {
                QVariantMap candidate = symbols.at(index).toMap();
                if (candidate.value(QStringLiteral("name")).toString() != ownerBase) {
                    continue;
                }
                QVariantList members = candidate.value(QStringLiteral("members")).toList();
                bool replaced = false;
                for (int m = 0; m < members.size(); ++m) {
                    const QVariantMap member = members.at(m).toMap();
                    if (member.value(QStringLiteral("name")).toString() == symbol.value(QStringLiteral("name")).toString()
                        && member.value(QStringLiteral("detail")).toString().contains(QStringLiteral("declaration"))) {
                        QVariantMap merged = symbol;
                        merged.insert(QStringLiteral("detail"), member.value(QStringLiteral("detail")).toString().remove(QStringLiteral("declaration")).trimmed());
                        members[m] = merged;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    members.append(symbol);
                }
                candidate.insert(QStringLiteral("members"), members);
                symbols[index] = candidate;
                return;
            }
            // The scope may be a class declared in a header or a namespace:
            // group under a neutral scope symbol.
            attachToOwner(symbols, ownerBase, symbol, QStringLiteral("scope"),
                          QStringLiteral("%1:: definitions; declared elsewhere").arg(owner));
            return;
        }
        for (int index = 0; index < symbols.size(); ++index) {
            const QVariantMap existing = symbols.at(index).toMap();
            if (existing.value(QStringLiteral("name")).toString() == symbol.value(QStringLiteral("name")).toString()
                && isCallableSymbolKind(existing.value(QStringLiteral("kind")).toString())
                && isCallableSymbolKind(symbol.value(QStringLiteral("kind")).toString())) {
                if (definition && existing.value(QStringLiteral("detail")).toString() == QStringLiteral("declaration")) {
                    symbols[index] = symbol;
                }
                return;
            }
        }
        symbols.append(symbol);
    };

    parseRecord = [&](TSNode specifier, const QString &nameOverride) -> QVariantMap {
        const QString specType = tsType(specifier);
        QString kind = specType == QStringLiteral("struct_specifier") ? QStringLiteral("struct")
                     : specType == QStringLiteral("union_specifier") ? QStringLiteral("union")
                                                                     : QStringLiteral("class");
        QString name = nameOverride.isEmpty() ? nodeText(fieldNode(specifier, "name"), source).simplified() : nameOverride;
        if (name.isEmpty()) {
            name = QStringLiteral("(anonymous %1)").arg(kind);
        }
        QString detail;
        for (uint32_t k = 0; k < ts_node_named_child_count(specifier); ++k) {
            TSNode child = ts_node_named_child(specifier, k);
            if (tsType(child) == QStringLiteral("base_class_clause")) {
                detail = nodeText(child, source).simplified();
                if (detail.startsWith(QLatin1Char(':'))) {
                    detail = QStringLiteral("inherits ") + detail.mid(1).trimmed();
                }
            }
        }
        QVariantList members;
        QString access = kind == QStringLiteral("class") ? QStringLiteral("private") : QStringLiteral("public");
        TSNode body = fieldNode(specifier, "body");
        const uint32_t count = ts_node_named_child_count(body);
        for (uint32_t index = 0; index < count; ++index) {
            TSNode member = ts_node_named_child(body, index);
            const QString memberType = tsType(member);
            if (memberType == QStringLiteral("access_specifier")) {
                access = nodeText(member, source).simplified();
                continue;
            }
            TSNode inner = member;
            if (memberType == QStringLiteral("template_declaration")) {
                for (uint32_t k = 0; k < ts_node_named_child_count(member); ++k) {
                    const QString t = tsType(ts_node_named_child(member, k));
                    if (t == QStringLiteral("function_definition") || t == QStringLiteral("declaration") || t == QStringLiteral("field_declaration")) {
                        inner = ts_node_named_child(member, k);
                    }
                }
            }
            const QString innerType = tsType(inner);
            if (innerType == QStringLiteral("function_definition")) {
                const CppDeclaratorInfo info = cppUnwrapDeclarator(fieldNode(inner, "declarator"), source);
                if (!ts_node_is_null(info.function)) {
                    QVariantMap method = makeFunction(inner, fieldNode(inner, "type"), info, QStringLiteral("method"), access, true);
                    method.remove(QStringLiteral("_owner"));
                    method.remove(QStringLiteral("_definition"));
                    if (nodeText(info.nameNode, source) == name) {
                        method.insert(QStringLiteral("kind"), QStringLiteral("constructor"));
                    }
                    ownerKeyByStart.insert(ts_node_start_byte(inner), symbolKey(method));
                    members.append(method);
                }
            } else if (innerType == QStringLiteral("field_declaration") || innerType == QStringLiteral("declaration")) {
                TSNode typeNode = fieldNode(inner, "type");
                const QString typeType = tsType(typeNode);
                if ((typeType == QStringLiteral("class_specifier") || typeType == QStringLiteral("struct_specifier")
                     || typeType == QStringLiteral("union_specifier")) && !ts_node_is_null(fieldNode(typeNode, "body"))) {
                    members.append(parseRecord(typeNode, QString()));
                    continue;
                }
                if (typeType == QStringLiteral("enum_specifier") && !ts_node_is_null(fieldNode(typeNode, "body"))) {
                    members.append(makeSymbol(QStringLiteral("enum"), nodeText(fieldNode(typeNode, "name"), source), nodeLine(typeNode),
                                              access, {}, nodeSnippet(typeNode, source)));
                    continue;
                }
                const QList<TSNode> declarators = fieldNodes(inner, "declarator");
                for (const TSNode &declarator : declarators) {
                    const CppDeclaratorInfo info = cppUnwrapDeclarator(declarator, source);
                    if (!ts_node_is_null(info.function) && tsType(info.nameNode) == QStringLiteral("parenthesized_declarator")) {
                        // (*callback)(args): a function-pointer field, not a method.
                        members.append(makeSymbol(QStringLiteral("field"), cppDeclaratorName(info.nameNode, source), nodeLine(inner),
                                                  QStringLiteral("%1 function pointer").arg(access), {}, nodeSnippet(inner, source, 2)));
                        continue;
                    }
                    if (!ts_node_is_null(info.function)) {
                        QVariantMap method = makeFunction(inner, typeNode, info, QStringLiteral("method"),
                                                          access + QStringLiteral(" declaration"), false);
                        method.remove(QStringLiteral("_owner"));
                        method.remove(QStringLiteral("_definition"));
                        if (nodeText(info.nameNode, source) == name) {
                            method.insert(QStringLiteral("kind"), QStringLiteral("constructor"));
                        }
                        members.append(method);
                    } else {
                        const QString fieldName = cppDeclaratorName(declarator, source);
                        if (!fieldName.isEmpty()) {
                            members.append(makeSymbol(QStringLiteral("field"), fieldName, nodeLine(inner),
                                                      QStringLiteral("%1 %2%3").arg(access, nodeText(typeNode, source).simplified(), info.suffix),
                                                      {}, nodeSnippet(inner, source, 2)));
                        }
                    }
                }
            }
        }
        QVariantMap symbol = makeSymbol(kind, name, nodeLine(specifier), detail, members, nodeSnippet(specifier, source));
        symbol.insert(QStringLiteral("endLine"), static_cast<int>(ts_node_end_point(specifier).row) + 1);
        return symbol;
    };

    std::function<void(TSNode, const QString &)> processContainer = [&](TSNode container, const QString &scopeDetail) {
        const uint32_t count = ts_node_named_child_count(container);
        for (uint32_t index = 0; index < count; ++index) {
            TSNode node = ts_node_named_child(container, index);
            const QString type = tsType(node);
            if (type == QStringLiteral("preproc_include")) {
                TSNode pathNode = fieldNode(node, "path");
                QString target = nodeText(pathNode, source).trimmed();
                const bool system = target.startsWith(QLatin1Char('<'));
                target = target.mid(1, target.size() - 2);
                QVariantMap item = makeSourceContextItem(path, QStringLiteral("cpp"), nodeLine(node), nodeSnippet(node, source, 1),
                                                         QStringLiteral("include dependency"));
                item.insert(QStringLiteral("target"), target);
                item.insert(QStringLiteral("type"), system ? QStringLiteral("system include") : QStringLiteral("include"));
                item.insert(QStringLiteral("label"), target);
                QString resolved;
                if (!system) {
                    QDir dir = QFileInfo(path).dir();
                    for (int level = 0; level < 4; ++level) {
                        for (const QString &prefix : {QString(), QStringLiteral("include/"), QStringLiteral("src/")}) {
                            const QString candidate = QDir::cleanPath(dir.absoluteFilePath(prefix + target));
                            if (resolved.isEmpty() && QFileInfo::exists(candidate)) {
                                resolved = candidate;
                            }
                        }
                        if (!resolved.isEmpty() || !dir.cdUp()) {
                            break;
                        }
                    }
                }
                item.insert(QStringLiteral("path"), resolved);
                item.insert(QStringLiteral("exists"), system || !resolved.isEmpty());
                dependencies.append(item);
            } else if (type == QStringLiteral("namespace_definition")) {
                const QString name = nodeText(fieldNode(node, "name"), source).simplified();
                processContainer(fieldNode(node, "body"), name.isEmpty() ? QStringLiteral("in anonymous namespace")
                                                                          : QStringLiteral("in namespace %1").arg(name));
            } else if (type == QStringLiteral("linkage_specification")) {
                TSNode body = fieldNode(node, "body");
                if (tsType(body) == QStringLiteral("declaration_list")) {
                    processContainer(body, scopeDetail);
                } else {
                    // single declaration: process via a synthetic container walk
                    const uint32_t c = ts_node_named_child_count(node);
                    for (uint32_t k = 0; k < c; ++k) {
                        TSNode child = ts_node_named_child(node, k);
                        if (tsType(child) != QStringLiteral("string_literal")) {
                            // reuse the loop body by recursing on the parent with only this child
                            processContainer(child, scopeDetail);
                        }
                    }
                }
            } else if (type.startsWith(QStringLiteral("preproc_if")) || type == QStringLiteral("preproc_else")
                       || type == QStringLiteral("preproc_elif") || type == QStringLiteral("template_declaration")
                       || type == QStringLiteral("declaration_list")) {
                processContainer(node, scopeDetail);
            } else if (type == QStringLiteral("function_definition")) {
                const CppDeclaratorInfo info = cppUnwrapDeclarator(fieldNode(node, "declarator"), source);
                if (ts_node_is_null(info.function)) {
                    continue;
                }
                QVariantMap symbol = makeFunction(node, fieldNode(node, "type"), info, QStringLiteral("function"), scopeDetail, true);
                ownerKeyByStart.insert(ts_node_start_byte(node), symbolKey(symbol));
                addTopLevel(symbol);
            } else if (type == QStringLiteral("declaration") || type == QStringLiteral("type_definition")
                       || type == QStringLiteral("class_specifier") || type == QStringLiteral("struct_specifier")
                       || type == QStringLiteral("union_specifier") || type == QStringLiteral("enum_specifier")) {
                TSNode typeNode = (type == QStringLiteral("declaration") || type == QStringLiteral("type_definition"))
                    ? fieldNode(node, "type") : node;
                const QString typeType = tsType(typeNode);
                const bool hasBody = !ts_node_is_null(fieldNode(typeNode, "body"));
                QString typedefName;
                if (type == QStringLiteral("type_definition")) {
                    typedefName = cppDeclaratorName(fieldNode(node, "declarator"), source);
                }
                if ((typeType == QStringLiteral("class_specifier") || typeType == QStringLiteral("struct_specifier")
                     || typeType == QStringLiteral("union_specifier")) && hasBody) {
                    QVariantMap record = parseRecord(typeNode, nodeText(fieldNode(typeNode, "name"), source).isEmpty() ? typedefName : QString());
                    if (!scopeDetail.isEmpty()) {
                        record.insert(QStringLiteral("detail"), record.value(QStringLiteral("detail")).toString().isEmpty()
                                                                    ? scopeDetail
                                                                    : record.value(QStringLiteral("detail")).toString() + QStringLiteral(", ") + scopeDetail);
                    }
                    symbols.append(record);
                    continue;
                }
                if (typeType == QStringLiteral("enum_specifier") && hasBody) {
                    QVariantList enumerators;
                    TSNode body = fieldNode(typeNode, "body");
                    for (uint32_t k = 0; k < ts_node_named_child_count(body); ++k) {
                        TSNode e = ts_node_named_child(body, k);
                        if (tsType(e) == QStringLiteral("enumerator")) {
                            enumerators.append(makeSymbol(QStringLiteral("enum member"), nodeText(fieldNode(e, "name"), source), nodeLine(e),
                                                          nodeText(fieldNode(e, "value"), source).simplified(), {}, nodeSnippet(e, source, 1)));
                        }
                    }
                    QString enumName = nodeText(fieldNode(typeNode, "name"), source).simplified();
                    if (enumName.isEmpty()) {
                        enumName = typedefName.isEmpty() ? QStringLiteral("(anonymous enum)") : typedefName;
                    }
                    symbols.append(makeSymbol(QStringLiteral("enum"), enumName, nodeLine(node), scopeDetail, enumerators, nodeSnippet(node, source)));
                    continue;
                }
                if (type == QStringLiteral("type_definition")) {
                    if (!typedefName.isEmpty()) {
                        symbols.append(makeSymbol(QStringLiteral("typedef"), typedefName, nodeLine(node),
                                                  nodeText(typeNode, source).simplified().left(60), {}, nodeSnippet(node, source, 2)));
                    }
                    continue;
                }
                if (type != QStringLiteral("declaration")) {
                    continue;
                }
                for (const TSNode &declarator : fieldNodes(node, "declarator")) {
                    const CppDeclaratorInfo info = cppUnwrapDeclarator(declarator, source);
                    if (!ts_node_is_null(info.function)) {
                        QVariantMap symbol = makeFunction(node, typeNode, info, QStringLiteral("function"),
                                                          scopeDetail.isEmpty() ? QStringLiteral("declaration") : scopeDetail, false);
                        if (scopeDetail.isEmpty()) {
                            symbol.insert(QStringLiteral("detail"), QStringLiteral("declaration"));
                        }
                        addTopLevel(symbol);
                    } else if (container.id == root.id || !scopeDetail.isEmpty()) {
                        const QString name = cppDeclaratorName(declarator, source);
                        if (!name.isEmpty()) {
                            symbols.append(makeSymbol(QStringLiteral("variable"), name, nodeLine(node),
                                                      nodeText(typeNode, source).simplified() + info.suffix, {}, nodeSnippet(node, source, 2)));
                        }
                    }
                }
            }
        }
    };
    processContainer(root, QString());

    auto calleeName = [&](TSNode call) -> QString {
        TSNode function = fieldNode(call, "function");
        const QString type = tsType(function);
        if (type == QStringLiteral("identifier")) {
            return nodeText(function, source);
        }
        if (type == QStringLiteral("field_expression")) {
            return nodeText(fieldNode(function, "field"), source);
        }
        if (type == QStringLiteral("qualified_identifier")) {
            return nodeText(fieldNode(function, "name"), source).section(QStringLiteral("::"), -1);
        }
        if (type == QStringLiteral("template_function")) {
            return nodeText(fieldNode(function, "name"), source);
        }
        return {};
    };
    symbols = applyPositionalCallRelations(symbols, root, source, ownerKeyByStart, {QStringLiteral("call_expression")}, calleeName);

    result.insert(QStringLiteral("analysisHasAstErrors"), ts_node_has_error(root));
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), dependencies);
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}


// ---------------------------------------------------------------------------
// Shell (bash / sh / zsh): structural parser
//
// Functions (`name() {`, `function name {`, `function name() {`) are found at
// statement starts and extended to their matching brace, skipping quotes,
// comments and here-documents. Parameters are read from `local x="$1"`
// style assignments (else the positional parameters used), calls are
// function names in command position, and `source` / `.` are dependencies.
// ---------------------------------------------------------------------------

namespace {

// Blank comments, quoted string text and here-doc bodies (newlines kept),
// while keeping $( ... ) / `...` command substitutions visible even inside
// double quotes - calls live there, and their own quotes nest.
class ShellNoiseBlanker
{
public:
    explicit ShellNoiseBlanker(const QString &text) : m_out(text), m_size(text.size()) {}

    QString run()
    {
        scanCode(0, QChar());
        return m_out;
    }

private:
    void blank(int from, int to)
    {
        for (int i = from; i < to && i < m_size; ++i) {
            if (m_out.at(i) != QLatin1Char('\n')) {
                m_out[i] = QLatin1Char(' ');
            }
        }
    }

    // An unterminated quote must not swallow later functions: a line that is
    // clearly a function header at column 0 ends any string still open.
    bool functionHeaderStartsAt(int lineStart) const
    {
        static const QRegularExpression header(QStringLiteral(R"(^(?:function\s+[A-Za-z_][\w:.-]*|[A-Za-z_][\w:.-]*\s*\(\s*\))\s*(?:\{|$))"));
        const int end = m_out.indexOf(QLatin1Char('\n'), lineStart);
        return header.match(m_out.mid(lineStart, (end < 0 ? m_size : end) - lineStart)).hasMatch();
    }

    // Scan code until `terminator` (')' for $( ), '`' for backticks, none for
    // top level). Returns the index just past the terminator.
    int scanCode(int index, QChar terminator)
    {
        int parenDepth = 0;
        while (index < m_size) {
            const QChar ch = m_out.at(index);
            if (ch == QLatin1Char('\\')) {
                index += 2;
                continue;
            }
            if (!terminator.isNull() && ch == terminator && (terminator != QLatin1Char(')') || parenDepth == 0)) {
                return index + 1;
            }
            if (ch == QLatin1Char('(')) {
                ++parenDepth;
            } else if (ch == QLatin1Char(')') && parenDepth > 0) {
                --parenDepth;
            } else if (ch == QLatin1Char('#') && (index == 0 || m_out.at(index - 1).isSpace() || m_out.at(index - 1) == QLatin1Char(';'))) {
                int end = m_out.indexOf(QLatin1Char('\n'), index);
                if (end < 0) end = m_size;
                blank(index, end);
                index = end;
                continue;
            } else if (ch == QLatin1Char('\'')) {
                const bool ansi = index > 0 && m_out.at(index - 1) == QLatin1Char('$');
                int end = index + 1;
                while (end < m_size && m_out.at(end) != QLatin1Char('\'')) {
                    if (ansi && m_out.at(end) == QLatin1Char('\\')) ++end;
                    if (m_out.at(end) == QLatin1Char('\n') && functionHeaderStartsAt(end + 1)) {
                        break;
                    }
                    ++end;
                }
                blank(index + 1, end);
                index = end + 1;
                continue;
            } else if (ch == QLatin1Char('"')) {
                index = scanDouble(index + 1);
                continue;
            } else if (ch == QLatin1Char('`')) {
                index = scanCode(index + 1, QLatin1Char('`'));
                continue;
            } else if (ch == QLatin1Char('$') && index + 1 < m_size && m_out.at(index + 1) == QLatin1Char('(')) {
                index = scanCode(index + 2, QLatin1Char(')'));
                continue;
            } else if (ch == QLatin1Char('<') && index + 1 < m_size && m_out.at(index + 1) == QLatin1Char('<')) {
                static const QRegularExpression heredoc(QStringLiteral(R"(<<-?\s*['"]?([A-Za-z_]\w*)['"]?)"));
                const auto match = heredoc.match(m_out, index);
                if (match.hasMatch() && match.capturedStart(0) == index) {
                    const int bodyStart = m_out.indexOf(QLatin1Char('\n'), index);
                    if (bodyStart >= 0) {
                        const QRegularExpression endMarker(QStringLiteral("^\\s*%1\\s*$").arg(QRegularExpression::escape(match.captured(1))),
                                                           QRegularExpression::MultilineOption);
                        const auto endMatch = endMarker.match(m_out, bodyStart + 1);
                        const int end = endMatch.hasMatch() ? endMatch.capturedEnd(0) : m_size;
                        blank(bodyStart + 1, end);
                        index = end;
                        continue;
                    }
                }
            }
            ++index;
        }
        return m_size;
    }

    // Inside "...": blank literal text, recurse into substitutions.
    int scanDouble(int index)
    {
        int literalStart = index;
        while (index < m_size) {
            const QChar ch = m_out.at(index);
            if (ch == QLatin1Char('\\')) {
                index += 2;
                continue;
            }
            if (ch == QLatin1Char('"')) {
                blank(literalStart, index);
                return index + 1;
            }
            if (ch == QLatin1Char('\n') && functionHeaderStartsAt(index + 1)) {
                blank(literalStart, index);
                return index + 1; // unterminated string: stop before the next function
            }
            if (ch == QLatin1Char('$') && index + 1 < m_size && m_out.at(index + 1) == QLatin1Char('(')) {
                blank(literalStart, index);
                index = scanCode(index + 2, QLatin1Char(')'));
                literalStart = index;
                continue;
            }
            if (ch == QLatin1Char('`')) {
                blank(literalStart, index);
                index = scanCode(index + 1, QLatin1Char('`'));
                literalStart = index;
                continue;
            }
            ++index;
        }
        blank(literalStart, m_size);
        return m_size;
    }

    QString m_out;
    int m_size;
};

QString blankShellNoise(const QString &text)
{
    return ShellNoiseBlanker(text).run();
}

} // namespace

QVariantMap SymbolParser::parseShell(const QString &path, const QString &text) const
{
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("shell"));
    const QString clean = blankShellNoise(text);
    const QStringList physicalLines = text.split(QLatin1Char('\n'));
    static const QRegularExpression functionPattern(
        QStringLiteral(R"(^[ \t]*(?:function\s+([A-Za-z_][\w:.-]*)\s*(?:\(\s*\))?|([A-Za-z_][\w:.-]*)\s*\(\s*\))\s*(?:\n\s*)?\{)"),
        QRegularExpression::MultilineOption);

    struct ShellFunction
    {
        QVariantMap symbol;
        int bodyStart = 0;
        int bodyEnd = 0;
    };
    QList<ShellFunction> functions;
    auto it = functionPattern.globalMatch(clean);
    int resumeAt = 0;
    while (it.hasNext()) {
        const auto match = it.next();
        if (match.capturedStart(0) < resumeAt) {
            continue; // nested function inside a previous body
        }
        const QString name = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
        const int open = match.capturedEnd(0) - 1;
        int depth = 0;
        int close = clean.size();
        for (int i = open; i < clean.size(); ++i) {
            const QChar ch = clean.at(i);
            if (ch == QLatin1Char('{')) {
                ++depth;
            } else if (ch == QLatin1Char('}')) {
                if (--depth == 0) {
                    close = i;
                    break;
                }
            }
        }
        // Damage limit: a body never extends past the next function header at
        // column 0 (an unbalanced brace or an unterminated quote upstream can
        // otherwise make it swallow the rest of the file).
        static const QRegularExpression columnZeroHeader(
            QStringLiteral(R"(^(?:function\s+[A-Za-z_][\w:.-]*\s*(?:\(\s*\))?|[A-Za-z_][\w:.-]*\s*\(\s*\))\s*(?:\n\s*)?\{)"),
            QRegularExpression::MultilineOption);
        const auto nextHeader = columnZeroHeader.match(text, match.capturedEnd(0));
        if (nextHeader.hasMatch() && nextHeader.capturedStart(0) < close) {
            close = nextHeader.capturedStart(0) - 1;
        }
        const int line = lineNumberAtOffset(clean, match.capturedStart(0));
        const int endLine = lineNumberAtOffset(clean, qMax(match.capturedStart(0), close));
        QStringList snippetLines;
        for (int l = line; l <= endLine && l <= physicalLines.size() && snippetLines.size() < 10; ++l) {
            snippetLines.append(physicalLines.at(l - 1));
        }
        if (endLine - line + 1 > 10) {
            snippetLines.append(QStringLiteral("..."));
        }
        QVariantMap symbol = makeSymbol(QStringLiteral("function"), name, line, QString(), {}, snippetLines.join(QLatin1Char('\n')));
        symbol.insert(QStringLiteral("endLine"), endLine);

        // Parameters: local/declare x="$1" names, else the positional parameters used.
        // Original text (parameter expansions live inside double quotes), with
        // single-quoted spans removed ('{print $1}' is awk, not a parameter).
        QString body = text.mid(open, close - open);
        static const QRegularExpression singleQuoted(QStringLiteral(R"('[^'\n]*')"));
        body.replace(singleQuoted, QStringLiteral("''"));
        QMap<int, QString> named;
        static const QRegularExpression localParam(QStringLiteral(R"(\b(?:local|declare|typeset|readonly)\s+(?:-\w+\s+)*([A-Za-z_]\w*)=["']?\$\{?([1-9])\b)"));
        auto localIt = localParam.globalMatch(body);
        while (localIt.hasNext()) {
            const auto localMatch = localIt.next();
            named.insert(localMatch.captured(2).toInt(), localMatch.captured(1));
        }
        static const QRegularExpression positional(QStringLiteral(R"(\$\{?([1-9])\b)"));
        int highest = 0;
        auto positionalIt = positional.globalMatch(body);
        while (positionalIt.hasNext()) {
            highest = qMax(highest, positionalIt.next().captured(1).toInt());
        }
        QVariantList parameters;
        for (int n = 1; n <= highest; ++n) {
            parameters.append(makeSignatureParameter(named.value(n, QStringLiteral("$%1").arg(n)),
                                                     named.contains(n) ? QStringLiteral("$%1").arg(n) : QString()));
        }
        if (body.contains(QStringLiteral("$@")) || body.contains(QStringLiteral("${@")) || body.contains(QStringLiteral("$*"))) {
            parameters.append(makeSignatureParameter(QStringLiteral("$@"), QStringLiteral("remaining arguments")));
        }
        symbol.insert(QStringLiteral("parameters"), parameters);
        QStringList outputs{QStringLiteral("exit status")};
        static const QRegularExpression printsOutput(QStringLiteral(R"(\b(?:echo|printf|cat)\b)"));
        if (printsOutput.match(clean.mid(open, close - open)).hasMatch()) {
            outputs.append(QStringLiteral("stdout"));
        }
        symbol.insert(QStringLiteral("returns"), QVariantList{QVariantMap{{QStringLiteral("text"), outputs.join(QStringLiteral(" + "))}}});
        symbol.insert(QStringLiteral("signatureSource"), QStringLiteral("parser"));
        functions.append({symbol, open + 1, close});
        resumeAt = close;
    }

    // Calls: function names in command position.
    QHash<QString, int> byName;
    for (int i = 0; i < functions.size(); ++i) {
        byName.insert(functions.at(i).symbol.value(QStringLiteral("name")).toString(), i);
    }
    QHash<QString, QVariantList> callsByKey;
    QHash<QString, QVariantList> calledByByKey;
    static const QRegularExpression commandWord(QStringLiteral(R"((?:^|[;&|({`]|\$\(|\bthen\b|\bdo\b|\belse\b|&&|\|\|)\s*(?:command\s+|exec\s+|time\s+)?([A-Za-z_][\w:.-]*))"),
                                                QRegularExpression::MultilineOption);
    for (int i = 0; i < functions.size(); ++i) {
        const QString body = clean.mid(functions.at(i).bodyStart, functions.at(i).bodyEnd - functions.at(i).bodyStart);
        auto callIt = commandWord.globalMatch(body);
        while (callIt.hasNext()) {
            const QString name = callIt.next().captured(1);
            const int target = byName.value(name, -1);
            if (target < 0 || target == i) {
                continue;
            }
            const QVariantMap &source = functions.at(i).symbol;
            const QVariantMap &callee = functions.at(target).symbol;
            appendUniqueRelation(callsByKey, symbolKey(source), relationFromSymbol(callee), QStringLiteral("calls"));
            appendUniqueRelation(calledByByKey, symbolKey(callee), relationFromSymbol(source), QStringLiteral("called by"));
        }
    }
    QVariantList symbols;
    for (const ShellFunction &function : std::as_const(functions)) {
        symbols.append(function.symbol);
    }
    // Exported / readonly configuration at top level.
    static const QRegularExpression exported(QStringLiteral(R"(^(?:export|readonly|declare\s+-[xr]+)\s+([A-Za-z_]\w*)=)"),
                                             QRegularExpression::MultilineOption);
    auto exportIt = exported.globalMatch(clean);
    while (exportIt.hasNext()) {
        const auto match = exportIt.next();
        bool insideFunction = false;
        for (const ShellFunction &function : std::as_const(functions)) {
            if (match.capturedStart(0) > function.bodyStart && match.capturedStart(0) < function.bodyEnd) {
                insideFunction = true;
                break;
            }
        }
        if (!insideFunction) {
            const int line = lineNumberAtOffset(clean, match.capturedStart(0));
            symbols.append(makeSymbol(QStringLiteral("variable"), match.captured(1), line, QStringLiteral("exported"), {},
                                      physicalLines.value(line - 1)));
        }
    }
    symbols = applyRelationsToSymbols(symbols, callsByKey, calledByByKey);
    std::stable_sort(symbols.begin(), symbols.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("line")).toInt() < b.toMap().value(QStringLiteral("line")).toInt();
    });

    // Dependencies: source / . files (literal paths; $DIR-relative resolved by basename).
    QVariantList dependencies;
    static const QRegularExpression sourcePattern(QStringLiteral(R"(^[ \t]*(?:source|\.)[ \t]+([^;&|#\n]+))"),
                                                  QRegularExpression::MultilineOption);
    auto sourceIt = sourcePattern.globalMatch(text);
    QSet<QString> seen;
    while (sourceIt.hasNext()) {
        const auto match = sourceIt.next();
        QString target = match.captured(1).trimmed();
        if (target.startsWith(QLatin1Char('-'))) {
            continue;
        }
        // Drop directory-prefix idioms: $(dirname "$0"), ${BASH_SOURCE%/*}, $DIR, ...
        QString literal = target;
        static const QRegularExpression substitution(QStringLiteral(R"(\$\((?:[^()]|\([^()]*\))*\)|\$\{[^}]*\}|\$[A-Za-z_]\w*)"));
        literal.remove(substitution);
        literal.remove(QLatin1Char('"'));
        literal.remove(QLatin1Char('\''));
        literal = literal.trimmed();
        while (literal.startsWith(QLatin1Char('/')) && target.contains(QLatin1Char('$'))) {
            literal = literal.mid(1);
        }
        if (literal.isEmpty() || seen.contains(literal)) {
            continue;
        }
        seen.insert(literal);
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        const QString resolved = literal.startsWith(QLatin1Char('/')) ? literal
                                                                      : QDir::cleanPath(QFileInfo(path).dir().absoluteFilePath(literal));
        QVariantMap item = makeSourceContextItem(path, QStringLiteral("shell"), line, physicalLines.value(line - 1).trimmed(),
                                                 QStringLiteral("source dependency"));
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), QStringLiteral("source"));
        item.insert(QStringLiteral("label"), QFileInfo(literal).fileName());
        item.insert(QStringLiteral("path"), QFileInfo::exists(resolved) ? resolved : QString());
        item.insert(QStringLiteral("exists"), QFileInfo::exists(resolved) || target.contains(QLatin1Char('$')));
        dependencies.append(item);
    }

    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), dependencies);
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 functions").arg(functions.size()));
    return result;
}

QVariantMap SymbolParser::parseSwiftTreeSitter(const QString &path, const QString &text) const
{
    QVariantList symbols;
    const QByteArray source = text.toUtf8();
    TSLanguage *language = languageForName(QStringLiteral("swift"));
    if (!language) {
        return makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("swift"));
    }

    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, language)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("swift"));
    }

    TSTree *tree = parseAnalysedSource(parser, source);
    TSNode root = ts_tree_root_node(tree);
    const bool hasAstErrors = ts_node_has_error(root);

    std::function<QVariantList(TSNode)> parseSwiftBodyMembers = [&](TSNode body) {
        QVariantList members;
        if (ts_node_is_null(body)) {
            return members;
        }

        const uint32_t count = ts_node_named_child_count(body);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(body, i);
            const QString type = tsType(child);
            if (type == QStringLiteral("class_declaration")
                || type == QStringLiteral("protocol_declaration")
                || type == QStringLiteral("function_declaration")
                || type == QStringLiteral("protocol_function_declaration")
                || type == QStringLiteral("init_declaration")
                || type == QStringLiteral("deinit_declaration")
                || type == QStringLiteral("property_declaration")
                || type == QStringLiteral("protocol_property_declaration")
                || type == QStringLiteral("typealias_declaration")
                || type == QStringLiteral("associatedtype_declaration")) {
                const QString kind = swiftDeclarationKind(child, source);
                const QString name = swiftDeclarationName(child, source);
                QVariantList childMembers;
                if (type == QStringLiteral("class_declaration") || type == QStringLiteral("protocol_declaration")) {
                    childMembers = parseSwiftBodyMembers(fieldNode(child, "body"));
                }
                members.append(makeSymbol(kind, name, nodeLine(child), QString(), childMembers, nodeSnippet(child, source)));
            }
        }
        return members;
    };

    const uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_named_child(root, i);
        const QString type = tsType(child);
        if (type == QStringLiteral("class_declaration")
            || type == QStringLiteral("protocol_declaration")
            || type == QStringLiteral("function_declaration")
            || type == QStringLiteral("property_declaration")
            || type == QStringLiteral("typealias_declaration")) {
            QVariantList members;
            if (type == QStringLiteral("class_declaration") || type == QStringLiteral("protocol_declaration")) {
                members = parseSwiftBodyMembers(fieldNode(child, "body"));
            }
            symbols.append(makeSymbol(swiftDeclarationKind(child, source),
                                      swiftDeclarationName(child, source),
                                      nodeLine(child),
                                      QString(),
                                      members,
                                      nodeSnippet(child, source)));
        }
    }

    if (!symbols.isEmpty()) {
        QHash<QString, QVariantMap> byKey;
        QHash<QString, QStringList> keysByName;
        QHash<QString, QVariantList> callsByKey;
        QHash<QString, QVariantList> calledByByKey;
        collectSymbolsByKey(symbols, byKey, keysByName);

        std::function<void(TSNode, const QString &)> visit = [&](TSNode node, const QString &currentKey) {
            QString activeKey = currentKey;
            const QString nodeKey = swiftCallableKeyForNode(node, source, byKey);
            if (!nodeKey.isEmpty()) {
                activeKey = nodeKey;
            }

            if (!activeKey.isEmpty() && tsType(node) == QStringLiteral("call_expression")) {
                const QString targetName = swiftCallTargetName(node, source);
                const QStringList candidateKeys = keysByName.value(targetName);
                if (!targetName.isEmpty() && !candidateKeys.isEmpty()) {
                    const QString targetKey = bestRelationTargetKey(candidateKeys, byKey);
                    if (targetKey != activeKey && byKey.contains(targetKey)) {
                        appendUniqueRelation(callsByKey, activeKey, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
                        appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(activeKey)), QStringLiteral("called by"));
                    }
                }
            }

            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t index = 0; index < childCount; ++index) {
                visit(ts_node_named_child(node, index), activeKey);
            }
        };

        visit(root, QString());
        symbols = applyRelationsToSymbols(symbols, callsByKey, calledByByKey);
    }

    symbols = applyPhpCallbackRelations(symbols);
    symbols = applySnippetCallRelations(symbols);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("swift"));
    result.insert(QStringLiteral("analysisHasAstErrors"), hasAstErrors);
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), extractDependencyLinks(path, text));
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parsePhpTreeSitter(const QString &path, const QString &text) const
{
    QVariantList symbols;
    const QByteArray source = text.toUtf8();
    TSLanguage *language = languageForName(QStringLiteral("php"));
    if (!language) {
        return makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("php"));
    }

    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, language)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("php"));
    }

    TSTree *tree = parseAnalysedSource(parser, source);
    TSNode root = ts_tree_root_node(tree);
    const bool hasAstErrors = ts_node_has_error(root);

    auto parsePhpMembers = [&](TSNode declarationList) {
        QVariantList members;
        const uint32_t count = ts_node_named_child_count(declarationList);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(declarationList, i);
            const QString type = tsType(child);
            if (type == QStringLiteral("method_declaration")) {
                const QString name = nodeText(fieldNode(child, "name"), source);
                members.append(makeSymbol(QStringLiteral("method"), name, nodeLine(child), QString(), {}, nodeSnippet(child, source)));
            } else if (type == QStringLiteral("property_declaration")) {
                const QString snippet = nodeText(child, source);
                const QString fullSnippet = nodeSnippet(child, source);
                QRegularExpression propertyPattern(QStringLiteral(R"(\$([A-Za-z_]\w*))"));
                auto it = propertyPattern.globalMatch(snippet);
                while (it.hasNext()) {
                    const auto match = it.next();
                    members.append(makeSymbol(QStringLiteral("property"),
                                              QStringLiteral("$") + match.captured(1),
                                              nodeLine(child), QString(), {}, fullSnippet));
                }
            } else if (type == QStringLiteral("const_declaration")) {
                const QString snippet = nodeText(child, source);
                const QString fullSnippet = nodeSnippet(child, source);
                QRegularExpression constPattern(QStringLiteral(R"(\b([A-Z_][A-Z0-9_]*)\b)"));
                auto it = constPattern.globalMatch(snippet);
                while (it.hasNext()) {
                    const auto match = it.next();
                    members.append(makeSymbol(QStringLiteral("constant"), match.captured(1), nodeLine(child), QString(), {}, fullSnippet));
                }
            }
        }
        return members;
    };

    const uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_named_child(root, i);
        const QString type = tsType(child);

        if (type == QStringLiteral("php_tag") || type == QStringLiteral("text")) {
            continue;
        }
        if (type == QStringLiteral("class_declaration")
            || type == QStringLiteral("interface_declaration")
            || type == QStringLiteral("trait_declaration")) {
            const QString name = nodeText(fieldNode(child, "name"), source);
            QString kind = QStringLiteral("class");
            if (type == QStringLiteral("interface_declaration")) {
                kind = QStringLiteral("interface");
            } else if (type == QStringLiteral("trait_declaration")) {
                kind = QStringLiteral("trait");
            }
            const TSNode body = fieldNode(child, "body");
            symbols.append(makeSymbol(kind, name, nodeLine(child), QString(), parsePhpMembers(body), nodeSnippet(child, source)));
        } else if (type == QStringLiteral("function_definition")) {
            const QString name = nodeText(fieldNode(child, "name"), source);
            symbols.append(makeSymbol(QStringLiteral("function"), name, nodeLine(child), QString(), {}, nodeSnippet(child, source)));
        } else if (type == QStringLiteral("const_declaration")) {
            const QString snippet = nodeText(child, source);
            const QString fullSnippet = nodeSnippet(child, source);
            QRegularExpression constPattern(QStringLiteral(R"(\b([A-Z_][A-Z0-9_]*)\b)"));
            auto it = constPattern.globalMatch(snippet);
            while (it.hasNext()) {
                const auto match = it.next();
                symbols.append(makeSymbol(QStringLiteral("constant"), match.captured(1), nodeLine(child), QString(), {}, fullSnippet));
            }
        }
    }

    if (!symbols.isEmpty()) {
        QHash<QString, QVariantMap> byKey;
        QHash<QString, QStringList> keysByName;
        QHash<QString, QVariantList> callsByKey;
        QHash<QString, QVariantList> calledByByKey;
        collectSymbolsByKey(symbols, byKey, keysByName);

        std::function<void(TSNode, const QString &)> visit = [&](TSNode node, const QString &currentKey) {
            QString activeKey = currentKey;
            const QString nodeKey = phpCallableKeyForNode(node, source, byKey);
            if (!nodeKey.isEmpty()) {
                activeKey = nodeKey;
            }

            const QString nodeType = tsType(node);
            if (!activeKey.isEmpty()
                && (nodeType == QStringLiteral("function_call_expression")
                    || nodeType == QStringLiteral("member_call_expression")
                    || nodeType == QStringLiteral("scoped_call_expression"))) {
                const QString targetName = phpCallTargetName(node, source);
                const QStringList candidateKeys = keysByName.value(targetName);
                if (!targetName.isEmpty() && !candidateKeys.isEmpty()) {
                    const QString targetKey = bestRelationTargetKey(candidateKeys, byKey);
                    if (targetKey != activeKey && byKey.contains(targetKey)) {
                        appendUniqueRelation(callsByKey, activeKey, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
                        appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(activeKey)), QStringLiteral("called by"));
                    }
                }
            }

            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t index = 0; index < childCount; ++index) {
                visit(ts_node_named_child(node, index), activeKey);
            }
        };

        visit(root, QString());
        symbols = applyRelationsToSymbols(symbols, callsByKey, calledByByKey);
    }

    symbols = applySnippetCallRelations(symbols);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("php"));
    result.insert(QStringLiteral("analysisHasAstErrors"), hasAstErrors);
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parseScriptLikeTreeSitter(const QString &path, const QString &text, const QString &language) const
{
    QVariantList symbols;
    const bool reactMode = language == QStringLiteral("jsx") || language == QStringLiteral("tsx");
    const QByteArray source = text.toUtf8();
    TSLanguage *tsLanguage = languageForName(language);
    if (!tsLanguage) {
        return makeResultSkeleton(path, QFileInfo(path).fileName(), language);
    }

    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, tsLanguage)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return makeResultSkeleton(path, QFileInfo(path).fileName(), language);
    }

    TSTree *tree = parseAnalysedSource(parser, source);
    TSNode root = ts_tree_root_node(tree);
    const bool hasAstErrors = ts_node_has_error(root);

    auto classifyFunctionName = [&](const QString &name) {
        if (name.startsWith(QStringLiteral("use"))) {
            return QStringLiteral("hook");
        }
        if (reactMode && !name.isEmpty() && name.at(0).isUpper()) {
            return QStringLiteral("component");
        }
        return QStringLiteral("function");
    };

    auto appendSymbolIfNew = [&](const QVariantMap &symbol) {
        const QString name = symbol.value(QStringLiteral("name")).toString();
        const QString kind = symbol.value(QStringLiteral("kind")).toString();
        
        for (int i = 0; i < symbols.size(); ++i) {
            QVariantMap existing = symbols.at(i).toMap();
            if (existing.value(QStringLiteral("name")).toString() == name
                && (kind.isEmpty() || existing.value(QStringLiteral("kind")).toString() == kind)) {
                
                const QString newDetail = symbol.value(QStringLiteral("detail")).toString();
                if (!newDetail.isEmpty()) {
                    QString existingDetail = existing.value(QStringLiteral("detail")).toString();
                    if (existingDetail.isEmpty()) {
                        existing.insert(QStringLiteral("detail"), newDetail);
                    } else if (!existingDetail.contains(newDetail)) {
                        existing.insert(QStringLiteral("detail"), existingDetail + QStringLiteral(", ") + newDetail);
                    }
                    symbols[i] = existing;
                }
                return;
            }
        }
        symbols.append(symbol);
    };

    auto parseJsClassMembers = [&](TSNode body) {
        QVariantList members;
        const uint32_t count = ts_node_named_child_count(body);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(body, i);
            const QString type = tsType(child);
            if (type == QStringLiteral("method_definition")) {
                QString name = nodeText(fieldNode(child, "name"), source);
                if (name.isEmpty()) {
                    name = nodeText(ts_node_named_child(child, 0), source);
                }
                members.append(makeSymbol(QStringLiteral("method"), name, nodeLine(child), QString(), {}, nodeSnippet(child, source)));
            } else if (type == QStringLiteral("public_field_definition")
                       || type == QStringLiteral("field_definition")) {
                QString name = nodeText(fieldNode(child, "name"), source);
                if (name.isEmpty()) {
                    name = nodeText(ts_node_named_child(child, 0), source);
                }
                members.append(makeSymbol(QStringLiteral("property"), name, nodeLine(child), QString(), {}, nodeSnippet(child, source)));
            }
        }
        return members;
    };

    std::function<QVariantList(TSNode)> parseJsObjectMembers = [&](TSNode objectNode) {
        QVariantList members;
        const uint32_t count = ts_node_named_child_count(objectNode);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(objectNode, i);
            const QString type = tsType(child);
            if (type == QStringLiteral("pair")) {
                const TSNode keyNode = fieldNode(child, "key");
                const TSNode valueNode = fieldNode(child, "value");
                const QString key = nodeText(keyNode, source);
                const QString valueType = tsType(valueNode);
                if (valueType == QStringLiteral("arrow_function")
                    || valueType == QStringLiteral("function")
                    || valueType == QStringLiteral("function_expression")) {
                    members.append(makeSymbol(QStringLiteral("function"), key, nodeLine(child), QString(), {}, nodeSnippet(child, source)));
                } else if (valueType == QStringLiteral("object")) {
                    members.append(makeSymbol(QStringLiteral("property"), key, nodeLine(child),
                                              QStringLiteral("nested object"), parseJsObjectMembers(valueNode), nodeSnippet(child, source)));
                } else {
                    members.append(makeSymbol(QStringLiteral("property"), key, nodeLine(child), QString(), {}, nodeSnippet(child, source)));
                }
            } else if (type == QStringLiteral("method_definition")) {
                QString name = nodeText(fieldNode(child, "name"), source);
                if (name.isEmpty()) {
                    name = nodeText(ts_node_named_child(child, 0), source);
                }
                members.append(makeSymbol(QStringLiteral("method"), name, nodeLine(child), QString(), {}, nodeSnippet(child, source)));
            } else if (type == QStringLiteral("shorthand_property_identifier")) {
                members.append(makeSymbol(QStringLiteral("property"), nodeText(child, source), nodeLine(child), QString(), {}, nodeSnippet(child, source)));
            }
        }
        return members;
    };

    auto unwrapExport = [&](TSNode node) {
        const QString type = tsType(node);
        if (type != QStringLiteral("export_statement")) {
            return node;
        }
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            const QString childType = tsType(child);
            if (childType != QStringLiteral("comment")) {
                return child;
            }
        }
        return node;
    };

    QVariantList dependencies;
    QVariantList routes;
    QSet<QString> seenDependencies;

    auto appendDependency = [&](const QString &target, const QString &type, int line,
                                const QString &snippet = QString(),
                                const QVariantList &bindings = QVariantList{}) {
        if (seenDependencies.contains(type + QLatin1Char('|') + target)) {
            return;
        }
        seenDependencies.insert(type + QLatin1Char('|') + target);

        QVariantMap item = makeSourceContextItem(path, language, line,
                                                 snippet.isEmpty() ? snippetFromLine(text, line, 0) : snippet,
                                                 QStringLiteral("%1 dependency").arg(type));
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("label"), target);
        if (!bindings.isEmpty()) {
            item.insert(QStringLiteral("bindings"), bindings);
        }

        const QDir dir = QFileInfo(path).dir();
        if (target.startsWith(QStringLiteral("./")) || target.startsWith(QStringLiteral("../"))) {
            QString resolved = QDir::cleanPath(dir.filePath(target));
            QString chosenPath = resolved;
            const QStringList candidates = {
                resolved,
                resolved + QStringLiteral(".js"),
                resolved + QStringLiteral(".json"),
                resolved + QStringLiteral(".ts"),
                resolved + QStringLiteral(".tsx"),
                QDir(resolved).filePath(QStringLiteral("index.js")),
                QDir(resolved).filePath(QStringLiteral("index.ts"))
            };
            for (const QString &candidate : candidates) {
                if (QFileInfo::exists(candidate)) {
                    chosenPath = candidate;
                    break;
                }
            }
            item.insert(QStringLiteral("path"), chosenPath);
            item.insert(QStringLiteral("exists"), QFileInfo::exists(chosenPath));
            item.insert(QStringLiteral("label"), QFileInfo(chosenPath).fileName().isEmpty() ? target : QFileInfo(chosenPath).fileName());
        } else {
            item.insert(QStringLiteral("path"), QString());
            item.insert(QStringLiteral("exists"), true);
        }
        dependencies.append(item);
    };

    // Owner symbol per function node (by start byte), for call attribution of
    // functions that are not plain declarations (assigned members etc.).
    QHash<uint32_t, QString> ownerKeyByFunctionStart;
    auto isFunctionValue = [&](const QString &valueType) {
        return valueType == QStringLiteral("arrow_function") || valueType == QStringLiteral("function")
            || valueType == QStringLiteral("function_expression")
            || valueType == QStringLiteral("generator_function");
    };
    auto unwrapExpression = [&](TSNode node) {
        for (int guard = 0; guard < 4 && !ts_node_is_null(node); ++guard) {
            const QString nodeKind = tsType(node);
            if (nodeKind == QStringLiteral("parenthesized_expression")
                || nodeKind == QStringLiteral("unary_expression")
                || nodeKind == QStringLiteral("await_expression")) {
                node = ts_node_named_child(node, 0);
                continue;
            }
            break;
        }
        return node;
    };
    // Attach a member (method/property) to the top-level symbol `ownerName`,
    // creating a grouping symbol when the owner is not declared in this file.
    auto attachMember = [&](const QString &ownerName, QVariantMap member, const QString &ownerKindIfNew,
                            const QString &ownerDetailIfNew, int line, const QString &snippet) {
        for (int index = 0; index < symbols.size(); ++index) {
            QVariantMap owner = symbols.at(index).toMap();
            if (owner.value(QStringLiteral("name")).toString() != ownerName) {
                continue;
            }
            QVariantList members = owner.value(QStringLiteral("members")).toList();
            for (const QVariant &existing : std::as_const(members)) {
                if (existing.toMap().value(QStringLiteral("name")).toString()
                    == member.value(QStringLiteral("name")).toString()) {
                    return;
                }
            }
            members.append(member);
            owner.insert(QStringLiteral("members"), members);
            symbols[index] = owner;
            return;
        }
        symbols.append(makeSymbol(ownerKindIfNew, ownerName, line, ownerDetailIfNew, {member}, snippet));
    };
    auto isModuleWrapperCall = [&](TSNode call) {
        const QString callee = nodeText(unwrapExpression(fieldNode(call, "function")), source).trimmed();
        static const QSet<QString> wrappers = {
            QStringLiteral("define"), QStringLiteral("require"), QStringLiteral("requirejs"),
            QStringLiteral("$"), QStringLiteral("jQuery"), QStringLiteral("System.register"),
        };
        if (wrappers.contains(callee) || callee.endsWith(QStringLiteral(".ready"))) {
            return true;
        }
        if (callee.endsWith(QStringLiteral("addEventListener"))) {
            const QString args = nodeText(fieldNode(call, "arguments"), source);
            return args.contains(QStringLiteral("DOMContentLoaded")) || args.contains(QStringLiteral("'load'"))
                || args.contains(QStringLiteral("\"load\""));
        }
        return false;
    };

    // Closure-module / constructor-function pattern: functions declared directly
    // in a function's body, and `this.m = function () {}` assignments there,
    // are that function's members.
    auto functionBodyMembers = [&](TSNode function) {
        QVariantList members;
        TSNode body = fieldNode(function, "body");
        if (ts_node_is_null(body) || tsType(body) != QStringLiteral("statement_block")) {
            return members;
        }
        const uint32_t bodyCount = ts_node_named_child_count(body);
        for (uint32_t index = 0; index < bodyCount; ++index) {
            TSNode statement = ts_node_named_child(body, index);
            const QString statementType = tsType(statement);
            if (statementType == QStringLiteral("function_declaration")
                || statementType == QStringLiteral("generator_function_declaration")) {
                QVariantMap member = makeSymbol(QStringLiteral("function"), nodeText(fieldNode(statement, "name"), source),
                                                nodeLine(statement), QStringLiteral("inner"), {}, nodeSnippet(statement, source));
                ownerKeyByFunctionStart.insert(ts_node_start_byte(statement), symbolKey(member));
                members.append(member);
            } else if (statementType == QStringLiteral("expression_statement")) {
                TSNode expression = ts_node_named_child(statement, 0);
                if (tsType(expression) != QStringLiteral("assignment_expression")) {
                    continue;
                }
                const QString left = nodeText(fieldNode(expression, "left"), source).trimmed();
                TSNode value = fieldNode(expression, "right");
                if (left.startsWith(QStringLiteral("this.")) && left.count(QLatin1Char('.')) == 1
                    && isFunctionValue(tsType(value))) {
                    QVariantMap member = makeSymbol(QStringLiteral("method"), left.mid(5), nodeLine(expression),
                                                    QStringLiteral("assigned in constructor"), {}, nodeSnippet(statement, source));
                    ownerKeyByFunctionStart.insert(ts_node_start_byte(value), symbolKey(member));
                    members.append(member);
                }
            }
        }
        return members;
    };

    std::function<void(TSNode, int)> processContainer;
    std::function<void(TSNode, int)> processFunctionBodyForward;
    // Test suites (describe/it/test and hooks) and top-level event wiring
    // (addEventListener, jQuery .on/.click) are the structure of test files and
    // page scripts, which otherwise show no symbols at all.
    std::function<QVariantMap(TSNode, int)> testSymbolFor;
    testSymbolFor = [&](TSNode call, int depth) -> QVariantMap {
        static const QSet<QString> suites = {QStringLiteral("describe"), QStringLiteral("context"), QStringLiteral("suite")};
        static const QSet<QString> cases = {QStringLiteral("it"), QStringLiteral("test"), QStringLiteral("specify")};
        static const QSet<QString> hooks = {QStringLiteral("beforeEach"), QStringLiteral("afterEach"),
                                            QStringLiteral("beforeAll"), QStringLiteral("afterAll"),
                                            QStringLiteral("before"), QStringLiteral("after")};
        TSNode callee = fieldNode(call, "function");
        QString name = nodeText(callee, source).trimmed();
        // describe.only / it.skip / test.each(...)(...)
        if (tsType(callee) == QStringLiteral("member_expression")) {
            name = nodeText(fieldNode(callee, "object"), source).trimmed();
        } else if (tsType(callee) == QStringLiteral("call_expression")) {
            name = nodeText(fieldNode(callee, "function"), source).trimmed().section(QLatin1Char('.'), 0, 0);
        }
        const bool isSuite = suites.contains(name);
        const bool isCase = cases.contains(name);
        const bool isHook = hooks.contains(name);
        if (!isSuite && !isCase && !isHook) {
            return {};
        }
        TSNode arguments = fieldNode(call, "arguments");
        QString title = name;
        TSNode body{};
        bool hasBody = false;
        for (uint32_t a = 0; a < ts_node_named_child_count(arguments); ++a) {
            TSNode arg = ts_node_named_child(arguments, a);
            const QString argType = tsType(arg);
            if (a == 0 && (argType == QStringLiteral("string") || argType == QStringLiteral("template_string"))) {
                title = nodeValueText(arg, source);
            } else if (isFunctionValue(argType)) {
                body = arg;
                hasBody = true;
            }
        }
        if (!hasBody || (!isHook && title == name)) {
            return {};
        }
        const QString kind = isSuite ? QStringLiteral("test suite") : (isHook ? QStringLiteral("test hook") : QStringLiteral("test"));
        QVariantMap symbol = makeSymbol(kind, isHook ? name : title, nodeLine(call), isHook ? QString() : name, {},
                                        nodeSnippet(call, source));
        ownerKeyByFunctionStart.insert(ts_node_start_byte(body), symbolKey(symbol));
        if (isSuite && depth < 6) {
            QVariantList members;
            TSNode block = fieldNode(body, "body");
            for (uint32_t k = 0; k < ts_node_named_child_count(block); ++k) {
                TSNode statement = ts_node_named_child(block, k);
                if (tsType(statement) != QStringLiteral("expression_statement")) {
                    continue;
                }
                TSNode inner = unwrapExpression(ts_node_named_child(statement, 0));
                if (tsType(inner) == QStringLiteral("call_expression")) {
                    const QVariantMap child = testSymbolFor(inner, depth + 1);
                    if (!child.isEmpty()) {
                        members.append(child);
                    }
                }
            }
            symbol.insert(QStringLiteral("members"), members);
        }
        return symbol;
    };

    auto appendTestOrHandler = [&](TSNode call, int depth) -> bool {
        const QVariantMap test = testSymbolFor(call, depth);
        if (!test.isEmpty()) {
            symbols.append(test); // repeated titles are legitimate; keep each
            return true;
        }
        TSNode callee = fieldNode(call, "function");
        if (tsType(callee) != QStringLiteral("member_expression")) {
            return false;
        }
        const QString method = nodeText(fieldNode(callee, "property"), source).trimmed();
        static const QSet<QString> jqueryEvents = {
            QStringLiteral("click"), QStringLiteral("submit"), QStringLiteral("change"), QStringLiteral("keyup"),
            QStringLiteral("keydown"), QStringLiteral("ready"), QStringLiteral("hover"), QStringLiteral("input"),
        };
        TSNode arguments = fieldNode(call, "arguments");
        QString event;
        TSNode handlerFunction{};
        bool found = false;
        if (method == QStringLiteral("addEventListener") || method == QStringLiteral("on")) {
            const QString first = nodeValueText(ts_node_named_child(arguments, 0), source);
            for (uint32_t a = 1; a < ts_node_named_child_count(arguments); ++a) {
                TSNode arg = ts_node_named_child(arguments, a);
                if (isFunctionValue(tsType(arg))) {
                    handlerFunction = arg;
                    found = true;
                }
            }
            event = first;
        } else if (jqueryEvents.contains(method) && ts_node_named_child_count(arguments) == 1
                   && isFunctionValue(tsType(ts_node_named_child(arguments, 0)))) {
            handlerFunction = ts_node_named_child(arguments, 0);
            event = method;
            found = true;
        }
        if (!found || event.isEmpty()) {
            return false;
        }
        QString target = nodeText(fieldNode(callee, "object"), source).simplified();
        if (target.size() > 40) {
            target = target.left(37) + QStringLiteral("...");
        }
        QVariantMap handler = makeSymbol(QStringLiteral("handler"), QStringLiteral("%1 · %2").arg(target, event), nodeLine(call),
                                         QStringLiteral("event handler"), functionBodyMembers(handlerFunction), nodeSnippet(call, source));
        ownerKeyByFunctionStart.insert(ts_node_start_byte(handlerFunction), symbolKey(handler));
        symbols.append(handler);
        // Functions declared inside the handler still count as module structure.
        processFunctionBodyForward(handlerFunction, depth);
        return true;
    };

    auto processFunctionBody = [&](TSNode function, int depth) {
        TSNode body = fieldNode(function, "body");
        if (!ts_node_is_null(body) && tsType(body) == QStringLiteral("statement_block")) {
            processContainer(body, depth + 1);
        }
    };
    processFunctionBodyForward = processFunctionBody;

    processContainer = [&](TSNode container, int depth) {
    if (depth > 4) {
        return;
    }
    const uint32_t count = ts_node_named_child_count(container);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode originalNode = ts_node_named_child(container, i);
        const QString originalType = tsType(originalNode);
        bool isExported = originalType == QStringLiteral("export_statement");
        TSNode child = unwrapExport(originalNode);
        const QString type = tsType(child);

        if (originalType == QStringLiteral("import_statement")) {
            const TSNode sourceNode = fieldNode(originalNode, "source");
            const QString target = nodeValueText(sourceNode, source);
            appendDependency(target, QStringLiteral("import"), nodeLine(originalNode),
                             nodeSnippet(originalNode, source, 1),
                             parseScriptImportBindingsFromStatement(nodeText(originalNode, source)));
            continue;
        }

        if (originalType == QStringLiteral("export_statement")) {
            const TSNode sourceNode = fieldNode(originalNode, "source");
            if (!ts_node_is_null(sourceNode)) {
                const QString target = nodeValueText(sourceNode, source);
                appendDependency(target, QStringLiteral("export"), nodeLine(originalNode),
                                 nodeSnippet(originalNode, source, 1),
                                 parseScriptImportBindingsFromStatement(nodeText(originalNode, source)));
                // Barrel modules: the re-exported names are this module's API.
                QStringList names;
                for (uint32_t k = 0; k < ts_node_named_child_count(originalNode); ++k) {
                    TSNode clause = ts_node_named_child(originalNode, k);
                    if (tsType(clause) == QStringLiteral("export_clause")) {
                        for (uint32_t m = 0; m < ts_node_named_child_count(clause); ++m) {
                            TSNode specifier = ts_node_named_child(clause, m);
                            QString exported = nodeText(fieldNode(specifier, "alias"), source).trimmed();
                            if (exported.isEmpty()) {
                                exported = nodeText(fieldNode(specifier, "name"), source).trimmed();
                            }
                            if (!exported.isEmpty()) {
                                names.append(exported);
                            }
                        }
                    } else if (tsType(clause) == QStringLiteral("namespace_export")) {
                        names.append(nodeText(clause, source).simplified());
                    }
                }
                if (names.isEmpty()) {
                    names.append(QStringLiteral("* from %1").arg(target));
                }
                for (const QString &exported : std::as_const(names)) {
                    appendSymbolIfNew(makeSymbol(QStringLiteral("re-export"), exported, nodeLine(originalNode),
                                                 QStringLiteral("from %1").arg(target), {}, nodeSnippet(originalNode, source, 3)));
                }
                continue;
            }
        }

        if (type == QStringLiteral("function_declaration")
            || type == QStringLiteral("generator_function_declaration")) {
            const QString name = nodeText(fieldNode(child, "name"), source);
            QVariantMap symbol = makeSymbol(classifyFunctionName(name), name, nodeLine(child), QString(),
                                            functionBodyMembers(child), nodeSnippet(child, source));
            if (isExported) symbol.insert(QStringLiteral("detail"), QStringLiteral("exported"));
            appendSymbolIfNew(symbol);
        } else if (isExported && originalType == QStringLiteral("export_statement")
                   && !ts_node_is_null(fieldNode(originalNode, "value"))
                   && tsType(fieldNode(originalNode, "value")) != QStringLiteral("identifier")
                   && !isFunctionValue(tsType(fieldNode(originalNode, "value")))
                   && tsType(fieldNode(originalNode, "value")) != QStringLiteral("class")) {
            // export default { ... } / export default [ ... ] / export default defineConfig({...})
            TSNode value = fieldNode(originalNode, "value");
            QVariantList members;
            if (tsType(value) == QStringLiteral("object")) {
                members = parseJsObjectMembers(value);
            } else if (tsType(value) == QStringLiteral("call_expression")) {
                TSNode arguments = fieldNode(value, "arguments");
                for (uint32_t a = 0; a < ts_node_named_child_count(arguments); ++a) {
                    if (tsType(ts_node_named_child(arguments, a)) == QStringLiteral("object")) {
                        members.append(parseJsObjectMembers(ts_node_named_child(arguments, a)));
                    }
                }
            }
            appendSymbolIfNew(makeSymbol(QStringLiteral("variable"), QStringLiteral("default"), nodeLine(originalNode),
                                         QStringLiteral("default export"), members, nodeSnippet(originalNode, source)));
        } else if (type == QStringLiteral("identifier") && isExported) {
            const QString name = nodeText(child, source);
            bool known = false;
            for (int index = 0; index < symbols.size() && !known; ++index) {
                QVariantMap existing = symbols.at(index).toMap();
                if (existing.value(QStringLiteral("name")).toString() == name) {
                    known = true;
                    if (!existing.value(QStringLiteral("detail")).toString().contains(QStringLiteral("exported"))) {
                        const QString detail = existing.value(QStringLiteral("detail")).toString();
                        existing.insert(QStringLiteral("detail"), detail.isEmpty() ? QStringLiteral("exported")
                                                                                   : detail + QStringLiteral(", exported"));
                        symbols[index] = existing;
                    }
                }
            }
            if (!known) {
                appendSymbolIfNew(makeSymbol(QStringLiteral("variable"), name, nodeLine(child), QStringLiteral("exported"), {}, nodeSnippet(child, source)));
            }
        } else if (type == QStringLiteral("class_declaration")
                   || type == QStringLiteral("abstract_class_declaration")) {
            const QString name = nodeText(fieldNode(child, "name"), source);
            const TSNode body = fieldNode(child, "body");
            QVariantMap symbol = makeSymbol(QStringLiteral("class"), name, nodeLine(child), 
                                         isExported ? QStringLiteral("exported") : QString(),
                                         parseJsClassMembers(body), nodeSnippet(child, source));
            appendSymbolIfNew(symbol);
        } else if (type == QStringLiteral("interface_declaration")) {
            const QString name = nodeText(fieldNode(child, "name"), source);
            QVariantMap symbol = makeSymbol(QStringLiteral("props"), name, nodeLine(child), QString(), {}, nodeSnippet(child, source));
            if (isExported) symbol.insert(QStringLiteral("detail"), QStringLiteral("exported"));
            appendSymbolIfNew(symbol);
        } else if (type == QStringLiteral("type_alias_declaration")) {
            const QString name = nodeText(fieldNode(child, "name"), source);
            QVariantMap symbol = makeSymbol(QStringLiteral("type"), name, nodeLine(child), QString(), {}, nodeSnippet(child, source));
            if (isExported) symbol.insert(QStringLiteral("detail"), QStringLiteral("exported"));
            appendSymbolIfNew(symbol);
        } else if (type == QStringLiteral("lexical_declaration")
                   || type == QStringLiteral("variable_declaration")) {
            const uint32_t declarationCount = ts_node_named_child_count(child);
            for (uint32_t j = 0; j < declarationCount; ++j) {
                TSNode declarator = ts_node_named_child(child, j);
                if (tsType(declarator) == QStringLiteral("variable_declarator")) {
                    const QString name = nodeText(fieldNode(declarator, "name"), source);
                    const TSNode valueNode = fieldNode(declarator, "value");
                    const QString valueType = tsType(valueNode);
                    
                    QVariantMap symbol;
                    const QString calleeText = valueType == QStringLiteral("call_expression")
                        ? nodeText(fieldNode(valueNode, "function"), source).trimmed()
                        : QString();
                    const bool isRequireBinding = nodeText(valueNode, source).trimmed().startsWith(QStringLiteral("require("));
                    const bool isClassFactory = calleeText.endsWith(QStringLiteral(".extend"))
                        || calleeText.endsWith(QStringLiteral("createClass"))
                        || calleeText == QStringLiteral("defineComponent")
                        || calleeText.endsWith(QStringLiteral(".component"));
                    if (isRequireBinding) {
                        // A require() binding is a dependency (recorded below), not a symbol.
                    } else if (isClassFactory) {
                        QVariantList members;
                        TSNode arguments = fieldNode(valueNode, "arguments");
                        const uint32_t argCount = ts_node_named_child_count(arguments);
                        for (uint32_t a = 0; a < argCount; ++a) {
                            TSNode arg = ts_node_named_child(arguments, a);
                            const QString argType = tsType(arg);
                            if (argType == QStringLiteral("object")) {
                                members.append(parseJsObjectMembers(arg));
                            } else if (isFunctionValue(argType)) {
                                QVariantMap ctor = makeSymbol(QStringLiteral("constructor"), name, nodeLine(arg),
                                                              QString(), {}, nodeSnippet(arg, source));
                                ownerKeyByFunctionStart.insert(ts_node_start_byte(arg), symbolKey(ctor));
                                members.append(ctor);
                            }
                        }
                        symbol = makeSymbol(QStringLiteral("class"), name, nodeLine(declarator),
                                            QStringLiteral("%1(...)").arg(calleeText), members,
                                            nodeSnippet(child, source));
                    } else if (valueType == QStringLiteral("arrow_function")
                        || valueType == QStringLiteral("function")
                        || valueType == QStringLiteral("function_expression")) {
                        symbol = makeSymbol(classifyFunctionName(name), name, nodeLine(declarator),
                                                     valueType == QStringLiteral("function_expression")
                                                         ? QStringLiteral("function expression")
                                                         : (isExported ? QStringLiteral("exported") : QString()),
                                                     functionBodyMembers(valueNode), nodeSnippet(child, source));
                    } else if (valueType == QStringLiteral("class")
                               || valueType == QStringLiteral("class_expression")) {
                        const TSNode body = fieldNode(valueNode, "body");
                        symbol = makeSymbol(QStringLiteral("class"), name, nodeLine(declarator),
                                                     isExported ? QStringLiteral("exported class expression") : QStringLiteral("class expression"), 
                                                     parseJsClassMembers(body), nodeSnippet(child, source));
                    } else if (valueType == QStringLiteral("object")) {
                        symbol = makeSymbol(QStringLiteral("variable"), name, nodeLine(declarator),
                                                     isExported ? QStringLiteral("exported object") : QStringLiteral("object"), 
                                                     parseJsObjectMembers(valueNode), nodeSnippet(child, source));
                    } else {
                        symbol = makeSymbol(QStringLiteral("variable"), name, nodeLine(declarator));
                        if (isExported) symbol.insert(QStringLiteral("detail"), QStringLiteral("exported"));
                        symbol.insert(QStringLiteral("snippet"), nodeSnippet(child, source));
                    }
                    if (!symbol.isEmpty()) {
                        appendSymbolIfNew(symbol);
                    }

                    // Check for require in variable declaration
                    if (valueType == QStringLiteral("call_expression")) {
                        const QString functionName = nodeText(fieldNode(valueNode, "function"), source);
                        if (functionName == QStringLiteral("require")) {
                            TSNode arguments = ts_node_child_by_field_name(valueNode, "arguments", 9);
                            if (ts_node_named_child_count(arguments) > 0) {
                                TSNode arg = ts_node_named_child(arguments, 0);
                                const QString target = nodeValueText(arg, source);
                            appendDependency(target, QStringLiteral("require"), nodeLine(valueNode),
                                             nodeSnippet(valueNode, source, 1),
                                             parseRequireBindingsFromStatement(nodeText(child, source)));
                            }
                        }
                    }
                }
            }
        } else if (type == QStringLiteral("expression_statement")) {
            TSNode expression = ts_node_named_child(child, 0);
            const QString exprType = tsType(expression);
            if (exprType == QStringLiteral("assignment_expression")) {
                const QString left = nodeText(fieldNode(expression, "left"), source).trimmed();
                TSNode valueNode = fieldNode(expression, "right");
                const QString valueType = tsType(valueNode);
                const QStringList leftParts = left.split(QLatin1Char('.'));
                const bool leftIsModuleExport = left == QStringLiteral("module.exports")
                    || left.startsWith(QStringLiteral("module.exports."))
                    || left.startsWith(QStringLiteral("exports."));
                static const QSet<QString> globalObjects = {
                    QStringLiteral("window"), QStringLiteral("global"), QStringLiteral("globalThis"),
                    QStringLiteral("self"),
                };
                const bool simpleMemberPath = leftParts.size() >= 2 && std::all_of(leftParts.cbegin(), leftParts.cend(), [](const QString &part) {
                    static const QRegularExpression identifierPattern(QStringLiteral(R"(^[A-Za-z_$][\w$]*$)"));
                    return identifierPattern.match(part).hasMatch();
                });
                if (simpleMemberPath && leftParts.size() >= 2 && leftParts.last().startsWith(QStringLiteral("on"))
                    && leftParts.last().size() > 2 && leftParts.last() == leftParts.last().toLower()
                    && isFunctionValue(valueType)) {
                    // el.onclick = function () {...} / window.onload = () => {...}
                    QVariantMap handler = makeSymbol(QStringLiteral("handler"),
                                                     QStringLiteral("%1 · %2").arg(leftParts.mid(0, leftParts.size() - 1).join(QLatin1Char('.')),
                                                                                  leftParts.last().mid(2)),
                                                     nodeLine(expression), QStringLiteral("event handler"), {}, nodeSnippet(child, source));
                    ownerKeyByFunctionStart.insert(ts_node_start_byte(valueNode), symbolKey(handler));
                    appendSymbolIfNew(handler);
                    continue;
                }
                if (!leftIsModuleExport && simpleMemberPath && leftParts.first() != QStringLiteral("this")) {
                    const int line = nodeLine(expression);
                    const QString snippet = nodeSnippet(child, source);
                    const bool isPrototype = leftParts.size() >= 3 && leftParts.at(leftParts.size() - 2) == QStringLiteral("prototype");
                    if (leftParts.size() == 2 && leftParts.at(1) == QStringLiteral("prototype")
                        && valueType == QStringLiteral("object")) {
                        // X.prototype = { a: function () {}, ... }
                        for (const QVariant &memberValue : parseJsObjectMembers(valueNode)) {
                            QVariantMap member = memberValue.toMap();
                            if (member.value(QStringLiteral("kind")).toString() == QStringLiteral("function")) {
                                member.insert(QStringLiteral("kind"), QStringLiteral("method"));
                            }
                            attachMember(leftParts.first(), member, QStringLiteral("class"),
                                         QStringLiteral("prototype"), line, snippet);
                        }
                    } else if (isFunctionValue(valueType) || valueType == QStringLiteral("class")) {
                        const QString memberName = leftParts.last();
                        const QString ownerName = isPrototype ? leftParts.mid(0, leftParts.size() - 2).join(QLatin1Char('.'))
                                                              : leftParts.mid(0, leftParts.size() - 1).join(QLatin1Char('.'));
                        if (globalObjects.contains(ownerName)) {
                            QVariantMap symbol = makeSymbol(classifyFunctionName(memberName), memberName, line,
                                                            QStringLiteral("global"), {}, snippet);
                            ownerKeyByFunctionStart.insert(ts_node_start_byte(valueNode), symbolKey(symbol));
                            appendSymbolIfNew(symbol);
                        } else {
                            QVariantMap member = makeSymbol(valueType == QStringLiteral("class") ? QStringLiteral("class")
                                                                                                  : QStringLiteral("method"),
                                                            memberName, line,
                                                            isPrototype ? QStringLiteral("prototype") : QStringLiteral("assigned"),
                                                            {}, snippet);
                            ownerKeyByFunctionStart.insert(ts_node_start_byte(valueNode), symbolKey(member));
                            attachMember(ownerName, member, isPrototype ? QStringLiteral("class") : QStringLiteral("variable"),
                                         isPrototype ? QStringLiteral("prototype") : QStringLiteral("object"),
                                         line, snippet);
                        }
                    }
                }
                if (left == QStringLiteral("module.exports") && valueType == QStringLiteral("call_expression")
                    && !nodeText(valueNode, source).trimmed().startsWith(QStringLiteral("require("))) {
                    // module.exports = merge(common, {...}) / defineConfig({...})
                    QVariantList members;
                    TSNode arguments = fieldNode(valueNode, "arguments");
                    for (uint32_t a = 0; a < ts_node_named_child_count(arguments); ++a) {
                        if (tsType(ts_node_named_child(arguments, a)) == QStringLiteral("object")) {
                            members.append(parseJsObjectMembers(ts_node_named_child(arguments, a)));
                        }
                    }
                    appendSymbolIfNew(makeSymbol(QStringLiteral("module"), QStringLiteral("module.exports"), nodeLine(expression),
                                                 nodeText(fieldNode(valueNode, "function"), source).simplified() + QStringLiteral("(...)"),
                                                 members, nodeSnippet(child, source)));
                }
                if (left == QStringLiteral("module.exports")
                    && nodeText(valueNode, source).trimmed().startsWith(QStringLiteral("require("))) {
                    appendSymbolIfNew(makeSymbol(QStringLiteral("re-export"), QStringLiteral("module.exports"), nodeLine(expression),
                                                 nodeText(valueNode, source).simplified(), {}, nodeSnippet(child, source)));
                }
                if (left == QStringLiteral("module.exports") && isFunctionValue(valueType)) {
                    QString exportedName = nodeText(fieldNode(valueNode, "name"), source).trimmed();
                    if (exportedName.isEmpty()) {
                        exportedName = QStringLiteral("module.exports");
                    }
                    QVariantMap symbol = makeSymbol(QStringLiteral("function"), exportedName, nodeLine(expression),
                                                    QStringLiteral("CommonJS export"), {}, nodeSnippet(child, source));
                    ownerKeyByFunctionStart.insert(ts_node_start_byte(valueNode), symbolKey(symbol));
                    appendSymbolIfNew(symbol);
                    processFunctionBody(valueNode, depth);
                }
                if (left == QStringLiteral("module.exports") && valueType == QStringLiteral("object")) {
                    const QVariantList members = parseJsObjectMembers(valueNode);
                    appendSymbolIfNew(makeSymbol(QStringLiteral("module"), left, nodeLine(expression),
                                                 QStringLiteral("CommonJS export object"), members, nodeSnippet(child, source)));
                    for (const QVariant &memberValue : members) {
                        QVariantMap member = memberValue.toMap();
                        member.insert(QStringLiteral("detail"), QStringLiteral("exported via module.exports"));
                        appendSymbolIfNew(member);
                    }
                } else if ((left.startsWith(QStringLiteral("module.exports."))
                            || left.startsWith(QStringLiteral("exports.")))
                           && !left.endsWith(QLatin1Char('.'))) {
                    const QString exportedName = left.section(QLatin1Char('.'), -1);
                    if (valueType == QStringLiteral("arrow_function")
                        || valueType == QStringLiteral("function")
                        || valueType == QStringLiteral("function_expression")) {
                        appendSymbolIfNew(makeSymbol(QStringLiteral("function"), exportedName, nodeLine(expression),
                                                     QStringLiteral("CommonJS export"), {}, nodeSnippet(child, source)));
                    } else if (valueType == QStringLiteral("object")) {
                        const QVariantList members = parseJsObjectMembers(valueNode);
                        appendSymbolIfNew(makeSymbol(QStringLiteral("module"), exportedName, nodeLine(expression),
                                                     QStringLiteral("CommonJS export object"), members, nodeSnippet(child, source)));
                        for (const QVariant &memberValue : members) {
                            QVariantMap member = memberValue.toMap();
                            member.insert(QStringLiteral("detail"),
                                          QStringLiteral("exported via %1").arg(left));
                            appendSymbolIfNew(member);
                        }
                    } else {
                        appendSymbolIfNew(makeSymbol(QStringLiteral("variable"), exportedName, nodeLine(expression),
                                                     QStringLiteral("CommonJS export"), {}, nodeSnippet(child, source)));
                    }
                }
            }
            TSNode callNode = unwrapExpression(expression);
            if (tsType(callNode) == QStringLiteral("call_expression")) {
                if (appendTestOrHandler(callNode, depth)) {
                    continue;
                }
                // jQuery.extend({...}) / jQuery.fn.extend({...}) / Object.assign(X, {...})
                const QString callee = nodeText(fieldNode(callNode, "function"), source).trimmed();
                TSNode arguments = fieldNode(callNode, "arguments");
                QString owner;
                if (callee.endsWith(QStringLiteral(".extend")) && callee.count(QLatin1Char('.')) <= 2) {
                    owner = callee.left(callee.size() - 7);
                } else if (callee == QStringLiteral("Object.assign") && ts_node_named_child_count(arguments) >= 2) {
                    owner = nodeText(ts_node_named_child(arguments, 0), source).trimmed();
                }
                static const QRegularExpression ownerPattern(QStringLiteral(R"(^[A-Za-z_$][\w$.]*$)"));
                if (!owner.isEmpty() && ownerPattern.match(owner).hasMatch()) {
                    bool attached = false;
                    for (uint32_t a = 0; a < ts_node_named_child_count(arguments); ++a) {
                        TSNode arg = ts_node_named_child(arguments, a);
                        if (tsType(arg) != QStringLiteral("object")) {
                            continue;
                        }
                        for (const QVariant &memberValue : parseJsObjectMembers(arg)) {
                            QVariantMap member = memberValue.toMap();
                            if (member.value(QStringLiteral("kind")).toString() == QStringLiteral("function")) {
                                member.insert(QStringLiteral("kind"), QStringLiteral("method"));
                            }
                            attachMember(owner, member, QStringLiteral("variable"), QStringLiteral("extended object"),
                                         nodeLine(callNode), nodeSnippet(child, source));
                            attached = true;
                        }
                    }
                    if (attached) {
                        continue;
                    }
                }
            }
            if (tsType(callNode) == QStringLiteral("call_expression")) {
                TSNode callee = unwrapExpression(fieldNode(callNode, "function"));
                const bool iife = isFunctionValue(tsType(callee));
                if (iife) {
                    processFunctionBody(callee, depth);
                }
                if (iife || isModuleWrapperCall(callNode)) {
                    TSNode arguments = fieldNode(callNode, "arguments");
                    const uint32_t argCount = ts_node_named_child_count(arguments);
                    for (uint32_t a = 0; a < argCount; ++a) {
                        TSNode arg = ts_node_named_child(arguments, a);
                        if (isFunctionValue(tsType(arg))) {
                            processFunctionBody(arg, depth);
                        }
                    }
                }
            }
            if (exprType == QStringLiteral("call_expression")) {
                const QString functionName = nodeText(fieldNode(expression, "function"), source);
                if (functionName == QStringLiteral("require")) {
                    TSNode arguments = ts_node_child_by_field_name(expression, "arguments", 9);
                    if (ts_node_named_child_count(arguments) > 0) {
                        TSNode arg = ts_node_named_child(arguments, 0);
                        const QString target = nodeValueText(arg, source);
                    appendDependency(target, QStringLiteral("require"), nodeLine(expression),
                                     nodeSnippet(expression, source, 1),
                                     parseRequireBindingsFromStatement(nodeText(child, source)));
                    }
                } else if (functionName.contains(QStringLiteral("app.")) || functionName.contains(QStringLiteral("router."))) {
                    // Express route detection
                    const QString method = functionName.section(QLatin1Char('.'), -1).toUpper();
                    const QStringList expressMethods = {
                        QStringLiteral("GET"), QStringLiteral("POST"), QStringLiteral("PUT"), 
                        QStringLiteral("PATCH"), QStringLiteral("DELETE"), QStringLiteral("USE"),
                        QStringLiteral("OPTIONS"), QStringLiteral("HEAD")
                    };
                    if (expressMethods.contains(method)) {
                        TSNode arguments = ts_node_child_by_field_name(expression, "arguments", 9);
                        if (ts_node_named_child_count(arguments) > 0) {
                            TSNode arg = ts_node_named_child(arguments, 0);
                            const QString routePath = nodeValueText(arg, source);
                            QVariantMap route = makeSourceContextItem(path, language, nodeLine(expression),
                                                                      nodeSnippet(expression, source, 2),
                                                                      QStringLiteral("route"));
                            route.insert(QStringLiteral("owner"), functionName.section(QLatin1Char('.'), 0, 0));
                            route.insert(QStringLiteral("method"), method);
                            route.insert(QStringLiteral("path"), routePath);
                            route.insert(QStringLiteral("label"), method + QStringLiteral(" ") + routePath);
                            routes.append(route);
                        }
                    }
                }
            }
        }
        if (type == QStringLiteral("return_statement") && depth > 0 && ts_node_named_child_count(child) > 0) {
            // A module factory's return value is what the module exports.
            TSNode value = unwrapExpression(ts_node_named_child(child, 0));
            const QString valueType = tsType(value);
            QVariantList members;
            if (valueType == QStringLiteral("object")) {
                members = parseJsObjectMembers(value);
            }
            appendSymbolIfNew(makeSymbol(QStringLiteral("module"), QStringLiteral("module value"), nodeLine(child),
                                         nodeText(value, source).simplified().left(60), members, nodeSnippet(child, source)));
        }
        if (type == QStringLiteral("if_statement")) {
            TSNode consequence = fieldNode(child, "consequence");
            if (tsType(consequence) == QStringLiteral("statement_block")) {
                processContainer(consequence, depth + 1);
            }
            TSNode alternative = fieldNode(child, "alternative");
            if (!ts_node_is_null(alternative) && ts_node_named_child_count(alternative) > 0) {
                TSNode alternativeBody = ts_node_named_child(alternative, 0);
                if (tsType(alternativeBody) == QStringLiteral("statement_block")) {
                    processContainer(alternativeBody, depth + 1);
                }
            }
        } else if (type == QStringLiteral("statement_block")) {
            processContainer(child, depth + 1);
        }
    }
    };
    processContainer(root, 0);

    // Routes anywhere in the tree (tests and wrappers nest them): calls like
    // app.get('/x', ...), router.use('/x', ...), app.route('/x').post(...).
    {
        static const QSet<QString> routeMethods = {
            QStringLiteral("get"), QStringLiteral("post"), QStringLiteral("put"), QStringLiteral("patch"),
            QStringLiteral("delete"), QStringLiteral("del"), QStringLiteral("use"), QStringLiteral("all"),
            QStringLiteral("options"), QStringLiteral("head"),
        };
        QSet<QString> seenRoutes;
        for (const QVariant &existing : std::as_const(routes)) {
            const QVariantMap route = existing.toMap();
            seenRoutes.insert(QStringLiteral("%1|%2|%3").arg(route.value(QStringLiteral("method")).toString(),
                                                            route.value(QStringLiteral("path")).toString())
                                  .arg(route.value(QStringLiteral("line")).toInt()));
        }
        std::function<void(TSNode)> scanRoutes = [&](TSNode node) {
            if (tsType(node) == QStringLiteral("call_expression")) {
                TSNode callee = fieldNode(node, "function");
                if (tsType(callee) == QStringLiteral("member_expression")) {
                    const QString method = nodeText(fieldNode(callee, "property"), source).trimmed();
                    TSNode object = fieldNode(callee, "object");
                    QString routePath;
                    QString owner = nodeText(object, source).trimmed();
                    if (routeMethods.contains(method.toLower())) {
                        TSNode arguments = fieldNode(node, "arguments");
                        TSNode first = ts_node_named_child(arguments, 0);
                        const QString firstType = tsType(first);
                        if (ts_node_named_child_count(arguments) >= 1
                            && (firstType == QStringLiteral("string") || firstType == QStringLiteral("template_string"))) {
                            routePath = nodeValueText(first, source);
                        } else if (tsType(object) == QStringLiteral("call_expression")) {
                            // app.route('/x').get(handler)
                            TSNode inner = fieldNode(object, "function");
                            if (nodeText(fieldNode(inner, "property"), source).trimmed() == QStringLiteral("route")) {
                                routePath = nodeValueText(ts_node_named_child(fieldNode(object, "arguments"), 0), source);
                                owner = nodeText(fieldNode(inner, "object"), source).trimmed();
                            }
                        }
                        // HTTP clients look like route definitions (axios.post('/x')):
                        // known client receivers and interpolated paths are not routes.
                        static const QSet<QString> clientReceivers = {
                            QStringLiteral("axios"), QStringLiteral("$"), QStringLiteral("jQuery"), QStringLiteral("http"),
                            QStringLiteral("this.http"), QStringLiteral("httpClient"), QStringLiteral("this.httpClient"),
                            QStringLiteral("$http"), QStringLiteral("superagent"), QStringLiteral("ky"),
                        };
                        const bool plausiblePath = (routePath.startsWith(QLatin1Char('/')) || routePath == QStringLiteral("*"))
                            && !routePath.contains(QStringLiteral("${")) && !clientReceivers.contains(owner);
                        const bool plausibleOwner = !owner.contains(QLatin1Char('(')) && owner.size() <= 40
                            && owner != QStringLiteral("this") && !owner.contains(QStringLiteral("headers"))
                            && !owner.contains(QStringLiteral("map"), Qt::CaseInsensitive)
                            && !owner.contains(QStringLiteral("cache"), Qt::CaseInsensitive);
                        if (plausiblePath && plausibleOwner) {
                            const QString upper = method.toUpper() == QStringLiteral("DEL") ? QStringLiteral("DELETE") : method.toUpper();
                            const int line = nodeLine(node);
                            const QString key = QStringLiteral("%1|%2|%3").arg(upper, routePath).arg(line);
                            if (!seenRoutes.contains(key)) {
                                seenRoutes.insert(key);
                                QVariantMap route = makeSourceContextItem(path, language, line,
                                                                          nodeSnippet(node, source, 2),
                                                                          QStringLiteral("route"));
                                route.insert(QStringLiteral("owner"), owner);
                                route.insert(QStringLiteral("method"), upper);
                                route.insert(QStringLiteral("path"), routePath);
                                route.insert(QStringLiteral("label"), upper + QStringLiteral(" ") + routePath);
                                routes.append(route);
                            }
                        }
                    }
                }
            }
            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t index = 0; index < childCount; ++index) {
                scanRoutes(ts_node_named_child(node, index));
            }
        };
        scanRoutes(root);
    }

    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), language);
    result.insert(QStringLiteral("analysisHasAstErrors"), hasAstErrors);
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("quickLinks"), findHtmlConsumersForAsset(path, QStringLiteral("script")));

    // Merge Tree-sitter dependencies with regex as fallback
    QVariantList regexDeps = extractDependencyLinks(path, text);
    for (const QVariant &dep : regexDeps) {
        const QVariantMap d = dep.toMap();
        if (!seenDependencies.contains(d.value(QStringLiteral("type")).toString() + QLatin1Char('|') + d.value(QStringLiteral("target")).toString())) {
            dependencies.append(dep);
        }
    }
    result.insert(QStringLiteral("dependencies"), dependencies);

    // Merge Tree-sitter routes with regex as fallback
    QVariantList regexRoutes = extractExpressRoutes(text);
    for (const QVariant &route : regexRoutes) {
        bool found = false;
        const QVariantMap r = route.toMap();
        for (const QVariant &tsRoute : routes) {
            if (tsRoute.toMap().value(QStringLiteral("path")).toString() == r.value(QStringLiteral("path")).toString()
                && tsRoute.toMap().value(QStringLiteral("method")).toString() == r.value(QStringLiteral("method")).toString()) {
                found = true;
                break;
            }
        }
        if (!found) {
            routes.append(route);
        }
    }
    result.insert(QStringLiteral("routes"), routes);
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));

    if (!symbols.isEmpty()) {
        QHash<QString, QVariantMap> byKey;
        QHash<QString, QStringList> keysByName;
        QHash<QString, QVariantList> callsByKey;
        QHash<QString, QVariantList> calledByByKey;
        collectSymbolsByKey(symbols, byKey, keysByName);

        std::function<void(TSNode, const QString &)> visit = [&](TSNode node, const QString &currentKey) {
            if (ts_node_is_null(node)) {
                return;
            }

            QString activeKey = currentKey;
            QString nodeKey = ownerKeyByFunctionStart.value(ts_node_start_byte(node));
            if (!nodeKey.isEmpty() && !isFunctionValue(tsType(node))) {
                nodeKey.clear();
            }
            if (nodeKey.isEmpty() || !byKey.contains(nodeKey)) {
                nodeKey = jsCallableKeyForNode(node, source, byKey, reactMode);
            }
            if (!nodeKey.isEmpty()) {
                activeKey = nodeKey;
            }

            const QString nodeType = tsType(node);
            if (!activeKey.isEmpty()
                && (nodeType == QStringLiteral("call_expression")
                    || nodeType == QStringLiteral("new_expression"))) {
                const QString targetName = jsCallTargetName(node, source);
                const QStringList candidateKeys = keysByName.value(targetName);
                if (!targetName.isEmpty() && !candidateKeys.isEmpty()) {
                    const QString targetKey = bestRelationTargetKey(candidateKeys, byKey);
                    if (targetKey != activeKey && byKey.contains(targetKey)) {
                        appendUniqueRelation(callsByKey, activeKey, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
                        appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(activeKey)), QStringLiteral("called by"));
                    }
                }
            }

            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t index = 0; index < childCount; ++index) {
                visit(ts_node_named_child(node, index), activeKey);
            }
        };

        visit(root, QString());

        std::function<void(const QVariantList &)> collectSnippetRelations = [&](const QVariantList &items) {
            for (const QVariant &entry : items) {
                const QVariantMap symbol = entry.toMap();
                const QString ownerKey = symbolKey(symbol);
                if (!ownerKey.isEmpty()) {
                    const QSet<QString> relationNames = scriptCallNamesFromSnippet(symbol.value(QStringLiteral("snippet")).toString());
                    for (const QString &targetName : relationNames) {
                        const QStringList candidateKeys = keysByName.value(targetName);
                        if (candidateKeys.isEmpty()) {
                            continue;
                        }
                        const QString targetKey = bestRelationTargetKey(candidateKeys, byKey);
                        if (targetKey.isEmpty() || targetKey == ownerKey || !byKey.contains(targetKey)) {
                            continue;
                        }
                        appendUniqueRelation(callsByKey, ownerKey, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
                        appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(ownerKey)), QStringLiteral("called by"));
                    }
                }
                collectSnippetRelations(symbol.value(QStringLiteral("members")).toList());
            }
        };

        collectSnippetRelations(symbols);
        symbols = applyRelationsToSymbols(symbols, callsByKey, calledByByKey);
        result.insert(QStringLiteral("symbols"), symbols);
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    if (QFileInfo(path).fileName() == QStringLiteral("index.js")) {
        const QString packagePath = QFileInfo(QDir(QFileInfo(path).dir()).filePath(QStringLiteral("package.json"))).absoluteFilePath();
        if (QFileInfo::exists(packagePath)) {
            QFile packageFile(packagePath);
            if (packageFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                result.insert(QStringLiteral("packageSummary"),
                              extractPackageSummary(packagePath, QString::fromUtf8(packageFile.readAll())));
            }
        }
    }
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parseCssTreeSitter(const QString &path, const QString &text) const
{
    QVariantList symbols;
    const QByteArray source = text.toUtf8();
    TSLanguage *language = languageForName(QStringLiteral("css"));
    if (!language) {
        return makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("css"));
    }

    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, language)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("css"));
    }

    TSTree *tree = parseAnalysedSource(parser, source);
    TSNode root = ts_tree_root_node(tree);
    const bool hasAstErrors = ts_node_has_error(root);

    QSet<QString> seenClassLines;
    std::function<void(TSNode)> visit = [&](TSNode node) {
        const QString type = tsType(node);
        if (type == QStringLiteral("class_selector")) {
            const QString name = cssClassSelectorName(node, source);
            const TSNode snippetNode = firstAncestorOfType(node, {"rule_set", "block"});
            const TSNode effectiveNode = ts_node_is_null(snippetNode) ? node : snippetNode;
            const int symbolLine = nodeLine(effectiveNode);
            const QString dedupeKey = name + QLatin1Char(':') + QString::number(symbolLine);
            if (!seenClassLines.contains(dedupeKey)) {
                seenClassLines.insert(dedupeKey);
                symbols.append(makeSymbol(QStringLiteral("class"), name, symbolLine, QString(), {}, nodeSnippet(effectiveNode, source)));
            }
        } else if (type == QStringLiteral("property_name")) {
            const QString name = nodeText(node, source);
            if (name.startsWith(QStringLiteral("--"))) {
                const TSNode snippetNode = firstAncestorOfType(node, {"declaration", "block"});
                const TSNode effectiveNode = ts_node_is_null(snippetNode) ? node : snippetNode;
                symbols.append(makeSymbol(QStringLiteral("custom-property"), name, nodeLine(effectiveNode), QString(), {}, nodeSnippet(effectiveNode, source)));
            }
        }

        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            visit(ts_node_named_child(node, i));
        }
    };

    visit(root);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("css"));
    result.insert(QStringLiteral("analysisHasAstErrors"), hasAstErrors);
    result.insert(QStringLiteral("symbols"), symbols);
    if (detectLanguage(path) == QStringLiteral("css")) { // not for inline <style> blocks
        enrichCssAnalysisWithHtmlUsage(result, path, text);
    }
    return result;
}

QVariantMap SymbolParser::parsePython(const QString &path, const QString &text) const
{
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("python"));
    QVariantList symbols;
    QSet<QString> seenNames;

    auto appendSymbol = [&](const QVariantMap &symbol) {
        const QString name = symbol.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || seenNames.contains(name)) {
            return;
        }
        seenNames.insert(name);
        symbols.append(symbol);
    };

    QRegularExpression classPattern(QStringLiteral(R"(^([ \t]*)class\s+([A-Za-z_]\w*)(?:\([^)]*\))?\s*:)"),
                                    QRegularExpression::MultilineOption);
    auto classIt = classPattern.globalMatch(text);
    while (classIt.hasNext()) {
        const auto match = classIt.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        const QString indent = match.captured(1);
        const QString name = match.captured(2);
        QVariantList members;
        QRegularExpression memberPattern(QStringLiteral(R"(^%1[ \t]+def\s+([A-Za-z_]\w*)\s*\()")
                                             .arg(QRegularExpression::escape(indent)),
                                         QRegularExpression::MultilineOption);
        auto memberIt = memberPattern.globalMatch(text.mid(match.capturedEnd(0)));
        while (memberIt.hasNext()) {
            const auto member = memberIt.next();
            const QString memberName = member.captured(1);
            if (memberName.isEmpty()) {
                continue;
            }
            const int memberLine = line + text.mid(match.capturedEnd(0), member.capturedStart(0)).count(QLatin1Char('\n')) + 1;
            members.append(makeSymbol(QStringLiteral("method"), memberName, memberLine, QString(), {},
                                      snippetFromLine(text, memberLine, 1)));
        }
        appendSymbol(makeSymbol(QStringLiteral("class"), name, line, QString(), members,
                                snippetFromLine(text, line, 2)));
    }

    QRegularExpression functionPattern(QStringLiteral(R"(^([ \t]*)def\s+([A-Za-z_]\w*)\s*\()"),
                                       QRegularExpression::MultilineOption);
    auto functionIt = functionPattern.globalMatch(text);
    while (functionIt.hasNext()) {
        const auto match = functionIt.next();
        if (!match.captured(1).isEmpty()) {
            continue;
        }
        const QString name = match.captured(2);
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        appendSymbol(makeSymbol(QStringLiteral("function"), name, line, QString(), {},
                                snippetFromLine(text, line, 2)));
    }

    symbols = applySnippetCallRelations(symbols);

    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), extractPythonDependencies(path, text));
    result.insert(QStringLiteral("routes"), extractPythonRoutes(path, text));
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

// ---------------------------------------------------------------------------
// Python signatures from the syntax tree (issue #1)
//
// Python rarely annotates return types, and echoing the text after `return`
// (the old snippet heuristic) says little and misses every path past the
// snippet's first lines. With an annotation (`-> T`) that is the answer.
// Otherwise every return path in the body is classified into a type where the
// expression makes it evident (literals, constructors, comparisons, builtins),
// grouped by type with its path count and lines, plus an implicit `None` when
// control can fall off the end, and `Generator` for yield.
// ---------------------------------------------------------------------------

static QString pythonCallName(TSNode call, const QByteArray &source)
{
    const QString function = nodeText(fieldNode(call, "function"), source).trimmed();
    return function;
}

static QStringList pythonExpressionTypes(TSNode node, const QByteArray &source)
{
    const QString type = tsType(node);
    if (type.isEmpty()) {
        return {QStringLiteral("None")};
    }
    if (type == QStringLiteral("parenthesized_expression") && ts_node_named_child_count(node) == 1) {
        return pythonExpressionTypes(ts_node_named_child(node, 0), source);
    }
    if (type == QStringLiteral("none")) return {QStringLiteral("None")};
    if (type == QStringLiteral("true") || type == QStringLiteral("false")
        || type == QStringLiteral("comparison_operator") || type == QStringLiteral("not_operator")) {
        return {QStringLiteral("bool")};
    }
    if (type == QStringLiteral("integer")) return {QStringLiteral("int")};
    if (type == QStringLiteral("float")) return {QStringLiteral("float")};
    if (type == QStringLiteral("string") || type == QStringLiteral("concatenated_string")) {
        const QString text = nodeText(node, source).trimmed();
        if (text.startsWith(QLatin1Char('b')) || text.startsWith(QLatin1Char('B'))) {
            return {QStringLiteral("bytes")};
        }
        return {QStringLiteral("str")};
    }
    if (type == QStringLiteral("list") || type == QStringLiteral("list_comprehension")) return {QStringLiteral("list")};
    if (type == QStringLiteral("dictionary") || type == QStringLiteral("dictionary_comprehension")) return {QStringLiteral("dict")};
    if (type == QStringLiteral("set") || type == QStringLiteral("set_comprehension")) return {QStringLiteral("set")};
    if (type == QStringLiteral("tuple") || type == QStringLiteral("expression_list")) return {QStringLiteral("tuple")};
    if (type == QStringLiteral("generator_expression")) return {QStringLiteral("Generator")};
    if (type == QStringLiteral("lambda")) return {QStringLiteral("Callable")};
    if (type == QStringLiteral("conditional_expression")) {
        QStringList merged;
        const uint32_t count = ts_node_named_child_count(node);
        // `a if cond else b`: children are a, cond, b.
        for (uint32_t index = 0; index < count; index += 2) {
            for (const QString &part : pythonExpressionTypes(ts_node_named_child(node, index), source)) {
                if (!merged.contains(part)) {
                    merged.append(part);
                }
            }
        }
        return merged;
    }
    if (type == QStringLiteral("await")) {
        const QStringList inner = ts_node_named_child_count(node) > 0
            ? pythonExpressionTypes(ts_node_named_child(node, 0), source)
            : QStringList{};
        return inner.isEmpty() ? QStringList{QStringLiteral("awaited value")} : inner;
    }
    if (type == QStringLiteral("call")) {
        const QString name = pythonCallName(node, source);
        const QString last = name.section(QLatin1Char('.'), -1);
        static const QHash<QString, QString> builtins = {
            {QStringLiteral("str"), QStringLiteral("str")}, {QStringLiteral("repr"), QStringLiteral("str")},
            {QStringLiteral("int"), QStringLiteral("int")}, {QStringLiteral("len"), QStringLiteral("int")},
            {QStringLiteral("float"), QStringLiteral("float")}, {QStringLiteral("bool"), QStringLiteral("bool")},
            {QStringLiteral("isinstance"), QStringLiteral("bool")}, {QStringLiteral("hasattr"), QStringLiteral("bool")},
            {QStringLiteral("list"), QStringLiteral("list")}, {QStringLiteral("sorted"), QStringLiteral("list")},
            {QStringLiteral("dict"), QStringLiteral("dict")}, {QStringLiteral("set"), QStringLiteral("set")},
            {QStringLiteral("frozenset"), QStringLiteral("frozenset")}, {QStringLiteral("tuple"), QStringLiteral("tuple")},
            {QStringLiteral("bytes"), QStringLiteral("bytes")}, {QStringLiteral("iter"), QStringLiteral("Iterator")},
            {QStringLiteral("format"), QStringLiteral("str")}, {QStringLiteral("join"), QStringLiteral("str")},
        };
        if (name == last && builtins.contains(last)) {
            return {builtins.value(last)};
        }
        if (name != last && (last == QStringLiteral("join") || last == QStringLiteral("format")
                             || last == QStringLiteral("strip") || last == QStringLiteral("lower")
                             || last == QStringLiteral("upper") || last == QStringLiteral("replace"))) {
            return {QStringLiteral("str")};
        }
        if (!last.isEmpty() && last.at(0).isUpper()) {
            return {last};
        }
        return {QStringLiteral("result of %1()").arg(name.size() > 40 ? last : name)};
    }
    if (type == QStringLiteral("identifier") || type == QStringLiteral("attribute")) {
        const QString text = nodeText(node, source).trimmed();
        if (text == QStringLiteral("self")) {
            return {QStringLiteral("self")};
        }
        if (text == QStringLiteral("NotImplemented")) {
            return {QStringLiteral("NotImplemented")};
        }
        return {QStringLiteral("value of %1").arg(text)};
    }
    if (type == QStringLiteral("subscript")) {
        const QString container = nodeText(fieldNode(node, "value"), source).trimmed();
        return {container.size() <= 40 ? QStringLiteral("item of %1").arg(container) : QStringLiteral("item")};
    }
    if (type == QStringLiteral("binary_operator")) {
        const QStringList left = pythonExpressionTypes(fieldNode(node, "left"), source);
        const QStringList right = pythonExpressionTypes(fieldNode(node, "right"), source);
        if (left == right && left.size() == 1
            && (left.first() == QStringLiteral("str") || left.first() == QStringLiteral("int")
                || left.first() == QStringLiteral("float") || left.first() == QStringLiteral("list"))) {
            return left;
        }
        return {QStringLiteral("expression")};
    }
    return {QStringLiteral("expression")};
}

// Whether control can reach the end of a statement block (very conservative:
// only a trailing return/raise, or an if/else chain whose every branch ends
// that way, counts as terminating).
static bool pythonBlockTerminates(TSNode block)
{
    const uint32_t count = ts_node_named_child_count(block);
    if (count == 0) {
        return false;
    }
    TSNode last = ts_node_named_child(block, count - 1);
    const QString type = tsType(last);
    if (type == QStringLiteral("return_statement") || type == QStringLiteral("raise_statement")) {
        return true;
    }
    if (type == QStringLiteral("if_statement")) {
        bool hasElse = false;
        if (!pythonBlockTerminates(fieldNode(last, "consequence"))) {
            return false;
        }
        const uint32_t childCount = ts_node_named_child_count(last);
        for (uint32_t index = 0; index < childCount; ++index) {
            TSNode clause = ts_node_named_child(last, index);
            const QString clauseType = tsType(clause);
            if (clauseType == QStringLiteral("elif_clause")) {
                if (!pythonBlockTerminates(fieldNode(clause, "consequence"))) {
                    return false;
                }
            } else if (clauseType == QStringLiteral("else_clause")) {
                hasElse = true;
                if (!pythonBlockTerminates(fieldNode(clause, "body"))) {
                    return false;
                }
            }
        }
        return hasElse;
    }
    if (type == QStringLiteral("while_statement")) {
        const QString condition = tsType(fieldNode(last, "condition"));
        return condition == QStringLiteral("true");
    }
    if (type == QStringLiteral("try_statement") || type == QStringLiteral("with_statement")) {
        TSNode body = fieldNode(last, "body");
        return !ts_node_is_null(body) && pythonBlockTerminates(body);
    }
    return false;
}

static void collectPythonReturns(TSNode node, QList<TSNode> &returns, bool &yields)
{
    const QString type = tsType(node);
    if (type == QStringLiteral("function_definition") || type == QStringLiteral("class_definition")
        || type == QStringLiteral("lambda") || type == QStringLiteral("decorated_definition")) {
        return;
    }
    if (type == QStringLiteral("return_statement")) {
        returns.append(node);
    } else if (type == QStringLiteral("yield")) {
        yields = true;
    }
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t index = 0; index < count; ++index) {
        collectPythonReturns(ts_node_named_child(node, index), returns, yields);
    }
}

static QVariantList pythonReturnsForFunction(TSNode function, const QByteArray &source)
{
    QVariantList returns;
    const QString annotation = nodeText(fieldNode(function, "return_type"), source).simplified();
    if (!annotation.isEmpty()) {
        returns.append(QVariantMap{{QStringLiteral("text"), annotation},
                                   {QStringLiteral("source"), QStringLiteral("annotation")}});
        return returns;
    }

    TSNode body = fieldNode(function, "body");
    QList<TSNode> statements;
    bool yields = false;
    const uint32_t count = ts_node_named_child_count(body);
    for (uint32_t index = 0; index < count; ++index) {
        collectPythonReturns(ts_node_named_child(body, index), statements, yields);
    }

    if (yields) {
        returns.append(QVariantMap{{QStringLiteral("text"), QStringLiteral("Generator (yields)")},
                                   {QStringLiteral("source"), QStringLiteral("inferred")}});
        return returns;
    }

    QStringList order;
    QHash<QString, QList<int>> linesByType;
    for (const TSNode &statement : std::as_const(statements)) {
        const QStringList types = ts_node_named_child_count(statement) > 0
            ? pythonExpressionTypes(ts_node_named_child(statement, 0), source)
            : QStringList{QStringLiteral("None")};
        for (const QString &type : types) {
            if (!linesByType.contains(type)) {
                order.append(type);
            }
            linesByType[type].append(nodeLine(statement));
        }
    }
    const bool fallsThrough = !pythonBlockTerminates(body);
    if (fallsThrough) {
        const QString implicitNone = QStringLiteral("None");
        if (!linesByType.contains(implicitNone)) {
            order.append(implicitNone);
        }
        linesByType[implicitNone].append(-1);
    }

    const int totalPaths = statements.size() + (fallsThrough ? 1 : 0);
    for (const QString &type : std::as_const(order)) {
        const QList<int> lines = linesByType.value(type);
        QStringList lineTexts;
        bool implicit = false;
        for (int line : lines) {
            if (line < 0) {
                implicit = true;
            } else {
                lineTexts.append(QString::number(line));
            }
        }
        QString text = type;
        QStringList notes;
        if (!lineTexts.isEmpty() && totalPaths > 1) {
            notes.append(lineTexts.size() == 1 ? QStringLiteral("line %1").arg(lineTexts.first())
                                               : QStringLiteral("lines %1").arg(lineTexts.join(QStringLiteral(", "))));
        }
        if (implicit) {
            notes.append(statements.isEmpty() ? QStringLiteral("no return statement")
                                              : QStringLiteral("falls off the end"));
        }
        if (!notes.isEmpty()) {
            text += QStringLiteral(" (%1)").arg(notes.join(QStringLiteral("; ")));
        }
        QVariantList lineValues;
        for (int line : lines) {
            if (line > 0) {
                lineValues.append(line);
            }
        }
        returns.append(QVariantMap{{QStringLiteral("text"), text},
                                   {QStringLiteral("type"), type},
                                   {QStringLiteral("source"), QStringLiteral("inferred")},
                                   {QStringLiteral("lines"), lineValues}});
    }
    if (returns.size() > 1) {
        // Lead with a one-line summary so the inspector says up front that
        // there are several possible outcomes.
        QStringList typeNames;
        for (const QString &type : std::as_const(order)) {
            typeNames.append(type);
        }
        returns.prepend(QVariantMap{{QStringLiteral("text"),
                                     QStringLiteral("%1 (%2 return paths)").arg(typeNames.join(QStringLiteral(" | ")))
                                         .arg(totalPaths)},
                                    {QStringLiteral("source"), QStringLiteral("inferred")},
                                    {QStringLiteral("summary"), true}});
    }
    return returns;
}

static QVariantList pythonParametersForFunction(TSNode function, const QByteArray &source, bool isMethod)
{
    QVariantList parameters;
    TSNode params = fieldNode(function, "parameters");
    const uint32_t count = ts_node_named_child_count(params);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode param = ts_node_named_child(params, index);
        const QString type = tsType(param);
        QString name;
        QString annotation;
        QString defaultValue;
        if (type == QStringLiteral("identifier")) {
            name = nodeText(param, source);
        } else if (type == QStringLiteral("typed_parameter")) {
            name = nodeText(ts_node_named_child(param, 0), source);
            annotation = nodeText(fieldNode(param, "type"), source);
        } else if (type == QStringLiteral("default_parameter")) {
            name = nodeText(fieldNode(param, "name"), source);
            defaultValue = nodeText(fieldNode(param, "value"), source);
        } else if (type == QStringLiteral("typed_default_parameter")) {
            name = nodeText(fieldNode(param, "name"), source);
            annotation = nodeText(fieldNode(param, "type"), source);
            defaultValue = nodeText(fieldNode(param, "value"), source);
        } else if (type == QStringLiteral("list_splat_pattern") || type == QStringLiteral("dictionary_splat_pattern")) {
            name = nodeText(param, source);
        } else {
            continue; // keyword_separator `*`, positional_separator `/`, comments
        }
        name = name.simplified();
        if (name.isEmpty()) {
            continue;
        }
        if (isMethod && parameters.isEmpty() && index == 0
            && (name == QStringLiteral("self") || name == QStringLiteral("cls"))) {
            continue;
        }
        QVariantMap item = makeSignatureParameter(name, annotation.simplified());
        if (!defaultValue.isEmpty()) {
            item.insert(QStringLiteral("default"), defaultValue.simplified());
        }
        parameters.append(item);
    }
    return parameters;
}

static QVariantMap withPythonSignature(QVariantMap symbol, TSNode function, const QByteArray &source, bool isMethod)
{
    symbol.insert(QStringLiteral("parameters"), pythonParametersForFunction(function, source, isMethod));
    symbol.insert(QStringLiteral("returns"), pythonReturnsForFunction(function, source));
    symbol.insert(QStringLiteral("signatureSource"), QStringLiteral("ast"));
    return symbol;
}

QVariantMap SymbolParser::parsePythonTreeSitter(const QString &path, const QString &text) const
{
    QVariantList symbols;
    const QByteArray source = text.toUtf8();
    TSLanguage *language = languageForName(QStringLiteral("python"));
    if (!language) {
        return makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("python"));
    }

    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, language)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("python"));
    }

    TSTree *tree = parseAnalysedSource(parser, source);
    TSNode root = ts_tree_root_node(tree);
    const bool hasAstErrors = ts_node_has_error(root);

    auto appendUnique = [&](const QVariantMap &symbol) {
        const QString name = symbol.value(QStringLiteral("name")).toString();
        const QString kind = symbol.value(QStringLiteral("kind")).toString();
        for (const QVariant &existingValue : std::as_const(symbols)) {
            const QVariantMap existing = existingValue.toMap();
            if (existing.value(QStringLiteral("name")).toString() == name
                && existing.value(QStringLiteral("kind")).toString() == kind
                && existing.value(QStringLiteral("line")).toInt() == symbol.value(QStringLiteral("line")).toInt()) {
                return;
            }
        }
        symbols.append(symbol);
    };

    std::function<QVariantMap(TSNode, TSNode, const QString &)> parsePythonDefinition;
    std::function<QVariantList(TSNode)> parsePythonClassMembers;

    auto decorationDetail = [&](TSNode decoratedNode) {
        QStringList decorators;
        const uint32_t count = ts_node_named_child_count(decoratedNode);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(decoratedNode, i);
            if (tsType(child) == QStringLiteral("decorator")) {
                decorators.append(nodeText(child, source).trimmed());
            }
        }
        if (decorators.isEmpty()) {
            return QStringLiteral("decorated");
        }
        return decorators.join(QStringLiteral(", "));
    };

    parsePythonClassMembers = [&](TSNode blockNode) {
        QVariantList members;
        const uint32_t count = ts_node_named_child_count(blockNode);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(blockNode, i);
            TSNode snippetNode = child;
            QString detail;
            QString childType = tsType(child);
            if (childType == QStringLiteral("decorated_definition")) {
                detail = decorationDetail(child);
                child = fieldNode(child, "definition");
                childType = tsType(child);
            }
            if (childType != QStringLiteral("function_definition")) {
                continue;
            }
            const QString name = nodeText(fieldNode(child, "name"), source);
            if (name.isEmpty()) {
                continue;
            }
            QString kind = QStringLiteral("method");
            if (detail.contains(QStringLiteral("@property"))) {
                kind = QStringLiteral("property");
            }
            members.append(withPythonSignature(makeSymbol(kind, name, nodeLine(child), detail, {},
                                                          nodeSnippet(snippetNode, source)),
                                               child, source, true));
        }
        return members;
    };

    parsePythonDefinition = [&](TSNode rawNode, TSNode snippetNode, const QString &detail) -> QVariantMap {
        const QString type = tsType(rawNode);
        if (type == QStringLiteral("class_definition")) {
            const QString name = nodeText(fieldNode(rawNode, "name"), source);
            const TSNode body = fieldNode(rawNode, "body");
            return makeSymbol(QStringLiteral("class"), name, nodeLine(rawNode), detail,
                              parsePythonClassMembers(body), nodeSnippet(snippetNode, source));
        }
        if (type == QStringLiteral("function_definition")) {
            const QString name = nodeText(fieldNode(rawNode, "name"), source);
            return withPythonSignature(makeSymbol(QStringLiteral("function"), name, nodeLine(rawNode), detail, {},
                                                  nodeSnippet(snippetNode, source)),
                                       rawNode, source, false);
        }
        return {};
    };

    const uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_named_child(root, i);
        const QString type = tsType(child);
        if (type == QStringLiteral("class_definition") || type == QStringLiteral("function_definition")) {
            appendUnique(parsePythonDefinition(child, child, QString()));
            continue;
        }
        if (type == QStringLiteral("decorated_definition")) {
            TSNode definition = fieldNode(child, "definition");
            appendUnique(parsePythonDefinition(definition, child, decorationDetail(child)));
        }
    }

    if (!symbols.isEmpty()) {
        QHash<QString, QVariantMap> byKey;
        QHash<QString, QStringList> keysByName;
        QHash<QString, QVariantList> callsByKey;
        QHash<QString, QVariantList> calledByByKey;
        collectSymbolsByKey(symbols, byKey, keysByName);

        std::function<void(TSNode, const QString &)> visit = [&](TSNode node, const QString &currentKey) {
            QString activeKey = currentKey;
            const QString nodeKey = pythonCallableKeyForNode(node, source, byKey);
            if (!nodeKey.isEmpty()) {
                activeKey = nodeKey;
            }

            if (!activeKey.isEmpty() && tsType(node) == QStringLiteral("call")) {
                const QString targetName = pythonCallTargetName(node, source);
                const QStringList candidateKeys = keysByName.value(targetName);
                if (!targetName.isEmpty() && !candidateKeys.isEmpty()) {
                    const QString targetKey = bestRelationTargetKey(candidateKeys, byKey);
                    if (targetKey != activeKey && byKey.contains(targetKey)) {
                        appendUniqueRelation(callsByKey, activeKey, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
                        appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(activeKey)), QStringLiteral("called by"));
                    }
                }
            }

            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t index = 0; index < childCount; ++index) {
                visit(ts_node_named_child(node, index), activeKey);
            }
        };

        visit(root, QString());
        symbols = applyRelationsToSymbols(symbols, callsByKey, calledByByKey);
    }

    symbols = applySnippetCallRelations(symbols);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("python"));
    result.insert(QStringLiteral("analysisHasAstErrors"), hasAstErrors);
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), extractPythonDependencies(path, text));
    result.insert(QStringLiteral("routes"), extractPythonRoutes(path, text));
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parseCppLike(const QString &path, const QString &text, const QString &language) const
{
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), language);
    QVariantList symbols;
    QSet<QString> seenNames;

    auto appendSymbol = [&](const QVariantMap &symbol) {
        const QString name = symbol.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || seenNames.contains(name)) {
            return;
        }
        seenNames.insert(name);
        symbols.append(symbol);
    };

    QRegularExpression classPattern(QStringLiteral(R"(^\s*(class|struct)\s+([A-Za-z_]\w*))"),
                                    QRegularExpression::MultilineOption);
    auto classIt = classPattern.globalMatch(text);
    while (classIt.hasNext()) {
        const auto match = classIt.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        appendSymbol(makeSymbol(match.captured(1) == QStringLiteral("struct") ? QStringLiteral("struct")
                                                                              : QStringLiteral("class"),
                                match.captured(2), line, QString(), {},
                                snippetFromBraceBlock(text, match.capturedStart(0))));
    }

    QRegularExpression functionPattern(
        QStringLiteral(R"(^\s*(?:template\s*<[^>]+>\s*)?(?:inline\s+|static\s+|virtual\s+|constexpr\s+|friend\s+)?(?:[\w:<>~*&]+\s+)+([A-Za-z_~]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:\{|$))"),
        QRegularExpression::MultilineOption);
    auto functionIt = functionPattern.globalMatch(text);
    while (functionIt.hasNext()) {
        const auto match = functionIt.next();
        const QString name = match.captured(1);
        if (name == QStringLiteral("if") || name == QStringLiteral("while") || name == QStringLiteral("switch")
            || name == QStringLiteral("for") || name == QStringLiteral("catch")) {
            continue;
        }
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        appendSymbol(makeSymbol(QStringLiteral("function"), name, line, QString(), {},
                                snippetFromBraceBlock(text, match.capturedStart(0))));
    }

    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), extractCppDependencies(path, text));
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parseJava(const QString &path, const QString &text) const
{
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("java"));
    QVariantList symbols;

    QRegularExpression classPattern(QStringLiteral(R"(^\s*(?:public|protected|private|abstract|final)?\s*(class|interface|enum)\s+([A-Za-z_]\w*))"),
                                    QRegularExpression::MultilineOption);
    auto classIt = classPattern.globalMatch(text);
    while (classIt.hasNext()) {
        const auto match = classIt.next();
        const QString kind = match.captured(1) == QStringLiteral("interface") ? QStringLiteral("interface")
                               : (match.captured(1) == QStringLiteral("enum") ? QStringLiteral("enum")
                                                                               : QStringLiteral("class"));
        const QString name = match.captured(2);
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        QVariantList members;
        QRegularExpression methodPattern(
            QStringLiteral(R"(^\s*(?:public|protected|private|static|final|abstract|synchronized|native|\s)+[\w<>\[\],.?]+\s+([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:throws\s+[^{]+)?\{)"),
            QRegularExpression::MultilineOption);
        auto methodIt = methodPattern.globalMatch(text);
        while (methodIt.hasNext()) {
            const auto method = methodIt.next();
            const QString methodName = method.captured(1);
            if (methodName == name) {
                continue;
            }
            const int methodLine = lineNumberAtOffset(text, method.capturedStart(0));
            members.append(makeSymbol(QStringLiteral("method"), methodName, methodLine, QString(), {},
                                      snippetFromBraceBlock(text, method.capturedStart(0))));
        }
        symbols.append(makeSymbol(kind, name, line, QString(), members,
                                  snippetFromBraceBlock(text, match.capturedStart(0))));
    }

    symbols = applySnippetCallRelations(symbols);

    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), extractJavaDependencies(text));
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parseJavaTreeSitter(const QString &path, const QString &text) const
{
    QVariantList symbols;
    QVariantList dependencies;
    QSet<QString> seenSymbols;
    QSet<QString> seenDependencies;
    const QByteArray source = text.toUtf8();
    TSLanguage *language = languageForName(QStringLiteral("java"));
    if (!language) {
        return makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("java"));
    }

    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, language)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("java"));
    }

    TSTree *tree = parseAnalysedSource(parser, source);
    TSNode root = ts_tree_root_node(tree);
    const bool hasAstErrors = ts_node_has_error(root);

    auto hasModifier = [&](TSNode node, const QString &modifier) {
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            if (tsType(child) == QStringLiteral("modifiers")
                && nodeText(child, source).contains(modifier)) {
                return true;
            }
        }
        return false;
    };

    auto symbolDetail = [&](TSNode node) {
        QStringList details;
        if (hasModifier(node, QStringLiteral("public"))) {
            details.append(QStringLiteral("public"));
        } else if (hasModifier(node, QStringLiteral("protected"))) {
            details.append(QStringLiteral("protected"));
        } else if (hasModifier(node, QStringLiteral("private"))) {
            details.append(QStringLiteral("private"));
        }
        if (hasModifier(node, QStringLiteral("abstract"))) {
            details.append(QStringLiteral("abstract"));
        }
        if (hasModifier(node, QStringLiteral("static"))) {
            details.append(QStringLiteral("static"));
        }
        return details.join(QStringLiteral(", "));
    };

    auto appendSymbol = [&](const QVariantMap &symbol) {
        const QString name = symbol.value(QStringLiteral("name")).toString();
        const QString kind = symbol.value(QStringLiteral("kind")).toString();
        const int line = symbol.value(QStringLiteral("line")).toInt();
        if (name.isEmpty()) {
            return;
        }
        const QString key = QStringLiteral("%1|%2|%3").arg(kind, name).arg(line);
        if (seenSymbols.contains(key)) {
            return;
        }
        seenSymbols.insert(key);
        symbols.append(symbol);
    };

    auto appendDependency = [&](TSNode node) {
        QString target = nodeText(node, source).trimmed();
        if (target.startsWith(QStringLiteral("import "))) {
            target.remove(0, 7);
        }
        if (target.startsWith(QStringLiteral("static "))) {
            target.remove(0, 7);
        }
        if (target.endsWith(QLatin1Char(';'))) {
            target.chop(1);
        }
        target = target.trimmed();
        if (target.isEmpty() || seenDependencies.contains(target)) {
            return;
        }
        seenDependencies.insert(target);

        QVariantMap item = makeSourceContextItem(path, QStringLiteral("java"), nodeLine(node),
                                                 nodeSnippet(node, source, 2),
                                                 QStringLiteral("import"));
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), QStringLiteral("import"));
        item.insert(QStringLiteral("label"), target);
        item.insert(QStringLiteral("path"), QString());
        item.insert(QStringLiteral("exists"), true);
        dependencies.append(item);
    };

    auto fieldMembers = [&](TSNode node, const QString &kind) {
        QVariantList members;
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            if (tsType(child) != QStringLiteral("variable_declarator")) {
                continue;
            }
            const QString name = nodeText(fieldNode(child, "name"), source);
            if (name.isEmpty()) {
                continue;
            }
            members.append(makeSymbol(kind, name, nodeLine(child), symbolDetail(node), {}, nodeSnippet(node, source)));
        }
        return members;
    };

    std::function<QVariantList(TSNode)> parseJavaMembers = [&](TSNode bodyNode) {
        QVariantList members;
        if (ts_node_is_null(bodyNode)) {
            return members;
        }
        const uint32_t count = ts_node_named_child_count(bodyNode);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(bodyNode, i);
            const QString type = tsType(child);
            if (type == QStringLiteral("method_declaration")) {
                members.append(makeSymbol(QStringLiteral("method"),
                                          nodeText(fieldNode(child, "name"), source),
                                          nodeLine(child), symbolDetail(child), {}, nodeSnippet(child, source)));
            } else if (type == QStringLiteral("constructor_declaration")) {
                members.append(makeSymbol(QStringLiteral("constructor"),
                                          nodeText(fieldNode(child, "name"), source),
                                          nodeLine(child), symbolDetail(child), {}, nodeSnippet(child, source)));
            } else if (type == QStringLiteral("field_declaration")) {
                const QVariantList fields = fieldMembers(child, QStringLiteral("property"));
                for (const QVariant &field : fields) {
                    members.append(field);
                }
            } else if (type == QStringLiteral("constant_declaration")) {
                const QVariantList constants = fieldMembers(child, QStringLiteral("constant"));
                for (const QVariant &constant : constants) {
                    members.append(constant);
                }
            } else if (type == QStringLiteral("enum_constant")) {
                members.append(makeSymbol(QStringLiteral("variant"), nodeText(fieldNode(child, "name"), source),
                                          nodeLine(child), QString(), {}, nodeSnippet(child, source)));
            } else if (type == QStringLiteral("class_declaration")
                       || type == QStringLiteral("interface_declaration")
                       || type == QStringLiteral("enum_declaration")
                       || type == QStringLiteral("record_declaration")) {
                QString nestedKind = QStringLiteral("class");
                if (type == QStringLiteral("interface_declaration")) {
                    nestedKind = QStringLiteral("interface");
                } else if (type == QStringLiteral("enum_declaration")) {
                    nestedKind = QStringLiteral("enum");
                } else if (type == QStringLiteral("record_declaration")) {
                    nestedKind = QStringLiteral("record");
                }
                members.append(makeSymbol(nestedKind, nodeText(fieldNode(child, "name"), source),
                                          nodeLine(child), symbolDetail(child), {}, nodeSnippet(child, source)));
            }
        }
        return members;
    };

    auto appendTypeSymbol = [&](TSNode child, const QString &kind) {
        appendSymbol(makeSymbol(kind,
                                nodeText(fieldNode(child, "name"), source),
                                nodeLine(child),
                                symbolDetail(child),
                                parseJavaMembers(fieldNode(child, "body")),
                                nodeSnippet(child, source)));
    };

    const uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_named_child(root, i);
        const QString type = tsType(child);
        if (type == QStringLiteral("import_declaration")) {
            appendDependency(child);
        } else if (type == QStringLiteral("class_declaration")) {
            appendTypeSymbol(child, QStringLiteral("class"));
        } else if (type == QStringLiteral("interface_declaration")) {
            appendTypeSymbol(child, QStringLiteral("interface"));
        } else if (type == QStringLiteral("enum_declaration")) {
            appendTypeSymbol(child, QStringLiteral("enum"));
        } else if (type == QStringLiteral("record_declaration")) {
            appendTypeSymbol(child, QStringLiteral("record"));
        }
    }

    if (!symbols.isEmpty()) {
        QHash<QString, QVariantMap> byKey;
        QHash<QString, QStringList> keysByName;
        QHash<QString, QVariantList> callsByKey;
        QHash<QString, QVariantList> calledByByKey;
        collectSymbolsByKey(symbols, byKey, keysByName);

        std::function<void(TSNode, const QString &)> visit = [&](TSNode node, const QString &currentKey) {
            QString activeKey = currentKey;
            const QString nodeKey = javaCallableKeyForNode(node, source, byKey);
            if (!nodeKey.isEmpty()) {
                activeKey = nodeKey;
            }

            const QString nodeType = tsType(node);
            if (!activeKey.isEmpty()
                && (nodeType == QStringLiteral("method_invocation")
                    || nodeType == QStringLiteral("object_creation_expression"))) {
                const QString targetName = javaCallTargetName(node, source);
                const QStringList candidateKeys = keysByName.value(targetName);
                if (!targetName.isEmpty() && !candidateKeys.isEmpty()) {
                    const QString targetKey = bestRelationTargetKey(candidateKeys, byKey);
                    if (targetKey != activeKey && byKey.contains(targetKey)) {
                        appendUniqueRelation(callsByKey, activeKey, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
                        appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(activeKey)), QStringLiteral("called by"));
                    }
                }
            }

            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t index = 0; index < childCount; ++index) {
                visit(ts_node_named_child(node, index), activeKey);
            }
        };

        visit(root, QString());
        symbols = applyRelationsToSymbols(symbols, callsByKey, calledByByKey);
    }

    symbols = applySnippetCallRelations(symbols);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("java"));
    result.insert(QStringLiteral("analysisHasAstErrors"), hasAstErrors);
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"),
                  dependencies.isEmpty() ? extractJavaDependencies(text) : dependencies);
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parseCSharp(const QString &path, const QString &text) const
{
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("csharp"));
    QVariantList symbols;
    QSet<QString> seenNames;

    auto appendSymbol = [&](const QVariantMap &symbol) {
        const QString name = symbol.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || seenNames.contains(name)) {
            return;
        }
        seenNames.insert(name);
        symbols.append(symbol);
    };

    QRegularExpression typePattern(
        QStringLiteral(R"(^\s*(?:public|protected|private|internal|abstract|sealed|partial|static|\s)*(class|interface|record|struct|enum)\s+([A-Za-z_]\w*))"),
        QRegularExpression::MultilineOption);
    auto typeIt = typePattern.globalMatch(text);
    while (typeIt.hasNext()) {
        const auto match = typeIt.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        appendSymbol(makeSymbol(match.captured(1), match.captured(2), line, QString(), {},
                                snippetFromBraceBlock(text, match.capturedStart(0))));
    }

    QRegularExpression methodPattern(
        QStringLiteral(R"(^\s*(?:public|protected|private|internal|static|async|virtual|override|sealed|partial|\s)+[\w<>\[\],.?]+\s+([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{)"),
        QRegularExpression::MultilineOption);
    auto methodIt = methodPattern.globalMatch(text);
    while (methodIt.hasNext()) {
        const auto match = methodIt.next();
        const QString name = match.captured(1);
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        appendSymbol(makeSymbol(QStringLiteral("method"), name, line, QString(), {},
                                snippetFromBraceBlock(text, match.capturedStart(0))));
    }

    QRegularExpression topLevelVarPattern(QStringLiteral(R"(^\s*var\s+([A-Za-z_]\w*)\s*=)"),
                                          QRegularExpression::MultilineOption);
    auto varIt = topLevelVarPattern.globalMatch(text);
    while (varIt.hasNext()) {
        const auto match = varIt.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        appendSymbol(makeSymbol(QStringLiteral("variable"), match.captured(1), line, QString(), {},
                                snippetFromLine(text, line, 1)));
    }

    symbols = applySnippetCallRelations(symbols);

    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), extractCSharpDependencies(text));
    result.insert(QStringLiteral("routes"), extractAspNetRoutes(path, text));
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parseCSharpTreeSitter(const QString &path, const QString &text) const
{
    QVariantList symbols;
    QVariantList dependencies;
    QSet<QString> seenSymbols;
    QSet<QString> seenDependencies;
    const QByteArray source = text.toUtf8();
    TSLanguage *language = languageForName(QStringLiteral("csharp"));
    if (!language) {
        return makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("csharp"));
    }

    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, language)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("csharp"));
    }

    TSTree *tree = parseAnalysedSource(parser, source);
    TSNode root = ts_tree_root_node(tree);
    const bool hasAstErrors = ts_node_has_error(root);

    auto hasModifier = [&](TSNode node, const QString &modifier) {
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            if (tsType(child) == QStringLiteral("modifier")
                && nodeText(child, source) == modifier) {
                return true;
            }
        }
        return false;
    };

    auto symbolDetail = [&](TSNode node) {
        QStringList details;
        if (hasModifier(node, QStringLiteral("public"))) {
            details.append(QStringLiteral("public"));
        } else if (hasModifier(node, QStringLiteral("protected"))) {
            details.append(QStringLiteral("protected"));
        } else if (hasModifier(node, QStringLiteral("private"))) {
            details.append(QStringLiteral("private"));
        } else if (hasModifier(node, QStringLiteral("internal"))) {
            details.append(QStringLiteral("internal"));
        }
        if (hasModifier(node, QStringLiteral("static"))) {
            details.append(QStringLiteral("static"));
        }
        if (hasModifier(node, QStringLiteral("abstract"))) {
            details.append(QStringLiteral("abstract"));
        }
        return details.join(QStringLiteral(", "));
    };

    auto appendSymbol = [&](const QVariantMap &symbol) {
        const QString name = symbol.value(QStringLiteral("name")).toString();
        const QString kind = symbol.value(QStringLiteral("kind")).toString();
        const int line = symbol.value(QStringLiteral("line")).toInt();
        if (name.isEmpty()) {
            return;
        }
        const QString key = QStringLiteral("%1|%2|%3").arg(kind, name).arg(line);
        if (seenSymbols.contains(key)) {
            return;
        }
        seenSymbols.insert(key);
        symbols.append(symbol);
    };

    auto appendDependency = [&](TSNode node) {
        QString target = nodeText(node, source).trimmed();
        if (target.startsWith(QStringLiteral("using "))) {
            target.remove(0, 6);
        }
        if (target.endsWith(QLatin1Char(';'))) {
            target.chop(1);
        }
        target = target.trimmed();
        if (target.isEmpty() || seenDependencies.contains(target)) {
            return;
        }
        seenDependencies.insert(target);
        QVariantMap item = makeSourceContextItem(path, QStringLiteral("csharp"), nodeLine(node),
                                                 nodeSnippet(node, source, 2),
                                                 QStringLiteral("using"));
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), QStringLiteral("using"));
        item.insert(QStringLiteral("label"), target);
        item.insert(QStringLiteral("path"), QString());
        item.insert(QStringLiteral("exists"), true);
        dependencies.append(item);
    };

    std::function<QVariantList(TSNode)> parseCSharpMembers = [&](TSNode bodyNode) {
        QVariantList members;
        if (ts_node_is_null(bodyNode)) {
            return members;
        }
        const uint32_t count = ts_node_named_child_count(bodyNode);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(bodyNode, i);
            const QString type = tsType(child);
            if (type == QStringLiteral("method_declaration")) {
                members.append(makeSymbol(QStringLiteral("method"),
                                          nodeText(fieldNode(child, "name"), source),
                                          nodeLine(child), symbolDetail(child), {}, nodeSnippet(child, source)));
            } else if (type == QStringLiteral("constructor_declaration")) {
                members.append(makeSymbol(QStringLiteral("constructor"),
                                          nodeText(fieldNode(child, "name"), source),
                                          nodeLine(child), symbolDetail(child), {}, nodeSnippet(child, source)));
            } else if (type == QStringLiteral("property_declaration")) {
                members.append(makeSymbol(QStringLiteral("property"),
                                          nodeText(fieldNode(child, "name"), source),
                                          nodeLine(child), symbolDetail(child), {}, nodeSnippet(child, source)));
            } else if (type == QStringLiteral("field_declaration")) {
                const uint32_t fieldChildCount = ts_node_named_child_count(child);
                for (uint32_t fieldIndex = 0; fieldIndex < fieldChildCount; ++fieldIndex) {
                    TSNode fieldChild = ts_node_named_child(child, fieldIndex);
                    if (tsType(fieldChild) != QStringLiteral("variable_declaration")) {
                        continue;
                    }
                    const uint32_t varCount = ts_node_named_child_count(fieldChild);
                    for (uint32_t varIndex = 0; varIndex < varCount; ++varIndex) {
                        TSNode declarator = ts_node_named_child(fieldChild, varIndex);
                        if (tsType(declarator) != QStringLiteral("variable_declarator")) {
                            continue;
                        }
                        const QString name = nodeText(fieldNode(declarator, "name"), source);
                        if (!name.isEmpty()) {
                            members.append(makeSymbol(QStringLiteral("property"), name, nodeLine(declarator),
                                                      symbolDetail(child), {}, nodeSnippet(child, source)));
                        }
                    }
                }
            } else if (type == QStringLiteral("class_declaration")
                       || type == QStringLiteral("interface_declaration")
                       || type == QStringLiteral("struct_declaration")
                       || type == QStringLiteral("record_declaration")
                       || type == QStringLiteral("enum_declaration")) {
                QString nestedKind = QStringLiteral("class");
                if (type == QStringLiteral("interface_declaration")) {
                    nestedKind = QStringLiteral("interface");
                } else if (type == QStringLiteral("struct_declaration")) {
                    nestedKind = QStringLiteral("struct");
                } else if (type == QStringLiteral("record_declaration")) {
                    nestedKind = QStringLiteral("record");
                } else if (type == QStringLiteral("enum_declaration")) {
                    nestedKind = QStringLiteral("enum");
                }
                members.append(makeSymbol(nestedKind, nodeText(fieldNode(child, "name"), source),
                                          nodeLine(child), symbolDetail(child), {}, nodeSnippet(child, source)));
            }
        }
        return members;
    };

    std::function<void(TSNode)> walkCSharpScope = [&](TSNode node) {
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            const QString type = tsType(child);
            if (type == QStringLiteral("using_directive")) {
                appendDependency(child);
            } else if (type == QStringLiteral("namespace_declaration")
                       || type == QStringLiteral("file_scoped_namespace_declaration")
                       || type == QStringLiteral("declaration_list")) {
                // Block-scoped namespaces hold their declarations in a
                // declaration_list body.
                walkCSharpScope(child);
            } else if (type == QStringLiteral("class_declaration")
                       || type == QStringLiteral("interface_declaration")
                       || type == QStringLiteral("struct_declaration")
                       || type == QStringLiteral("record_declaration")
                       || type == QStringLiteral("enum_declaration")) {
                QString kind = QStringLiteral("class");
                if (type == QStringLiteral("interface_declaration")) {
                    kind = QStringLiteral("interface");
                } else if (type == QStringLiteral("struct_declaration")) {
                    kind = QStringLiteral("struct");
                } else if (type == QStringLiteral("record_declaration")) {
                    kind = QStringLiteral("record");
                } else if (type == QStringLiteral("enum_declaration")) {
                    kind = QStringLiteral("enum");
                }
                appendSymbol(makeSymbol(kind, nodeText(fieldNode(child, "name"), source),
                                        nodeLine(child), symbolDetail(child),
                                        parseCSharpMembers(fieldNode(child, "body")),
                                        nodeSnippet(child, source)));
            }
        }
    };

    walkCSharpScope(root);

    if (!symbols.isEmpty()) {
        QHash<QString, QVariantMap> byKey;
        QHash<QString, QStringList> keysByName;
        QHash<QString, QVariantList> callsByKey;
        QHash<QString, QVariantList> calledByByKey;
        collectSymbolsByKey(symbols, byKey, keysByName);

        std::function<void(TSNode, const QString &)> visit = [&](TSNode node, const QString &currentKey) {
            QString activeKey = currentKey;
            const QString nodeKey = csharpCallableKeyForNode(node, source, byKey);
            if (!nodeKey.isEmpty()) {
                activeKey = nodeKey;
            }

            const QString nodeType = tsType(node);
            if (!activeKey.isEmpty()
                && (nodeType == QStringLiteral("invocation_expression")
                    || nodeType == QStringLiteral("object_creation_expression"))) {
                const QString targetName = csharpCallTargetName(node, source);
                const QStringList candidateKeys = keysByName.value(targetName);
                if (!targetName.isEmpty() && !candidateKeys.isEmpty()) {
                    const QString targetKey = bestRelationTargetKey(candidateKeys, byKey);
                    if (targetKey != activeKey && byKey.contains(targetKey)) {
                        appendUniqueRelation(callsByKey, activeKey, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
                        appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(activeKey)), QStringLiteral("called by"));
                    }
                }
            }

            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t index = 0; index < childCount; ++index) {
                visit(ts_node_named_child(node, index), activeKey);
            }
        };

        visit(root, QString());
        symbols = applyRelationsToSymbols(symbols, callsByKey, calledByByKey);
    }

    symbols = applySnippetCallRelations(symbols);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("csharp"));
    result.insert(QStringLiteral("analysisHasAstErrors"), hasAstErrors);
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"),
                  dependencies.isEmpty() ? extractCSharpDependencies(text) : dependencies);
    result.insert(QStringLiteral("routes"), extractAspNetRoutes(path, text));
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parseRust(const QString &path, const QString &text) const
{
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("rust"));
    QVariantList symbols;
    QSet<QString> seenNames;

    auto appendSymbol = [&](const QVariantMap &symbol) {
        const QString name = symbol.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || seenNames.contains(name)) {
            return;
        }
        seenNames.insert(name);
        symbols.append(symbol);
    };

    QRegularExpression fnPattern(QStringLiteral(R"(^\s*(?:pub\s+)?fn\s+([A-Za-z_]\w*)\s*(?:<[^>]+>)?\s*\()"),
                                 QRegularExpression::MultilineOption);
    auto fnIt = fnPattern.globalMatch(text);
    while (fnIt.hasNext()) {
        const auto match = fnIt.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        appendSymbol(makeSymbol(QStringLiteral("function"), match.captured(1), line,
                                match.captured(0).contains(QStringLiteral("pub")) ? QStringLiteral("public") : QString(),
                                {}, snippetFromBraceBlock(text, match.capturedStart(0))));
    }

    QRegularExpression typePattern(QStringLiteral(R"(^\s*(?:pub\s+)?(struct|enum|trait)\s+([A-Za-z_]\w*))"),
                                   QRegularExpression::MultilineOption);
    auto typeIt = typePattern.globalMatch(text);
    while (typeIt.hasNext()) {
        const auto match = typeIt.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        appendSymbol(makeSymbol(match.captured(1), match.captured(2), line, QString(), {},
                                snippetFromBraceBlock(text, match.capturedStart(0))));
    }

    QRegularExpression implPattern(QStringLiteral(R"(^\s*impl(?:\s*<[^>]+>)?(?:\s+[A-Za-z_][\w:<>]*\s+for)?\s+([A-Za-z_][\w:<>]*)\s*\{)"),
                                   QRegularExpression::MultilineOption);
    auto implIt = implPattern.globalMatch(text);
    while (implIt.hasNext()) {
        const auto match = implIt.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        appendSymbol(makeSymbol(QStringLiteral("impl"), match.captured(1), line, QString(), {},
                                snippetFromBraceBlock(text, match.capturedStart(0))));
    }

    symbols = applySnippetCallRelations(symbols);

    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), extractRustDependencies(text));
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parseRustTreeSitter(const QString &path, const QString &text) const
{
    QVariantList symbols;
    QVariantList dependencies;
    QSet<QString> seenSymbols;
    QSet<QString> seenDependencies;
    const QByteArray source = text.toUtf8();
    TSLanguage *language = languageForName(QStringLiteral("rust"));
    if (!language) {
        return makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("rust"));
    }

    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, language)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("rust"));
    }

    TSTree *tree = parseAnalysedSource(parser, source);
    TSNode root = ts_tree_root_node(tree);
    const bool hasAstErrors = ts_node_has_error(root);

    auto hasVisibilityModifier = [](TSNode node) {
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            if (tsType(ts_node_named_child(node, i)) == QStringLiteral("visibility_modifier")) {
                return true;
            }
        }
        return false;
    };

    auto appendSymbol = [&](const QVariantMap &symbol) {
        const QString name = symbol.value(QStringLiteral("name")).toString();
        const QString kind = symbol.value(QStringLiteral("kind")).toString();
        const int line = symbol.value(QStringLiteral("line")).toInt();
        if (name.isEmpty()) {
            return;
        }
        const QString key = QStringLiteral("%1|%2|%3").arg(kind, name).arg(line);
        if (seenSymbols.contains(key)) {
            return;
        }
        seenSymbols.insert(key);
        symbols.append(symbol);
    };

    auto appendDependency = [&](const QString &target, const QString &type, TSNode node) {
        const QString trimmedTarget = target.trimmed();
        if (trimmedTarget.isEmpty()) {
            return;
        }
        const QString key = type + QLatin1Char('|') + trimmedTarget;
        if (seenDependencies.contains(key)) {
            return;
        }
        seenDependencies.insert(key);

        QVariantMap item = makeSourceContextItem(path, QStringLiteral("rust"), nodeLine(node),
                                                 nodeSnippet(node, source, 3),
                                                 QStringLiteral("%1 dependency").arg(type));
        item.insert(QStringLiteral("target"), trimmedTarget);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("label"),
                    type == QStringLiteral("use") ? rustDependencyLabel(trimmedTarget) : trimmedTarget);

        if (type == QStringLiteral("module")) {
            const QDir dir = QFileInfo(path).dir();
            const QStringList candidates = {
                dir.filePath(trimmedTarget + QStringLiteral(".rs")),
                dir.filePath(trimmedTarget + QStringLiteral("/mod.rs"))
            };
            QString chosenPath;
            for (const QString &candidate : candidates) {
                if (QFileInfo::exists(candidate)) {
                    chosenPath = candidate;
                    break;
                }
            }
            item.insert(QStringLiteral("path"), chosenPath);
            item.insert(QStringLiteral("exists"), !chosenPath.isEmpty());
            if (!chosenPath.isEmpty()) {
                item.insert(QStringLiteral("label"), QFileInfo(chosenPath).fileName());
            }
        } else {
            item.insert(QStringLiteral("path"), QString());
            item.insert(QStringLiteral("exists"), true);
        }

        dependencies.append(item);
    };

    std::function<QVariantList(TSNode)> parseRustMembers = [&](TSNode bodyNode) {
        QVariantList members;
        if (ts_node_is_null(bodyNode)) {
            return members;
        }
        const uint32_t count = ts_node_named_child_count(bodyNode);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(bodyNode, i);
            const QString type = tsType(child);
            if (type == QStringLiteral("function_item") || type == QStringLiteral("function_signature_item")) {
                const QString name = nodeText(fieldNode(child, "name"), source);
                QString detail = hasVisibilityModifier(child) ? QStringLiteral("public") : QString();
                members.append(makeSymbol(QStringLiteral("method"), name, nodeLine(child), detail, {},
                                          nodeSnippet(child, source)));
            } else if (type == QStringLiteral("const_item")) {
                const QString name = nodeText(fieldNode(child, "name"), source);
                members.append(makeSymbol(QStringLiteral("constant"), name, nodeLine(child), QString(), {},
                                          nodeSnippet(child, source)));
            }
        }
        return members;
    };

    const uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_named_child(root, i);
        const QString type = tsType(child);

        if (type == QStringLiteral("use_declaration")) {
            QString target = nodeText(child, source).trimmed();
            if (target.startsWith(QStringLiteral("use "))) {
                target.remove(0, 4);
            }
            if (target.endsWith(QLatin1Char(';'))) {
                target.chop(1);
            }
            appendDependency(target, QStringLiteral("use"), child);
            continue;
        }

        if (type == QStringLiteral("function_item")) {
            const QString name = nodeText(fieldNode(child, "name"), source);
            appendSymbol(makeSymbol(QStringLiteral("function"), name, nodeLine(child),
                                    hasVisibilityModifier(child) ? QStringLiteral("public") : QString(), {},
                                    nodeSnippet(child, source)));
        } else if (type == QStringLiteral("struct_item")) {
            const QString name = nodeText(fieldNode(child, "name"), source);
            appendSymbol(makeSymbol(QStringLiteral("struct"), name, nodeLine(child),
                                    hasVisibilityModifier(child) ? QStringLiteral("public") : QString(), {},
                                    nodeSnippet(child, source)));
        } else if (type == QStringLiteral("enum_item")) {
            const QString name = nodeText(fieldNode(child, "name"), source);
            QVariantList members;
            TSNode body = fieldNode(child, "body");
            const uint32_t variantCount = ts_node_named_child_count(body);
            for (uint32_t variantIndex = 0; variantIndex < variantCount; ++variantIndex) {
                TSNode variant = ts_node_named_child(body, variantIndex);
                if (tsType(variant) != QStringLiteral("enum_variant")) {
                    continue;
                }
                members.append(makeSymbol(QStringLiteral("variant"),
                                          nodeText(fieldNode(variant, "name"), source),
                                          nodeLine(variant), QString(), {}, nodeSnippet(variant, source)));
            }
            appendSymbol(makeSymbol(QStringLiteral("enum"), name, nodeLine(child),
                                    hasVisibilityModifier(child) ? QStringLiteral("public") : QString(),
                                    members, nodeSnippet(child, source)));
        } else if (type == QStringLiteral("trait_item")) {
            const QString name = nodeText(fieldNode(child, "name"), source);
            appendSymbol(makeSymbol(QStringLiteral("trait"), name, nodeLine(child),
                                    hasVisibilityModifier(child) ? QStringLiteral("public") : QString(),
                                    parseRustMembers(fieldNode(child, "body")), nodeSnippet(child, source)));
        } else if (type == QStringLiteral("impl_item")) {
            const QString traitName = nodeText(fieldNode(child, "trait"), source);
            const QString typeName = nodeText(fieldNode(child, "type"), source);
            const QString name = traitName.isEmpty() ? typeName : QStringLiteral("%1 for %2").arg(traitName, typeName);
            appendSymbol(makeSymbol(QStringLiteral("impl"), name, nodeLine(child), QString(),
                                    parseRustMembers(fieldNode(child, "body")), nodeSnippet(child, source)));
        } else if (type == QStringLiteral("mod_item")) {
            const QString name = nodeText(fieldNode(child, "name"), source);
            const TSNode body = fieldNode(child, "body");
            appendSymbol(makeSymbol(QStringLiteral("module"), name, nodeLine(child),
                                    hasVisibilityModifier(child) ? QStringLiteral("public") : QString(),
                                    parseRustMembers(body), nodeSnippet(child, source)));
            if (ts_node_is_null(body)) {
                appendDependency(name, QStringLiteral("module"), child);
            }
        }
    }

    if (!symbols.isEmpty()) {
        QHash<QString, QVariantMap> byKey;
        QHash<QString, QStringList> keysByName;
        QHash<QString, QVariantList> callsByKey;
        QHash<QString, QVariantList> calledByByKey;
        collectSymbolsByKey(symbols, byKey, keysByName);

        std::function<void(TSNode, const QString &)> visit = [&](TSNode node, const QString &currentKey) {
            QString activeKey = currentKey;
            const QString nodeKey = rustCallableKeyForNode(node, source, byKey);
            if (!nodeKey.isEmpty()) {
                activeKey = nodeKey;
            }

            if (!activeKey.isEmpty() && tsType(node) == QStringLiteral("call_expression")) {
                const QString targetName = rustCallTargetName(node, source);
                const QStringList candidateKeys = keysByName.value(targetName);
                if (!targetName.isEmpty() && !candidateKeys.isEmpty()) {
                    const QString targetKey = bestRelationTargetKey(candidateKeys, byKey);
                    if (targetKey != activeKey && byKey.contains(targetKey)) {
                        appendUniqueRelation(callsByKey, activeKey, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
                        appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(activeKey)), QStringLiteral("called by"));
                    }
                }
            }

            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t index = 0; index < childCount; ++index) {
                visit(ts_node_named_child(node, index), activeKey);
            }
        };

        visit(root, QString());
        symbols = applyRelationsToSymbols(symbols, callsByKey, calledByByKey);
    }

    symbols = applySnippetCallRelations(symbols);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    if (dependencies.isEmpty()) {
        dependencies = extractRustDependencies(text);
    }

    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("rust"));
    result.insert(QStringLiteral("analysisHasAstErrors"), hasAstErrors);
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), dependencies);
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

namespace {

// C-family comments and string/char literals blanked (newlines kept), so
// brackets and braces can be matched on the result with unchanged offsets.
QString blankCFamilyNoise(const QString &text)
{
    QString out = text;
    const int size = out.size();
    auto blank = [&](int from, int to) {
        for (int i = from; i < to && i < size; ++i) {
            if (out.at(i) != QLatin1Char('\n')) {
                out[i] = QLatin1Char(' ');
            }
        }
    };
    int index = 0;
    while (index < size) {
        const QChar ch = out.at(index);
        const QChar next = index + 1 < size ? out.at(index + 1) : QChar();
        if (ch == QLatin1Char('/') && next == QLatin1Char('/')) {
            int end = out.indexOf(QLatin1Char('\n'), index);
            if (end < 0) end = size;
            blank(index, end);
            index = end;
        } else if (ch == QLatin1Char('/') && next == QLatin1Char('*')) {
            int end = out.indexOf(QStringLiteral("*/"), index + 2);
            end = end < 0 ? size : end + 2;
            blank(index, end);
            index = end;
        } else if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            int end = index + 1;
            while (end < size && out.at(end) != ch && out.at(end) != QLatin1Char('\n')) {
                if (out.at(end) == QLatin1Char('\\')) ++end;
                ++end;
            }
            blank(index + 1, end);
            index = end + 1;
        } else {
            ++index;
        }
    }
    return out;
}

int matchingClose(const QString &clean, int open, QChar openChar, QChar closeChar)
{
    int depth = 0;
    for (int i = open; i < clean.size(); ++i) {
        if (clean.at(i) == openChar) {
            ++depth;
        } else if (clean.at(i) == closeChar && --depth == 0) {
            return i;
        }
    }
    return -1;
}

// Selector of an Objective-C message send whose '[' is at `open`:
// [receiver name] -> "name", [receiver a:x b:y] -> "a:b:".
QString objcMessageSelector(const QString &clean, int open, int close)
{
    int index = open + 1;
    // Skip the receiver: an expression up to the first whitespace at depth 0
    // (nested sends / calls / casts are balanced).
    int depth = 0;
    while (index < close) {
        const QChar ch = clean.at(index);
        if (ch == QLatin1Char('[') || ch == QLatin1Char('(')) {
            ++depth;
        } else if (ch == QLatin1Char(']') || ch == QLatin1Char(')')) {
            --depth;
        } else if (ch.isSpace() && depth == 0 && index > open + 1) {
            break;
        }
        ++index;
    }
    const QString rest = clean.mid(index, close - index);
    QString selector;
    depth = 0;
    QString word;
    bool sawColon = false;
    for (int i = 0; i < rest.size(); ++i) {
        const QChar ch = rest.at(i);
        if (ch == QLatin1Char('[') || ch == QLatin1Char('(') || ch == QLatin1Char('{')) {
            ++depth;
            word.clear();
        } else if (ch == QLatin1Char(']') || ch == QLatin1Char(')') || ch == QLatin1Char('}')) {
            --depth;
            word.clear();
        } else if (depth == 0 && (ch.isLetterOrNumber() || ch == QLatin1Char('_'))) {
            word += ch;
        } else if (depth == 0 && ch == QLatin1Char(':')) {
            if (!word.isEmpty()) {
                selector += word + QLatin1Char(':');
                sawColon = true;
            }
            word.clear();
        } else if (depth == 0) {
            if (!sawColon && !word.isEmpty() && selector.isEmpty()) {
                // unary selector: first word after the receiver
                return word;
            }
            word.clear();
        }
    }
    if (!sawColon) {
        return word;
    }
    return selector;
}

} // namespace

QVariantMap SymbolParser::parseObjectiveC(const QString &path, const QString &text, const QString &language) const
{
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), language);
    const QString clean = blankCFamilyNoise(text);
    QVariantList symbols;
    struct Body
    {
        QString key;
        int open = 0;
        int close = 0;
    };
    QList<Body> bodies;
    QList<QPair<int, int>> implementationRanges;

    auto snippetBetween = [&](int start, int end) {
        const int startLine = lineNumberAtOffset(text, start);
        const int endLine = lineNumberAtOffset(text, qMax(start, end));
        const QStringList lines = text.split(QLatin1Char('\n'));
        QStringList out;
        for (int l = startLine; l <= endLine && l <= lines.size() && out.size() < 10; ++l) {
            out.append(lines.at(l - 1));
        }
        if (endLine - startLine + 1 > 10) {
            out.append(QStringLiteral("..."));
        }
        return out.join(QLatin1Char('\n'));
    };

    // Properties declared in class extensions / interfaces within this file.
    QHash<QString, QVariantList> propertiesByClass;
    static const QRegularExpression interfacePattern(QStringLiteral(R"(^[ \t]*@interface\s+([A-Za-z_]\w*)[^\n]*$)"),
                                                     QRegularExpression::MultilineOption);
    static const QRegularExpression propertyPattern(QStringLiteral(R"(^[ \t]*@property\s*(\([^)]*\))?\s*([^;]*?)\b([A-Za-z_]\w*)\s*;)"),
                                                    QRegularExpression::MultilineOption);
    auto interfaceIt = interfacePattern.globalMatch(clean);
    while (interfaceIt.hasNext()) {
        const auto match = interfaceIt.next();
        const int end = clean.indexOf(QStringLiteral("@end"), match.capturedEnd(0));
        const QString block = clean.mid(match.capturedStart(0), (end < 0 ? clean.size() : end) - match.capturedStart(0));
        auto propertyIt = propertyPattern.globalMatch(block);
        while (propertyIt.hasNext()) {
            const auto property = propertyIt.next();
            const int line = lineNumberAtOffset(text, match.capturedStart(0) + property.capturedStart(0));
            propertiesByClass[match.captured(1)].append(
                makeSymbol(QStringLiteral("property"), property.captured(3), line,
                           (property.captured(1) + QLatin1Char(' ') + property.captured(2)).simplified(), {},
                           snippetFromLine(text, line, 0)));
        }
    }

    static const QRegularExpression implementationPattern(QStringLiteral(R"(^[ \t]*@implementation\s+([A-Za-z_]\w*)(?:\s*\(\s*([A-Za-z_]\w*)?\s*\))?)"),
                                                          QRegularExpression::MultilineOption);
    static const QRegularExpression methodHeader(QStringLiteral(R"(^[ \t]*([-+])\s*\(([^)]*)\)\s*)"),
                                                 QRegularExpression::MultilineOption);
    static const QRegularExpression keywordPart(QStringLiteral(R"(([A-Za-z_]\w*)\s*:\s*(?:\(([^)]*)\))?\s*([A-Za-z_]\w*))"));
    auto implementationIt = implementationPattern.globalMatch(clean);
    while (implementationIt.hasNext()) {
        const auto match = implementationIt.next();
        const QString className = match.captured(1);
        const int blockStart = match.capturedStart(0);
        int blockEnd = clean.indexOf(QStringLiteral("@end"), match.capturedEnd(0));
        if (blockEnd < 0) {
            blockEnd = clean.size();
        }
        implementationRanges.append({blockStart, blockEnd});
        QVariantList members = propertiesByClass.take(className);
        auto methodIt = methodHeader.globalMatch(clean.mid(0, blockEnd), match.capturedEnd(0));
        while (methodIt.hasNext()) {
            const auto method = methodIt.next();
            const int headerEnd = [&]() {
                for (int i = method.capturedEnd(0); i < blockEnd; ++i) {
                    if (clean.at(i) == QLatin1Char('{') || clean.at(i) == QLatin1Char(';')) {
                        return i;
                    }
                }
                return blockEnd;
            }();
            const QString header = clean.mid(method.capturedEnd(0), headerEnd - method.capturedEnd(0)).simplified();
            QString selector;
            QVariantList parameters;
            auto partIt = keywordPart.globalMatch(header);
            while (partIt.hasNext()) {
                const auto part = partIt.next();
                selector += part.captured(1) + QLatin1Char(':');
                parameters.append(makeSignatureParameter(part.captured(3), part.captured(2).simplified()));
            }
            if (selector.isEmpty()) {
                static const QRegularExpression unary(QStringLiteral(R"(^([A-Za-z_]\w*))"));
                selector = unary.match(header).captured(1);
            }
            if (selector.isEmpty()) {
                continue;
            }
            const int line = lineNumberAtOffset(text, method.capturedStart(0));
            int close = headerEnd;
            if (headerEnd < blockEnd && clean.at(headerEnd) == QLatin1Char('{')) {
                close = matchingClose(clean, headerEnd, QLatin1Char('{'), QLatin1Char('}'));
                if (close < 0 || close > blockEnd) {
                    close = blockEnd; // unbalanced body: stop at @end
                }
            }
            QVariantMap symbol = makeSymbol(QStringLiteral("method"), selector, line,
                                            method.captured(1) == QStringLiteral("+") ? QStringLiteral("class method") : QString(), {},
                                            snippetBetween(method.capturedStart(0), close));
            symbol.insert(QStringLiteral("endLine"), lineNumberAtOffset(text, close));
            symbol.insert(QStringLiteral("parameters"), parameters);
            const QString returnType = method.captured(2).simplified();
            symbol.insert(QStringLiteral("returns"), QVariantList{QVariantMap{{QStringLiteral("text"),
                                                                               returnType == QStringLiteral("void") ? QStringLiteral("none") : returnType}}});
            symbol.insert(QStringLiteral("signatureSource"), QStringLiteral("parser"));
            members.append(symbol);
            if (headerEnd < blockEnd && clean.at(headerEnd) == QLatin1Char('{')) {
                bodies.append({symbolKey(symbol), headerEnd, close});
            }
        }
        symbols.append(makeSymbol(QStringLiteral("class"), className, lineNumberAtOffset(text, blockStart),
                                  match.captured(2).isEmpty() ? QString() : QStringLiteral("category (%1)").arg(match.captured(2)), members,
                                  snippetFromLine(text, lineNumberAtOffset(text, blockStart), 3)));
    }

    // C functions outside @implementation blocks.
    static const QRegularExpression functionPattern(
        QStringLiteral(R"(^[ \t]*(?:static\s+|extern\s+|inline\s+|FOUNDATION_EXPORT\s+)*(?:[\w<>*]+\s+)+\**([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{)"),
        QRegularExpression::MultilineOption);
    auto functionIt = functionPattern.globalMatch(clean);
    while (functionIt.hasNext()) {
        const auto match = functionIt.next();
        const QString name = match.captured(1);
        static const QSet<QString> keywords = {QStringLiteral("if"), QStringLiteral("while"), QStringLiteral("for"),
                                               QStringLiteral("switch"), QStringLiteral("return")};
        const bool insideImplementation = std::any_of(implementationRanges.cbegin(), implementationRanges.cend(),
                                                      [&](const QPair<int, int> &range) {
                                                          return match.capturedStart(0) > range.first && match.capturedStart(0) < range.second;
                                                      });
        if (keywords.contains(name) || insideImplementation) {
            continue;
        }
        const int open = match.capturedEnd(0) - 1;
        int close = matchingClose(clean, open, QLatin1Char('{'), QLatin1Char('}'));
        if (close < 0) {
            close = clean.size() - 1;
        }
        QVariantMap symbol = makeSymbol(QStringLiteral("function"), name, lineNumberAtOffset(text, match.capturedStart(0)), QString(), {},
                                        snippetBetween(match.capturedStart(0), close));
        QVariantList parameters;
        for (const QString &part : splitTopLevelSignatureParts(match.captured(2).simplified())) {
            if (part == QStringLiteral("void") || part.isEmpty()) {
                continue;
            }
            static const QRegularExpression trailingName(QStringLiteral(R"(([A-Za-z_]\w*)\s*$)"));
            const auto nameMatch = trailingName.match(part);
            parameters.append(makeSignatureParameter(nameMatch.captured(1), part.left(nameMatch.capturedStart(1)).trimmed()));
        }
        symbol.insert(QStringLiteral("parameters"), parameters);
        symbol.insert(QStringLiteral("returns"), QVariantList{QVariantMap{{QStringLiteral("text"),
            clean.mid(match.capturedStart(0), match.capturedStart(1) - match.capturedStart(0)).simplified()
                .remove(QRegularExpression(QStringLiteral(R"(\b(static|extern|inline|FOUNDATION_EXPORT)\b)"))).simplified()}}});
        symbol.insert(QStringLiteral("signatureSource"), QStringLiteral("parser"));
        symbols.append(symbol);
        bodies.append({symbolKey(symbol), open, close});
    }

    // Calls: message sends matched to method selectors, C calls to functions.
    QHash<QString, QVariantMap> byKey;
    QHash<QString, QStringList> keysByName;
    collectSymbolsByKey(symbols, byKey, keysByName);
    QHash<QString, QVariantList> callsByKey;
    QHash<QString, QVariantList> calledByByKey;
    static const QRegularExpression cCall(QStringLiteral(R"(\b([A-Za-z_]\w*)\s*\()"));
    auto link = [&](const QString &ownerKey, const QString &targetName) {
        const QStringList candidates = keysByName.value(targetName);
        if (candidates.isEmpty()) {
            return;
        }
        const QString targetKey = bestRelationTargetKey(candidates, byKey);
        if (targetKey.isEmpty() || targetKey == ownerKey || !byKey.contains(targetKey)
            || !isCallableSymbolKind(byKey.value(targetKey).value(QStringLiteral("kind")).toString())) {
            return;
        }
        appendUniqueRelation(callsByKey, ownerKey, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
        appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(ownerKey)), QStringLiteral("called by"));
    };
    for (const Body &body : std::as_const(bodies)) {
        for (int i = body.open; i < body.close; ++i) {
            if (clean.at(i) != QLatin1Char('[')) {
                continue;
            }
            const int close = matchingClose(clean, i, QLatin1Char('['), QLatin1Char(']'));
            if (close < 0 || close > body.close) {
                continue;
            }
            const QString selector = objcMessageSelector(clean, i, close);
            if (!selector.isEmpty()) {
                link(body.key, selector);
            }
        }
        auto callIt = cCall.globalMatch(clean.mid(body.open, body.close - body.open));
        while (callIt.hasNext()) {
            link(body.key, callIt.next().captured(1));
        }
    }
    symbols = applyRelationsToSymbols(symbols, callsByKey, calledByByKey);

    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), extractObjectiveCDependencies(path, text));
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

namespace {

// Kotlin comments (nested /* */), string literals (incl. """raw""" and
// ${...} templates with nested quotes) and char literals blanked; newlines
// and offsets kept.
QString blankKotlinNoise(const QString &text)
{
    QString out = text;
    const int size = out.size();
    auto blank = [&](int from, int to) {
        for (int i = from; i < to && i < size; ++i) {
            if (out.at(i) != QLatin1Char('\n')) {
                out[i] = QLatin1Char(' ');
            }
        }
    };
    std::function<int(int, bool)> skipString = [&](int start, bool raw) -> int {
        // start: index just after the opening quote(s); returns index after closing quote(s)
        int i = start;
        while (i < size) {
            const QChar ch = out.at(i);
            if (raw) {
                if (out.mid(i, 3) == QStringLiteral("\"\"\"")) {
                    int end = i + 3;
                    while (end < size && out.at(end) == QLatin1Char('"')) {
                        ++end; // """" closes with extra quotes as content
                    }
                    return end;
                }
            } else {
                if (ch == QLatin1Char('\\')) {
                    i += 2;
                    continue;
                }
                if (ch == QLatin1Char('"')) {
                    return i + 1;
                }
                if (ch == QLatin1Char('\n')) {
                    return i; // unterminated: stop at the line end
                }
            }
            if (ch == QLatin1Char('$') && i + 1 < size && out.at(i + 1) == QLatin1Char('{')) {
                int depth = 0;
                for (; i < size; ++i) {
                    const QChar inner = out.at(i);
                    if (inner == QLatin1Char('"')) {
                        const bool innerRaw = out.mid(i, 3) == QStringLiteral("\"\"\"");
                        i = skipString(i + (innerRaw ? 3 : 1), innerRaw) - 1;
                    } else if (inner == QLatin1Char('{')) {
                        ++depth;
                    } else if (inner == QLatin1Char('}') && --depth == 0) {
                        break;
                    } else if (inner == QLatin1Char('\n') && !raw) {
                        break;
                    }
                }
            }
            ++i;
        }
        return size;
    };
    int index = 0;
    while (index < size) {
        const QChar ch = out.at(index);
        const QChar next = index + 1 < size ? out.at(index + 1) : QChar();
        if (ch == QLatin1Char('/') && next == QLatin1Char('/')) {
            int end = out.indexOf(QLatin1Char('\n'), index);
            end = end < 0 ? size : end;
            blank(index, end);
            index = end;
        } else if (ch == QLatin1Char('/') && next == QLatin1Char('*')) {
            int depth = 0;
            int end = index;
            while (end < size) {
                if (out.mid(end, 2) == QStringLiteral("/*")) {
                    ++depth;
                    end += 2;
                } else if (out.mid(end, 2) == QStringLiteral("*/")) {
                    end += 2;
                    if (--depth == 0) {
                        break;
                    }
                } else {
                    ++end;
                }
            }
            blank(index, end);
            index = end;
        } else if (ch == QLatin1Char('"')) {
            const bool raw = out.mid(index, 3) == QStringLiteral("\"\"\"");
            const int end = skipString(index + (raw ? 3 : 1), raw);
            blank(index + 1, qMax(index + 1, end - 1));
            index = qMax(index + 1, end);
        } else if (ch == QLatin1Char('\'')) {
            int end = index + 1;
            while (end < size && out.at(end) != QLatin1Char('\'') && out.at(end) != QLatin1Char('\n')) {
                end += out.at(end) == QLatin1Char('\\') ? 2 : 1;
            }
            blank(index + 1, end);
            index = end + 1;
        } else {
            ++index;
        }
    }
    return out;
}

// Index of the bracket closing the one at `open`, within `limit`; -1 if the
// block is not closed there.
int kotlinMatchingClose(const QString &clean, int open, int limit)
{
    const QChar openChar = clean.at(open);
    const QChar closeChar = openChar == QLatin1Char('{') ? QLatin1Char('}')
        : openChar == QLatin1Char('(') ? QLatin1Char(')') : openChar == QLatin1Char('[') ? QLatin1Char(']') : QLatin1Char('>');
    int depth = 0;
    for (int i = open; i < limit && i < clean.size(); ++i) {
        if (clean.at(i) == openChar) {
            ++depth;
        } else if (clean.at(i) == closeChar && --depth == 0) {
            return i;
        }
    }
    return -1;
}

// End of a declaration without a body: the line end at bracket depth 0
// (continuation lines starting with an operator or '.' join it).
int kotlinStatementEnd(const QString &clean, int from, int limit)
{
    int depth = 0;
    for (int i = from; i < limit; ++i) {
        const QChar ch = clean.at(i);
        if (ch == QLatin1Char('(') || ch == QLatin1Char('[') || ch == QLatin1Char('{')) {
            ++depth;
        } else if ((ch == QLatin1Char(')') || ch == QLatin1Char(']') || ch == QLatin1Char('}')) && depth > 0) {
            --depth;
        } else if (ch == QLatin1Char('}') && depth == 0) {
            return i; // end of the enclosing body
        } else if (ch == QLatin1Char('\n') && depth == 0) {
            int next = i + 1;
            while (next < limit && (clean.at(next) == QLatin1Char(' ') || clean.at(next) == QLatin1Char('\t'))) {
                ++next;
            }
            const QChar lead = next < limit ? clean.at(next) : QChar();
            const bool continues = lead == QLatin1Char('.') || lead == QLatin1Char('?') || lead == QLatin1Char('+')
                || lead == QLatin1Char('-') || lead == QLatin1Char('*') || lead == QLatin1Char('&')
                || lead == QLatin1Char('|') || lead == QLatin1Char(':') || lead == QLatin1Char('=');
            if (!continues) {
                return i;
            }
        }
    }
    return limit;
}

QStringList kotlinSplitTopLevel(const QString &text)
{
    QStringList parts;
    int depth = 0;
    int start = 0;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch == QLatin1Char('(') || ch == QLatin1Char('<') || ch == QLatin1Char('[') || ch == QLatin1Char('{')) {
            ++depth;
        } else if ((ch == QLatin1Char(')') || ch == QLatin1Char('>') || ch == QLatin1Char(']') || ch == QLatin1Char('}')) && depth > 0) {
            --depth;
        } else if (ch == QLatin1Char(',') && depth == 0) {
            parts.append(text.mid(start, i - start).trimmed());
            start = i + 1;
        }
    }
    const QString last = text.mid(start).trimmed();
    if (!last.isEmpty()) {
        parts.append(last);
    }
    return parts;
}

// The start of the next declaration header indented no deeper than
// `indent` after `from` (bounded by `limit`): a member's body or statement
// cannot run past it, so a lost brace or an unclosed call costs only that
// member.
int kotlinNextSiblingHeader(const QString &clean, int from, int limit, int indent)
{
    static const QRegularExpression header(QStringLiteral(
        R"(^([ \t]*)(?:@[\w.]+(?:\([^\n]*\))?\s+)*(?:(?:public|private|protected|internal|open|abstract|final|sealed|data|enum|annotation|inner|value|inline|companion|override|suspend|operator|infix|tailrec|external|lateinit|const|expect|actual)\s+)*(?:fun|class|interface|object|val|var|typealias|constructor|init)\b)"),
        QRegularExpression::MultilineOption);
    int lineStart = clean.indexOf(QLatin1Char('\n'), from);
    while (lineStart >= 0 && lineStart + 1 < limit) {
        const auto match = header.match(clean, lineStart + 1, QRegularExpression::NormalMatch,
                                        QRegularExpression::AnchoredMatchOption);
        if (match.hasMatch() && match.captured(1).size() <= indent) {
            return lineStart + 1;
        }
        lineStart = clean.indexOf(QLatin1Char('\n'), lineStart + 1);
    }
    return limit;
}

struct KotlinBody
{
    QString key;
    int open = 0;
    int close = 0;
};

} // namespace

QVariantMap SymbolParser::parseKotlin(const QString &path, const QString &text) const
{
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("kotlin"));
    const QString clean = blankKotlinNoise(text);
    const QStringList textLines = text.split(QLatin1Char('\n'));
    QList<KotlinBody> bodies;

    auto snippetBetween = [&](int start, int end) {
        const int startLine = lineNumberAtOffset(text, start);
        const int endLine = lineNumberAtOffset(text, qMax(start, end));
        QStringList out;
        for (int l = startLine; l <= endLine && l <= textLines.size() && out.size() < 10; ++l) {
            out.append(textLines.at(l - 1));
        }
        if (endLine - startLine + 1 > 10) {
            out.append(QStringLiteral("..."));
        }
        return out.join(QLatin1Char('\n'));
    };

    // Column-0 declaration headers bound unbalanced bodies (a lost brace
    // costs at most the declaration it is in).
    QVector<int> topLevelStarts;
    static const QRegularExpression topLevelHeader(QStringLiteral(
        R"(^(?:@[\w.]+(?:\([^\n]*\))?\s+)*(?:(?:public|private|internal|protected|open|abstract|sealed|data|enum|annotation|inline|value|suspend|operator|infix|tailrec|external|const|expect|actual)\s+)*(?:fun|class|interface|object|val|var|typealias)\b)"),
        QRegularExpression::MultilineOption);
    auto topIt = topLevelHeader.globalMatch(clean);
    while (topIt.hasNext()) {
        topLevelStarts.append(topIt.next().capturedStart(0));
    }
    auto nextTopLevelAfter = [&](int offset, int limit) {
        for (int start : std::as_const(topLevelStarts)) {
            if (start > offset) {
                return qMin(start, limit);
            }
        }
        return limit;
    };

    static const QRegularExpression declarationHeader(QStringLiteral(
        R"((?:^|(?<=[;{}]))[ \t]*((?:@[\w.]+(?:\([^\n]*?\))?\s+)*)((?:(?:public|private|protected|internal|open|abstract|final|sealed|data|enum|annotation|inner|value|inline|companion|override|suspend|operator|infix|tailrec|external|lateinit|const|expect|actual|fun(?=\s+interface))\s+)*)(class|interface|object|fun|val|var|typealias|constructor|init)\b)"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression identifier(QStringLiteral(R"(^\s*([A-Za-z_]\w*|`[^`]+`))"));

    std::function<QVariantList(int, int, int, const QString &)> parseScope =
        [&](int from, int to, int depth, const QString &owner) -> QVariantList {
        QVariantList symbols;
        int position = from;
        while (position < to) {
            const auto match = declarationHeader.match(clean, position);
            if (!match.hasMatch() || match.capturedStart(3) >= to) {
                break;
            }
            const QString keyword = match.captured(3);
            const QString modifiers = match.captured(2).simplified();
            const QString annotations = match.captured(1).simplified();
            const int start = match.capturedStart(0) + (clean.at(match.capturedStart(0)) == QLatin1Char('\n') ? 1 : 0);
            const int line = lineNumberAtOffset(text, match.capturedStart(3));
            int cursor = match.capturedEnd(3);
            int headerIndent = 0;
            for (int i = match.capturedStart(3) - 1; i >= 0 && clean.at(i) != QLatin1Char('\n'); --i) {
                headerIndent = clean.at(i) == QLatin1Char(' ') || clean.at(i) == QLatin1Char('\t') ? headerIndent + 1 : 0;
            }
            const int limit = depth == 0 ? nextTopLevelAfter(match.capturedStart(0), to)
                                         : kotlinNextSiblingHeader(clean, match.capturedEnd(3), to, headerIndent);

            if (keyword == QStringLiteral("init")) {
                int open = cursor;
                while (open < to && clean.at(open).isSpace()) {
                    ++open;
                }
                const int close = open < to && clean.at(open) == QLatin1Char('{') ? kotlinMatchingClose(clean, open, limit) : -1;
                position = close > 0 ? close + 1 : kotlinStatementEnd(clean, cursor, to) + 1;
                continue;
            }

            QString kind;
            QString name;
            QString detail = (annotations + QLatin1Char(' ') + modifiers).simplified();
            if (keyword == QStringLiteral("class") || keyword == QStringLiteral("interface") || keyword == QStringLiteral("object")) {
                kind = keyword;
                if (modifiers.contains(QStringLiteral("enum"))) {
                    kind = QStringLiteral("enum");
                } else if (keyword == QStringLiteral("class") && modifiers.contains(QStringLiteral("annotation"))) {
                    kind = QStringLiteral("annotation");
                }
                const auto nameMatch = identifier.match(clean.mid(cursor, 200));
                name = nameMatch.captured(1);
                if (name.isEmpty() && keyword == QStringLiteral("object") && modifiers.contains(QStringLiteral("companion"))) {
                    name = QStringLiteral("Companion");
                } else {
                    cursor += nameMatch.capturedEnd(1);
                }
                if (name.isEmpty()) {
                    position = match.capturedEnd(0);
                    continue;
                }
            } else if (keyword == QStringLiteral("fun") || keyword == QStringLiteral("constructor")) {
                kind = keyword == QStringLiteral("fun") ? (depth > 0 && !owner.isEmpty() ? QStringLiteral("method") : QStringLiteral("function"))
                                                        : QStringLiteral("constructor");
                if (keyword == QStringLiteral("constructor")) {
                    name = owner.isEmpty() ? QStringLiteral("constructor") : owner;
                } else {
                    // fun <T> Receiver.name(
                    int scan = cursor;
                    while (scan < limit && clean.at(scan).isSpace()) {
                        ++scan;
                    }
                    if (scan < limit && clean.at(scan) == QLatin1Char('<')) {
                        const int close = kotlinMatchingClose(clean, scan, limit);
                        scan = close > 0 ? close + 1 : scan + 1;
                    }
                    const int paren = clean.indexOf(QLatin1Char('('), scan);
                    if (paren < 0 || paren >= limit) {
                        position = match.capturedEnd(0);
                        continue;
                    }
                    const QString head = clean.mid(scan, paren - scan).trimmed();
                    const int dot = head.lastIndexOf(QLatin1Char('.'));
                    name = (dot >= 0 ? head.mid(dot + 1) : head).trimmed();
                    if (dot >= 0) {
                        detail = (detail + QStringLiteral(" extension of ") + head.left(dot)).simplified();
                    }
                    cursor = paren;
                }
                static const QRegularExpression validName(QStringLiteral(R"(^(?:[A-Za-z_]\w*|`[^`]+`)$)"));
                if (!validName.match(name).hasMatch()) {
                    position = match.capturedEnd(0);
                    continue;
                }
            } else if (keyword == QStringLiteral("val") || keyword == QStringLiteral("var")) {
                if (depth > 0 && owner.isEmpty()) {
                    position = kotlinStatementEnd(clean, cursor, to) + 1;
                    continue;
                }
                static const QRegularExpression propertyName(QStringLiteral(R"(^\s*(?:<[^>]*>\s*)?(?:[\w.<>?]+\.)?([A-Za-z_]\w*)\s*(?::\s*([^=\n{]+?))?\s*(?:=|by\b|\n|$|\{|get\b))"));
                const auto propertyMatch = propertyName.match(clean.mid(cursor, 400));
                name = propertyMatch.captured(1);
                if (name.isEmpty()) {
                    position = match.capturedEnd(0);
                    continue;
                }
                kind = modifiers.contains(QStringLiteral("const")) ? QStringLiteral("constant") : QStringLiteral("property");
                const QString type = propertyMatch.captured(2).simplified();
                detail = (detail + QLatin1Char(' ') + keyword + (type.isEmpty() ? QString() : QStringLiteral(": ") + type)).simplified();
            } else if (keyword == QStringLiteral("typealias")) {
                kind = QStringLiteral("type");
                name = identifier.match(clean.mid(cursor, 200)).captured(1);
                if (name.isEmpty()) {
                    position = match.capturedEnd(0);
                    continue;
                }
            }

            // Header: type parameters, parameter list, return / supertypes,
            // then a body `{...}`, an expression body `= ...`, or nothing.
            QVariantList parameters;
            QVariantList members;
            QString returns;
            int end = cursor;
            int bodyOpen = -1;
            int bodyClose = -1;
            if (kind == QStringLiteral("property") || kind == QStringLiteral("constant") || kind == QStringLiteral("type")) {
                end = kotlinStatementEnd(clean, cursor, limit);
            } else {
                int scan = cursor;
                auto skipSpace = [&]() {
                    while (scan < limit && (clean.at(scan) == QLatin1Char(' ') || clean.at(scan) == QLatin1Char('\t'))) {
                        ++scan;
                    }
                };
                skipSpace();
                if (scan < limit && clean.at(scan) == QLatin1Char('<')) {
                    const int close = kotlinMatchingClose(clean, scan, limit);
                    scan = close > 0 ? close + 1 : scan;
                    skipSpace();
                }
                // primary constructor modifiers: class Foo private constructor(...)
                static const QRegularExpression constructorKeyword(QStringLiteral(R"(^(?:(?:private|public|internal|protected)\s+)?(?:@\w+\s+)*constructor\b)"));
                const auto constructorMatch = constructorKeyword.match(clean.mid(scan, 80));
                if (kind != QStringLiteral("function") && kind != QStringLiteral("method") && kind != QStringLiteral("constructor")
                    && constructorMatch.hasMatch()) {
                    scan += constructorMatch.capturedEnd(0);
                    skipSpace();
                }
                if (scan < limit && clean.at(scan) == QLatin1Char('(')) {
                    const int close = kotlinMatchingClose(clean, scan, limit);
                    const QString list = close > 0 ? clean.mid(scan + 1, close - scan - 1) : QString();
                    for (const QString &raw : kotlinSplitTopLevel(list.simplified())) {
                        static const QRegularExpression parameter(QStringLiteral(
                            R"(^(?:@[\w.]+(?:\([^)]*\))?\s+)*((?:(?:vararg|noinline|crossinline|private|public|protected|internal|override|open)\s+)*)(val\s+|var\s+)?([A-Za-z_]\w*)\s*:\s*(.+?)(?:\s*=\s*(.+))?$)"));
                        const auto parameterMatch = parameter.match(raw);
                        if (!parameterMatch.hasMatch()) {
                            continue;
                        }
                        const QString parameterName = parameterMatch.captured(3);
                        QString type = parameterMatch.captured(4).trimmed();
                        if (parameterMatch.captured(1).contains(QStringLiteral("vararg"))) {
                            type = QStringLiteral("vararg ") + type;
                        }
                        QVariantMap signatureParameter = makeSignatureParameter(parameterName, type);
                        if (!parameterMatch.captured(5).isEmpty()) {
                            signatureParameter.insert(QStringLiteral("default"), parameterMatch.captured(5).trimmed().left(60));
                        }
                        parameters.append(signatureParameter);
                        // class Foo(val a: Int): constructor properties
                        if (!parameterMatch.captured(2).isEmpty() && kind != QStringLiteral("function") && kind != QStringLiteral("method")) {
                            members.append(makeSymbol(QStringLiteral("property"), parameterName, lineNumberAtOffset(text, scan),
                                                      (parameterMatch.captured(2).trimmed() + QStringLiteral(": ") + type).simplified(), {},
                                                      snippetFromLine(text, lineNumberAtOffset(text, scan), 0)));
                        }
                    }
                    scan = close > 0 ? close + 1 : scan + 1;
                    skipSpace();
                }
                // `: ReturnType` (functions) or `: Supertypes` (types), up to the body.
                int headerEnd = scan;
                int parenDepth = 0;
                for (; headerEnd < limit; ++headerEnd) {
                    const QChar ch = clean.at(headerEnd);
                    if (ch == QLatin1Char('(') || ch == QLatin1Char('<')) {
                        ++parenDepth;
                    } else if ((ch == QLatin1Char(')') || ch == QLatin1Char('>')) && parenDepth > 0) {
                        --parenDepth;
                    } else if (parenDepth == 0 && (ch == QLatin1Char('{') || ch == QLatin1Char('='))) {
                        break;
                    } else if (parenDepth == 0 && ch == QLatin1Char('\n')) {
                        // A header can continue on the next line with ':' or 'where'.
                        int next = headerEnd + 1;
                        while (next < limit && clean.at(next).isSpace()) {
                            ++next;
                        }
                        if (next < limit && (clean.at(next) == QLatin1Char('{') || clean.at(next) == QLatin1Char(':')
                                             || clean.mid(next, 5) == QStringLiteral("where") || clean.at(next) == QLatin1Char('='))) {
                            continue;
                        }
                        break;
                    }
                }
                const QString tail = clean.mid(scan, headerEnd - scan).simplified();
                if (tail.startsWith(QLatin1Char(':'))) {
                    const QString typeText = tail.mid(1).section(QStringLiteral(" where "), 0, 0).trimmed();
                    if (kind == QStringLiteral("function") || kind == QStringLiteral("method")) {
                        returns = typeText;
                    } else if (!typeText.isEmpty()) {
                        detail = (detail + QStringLiteral(" : ") + typeText).simplified();
                    }
                }
                if (headerEnd < limit && clean.at(headerEnd) == QLatin1Char('{')) {
                    bodyOpen = headerEnd;
                    bodyClose = kotlinMatchingClose(clean, headerEnd, limit);
                    if (bodyClose < 0) {
                        bodyClose = qMax(bodyOpen, limit - 1); // unbalanced: stop at the next top-level declaration
                    }
                    end = bodyClose;
                } else if (headerEnd < limit && clean.at(headerEnd) == QLatin1Char('=')) {
                    bodyOpen = headerEnd;
                    int expression = headerEnd + 1;
                    while (expression < limit && clean.at(expression).isSpace()) {
                        ++expression; // `=` then the expression on the next line
                    }
                    end = kotlinStatementEnd(clean, expression, limit);
                    bodyClose = end;
                    if (returns.isEmpty() && (kind == QStringLiteral("function") || kind == QStringLiteral("method"))) {
                        returns = QStringLiteral("inferred");
                    }
                } else {
                    end = headerEnd;
                }
            }

            const bool typeLike = kind == QStringLiteral("class") || kind == QStringLiteral("interface")
                || kind == QStringLiteral("object") || kind == QStringLiteral("enum") || kind == QStringLiteral("annotation");
            if (typeLike && bodyOpen >= 0 && clean.at(bodyOpen) == QLatin1Char('{')) {
                int membersFrom = bodyOpen + 1;
                if (kind == QStringLiteral("enum")) {
                    // Entries come first, up to ';' (or the body end).
                    int entriesEnd = bodyClose;
                    int nested = 0;
                    for (int i = bodyOpen + 1; i < bodyClose; ++i) {
                        const QChar ch = clean.at(i);
                        if (ch == QLatin1Char('(') || ch == QLatin1Char('{')) {
                            ++nested;
                        } else if ((ch == QLatin1Char(')') || ch == QLatin1Char('}')) && nested > 0) {
                            --nested;
                        } else if (ch == QLatin1Char(';') && nested == 0) {
                            entriesEnd = i;
                            break;
                        }
                    }
                    static const QRegularExpression entry(QStringLiteral(R"((?:^|,)\s*(?:@\w+\s+)*([A-Z_][\w]*)\s*(?=[,({]|$))"));
                    QString entries = clean.mid(bodyOpen + 1, entriesEnd - bodyOpen - 1);
                    // drop argument lists and entry bodies
                    QString flat;
                    int nest = 0;
                    for (const QChar ch : std::as_const(entries)) {
                        if (ch == QLatin1Char('(') || ch == QLatin1Char('{')) {
                            ++nest;
                        } else if ((ch == QLatin1Char(')') || ch == QLatin1Char('}')) && nest > 0) {
                            --nest;
                        } else if (nest == 0) {
                            flat += ch;
                        }
                    }
                    for (const QString &part : flat.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                        const QString entryName = part.trimmed().section(QLatin1Char(' '), -1);
                        static const QRegularExpression entryNamePattern(QStringLiteral(R"(^[A-Za-z_]\w*$)"));
                        if (entryNamePattern.match(entryName).hasMatch()) {
                            const int entryOffset = clean.indexOf(entryName, bodyOpen);
                            members.append(makeSymbol(QStringLiteral("enum member"), entryName,
                                                      lineNumberAtOffset(text, entryOffset < 0 ? bodyOpen : entryOffset), QString(), {},
                                                      snippetFromLine(text, lineNumberAtOffset(text, entryOffset < 0 ? bodyOpen : entryOffset), 0)));
                        }
                    }
                    membersFrom = entriesEnd < bodyClose ? entriesEnd + 1 : bodyClose;
                }
                members += parseScope(membersFrom, bodyClose, depth + 1, name);
            }

            QVariantMap symbol = makeSymbol(kind, name, line, detail, members, snippetBetween(start, end));
            symbol.insert(QStringLiteral("endLine"), lineNumberAtOffset(text, qMax(start, end)));
            if (kind == QStringLiteral("function") || kind == QStringLiteral("method") || kind == QStringLiteral("constructor")) {
                symbol.insert(QStringLiteral("parameters"), parameters);
                symbol.insert(QStringLiteral("returns"), QVariantList{QVariantMap{{QStringLiteral("text"),
                                                                                  returns.isEmpty() ? QStringLiteral("Unit") : returns}}});
                symbol.insert(QStringLiteral("signatureSource"), QStringLiteral("parser"));
                if (bodyOpen >= 0) {
                    bodies.append({symbolKey(symbol), bodyOpen, qMax(bodyOpen, bodyClose)});
                }
            }
            symbols.append(symbol);
            position = qMax(match.capturedEnd(0), end + 1);
        }
        return symbols;
    };

    QVariantList symbols = parseScope(0, clean.size(), 0, QString());

    // Calls: same-file relations and raw call sites for the project index.
    QHash<QString, QVariantMap> byKey;
    QHash<QString, QStringList> keysByName;
    collectSymbolsByKey(symbols, byKey, keysByName);
    QHash<QString, QVariantList> callsByKey;
    QHash<QString, QVariantList> calledByByKey;
    QHash<QString, QVariantList> sitesByKey;
    static const QRegularExpression call(QStringLiteral(R"((?:([A-Za-z_]\w*)\s*(?:\?\.|\.)\s*)?\b([A-Za-z_]\w*)\s*(?:<[^<>()\n]*>)?\s*\()"));
    static const QSet<QString> keywords = {
        QStringLiteral("if"), QStringLiteral("when"), QStringLiteral("for"), QStringLiteral("while"), QStringLiteral("return"),
        QStringLiteral("catch"), QStringLiteral("fun"), QStringLiteral("constructor"), QStringLiteral("super"), QStringLiteral("this"),
        QStringLiteral("listOf"), QStringLiteral("mapOf"), QStringLiteral("setOf"), QStringLiteral("println"), QStringLiteral("require"),
        QStringLiteral("check"), QStringLiteral("error"), QStringLiteral("TODO"),
    };
    for (const KotlinBody &body : std::as_const(bodies)) {
        QVariantList sites;
        QSet<QString> seen;
        auto it = call.globalMatch(clean.mid(body.open, body.close - body.open + 1));
        while (it.hasNext()) {
            const auto match = it.next();
            const QString name = match.captured(2);
            if (keywords.contains(name)) {
                continue;
            }
            QString qualifier = match.captured(1);
            if (qualifier.isEmpty()) {
                // `Cart("x").add(...)`, `list?.map(...)`: a receiver that is an expression.
                int before = body.open + match.capturedStart(2) - 1;
                while (before > body.open && clean.at(before).isSpace()) {
                    --before;
                }
                if (before > body.open && clean.at(before) == QLatin1Char('.')) {
                    qualifier = QStringLiteral("(expression)");
                }
            }
            const QString id = qualifier + QLatin1Char('|') + name;
            if (!seen.contains(id) && sites.size() < 200) {
                seen.insert(id);
                QVariantMap site{{QStringLiteral("name"), name},
                                 {QStringLiteral("line"), lineNumberAtOffset(text, body.open + match.capturedStart(2))}};
                if (!qualifier.isEmpty()) {
                    site.insert(QStringLiteral("qualifier"), qualifier);
                }
                sites.append(site);
            }
            if (qualifier.isEmpty() || qualifier == QStringLiteral("this")) {
                const QStringList candidates = keysByName.value(name);
                if (candidates.isEmpty()) {
                    continue;
                }
                const QString targetKey = bestRelationTargetKey(candidates, byKey);
                if (targetKey.isEmpty() || targetKey == body.key || !byKey.contains(targetKey)) {
                    continue;
                }
                const QString targetKind = byKey.value(targetKey).value(QStringLiteral("kind")).toString();
                if (!isCallableSymbolKind(targetKind) && targetKind != QStringLiteral("class")) {
                    continue;
                }
                appendUniqueRelation(callsByKey, body.key, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
                appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(body.key)), QStringLiteral("called by"));
            }
        }
        sitesByKey.insert(body.key, sites);
    }
    symbols = applyRelationsToSymbols(symbols, callsByKey, calledByByKey);
    std::function<QVariantList(QVariantList)> attachSites = [&](QVariantList list) {
        for (int index = 0; index < list.size(); ++index) {
            QVariantMap symbol = list.at(index).toMap();
            const QVariantList sites = sitesByKey.value(symbolKey(symbol));
            if (!sites.isEmpty()) {
                symbol.insert(QStringLiteral("callSites"), sites);
            }
            symbol.insert(QStringLiteral("members"), attachSites(symbol.value(QStringLiteral("members")).toList()));
            list[index] = symbol;
        }
        return list;
    };
    symbols = attachSites(symbols);

    // Imports, resolved like Java (package -> source root, sibling source sets).
    QVariantList dependencies;
    static const QRegularExpression importPattern(QStringLiteral(R"(^[ \t]*import\s+([\w`]+(?:\.[\w`]+)*(?:\.\*)?)(?:\s+as\s+(\w+))?)"),
                                                  QRegularExpression::MultilineOption);
    static const QRegularExpression packagePattern(QStringLiteral(R"(^[ \t]*package\s+([\w.]+))"), QRegularExpression::MultilineOption);
    const QString package = packagePattern.match(clean).captured(1);
    QStringList sourceRoots;
    {
        QDir root = QFileInfo(path).absoluteDir();
        const QStringList packageParts = package.split(QLatin1Char('.'), Qt::SkipEmptyParts);
        // Kotlin does not require directories to match packages: only use
        // the package root when they do.
        bool matches = true;
        QDir probe = root;
        for (int index = packageParts.size() - 1; index >= 0 && matches; --index) {
            matches = probe.dirName() == packageParts.at(index) && probe.cdUp();
        }
        if (matches) {
            root = probe;
        }
        sourceRoots.append(root.absolutePath());
        QDir sets(root.absolutePath());
        if (sets.cdUp() && sets.cdUp() && sets.dirName() == QStringLiteral("src")) {
            for (const QString &set : sets.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
                for (const QString &flavour : {QStringLiteral("kotlin"), QStringLiteral("java")}) {
                    const QString candidate = sets.filePath(set + QLatin1Char('/') + flavour);
                    if (!sourceRoots.contains(candidate) && QFileInfo(candidate).isDir()) {
                        sourceRoots.append(candidate);
                    }
                }
            }
        }
    }
    auto importIt = importPattern.globalMatch(clean);
    while (importIt.hasNext()) {
        const auto match = importIt.next();
        const QString target = match.captured(1).remove(QLatin1Char('`'));
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        QVariantMap item = makeSourceContextItem(path, QStringLiteral("kotlin"), line, snippetFromLine(text, line, 0),
                                                 QStringLiteral("import"));
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), QStringLiteral("import"));
        item.insert(QStringLiteral("label"), target);
        QString resolved;
        if (!target.endsWith(QStringLiteral(".*"))) {
            QStringList parts = target.split(QLatin1Char('.'));
            for (int length = parts.size(); length > 0 && resolved.isEmpty() && length >= parts.size() - 1; --length) {
                for (const QString &root : std::as_const(sourceRoots)) {
                    for (const QString &suffix : {QStringLiteral(".kt"), QStringLiteral(".java")}) {
                        const QString candidate = root + QLatin1Char('/') + parts.mid(0, length).join(QLatin1Char('/')) + suffix;
                        if (resolved.isEmpty() && QFileInfo(candidate).isFile()) {
                            resolved = QFileInfo(candidate).absoluteFilePath();
                        }
                    }
                }
            }
            const QString local = match.captured(2).isEmpty() ? parts.last() : match.captured(2);
            if (!resolved.isEmpty()) {
                item.insert(QStringLiteral("bindings"), QVariantList{QVariantMap{{QStringLiteral("local"), local},
                                                                                 {QStringLiteral("imported"), parts.last()}}});
            }
        }
        item.insert(QStringLiteral("path"), resolved);
        item.insert(QStringLiteral("exists"), true);
        dependencies.append(item);
    }

    // Routes: Spring mapping annotations and Ktor routing blocks.
    QVariantList routes;
    static const QRegularExpression springMapping(QStringLiteral(
        R"re(@(Get|Post|Put|Delete|Patch|Request)Mapping\s*(?:\(\s*(?:value\s*=\s*|path\s*=\s*)?\[?\s*"([^"]*)")?)re"));
    auto springIt = springMapping.globalMatch(text);
    QString classPrefix;
    while (springIt.hasNext()) {
        const auto match = springIt.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        // A class-level @RequestMapping sets the prefix for the methods below it.
        const QString following = clean.mid(match.capturedEnd(0), 300);
        static const QRegularExpression classFollows(QStringLiteral(R"(^[^\n]*\n(?:\s*@[^\n]*\n)*\s*(?:(?:open|data|abstract|public|internal)\s+)*class\b)"));
        if (match.captured(1) == QStringLiteral("Request") && classFollows.match(following).hasMatch()) {
            classPrefix = match.captured(2);
            continue;
        }
        const QString method = match.captured(1) == QStringLiteral("Request") ? QStringLiteral("ANY") : match.captured(1).toUpper();
        QString routePath = classPrefix + match.captured(2);
        if (routePath.isEmpty()) {
            routePath = QStringLiteral("/");
        }
        QVariantMap route = makeSourceContextItem(path, QStringLiteral("kotlin"), line, snippetFromLine(text, line, 2), QStringLiteral("route"));
        route.insert(QStringLiteral("method"), method);
        route.insert(QStringLiteral("path"), routePath);
        route.insert(QStringLiteral("label"), method + QLatin1Char(' ') + routePath);
        routes.append(route);
    }
    // Ktor: route("/a") { get("/b") { ... } } -> GET /a/b
    static const QRegularExpression ktorBlock(QStringLiteral(R"re(\b(route|get|post|put|delete|patch|head|options)\s*\(\s*"([^"]*)"[^)]*\)\s*\{)re"));
    struct Prefix { int close; QString path; };
    QVector<Prefix> prefixes;
    const bool usesKtor = text.contains(QStringLiteral("io.ktor"));
    auto ktorIt = ktorBlock.globalMatch(text);
    while (usesKtor && ktorIt.hasNext()) {
        const auto match = ktorIt.next();
        const int open = match.capturedEnd(0) - 1;
        while (!prefixes.isEmpty() && prefixes.constLast().close < open) {
            prefixes.removeLast();
        }
        QString prefix;
        for (const Prefix &entry : std::as_const(prefixes)) {
            prefix += entry.path;
        }
        QString segment = match.captured(2);
        if (!segment.isEmpty() && !segment.startsWith(QLatin1Char('/'))) {
            segment.prepend(QLatin1Char('/'));
        }
        if (match.captured(1) == QStringLiteral("route")) {
            const int close = kotlinMatchingClose(clean, open, clean.size());
            prefixes.append({close < 0 ? clean.size() : close, segment});
            continue;
        }
        const QString method = match.captured(1).toUpper();
        const QString routePath = (prefix + segment).isEmpty() ? QStringLiteral("/") : prefix + segment;
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        QVariantMap route = makeSourceContextItem(path, QStringLiteral("kotlin"), line, snippetFromLine(text, line, 2), QStringLiteral("route"));
        route.insert(QStringLiteral("method"), method);
        route.insert(QStringLiteral("path"), routePath);
        route.insert(QStringLiteral("label"), method + QLatin1Char(' ') + routePath);
        routes.append(route);
    }

    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), dependencies);
    result.insert(QStringLiteral("routes"), routes);
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parsePhp(const QString &path, const QString &text) const
{
    QVariantList symbols;
    QRegularExpression classPattern(
        QStringLiteral(R"(^\s*((?:abstract\s+|final\s+)?(?:class|trait|interface))\s+([A-Za-z_]\w*)[\s\S]*?\{)"),
        QRegularExpression::MultilineOption);

    QRegularExpression topConstPattern(
        QStringLiteral(R"(^\s*const\s+([A-Z_]\w*)\s*=)"),
        QRegularExpression::MultilineOption);

    auto classIt = classPattern.globalMatch(text);
    while (classIt.hasNext()) {
        const auto match = classIt.next();
        const QString kindText = match.captured(1);
        const QString name = match.captured(2);
        const int openBrace = match.capturedEnd(0) - 1;
        int depth = 1;
        int cursor = openBrace + 1;
        while (cursor < text.size() && depth > 0) {
            if (text.at(cursor) == QLatin1Char('{')) {
                ++depth;
            } else if (text.at(cursor) == QLatin1Char('}')) {
                --depth;
            }
            ++cursor;
        }
        const QString body = text.mid(openBrace + 1, qMax(0, cursor - openBrace - 2));
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        QString kind = QStringLiteral("class");
        if (kindText.contains(QStringLiteral("trait"))) {
            kind = QStringLiteral("trait");
        } else if (kindText.contains(QStringLiteral("interface"))) {
            kind = QStringLiteral("interface");
        }
        symbols.append(makeSymbol(kind, name, line, QString(), parseClassMembers(body, QStringLiteral("php"))));
    }

    auto constIt = topConstPattern.globalMatch(text);
    while (constIt.hasNext()) {
        const auto match = constIt.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        symbols.append(makeSymbol(QStringLiteral("constant"), match.captured(1), line));
    }

    QVariantMap result;
    result.insert(QStringLiteral("path"), path);
    result.insert(QStringLiteral("fileName"), QFileInfo(path).fileName());
    result.insert(QStringLiteral("language"), QStringLiteral("php"));
    symbols = applyPhpCallbackRelations(symbols);
    symbols = applySnippetCallRelations(symbols);
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("quickLinks"), QVariantList{});
    result.insert(QStringLiteral("cssSummary"), QVariantMap{});
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parseScriptLike(const QString &path, const QString &text, bool reactMode) const
{
    QVariantList symbols;
    auto makePartialScriptSymbol = [&](const QString &kind, const QString &name, int line,
                                       const QString &detail, const QVariantList &members,
                                       const QString &snippet, const QString &snippetKind) {
        QString normalizedSnippet = snippet;
        if (snippetKind == QStringLiteral("block_excerpt")) {
            QStringList lines = normalizedSnippet.split(QLatin1Char('\n'));
            while (!lines.isEmpty()) {
                const QString first = lines.first().trimmed();
                if (first.isEmpty() || first == QStringLiteral("}") || first == QStringLiteral("};")
                    || first == QStringLiteral("*/")) {
                    lines.removeFirst();
                    continue;
                }
                break;
            }
            if (!name.isEmpty()) {
                int anchorIndex = -1;
                const QStringList declarationStarts = {
                    QStringLiteral("function ") + name,
                    QStringLiteral("async function ") + name,
                    QStringLiteral("class ") + name,
                    QStringLiteral("const ") + name,
                    QStringLiteral("let ") + name,
                    QStringLiteral("var ") + name
                };

                for (int i = 0; i < lines.size(); ++i) {
                    const QString trimmedLine = lines.at(i).trimmed();
                    for (const QString &declarationStart : declarationStarts) {
                        if (trimmedLine.startsWith(declarationStart)) {
                            anchorIndex = i;
                            break;
                        }
                    }
                    if (anchorIndex >= 0) {
                        break;
                    }
                }

                if (anchorIndex > 0) {
                    lines = lines.mid(anchorIndex);
                }
            }

            normalizedSnippet = lines.join(QLatin1Char('\n')).trimmed();
        }

        QVariantMap symbol = makeSymbol(kind, name, line, detail, members, normalizedSnippet);
        symbol.insert(QStringLiteral("snippetKind"), snippetKind);
        symbol.insert(QStringLiteral("diagnosticsMode"), QStringLiteral("none"));
        return symbol;
    };

    QRegularExpression namedFunctionPattern(
        QStringLiteral(R"((?:export\s+)?function\s+([A-Za-z_]\w*)\s*\()"),
        QRegularExpression::MultilineOption);
    auto namedFunctions = namedFunctionPattern.globalMatch(text);
    while (namedFunctions.hasNext()) {
        const auto match = namedFunctions.next();
        const QString name = match.captured(1);
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        QString kind = name.startsWith(QStringLiteral("use")) ? QStringLiteral("hook") : QStringLiteral("function");
        if (reactMode && !name.isEmpty() && name.at(0).isUpper()) {
            kind = QStringLiteral("component");
        }
        symbols.append(makePartialScriptSymbol(kind, name, line, QString(), {},
                                               snippetFromBraceBlock(text, match.capturedStart(0)),
                                               QStringLiteral("block_excerpt")));
    }

    QRegularExpression classPattern(
        QStringLiteral(R"((?:export\s+)?class\s+([A-Za-z_]\w*)[\s\S]*?\{)"),
        QRegularExpression::MultilineOption);
    auto classes = classPattern.globalMatch(text);
    while (classes.hasNext()) {
        const auto match = classes.next();
        const QString name = match.captured(1);
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        const int openBrace = match.capturedEnd(0) - 1;
        int depth = 1;
        int cursor = openBrace + 1;
        while (cursor < text.size() && depth > 0) {
            if (text.at(cursor) == QLatin1Char('{')) {
                ++depth;
            } else if (text.at(cursor) == QLatin1Char('}')) {
                --depth;
            }
            ++cursor;
        }
        const QString body = text.mid(openBrace + 1, qMax(0, cursor - openBrace - 2));
        symbols.append(makePartialScriptSymbol(QStringLiteral("class"), name, line, QString(),
                                               parseClassMembers(body, QStringLiteral("js")),
                                               snippetFromBraceBlock(text, match.capturedStart(0)),
                                               QStringLiteral("block_excerpt")));
    }

    QRegularExpression objectExportPattern(
        QStringLiteral(R"(^(?:export\s+)?(?:const|let|var)\s+([A-Za-z_]\w*)\s*=\s*\{)"),
        QRegularExpression::MultilineOption);
    auto objectExports = objectExportPattern.globalMatch(text);
    while (objectExports.hasNext()) {
        const auto match = objectExports.next();
        const QString name = match.captured(1);
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        const int openBrace = match.capturedEnd(0) - 1;
        int depth = 1;
        int cursor = openBrace + 1;
        while (cursor < text.size() && depth > 0) {
            if (text.at(cursor) == QLatin1Char('{')) {
                ++depth;
            } else if (text.at(cursor) == QLatin1Char('}')) {
                --depth;
            }
            ++cursor;
        }
        const QString body = text.mid(openBrace + 1, qMax(0, cursor - openBrace - 2));
        const QVariantList members = parseObjectMembers(body);
        QString kind = QStringLiteral("variable");
        if (reactMode && !name.isEmpty() && name.at(0).isUpper()) {
            kind = QStringLiteral("component");
        }
        symbols.append(makePartialScriptSymbol(kind, name, line, QStringLiteral("object export"), members,
                                               snippetFromBraceBlock(text, match.capturedStart(0)),
                                               QStringLiteral("block_excerpt")));
    }

    QRegularExpression commonJsObjectPattern(
        QStringLiteral(R"(module\.exports\s*=\s*\{)"),
        QRegularExpression::MultilineOption);
    auto commonJs = commonJsObjectPattern.globalMatch(text);
    while (commonJs.hasNext()) {
        const auto match = commonJs.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        const int openBrace = match.capturedEnd(0) - 1;
        int depth = 1;
        int cursor = openBrace + 1;
        while (cursor < text.size() && depth > 0) {
            if (text.at(cursor) == QLatin1Char('{')) {
                ++depth;
            } else if (text.at(cursor) == QLatin1Char('}')) {
                --depth;
            }
            ++cursor;
        }
        const QString body = text.mid(openBrace + 1, qMax(0, cursor - openBrace - 2));
        symbols.append(makePartialScriptSymbol(QStringLiteral("module"), QStringLiteral("module.exports"), line,
                                               QStringLiteral("CommonJS export object"), parseObjectMembers(body),
                                               snippetFromBraceBlock(text, match.capturedStart(0)),
                                               QStringLiteral("block_excerpt")));
    }

    QRegularExpression arrowPattern(
        QStringLiteral(R"(^(?:export\s+)?const\s+([A-Za-z_]\w*)\s*=\s*(?:async\s*)?\([^)]*\)\s*=>)"),
        QRegularExpression::MultilineOption);
    auto arrows = arrowPattern.globalMatch(text);
    while (arrows.hasNext()) {
        const auto match = arrows.next();
        const QString name = match.captured(1);
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        QString kind = name.startsWith(QStringLiteral("use")) ? QStringLiteral("hook") : QStringLiteral("function");
        if (reactMode && !name.isEmpty() && name.at(0).isUpper()) {
            kind = QStringLiteral("component");
        }
        symbols.append(makePartialScriptSymbol(kind, name, line, QString(), {},
                                               snippetFromLine(text, line, 0),
                                               QStringLiteral("line_excerpt")));
    }

    QRegularExpression functionExpressionPattern(
        QStringLiteral(R"(^(?:export\s+)?(?:const|let|var)\s+([A-Za-z_]\w*)\s*=\s*(?:async\s*)?function\s*\()"),
        QRegularExpression::MultilineOption);
    auto functionExpressions = functionExpressionPattern.globalMatch(text);
    while (functionExpressions.hasNext()) {
        const auto match = functionExpressions.next();
        const QString name = match.captured(1);
        bool alreadyPresent = false;
        for (const QVariant &symbolValue : std::as_const(symbols)) {
            if (symbolValue.toMap().value(QStringLiteral("name")).toString() == name) {
                alreadyPresent = true;
                break;
            }
        }
        if (alreadyPresent) {
            continue;
        }
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        QString kind = name.startsWith(QStringLiteral("use")) ? QStringLiteral("hook") : QStringLiteral("function");
        if (reactMode && !name.isEmpty() && name.at(0).isUpper()) {
            kind = QStringLiteral("component");
        }
        symbols.append(makePartialScriptSymbol(kind, name, line, QStringLiteral("function expression"), {},
                                               snippetFromBraceBlock(text, match.capturedStart(0)),
                                               QStringLiteral("block_excerpt")));
    }

    QRegularExpression interfacePattern(
        QStringLiteral(R"(^[ \t]*(?:export\s+)?interface\s+([A-Za-z_]\w*))"),
        QRegularExpression::MultilineOption);
    auto interfaces = interfacePattern.globalMatch(text);
    while (interfaces.hasNext()) {
        const auto match = interfaces.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        symbols.append(makePartialScriptSymbol(QStringLiteral("props"), match.captured(1), line, QString(), {},
                                               snippetFromLine(text, line, 0),
                                               QStringLiteral("line_excerpt")));
    }

    QRegularExpression typePattern(
        QStringLiteral(R"(^[ \t]*(?:export\s+)?type\s+([A-Za-z_]\w*)\s*=)"),
        QRegularExpression::MultilineOption);
    auto types = typePattern.globalMatch(text);
    while (types.hasNext()) {
        const auto match = types.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        symbols.append(makePartialScriptSymbol(QStringLiteral("type"), match.captured(1), line, QString(), {},
                                               snippetFromLine(text, line, 0),
                                               QStringLiteral("line_excerpt")));
    }

    QRegularExpression variablePattern(
        QStringLiteral(R"(^(?:export\s+)?(?:const|let|var)\s+([A-Za-z_]\w*)\s*=)"),
        QRegularExpression::MultilineOption);
    auto variables = variablePattern.globalMatch(text);
    while (variables.hasNext()) {
        const auto match = variables.next();
        const QString name = match.captured(1);
        bool alreadyPresent = false;
        for (const QVariant &symbolValue : std::as_const(symbols)) {
            if (symbolValue.toMap().value(QStringLiteral("name")).toString() == name) {
                alreadyPresent = true;
                break;
            }
        }
        if (alreadyPresent) {
            continue;
        }
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        symbols.append(makePartialScriptSymbol(QStringLiteral("variable"), name, line, QString(), {},
                                               snippetFromLine(text, line, 0),
                                               QStringLiteral("line_excerpt")));
    }

    if (!symbols.isEmpty()) {
        QHash<QString, QVariantMap> byKey;
        QHash<QString, QStringList> keysByName;
        QHash<QString, QVariantList> callsByKey;
        QHash<QString, QVariantList> calledByByKey;
        collectSymbolsByKey(symbols, byKey, keysByName);

        std::function<void(const QVariantList &)> collectSnippetRelations = [&](const QVariantList &items) {
            for (const QVariant &entry : items) {
                const QVariantMap symbol = entry.toMap();
                const QString ownerKey = symbolKey(symbol);
                if (!ownerKey.isEmpty()) {
                    const QSet<QString> relationNames = scriptCallNamesFromSnippet(symbol.value(QStringLiteral("snippet")).toString());
                    for (const QString &targetName : relationNames) {
                        const QStringList candidateKeys = keysByName.value(targetName);
                        if (candidateKeys.isEmpty()) {
                            continue;
                        }
                        const QString targetKey = bestRelationTargetKey(candidateKeys, byKey);
                        if (targetKey.isEmpty() || targetKey == ownerKey || !byKey.contains(targetKey)) {
                            continue;
                        }
                        appendUniqueRelation(callsByKey, ownerKey, relationFromSymbol(byKey.value(targetKey)), QStringLiteral("calls"));
                        appendUniqueRelation(calledByByKey, targetKey, relationFromSymbol(byKey.value(ownerKey)), QStringLiteral("called by"));
                    }
                }
                collectSnippetRelations(symbol.value(QStringLiteral("members")).toList());
            }
        };

        collectSnippetRelations(symbols);
        symbols = applyRelationsToSymbols(symbols, callsByKey, calledByByKey);
    }

    QVariantMap result;
    result.insert(QStringLiteral("path"), path);
    result.insert(QStringLiteral("fileName"), QFileInfo(path).fileName());
    result.insert(QStringLiteral("language"), reactMode ? QFileInfo(path).suffix().toLower() : QStringLiteral("script"));
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("quickLinks"), findHtmlConsumersForAsset(path, QStringLiteral("script")));
    result.insert(QStringLiteral("dependencies"), extractDependencyLinks(path, text));
    result.insert(QStringLiteral("routes"), extractExpressRoutes(text));
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("packageSummary"), QVariantMap{});
    result.insert(QStringLiteral("cssSummary"), QVariantMap{});
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parseHtml(const QString &path, const QString &text) const
{
    const WebLinks::HtmlPage page = WebLinks::parseHtmlPage(path, text);
    const QDir dir = QFileInfo(path).dir();
    QVariantList links;
    QVariantList symbols;
    QVariantList dependencies;

    // Linked assets (scripts, stylesheets, pages, form targets, frames).
    for (const WebLinks::HtmlAsset &asset : page.assets) {
        QVariantMap item = makeSourceContextItem(path, QStringLiteral("html"), asset.line, asset.snippet,
                                                 QStringLiteral("%1 link").arg(asset.kind));
        const QString fileName = QFileInfo(asset.target).fileName();
        item.insert(QStringLiteral("label"), fileName.isEmpty() ? asset.target : fileName);
        item.insert(QStringLiteral("target"), asset.target);
        item.insert(QStringLiteral("type"), asset.kind);
        item.insert(QStringLiteral("path"), asset.resolvedPath);
        item.insert(QStringLiteral("targetPath"), asset.resolvedPath);
        item.insert(QStringLiteral("exists"), asset.exists);
        links.append(item);
    }

    // Inline <script> and <style> blocks, parsed with the real JS / CSS parsers
    // on line-aligned text so their symbols point at the right HTML lines.
    QVariantList inlineScriptSymbols;
    for (const WebLinks::HtmlInlineBlock &block : page.inlineBlocks) {
        const QString padded = linePaddedBlock(block.content, block.startLine);
        if (block.kind == QStringLiteral("script")) {
            const QVariantMap analysis = parseScriptLikeTreeSitter(path, padded, QStringLiteral("script"));
            for (const QVariant &entry : analysis.value(QStringLiteral("symbols")).toList()) {
                QVariantMap symbol = entry.toMap();
                const QString detail = symbol.value(QStringLiteral("detail")).toString();
                symbol.insert(QStringLiteral("detail"), detail.isEmpty() ? QStringLiteral("inline <script>")
                                                                         : detail + QStringLiteral(", inline <script>"));
                inlineScriptSymbols.append(symbol);
            }
            for (const QVariant &entry : analysis.value(QStringLiteral("dependencies")).toList()) {
                dependencies.append(entry);
            }
        } else {
            const QVariantMap analysis = parseCssTreeSitter(path, padded);
            const QVariantList cssSymbols = analysis.value(QStringLiteral("symbols")).toList();
            if (!cssSymbols.isEmpty()) {
                symbols.append(makeSymbol(QStringLiteral("style"), QStringLiteral("<style>"), block.startLine,
                                          QStringLiteral("inline stylesheet"), cssSymbols,
                                          snippetFromLine(text, block.startLine, 3)));
            }
        }
    }
    symbols.append(inlineScriptSymbols);

    // Functions a handler can call: inline script functions (this file) and
    // global functions of the linked local scripts.
    QHash<QString, QVariantMap> inlineFunctions;
    for (const QVariant &entry : std::as_const(inlineScriptSymbols)) {
        const QVariantMap symbol = entry.toMap();
        if (isCallableSymbolKind(symbol.value(QStringLiteral("kind")).toString())) {
            inlineFunctions.insert(symbol.value(QStringLiteral("name")).toString(), symbol);
        }
    }
    QList<QPair<QString, QHash<QString, int>>> scriptFunctions;
    for (const WebLinks::HtmlAsset &asset : page.assets) {
        if (asset.kind == QStringLiteral("script") && asset.local && asset.exists) {
            scriptFunctions.append({asset.resolvedPath, WebLinks::globalFunctionsOfScript(asset.resolvedPath)});
        }
    }

    for (const WebLinks::HtmlHandler &handler : page.handlers) {
        QVariantMap symbol = makeSymbol(QStringLiteral("handler"), handlerSymbolName(handler), handler.line,
                                        handler.code, {}, handler.snippet);
        QVariantList calls;
        for (const QString &name : handler.calledNames) {
            if (inlineFunctions.contains(name)) {
                calls.append(relationFromSymbol(inlineFunctions.value(name)));
                continue;
            }
            for (const auto &script : std::as_const(scriptFunctions)) {
                if (script.second.contains(name)) {
                    const int line = script.second.value(name);
                    const QString snippet = snippetFromLine(readScanableScript(script.first), line, 0);
                    calls.append(makeCrossFileRelation(QStringLiteral("function"), name, script.first,
                                                       QStringLiteral("script"), line, snippet,
                                                       QStringLiteral("defined in %1").arg(QFileInfo(script.first).fileName())));
                    break;
                }
            }
        }
        symbol.insert(QStringLiteral("calls"), calls);
        symbols.append(symbol);
        // Reverse edge on inline functions of this file.
        for (const QString &name : handler.calledNames) {
            if (!inlineFunctions.contains(name)) {
                continue;
            }
            const QVariantMap target = inlineFunctions.value(name);
            for (int index = 0; index < symbols.size(); ++index) {
                QVariantMap candidate = symbols.at(index).toMap();
                if (symbolKey(candidate) == symbolKey(target)) {
                    QVariantList calledBy = candidate.value(QStringLiteral("calledBy")).toList();
                    calledBy.append(relationFromSymbol(symbol));
                    candidate.insert(QStringLiteral("calledBy"), calledBy);
                    symbols[index] = candidate;
                }
            }
        }
    }

    // Elements with ids, forms and custom elements.
    for (const WebLinks::HtmlElement &element : page.elements) {
        if (element.id.isEmpty()) {
            continue;
        }
        QString detail = QStringLiteral("<%1>").arg(element.tag);
        if (!element.classes.isEmpty()) {
            detail += QStringLiteral(" .") + element.classes.join(QStringLiteral(" ."));
        }
        symbols.append(makeSymbol(QStringLiteral("element"), QStringLiteral("#") + element.id, element.line,
                                  detail, {}, element.snippet));
    }
    QSet<QString> seenCustomTags;
    for (const WebLinks::HtmlElement &element : page.customElements) {
        if (seenCustomTags.contains(element.tag)) {
            continue;
        }
        seenCustomTags.insert(element.tag);
        symbols.append(makeSymbol(QStringLiteral("component"), QStringLiteral("<%1>").arg(element.tag), element.line,
                                  QStringLiteral("custom element"), {}, element.snippet));
    }
    for (const WebLinks::HtmlAsset &asset : page.assets) {
        if (asset.kind == QStringLiteral("form")) {
            symbols.append(makeSymbol(QStringLiteral("form"), asset.target, asset.line,
                                      QStringLiteral("form action"), {}, asset.snippet));
        }
    }
    std::stable_sort(symbols.begin(), symbols.end(), [](const QVariant &left, const QVariant &right) {
        return left.toMap().value(QStringLiteral("line")).toInt() < right.toMap().value(QStringLiteral("line")).toInt();
    });

    // Where linked scripts use this page's ids (reverse of the JS-side links).
    int usageLinks = 0;
    for (const auto &script : std::as_const(scriptFunctions)) {
        const QString scriptText = readScanableScript(script.first);
        if (scriptText.isEmpty()) {
            continue;
        }
        QSet<QString> seenIds;
        for (const WebLinks::DomReference &ref : WebLinks::extractDomReferences(scriptText)) {
            if (usageLinks >= 60 || ref.kind != QStringLiteral("id") || seenIds.contains(ref.name)
                || !page.elementWithId(ref.name)) {
                continue;
            }
            seenIds.insert(ref.name);
            links.append(makeWebLinkItem(QStringLiteral("dom-id-use"),
                                         QStringLiteral("#%1 ← %2").arg(ref.name, fileLineLabel(script.first, ref.line)),
                                         script.first, QStringLiteral("script"), ref.line, ref.snippet,
                                         ref.via));
            ++usageLinks;
        }
    }

    // Class usage vs available CSS (linked sheets, inline styles, sibling sheets).
    const QStringList usedClasses = page.usedClasses();
    QMap<QString, QVariantMap> availableClassEntries = cssClassesForPage(page);
    bool hasLocalCssSource = !availableClassEntries.isEmpty();
    for (const WebLinks::HtmlAsset &asset : page.assets) {
        if (asset.kind == QStringLiteral("stylesheet") && asset.local && asset.exists) {
            hasLocalCssSource = true;
        }
    }
    const QFileInfoList siblings = dir.entryInfoList({QStringLiteral("*.css")}, QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : siblings) {
        hasLocalCssSource = true;
        const QMap<QString, QVariantMap> index = cachedCssClassIndex(entry.absoluteFilePath());
        for (auto it = index.constBegin(); it != index.constEnd(); ++it) {
            if (!availableClassEntries.contains(it.key())) {
                availableClassEntries.insert(it.key(), it.value());
            }
        }
    }
    QVariantList matchedClasses;
    QVariantList missingClasses;
    for (const QString &name : usedClasses) {
        if (availableClassEntries.contains(name)) {
            matchedClasses.append(availableClassEntries.value(name));
        } else {
            const QList<const WebLinks::HtmlElement *> elements = page.elementsWithClass(name);
            const int line = elements.isEmpty() ? 0 : elements.first()->line;
            missingClasses.append(makeCssClassSummaryEntry(name, false, path, line,
                                                           elements.isEmpty() ? QString() : elements.first()->snippet));
        }
    }
    QVariantMap cssSummary;
    if (hasLocalCssSource && (!usedClasses.isEmpty() || !availableClassEntries.isEmpty())) {
        cssSummary.insert(QStringLiteral("usedClasses"), toVariantList(usedClasses));
        cssSummary.insert(QStringLiteral("matchedClasses"), matchedClasses);
        cssSummary.insert(QStringLiteral("missingClasses"), missingClasses);
        cssSummary.insert(QStringLiteral("availableClasses"), toVariantList(availableClassEntries.keys()));
    }

    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("html"));
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("quickLinks"), links);
    result.insert(QStringLiteral("dependencies"), dependencies);
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("cssSummary"), cssSummary);
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 elements with ids, %2 handlers, %3 quick links, %4 classes used")
                                                 .arg(std::count_if(page.elements.cbegin(), page.elements.cend(),
                                                                    [](const WebLinks::HtmlElement &e) { return !e.id.isEmpty(); }))
                                                 .arg(page.handlers.size())
                                                 .arg(links.size())
                                                 .arg(usedClasses.size()));
    return result;
}

QVariantMap SymbolParser::parseQml(const QString &path, const QString &text) const
{
    QVariantList symbols;
    QVariantList dependencies;
    const QFileInfo fileInfo(path);
    const QDir dir = fileInfo.dir();
    QSet<QString> seenDependencyKeys;

    auto appendDependency = [&](const QString &target, const QString &type, int line,
                                const QString &detail = QString()) {
        const QString key = type + QLatin1Char('|') + target;
        if (target.isEmpty() || seenDependencyKeys.contains(key)) {
            return;
        }
        seenDependencyKeys.insert(key);
        QVariantMap item = makeSourceContextItem(path, QStringLiteral("qml"), line,
                                                 snippetFromLine(text, line, 0),
                                                 detail.isEmpty() ? QStringLiteral("qml import") : detail);
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("label"), target);
        if (target.startsWith(QStringLiteral("./")) || target.startsWith(QStringLiteral("../"))) {
            const QString resolved = QDir::cleanPath(dir.filePath(target));
            item.insert(QStringLiteral("path"), resolved);
            item.insert(QStringLiteral("exists"), QFileInfo::exists(resolved));
        } else {
            item.insert(QStringLiteral("path"), QString());
            item.insert(QStringLiteral("exists"), true);
        }
        dependencies.append(item);
    };

    const QRegularExpression importPattern(
        QStringLiteral("^\\s*import\\s+(?:\"([^\"]+)\"|'([^']+)'|([A-Za-z_][A-Za-z0-9_.]*))(?:\\s+([0-9]+(?:\\.[0-9]+)?))?(?:\\s+as\\s+([A-Za-z_]\\w*))?"),
        QRegularExpression::MultilineOption);
    auto imports = importPattern.globalMatch(text);
    while (imports.hasNext()) {
        const auto match = imports.next();
        const QString quotedTarget = !match.captured(1).isEmpty() ? match.captured(1) : match.captured(2);
        const QString moduleTarget = match.captured(3);
        const QString version = match.captured(4);
        const QString alias = match.captured(5);
        QString target = !quotedTarget.isEmpty() ? quotedTarget : moduleTarget;
        QString detail = QStringLiteral("qml import");
        if (!version.isEmpty()) {
            detail += QStringLiteral(" %1").arg(version);
        }
        if (!alias.isEmpty()) {
            detail += QStringLiteral(" as %1").arg(alias);
        }
        appendDependency(target, QStringLiteral("import"),
                         lineNumberAtOffset(text, match.capturedStart(0)), detail);
    }

    const QRegularExpression inlineComponentPattern(
        QStringLiteral(R"(^\s*component\s+([A-Z][A-Za-z0-9_]*)\s*:\s*([A-Z][A-Za-z0-9_.]*))"),
        QRegularExpression::MultilineOption);
    auto inlineComponents = inlineComponentPattern.globalMatch(text);
    while (inlineComponents.hasNext()) {
        const auto match = inlineComponents.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        symbols.append(makeSymbol(QStringLiteral("component"),
                                  match.captured(1),
                                  line,
                                  QStringLiteral("inline %1").arg(match.captured(2)),
                                  {},
                                  snippetFromLine(text, line, 1)));
    }

    QVariantList rootMembers;
    QSet<QString> seenRootMemberNames;
    auto appendRootMember = [&](const QVariantMap &member) {
        const QString key = member.value(QStringLiteral("kind")).toString()
            + QLatin1Char('|')
            + member.value(QStringLiteral("name")).toString();
        if (member.value(QStringLiteral("name")).toString().isEmpty() || seenRootMemberNames.contains(key)) {
            return;
        }
        seenRootMemberNames.insert(key);
        rootMembers.append(member);
    };

    const QRegularExpression idPattern(QStringLiteral(R"(^\s*id\s*:\s*([A-Za-z_]\w*))"),
                                       QRegularExpression::MultilineOption);
    auto ids = idPattern.globalMatch(text);
    while (ids.hasNext()) {
        const auto match = ids.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        appendRootMember(makeSymbol(QStringLiteral("property"), match.captured(1), line,
                                    QStringLiteral("id"), {}, snippetFromLine(text, line, 0)));
    }

    const QRegularExpression propertyPattern(
        QStringLiteral(R"(^\s*(?:default\s+)?(?:readonly\s+)?property\s+([A-Za-z_][A-Za-z0-9_<>\[\].]*)\s+([A-Za-z_]\w*))"),
        QRegularExpression::MultilineOption);
    auto properties = propertyPattern.globalMatch(text);
    while (properties.hasNext()) {
        const auto match = properties.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        appendRootMember(makeSymbol(QStringLiteral("property"), match.captured(2), line,
                                    match.captured(1), {}, snippetFromLine(text, line, 0)));
    }

    const QRegularExpression signalPattern(QStringLiteral(R"(^\s*signal\s+([A-Za-z_]\w*)\s*\()"),
                                           QRegularExpression::MultilineOption);
    auto signalMatches = signalPattern.globalMatch(text);
    while (signalMatches.hasNext()) {
        const auto match = signalMatches.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        appendRootMember(makeSymbol(QStringLiteral("signal"), match.captured(1), line,
                                    QString(), {}, snippetFromLine(text, line, 0)));
    }

    const QRegularExpression functionPattern(QStringLiteral(R"(^\s*function\s+([A-Za-z_]\w*)\s*\()"),
                                             QRegularExpression::MultilineOption);
    auto functions = functionPattern.globalMatch(text);
    while (functions.hasNext()) {
        const auto match = functions.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        appendRootMember(makeSymbol(QStringLiteral("function"), match.captured(1), line,
                                    QString(), {}, snippetFromBraceBlock(text, match.capturedStart(0))));
    }

    const QRegularExpression handlerPattern(QStringLiteral(R"(^\s*(on[A-Z][A-Za-z0-9_]*)\s*:)"),
                                            QRegularExpression::MultilineOption);
    auto handlers = handlerPattern.globalMatch(text);
    while (handlers.hasNext()) {
        const auto match = handlers.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        // Block handlers (onClicked: { ... }) own their whole block.
        const int lineEnd = text.indexOf(QLatin1Char('\n'), match.capturedEnd(0));
        const QString rest = text.mid(match.capturedEnd(0), (lineEnd < 0 ? text.size() : lineEnd) - match.capturedEnd(0));
        const QString handlerSnippet = rest.contains(QLatin1Char('{'))
            ? snippetFromBraceBlock(text, match.capturedStart(0))
            : snippetFromLine(text, line, 0);
        appendRootMember(makeSymbol(QStringLiteral("method"), match.captured(1), line,
                                    QStringLiteral("signal handler"), {}, handlerSnippet));
    }

    const QRegularExpression rootComponentPattern(
        QStringLiteral(R"(^\s*([A-Z][A-Za-z0-9_.]*)\s*\{)"),
        QRegularExpression::MultilineOption);
    const auto rootComponent = rootComponentPattern.match(text);
    if (rootComponent.hasMatch()) {
        const int start = rootComponent.capturedStart(0);
        symbols.prepend(makeSymbol(QStringLiteral("component"),
                                   rootComponent.captured(1),
                                   lineNumberAtOffset(text, start),
                                   QStringLiteral("qml root"),
                                   rootMembers,
                                   snippetFromBraceBlock(text, start)));
    } else {
        for (const QVariant &member : std::as_const(rootMembers)) {
            symbols.append(member);
        }
    }

    symbols = applySnippetCallRelations(symbols);

    QVariantMap result = makeResultSkeleton(path, fileInfo.fileName(), QStringLiteral("qml"));
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), dependencies);
    result.insert(QStringLiteral("summary"),
                  QStringLiteral("%1 symbols, %2 imports")
                      .arg(symbols.size())
                      .arg(dependencies.size()));
    return result;
}

QVariantMap SymbolParser::parseCss(const QString &path, const QString &text) const
{
    QVariantList symbols;

    QRegularExpression classPattern(QStringLiteral(R"(\.([A-Za-z_-][\w-]*))"));
    auto classIt = classPattern.globalMatch(text);
    while (classIt.hasNext()) {
        const auto match = classIt.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        const QString snippet = text.mid(match.capturedStart(0), 100).split(QLatin1Char('\n')).at(0) + QStringLiteral(" { ... }");
        symbols.append(makeSymbol(QStringLiteral("class"), match.captured(1), line, QString(), {}, snippet));
    }

    QRegularExpression varPattern(QStringLiteral(R"((--[A-Za-z_-][\w-]*)\s*:)"));
    auto varIt = varPattern.globalMatch(text);
    while (varIt.hasNext()) {
        const auto match = varIt.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        const QString snippet = text.mid(match.capturedStart(0), 100).split(QLatin1Char('\n')).at(0);
        symbols.append(makeSymbol(QStringLiteral("custom-property"), match.captured(1), line, QString(), {}, snippet));
    }

    QVariantMap result;
    result.insert(QStringLiteral("path"), path);
    result.insert(QStringLiteral("fileName"), QFileInfo(path).fileName());
    result.insert(QStringLiteral("language"), QStringLiteral("css"));
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), QVariantList{});
    result.insert(QStringLiteral("routes"), QVariantList{});
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("packageSummary"), QVariantMap{});
    enrichCssAnalysisWithHtmlUsage(result, path, text);
    return result;
}

QVariantMap SymbolParser::parseJson(const QString &path, const QString &text) const
{
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("json"));
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));

    const QString fileName = QFileInfo(path).fileName();
    if (fileName == QStringLiteral("package.json")) {
        const QVariantMap packageSummary = extractPackageSummary(path, text);
        result.insert(QStringLiteral("packageSummary"), packageSummary);

        QVariantList symbols;
        const QVariantMap scripts = packageSummary.value(QStringLiteral("scripts")).toMap();
        for (auto it = scripts.constBegin(); it != scripts.constEnd(); ++it) {
            symbols.append(makeSymbol(QStringLiteral("script"), it.key(), 1, it.value().toString()));
        }
        result.insert(QStringLiteral("symbols"), symbols);
        result.insert(QStringLiteral("summary"),
                      QStringLiteral("%1 scripts, %2 dependencies")
                          .arg(scripts.size())
                          .arg(packageSummary.value(QStringLiteral("dependencyCount")).toInt()));
        return result;
    }

    if (fileName.contains(QStringLiteral("openapi"), Qt::CaseInsensitive)) {
        const auto doc = QJsonDocument::fromJson(text.toUtf8());
        if (doc.isObject()) {
            const QJsonObject root = doc.object();
            QVariantList symbols;
            const QJsonObject paths = root.value(QStringLiteral("paths")).toObject();
            for (auto it = paths.constBegin(); it != paths.constEnd(); ++it) {
                const QJsonObject operations = it.value().toObject();
                for (auto opIt = operations.constBegin(); opIt != operations.constEnd(); ++opIt) {
                    const QString method = opIt.key().toUpper();
                    const QString routePath = it.key();
                    symbols.append(makeSymbol(QStringLiteral("route"), method + " " + routePath, 1));
                }
            }
            result.insert(QStringLiteral("symbols"), symbols);
            result.insert(QStringLiteral("summary"),
                          QStringLiteral("%1 documented API operations").arg(symbols.size()));
        }
    }

    return result;
}

QString SymbolParser::detectLanguage(const QString &path)
{
    if (QFileInfo(path).fileName() == QStringLiteral("package.json")) {
        return QStringLiteral("json");
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("php")) {
        return QStringLiteral("php");
    }
    if (suffix == QStringLiteral("html") || suffix == QStringLiteral("htm")) {
        return QStringLiteral("html");
    }
    if (suffix == QStringLiteral("js") || suffix == QStringLiteral("mjs") || suffix == QStringLiteral("cjs")) {
        return QStringLiteral("script");
    }
    if (suffix == QStringLiteral("qml")) {
        return QStringLiteral("qml");
    }
    if (suffix == QStringLiteral("css")) {
        return QStringLiteral("css");
    }
    if (suffix == QStringLiteral("jsx")) {
        return QStringLiteral("jsx");
    }
    if (suffix == QStringLiteral("tsx")) {
        return QStringLiteral("tsx");
    }
    if (suffix == QStringLiteral("ts") || suffix == QStringLiteral("mts")
        || suffix == QStringLiteral("cts") || path.endsWith(QStringLiteral(".d.ts"))) {
        return QStringLiteral("ts");
    }
    if (suffix == QStringLiteral("json")) {
        return QStringLiteral("json");
    }
    if (suffix == QStringLiteral("py")) {
        return QStringLiteral("python");
    }
    if (suffix == QStringLiteral("java")) {
        return QStringLiteral("java");
    }
    if (suffix == QStringLiteral("swift")) {
        return QStringLiteral("swift");
    }
    if (suffix == QStringLiteral("cs")) {
        return QStringLiteral("csharp");
    }
    if (suffix == QStringLiteral("rs")) {
        return QStringLiteral("rust");
    }
    if (suffix == QStringLiteral("m") || suffix == QStringLiteral("mm")) {
        return QStringLiteral("objc");
    }
    if (suffix == QStringLiteral("c") || suffix == QStringLiteral("cc")
        || suffix == QStringLiteral("cpp") || suffix == QStringLiteral("cxx")
        || suffix == QStringLiteral("h") || suffix == QStringLiteral("hh")
        || suffix == QStringLiteral("hpp") || suffix == QStringLiteral("hxx")) {
        return QStringLiteral("cpp");
    }
    if (suffix == QStringLiteral("kt") || suffix == QStringLiteral("kts")) {
        return QStringLiteral("kotlin");
    }
    if (suffix == QStringLiteral("sh") || suffix == QStringLiteral("bash") || suffix == QStringLiteral("zsh")) {
        return QStringLiteral("shell");
    }
    if (suffix == QStringLiteral("go")) {
        return QStringLiteral("go");
    }
    if (suffix == QStringLiteral("sql")) {
        return QStringLiteral("sql");
    }
    if (suffix == QStringLiteral("vb")) {
        return QStringLiteral("vbnet");
    }
    return QStringLiteral("text");
}

QVariantList SymbolParser::parseClassMembers(const QString &body, const QString &language)
{
    QVariantList members;

    if (language == QStringLiteral("php")) {
        QRegularExpression methodPattern(
            QStringLiteral(R"((public|protected|private)?\s*(static\s+)?function\s+([A-Za-z_]\w*)\s*\()"),
            QRegularExpression::MultilineOption);
        auto methods = methodPattern.globalMatch(body);
        while (methods.hasNext()) {
            const auto match = methods.next();
            const int line = body.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
            members.append(makeSymbol(QStringLiteral("method"), match.captured(3), line));
        }

        QRegularExpression propertyPattern(
            QStringLiteral(R"((public|protected|private)\s+(static\s+)?\$([A-Za-z_]\w*))"),
            QRegularExpression::MultilineOption);
        auto properties = propertyPattern.globalMatch(body);
        while (properties.hasNext()) {
            const auto match = properties.next();
            const int line = body.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
            members.append(makeSymbol(QStringLiteral("property"), QStringLiteral("$") + match.captured(3), line));
        }

        QRegularExpression constPattern(
            QStringLiteral(R"((public|protected|private)?\s*const\s+([A-Z_]\w*))"),
            QRegularExpression::MultilineOption);
        auto constants = constPattern.globalMatch(body);
        while (constants.hasNext()) {
            const auto match = constants.next();
            const int line = body.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
            members.append(makeSymbol(QStringLiteral("constant"), match.captured(2), line));
        }
    } else if (language == QStringLiteral("js")) {
        QRegularExpression methodPattern(
            QStringLiteral(R"((?:^|[\n;])\s*(?:async\s+)?([A-Za-z_]\w*)\s*\([^)]*\)\s*\{)"),
            QRegularExpression::MultilineOption);
        auto methods = methodPattern.globalMatch(body);
        while (methods.hasNext()) {
            const auto match = methods.next();
            const QString name = match.captured(1);
            if (isControlKeywordName(name)) {
                continue;
            }
            const int line = body.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
            members.append(makeSymbol(QStringLiteral("method"), name, line, QString(), {},
                                      snippetFromBraceBlock(body, match.capturedStart(0))));
        }

        QRegularExpression propertyPattern(
            QStringLiteral(R"(this\.([A-Za-z_]\w*)\s*=)"),
            QRegularExpression::MultilineOption);
        auto properties = propertyPattern.globalMatch(body);
        while (properties.hasNext()) {
            const auto match = properties.next();
            const int line = body.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
            members.append(makeSymbol(QStringLiteral("property"), match.captured(1), line));
        }
    }

    return members;
}

QVariantList SymbolParser::parseObjectMembers(const QString &body)
{
    QVariantList members;

    QRegularExpression methodPattern(
        QStringLiteral(R"((?:^|[\n,])\s*(?:async\s+)?([A-Za-z_]\w*)\s*\([^)]*\)\s*\{)"),
        QRegularExpression::MultilineOption);
    auto methods = methodPattern.globalMatch(body);
    while (methods.hasNext()) {
        const auto match = methods.next();
        const QString name = match.captured(1);
        if (isControlKeywordName(name)) {
            continue;
        }
        const int line = body.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        members.append(makeSymbol(QStringLiteral("method"), name, line, QString(), {},
                                  snippetFromBraceBlock(body, match.capturedStart(0))));
    }

    QRegularExpression arrowPattern(
        QStringLiteral(R"((?:^|[\n,])\s*([A-Za-z_]\w*)\s*:\s*(?:async\s*)?\([^)]*\)\s*=>)"),
        QRegularExpression::MultilineOption);
    auto arrows = arrowPattern.globalMatch(body);
    while (arrows.hasNext()) {
        const auto match = arrows.next();
        const QString name = match.captured(1);
        if (isControlKeywordName(name)) {
            continue;
        }
        const int line = body.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        members.append(makeSymbol(QStringLiteral("function"), name, line, QString(), {},
                                  snippetFromLine(body, line, 0)));
    }

    QRegularExpression functionPropertyPattern(
        QStringLiteral(R"((?:^|[\n,])\s*([A-Za-z_]\w*)\s*:\s*(?:async\s*)?function\b)"),
        QRegularExpression::MultilineOption);
    auto functionProperties = functionPropertyPattern.globalMatch(body);
    while (functionProperties.hasNext()) {
        const auto match = functionProperties.next();
        const QString name = match.captured(1);
        if (isControlKeywordName(name)) {
            continue;
        }
        bool alreadyPresent = false;
        for (const QVariant &memberValue : std::as_const(members)) {
            if (memberValue.toMap().value(QStringLiteral("name")).toString() == name) {
                alreadyPresent = true;
                break;
            }
        }
        if (alreadyPresent) {
            continue;
        }
        const int line = body.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        members.append(makeSymbol(QStringLiteral("method"), name, line, QString(), {},
                                  snippetFromBraceBlock(body, match.capturedStart(0))));
    }

    QRegularExpression propertyPattern(
        QStringLiteral(R"((?:^|[\n,])\s*([A-Za-z_]\w*)\s*:)"),
        QRegularExpression::MultilineOption);
    auto properties = propertyPattern.globalMatch(body);
    while (properties.hasNext()) {
        const auto match = properties.next();
        const QString name = match.captured(1);
        bool alreadyPresent = false;
        for (const QVariant &memberValue : std::as_const(members)) {
            if (memberValue.toMap().value(QStringLiteral("name")).toString() == name) {
                alreadyPresent = true;
                break;
            }
        }
        if (alreadyPresent) {
            continue;
        }
        const int line = body.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        members.append(makeSymbol(QStringLiteral("property"), name, line));
    }

    return members;
}
QStringList SymbolParser::extractHtmlClasses(const QString &text)
{
    QSet<QString> classes;
    QString cleaned = text;
    cleaned.remove(QRegularExpression(QStringLiteral(R"(<!--([\s\S]*?)-->)")));
    cleaned.remove(QRegularExpression(QStringLiteral(R"(<script\b[^>]*>[\s\S]*?</script>)"),
                                      QRegularExpression::CaseInsensitiveOption));
    cleaned.remove(QRegularExpression(QStringLiteral(R"(<style\b[^>]*>[\s\S]*?</style>)"),
                                      QRegularExpression::CaseInsensitiveOption));

    QRegularExpression htmlClassPattern(QStringLiteral(R"(class\s*=\s*["']([^"']+)["'])"),
                                        QRegularExpression::CaseInsensitiveOption);
    auto htmlIt = htmlClassPattern.globalMatch(cleaned);
    while (htmlIt.hasNext()) {
        const auto match = htmlIt.next();
        const QStringList parts = match.captured(1).split(QRegularExpression(QStringLiteral(R"(\s+)") ),
                                                          Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            classes.insert(part.trimmed());
        }
    }

    QStringList values = classes.values();
    values.sort(Qt::CaseInsensitive);
    return values;
}

static QString normalizeLinkedAssetTarget(QString target)
{
    const int queryIndex = target.indexOf(QLatin1Char('?'));
    const int fragmentIndex = target.indexOf(QLatin1Char('#'));
    int endIndex = target.size();
    if (queryIndex >= 0) {
        endIndex = qMin(endIndex, queryIndex);
    }
    if (fragmentIndex >= 0) {
        endIndex = qMin(endIndex, fragmentIndex);
    }
    return target.left(endIndex).trimmed();
}

static QStringList extractHtmlLinkedAssets(const QString &htmlText, const QString &assetType)
{
    QStringList assets;
    QSet<QString> seen;
    QString cleaned = htmlText;
    cleaned.remove(QRegularExpression(QStringLiteral(R"(<!--([\s\S]*?)-->)")));

    const QRegularExpression scriptPattern(QStringLiteral(R"(<script[^>]+src=["']([^"']+)["'])"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression stylePattern(QStringLiteral(R"(<link[^>]+href=["']([^"']+)["'][^>]*rel=["'][^"']*stylesheet[^"']*["'])"),
                                          QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression alternateStylePattern(QStringLiteral(R"(<link[^>]+rel=["'][^"']*stylesheet[^"']*["'][^>]+href=["']([^"']+)["'])"),
                                                   QRegularExpression::CaseInsensitiveOption);

    auto appendUnique = [&](const QString &value) {
        const QString normalized = normalizeLinkedAssetTarget(value);
        if (!normalized.isEmpty() && !seen.contains(normalized)) {
            seen.insert(normalized);
            assets.append(normalized);
        }
    };

    if (assetType == QStringLiteral("script")) {
        auto scripts = scriptPattern.globalMatch(cleaned);
        while (scripts.hasNext()) {
            appendUnique(scripts.next().captured(1));
        }
        return assets;
    }

    auto styles = stylePattern.globalMatch(cleaned);
    while (styles.hasNext()) {
        appendUnique(styles.next().captured(1));
    }

    auto alternateStyles = alternateStylePattern.globalMatch(cleaned);
    while (alternateStyles.hasNext()) {
        appendUnique(alternateStyles.next().captured(1));
    }

    return assets;
}

void SymbolParser::enrichCssAnalysisWithHtmlUsage(QVariantMap &result, const QString &path, const QString &text)
{
    QVariantList quickLinks;
    QVariantList matchedClasses;
    QVariantList missingClasses;
    QStringList usedClasses;
    const QStringList extractedClasses = extractCssClasses(text);
    const QSet<QString> cssClassNames(extractedClasses.cbegin(), extractedClasses.cend());
    const QString absoluteCssPath = QFileInfo(path).absoluteFilePath();

    // Consumer pages: the stylesheet's folder or the nearest ancestor folder
    // whose pages link it (pages usually sit above css/).
    const QList<WebLinks::HtmlPage> pages = WebLinks::pagesReferencingAsset(path, QStringLiteral("stylesheet"));
    QSet<QString> seenScripts;
    QMap<QString, QVariantMap> ownIndex;
    QHash<QString, QVariantMap> scriptApplied; // class -> first JS use
    for (const WebLinks::HtmlPage &page : pages) {
        int line = 1;
        QString snippet;
        for (const WebLinks::HtmlAsset &asset : page.assets) {
            if (asset.kind == QStringLiteral("stylesheet") && asset.resolvedPath == absoluteCssPath) {
                line = asset.line;
                snippet = asset.snippet;
                break;
            }
        }
        quickLinks.append(makeWebLinkItem(QStringLiteral("consumer"), QFileInfo(page.path).fileName(), page.path,
                                          QStringLiteral("html"), line, snippet,
                                          QStringLiteral("referenced by HTML file")));

        for (const QString &className : page.usedClasses()) {
            if (!usedClasses.contains(className)) {
                usedClasses.append(className);
            }
            const auto containsName = [&](const QVariantList &list) {
                return std::any_of(list.cbegin(), list.cend(), [&](const QVariant &existing) {
                    return existing.toMap().value(QStringLiteral("name")).toString() == className;
                });
            };
            if (cssClassNames.contains(className)) {
                if (!containsName(matchedClasses)) {
                    if (ownIndex.isEmpty()) {
                        ownIndex = buildCssClassIndex(path, text);
                    }
                    QVariantMap entry = ownIndex.value(className,
                                                       makeCssClassSummaryEntry(className, true, path));
                    entry.insert(QStringLiteral("language"), QStringLiteral("css"));
                    matchedClasses.append(entry);
                }
            } else if (!containsName(missingClasses)) {
                const QList<const WebLinks::HtmlElement *> elements = page.elementsWithClass(className);
                const int elementLine = elements.isEmpty() ? 0 : elements.first()->line;
                QVariantMap missing = makeCssClassSummaryEntry(className, false, page.path, elementLine,
                                                               QStringLiteral("Used in %1").arg(QFileInfo(page.path).fileName()));
                missing.insert(QStringLiteral("language"), QStringLiteral("html"));
                missingClasses.append(missing);
            }
        }

        // Classes applied from the page's scripts (classList, className, jQuery).
        for (const WebLinks::HtmlAsset &asset : page.assets) {
            if (asset.kind != QStringLiteral("script") || !asset.local || !asset.exists
                || seenScripts.contains(asset.resolvedPath)
                || shouldSkipFileBySize(QFileInfo(asset.resolvedPath), kMaxAuxiliaryFileBytes)) {
                continue;
            }
            seenScripts.insert(asset.resolvedPath);
            const QString scriptText = readScanableScript(asset.resolvedPath);
            if (scriptText.isEmpty()) {
                continue;
            }
            for (const WebLinks::DomReference &ref : WebLinks::extractDomReferences(scriptText)) {
                if (ref.kind == QStringLiteral("class") && cssClassNames.contains(ref.name)
                    && !scriptApplied.contains(ref.name)) {
                    QVariantMap use;
                    use.insert(QStringLiteral("name"), ref.name);
                    use.insert(QStringLiteral("path"), asset.resolvedPath);
                    use.insert(QStringLiteral("line"), ref.line);
                    use.insert(QStringLiteral("snippet"), ref.snippet);
                    use.insert(QStringLiteral("via"), ref.via);
                    scriptApplied.insert(ref.name, use);
                }
            }
        }
    }

    QVariantList scriptAppliedClasses;
    for (auto it = scriptApplied.constBegin(); it != scriptApplied.constEnd(); ++it) {
        const QVariantMap use = it.value();
        scriptAppliedClasses.append(use);
        if (quickLinks.size() < 80) {
            quickLinks.append(makeWebLinkItem(QStringLiteral("script-class"),
                                              QStringLiteral(".%1 ← %2").arg(it.key(), fileLineLabel(use.value(QStringLiteral("path")).toString(),
                                                                                                    use.value(QStringLiteral("line")).toInt())),
                                              use.value(QStringLiteral("path")).toString(), QStringLiteral("script"),
                                              use.value(QStringLiteral("line")).toInt(),
                                              use.value(QStringLiteral("snippet")).toString(),
                                              use.value(QStringLiteral("via")).toString()));
        }
    }

    QVariantMap cssSummary;
    if (!pages.isEmpty()) {
        QStringList availableClasses = extractedClasses;
        availableClasses.sort(Qt::CaseInsensitive);
        usedClasses.sort(Qt::CaseInsensitive);
        QStringList unusedClasses;
        for (const QString &name : std::as_const(availableClasses)) {
            if (!usedClasses.contains(name) && !scriptApplied.contains(name)) {
                unusedClasses.append(name);
            }
        }
        cssSummary.insert(QStringLiteral("usedClasses"), toVariantList(usedClasses));
        cssSummary.insert(QStringLiteral("matchedClasses"), matchedClasses);
        cssSummary.insert(QStringLiteral("missingClasses"), missingClasses);
        cssSummary.insert(QStringLiteral("availableClasses"), toVariantList(availableClasses));
        cssSummary.insert(QStringLiteral("scriptAppliedClasses"), scriptAppliedClasses);
        cssSummary.insert(QStringLiteral("unusedClasses"), toVariantList(unusedClasses));
    }

    result.insert(QStringLiteral("quickLinks"), quickLinks);
    result.insert(QStringLiteral("cssSummary"), cssSummary);

    // @import and url() references are the stylesheet's dependencies.
    QVariantList dependencies = result.value(QStringLiteral("dependencies")).toList();
    static const QRegularExpression importPattern(QStringLiteral(R"(@import\s+(?:url\()?\s*['"]?([^'")\s;]+))"));
    static const QRegularExpression urlPattern(QStringLiteral(R"(url\(\s*['"]?([^'")]+?)['"]?\s*\))"));
    QSet<QString> seenTargets;
    auto addDependency = [&](const QString &target, const QString &type, int offset) {
        if (target.startsWith(QStringLiteral("data:")) || seenTargets.contains(target) || dependencies.size() >= 60) {
            return;
        }
        seenTargets.insert(target);
        const int line = lineNumberAtOffset(text, offset);
        QVariantMap item = makeSourceContextItem(path, QStringLiteral("css"), line, snippetFromLine(text, line, 0),
                                                 QStringLiteral("%1 dependency").arg(type));
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("label"), target);
        const bool local = !target.contains(QStringLiteral("://")) && !target.startsWith(QStringLiteral("//"));
        const QString resolved = local ? QDir::cleanPath(QFileInfo(path).dir().absoluteFilePath(target.section(QLatin1Char('?'), 0, 0).section(QLatin1Char('#'), 0, 0)))
                                       : QString();
        item.insert(QStringLiteral("path"), resolved);
        item.insert(QStringLiteral("exists"), local ? QFileInfo::exists(resolved) : true);
        dependencies.append(item);
    };
    auto importIt = importPattern.globalMatch(text);
    while (importIt.hasNext()) {
        const auto match = importIt.next();
        addDependency(match.captured(1), QStringLiteral("import"), match.capturedStart(0));
    }
    auto urlIt = urlPattern.globalMatch(text);
    while (urlIt.hasNext()) {
        const auto match = urlIt.next();
        addDependency(match.captured(1).trimmed(), QStringLiteral("asset"), match.capturedStart(0));
    }
    result.insert(QStringLiteral("dependencies"), dependencies);

    const int symbolCount = result.value(QStringLiteral("symbols")).toList().size();
    result.insert(QStringLiteral("summary"),
                  QStringLiteral("%1 selectors and variables, %2 HTML consumers")
                      .arg(symbolCount)
                      .arg(pages.size()));
}

QVariantList SymbolParser::findHtmlConsumersForAsset(const QString &path, const QString &assetType)
{
    QVariantList quickLinks;
    QSet<QString> seenQuickLinkPaths;
    const QFileInfo assetInfo(path);
    const QString absoluteAssetPath = assetInfo.absoluteFilePath();
    const QString canonicalAssetPath = assetInfo.canonicalFilePath();
    const QFileInfoList siblings = assetInfo.dir().entryInfoList({QStringLiteral("*.html"), QStringLiteral("*.htm")},
                                                                 QDir::Files | QDir::NoDotAndDotDot,
                                                                 QDir::Name);

    for (const QFileInfo &htmlInfo : siblings) {
        if (shouldSkipFileBySize(htmlInfo, kMaxAuxiliaryFileBytes)) {
            continue;
        }

        QFile htmlFile(htmlInfo.absoluteFilePath());
        if (!htmlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }

        const QString htmlText = QString::fromUtf8(htmlFile.readAll());
        const QStringList targets = extractHtmlLinkedAssets(htmlText, assetType);
        bool referencesAsset = false;
        for (const QString &target : targets) {
            const bool isLocalTarget = target.startsWith(QStringLiteral("./"))
                || target.startsWith(QStringLiteral("../"))
                || target.startsWith(QLatin1Char('/'))
                || (!target.contains(QStringLiteral("://")) && !target.startsWith(QStringLiteral("//")));
            if (!isLocalTarget) {
                continue;
            }
            const QString resolved = QDir::cleanPath(htmlInfo.dir().filePath(target));
            const QFileInfo resolvedInfo(resolved);
            const QString resolvedAbsolutePath = resolvedInfo.absoluteFilePath();
            const QString resolvedCanonicalPath = resolvedInfo.canonicalFilePath();
            if (resolvedAbsolutePath == absoluteAssetPath
                || (!canonicalAssetPath.isEmpty() && resolvedCanonicalPath == canonicalAssetPath)
                || resolvedInfo.fileName() == assetInfo.fileName()) {
                referencesAsset = true;
                break;
            }
        }

        if (!referencesAsset) {
            continue;
        }

        const QString htmlPath = htmlInfo.absoluteFilePath();
        if (seenQuickLinkPaths.contains(htmlPath)) {
            continue;
        }
        seenQuickLinkPaths.insert(htmlPath);

        int line = 1;
        const int offset = htmlText.indexOf(assetInfo.fileName());
        if (offset >= 0) {
            line = lineNumberAtOffset(htmlText, offset);
        }
        QVariantMap link = makeSourceContextItem(path, detectLanguage(path), line,
                                                 snippetFromLine(htmlText, line, 1),
                                                 QStringLiteral("referenced by HTML file"));
        link.insert(QStringLiteral("label"), htmlInfo.fileName());
        link.insert(QStringLiteral("target"), htmlInfo.fileName());
        link.insert(QStringLiteral("type"), QStringLiteral("consumer"));
        link.insert(QStringLiteral("path"), htmlPath);
        link.insert(QStringLiteral("targetPath"), htmlPath);
        link.insert(QStringLiteral("language"), QStringLiteral("html"));
        link.insert(QStringLiteral("exists"), true);
        quickLinks.append(link);
    }

    return quickLinks;
}

QStringList SymbolParser::extractCssClasses(const QString &text)
{
    QStringList classes = extractCssClassesTreeSitter(text);
    if (!classes.isEmpty()) {
        return classes;
    }

    QSet<QString> fallbackClasses;
    const QString cleaned = normalizeCssSelectorText(text);

    QRegularExpression cssClassPattern(QStringLiteral(R"(\.([A-Za-z_-][\w-]*))"));
    auto cssIt = cssClassPattern.globalMatch(cleaned);
    while (cssIt.hasNext()) {
        fallbackClasses.insert(cssIt.next().captured(1));
    }

    classes = fallbackClasses.values();
    classes.sort(Qt::CaseInsensitive);
    return classes;
}

QVariantList SymbolParser::extractDependencyLinks(const QString &path, const QString &text)
{
    QVariantList links;
    QSet<QString> seen;
    const QFileInfo fileInfo(path);
    const QDir dir = fileInfo.dir();
    const QString language = detectLanguage(path);

    auto appendLink = [&](const QString &target, const QString &type, int line,
                          const QVariantList &bindings = QVariantList{}) {
        const QString normalizedTarget = normalizeLinkedAssetTarget(target);
        if (seen.contains(type + QLatin1Char('|') + normalizedTarget)) {
            return;
        }
        seen.insert(type + QLatin1Char('|') + normalizedTarget);

        QVariantMap item = makeSourceContextItem(path, language, line, snippetFromLine(text, line, 0),
                                                 QStringLiteral("%1 dependency").arg(type));
        item.insert(QStringLiteral("target"), normalizedTarget);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("label"), normalizedTarget);
        if (!bindings.isEmpty()) {
            item.insert(QStringLiteral("bindings"), bindings);
        }

        if (normalizedTarget.startsWith(QStringLiteral("./")) || normalizedTarget.startsWith(QStringLiteral("../"))) {
            QString resolved = QDir::cleanPath(dir.filePath(normalizedTarget));
            QString chosenPath = resolved;
            const QStringList candidates = {
                resolved,
                resolved + QStringLiteral(".js"),
                resolved + QStringLiteral(".mjs"),
                resolved + QStringLiteral(".cjs"),
                resolved + QStringLiteral(".jsx"),
                resolved + QStringLiteral(".json"),
                resolved + QStringLiteral(".ts"),
                resolved + QStringLiteral(".tsx"),
                resolved + QStringLiteral(".py"),
                resolved + QStringLiteral(".php"),
                resolved + QStringLiteral(".java"),
                resolved + QStringLiteral(".cs"),
                resolved + QStringLiteral(".cpp"),
                resolved + QStringLiteral(".hpp"),
                QDir(resolved).filePath(QStringLiteral("index.js")),
                QDir(resolved).filePath(QStringLiteral("index.mjs")),
                QDir(resolved).filePath(QStringLiteral("index.cjs")),
                QDir(resolved).filePath(QStringLiteral("index.jsx")),
                QDir(resolved).filePath(QStringLiteral("index.ts")),
                QDir(resolved).filePath(QStringLiteral("index.tsx")),
                QDir(resolved).filePath(QStringLiteral("__init__.py"))
            };
            for (const QString &candidate : candidates) {
                if (QFileInfo::exists(candidate)) {
                    chosenPath = candidate;
                    break;
                }
            }
            item.insert(QStringLiteral("path"), chosenPath);
            item.insert(QStringLiteral("exists"), QFileInfo::exists(chosenPath));
            item.insert(QStringLiteral("label"), QFileInfo(chosenPath).fileName().isEmpty() ? target : QFileInfo(chosenPath).fileName());
        } else {
            item.insert(QStringLiteral("path"), QString());
            item.insert(QStringLiteral("exists"), true);
        }

        links.append(item);
    };

    QRegularExpression requirePattern(
        QStringLiteral("(?:(?:const|let|var)\\s+[A-Za-z_{}\\s,:]+\\s*=\\s*)?require\\s*\\(\\s*['\\\"]([^'\\\"]+)['\\\"]\\s*\\)"));
    auto requireMatches = requirePattern.globalMatch(text);
    while (requireMatches.hasNext()) {
        const auto match = requireMatches.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        appendLink(match.captured(1), QStringLiteral("require"), line,
                   parseRequireBindingsFromStatement(match.captured(0)));
    }

    QRegularExpression importPattern(QStringLiteral(R"(import\s+([\s\S]*?)\sfrom\s+['"]([^'"]+)['"])"));
    auto imports = importPattern.globalMatch(text);
    while (imports.hasNext()) {
        const auto match = imports.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        appendLink(match.captured(2), QStringLiteral("import"), line,
                   parseScriptImportBindingsFromStatement(match.captured(0)));
    }

    return links;
}

namespace {

// Python strings (incl. triple-quoted, any prefix) and comments blanked,
// newlines kept, so import statements can be found without docstring noise.
QString blankPythonNoise(const QString &text)
{
    QString out = text;
    const int size = out.size();
    auto blank = [&](int from, int to) {
        for (int i = from; i < to && i < size; ++i) {
            if (out.at(i) != QLatin1Char('\n')) {
                out[i] = QLatin1Char(' ');
            }
        }
    };
    int index = 0;
    while (index < size) {
        const QChar ch = out.at(index);
        if (ch == QLatin1Char('#')) {
            int end = out.indexOf(QLatin1Char('\n'), index);
            end = end < 0 ? size : end;
            blank(index, end);
            index = end;
        } else if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            const QString triple(3, ch);
            if (out.mid(index, 3) == triple) {
                int end = index + 3;
                while (end < size && out.mid(end, 3) != triple) {
                    end += out.at(end) == QLatin1Char('\\') ? 2 : 1;
                }
                end = qMin(size, end + 3);
                blank(index, end);
                index = end;
            } else {
                int end = index + 1;
                while (end < size && out.at(end) != ch && out.at(end) != QLatin1Char('\n')) {
                    end += out.at(end) == QLatin1Char('\\') ? 2 : 1;
                }
                end = qMin(size, end + 1);
                blank(index, end);
                index = end;
            }
        } else {
            ++index;
        }
    }
    return out;
}

// A module (a.b.c, with `level` leading dots for relative imports) as a
// local file: <root>/a/b/c.py or <root>/a/b/c/__init__.py. Absolute imports
// are looked up from the file's folder upwards (and in `src/` layouts).
QString resolvePythonModule(const QString &filePath, const QString &module, int level)
{
    const QStringList parts = module.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    auto probe = [&](const QDir &root) -> QString {
        if (parts.isEmpty()) {
            const QString init = root.filePath(QStringLiteral("__init__.py"));
            return QFileInfo::exists(init) ? QFileInfo(init).absoluteFilePath() : QString();
        }
        const QString base = root.filePath(parts.join(QLatin1Char('/')));
        if (QFileInfo(base + QStringLiteral(".py")).isFile()) {
            return QFileInfo(base + QStringLiteral(".py")).absoluteFilePath();
        }
        if (QFileInfo(base + QStringLiteral("/__init__.py")).isFile()) {
            return QFileInfo(base + QStringLiteral("/__init__.py")).absoluteFilePath();
        }
        return QString();
    };
    QDir dir = QFileInfo(filePath).absoluteDir();
    if (level > 0) {
        for (int up = 1; up < level; ++up) {
            if (!dir.cdUp()) {
                return QString();
            }
        }
        return probe(dir);
    }
    // Absolute imports never resolve inside the importing file's own package
    // (Python 3): start above the outermost enclosing package.
    for (int guard = 0; guard < 16 && QFileInfo::exists(dir.filePath(QStringLiteral("__init__.py"))); ++guard) {
        if (!dir.cdUp()) {
            break;
        }
    }
    for (int depth = 0; depth < 8; ++depth) {
        QString found = probe(dir);
        if (found.isEmpty() && QFileInfo(dir.filePath(QStringLiteral("src"))).isDir()) {
            found = probe(QDir(dir.filePath(QStringLiteral("src"))));
        }
        if (!found.isEmpty()) {
            return found;
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return QString();
}

} // namespace

QVariantList SymbolParser::extractPythonDependencies(const QString &path, const QString &text)
{
    QVariantList links;
    const QString clean = blankPythonNoise(text);
    const QStringList physical = clean.split(QLatin1Char('\n'));

    // Logical lines: bracket and backslash continuations joined.
    struct Logical { QString text; int line; };
    QVector<Logical> logical;
    QString pending;
    int pendingLine = 0;
    int depth = 0;
    for (int index = 0; index < physical.size(); ++index) {
        QString line = physical.at(index);
        if (pending.isEmpty()) {
            pendingLine = index + 1;
        }
        for (const QChar ch : std::as_const(line)) {
            if (ch == QLatin1Char('(') || ch == QLatin1Char('[') || ch == QLatin1Char('{')) {
                ++depth;
            } else if ((ch == QLatin1Char(')') || ch == QLatin1Char(']') || ch == QLatin1Char('}')) && depth > 0) {
                --depth;
            }
        }
        const bool backslash = line.trimmed().endsWith(QLatin1Char('\\'));
        if (backslash) {
            line = line.trimmed();
            line.chop(1);
        }
        pending += line + QLatin1Char(' ');
        if (depth == 0 && !backslash) {
            logical.append({pending, pendingLine});
            pending.clear();
        }
    }
    if (!pending.trimmed().isEmpty()) {
        logical.append({pending, pendingLine});
    }

    static const QRegularExpression importStatement(QStringLiteral(R"(^\s*import\s+(.+)$)"));
    static const QRegularExpression fromStatement(QStringLiteral(R"(^\s*from\s+(\.*)([A-Za-z_][\w.]*)?\s+import\s+(.+)$)"));
    static const QRegularExpression dottedName(QStringLiteral(R"(^[A-Za-z_][\w.]*$)"));
    QSet<QString> seen;
    auto append = [&](const QString &target, const QString &resolved, const QVariantList &bindings, int line) {
        const QString id = target + QLatin1Char('|') + resolved;
        if (seen.contains(id)) {
            // Merge bindings of repeated imports of the same module.
            for (int index = 0; index < links.size(); ++index) {
                QVariantMap item = links.at(index).toMap();
                if (item.value(QStringLiteral("target")).toString() == target
                    && item.value(QStringLiteral("path")).toString() == resolved) {
                    QVariantList merged = item.value(QStringLiteral("bindings")).toList();
                    merged += bindings;
                    item.insert(QStringLiteral("bindings"), merged);
                    links[index] = item;
                    break;
                }
            }
            return;
        }
        seen.insert(id);
        QVariantMap item = makeSourceContextItem(path, QStringLiteral("python"), line,
                                                 snippetFromLine(text, line, 0), QStringLiteral("import"));
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), QStringLiteral("import"));
        item.insert(QStringLiteral("label"), resolved.isEmpty() ? target
                                                                : QDir(QFileInfo(path).absolutePath()).relativeFilePath(resolved));
        item.insert(QStringLiteral("path"), resolved);
        item.insert(QStringLiteral("exists"), true);
        if (!bindings.isEmpty()) {
            item.insert(QStringLiteral("bindings"), bindings);
        }
        links.append(item);
    };
    auto binding = [](const QString &local, const QString &imported) {
        return QVariantMap{{QStringLiteral("local"), local}, {QStringLiteral("imported"), imported}};
    };

    for (const Logical &statement : std::as_const(logical)) {
        const QString simplified = statement.text.simplified();
        const auto fromMatch = fromStatement.match(simplified);
        if (fromMatch.hasMatch()) {
            const int level = fromMatch.captured(1).size();
            const QString module = fromMatch.captured(2);
            const QString target = fromMatch.captured(1) + module;
            QString names = fromMatch.captured(3);
            names.remove(QLatin1Char('(')).remove(QLatin1Char(')'));
            const QString modulePath = resolvePythonModule(path, module, level);
            QVariantList moduleBindings;
            for (const QString &entry : names.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const QString name = entry.section(QStringLiteral(" as "), 0, 0).trimmed();
                const QString local = entry.contains(QStringLiteral(" as "))
                    ? entry.section(QStringLiteral(" as "), 1, 1).trimmed() : name;
                if (name == QStringLiteral("*")) {
                    continue; // star import: no bindings, like an include
                }
                if (!dottedName.match(name).hasMatch() || !dottedName.match(local).hasMatch()) {
                    continue;
                }
                // `from pkg import module` names a submodule, not a symbol.
                const QString submodule = resolvePythonModule(path, module.isEmpty() ? name : module + QLatin1Char('.') + name, level);
                if (!submodule.isEmpty() && submodule != modulePath) {
                    append(target + (module.isEmpty() ? QString() : QStringLiteral(".")) + name, submodule,
                           {binding(local, QStringLiteral("*"))}, statement.line);
                } else {
                    moduleBindings.append(binding(local, name));
                }
            }
            if (!moduleBindings.isEmpty() || names.trimmed() == QStringLiteral("*")) {
                append(target.isEmpty() ? QStringLiteral(".") : target, modulePath, moduleBindings, statement.line);
            }
            continue;
        }
        const auto importMatch = importStatement.match(simplified);
        if (!importMatch.hasMatch()) {
            continue;
        }
        for (const QString &entry : importMatch.captured(1).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString module = entry.section(QStringLiteral(" as "), 0, 0).trimmed();
            if (!dottedName.match(module).hasMatch()) {
                continue;
            }
            const QString local = entry.contains(QStringLiteral(" as "))
                ? entry.section(QStringLiteral(" as "), 1, 1).trimmed() : module;
            append(module, resolvePythonModule(path, module, 0), {binding(local, QStringLiteral("*"))}, statement.line);
        }
    }
    return links;
}

QVariantList SymbolParser::extractCppDependencies(const QString &path, const QString &text)
{
    QVariantList links;
    QSet<QString> seen;
    const QDir dir = QFileInfo(path).dir();
    QRegularExpression includePattern(QStringLiteral(R"(^\s*#include\s*[<"]([^>"]+)[>"])"),
                                      QRegularExpression::MultilineOption);
    auto includeIt = includePattern.globalMatch(text);
    while (includeIt.hasNext()) {
        const auto match = includeIt.next();
        const QString target = match.captured(1);
        if (seen.contains(target)) {
            continue;
        }
        seen.insert(target);
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        QVariantMap item = makeSourceContextItem(path, QStringLiteral("cpp"), line, snippetFromLine(text, line, 0),
                                                 QStringLiteral("include"));
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), QStringLiteral("include"));
        item.insert(QStringLiteral("label"), target);
        const QString resolved = QDir::cleanPath(dir.filePath(target));
        item.insert(QStringLiteral("path"), QFileInfo::exists(resolved) ? resolved : QString());
        item.insert(QStringLiteral("exists"), QFileInfo::exists(resolved) || !target.contains(QLatin1Char('/')));
        links.append(item);
    }
    return links;
}

QVariantList SymbolParser::extractJavaDependencies(const QString &text)
{
    QVariantList links;
    QSet<QString> seen;
    QRegularExpression importPattern(QStringLiteral(R"(^\s*import\s+([A-Za-z0-9_.*]+)\s*;)"),
                                     QRegularExpression::MultilineOption);
    auto importIt = importPattern.globalMatch(text);
    while (importIt.hasNext()) {
        const auto match = importIt.next();
        const QString target = match.captured(1);
        if (seen.contains(target)) {
            continue;
        }
        seen.insert(target);
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        QVariantMap item = makeSourceContextItem(QString(), QStringLiteral("java"), line,
                                                 snippetFromLine(text, line, 0), QStringLiteral("import"));
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), QStringLiteral("import"));
        item.insert(QStringLiteral("label"), target);
        item.insert(QStringLiteral("path"), QString());
        item.insert(QStringLiteral("exists"), true);
        links.append(item);
    }
    return links;
}

QVariantList SymbolParser::extractCSharpDependencies(const QString &text)
{
    QVariantList links;
    QSet<QString> seen;
    QRegularExpression usingPattern(QStringLiteral(R"(^\s*using\s+([A-Za-z0-9_.]+)\s*;)"),
                                    QRegularExpression::MultilineOption);
    auto usingIt = usingPattern.globalMatch(text);
    while (usingIt.hasNext()) {
        const auto match = usingIt.next();
        const QString target = match.captured(1);
        if (seen.contains(target)) {
            continue;
        }
        seen.insert(target);
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        QVariantMap item = makeSourceContextItem(QString(), QStringLiteral("csharp"), line,
                                                 snippetFromLine(text, line, 0), QStringLiteral("using"));
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), QStringLiteral("using"));
        item.insert(QStringLiteral("label"), target);
        item.insert(QStringLiteral("path"), QString());
        item.insert(QStringLiteral("exists"), true);
        links.append(item);
    }
    return links;
}

QVariantList SymbolParser::extractRustDependencies(const QString &text)
{
    QVariantList links;
    QSet<QString> seen;

    auto appendDependency = [&](const QString &target, int line) {
        if (target.isEmpty() || seen.contains(target)) {
            return;
        }
        seen.insert(target);
        QVariantMap item = makeSourceContextItem(QString(), QStringLiteral("rust"), line,
                                                 snippetFromLine(text, line, 0), QStringLiteral("use"));
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), QStringLiteral("use"));
        item.insert(QStringLiteral("label"), rustDependencyLabel(target));
        item.insert(QStringLiteral("path"), QString());
        item.insert(QStringLiteral("exists"), true);
        links.append(item);
    };

    QRegularExpression usePattern(QStringLiteral(R"(^\s*use\s+([^;]+)\s*;)"),
                                  QRegularExpression::MultilineOption);
    auto useIt = usePattern.globalMatch(text);
    while (useIt.hasNext()) {
        const auto match = useIt.next();
        appendDependency(match.captured(1).trimmed(), lineNumberAtOffset(text, match.capturedStart(0)));
    }

    QRegularExpression modPattern(QStringLiteral(R"(^\s*mod\s+([A-Za-z_]\w*)\s*;)"),
                                  QRegularExpression::MultilineOption);
    auto modIt = modPattern.globalMatch(text);
    while (modIt.hasNext()) {
        const auto match = modIt.next();
        appendDependency(match.captured(1), lineNumberAtOffset(text, match.capturedStart(0)));
    }

    return links;
}

QVariantList SymbolParser::extractObjectiveCDependencies(const QString &path, const QString &text)
{
    QVariantList links;
    QSet<QString> seen;
    const QDir dir = QFileInfo(path).dir();

    QRegularExpression importPattern(QStringLiteral(R"(^\s*#(?:import|include)\s*[<"]([^>"]+)[>"])"),
                                     QRegularExpression::MultilineOption);
    auto importIt = importPattern.globalMatch(text);
    while (importIt.hasNext()) {
        const auto match = importIt.next();
        const QString target = match.captured(1);
        if (target.isEmpty() || seen.contains(target)) {
            continue;
        }
        seen.insert(target);
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        QVariantMap item = makeSourceContextItem(path, QStringLiteral("objc"), line, snippetFromLine(text, line, 0),
                                                 QStringLiteral("import"));
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), QStringLiteral("import"));
        item.insert(QStringLiteral("label"), target);
        const QString resolved = QDir::cleanPath(dir.filePath(target));
        item.insert(QStringLiteral("path"), QFileInfo::exists(resolved) ? resolved : QString());
        item.insert(QStringLiteral("exists"), QFileInfo::exists(resolved) || !target.contains(QLatin1Char('/')));
        links.append(item);
    }

    return links;
}

// JS comments blanked (strings kept, offsets unchanged): documentation
// examples such as `app.get('/about/me', ...)` in JSDoc are not routes.
static QString blankJsComments(const QString &text)
{
    QString out = text;
    const int size = out.size();
    QChar quote;
    for (int index = 0; index < size; ++index) {
        const QChar ch = out.at(index);
        if (!quote.isNull()) {
            if (ch == QLatin1Char('\\')) {
                ++index;
            } else if (ch == quote || (ch == QLatin1Char('\n') && quote != QLatin1Char('`'))) {
                quote = QChar();
            }
            continue;
        }
        const QChar next = index + 1 < size ? out.at(index + 1) : QChar();
        if (ch == QLatin1Char('/') && next == QLatin1Char('/')) {
            while (index < size && out.at(index) != QLatin1Char('\n')) {
                out[index++] = QLatin1Char(' ');
            }
        } else if (ch == QLatin1Char('/') && next == QLatin1Char('*')) {
            const int end = out.indexOf(QStringLiteral("*/"), index + 2);
            const int stop = end < 0 ? size : end + 2;
            for (; index < stop; ++index) {
                if (out.at(index) != QLatin1Char('\n')) {
                    out[index] = QLatin1Char(' ');
                }
            }
            --index;
        } else if (ch == QLatin1Char('\'') || ch == QLatin1Char('"') || ch == QLatin1Char('`')) {
            quote = ch;
        }
    }
    return out;
}

QVariantList SymbolParser::extractExpressRoutes(const QString &rawText)
{
    const QString text = blankJsComments(rawText);
    QVariantList routes;
    QRegularExpression routePattern(
        QStringLiteral(R"(\b(app|router)\.(get|post|put|patch|delete|options|head|use)\s*\(\s*['"]([^'"]+)['"])"),
        QRegularExpression::CaseInsensitiveOption);
    auto matches = routePattern.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        QVariantMap route;
        route.insert(QStringLiteral("owner"), match.captured(1));
        route.insert(QStringLiteral("method"), match.captured(2).toUpper());
        route.insert(QStringLiteral("path"), match.captured(3));
        route.insert(QStringLiteral("line"), line);
        route.insert(QStringLiteral("snippet"), snippetFromLine(text, line, 1));
        route.insert(QStringLiteral("detail"), QStringLiteral("route"));
        route.insert(QStringLiteral("label"), match.captured(2).toUpper() + QStringLiteral(" ") + match.captured(3));
        routes.append(route);
    }
    return routes;
}

QVariantList SymbolParser::extractPythonRoutes(const QString &path, const QString &text)
{
    QVariantList routes;
    QRegularExpression routePattern(
        QStringLiteral(R"(^[ \t]*@(?:\w+\.)?route\s*\(\s*['"]([^'"]+)['"](?:\s*,\s*methods\s*=\s*\[([^\]]*)\])?)"),
        QRegularExpression::MultilineOption);
    auto routeIt = routePattern.globalMatch(text);
    while (routeIt.hasNext()) {
        const auto match = routeIt.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        QString method = QStringLiteral("GET");
        if (!match.captured(2).trimmed().isEmpty()) {
            method = match.captured(2).trimmed();
            method.remove(QLatin1Char('\''));
            method.remove(QLatin1Char('"'));
        }
        QVariantMap route = makeSourceContextItem(path, QStringLiteral("python"), line, snippetFromLine(text, line, 0),
                                                  QStringLiteral("route"));
        route.insert(QStringLiteral("owner"), QStringLiteral("app"));
        route.insert(QStringLiteral("method"), method);
        route.insert(QStringLiteral("path"), match.captured(1));
        route.insert(QStringLiteral("label"), method + QStringLiteral(" ") + match.captured(1));
        routes.append(route);
    }

    QRegularExpression fastApiPattern(
        QStringLiteral(R"(^[ \t]*@(?:\w+\.)?(get|post|put|patch|delete|options|head)\s*\(\s*['"]([^'"]+)['"])"),
        QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    auto fastApiIt = fastApiPattern.globalMatch(text);
    while (fastApiIt.hasNext()) {
        const auto match = fastApiIt.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        const QString method = match.captured(1).toUpper();
        QVariantMap route = makeSourceContextItem(path, QStringLiteral("python"), line, snippetFromLine(text, line, 0),
                                                  QStringLiteral("route"));
        route.insert(QStringLiteral("owner"), QStringLiteral("app"));
        route.insert(QStringLiteral("method"), method);
        route.insert(QStringLiteral("path"), match.captured(2));
        route.insert(QStringLiteral("label"), method + QStringLiteral(" ") + match.captured(2));
        routes.append(route);
    }

    return routes;
}

QVariantList SymbolParser::extractAspNetRoutes(const QString &path, const QString &text)
{
    QVariantList routes;
    QRegularExpression routePattern(
        QStringLiteral(R"(\bapp\.(MapGet|MapPost|MapPut|MapPatch|MapDelete|MapMethods|MapControllerRoute)\s*\(([\s\S]*?)\))"));
    auto routeIt = routePattern.globalMatch(text);
    while (routeIt.hasNext()) {
        const auto match = routeIt.next();
        const QString routeType = match.captured(1);
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        QString method = routeType.mid(3).toUpper();
        QString routePath;
        QRegularExpression quotedPath(QStringLiteral(R"(["]([^"]+)["])"));
        auto pathMatch = quotedPath.match(match.captured(2));
        if (pathMatch.hasMatch()) {
            routePath = pathMatch.captured(1);
        }
        if (routeType == QStringLiteral("MapControllerRoute")) {
            method = QStringLiteral("ROUTE");
            QRegularExpression patternField(QStringLiteral("pattern\\s*:\\s*\"([^\"]+)\""));
            const auto patternMatch = patternField.match(match.captured(2));
            if (patternMatch.hasMatch()) {
                routePath = patternMatch.captured(1);
            } else if (routePath.isEmpty()) {
                routePath = QStringLiteral("{controller=Home}/{action=Index}/{id?}");
            }
        }
        QVariantMap route = makeSourceContextItem(path, QStringLiteral("csharp"), line, snippetFromLine(text, line, 1),
                                                  QStringLiteral("route"));
        route.insert(QStringLiteral("owner"), QStringLiteral("app"));
        route.insert(QStringLiteral("method"), method);
        route.insert(QStringLiteral("path"), routePath);
        route.insert(QStringLiteral("label"), method + QStringLiteral(" ") + routePath);
        routes.append(route);
    }
    return routes;
}

QVariantMap SymbolParser::makeSymbolPublic(const QString &kind, const QString &name, int line, const QString &detail,
                                           const QVariantList &members, const QString &snippet)
{
    return makeSymbol(kind, name, line, detail, members, snippet);
}

QVariantList SymbolParser::findRelatedFilesPublic(const QString &path)
{
    return findRelatedFiles(path);
}

QVariantList SymbolParser::findRelatedFiles(const QString &path)
{
    QVariantList related;
    QSet<QString> seen;
    const QFileInfo info(path);
    const QDir dir = info.dir();
    const QString fileName = info.fileName();
    const QString baseName = info.completeBaseName();

    auto appendRelated = [&](const QString &candidatePath, const QString &type) {
        const QString absolute = QFileInfo(candidatePath).absoluteFilePath();
        if (!QFileInfo::exists(absolute) || seen.contains(absolute) || absolute == info.absoluteFilePath()) {
            return;
        }
        seen.insert(absolute);
        QVariantMap item;
        item.insert(QStringLiteral("path"), absolute);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("label"), QFileInfo(absolute).fileName());
        item.insert(QStringLiteral("exists"), true);
        related.append(item);
    };

    const bool isTest = fileName.contains(QStringLiteral(".test.")) || fileName.contains(QStringLiteral(".spec."));
    if (isTest) {
        QString implementationBase = fileName;
        implementationBase.replace(QStringLiteral(".test"), QString());
        implementationBase.replace(QStringLiteral(".spec"), QString());
        appendRelated(dir.filePath(QStringLiteral("../") + implementationBase), QStringLiteral("implementation"));
    } else {
        appendRelated(dir.filePath(QStringLiteral("tests/%1.test.js").arg(baseName)), QStringLiteral("test"));
        appendRelated(dir.filePath(QStringLiteral("tests/%1.spec.js").arg(baseName)), QStringLiteral("test"));
        appendRelated(dir.filePath(QStringLiteral("tests/%1.test.ts").arg(baseName)), QStringLiteral("test"));
        appendRelated(dir.filePath(QStringLiteral("tests/%1.spec.ts").arg(baseName)), QStringLiteral("test"));
        appendRelated(dir.filePath(QStringLiteral("../tests/%1.test.js").arg(baseName)), QStringLiteral("test"));
        appendRelated(dir.filePath(QStringLiteral("../tests/%1.spec.js").arg(baseName)), QStringLiteral("test"));
    }

    if (fileName == QStringLiteral("index.js") || fileName == QStringLiteral("index.ts")) {
        appendRelated(dir.filePath(QStringLiteral("package.json")), QStringLiteral("package"));
        appendRelated(dir.filePath(QStringLiteral("public_html/index.html")), QStringLiteral("frontend"));
    }

    if (fileName.endsWith(QStringLiteral(".cpp")) || fileName.endsWith(QStringLiteral(".cc"))
        || fileName.endsWith(QStringLiteral(".cxx"))) {
        appendRelated(dir.filePath(baseName + QStringLiteral(".h")), QStringLiteral("header"));
        appendRelated(dir.filePath(baseName + QStringLiteral(".hpp")), QStringLiteral("header"));
    }

    if (fileName.endsWith(QStringLiteral(".h")) || fileName.endsWith(QStringLiteral(".hpp"))
        || fileName.endsWith(QStringLiteral(".hh")) || fileName.endsWith(QStringLiteral(".hxx"))) {
        appendRelated(dir.filePath(baseName + QStringLiteral(".cpp")), QStringLiteral("implementation"));
        appendRelated(dir.filePath(baseName + QStringLiteral(".cc")), QStringLiteral("implementation"));
        appendRelated(dir.filePath(baseName + QStringLiteral(".cxx")), QStringLiteral("implementation"));
    }

    if (fileName.endsWith(QStringLiteral(".py"))) {
        appendRelated(dir.filePath(QStringLiteral("test_%1.py").arg(baseName)), QStringLiteral("test"));
        appendRelated(dir.filePath(QStringLiteral("%1_test.py").arg(baseName)), QStringLiteral("test"));
        appendRelated(dir.filePath(QStringLiteral("tests/test_%1.py").arg(baseName)), QStringLiteral("test"));
    }

    if (fileName.endsWith(QStringLiteral(".cs"))) {
        appendRelated(dir.filePath(QStringLiteral("%1.csproj").arg(dir.dirName())), QStringLiteral("project"));
    }

    if (fileName.endsWith(QStringLiteral(".m")) || fileName.endsWith(QStringLiteral(".mm"))) {
        appendRelated(dir.filePath(baseName + QStringLiteral(".h")), QStringLiteral("header"));
    }

    if (fileName.endsWith(QStringLiteral(".rs"))) {
        appendRelated(dir.filePath(QStringLiteral("mod.rs")), QStringLiteral("module"));
        appendRelated(dir.filePath(QStringLiteral("lib.rs")), QStringLiteral("crate"));
        appendRelated(dir.filePath(QStringLiteral("main.rs")), QStringLiteral("entrypoint"));
    }

    if (fileName == QStringLiteral("package.json")) {
        appendRelated(dir.filePath(QStringLiteral("index.js")), QStringLiteral("entrypoint"));
        appendRelated(dir.filePath(QStringLiteral("index.ts")), QStringLiteral("entrypoint"));
    }

    return related;
}

QVariantMap SymbolParser::extractPackageSummary(const QString &path, const QString &text)
{
    Q_UNUSED(path)
    QVariantMap summary;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) {
        return summary;
    }

    const QJsonObject root = doc.object();
    summary.insert(QStringLiteral("name"), root.value(QStringLiteral("name")).toString());
    summary.insert(QStringLiteral("version"), root.value(QStringLiteral("version")).toString());
    summary.insert(QStringLiteral("main"), root.value(QStringLiteral("main")).toString());

    QVariantMap scripts;
    const QJsonObject scriptsObject = root.value(QStringLiteral("scripts")).toObject();
    for (auto it = scriptsObject.constBegin(); it != scriptsObject.constEnd(); ++it) {
        scripts.insert(it.key(), it.value().toString());
    }
    summary.insert(QStringLiteral("scripts"), scripts);

    QVariantList dependencies;
    auto collectDeps = [&](const QString &sectionName) {
        const QJsonObject depsObject = root.value(sectionName).toObject();
        for (auto it = depsObject.constBegin(); it != depsObject.constEnd(); ++it) {
            QVariantMap dep;
            dep.insert(QStringLiteral("name"), it.key());
            dep.insert(QStringLiteral("version"), it.value().toString());
            dep.insert(QStringLiteral("section"), sectionName);
            dependencies.append(dep);
        }
    };
    collectDeps(QStringLiteral("dependencies"));
    collectDeps(QStringLiteral("devDependencies"));
    summary.insert(QStringLiteral("dependencies"), dependencies);
    summary.insert(QStringLiteral("dependencyCount"), dependencies.size());

    return summary;
}

namespace {

// VB.NET comments (' and REM) and string literals blanked, offsets kept.
QString blankVbNoise(const QString &text)
{
    QString out = text;
    const int size = out.size();
    bool inString = false;
    bool atLineStart = true;
    for (int index = 0; index < size; ++index) {
        const QChar ch = out.at(index);
        if (ch == QLatin1Char('\n')) {
            inString = false;
            atLineStart = true;
            continue;
        }
        if (inString) {
            if (ch == QLatin1Char('"')) {
                if (index + 1 < size && out.at(index + 1) == QLatin1Char('"')) {
                    out[index] = QLatin1Char(' ');
                    out[++index] = QLatin1Char(' ');
                    continue;
                }
                inString = false;
            } else {
                out[index] = QLatin1Char(' ');
            }
            continue;
        }
        const bool remComment = atLineStart && out.mid(index, 3).compare(QStringLiteral("REM"), Qt::CaseInsensitive) == 0
            && (index + 3 >= size || out.at(index + 3).isSpace());
        if (ch == QLatin1Char('\'') || remComment) {
            while (index < size && out.at(index) != QLatin1Char('\n')) {
                out[index++] = QLatin1Char(' ');
            }
            --index;
            continue;
        }
        if (ch == QLatin1Char('"')) {
            inString = true;
        }
        if (!ch.isSpace()) {
            atLineStart = false;
        }
    }
    return out;
}

bool isTextCallSiteOwnerKind(const QString &kind)
{
    static const QSet<QString> kinds = {
        QStringLiteral("function"), QStringLiteral("method"), QStringLiteral("constructor"),
        QStringLiteral("procedure"), QStringLiteral("operator"), QStringLiteral("property"), QStringLiteral("event"),
    };
    return kinds.contains(kind);
}

struct TextCallSite
{
    QString name;
    QString qualifier;
    int line = 0;
};

void collectObjcSites(const QString &clean, int from, int to, const QVector<int> &lineStarts, QVector<TextCallSite> &sites)
{
    auto lineOf = [&](int offset) {
        return int(std::upper_bound(lineStarts.cbegin(), lineStarts.cend(), offset) - lineStarts.cbegin());
    };
    for (int i = from; i < to; ++i) {
        if (clean.at(i) != QLatin1Char('[')) {
            continue;
        }
        const int close = matchingClose(clean, i, QLatin1Char('['), QLatin1Char(']'));
        if (close < 0 || close > to) {
            continue;
        }
        const QString selector = objcMessageSelector(clean, i, close);
        if (selector.isEmpty()) {
            continue;
        }
        int receiverEnd = i + 1;
        while (receiverEnd < close && (clean.at(receiverEnd).isLetterOrNumber() || clean.at(receiverEnd) == QLatin1Char('_'))) {
            ++receiverEnd;
        }
        QString receiver = clean.mid(i + 1, receiverEnd - i - 1);
        if (receiverEnd < close && !clean.at(receiverEnd).isSpace()) {
            receiver = QStringLiteral("(expression)");
        }
        sites.append({selector, receiver.isEmpty() ? QStringLiteral("(expression)") : receiver, lineOf(i)});
    }
    static const QRegularExpression cCall(QStringLiteral(R"((?<![\w.>@:])([A-Za-z_]\w*)\s*\()"));
    static const QSet<QString> keywords = {
        QStringLiteral("if"), QStringLiteral("for"), QStringLiteral("while"), QStringLiteral("switch"),
        QStringLiteral("return"), QStringLiteral("sizeof"), QStringLiteral("typeof"), QStringLiteral("catch"),
        QStringLiteral("selector"), QStringLiteral("encode"), QStringLiteral("protocol"), QStringLiteral("synchronized"),
        QStringLiteral("autoreleasepool"), QStringLiteral("defined"), QStringLiteral("NSLog"), QStringLiteral("assert"),
        QStringLiteral("__typeof__"), QStringLiteral("__typeof"), QStringLiteral("dispatch_async"),
    };
    auto it = cCall.globalMatch(clean.mid(from, to - from));
    while (it.hasNext()) {
        const auto match = it.next();
        const QString name = match.captured(1);
        if (!keywords.contains(name)) {
            sites.append({name, QString(), lineOf(from + match.capturedStart(1))});
        }
    }
}

void collectVbSites(const QStringList &lines, int fromLine, int toLine, QVector<TextCallSite> &sites)
{
    static const QRegularExpression call(QStringLiteral(R"((?<![\w.])(?:([A-Za-z_]\w*)\.)?([A-Za-z_]\w*)\s*\()"));
    static const QRegularExpression construct(QStringLiteral(R"(\bNew\s+([A-Za-z_][\w.]*))"),
                                              QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression callStatement(QStringLiteral(R"(^\s*Call\s+(?:([A-Za-z_]\w*)\.)?([A-Za-z_]\w*))"),
                                                  QRegularExpression::CaseInsensitiveOption);
    static const QSet<QString> keywords = {
        QStringLiteral("if"), QStringLiteral("elseif"), QStringLiteral("while"), QStringLiteral("for"),
        QStringLiteral("select"), QStringLiteral("case"), QStringLiteral("ctype"), QStringLiteral("directcast"),
        QStringLiteral("trycast"), QStringLiteral("cint"), QStringLiteral("cstr"), QStringLiteral("cdbl"),
        QStringLiteral("cbool"), QStringLiteral("clng"), QStringLiteral("cdate"), QStringLiteral("cobj"),
        QStringLiteral("cdec"), QStringLiteral("csng"), QStringLiteral("cshort"), QStringLiteral("cbyte"),
        QStringLiteral("cchar"), QStringLiteral("gettype"), QStringLiteral("nameof"), QStringLiteral("addressof"),
        QStringLiteral("sub"), QStringLiteral("function"), QStringLiteral("new"), QStringLiteral("not"),
        QStringLiteral("and"), QStringLiteral("or"), QStringLiteral("andalso"), QStringLiteral("orelse"),
        QStringLiteral("return"), QStringLiteral("dim"), QStringLiteral("as"), QStringLiteral("of"),
        QStringLiteral("using"), QStringLiteral("with"), QStringLiteral("catch"), QStringLiteral("when"),
        QStringLiteral("until"), QStringLiteral("is"), QStringLiteral("isnot"), QStringLiteral("typeof"),
        QStringLiteral("throw"), QStringLiteral("handles"), QStringLiteral("implements"), QStringLiteral("property"),
        QStringLiteral("get"), QStringLiteral("set"), QStringLiteral("in"), QStringLiteral("to"), QStringLiteral("step"),
        QStringLiteral("do"), QStringLiteral("loop"), QStringLiteral("then"), QStringLiteral("else"),
        QStringLiteral("iif"), QStringLiteral("if"), QStringLiteral("synclock"), QStringLiteral("raiseevent"),
    };
    for (int lineNumber = fromLine; lineNumber <= toLine && lineNumber <= lines.size(); ++lineNumber) {
        const QString &line = lines.at(lineNumber - 1);
        QSet<int> constructed;
        auto constructIt = construct.globalMatch(line);
        while (constructIt.hasNext()) {
            const auto match = constructIt.next();
            const QString type = match.captured(1).section(QLatin1Char('.'), -1);
            sites.append({type, QStringLiteral("new"), lineNumber});
            constructed.insert(match.capturedStart(1) + match.captured(1).size() - type.size());
        }
        const auto statement = callStatement.match(line);
        if (statement.hasMatch()) {
            sites.append({statement.captured(2), statement.captured(1), lineNumber});
        }
        auto callIt = call.globalMatch(line);
        while (callIt.hasNext()) {
            const auto match = callIt.next();
            const QString name = match.captured(2);
            if (keywords.contains(name.toLower()) || constructed.contains(match.capturedStart(2))) {
                continue;
            }
            // `RaiseEvent Changed(...)` is not a call of a method.
            const QString before = line.left(match.capturedStart(0)).trimmed();
            if (before.endsWith(QStringLiteral("RaiseEvent"), Qt::CaseInsensitive)
                || before.endsWith(QStringLiteral(" As"), Qt::CaseInsensitive)) {
                continue;
            }
            sites.append({name, match.captured(1), lineNumber});
        }
    }
}

void collectShellSites(const QStringList &lines, int fromLine, int toLine, QVector<TextCallSite> &sites)
{
    static const QRegularExpression separators(QStringLiteral(R"(;|&&|\|\||\||\$\(|`|\{|\(|\)|\})"));
    static const QSet<QString> prefixes = {
        QStringLiteral("if"), QStringLiteral("then"), QStringLiteral("else"), QStringLiteral("elif"),
        QStringLiteral("while"), QStringLiteral("until"), QStringLiteral("do"), QStringLiteral("!"),
        QStringLiteral("time"), QStringLiteral("exec"), QStringLiteral("command"), QStringLiteral("builtin"),
        QStringLiteral("nohup"), QStringLiteral("eval"),
    };
    static const QSet<QString> nonCommands = {
        QStringLiteral("fi"), QStringLiteral("done"), QStringLiteral("esac"), QStringLiteral("for"),
        QStringLiteral("case"), QStringLiteral("in"), QStringLiteral("function"), QStringLiteral("select"),
        QStringLiteral("local"), QStringLiteral("export"), QStringLiteral("declare"), QStringLiteral("readonly"),
        QStringLiteral("typeset"), QStringLiteral("return"), QStringLiteral("exit"), QStringLiteral("set"),
        QStringLiteral("unset"), QStringLiteral("shift"), QStringLiteral("source"), QStringLiteral("."),
        QStringLiteral("test"), QStringLiteral("["), QStringLiteral("[["), QStringLiteral("]]"), QStringLiteral("]"),
        QStringLiteral("echo"), QStringLiteral("printf"), QStringLiteral("cd"), QStringLiteral("true"),
        QStringLiteral("false"), QStringLiteral("break"), QStringLiteral("continue"), QStringLiteral("trap"),
    };
    static const QRegularExpression word(QStringLiteral(R"(^[A-Za-z_][\w:.-]*$)"));
    for (int lineNumber = fromLine; lineNumber <= toLine && lineNumber <= lines.size(); ++lineNumber) {
        const QString line = lines.at(lineNumber - 1);
        for (const QString &segment : line.split(separators)) {
            const QStringList words = segment.split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
            int index = 0;
            while (index < words.size()
                   && (prefixes.contains(words.at(index)) || words.at(index).contains(QLatin1Char('=')))) {
                ++index; // keywords and VAR=value prefixes
            }
            if (index >= words.size()) {
                continue;
            }
            const QString command = words.at(index);
            if (nonCommands.contains(command) || !word.match(command).hasMatch()) {
                continue;
            }
            sites.append({command, QString(), lineNumber});
        }
    }
}

QVariantList siteListFor(const QVector<TextCallSite> &sites, int limit)
{
    QVariantList list;
    QSet<QString> seen;
    for (const TextCallSite &site : sites) {
        const QString id = site.qualifier + QLatin1Char('|') + site.name;
        if (site.name.isEmpty() || seen.contains(id)) {
            continue;
        }
        seen.insert(id);
        QVariantMap entry{{QStringLiteral("name"), site.name}, {QStringLiteral("line"), site.line}};
        if (!site.qualifier.isEmpty()) {
            entry.insert(QStringLiteral("qualifier"), site.qualifier);
        }
        list.append(entry);
        if (list.size() >= limit) {
            break;
        }
    }
    return list;
}

QVariantMap applyTextCallSites(QVariantMap analysis, const QString &text, const QString &language)
{
    QString clean;
    if (language == QStringLiteral("objc")) {
        clean = blankCFamilyNoise(text);
    } else if (language == QStringLiteral("shell")) {
        clean = blankShellNoise(text);
    } else if (language == QStringLiteral("vbnet")) {
        clean = blankVbNoise(text);
    } else {
        return analysis;
    }
    const QStringList lines = clean.split(QLatin1Char('\n'));
    QVector<int> lineStarts;
    lineStarts.reserve(lines.size());
    int offset = 0;
    for (const QString &line : lines) {
        lineStarts.append(offset);
        offset += line.size() + 1;
    }

    auto sitesBetween = [&](int fromLine, int toLine) {
        QVector<TextCallSite> sites;
        if (fromLine > toLine) {
            return sites;
        }
        if (language == QStringLiteral("objc")) {
            const int from = lineStarts.value(fromLine - 1, clean.size());
            const int to = toLine < lines.size() ? lineStarts.at(toLine) : clean.size();
            collectObjcSites(clean, from, qMin(to, clean.size()), lineStarts, sites);
        } else if (language == QStringLiteral("vbnet")) {
            collectVbSites(lines, fromLine, toLine, sites);
        } else {
            collectShellSites(lines, fromLine, toLine, sites);
        }
        return sites;
    };

    QVector<bool> covered(lines.size() + 2, false);
    std::function<QVariantList(QVariantList, int)> apply = [&](QVariantList symbols, int depth) {
        for (int index = 0; index < symbols.size(); ++index) {
            QVariantMap symbol = symbols.at(index).toMap();
            const int line = symbol.value(QStringLiteral("line")).toInt();
            const int endLine = symbol.value(QStringLiteral("endLine")).toInt();
            if (depth < 3) {
                symbol.insert(QStringLiteral("members"), apply(symbol.value(QStringLiteral("members")).toList(), depth + 1));
            }
            covered[qBound(0, line, int(covered.size()) - 1)] = true; // declaration lines are not module code
            if (isTextCallSiteOwnerKind(symbol.value(QStringLiteral("kind")).toString()) && endLine > line) {
                for (int covering = line; covering <= endLine && covering < covered.size(); ++covering) {
                    covered[covering] = true;
                }
                const QVariantList sites = siteListFor(sitesBetween(line + 1, endLine), 200);
                if (!sites.isEmpty()) {
                    symbol.insert(QStringLiteral("callSites"), sites);
                }
            }
            symbols[index] = symbol;
        }
        return symbols;
    };
    analysis.insert(QStringLiteral("symbols"), apply(analysis.value(QStringLiteral("symbols")).toList(), 0));

    QVector<TextCallSite> moduleSites;
    int runStart = 0;
    for (int line = 1; line <= lines.size() + 1; ++line) {
        const bool isCovered = line > lines.size() || covered.at(line);
        if (!isCovered && runStart == 0) {
            runStart = line;
        } else if (isCovered && runStart > 0) {
            moduleSites += sitesBetween(runStart, line - 1);
            runStart = 0;
        }
    }
    const QVariantList moduleList = siteListFor(moduleSites, 300);
    if (!moduleList.isEmpty()) {
        analysis.insert(QStringLiteral("moduleCallSites"), moduleList);
    }
    return analysis;
}

} // namespace
