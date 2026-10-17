#include "lumencode_bridge.h"

#include <tree_sitter/api.h>
#include "swift.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

extern "C" const TSLanguage *tree_sitter_swift(void);
extern "C" const TSLanguage *tree_sitter_java(void);
extern "C" const TSLanguage *tree_sitter_c_sharp(void);
extern "C" const TSLanguage *tree_sitter_javascript(void);
extern "C" const TSLanguage *tree_sitter_typescript(void);
extern "C" const TSLanguage *tree_sitter_tsx(void);
extern "C" const TSLanguage *tree_sitter_php(void);
extern "C" const TSLanguage *tree_sitter_css(void);
extern "C" const TSLanguage *tree_sitter_python(void);
extern "C" const TSLanguage *tree_sitter_rust(void);
extern "C" const TSLanguage *tree_sitter_html(void);

// ============================================================================
// AST Repair Infrastructure (from Recovery.cpp)
// ============================================================================
namespace recovery {

struct RepairResult {
    std::string source;
    std::vector<int> blankedLines;  // 1-indexed line numbers
    bool clean = false;
};

struct ErrorScore {
    int errorNodes = 0;
    int64_t errorBytes = 0;
    int declarations = 0;

    bool isClean() const { return errorNodes == 0; }

    bool operator<(const ErrorScore &other) const {
        if (errorBytes != other.errorBytes) {
            return errorBytes < other.errorBytes;
        }
        return errorNodes < other.errorNodes;
    }
};

struct RepairCandidates {
    std::vector<int> primary;
    std::vector<int> secondary;
    std::vector<std::pair<int, int>> errorSpans;
};

static thread_local const std::string *t_astSourceOverride = nullptr;

class ScopedSourceOverride {
public:
    explicit ScopedSourceOverride(const std::string &source)
        : m_previous(t_astSourceOverride) {
        t_astSourceOverride = &source;
    }
    ~ScopedSourceOverride() { t_astSourceOverride = m_previous; }
    ScopedSourceOverride(const ScopedSourceOverride &) = delete;
    ScopedSourceOverride &operator=(const ScopedSourceOverride &) = delete;

private:
    const std::string *m_previous;
};

static const std::string &effectiveSource(const std::string &original) {
    if (t_astSourceOverride && t_astSourceOverride->size() == original.size()) {
        return *t_astSourceOverride;
    }
    return original;
}

static TSLanguage *recoveryLanguageForName(const std::string &language) {
    if (language == "swift") return const_cast<TSLanguage *>(tree_sitter_swift());
    if (language == "java") return const_cast<TSLanguage *>(tree_sitter_java());
    if (language == "cs") return const_cast<TSLanguage *>(tree_sitter_c_sharp());
    if (language == "script" || language == "jsx") return const_cast<TSLanguage *>(tree_sitter_javascript());
    if (language == "ts" || language == "tsx") {
        return language == "tsx" ? const_cast<TSLanguage *>(tree_sitter_tsx())
                                  : const_cast<TSLanguage *>(tree_sitter_typescript());
    }
    if (language == "php") return const_cast<TSLanguage *>(tree_sitter_php());
    if (language == "css") return const_cast<TSLanguage *>(tree_sitter_css());
    if (language == "python") return const_cast<TSLanguage *>(tree_sitter_python());
    if (language == "rust") return const_cast<TSLanguage *>(tree_sitter_rust());
    return nullptr;
}

static std::vector<int> lineStartOffsets(const std::string &source) {
    std::vector<int> starts{0};
    for (int i = 0; i < static_cast<int>(source.size()); ++i) {
        if (source[i] == '\n') {
            starts.push_back(i + 1);
        }
    }
    return starts;
}

static std::string blankSourceRow(std::string source, const std::vector<int> &starts, int row) {
    if (row < 0 || row >= static_cast<int>(starts.size())) {
        return source;
    }
    const int begin = starts[row];
    const int end = row + 1 < static_cast<int>(starts.size()) ? starts[row + 1] - 1 : source.size();
    for (int i = begin; i < end; ++i) {
        if (source[i] != '\n' && source[i] != '\r') {
            source[i] = ' ';
        }
    }
    return source;
}

static bool isDeclarationLikeType(const char *type) {
    return std::strstr(type, "declaration") || std::strstr(type, "definition")
        || std::strstr(type, "_item") || std::strstr(type, "method")
        || std::strstr(type, "function") || std::strstr(type, "class");
}

static int countDeclarationNodes(TSNode node) {
    int total = 0;
    if (ts_node_is_named(node)) {
        const char *type = ts_node_type(node);
        if (type && isDeclarationLikeType(type)) {
            total = 1;
        }
    }
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        total += countDeclarationNodes(ts_node_named_child(node, i));
    }
    return total;
}

static void collectErrors(TSNode node, ErrorScore &score, RepairCandidates &candidates) {
    if (ts_node_is_null(node) || !ts_node_has_error(node)) {
        return;
    }

    if (ts_node_is_missing(node)) {
        ++score.errorNodes;
        score.errorBytes += 1;
        const int row = static_cast<int>(ts_node_start_point(node).row);
        candidates.primary.push_back(row);
        if (row > 0) candidates.primary.push_back(row - 1);
        return;
    }

    if (ts_node_is_error(node)) {
        ++score.errorNodes;
        score.errorBytes += std::max<int64_t>(1, ts_node_end_byte(node) - ts_node_start_byte(node));
        const int startRow = static_cast<int>(ts_node_start_point(node).row);
        candidates.secondary.push_back(startRow);
        candidates.errorSpans.push_back({startRow, static_cast<int>(ts_node_end_point(node).row)});

        const uint32_t childCount = ts_node_child_count(node);
        for (uint32_t i = 0; i < childCount; ++i) {
            TSNode child = ts_node_child(node, i);
            const int row = static_cast<int>(ts_node_start_point(child).row);
            if (ts_node_is_error(child) || ts_node_is_missing(child) || !ts_node_is_named(child)
                || ts_node_child_count(child) == 0) {
                candidates.primary.push_back(row);
            }
        }
    }

    const uint32_t childCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < childCount; ++i) {
        collectErrors(ts_node_child(node, i), score, candidates);
    }
}

static ErrorScore scoreAST(TSParser *parser, const std::string &source, RepairCandidates *outCandidates) {
    ErrorScore score;
    const std::string &effective = effectiveSource(source);
    TSTree *tree = ts_parser_parse_string(parser, nullptr, effective.c_str(), effective.size());

    if (!tree) {
        score.errorNodes = 1;
        score.errorBytes = source.size();
        return score;
    }

    RepairCandidates candidates;
    TSNode root = ts_tree_root_node(tree);
    collectErrors(root, score, candidates);
    score.declarations = countDeclarationNodes(root);

    ts_tree_delete(tree);

    if (outCandidates) {
        *outCandidates = candidates;
    }
    return score;
}

static std::set<int> textuallySuspectRows(const std::string &source, const std::vector<int> &starts) {
    std::set<int> suspects;

    for (int row = 0; row < static_cast<int>(starts.size()); ++row) {
        const int begin = starts[row];
        const int end = row + 1 < static_cast<int>(starts.size()) ? starts[row + 1] - 1 : source.size();
        const std::string line = source.substr(begin, end - begin);

        int net = 0;
        char quote = 0;

        for (size_t i = 0; i < line.size(); ++i) {
            const char ch = line[i];

            if (quote) {
                if (ch == '\\') {
                    ++i;
                } else if (ch == quote) {
                    quote = 0;
                }
                continue;
            }

            if (ch == '/' && i + 1 < line.size() && (line[i + 1] == '/' || line[i + 1] == '*')) {
                break;
            }
            if (ch == '#' && (i == 0 || line[i - 1] == ' ')) {
                break;
            }

            if (ch == '"' || ch == '\'') {
                const bool apostrophe = ch == '\'' && i > 0 && std::isalnum(static_cast<unsigned char>(line[i - 1]));
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
        }
    }

    return suspects;
}

static std::vector<int> normalizeRows(const std::vector<int> &rows, const std::set<int> &blanked) {
    std::set<int> normalized;
    for (int row : rows) {
        if (row >= 0 && !blanked.count(row)) {
            normalized.insert(row);
        }
    }
    return std::vector<int>(normalized.begin(), normalized.end());
}

static bool isBetterRepair(const ErrorScore &candidate, const ErrorScore &best, bool bestIsEmpty) {
    if (bestIsEmpty) {
        return candidate < best;
    }
    if (candidate.declarations != best.declarations) {
        return candidate.declarations > best.declarations;
    }
    return candidate < best;
}

RepairResult repairAST(const std::string &original, const std::string &language) {
    RepairResult result;
    result.source = original;

    constexpr int kMaxBlankedLines = 12;
    constexpr int kMaxTrialParses = 80;
    constexpr auto kMaxBudgetMs = std::chrono::milliseconds(1500);

    TSLanguage *tsLanguage = recoveryLanguageForName(language);
    if (!tsLanguage) {
        return result;
    }

    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, tsLanguage)) {
        if (parser) ts_parser_delete(parser);
        return result;
    }

    auto startTime = std::chrono::steady_clock::now();
    const std::vector<int> starts = lineStartOffsets(original);
    const auto suspects = textuallySuspectRows(original, starts);

    RepairCandidates candidates;
    ErrorScore current = scoreAST(parser, result.source, &candidates);
    std::set<int> blanked;
    int trialParses = 0;

    constexpr int kMaxInitialErrorNodes = 25;
    if (current.errorNodes > kMaxInitialErrorNodes) {
        ts_parser_delete(parser);
        return result;
    }

    while (!current.isClean() && blanked.size() < kMaxBlankedLines
           && trialParses < kMaxTrialParses) {
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed > kMaxBudgetMs) {
            break;
        }

        std::vector<int> bestRows;
        ErrorScore bestScore = current;
        std::string bestSource;

        auto candidates_to_try = normalizeRows(candidates.primary, blanked);
        if (candidates_to_try.size() > 10) {
            candidates_to_try.resize(10);
        }

        for (int row : candidates_to_try) {
            std::string trial = result.source;
            trial = blankSourceRow(trial, starts, row);

            if (trial == result.source) continue;
            if (++trialParses > kMaxTrialParses) break;

            RepairCandidates trialCandidates;
            {
                ScopedSourceOverride guard(trial);
                const ErrorScore trialScore = scoreAST(parser, result.source, &trialCandidates);

                if (trialScore < current && isBetterRepair(trialScore, bestScore, bestRows.empty())) {
                    bestScore = trialScore;
                    bestRows = {row};
                    bestSource = trial;
                }
            }
        }

        if (bestRows.empty()) {
            auto secondary_to_try = normalizeRows(candidates.secondary, blanked);
            if (secondary_to_try.size() > 10) {
                secondary_to_try.resize(10);
            }

            for (int row : secondary_to_try) {
                std::string trial = result.source;
                trial = blankSourceRow(trial, starts, row);

                if (trial == result.source) continue;
                if (++trialParses > kMaxTrialParses) break;

                RepairCandidates trialCandidates;
                {
                    ScopedSourceOverride guard(trial);
                    const ErrorScore trialScore = scoreAST(parser, result.source, &trialCandidates);

                    if (trialScore < current && isBetterRepair(trialScore, bestScore, bestRows.empty())) {
                        bestScore = trialScore;
                        bestRows = {row};
                        bestSource = trial;
                    }
                }
            }
        }

        if (bestRows.empty()) {
            break;
        }

        for (int row : bestRows) {
            blanked.insert(row);
        }

        result.source = bestSource;
        current = bestScore;
        candidates.primary.clear();
        candidates.secondary.clear();

        ErrorScore newScore = scoreAST(parser, result.source, &candidates);
        current = newScore;
    }

    ts_parser_delete(parser);

    for (int row : blanked) {
        result.blankedLines.push_back(row + 1);
    }
    std::sort(result.blankedLines.begin(), result.blankedLines.end());

    result.clean = current.isClean();

    if (!result.clean && result.blankedLines.empty()) {
        result.source = original;
    }

    return result;
}

}  // namespace recovery

namespace {

struct SymbolRelation {
    std::string kind;
    std::string name;
    int line = 0;
    std::string detail;
    std::string snippet;
};

struct ParameterInfo {
    std::string name;
    std::string type;
    std::string defaultValue;
    bool isVariadic = false;
    bool isOptional = false;
};

struct SignatureInfo {
    std::vector<ParameterInfo> parameters;
    std::string returnType;
    bool isAsync = false;
    bool isThrows = false;
    std::vector<std::string> typeParameters;
};

struct Symbol {
    std::string kind;
    std::string name;
    int line = 0;
    std::string detail;
    std::string snippet;
    std::vector<Symbol> members;
    std::vector<SymbolRelation> calls;
    std::vector<SymbolRelation> calledBy;
    SignatureInfo signature;  // Optional signature information
};

struct Dependency {
    std::string type;
    std::string label;
    std::string target;
    int line = 0;
    std::string snippet;
    std::string detail;
    std::string path;
    bool exists = false;
};

struct Route {
    std::string method;
    std::string path;
    int line = 0;
    std::string label;
    std::string snippet;
    std::string detail;
};

struct RelatedFile {
    std::string type;
    std::string label;
    std::string path;
    bool exists = false;
};

struct QuickLink {
    std::string type;
    std::string label;
    std::string target;
    bool exists = false;
    int line = 0;
    std::string snippet;
    std::string detail;
};

struct CssClassItem {
    std::string name;
    std::string kind = "class";
    bool matched = false;
    std::string path;
    int line = 0;
    std::string snippet;
};

struct PackageSummary {
    std::string name;
    std::string version;
    std::string main;
    std::vector<std::string> dependencies;
    std::map<std::string, std::string> scripts;
};

struct CssSummary {
    std::vector<std::string> usedClasses;
    std::vector<std::string> availableClasses;
    std::vector<CssClassItem> matchedClasses;
    std::vector<CssClassItem> missingClasses;
};

struct IndexFact {
    std::string kind;        // "definition" | "import" | "call"
    std::string name;        // Symbol name or import target
    std::string owner;       // Type/class that owns this definition
    std::string file;        // File path
    int line = 0;
    std::string receiver;    // For calls: receiver object name
    std::string binding;     // For imports: local binding
};

struct IndexFacts {
    std::vector<IndexFact> definitions;
    std::vector<IndexFact> imports;
    std::vector<IndexFact> callSites;
};

struct AnalysisResult {
    std::string path;
    std::string fileName;
    std::string language;
    std::vector<Symbol> symbols;
    std::vector<QuickLink> quickLinks;
    std::vector<Dependency> dependencies;
    std::vector<Route> routes;
    std::vector<RelatedFile> relatedFiles;
    PackageSummary packageSummary;
    CssSummary cssSummary;
    std::string summary;

    // Recovery metadata
    std::string analysisSourceMode = "ast";  // "ast" | "recovered" | "heuristic"
    double analysisConfidence = 1.0;          // 0.0-1.0
    std::vector<int> analysisDamagedLines;   // 1-indexed line numbers

    // Index facts (Phase 3)
    IndexFacts indexFacts;                   // Per-file facts for project index
};

std::string symbolKey(const std::string &name, int line)
{
    return name + "|" + std::to_string(line);
}

void collectSymbolPointers(
    std::vector<Symbol> &symbols,
    std::map<std::string, Symbol *> &byKey,
    std::map<std::string, std::vector<std::string>> &keysByName
)
{
    for (auto &symbol : symbols) {
        const std::string key = symbolKey(symbol.name, symbol.line);
        byKey[key] = &symbol;
        keysByName[symbol.name].push_back(key);
        collectSymbolPointers(symbol.members, byKey, keysByName);
    }
}

std::string readFile(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return "";
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    std::string contents = buffer.str();
    if (contents.size() >= 3
        && static_cast<unsigned char>(contents[0]) == 0xEF
        && static_cast<unsigned char>(contents[1]) == 0xBB
        && static_cast<unsigned char>(contents[2]) == 0xBF) {
        contents.erase(0, 3);
    }
    return contents;
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim(const std::string &value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    if (first >= last) {
        return "";
    }
    return std::string(first, last);
}

std::string detectLanguage(const std::filesystem::path &path)
{
    if (lower(path.filename().string()) == "package.json") return "json";

    const std::string ext = lower(path.extension().string());
    if (ext == ".js" || ext == ".mjs" || ext == ".cjs") return "script";
    if (ext == ".swift") return "swift";
    if (ext == ".jsx") return "jsx";
    if (ext == ".ts" || ext == ".mts" || ext == ".cts") return "ts";
    if (ext == ".tsx") return "tsx";
    if (ext == ".php") return "php";
    if (ext == ".html" || ext == ".htm") return "html";
    if (ext == ".css") return "css";
    if (ext == ".json") return "json";
    if (ext == ".py") return "python";
    if (ext == ".rs") return "rust";
    if (ext == ".java") return "java";
    if (ext == ".cs") return "csharp";
    if (ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".h" || ext == ".hpp" || ext == ".m" || ext == ".mm") return "cpp";
    return "";
}

std::string jsonEscape(const std::string &value)
{
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                static const char *digits = "0123456789abcdef";
                out << "\\u00" << digits[(ch >> 4) & 0x0F] << digits[ch & 0x0F];
            } else {
                out << static_cast<char>(ch);
            }
        }
    }
    return out.str();
}

std::string quote(const std::string &value)
{
    return "\"" + jsonEscape(value) + "\"";
}

std::string makeId(const std::string &prefix, std::size_t index)
{
    return prefix + std::to_string(1000000000000ULL + index).substr(1);
}

int lineNumberAtOffset(const std::string &text, std::size_t offset)
{
    int line = 1;
    for (std::size_t i = 0; i < offset && i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++line;
        }
    }
    return line;
}

std::string lineSnippetAt(const std::string &text, int lineNumber, int contextLines = 0)
{
    if (lineNumber <= 0) {
        return "";
    }

    std::istringstream stream(text);
    std::vector<std::string> lines;
    for (std::string line; std::getline(stream, line);) {
        lines.push_back(line);
    }
    if (lineNumber > static_cast<int>(lines.size())) {
        return "";
    }

    const int start = std::max(0, lineNumber - 1 - contextLines);
    const int end = std::min(static_cast<int>(lines.size()), lineNumber + contextLines);
    std::ostringstream result;
    for (int i = start; i < end; ++i) {
        if (i > start) {
            result << '\n';
        }
        result << lines[static_cast<std::size_t>(i)];
    }
    return trim(result.str());
}

std::string stripRegexMatches(const std::string &text, const std::regex &pattern)
{
    return std::regex_replace(text, pattern, "");
}

std::string maskRegexMatches(const std::string &text, const std::regex &pattern)
{
    std::string result = text;
    auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const auto &match = *it;
        const std::size_t start = static_cast<std::size_t>(match.position());
        const std::size_t length = static_cast<std::size_t>(match.length());
        for (std::size_t index = start; index < start + length && index < result.size(); ++index) {
            if (result[index] != '\n' && result[index] != '\r') {
                result[index] = ' ';
            }
        }
    }
    return result;
}

std::string nodeText(TSNode node, const std::string &source)
{
    if (ts_node_is_null(node)) {
        return "";
    }
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (end <= start || static_cast<std::size_t>(end) > source.size()) {
        return "";
    }
    return source.substr(start, end - start);
}

int nodeLine(TSNode node)
{
    if (ts_node_is_null(node)) {
        return 0;
    }
    return static_cast<int>(ts_node_start_point(node).row) + 1;
}

TSNode fieldNode(TSNode node, const char *fieldName)
{
    return ts_node_child_by_field_name(node, fieldName, static_cast<uint32_t>(std::strlen(fieldName)));
}

TSNode firstAncestorOfTypes(TSNode node, std::initializer_list<const char *> types)
{
    TSNode current = node;
    while (!ts_node_is_null(current)) {
        const std::string currentType = ts_node_type(current);
        for (const char *type : types) {
            if (currentType == type) {
                return current;
            }
        }
        current = ts_node_parent(current);
    }
    return TSNode{};
}

std::string nodeSnippet(TSNode node, const std::string &source, int maxLines = 10)
{
    const std::string text = nodeText(node, source);
    if (text.empty()) {
        return "";
    }

    std::istringstream stream(text);
    std::ostringstream out;
    std::string line;
    int lineCount = 0;
    while (lineCount < maxLines && std::getline(stream, line)) {
        if (lineCount > 0) {
            out << '\n';
        }
        out << line;
        ++lineCount;
    }
    if (std::getline(stream, line)) {
        out << "\n...";
    }
    return trim(out.str());
}

std::string firstLine(const std::string &value)
{
    const std::size_t newline = value.find('\n');
    return trim(newline == std::string::npos ? value : value.substr(0, newline));
}

std::string trimmedDeclarationHeadline(std::string value)
{
    value = trim(value);
    const std::size_t brace = value.find('{');
    if (brace != std::string::npos) {
        value = value.substr(0, brace);
    }
    const std::size_t where = value.find(" where ");
    if (where != std::string::npos) {
        value = value.substr(0, where);
    }
    return trim(value);
}

std::string declarationDetailFromNode(TSNode node, const std::string &source)
{
    return trimmedDeclarationHeadline(firstLine(nodeText(node, source)));
}

std::string firstIdentifier(const std::string &text)
{
    static const std::regex identifierPattern(R"([A-Za-z_][A-Za-z0-9_]*)");
    std::smatch match;
    if (std::regex_search(text, match, identifierPattern)) {
        return match[0].str();
    }
    return trim(text);
}

std::string firstVariableName(const std::string &text)
{
    static const std::regex varPattern(R"(\b(?:var|let)\s+([A-Za-z_][A-Za-z0-9_]*))");
    std::smatch match;
    if (std::regex_search(text, match, varPattern)) {
        return match[1].str();
    }
    return firstIdentifier(text);
}

std::string normalizeValue(std::string value)
{
    value = trim(value);
    if (value.size() >= 2) {
        const char first = value.front();
        const char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'') || (first == '`' && last == '`')) {
            return value.substr(1, value.size() - 2);
        }
    }
    return value;
}

std::string resolveRelativePath(const std::filesystem::path &basePath, const std::string &target)
{
    const auto resolved = (basePath.parent_path() / target).lexically_normal();
    return resolved.string();
}

std::string resolveImportPath(const std::filesystem::path &basePath, const std::string &target, bool &exists)
{
    exists = false;
    if (!(target.rfind("./", 0) == 0 || target.rfind("../", 0) == 0)) {
        exists = true;
        return "";
    }

    const std::filesystem::path resolved = std::filesystem::path(resolveRelativePath(basePath, target));
    const std::vector<std::filesystem::path> candidates = {
        resolved,
        resolved.string() + ".js",
        resolved.string() + ".jsx",
        resolved.string() + ".ts",
        resolved.string() + ".tsx",
        resolved.string() + ".css",
        resolved.string() + ".scss",
        resolved.string() + ".sass",
        resolved.string() + ".less",
        resolved.string() + ".html",
        resolved.string() + ".htm",
        resolved.string() + ".json",
        resolved.string() + ".py",
        resolved.string() + ".php",
        resolved.string() + ".java",
        resolved.string() + ".cs",
        resolved.string() + ".cpp",
        resolved.string() + ".hpp",
        resolved.string() + ".svg",
        resolved.string() + ".png",
        resolved.string() + ".jpg",
        resolved.string() + ".jpeg",
        resolved.string() + ".gif",
        resolved.string() + ".webp",
        resolved / "index.js",
        resolved / "index.jsx",
        resolved / "index.ts",
        resolved / "index.tsx",
        resolved / "index.css",
        resolved / "index.scss",
        resolved / "index.html",
        resolved / "__init__.py"
    };

    for (const auto &candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            exists = true;
            return candidate.string();
        }
    }

    return resolved.string();
}

std::string stripUrlDecorations(std::string value)
{
    const std::size_t queryIndex = value.find('?');
    const std::size_t hashIndex = value.find('#');
    const std::size_t cutIndex = std::min(
        queryIndex == std::string::npos ? value.size() : queryIndex,
        hashIndex == std::string::npos ? value.size() : hashIndex
    );
    return value.substr(0, cutIndex);
}

std::string resolveLinkedResourcePath(const std::filesystem::path &basePath, const std::string &target, bool &exists)
{
    exists = false;
    const std::string cleaned = stripUrlDecorations(trim(target));
    const std::string lowered = lower(cleaned);
    if (cleaned.empty()) {
        return "";
    }
    if (lowered.find("://") != std::string::npos
        || lowered.rfind("//", 0) == 0
        || lowered.rfind("data:", 0) == 0
        || lowered.rfind("mailto:", 0) == 0
        || lowered.rfind("javascript:", 0) == 0) {
        exists = true;
        return "";
    }

    std::filesystem::path resolved;
    if (!cleaned.empty() && cleaned.front() == '/') {
        resolved = (basePath.parent_path() / cleaned.substr(1)).lexically_normal();
    } else {
        resolved = (basePath.parent_path() / cleaned).lexically_normal();
    }
    exists = std::filesystem::exists(resolved);
    return resolved.string();
}

template <typename T>
void uniquePush(std::vector<T> &items, std::set<std::string> &seen, const std::string &key, T item)
{
    if (seen.insert(key).second) {
        items.push_back(std::move(item));
    }
}

std::string upper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

void appendSymbolsForRegex(
    const std::string &text,
    const std::regex &pattern,
    const std::string &kind,
    std::vector<Symbol> &symbols,
    const std::string &detail = ""
)
{
    auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const auto &match = *it;
        if (match.size() < 2) {
            continue;
        }
        const int line = lineNumberAtOffset(text, static_cast<std::size_t>(match.position()));
        symbols.push_back(Symbol{
            kind,
            match[1].str(),
            line,
            detail,
            lineSnippetAt(text, line)
        });
    }
}

std::string swiftDeclarationKind(TSNode node, const std::string &source)
{
    const std::string type = ts_node_type(node);
    if (type == "protocol_declaration") {
        return "protocol";
    }
    if (type == "typealias_declaration") {
        return "typealias";
    }
    if (type == "associatedtype_declaration") {
        return "associatedtype";
    }
    if (type == "init_declaration") {
        return "initializer";
    }
    if (type == "deinit_declaration") {
        return "deinitializer";
    }
    if (type == "function_declaration" || type == "protocol_function_declaration") {
        return "function";
    }
    if (type == "property_declaration" || type == "protocol_property_declaration") {
        return "property";
    }
    if (type == "class_declaration") {
        const std::string kind = lower(nodeText(fieldNode(node, "declaration_kind"), source));
        return kind.empty() ? "type" : kind;
    }
    return "symbol";
}

std::string swiftDeclarationName(TSNode node, const std::string &source)
{
    const std::string type = ts_node_type(node);
    if (type == "init_declaration") {
        return "init";
    }
    if (type == "deinit_declaration") {
        return "deinit";
    }

    const std::string nameText = trim(nodeText(fieldNode(node, "name"), source));
    if (!nameText.empty()) {
        if (type == "property_declaration" || type == "protocol_property_declaration") {
            return firstVariableName(nameText);
        }
        return firstIdentifier(nameText);
    }

    const std::string snippet = nodeText(node, source);
    if (type == "property_declaration" || type == "protocol_property_declaration") {
        return firstVariableName(snippet);
    }
    return firstIdentifier(snippet);
}

std::vector<Symbol> extractSwiftBodyMembers(TSNode body, const std::string &source);

Symbol makeSwiftSymbol(TSNode node, const std::string &source)
{
    const std::string kind = swiftDeclarationKind(node, source);
    const std::string name = swiftDeclarationName(node, source);
    Symbol symbol{
        kind,
        name,
        nodeLine(node),
        declarationDetailFromNode(node, source),
        nodeSnippet(node, source)
    };

    const char *nodeType = ts_node_type(node);
    if (std::strcmp(nodeType, "class_declaration") == 0 || std::strcmp(nodeType, "protocol_declaration") == 0) {
        symbol.members = extractSwiftBodyMembers(fieldNode(node, "body"), source);
    }

    return symbol;
}

std::vector<Symbol> extractSwiftBodyMembers(TSNode body, const std::string &source)
{
    std::vector<Symbol> members;
    if (ts_node_is_null(body)) {
        return members;
    }

    const uint32_t count = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_named_child(body, i);
        const std::string type = ts_node_type(child);
        if (type == "class_declaration"
            || type == "protocol_declaration"
            || type == "function_declaration"
            || type == "protocol_function_declaration"
            || type == "init_declaration"
            || type == "deinit_declaration"
            || type == "property_declaration"
            || type == "protocol_property_declaration"
            || type == "typealias_declaration"
            || type == "associatedtype_declaration") {
            members.push_back(makeSwiftSymbol(child, source));
        }
    }
    return members;
}

std::vector<Symbol> extractSwiftSymbolsWithTreeSitter(const std::string &text)
{
    std::vector<Symbol> symbols;
    TSParser *parser = ts_parser_new();
    if (!parser) {
        return symbols;
    }

    const TSLanguage *language = tree_sitter_swift();
    if (!language || !ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return symbols;
    }

    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return symbols;
    }

    const TSNode root = ts_tree_root_node(tree);
    const uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_named_child(root, i);
        const std::string type = ts_node_type(child);
        if (type == "class_declaration"
            || type == "protocol_declaration"
            || type == "function_declaration"
            || type == "property_declaration"
            || type == "typealias_declaration") {
            symbols.push_back(makeSwiftSymbol(child, text));
        }
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return symbols;
}

std::string joinStrings(const std::vector<std::string> &values, const std::string &separator)
{
    std::ostringstream out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            out << separator;
        }
        out << values[index];
    }
    return out.str();
}

TSLanguage *languageForName(const std::string &language)
{
    if (language == "swift") return const_cast<TSLanguage *>(tree_sitter_swift());
    if (language == "java") return const_cast<TSLanguage *>(tree_sitter_java());
    if (language == "csharp") return const_cast<TSLanguage *>(tree_sitter_c_sharp());
    if (language == "script" || language == "jsx") return const_cast<TSLanguage *>(tree_sitter_javascript());
    if (language == "ts") return const_cast<TSLanguage *>(tree_sitter_typescript());
    if (language == "tsx") return const_cast<TSLanguage *>(tree_sitter_tsx());
    if (language == "php") return const_cast<TSLanguage *>(tree_sitter_php());
    if (language == "css") return const_cast<TSLanguage *>(tree_sitter_css());
    if (language == "python") return const_cast<TSLanguage *>(tree_sitter_python());
    if (language == "rust") return const_cast<TSLanguage *>(tree_sitter_rust());
    if (language == "html") return const_cast<TSLanguage *>(tree_sitter_html());
    return nullptr;
}

std::string identifierFromNode(TSNode node, const std::string &source)
{
    return firstIdentifier(trim(nodeText(node, source)));
}

std::string lastIdentifier(const std::string &text)
{
    static const std::regex identifierPattern(R"(([A-Za-z_][A-Za-z0-9_]*)\s*$)");
    std::smatch match;
    if (std::regex_search(text, match, identifierPattern)) {
        return match[1].str();
    }
    return "";
}

std::string scriptCallableKeyForNode(
    TSNode node,
    const std::string &source,
    const std::map<std::string, Symbol *> &byKey
)
{
    const std::string type = ts_node_type(node);
    std::string name;
    int line = 0;

    if (type == "function_declaration" || type == "generator_function_declaration") {
        name = identifierFromNode(fieldNode(node, "name"), source);
        line = nodeLine(node);
    } else if (type == "variable_declarator") {
        TSNode valueNode = fieldNode(node, "value");
        const std::string valueType = ts_node_is_null(valueNode) ? "" : std::string(ts_node_type(valueNode));
        if (valueType == "arrow_function" || valueType == "function" || valueType == "function_expression" || valueType == "generator_function") {
            name = identifierFromNode(fieldNode(node, "name"), source);
            line = nodeLine(node);
        }
    } else if (type == "method_definition") {
        name = identifierFromNode(fieldNode(node, "name"), source);
        line = nodeLine(node);
    } else if (type == "pair") {
        TSNode keyNode = fieldNode(node, "key");
        TSNode valueNode = fieldNode(node, "value");
        const std::string valueType = ts_node_is_null(valueNode) ? "" : std::string(ts_node_type(valueNode));
        if (valueType == "arrow_function" || valueType == "function" || valueType == "function_expression" || valueType == "generator_function") {
            name = identifierFromNode(keyNode, source);
            line = nodeLine(node);
        }
    }

    if (name.empty() || line <= 0) {
        return "";
    }

    const std::string key = symbolKey(name, line);
    return byKey.find(key) != byKey.end() ? key : "";
}

std::string scriptCallTargetName(TSNode node, const std::string &source)
{
    if (std::string(ts_node_type(node)) != "call_expression") {
        return "";
    }

    const std::string raw = trim(nodeText(fieldNode(node, "function"), source));
    if (raw.empty()) {
        return "";
    }

    const std::string candidate = lastIdentifier(raw);
    static const std::set<std::string> excluded = {
        "if", "for", "while", "switch", "catch", "return", "typeof",
        "console", "log", "map", "filter", "reduce", "push", "pop"
    };
    if (candidate.empty() || excluded.find(candidate) != excluded.end()) {
        return "";
    }
    return candidate;
}

std::string pythonCallableKeyForNode(
    TSNode node,
    const std::string &source,
    const std::map<std::string, Symbol *> &byKey
)
{
    TSNode definitionNode = node;
    const std::string type = ts_node_type(node);
    if (type == "decorated_definition") {
        definitionNode = fieldNode(node, "definition");
    }

    if (ts_node_is_null(definitionNode) || std::string(ts_node_type(definitionNode)) != "function_definition") {
        return "";
    }

    const std::string name = identifierFromNode(fieldNode(definitionNode, "name"), source);
    const int line = nodeLine(definitionNode);
    if (name.empty() || line <= 0) {
        return "";
    }

    const std::string key = symbolKey(name, line);
    return byKey.find(key) != byKey.end() ? key : "";
}

std::string pythonCallTargetName(TSNode node, const std::string &source)
{
    if (std::string(ts_node_type(node)) != "call") {
        return "";
    }

    const std::string raw = trim(nodeText(fieldNode(node, "function"), source));
    if (raw.empty()) {
        return "";
    }

    static const std::set<std::string> excluded = {
        "if", "for", "while", "with", "return", "print", "len", "range", "str", "int", "bool"
    };
    const std::string candidate = lastIdentifier(raw);
    if (candidate.empty() || excluded.find(candidate) != excluded.end()) {
        return "";
    }
    return candidate;
}

std::string rustCallableKeyForNode(
    TSNode node,
    const std::string &source,
    const std::map<std::string, Symbol *> &byKey
)
{
    if (std::string(ts_node_type(node)) != "function_item") {
        return "";
    }

    const std::string name = identifierFromNode(fieldNode(node, "name"), source);
    const int line = nodeLine(node);
    if (name.empty() || line <= 0) {
        return "";
    }

    const std::string key = symbolKey(name, line);
    return byKey.find(key) != byKey.end() ? key : "";
}

std::string rustCallTargetName(TSNode node, const std::string &source)
{
    if (std::string(ts_node_type(node)) != "call_expression") {
        return "";
    }

    const std::string raw = trim(nodeText(fieldNode(node, "function"), source));
    if (raw.empty()) {
        return "";
    }

    static const std::set<std::string> excluded = {
        "if", "for", "while", "loop", "match", "Some", "None", "Ok", "Err"
    };
    const std::string candidate = lastIdentifier(raw);
    if (candidate.empty() || excluded.find(candidate) != excluded.end()) {
        return "";
    }
    return candidate;
}

void appendRelationUnique(std::vector<SymbolRelation> &relations, const Symbol &target, const std::string &detail)
{
    for (const auto &existing : relations) {
        if (existing.name == target.name && existing.line == target.line) {
            return;
        }
    }
    relations.push_back(SymbolRelation{target.kind, target.name, target.line, detail, target.snippet});
}

bool javaNodeHasModifier(TSNode node, const std::string &source, const std::string &modifier)
{
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode child = ts_node_named_child(node, index);
        if (std::string(ts_node_type(child)) == "modifiers"
            && nodeText(child, source).find(modifier) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string javaSymbolDetail(TSNode node, const std::string &source)
{
    std::vector<std::string> details;
    for (const std::string &modifier : {"public", "protected", "private", "abstract", "static"}) {
        if (javaNodeHasModifier(node, source, modifier)) {
            details.push_back(modifier);
        }
    }
    return joinStrings(details, ", ");
}

std::vector<Symbol> extractJavaMembers(TSNode bodyNode, const std::string &source);

Symbol makeJavaSymbol(TSNode node, const std::string &source, const std::string &kind)
{
    const std::string modifiers = javaSymbolDetail(node, source);
    std::string detail = declarationDetailFromNode(node, source);
    if (!modifiers.empty() && detail.find(modifiers) == std::string::npos) {
        detail = modifiers + (detail.empty() ? "" : " • " + detail);
    }
    Symbol symbol{
        kind,
        trim(nodeText(fieldNode(node, "name"), source)),
        nodeLine(node),
        detail,
        nodeSnippet(node, source)
    };

    const std::string type = ts_node_type(node);
    if (type == "class_declaration"
        || type == "interface_declaration"
        || type == "enum_declaration"
        || type == "record_declaration") {
        symbol.members = extractJavaMembers(fieldNode(node, "body"), source);
    }
    return symbol;
}

std::vector<Symbol> javaFieldMembers(TSNode node, const std::string &source, const std::string &kind)
{
    std::vector<Symbol> members;
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode child = ts_node_named_child(node, index);
        if (std::string(ts_node_type(child)) != "variable_declarator") {
            continue;
        }
        const std::string name = trim(nodeText(fieldNode(child, "name"), source));
        if (name.empty()) {
            continue;
        }
        members.push_back(Symbol{
            kind,
            name,
            nodeLine(child),
            declarationDetailFromNode(node, source),
            nodeSnippet(node, source)
        });
    }
    return members;
}

std::vector<Symbol> extractJavaMembers(TSNode bodyNode, const std::string &source)
{
    std::vector<Symbol> members;
    if (ts_node_is_null(bodyNode)) {
        return members;
    }

    const uint32_t count = ts_node_named_child_count(bodyNode);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode child = ts_node_named_child(bodyNode, index);
        const std::string type = ts_node_type(child);
        if (type == "method_declaration") {
            members.push_back(makeJavaSymbol(child, source, "method"));
        } else if (type == "constructor_declaration") {
            members.push_back(makeJavaSymbol(child, source, "constructor"));
        } else if (type == "field_declaration") {
            auto fields = javaFieldMembers(child, source, "property");
            members.insert(members.end(), fields.begin(), fields.end());
        } else if (type == "constant_declaration") {
            auto constants = javaFieldMembers(child, source, "constant");
            members.insert(members.end(), constants.begin(), constants.end());
        } else if (type == "enum_constant") {
            members.push_back(Symbol{
                "variant",
                trim(nodeText(fieldNode(child, "name"), source)),
                nodeLine(child),
                "",
                nodeSnippet(child, source)
            });
        } else if (type == "class_declaration") {
            members.push_back(makeJavaSymbol(child, source, "class"));
        } else if (type == "interface_declaration") {
            members.push_back(makeJavaSymbol(child, source, "interface"));
        } else if (type == "enum_declaration") {
            members.push_back(makeJavaSymbol(child, source, "enum"));
        } else if (type == "record_declaration") {
            members.push_back(makeJavaSymbol(child, source, "record"));
        }
    }
    return members;
}

std::vector<Symbol> extractJavaSymbolsWithTreeSitter(const std::string &text)
{
    std::vector<Symbol> symbols;
    TSParser *parser = ts_parser_new();
    if (!parser) {
        return symbols;
    }

    const TSLanguage *language = tree_sitter_java();
    if (!language || !ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return symbols;
    }

    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return symbols;
    }

    std::set<std::string> seen;
    const TSNode root = ts_tree_root_node(tree);
    const uint32_t count = ts_node_named_child_count(root);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode child = ts_node_named_child(root, index);
        const std::string type = ts_node_type(child);
        Symbol symbol;
        bool matched = true;
        if (type == "class_declaration") {
            symbol = makeJavaSymbol(child, text, "class");
        } else if (type == "interface_declaration") {
            symbol = makeJavaSymbol(child, text, "interface");
        } else if (type == "enum_declaration") {
            symbol = makeJavaSymbol(child, text, "enum");
        } else if (type == "record_declaration") {
            symbol = makeJavaSymbol(child, text, "record");
        } else {
            matched = false;
        }

        if (matched && !symbol.name.empty()) {
            uniquePush(symbols, seen, symbol.kind + "|" + symbol.name + "|" + std::to_string(symbol.line), std::move(symbol));
        }
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return symbols;
}

bool csharpNodeHasModifier(TSNode node, const std::string &source, const std::string &modifier)
{
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode child = ts_node_named_child(node, index);
        if (std::string(ts_node_type(child)) == "modifier"
            && trim(nodeText(child, source)) == modifier) {
            return true;
        }
    }
    return false;
}

std::string csharpSymbolDetail(TSNode node, const std::string &source)
{
    std::vector<std::string> details;
    for (const std::string &modifier : {"public", "protected", "private", "internal", "static", "abstract"}) {
        if (csharpNodeHasModifier(node, source, modifier)) {
            details.push_back(modifier);
        }
    }
    return joinStrings(details, ", ");
}

std::vector<Symbol> extractCSharpMembers(TSNode bodyNode, const std::string &source);

Symbol makeCSharpSymbol(TSNode node, const std::string &source, const std::string &kind)
{
    const std::string modifiers = csharpSymbolDetail(node, source);
    std::string detail = declarationDetailFromNode(node, source);
    if (!modifiers.empty() && detail.find(modifiers) == std::string::npos) {
        detail = modifiers + (detail.empty() ? "" : " • " + detail);
    }
    Symbol symbol{
        kind,
        trim(nodeText(fieldNode(node, "name"), source)),
        nodeLine(node),
        detail,
        nodeSnippet(node, source)
    };

    const std::string type = ts_node_type(node);
    if (type == "class_declaration"
        || type == "interface_declaration"
        || type == "struct_declaration"
        || type == "record_declaration"
        || type == "enum_declaration") {
        symbol.members = extractCSharpMembers(fieldNode(node, "body"), source);
    }
    return symbol;
}

std::vector<Symbol> extractCSharpMembers(TSNode bodyNode, const std::string &source)
{
    std::vector<Symbol> members;
    if (ts_node_is_null(bodyNode)) {
        return members;
    }

    const uint32_t count = ts_node_named_child_count(bodyNode);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode child = ts_node_named_child(bodyNode, index);
        const std::string type = ts_node_type(child);
        if (type == "method_declaration") {
            members.push_back(makeCSharpSymbol(child, source, "method"));
        } else if (type == "constructor_declaration") {
            members.push_back(makeCSharpSymbol(child, source, "constructor"));
        } else if (type == "property_declaration") {
            members.push_back(makeCSharpSymbol(child, source, "property"));
        } else if (type == "field_declaration") {
            const uint32_t fieldChildCount = ts_node_named_child_count(child);
            for (uint32_t fieldIndex = 0; fieldIndex < fieldChildCount; ++fieldIndex) {
                TSNode fieldChild = ts_node_named_child(child, fieldIndex);
                if (std::string(ts_node_type(fieldChild)) != "variable_declaration") {
                    continue;
                }
                const uint32_t varCount = ts_node_named_child_count(fieldChild);
                for (uint32_t varIndex = 0; varIndex < varCount; ++varIndex) {
                    TSNode declarator = ts_node_named_child(fieldChild, varIndex);
                    if (std::string(ts_node_type(declarator)) != "variable_declarator") {
                        continue;
                    }
                    const std::string name = trim(nodeText(fieldNode(declarator, "name"), source));
                    if (!name.empty()) {
                        members.push_back(Symbol{
                            "property",
                            name,
                            nodeLine(declarator),
                            declarationDetailFromNode(child, source),
                            nodeSnippet(child, source)
                        });
                    }
                }
            }
        } else if (type == "class_declaration") {
            members.push_back(makeCSharpSymbol(child, source, "class"));
        } else if (type == "interface_declaration") {
            members.push_back(makeCSharpSymbol(child, source, "interface"));
        } else if (type == "struct_declaration") {
            members.push_back(makeCSharpSymbol(child, source, "struct"));
        } else if (type == "record_declaration") {
            members.push_back(makeCSharpSymbol(child, source, "record"));
        } else if (type == "enum_declaration") {
            members.push_back(makeCSharpSymbol(child, source, "enum"));
        }
    }
    return members;
}

void walkCSharpScope(TSNode node, const std::string &source, std::vector<Symbol> &symbols, std::set<std::string> &seen)
{
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode child = ts_node_named_child(node, index);
        const std::string type = ts_node_type(child);
        if (type == "namespace_declaration" || type == "file_scoped_namespace_declaration") {
            walkCSharpScope(child, source, symbols, seen);
            continue;
        }

        Symbol symbol;
        bool matched = true;
        if (type == "class_declaration") {
            symbol = makeCSharpSymbol(child, source, "class");
        } else if (type == "interface_declaration") {
            symbol = makeCSharpSymbol(child, source, "interface");
        } else if (type == "struct_declaration") {
            symbol = makeCSharpSymbol(child, source, "struct");
        } else if (type == "record_declaration") {
            symbol = makeCSharpSymbol(child, source, "record");
        } else if (type == "enum_declaration") {
            symbol = makeCSharpSymbol(child, source, "enum");
        } else {
            matched = false;
        }

        if (matched && !symbol.name.empty()) {
            uniquePush(symbols, seen, symbol.kind + "|" + symbol.name + "|" + std::to_string(symbol.line), std::move(symbol));
        }
    }
}

std::vector<Symbol> extractCSharpSymbolsWithTreeSitter(const std::string &text)
{
    std::vector<Symbol> symbols;
    TSParser *parser = ts_parser_new();
    if (!parser) {
        return symbols;
    }

    const TSLanguage *language = tree_sitter_c_sharp();
    if (!language || !ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return symbols;
    }

    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return symbols;
    }

    std::set<std::string> seen;
    walkCSharpScope(ts_tree_root_node(tree), text, symbols, seen);

    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return symbols;
}

std::vector<Symbol> parseScriptClassMembers(TSNode bodyNode, const std::string &source);
std::vector<Symbol> parseScriptObjectMembers(TSNode objectNode, const std::string &source);

std::string classifyScriptFunctionName(const std::string &name, bool reactMode)
{
    if (name.rfind("use", 0) == 0) {
        return "hook";
    }
    if (reactMode && !name.empty() && std::isupper(static_cast<unsigned char>(name.front())) != 0) {
        return "component";
    }
    return "function";
}

std::vector<Symbol> parseScriptClassMembers(TSNode bodyNode, const std::string &source)
{
    std::vector<Symbol> members;
    if (ts_node_is_null(bodyNode)) {
        return members;
    }

    const uint32_t count = ts_node_named_child_count(bodyNode);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode child = ts_node_named_child(bodyNode, index);
        const std::string type = ts_node_type(child);
        if (type == "method_definition") {
            std::string name = trim(nodeText(fieldNode(child, "name"), source));
            if (name.empty() && ts_node_named_child_count(child) > 0) {
                name = identifierFromNode(ts_node_named_child(child, 0), source);
            }
            if (!name.empty()) {
                members.push_back(Symbol{"method", name, nodeLine(child), "", nodeSnippet(child, source)});
            }
        } else if (type == "field_definition" || type == "public_field_definition") {
            std::string name = trim(nodeText(fieldNode(child, "name"), source));
            if (name.empty() && ts_node_named_child_count(child) > 0) {
                name = identifierFromNode(ts_node_named_child(child, 0), source);
            }
            if (!name.empty()) {
                members.push_back(Symbol{"property", name, nodeLine(child), "", nodeSnippet(child, source)});
            }
        }
    }
    return members;
}

std::vector<Symbol> parseScriptObjectMembers(TSNode objectNode, const std::string &source)
{
    std::vector<Symbol> members;
    const uint32_t count = ts_node_named_child_count(objectNode);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode child = ts_node_named_child(objectNode, index);
        const std::string type = ts_node_type(child);
        if (type == "pair") {
            const std::string key = identifierFromNode(fieldNode(child, "key"), source);
            TSNode valueNode = fieldNode(child, "value");
            const std::string valueType = ts_node_is_null(valueNode) ? "" : std::string(ts_node_type(valueNode));
            if (key.empty()) {
                continue;
            }
            if (valueType == "arrow_function" || valueType == "function" || valueType == "function_expression") {
                members.push_back(Symbol{"function", key, nodeLine(child), "", nodeSnippet(child, source)});
            } else if (valueType == "object") {
                members.push_back(Symbol{"property", key, nodeLine(child), "nested object", nodeSnippet(child, source), parseScriptObjectMembers(valueNode, source)});
            } else {
                members.push_back(Symbol{"property", key, nodeLine(child), "", nodeSnippet(child, source)});
            }
        } else if (type == "method_definition") {
            const std::string name = identifierFromNode(fieldNode(child, "name"), source);
            if (!name.empty()) {
                members.push_back(Symbol{"method", name, nodeLine(child), "", nodeSnippet(child, source)});
            }
        } else if (type == "shorthand_property_identifier") {
            const std::string name = identifierFromNode(child, source);
            if (!name.empty()) {
                members.push_back(Symbol{"property", name, nodeLine(child), "", nodeSnippet(child, source)});
            }
        }
    }
    return members;
}

std::vector<Symbol> extractScriptSymbolsWithTreeSitter(const std::string &text, const std::string &language)
{
    std::vector<Symbol> symbols;
    TSParser *parser = ts_parser_new();
    if (!parser) {
        return symbols;
    }

    const TSLanguage *tsLanguage = languageForName(language);
    if (!tsLanguage || !ts_parser_set_language(parser, tsLanguage)) {
        ts_parser_delete(parser);
        return symbols;
    }

    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return symbols;
    }

    const bool reactMode = language == "jsx" || language == "tsx";
    std::set<std::string> seen;
    const TSNode root = ts_tree_root_node(tree);
    const uint32_t count = ts_node_named_child_count(root);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode originalNode = ts_node_named_child(root, index);
        const std::string originalType = ts_node_type(originalNode);
        bool isExported = originalType == "export_statement";
        TSNode child = originalNode;
        if (originalType == "export_statement") {
            const uint32_t childCount = ts_node_named_child_count(originalNode);
            for (uint32_t childIndex = 0; childIndex < childCount; ++childIndex) {
                TSNode candidate = ts_node_named_child(originalNode, childIndex);
                if (std::string(ts_node_type(candidate)) != "comment") {
                    child = candidate;
                    break;
                }
            }
        }

        const std::string type = ts_node_type(child);
        Symbol symbol;
        bool matched = true;
        if (type == "function_declaration" || type == "generator_function_declaration") {
            const std::string name = identifierFromNode(fieldNode(child, "name"), text);
            symbol = Symbol{classifyScriptFunctionName(name, reactMode), name, nodeLine(child), isExported ? "exported" : "", nodeSnippet(child, text)};
        } else if (type == "class_declaration" || type == "abstract_class_declaration") {
            const std::string name = identifierFromNode(fieldNode(child, "name"), text);
            symbol = Symbol{"class", name, nodeLine(child), isExported ? "exported" : "", nodeSnippet(child, text), parseScriptClassMembers(fieldNode(child, "body"), text)};
        } else if (type == "interface_declaration") {
            const std::string name = identifierFromNode(fieldNode(child, "name"), text);
            symbol = Symbol{"props", name, nodeLine(child), isExported ? "exported" : "", nodeSnippet(child, text)};
        } else if (type == "type_alias_declaration") {
            const std::string name = identifierFromNode(fieldNode(child, "name"), text);
            symbol = Symbol{"type", name, nodeLine(child), isExported ? "exported" : "", nodeSnippet(child, text)};
        } else if (type == "lexical_declaration" || type == "variable_declaration") {
            const uint32_t declCount = ts_node_named_child_count(child);
            matched = false;
            for (uint32_t declIndex = 0; declIndex < declCount; ++declIndex) {
                TSNode declarator = ts_node_named_child(child, declIndex);
                if (std::string(ts_node_type(declarator)) != "variable_declarator") {
                    continue;
                }
                const std::string name = identifierFromNode(fieldNode(declarator, "name"), text);
                TSNode valueNode = fieldNode(declarator, "value");
                const std::string valueType = ts_node_is_null(valueNode) ? "" : std::string(ts_node_type(valueNode));
                Symbol item;
                if (valueType == "arrow_function" || valueType == "function" || valueType == "function_expression") {
                    std::string detail = valueType == "function_expression" ? "function expression" : "";
                    if (isExported && detail.empty()) {
                        detail = "exported";
                    }
                    item = Symbol{classifyScriptFunctionName(name, reactMode), name, nodeLine(declarator), detail, nodeSnippet(child, text)};
                } else if (valueType == "class" || valueType == "class_expression") {
                    item = Symbol{"class", name, nodeLine(declarator), isExported ? "exported class expression" : "class expression", nodeSnippet(child, text), parseScriptClassMembers(fieldNode(valueNode, "body"), text)};
                } else if (valueType == "object") {
                    item = Symbol{"variable", name, nodeLine(declarator), isExported ? "exported object" : "object", nodeSnippet(child, text), parseScriptObjectMembers(valueNode, text)};
                } else {
                    item = Symbol{"variable", name, nodeLine(declarator), isExported ? "exported" : "", nodeSnippet(child, text)};
                }
                if (!item.name.empty()) {
                    uniquePush(symbols, seen, item.kind + "|" + item.name + "|" + std::to_string(item.line), std::move(item));
                }
            }
        } else {
            matched = false;
        }

        if (matched && !symbol.name.empty()) {
            uniquePush(symbols, seen, symbol.kind + "|" + symbol.name + "|" + std::to_string(symbol.line), std::move(symbol));
        }
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return symbols;
}

void annotateScriptCallTopology(std::vector<Symbol> &symbols, const std::string &text, const std::string &language)
{
    if (symbols.empty()) {
        return;
    }

    TSParser *parser = ts_parser_new();
    if (!parser) {
        return;
    }

    const TSLanguage *tsLanguage = languageForName(language);
    if (!tsLanguage || !ts_parser_set_language(parser, tsLanguage)) {
        ts_parser_delete(parser);
        return;
    }

    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return;
    }

    std::map<std::string, Symbol *> byKey;
    std::map<std::string, std::vector<std::string>> keysByName;
    collectSymbolPointers(symbols, byKey, keysByName);

    std::function<void(TSNode, const std::string &)> visit = [&](TSNode node, const std::string &currentKey) {
        std::string activeKey = currentKey;
        const std::string nodeKey = scriptCallableKeyForNode(node, text, byKey);
        if (!nodeKey.empty()) {
            activeKey = nodeKey;
        }

        if (!activeKey.empty() && std::string(ts_node_type(node)) == "call_expression") {
            const std::string targetName = scriptCallTargetName(node, text);
            const auto found = keysByName.find(targetName);
            if (!targetName.empty() && found != keysByName.end() && found->second.size() == 1) {
                const std::string targetKey = found->second.front();
                if (targetKey != activeKey) {
                    Symbol *sourceSymbol = byKey[activeKey];
                    Symbol *targetSymbol = byKey[targetKey];
                    if (sourceSymbol && targetSymbol) {
                        appendRelationUnique(sourceSymbol->calls, *targetSymbol, "calls");
                        appendRelationUnique(targetSymbol->calledBy, *sourceSymbol, "called by");
                    }
                }
            }
        }

        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t index = 0; index < count; ++index) {
            visit(ts_node_named_child(node, index), activeKey);
        }
    };

    visit(ts_tree_root_node(tree), "");
    ts_tree_delete(tree);
    ts_parser_delete(parser);
}

std::vector<Symbol> extractPythonSymbolsWithTreeSitter(const std::string &text)
{
    std::vector<Symbol> symbols;
    TSParser *parser = ts_parser_new();
    if (!parser) {
        return symbols;
    }
    const TSLanguage *language = tree_sitter_python();
    if (!language || !ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return symbols;
    }
    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return symbols;
    }

    auto decorationDetail = [&](TSNode decoratedNode) {
        std::vector<std::string> decorators;
        const uint32_t count = ts_node_named_child_count(decoratedNode);
        for (uint32_t index = 0; index < count; ++index) {
            TSNode child = ts_node_named_child(decoratedNode, index);
            if (std::string(ts_node_type(child)) == "decorator") {
                decorators.push_back(trim(nodeText(child, text)));
            }
        }
        return decorators.empty() ? std::string("decorated") : joinStrings(decorators, ", ");
    };

    auto parsePythonClassMembers = [&](TSNode blockNode) {
        std::vector<Symbol> members;
        const uint32_t count = ts_node_named_child_count(blockNode);
        for (uint32_t index = 0; index < count; ++index) {
            TSNode child = ts_node_named_child(blockNode, index);
            TSNode snippetNode = child;
            std::string detail;
            std::string childType = ts_node_type(child);
            if (childType == "decorated_definition") {
                detail = decorationDetail(child);
                child = fieldNode(child, "definition");
                childType = ts_node_is_null(child) ? "" : std::string(ts_node_type(child));
            }
            if (childType != "function_definition") {
                continue;
            }
            const std::string name = identifierFromNode(fieldNode(child, "name"), text);
            if (name.empty()) {
                continue;
            }
            const std::string signature = declarationDetailFromNode(child, text);
            const std::string combinedDetail = detail.empty() ? signature : detail + " • " + signature;
            members.push_back(Symbol{
                detail.find("@property") != std::string::npos ? "property" : "method",
                name,
                nodeLine(child),
                combinedDetail,
                nodeSnippet(snippetNode, text)
            });
        }
        return members;
    };

    auto appendPythonDefinition = [&](TSNode rawNode, TSNode snippetNode, const std::string &detail, std::set<std::string> &seen) {
        const std::string type = ts_node_type(rawNode);
        Symbol symbol;
        if (type == "class_definition") {
            const std::string signature = declarationDetailFromNode(rawNode, text);
            const std::string combinedDetail = detail.empty() ? signature : detail + " • " + signature;
            symbol = Symbol{"class", identifierFromNode(fieldNode(rawNode, "name"), text), nodeLine(rawNode), combinedDetail, nodeSnippet(snippetNode, text), parsePythonClassMembers(fieldNode(rawNode, "body"))};
        } else if (type == "function_definition") {
            const std::string signature = declarationDetailFromNode(rawNode, text);
            const std::string combinedDetail = detail.empty() ? signature : detail + " • " + signature;
            symbol = Symbol{"function", identifierFromNode(fieldNode(rawNode, "name"), text), nodeLine(rawNode), combinedDetail, nodeSnippet(snippetNode, text)};
        }
        if (!symbol.name.empty()) {
            uniquePush(symbols, seen, symbol.kind + "|" + symbol.name + "|" + std::to_string(symbol.line), std::move(symbol));
        }
    };

    std::set<std::string> seen;
    TSNode root = ts_tree_root_node(tree);
    const uint32_t count = ts_node_named_child_count(root);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode child = ts_node_named_child(root, index);
        const std::string type = ts_node_type(child);
        if (type == "class_definition" || type == "function_definition") {
            appendPythonDefinition(child, child, "", seen);
        } else if (type == "decorated_definition") {
            TSNode definition = fieldNode(child, "definition");
            if (!ts_node_is_null(definition)) {
                appendPythonDefinition(definition, child, decorationDetail(child), seen);
            }
        }
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return symbols;
}

void annotatePythonCallTopology(std::vector<Symbol> &symbols, const std::string &text)
{
    if (symbols.empty()) {
        return;
    }

    TSParser *parser = ts_parser_new();
    if (!parser) {
        return;
    }

    const TSLanguage *language = tree_sitter_python();
    if (!language || !ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return;
    }

    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return;
    }

    std::map<std::string, Symbol *> byKey;
    std::map<std::string, std::vector<std::string>> keysByName;
    collectSymbolPointers(symbols, byKey, keysByName);

    std::function<void(TSNode, const std::string &)> visit = [&](TSNode node, const std::string &currentKey) {
        std::string activeKey = currentKey;
        const std::string nodeKey = pythonCallableKeyForNode(node, text, byKey);
        if (!nodeKey.empty()) {
            activeKey = nodeKey;
        }

        if (!activeKey.empty() && std::string(ts_node_type(node)) == "call") {
            const std::string targetName = pythonCallTargetName(node, text);
            const auto found = keysByName.find(targetName);
            if (!targetName.empty() && found != keysByName.end() && found->second.size() == 1) {
                const std::string targetKey = found->second.front();
                if (targetKey != activeKey) {
                    Symbol *sourceSymbol = byKey[activeKey];
                    Symbol *targetSymbol = byKey[targetKey];
                    if (sourceSymbol && targetSymbol) {
                        appendRelationUnique(sourceSymbol->calls, *targetSymbol, "calls");
                        appendRelationUnique(targetSymbol->calledBy, *sourceSymbol, "called by");
                    }
                }
            }
        }

        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t index = 0; index < count; ++index) {
            visit(ts_node_named_child(node, index), activeKey);
        }
    };

    visit(ts_tree_root_node(tree), "");
    ts_tree_delete(tree);
    ts_parser_delete(parser);
}

std::vector<Symbol> extractPhpSymbolsWithTreeSitter(const std::string &text)
{
    std::vector<Symbol> symbols;
    TSParser *parser = ts_parser_new();
    if (!parser) {
        return symbols;
    }
    const TSLanguage *language = tree_sitter_php();
    if (!language || !ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return symbols;
    }
    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return symbols;
    }

    auto parsePhpMembers = [&](TSNode declarationList) {
        std::vector<Symbol> members;
        const uint32_t count = ts_node_named_child_count(declarationList);
        for (uint32_t index = 0; index < count; ++index) {
            TSNode child = ts_node_named_child(declarationList, index);
            const std::string type = ts_node_type(child);
            if (type == "method_declaration") {
                const std::string name = identifierFromNode(fieldNode(child, "name"), text);
                if (!name.empty()) {
                    members.push_back(Symbol{"method", name, nodeLine(child), declarationDetailFromNode(child, text), nodeSnippet(child, text)});
                }
            } else if (type == "property_declaration") {
                const std::string detail = declarationDetailFromNode(child, text);
                const std::string snippet = nodeText(child, text);
                const std::regex propertyPattern(R"(\$([A-Za-z_]\w*))");
                auto begin = std::sregex_iterator(snippet.begin(), snippet.end(), propertyPattern);
                auto end = std::sregex_iterator();
                for (auto it = begin; it != end; ++it) {
                    members.push_back(Symbol{"property", "$" + (*it)[1].str(), nodeLine(child), detail, nodeSnippet(child, text)});
                }
            } else if (type == "const_declaration") {
                const std::string detail = declarationDetailFromNode(child, text);
                const std::string snippet = nodeText(child, text);
                const std::regex constPattern(R"(\b([A-Z_][A-Z0-9_]*)\b)");
                auto begin = std::sregex_iterator(snippet.begin(), snippet.end(), constPattern);
                auto end = std::sregex_iterator();
                for (auto it = begin; it != end; ++it) {
                    members.push_back(Symbol{"constant", (*it)[1].str(), nodeLine(child), detail, nodeSnippet(child, text)});
                }
            }
        }
        return members;
    };

    std::set<std::string> seen;
    TSNode root = ts_tree_root_node(tree);
    const uint32_t count = ts_node_named_child_count(root);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode child = ts_node_named_child(root, index);
        const std::string type = ts_node_type(child);
        Symbol symbol;
        bool matched = true;
        if (type == "class_declaration" || type == "interface_declaration" || type == "trait_declaration") {
            std::string kind = "class";
            if (type == "interface_declaration") kind = "interface";
            if (type == "trait_declaration") kind = "trait";
            symbol = Symbol{kind, identifierFromNode(fieldNode(child, "name"), text), nodeLine(child), declarationDetailFromNode(child, text), nodeSnippet(child, text), parsePhpMembers(fieldNode(child, "body"))};
        } else if (type == "function_definition") {
            symbol = Symbol{"function", identifierFromNode(fieldNode(child, "name"), text), nodeLine(child), declarationDetailFromNode(child, text), nodeSnippet(child, text)};
        } else if (type == "const_declaration") {
            const std::string detail = declarationDetailFromNode(child, text);
            const std::regex constPattern(R"(\b([A-Z_][A-Z0-9_]*)\b)");
            const std::string snippet = nodeText(child, text);
            auto begin = std::sregex_iterator(snippet.begin(), snippet.end(), constPattern);
            auto end = std::sregex_iterator();
            matched = false;
            for (auto it = begin; it != end; ++it) {
                Symbol constant{"constant", (*it)[1].str(), nodeLine(child), detail, nodeSnippet(child, text)};
                uniquePush(symbols, seen, constant.kind + "|" + constant.name + "|" + std::to_string(constant.line), std::move(constant));
            }
        } else {
            matched = false;
        }
        if (matched && !symbol.name.empty()) {
            uniquePush(symbols, seen, symbol.kind + "|" + symbol.name + "|" + std::to_string(symbol.line), std::move(symbol));
        }
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return symbols;
}

std::vector<Symbol> extractCssSymbolsWithTreeSitter(const std::string &text)
{
    std::vector<Symbol> symbols;
    TSParser *parser = ts_parser_new();
    if (!parser) {
        return symbols;
    }
    const TSLanguage *language = tree_sitter_css();
    if (!language || !ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return symbols;
    }
    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return symbols;
    }

    std::set<std::string> seen;
    std::function<void(TSNode)> visit = [&](TSNode node) {
        const std::string type = ts_node_type(node);
        if (type == "class_selector") {
            std::string name = trim(nodeText(node, text));
            if (!name.empty() && name.front() == '.') {
                name.erase(name.begin());
            }
            Symbol symbol{"class", name, nodeLine(node), "", nodeSnippet(node, text)};
            if (!symbol.name.empty()) {
                uniquePush(symbols, seen, symbol.kind + "|" + symbol.name + "|" + std::to_string(symbol.line), std::move(symbol));
            }
        } else if (type == "property_name") {
            const std::string name = trim(nodeText(node, text));
            if (name.rfind("--", 0) == 0) {
                Symbol symbol{"custom-property", name, nodeLine(node), "", nodeSnippet(node, text)};
                uniquePush(symbols, seen, symbol.kind + "|" + symbol.name + "|" + std::to_string(symbol.line), std::move(symbol));
            }
        }

        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t index = 0; index < count; ++index) {
            visit(ts_node_named_child(node, index));
        }
    };

    visit(ts_tree_root_node(tree));
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return symbols;
}

std::vector<Symbol> extractRustSymbolsWithTreeSitter(const std::string &text)
{
    std::vector<Symbol> symbols;
    TSParser *parser = ts_parser_new();
    if (!parser) {
        return symbols;
    }
    const TSLanguage *language = tree_sitter_rust();
    if (!language || !ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return symbols;
    }
    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return symbols;
    }

    auto parseRustImplMembers = [&](TSNode declList) {
        std::vector<Symbol> members;
        const uint32_t count = ts_node_named_child_count(declList);
        for (uint32_t index = 0; index < count; ++index) {
            TSNode child = ts_node_named_child(declList, index);
            const std::string type = ts_node_type(child);
            if (type == "function_item") {
                const std::string name = identifierFromNode(fieldNode(child, "name"), text);
                if (!name.empty()) {
                    members.push_back(Symbol{"method", name, nodeLine(child), declarationDetailFromNode(child, text), nodeSnippet(child, text)});
                }
            } else if (type == "const_item") {
                const std::string name = identifierFromNode(fieldNode(child, "name"), text);
                if (!name.empty()) {
                    members.push_back(Symbol{"constant", name, nodeLine(child), declarationDetailFromNode(child, text), nodeSnippet(child, text)});
                }
            }
        }
        return members;
    };

    std::set<std::string> seen;
    TSNode root = ts_tree_root_node(tree);
    const uint32_t count = ts_node_named_child_count(root);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode child = ts_node_named_child(root, index);
        const std::string type = ts_node_type(child);
        Symbol symbol;
        bool matched = true;
        if (type == "function_item") {
            symbol = Symbol{"function", identifierFromNode(fieldNode(child, "name"), text), nodeLine(child), declarationDetailFromNode(child, text), nodeSnippet(child, text)};
        } else if (type == "struct_item") {
            symbol = Symbol{"struct", identifierFromNode(fieldNode(child, "name"), text), nodeLine(child), declarationDetailFromNode(child, text), nodeSnippet(child, text)};
        } else if (type == "trait_item") {
            symbol = Symbol{"trait", identifierFromNode(fieldNode(child, "name"), text), nodeLine(child), declarationDetailFromNode(child, text), nodeSnippet(child, text)};
        } else if (type == "enum_item") {
            symbol = Symbol{"enum", identifierFromNode(fieldNode(child, "name"), text), nodeLine(child), declarationDetailFromNode(child, text), nodeSnippet(child, text)};
        } else if (type == "mod_item") {
            symbol = Symbol{"module", identifierFromNode(fieldNode(child, "name"), text), nodeLine(child), declarationDetailFromNode(child, text), nodeSnippet(child, text)};
        } else if (type == "const_item") {
            symbol = Symbol{"constant", identifierFromNode(fieldNode(child, "name"), text), nodeLine(child), declarationDetailFromNode(child, text), nodeSnippet(child, text)};
        } else if (type == "static_item") {
            symbol = Symbol{"static", identifierFromNode(fieldNode(child, "name"), text), nodeLine(child), declarationDetailFromNode(child, text), nodeSnippet(child, text)};
        } else if (type == "impl_item") {
            TSNode typeNode = fieldNode(child, "type");
            symbol = Symbol{"impl", trim(nodeText(typeNode, text)), nodeLine(child), declarationDetailFromNode(child, text), nodeSnippet(child, text), parseRustImplMembers(fieldNode(child, "body"))};
        } else {
            matched = false;
        }
        if (matched && !symbol.name.empty()) {
            uniquePush(symbols, seen, symbol.kind + "|" + symbol.name + "|" + std::to_string(symbol.line), std::move(symbol));
        }
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return symbols;
}

void annotateRustCallTopology(std::vector<Symbol> &symbols, const std::string &text)
{
    if (symbols.empty()) {
        return;
    }

    TSParser *parser = ts_parser_new();
    if (!parser) {
        return;
    }

    const TSLanguage *language = tree_sitter_rust();
    if (!language || !ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return;
    }

    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return;
    }

    std::map<std::string, Symbol *> byKey;
    std::map<std::string, std::vector<std::string>> keysByName;
    collectSymbolPointers(symbols, byKey, keysByName);

    std::function<void(TSNode, const std::string &)> visit = [&](TSNode node, const std::string &currentKey) {
        std::string activeKey = currentKey;
        const std::string nodeKey = rustCallableKeyForNode(node, text, byKey);
        if (!nodeKey.empty()) {
            activeKey = nodeKey;
        }

        if (!activeKey.empty() && std::string(ts_node_type(node)) == "call_expression") {
            const std::string targetName = rustCallTargetName(node, text);
            const auto found = keysByName.find(targetName);
            if (!targetName.empty() && found != keysByName.end() && found->second.size() == 1) {
                const std::string targetKey = found->second.front();
                if (targetKey != activeKey) {
                    Symbol *sourceSymbol = byKey[activeKey];
                    Symbol *targetSymbol = byKey[targetKey];
                    if (sourceSymbol && targetSymbol) {
                        appendRelationUnique(sourceSymbol->calls, *targetSymbol, "calls");
                        appendRelationUnique(targetSymbol->calledBy, *sourceSymbol, "called by");
                    }
                }
            }
        }

        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t index = 0; index < count; ++index) {
            visit(ts_node_named_child(node, index), activeKey);
        }
    };

    visit(ts_tree_root_node(tree), "");
    ts_tree_delete(tree);
    ts_parser_delete(parser);
}

std::vector<std::string> extractHtmlClassesWithTreeSitter(const std::string &text)
{
    std::vector<std::string> values;
    TSParser *parser = ts_parser_new();
    if (!parser) {
        return values;
    }
    const TSLanguage *language = tree_sitter_html();
    if (!language || !ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return values;
    }
    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return values;
    }

    std::set<std::string> classes;
    std::function<void(TSNode)> visit = [&](TSNode node) {
        if (std::string(ts_node_type(node)) == "attribute") {
            TSNode nameNode = fieldNode(node, "name");
            if (lower(trim(nodeText(nameNode, text))) == "class") {
                std::string raw = trim(nodeText(fieldNode(node, "value"), text));
                raw = normalizeValue(raw);
                std::istringstream stream(raw);
                for (std::string part; stream >> part;) {
                    classes.insert(part);
                }
            }
        }
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t index = 0; index < count; ++index) {
            visit(ts_node_named_child(node, index));
        }
    };

    visit(ts_tree_root_node(tree));
    values.assign(classes.begin(), classes.end());
    std::sort(values.begin(), values.end());
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return values;
}

std::vector<CssClassItem> extractCssClassesWithTreeSitter(const std::filesystem::path &path, const std::string &text)
{
    std::vector<CssClassItem> classes;
    TSParser *parser = ts_parser_new();
    if (!parser) {
        return classes;
    }
    const TSLanguage *language = tree_sitter_css();
    if (!language || !ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return classes;
    }
    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return classes;
    }

    std::set<std::string> seen;
    std::function<void(TSNode)> visit = [&](TSNode node) {
        if (std::string(ts_node_type(node)) == "class_selector") {
            std::string name = trim(nodeText(node, text));
            if (!name.empty() && name.front() == '.') {
                name.erase(name.begin());
            }
            if (!name.empty() && seen.insert(name).second) {
                classes.push_back(CssClassItem{name, "class", false, path.string(), nodeLine(node), nodeSnippet(node, text)});
            }
        }
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t index = 0; index < count; ++index) {
            visit(ts_node_named_child(node, index));
        }
    };

    visit(ts_tree_root_node(tree));
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return classes;
}

std::vector<QuickLink> extractHtmlQuickLinksWithTreeSitter(const std::filesystem::path &path, const std::string &text)
{
    std::vector<QuickLink> links;
    TSParser *parser = ts_parser_new();
    if (!parser) {
        return links;
    }
    const TSLanguage *language = tree_sitter_html();
    if (!language || !ts_parser_set_language(parser, language)) {
        ts_parser_delete(parser);
        return links;
    }
    TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return links;
    }

    std::set<std::string> seen;
    auto append = [&](const std::string &type, const std::string &target, TSNode contextNode, const std::string &detail) {
        bool exists = false;
        const std::string resolvedPath = resolveLinkedResourcePath(path, target, exists);
        const std::string label = std::filesystem::path(stripUrlDecorations(target)).filename().string();
        uniquePush(links, seen, type + "|" + target, QuickLink{
            type,
            label.empty() ? target : label,
            resolvedPath.empty() ? target : resolvedPath,
            exists,
            nodeLine(contextNode),
            nodeSnippet(contextNode, text, 2),
            detail
        });
    };

    std::function<void(TSNode)> visit = [&](TSNode node) {
        if (std::string(ts_node_type(node)) == "attribute") {
            const std::string attributeName = lower(trim(nodeText(fieldNode(node, "name"), text)));
            const std::string attributeValue = normalizeValue(trim(nodeText(fieldNode(node, "value"), text)));
            TSNode startTag = firstAncestorOfTypes(node, {"start_tag", "script_element", "style_element"});
            TSNode tagNode = ts_node_is_null(startTag) ? TSNode{} : fieldNode(startTag, "name");
            const std::string tagName = lower(trim(nodeText(tagNode, text)));
            const std::string tagText = lower(nodeText(startTag, text));
            if (attributeName == "src" && tagName == "script" && !attributeValue.empty()) {
                append("script", attributeValue, startTag, "linked script");
            } else if (attributeName == "href" && tagName == "link" && !attributeValue.empty()) {
                const std::string type = tagText.find("stylesheet") != std::string::npos ? "stylesheet" : "link";
                append(type, attributeValue, startTag, "linked resource");
            }
        }
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t index = 0; index < count; ++index) {
            visit(ts_node_named_child(node, index));
        }
    };

    visit(ts_tree_root_node(tree));
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return links;
}

// ============================================================================
// Signature Extraction (Phase 2)
// ============================================================================

// Extract signature info from TypeScript/JavaScript function-like nodes
static SignatureInfo extractTSSignature(TSNode node, const std::string &text) {
    SignatureInfo sig;
    const char *nodeType = ts_node_type(node);

    if (!nodeType || (std::strcmp(nodeType, "function_declaration") != 0 &&
                      std::strcmp(nodeType, "arrow_function") != 0 &&
                      std::strcmp(nodeType, "method_definition") != 0)) {
        return sig;  // Not a function node
    }

    // Check for async keyword
    for (uint32_t i = 0; i < ts_node_child_count(node); ++i) {
        TSNode child = ts_node_child(node, i);
        const char *childType = ts_node_type(child);
        if (childType && std::strcmp(childType, "async") == 0) {
            sig.isAsync = true;
        }
    }

    // Extract parameters from formal_parameters
    for (uint32_t i = 0; i < ts_node_child_count(node); ++i) {
        TSNode child = ts_node_child(node, i);
        const char *childType = ts_node_type(child);
        if (childType && std::strcmp(childType, "formal_parameters") == 0) {
            for (uint32_t j = 0; j < ts_node_child_count(child); ++j) {
                TSNode param = ts_node_child(child, j);
                const char *paramType = ts_node_type(param);

                if (!paramType) continue;

                // Simple parameter extraction (name only for now)
                if (std::strcmp(paramType, "identifier") == 0) {
                    ParameterInfo p;
                    uint32_t start = ts_node_start_byte(param);
                    uint32_t end = ts_node_end_byte(param);
                    if (start < end && static_cast<int>(end) <= static_cast<int>(text.size())) {
                        p.name = text.substr(start, end - start);
                        sig.parameters.push_back(p);
                    }
                } else if (std::strcmp(paramType, "rest_pattern") == 0) {
                    // Handle ...args
                    TSNode identifier = ts_node_named_child(param, 0);
                    if (!ts_node_is_null(identifier)) {
                        ParameterInfo p;
                        uint32_t start = ts_node_start_byte(identifier);
                        uint32_t end = ts_node_end_byte(identifier);
                        if (start < end && static_cast<int>(end) <= static_cast<int>(text.size())) {
                            p.name = text.substr(start, end - start);
                            p.isVariadic = true;
                            sig.parameters.push_back(p);
                        }
                    }
                }
            }
        }
    }

    // Look for return type annotation or Promise type
    // This is simplified - full implementation would walk the return_type node
    const uint32_t nodeStart = ts_node_start_byte(node);
    const uint32_t nodeEnd = ts_node_end_byte(node);
    const size_t nodeLen = (nodeEnd > nodeStart) ? static_cast<size_t>(nodeEnd - nodeStart) : 0;
    const size_t maxLen = (nodeLen > 1000) ? 1000 : nodeLen;
    if (maxLen > 0 && nodeStart < text.size()) {
        std::string funcText = text.substr(nodeStart, std::min(maxLen, text.size() - nodeStart));
        if (funcText.find("Promise") != std::string::npos) {
            sig.returnType = "Promise<unknown>";
        }
    }

    return sig;
}

std::vector<Symbol> extractSymbols(const std::filesystem::path &path, const std::string &text, const std::string &language)
{
    std::vector<Symbol> symbols;
    const auto multiline = std::regex_constants::multiline;

    if (language == "swift") {
        symbols = extractSwiftSymbolsWithTreeSitter(text);
        if (symbols.empty()) {
            appendSymbolsForRegex(text, std::regex(R"(\b(?:struct|class|enum|actor|protocol)\s+([A-Za-z_]\w*))"), "type", symbols);
            appendSymbolsForRegex(text, std::regex(R"(\bfunc\s+([A-Za-z_]\w*)\s*\()"), "function", symbols);
        }
    } else if (language == "script" || language == "jsx" || language == "ts" || language == "tsx") {
        symbols = extractScriptSymbolsWithTreeSitter(text, language);
        if (symbols.empty()) {
            appendSymbolsForRegex(text, std::regex(R"(\b(?:export\s+)?function\s+([A-Za-z_]\w*)\s*\()"), "function", symbols);
            appendSymbolsForRegex(text, std::regex(R"(\b(?:export\s+)?class\s+([A-Za-z_]\w*))"), "class", symbols);
            appendSymbolsForRegex(text, std::regex(R"(\b(?:const|let|var)\s+([A-Za-z_]\w*)\s*=)"), "variable", symbols);
        }
    } else if (language == "python") {
        symbols = extractPythonSymbolsWithTreeSitter(text);
        if (symbols.empty()) {
            appendSymbolsForRegex(text, std::regex(R"(^\s*def\s+([A-Za-z_]\w*)\s*\()", multiline), "function", symbols);
            appendSymbolsForRegex(text, std::regex(R"(^\s*class\s+([A-Za-z_]\w*))", multiline), "class", symbols);
        }
    } else if (language == "rust") {
        symbols = extractRustSymbolsWithTreeSitter(text);
        if (symbols.empty()) {
            appendSymbolsForRegex(text, std::regex(R"(\bfn\s+([A-Za-z_]\w*)\s*\()"), "function", symbols);
            appendSymbolsForRegex(text, std::regex(R"(\b(?:struct|enum|trait)\s+([A-Za-z_]\w*))"), "type", symbols);
        }
    } else if (language == "java") {
        symbols = extractJavaSymbolsWithTreeSitter(text);
        if (symbols.empty()) {
            appendSymbolsForRegex(text, std::regex(R"(\b(?:class|struct|interface|enum)\s+([A-Za-z_]\w*))"), "type", symbols);
        }
    } else if (language == "csharp") {
        symbols = extractCSharpSymbolsWithTreeSitter(text);
        if (symbols.empty()) {
            appendSymbolsForRegex(text, std::regex(R"(\b(?:class|struct|interface|enum)\s+([A-Za-z_]\w*))"), "type", symbols);
        }
    } else if (language == "cpp") {
        std::string scrubbed = maskRegexMatches(text, std::regex(R"(//.*?$)", multiline));
        scrubbed = maskRegexMatches(scrubbed, std::regex(R"(/\*[\s\S]*?\*/)"));
        scrubbed = maskRegexMatches(scrubbed, std::regex(R"("(?:\\.|[^"\\])*")"));
        scrubbed = maskRegexMatches(scrubbed, std::regex(R"('(?:\\.|[^'\\])*')"));
        appendSymbolsForRegex(scrubbed, std::regex(R"(\b(?:class|struct|interface|enum)\s+([A-Za-z_]\w*))"), "type", symbols);
    } else if (language == "php") {
        symbols = extractPhpSymbolsWithTreeSitter(text);
        if (symbols.empty()) {
            appendSymbolsForRegex(text, std::regex(R"(\b(?:class|trait|interface)\s+([A-Za-z_]\w*))"), "type", symbols);
            appendSymbolsForRegex(text, std::regex(R"(\bfunction\s+([A-Za-z_]\w*)\s*\()"), "function", symbols);
        }
    } else if (language == "css") {
        symbols = extractCssSymbolsWithTreeSitter(text);
        if (symbols.empty()) {
            appendSymbolsForRegex(text, std::regex(R"(\.([A-Za-z_-][A-Za-z0-9_-]*)\s*\{)"), "class", symbols);
        }
    } else if (language == "json" && lower(path.filename().string()) == "package.json") {
        const std::regex scriptsPattern("\"scripts\"\\s*:\\s*\\{([\\s\\S]*?)\\}");
        auto begin = std::sregex_iterator(text.begin(), text.end(), scriptsPattern);
        auto end = std::sregex_iterator();
        if (begin != end) {
            const std::size_t bodyOffset = static_cast<std::size_t>((*begin).position(1));
            const std::string body = (*begin)[1].str();
            const std::regex scriptEntryPattern("\"([^\"]+)\"\\s*:\\s*\"([^\"]*)\"");
            auto scriptBegin = std::sregex_iterator(body.begin(), body.end(), scriptEntryPattern);
            auto scriptEnd = std::sregex_iterator();
            for (auto it = scriptBegin; it != scriptEnd; ++it) {
                const int line = lineNumberAtOffset(text, bodyOffset + static_cast<std::size_t>(it->position()));
                symbols.push_back(Symbol{
                    "script",
                    (*it)[1].str(),
                    line,
                    (*it)[2].str(),
                    lineSnippetAt(text, line)
                });
            }
        }
    } else if (language == "json" && lower(path.filename().string()).find("openapi") != std::string::npos) {
        const std::regex pathPattern("\"(/[^\\\"]*)\"\\s*:\\s*\\{([\\s\\S]*?)\\}");
        auto begin = std::sregex_iterator(text.begin(), text.end(), pathPattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const std::string routePath = (*it)[1].str();
            const std::string body = (*it)[2].str();
            const std::size_t bodyOffset = static_cast<std::size_t>((*it).position(2));
            const std::regex opPattern("\"(get|post|put|patch|delete|options|head)\"\\s*:");
            auto opBegin = std::sregex_iterator(body.begin(), body.end(), opPattern);
            auto opEnd = std::sregex_iterator();
            for (auto op = opBegin; op != opEnd; ++op) {
                const int line = lineNumberAtOffset(text, bodyOffset + static_cast<std::size_t>(op->position()));
                symbols.push_back(Symbol{
                    "route",
                    upper(normalizeValue((*op)[1].str())) + " " + routePath,
                    line,
                    "",
                    lineSnippetAt(text, line)
                });
            }
        }
    }

    return symbols;
}

std::vector<Dependency> extractDependencies(const std::filesystem::path &path, const std::string &text, const std::string &language)
{
    std::vector<Dependency> dependencies;
    std::set<std::string> seen;
    const auto multiline = std::regex_constants::multiline;

    auto appendDependency = [&](const std::string &type, const std::string &target, int line, const std::string &detail = "") {
        bool exists = false;
        const std::string resolvedPath = resolveImportPath(path, target, exists);
        const std::string label = !resolvedPath.empty() ? std::filesystem::path(resolvedPath).filename().string() : target;
        uniquePush(dependencies, seen, type + "|" + target, Dependency{
            type,
            label.empty() ? target : label,
            target,
            line,
            lineSnippetAt(text, line),
            detail.empty() ? type + " dependency" : detail,
            resolvedPath,
            exists
        });
    };

    if (language == "swift") {
        const std::regex pattern(R"(^\s*import\s+([A-Za-z_]\w*))", multiline);
        auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            appendDependency("import", (*it)[1].str(), lineNumberAtOffset(text, static_cast<std::size_t>(it->position())));
        }
    } else if (language == "script" || language == "jsx" || language == "ts" || language == "tsx") {
        const std::vector<std::pair<std::string, std::regex>> patterns = {
            {"import", std::regex(R"(\bimport\s+(?:[^;]*?\s+from\s+)?["']([^"']+)["'])")},
            {"require", std::regex(R"(require\s*\(\s*["']([^"']+)["']\s*\))")}
        };
        for (const auto &entry : patterns) {
            auto begin = std::sregex_iterator(text.begin(), text.end(), entry.second);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                appendDependency(entry.first, (*it)[1].str(), lineNumberAtOffset(text, static_cast<std::size_t>(it->position())));
            }
        }
    } else if (language == "python") {
        const std::regex pattern(R"(^\s*(?:from\s+([A-Za-z0-9_\.]+)\s+import|import\s+([A-Za-z0-9_\.]+)))", multiline);
        auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const std::string label = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
            appendDependency("import", label, lineNumberAtOffset(text, static_cast<std::size_t>(it->position())));
        }
    } else if (language == "cpp") {
        const std::regex pattern(R"(^\s*#include\s*[<"]([^>"]+)[>"])", multiline);
        auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            appendDependency("include", (*it)[1].str(), lineNumberAtOffset(text, static_cast<std::size_t>(it->position())), "include");
        }
    } else if (language == "java") {
        const std::regex pattern(R"(^\s*import\s+([A-Za-z0-9_.*]+)\s*;)", multiline);
        auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            appendDependency("import", (*it)[1].str(), lineNumberAtOffset(text, static_cast<std::size_t>(it->position())));
        }
    } else if (language == "csharp") {
        const std::regex pattern(R"(^\s*using\s+([A-Za-z0-9_.]+)\s*;)", multiline);
        auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            appendDependency("using", (*it)[1].str(), lineNumberAtOffset(text, static_cast<std::size_t>(it->position())));
        }
    } else if (language == "rust") {
        const std::regex usePattern(R"(^\s*use\s+([^;]+)\s*;)", multiline);
        auto useBegin = std::sregex_iterator(text.begin(), text.end(), usePattern);
        auto useEnd = std::sregex_iterator();
        for (auto it = useBegin; it != useEnd; ++it) {
            appendDependency("use", trim((*it)[1].str()), lineNumberAtOffset(text, static_cast<std::size_t>(it->position())));
        }
        const std::regex modPattern(R"(^\s*mod\s+([A-Za-z_]\w*)\s*;)", multiline);
        auto modBegin = std::sregex_iterator(text.begin(), text.end(), modPattern);
        auto modEnd = std::sregex_iterator();
        for (auto it = modBegin; it != modEnd; ++it) {
            appendDependency("mod", (*it)[1].str(), lineNumberAtOffset(text, static_cast<std::size_t>(it->position())));
        }
    }

    return dependencies;
}

std::vector<Route> extractRoutes(const std::filesystem::path &path, const std::string &text, const std::string &language)
{
    std::vector<Route> routes;
    const auto multiline = std::regex_constants::multiline;
    const auto icase = std::regex_constants::icase;

    if (language == "script" || language == "jsx" || language == "ts" || language == "tsx") {
        const std::regex pattern(R"(\b(app|router)\.(get|post|put|patch|delete|options|head|use)\s*\(\s*['"]([^'"]+)['"])", icase);
        auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const int line = lineNumberAtOffset(text, static_cast<std::size_t>(it->position()));
            const std::string method = upper((*it)[2].str());
            const std::string routePath = (*it)[3].str();
            routes.push_back(Route{
                method,
                routePath,
                line,
                method + " " + routePath,
                lineSnippetAt(text, line, 1),
                "route"
            });
        }
    } else if (language == "python") {
        const std::regex routePattern(R"(^\s*@(?:\w+\.)?route\s*\(\s*['"]([^'"]+)['"](?:\s*,\s*methods\s*=\s*\[([^\]]*)\])?)", multiline);
        auto routeBegin = std::sregex_iterator(text.begin(), text.end(), routePattern);
        auto routeEnd = std::sregex_iterator();
        for (auto it = routeBegin; it != routeEnd; ++it) {
            const int line = lineNumberAtOffset(text, static_cast<std::size_t>(it->position()));
            std::string method = "GET";
            if ((*it)[2].matched) {
                method = upper(trim((*it)[2].str()));
                method.erase(std::remove(method.begin(), method.end(), '\''), method.end());
                method.erase(std::remove(method.begin(), method.end(), '"'), method.end());
            }
            const std::string routePath = (*it)[1].str();
            routes.push_back(Route{method, routePath, line, method + " " + routePath, lineSnippetAt(text, line, 1), "route"});
        }

        const std::regex fastApiPattern(R"(^\s*@(?:\w+\.)?(get|post|put|patch|delete|options|head)\s*\(\s*['"]([^'"]+)['"])", multiline | icase);
        auto fastApiBegin = std::sregex_iterator(text.begin(), text.end(), fastApiPattern);
        auto fastApiEnd = std::sregex_iterator();
        for (auto it = fastApiBegin; it != fastApiEnd; ++it) {
            const int line = lineNumberAtOffset(text, static_cast<std::size_t>(it->position()));
            const std::string method = upper((*it)[1].str());
            const std::string routePath = (*it)[2].str();
            routes.push_back(Route{method, routePath, line, method + " " + routePath, lineSnippetAt(text, line, 1), "route"});
        }
    } else if (language == "csharp") {
        const std::regex pattern(R"(\bapp\.(MapGet|MapPost|MapPut|MapPatch|MapDelete|MapMethods|MapControllerRoute)\s*\(([\s\S]*?)\))");
        const std::regex quotedPathPattern("\"([^\"]+)\"");
        const std::regex patternFieldPattern("pattern\\s*:\\s*\"([^\"]+)\"");
        auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const int line = lineNumberAtOffset(text, static_cast<std::size_t>(it->position()));
            const std::string routeType = (*it)[1].str();
            const std::string callBody = (*it)[2].str();
            std::string method = upper(routeType.substr(3));
            std::string routePath;
            std::smatch pathMatch;
            if (std::regex_search(callBody, pathMatch, quotedPathPattern)) {
                routePath = pathMatch[1].str();
            }
            if (routeType == "MapControllerRoute") {
                method = "ROUTE";
                std::smatch patternMatch;
                if (std::regex_search(callBody, patternMatch, patternFieldPattern)) {
                    routePath = patternMatch[1].str();
                } else if (routePath.empty()) {
                    routePath = "{controller=Home}/{action=Index}/{id?}";
                }
            }
            routes.push_back(Route{method, routePath, line, method + " " + routePath, lineSnippetAt(text, line, 1), "route"});
        }
    } else if (language == "json" && lower(path.filename().string()).find("openapi") != std::string::npos) {
        const std::regex pathPattern("\"(/[^\\\"]*)\"\\s*:\\s*\\{([\\s\\S]*?)\\}");
        auto begin = std::sregex_iterator(text.begin(), text.end(), pathPattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const std::string routePath = (*it)[1].str();
            const std::string body = (*it)[2].str();
            const std::size_t bodyOffset = static_cast<std::size_t>((*it).position(2));
            const std::regex opPattern("\"(get|post|put|patch|delete|options|head)\"\\s*:");
            auto opBegin = std::sregex_iterator(body.begin(), body.end(), opPattern);
            auto opEnd = std::sregex_iterator();
            for (auto op = opBegin; op != opEnd; ++op) {
                const std::string method = upper((*op)[1].str());
                const int line = lineNumberAtOffset(text, bodyOffset + static_cast<std::size_t>(op->position()));
                routes.push_back(Route{method, routePath, line, method + " " + routePath, lineSnippetAt(text, line), "documented API operation"});
            }
        }
    }

    return routes;
}

std::vector<RelatedFile> extractRelatedFiles(const std::filesystem::path &path)
{
    std::vector<RelatedFile> related;
    std::set<std::string> seen;
    const std::filesystem::path dir = path.parent_path();
    const std::string fileName = path.filename().string();
    const std::string stem = path.stem().string();

    auto appendRelated = [&](const std::filesystem::path &candidate, const std::string &type) {
        const std::string absolute = candidate.lexically_normal().string();
        if (absolute == path.string()) {
            return;
        }
        if (!seen.insert(absolute).second) {
            return;
        }
        related.push_back(RelatedFile{
            type,
            candidate.filename().string(),
            absolute,
            std::filesystem::exists(candidate)
        });
    };

    if (fileName.find(".test.") != std::string::npos || fileName.find(".spec.") != std::string::npos) {
        std::string implName = fileName;
        const auto testPos = implName.find(".test.");
        if (testPos != std::string::npos) implName.erase(testPos, 5);
        const auto specPos = implName.find(".spec.");
        if (specPos != std::string::npos) implName.erase(specPos, 5);
        appendRelated(dir / implName, "implementation");
    } else {
        appendRelated(dir / "tests" / (stem + ".test.js"), "test");
        appendRelated(dir / "tests" / (stem + ".spec.js"), "test");
        appendRelated(dir / "tests" / (stem + ".test.ts"), "test");
        appendRelated(dir / "tests" / (stem + ".spec.ts"), "test");
        appendRelated(dir.parent_path() / "tests" / (stem + ".test.js"), "test");
        appendRelated(dir.parent_path() / "tests" / (stem + ".spec.js"), "test");
        appendRelated(dir / ("test_" + stem + ".py"), "test");
        appendRelated(dir / (stem + "_test.py"), "test");
        appendRelated(dir / "tests" / ("test_" + stem + ".py"), "test");
    }

    const std::string ext = lower(path.extension().string());
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
        appendRelated(dir / (stem + ".h"), "header");
        appendRelated(dir / (stem + ".hpp"), "header");
    }
    if (ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx") {
        appendRelated(dir / (stem + ".cpp"), "implementation");
        appendRelated(dir / (stem + ".cc"), "implementation");
        appendRelated(dir / (stem + ".cxx"), "implementation");
    }
    if (ext == ".cs") {
        appendRelated(dir / (dir.filename().string() + ".csproj"), "project");
    }
    if (ext == ".m" || ext == ".mm") {
        appendRelated(dir / (stem + ".h"), "header");
    }
    if (ext == ".rs") {
        appendRelated(dir / "mod.rs", "module");
        appendRelated(dir / "lib.rs", "crate");
        appendRelated(dir / "main.rs", "entrypoint");
    }
    if (ext == ".js" || ext == ".jsx" || ext == ".ts" || ext == ".tsx") {
        appendRelated(dir / (stem + ".css"), "stylesheet");
        appendRelated(dir / (stem + ".module.css"), "stylesheet");
        appendRelated(dir / (stem + ".scss"), "stylesheet");
        appendRelated(dir / (stem + ".module.scss"), "stylesheet");
        appendRelated(dir / (stem + ".sass"), "stylesheet");
        appendRelated(dir / (stem + ".less"), "stylesheet");
        appendRelated(dir / (stem + ".html"), "markup");
        appendRelated(dir / (stem + ".svg"), "asset");
        appendRelated(dir / (stem + ".png"), "asset");
        appendRelated(dir / (stem + ".jpg"), "asset");
    }
    if (ext == ".css" || ext == ".scss" || ext == ".sass" || ext == ".less") {
        appendRelated(dir / (stem + ".tsx"), "component");
        appendRelated(dir / (stem + ".jsx"), "component");
        appendRelated(dir / (stem + ".ts"), "script");
        appendRelated(dir / (stem + ".js"), "script");
        std::string baseStem = stem;
        const std::string moduleSuffix = ".module";
        if (baseStem.size() > moduleSuffix.size()
            && baseStem.rfind(moduleSuffix) == baseStem.size() - moduleSuffix.size()) {
            baseStem = baseStem.substr(0, baseStem.size() - moduleSuffix.size());
            appendRelated(dir / (baseStem + ".tsx"), "component");
            appendRelated(dir / (baseStem + ".jsx"), "component");
            appendRelated(dir / (baseStem + ".ts"), "script");
            appendRelated(dir / (baseStem + ".js"), "script");
        }
    }
    if (fileName == "package.json") {
        appendRelated(dir / "index.js", "entrypoint");
        appendRelated(dir / "index.ts", "entrypoint");
        appendRelated(dir / "src" / "main.ts", "entrypoint");
        appendRelated(dir / "src" / "main.tsx", "entrypoint");
    }
    if (fileName == "index.js" || fileName == "index.ts") {
        appendRelated(dir / "package.json", "package");
        appendRelated(dir / "public_html" / "index.html", "frontend");
    }

    std::vector<RelatedFile> existing;
    for (const auto &item : related) {
        if (item.exists) {
            existing.push_back(item);
        }
    }
    return existing;
}

std::vector<std::string> extractHtmlClasses(const std::string &text)
{
    auto astValues = extractHtmlClassesWithTreeSitter(text);
    if (!astValues.empty()) {
        return astValues;
    }

    std::set<std::string> classes;
    std::string cleaned = text;
    const std::regex scriptPattern(R"(<script\b[^>]*>[\s\S]*?</script>)", std::regex_constants::icase);
    const std::regex stylePattern(R"(<style\b[^>]*>[\s\S]*?</style>)", std::regex_constants::icase);
    cleaned = stripRegexMatches(cleaned, scriptPattern);
    cleaned = stripRegexMatches(cleaned, stylePattern);
    const std::regex pattern(R"(class\s*=\s*["']([^"']+)["'])", std::regex_constants::icase);
    auto begin = std::sregex_iterator(cleaned.begin(), cleaned.end(), pattern);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::istringstream stream((*it)[1].str());
        for (std::string part; stream >> part;) {
            classes.insert(part);
        }
    }
    std::vector<std::string> values(classes.begin(), classes.end());
    std::sort(values.begin(), values.end());
    return values;
}

void appendClassTokens(std::set<std::string> &classes, const std::string &value)
{
    std::istringstream stream(value);
    for (std::string part; stream >> part;) {
        part = trim(part);
        if (part.empty()) {
            continue;
        }
        while (!part.empty() && (part.front() == '.' || part.front() == '#')) {
            part.erase(part.begin());
        }
        while (!part.empty() && !std::isalnum(static_cast<unsigned char>(part.back())) && part.back() != '_' && part.back() != '-') {
            part.pop_back();
        }
        if (!part.empty()) {
            classes.insert(part);
        }
    }
}

std::vector<std::string> extractScriptClassReferences(const std::string &text)
{
    std::set<std::string> classes;
    const std::vector<std::regex> patterns = {
        std::regex(R"(\bclass(?:Name)?\s*=\s*["']([^"']+)["'])"),
        std::regex(R"(\bclass(?:Name)?\s*=\s*\{\s*["']([^"']+)["']\s*\})"),
        std::regex(R"(\bclass(?:Name)?\s*=\s*\{\s*`([^`]+)`\s*\})")
    };

    for (const auto &pattern : patterns) {
        auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            appendClassTokens(classes, (*it)[1].str());
        }
    }

    std::vector<std::string> values(classes.begin(), classes.end());
    std::sort(values.begin(), values.end());
    return values;
}

std::vector<CssClassItem> extractCssClassesWithContext(const std::filesystem::path &path, const std::string &text)
{
    auto astClasses = extractCssClassesWithTreeSitter(path, text);
    if (!astClasses.empty()) {
        return astClasses;
    }

    std::vector<CssClassItem> classes;
    std::set<std::string> seen;
    const std::regex pattern(R"(\.([A-Za-z_-][A-Za-z0-9_-]*))");
    auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string name = (*it)[1].str();
        if (!seen.insert(name).second) {
            continue;
        }
        const int line = lineNumberAtOffset(text, static_cast<std::size_t>(it->position()));
        classes.push_back(CssClassItem{name, "class", false, path.string(), line, lineSnippetAt(text, line)});
    }
    return classes;
}

std::vector<QuickLink> extractQuickLinks(const std::filesystem::path &path, const std::string &text, const std::string &language)
{
    std::vector<QuickLink> links;
    std::set<std::string> seen;

    auto append = [&](const std::string &type, const std::string &label, const std::string &target, int line, const std::string &detail) {
        bool exists = false;
        const std::string resolvedPath = language == "html"
            ? resolveLinkedResourcePath(path, target, exists)
            : resolveImportPath(path, target, exists);
        uniquePush(links, seen, type + "|" + target, QuickLink{
            type,
            label,
            resolvedPath.empty() ? target : resolvedPath,
            exists,
            line,
            lineSnippetAt(text, line),
            detail
        });
    };

    if (language == "html") {
        links = extractHtmlQuickLinksWithTreeSitter(path, text);
        if (links.empty()) {
            const std::regex scriptPattern(R"(<script[^>]*src\s*=\s*["']([^"']+)["'])", std::regex_constants::icase);
            auto scriptBegin = std::sregex_iterator(text.begin(), text.end(), scriptPattern);
            auto scriptEnd = std::sregex_iterator();
            for (auto it = scriptBegin; it != scriptEnd; ++it) {
                append("script", (*it)[1].str(), (*it)[1].str(), lineNumberAtOffset(text, static_cast<std::size_t>(it->position())), "linked script");
            }
            const std::regex stylePattern(R"(<link[^>]*href\s*=\s*["']([^"']+)["'][^>]*>)", std::regex_constants::icase);
            auto styleBegin = std::sregex_iterator(text.begin(), text.end(), stylePattern);
            auto styleEnd = std::sregex_iterator();
            for (auto it = styleBegin; it != styleEnd; ++it) {
                const std::string tag = it->str();
                const std::string href = (*it)[1].str();
                const std::string type = lower(tag).find("stylesheet") != std::string::npos ? "stylesheet" : "link";
                append(type, href, href, lineNumberAtOffset(text, static_cast<std::size_t>(it->position())), "linked resource");
            }
        }
    } else if (language == "script" || language == "jsx" || language == "ts" || language == "tsx") {
        auto appendSelectorLink = [&](const std::string &type, const std::string &value, int line, const std::string &detail) {
            const std::string normalized = trim(value);
            if (normalized.empty()) {
                return;
            }
            uniquePush(links, seen, type + "|" + normalized + "|" + std::to_string(line), QuickLink{
                type,
                normalized,
                normalized,
                true,
                line,
                lineSnippetAt(text, line),
                detail
            });
        };

        const std::vector<std::pair<std::string, std::regex>> selectorPatterns = {
            {"css_class", std::regex(R"(querySelector(?:All)?\s*\(\s*['"]\.([^'"]+)['"]\s*\))")},
            {"html_id", std::regex(R"(querySelector(?:All)?\s*\(\s*['"]#([^'"]+)['"]\s*\))")},
            {"html_id", std::regex(R"(getElementById\s*\(\s*['"]([^'"]+)['"]\s*\))")},
            {"css_class", std::regex(R"(getElementsByClassName\s*\(\s*['"]([^'"]+)['"]\s*\))")},
            {"css_class", std::regex(R"(classList\.(?:add|remove|toggle|contains)\s*\(\s*['"]([^'"]+)['"]\s*\))")}
        };

        for (const auto &entry : selectorPatterns) {
            auto begin = std::sregex_iterator(text.begin(), text.end(), entry.second);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                const int line = lineNumberAtOffset(text, static_cast<std::size_t>(it->position()));
                const std::string prefix = entry.first == "html_id" ? "#" : ".";
                appendSelectorLink(entry.first, prefix + (*it)[1].str(), line, "selector reference");
            }
        }

        if (language == "jsx" || language == "tsx") {
            const std::vector<std::pair<std::string, std::regex>> classPatterns = {
                {"class reference", std::regex(R"(\bclass(?:Name)?\s*=\s*["']([^"']+)["'])")},
                {"class reference", std::regex(R"(\bclass(?:Name)?\s*=\s*\{\s*["']([^"']+)["']\s*\})")},
                {"class template", std::regex(R"(\bclass(?:Name)?\s*=\s*\{\s*`([^`]+)`\s*\})")}
            };

            for (const auto &entry : classPatterns) {
                auto begin = std::sregex_iterator(text.begin(), text.end(), entry.second);
                auto end = std::sregex_iterator();
                for (auto it = begin; it != end; ++it) {
                    const int line = lineNumberAtOffset(text, static_cast<std::size_t>(it->position()));
                    std::set<std::string> classes;
                    appendClassTokens(classes, (*it)[1].str());
                    for (const auto &name : classes) {
                        appendSelectorLink("css_class", "." + name, line, entry.first);
                    }
                }
            }
        }
    }

    return links;
}

PackageSummary extractPackageSummary(const std::filesystem::path &path, const std::string &text)
{
    PackageSummary summary;
    if (lower(path.filename().string()) != "package.json") {
        return summary;
    }

    auto findStringField = [&](const std::string &field) {
        std::regex pattern("\"" + field + "\"\\s*:\\s*\"([^\"]*)\"");
        std::smatch match;
        if (std::regex_search(text, match, pattern)) {
            return match[1].str();
        }
        return std::string();
    };

    summary.name = findStringField("name");
    summary.version = findStringField("version");
    summary.main = findStringField("main");

    auto extractObjectBody = [&](const std::string &field) {
        std::regex pattern("\"" + field + "\"\\s*:\\s*\\{([\\s\\S]*?)\\}");
        std::smatch match;
        if (std::regex_search(text, match, pattern)) {
            return match[1].str();
        }
        return std::string();
    };

    const std::string scriptsBody = extractObjectBody("scripts");
    const std::regex objectEntryPattern("\"([^\"]+)\"\\s*:\\s*\"([^\"]*)\"");
    for (auto it = std::sregex_iterator(scriptsBody.begin(), scriptsBody.end(), objectEntryPattern);
         it != std::sregex_iterator(); ++it) {
        summary.scripts[(*it)[1].str()] = (*it)[2].str();
    }

    for (const std::string section : {"dependencies", "devDependencies"}) {
        const std::string body = extractObjectBody(section);
        for (auto it = std::sregex_iterator(body.begin(), body.end(), objectEntryPattern);
             it != std::sregex_iterator(); ++it) {
            summary.dependencies.push_back((*it)[1].str());
        }
    }

    std::sort(summary.dependencies.begin(), summary.dependencies.end());
    summary.dependencies.erase(std::unique(summary.dependencies.begin(), summary.dependencies.end()), summary.dependencies.end());

    return summary;
}

CssSummary extractCssSummary(const std::filesystem::path &path, const std::string &text, const std::string &language)
{
    CssSummary summary;

    if (language == "css") {
        const auto available = extractCssClassesWithContext(path, text);
        for (const auto &item : available) {
            summary.availableClasses.push_back(item.name);
        }
        std::sort(summary.availableClasses.begin(), summary.availableClasses.end());
        return summary;
    }

    const bool markupLanguage = language == "html";
    const bool componentLanguage = language == "jsx" || language == "tsx";
    const bool scriptLanguage = language == "script" || language == "ts";

    if (!markupLanguage && !componentLanguage && !scriptLanguage) {
        return summary;
    }

    if (markupLanguage) {
        summary.usedClasses = extractHtmlClasses(text);
    } else if (componentLanguage) {
        summary.usedClasses = extractScriptClassReferences(text);
    }

    std::set<std::string> availableNames;
    std::map<std::string, CssClassItem> availableItems;
    const auto quickLinks = extractQuickLinks(path, text, language);

    if (!markupLanguage) {
        for (const auto &link : quickLinks) {
            if (link.type == "css_class") {
                std::string name = trim(link.label);
                if (!name.empty() && name.front() == '.') {
                    name.erase(name.begin());
                }
                if (!name.empty()) {
                    summary.usedClasses.push_back(name);
                }
            }
        }
    }

    std::sort(summary.usedClasses.begin(), summary.usedClasses.end());
    summary.usedClasses.erase(std::unique(summary.usedClasses.begin(), summary.usedClasses.end()), summary.usedClasses.end());

    for (const auto &link : quickLinks) {
        if (link.type != "stylesheet" || !link.exists) {
            continue;
        }
        const std::filesystem::path cssPath(link.target);
        const std::string cssText = readFile(cssPath);
        for (const auto &item : extractCssClassesWithContext(cssPath, cssText)) {
            availableNames.insert(item.name);
            availableItems[item.name] = item;
        }
    }

    const auto dependencies = extractDependencies(path, text, language);
    for (const auto &dependency : dependencies) {
        if (!dependency.exists || dependency.path.empty()) {
            continue;
        }
        const std::string ext = lower(std::filesystem::path(dependency.path).extension().string());
        if (ext != ".css" && ext != ".scss" && ext != ".sass" && ext != ".less") {
            continue;
        }
        const std::filesystem::path cssPath(dependency.path);
        const std::string cssText = readFile(cssPath);
        for (const auto &item : extractCssClassesWithContext(cssPath, cssText)) {
            availableNames.insert(item.name);
            availableItems[item.name] = item;
        }
    }

    const auto relatedFiles = extractRelatedFiles(path);
    for (const auto &related : relatedFiles) {
        if (!related.exists || related.type != "stylesheet") {
            continue;
        }
        const std::filesystem::path cssPath(related.path);
        const std::string cssText = readFile(cssPath);
        for (const auto &item : extractCssClassesWithContext(cssPath, cssText)) {
            availableNames.insert(item.name);
            availableItems[item.name] = item;
        }
    }

    for (const auto &name : availableNames) {
        summary.availableClasses.push_back(name);
    }

    for (const auto &name : summary.usedClasses) {
        const auto found = availableItems.find(name);
        if (found != availableItems.end()) {
            CssClassItem item = found->second;
            item.matched = true;
            summary.matchedClasses.push_back(item);
        } else {
            summary.missingClasses.push_back(CssClassItem{name, "class", false, "", 0, "." + name + " { ... }"});
        }
    }

    return summary;
}

std::string summarize(const AnalysisResult &result)
{
    if (!result.packageSummary.name.empty() || !result.packageSummary.dependencies.empty() || !result.packageSummary.scripts.empty()) {
        std::ostringstream summary;
        summary << result.packageSummary.scripts.size() << " scripts, "
                << result.packageSummary.dependencies.size() << " dependencies";
        return summary.str();
    }

    if (!result.routes.empty()) {
        std::ostringstream summary;
        summary << result.symbols.size() << " symbols, " << result.routes.size() << " routes";
        return summary.str();
    }

    if (!result.quickLinks.empty()) {
        std::ostringstream summary;
        summary << result.quickLinks.size() << " links, " << result.cssSummary.usedClasses.size() << " used CSS classes";
        return summary.str();
    }

    std::ostringstream summary;
    summary << "Native bridge analyzed " << result.fileName;
    if (!result.language.empty()) {
        summary << " as " << result.language;
    }
    summary << ", found " << result.symbols.size() << " symbols and " << result.dependencies.size() << " dependencies.";
    return summary.str();
}

AnalysisResult analyzeFile(const std::filesystem::path &path, const std::string &source, const std::string &language)
{
    AnalysisResult result;
    result.path = path.string();
    result.fileName = path.filename().string();
    result.language = language;

    // Attempt to repair broken AST before analysis
    std::string analysisSource = source;
    recovery::RepairResult repairResult = recovery::repairAST(source, language);
    if (repairResult.clean || !repairResult.blankedLines.empty()) {
        analysisSource = repairResult.source;
        result.analysisDamagedLines = repairResult.blankedLines;
        if (repairResult.clean) {
            result.analysisSourceMode = "recovered";
            result.analysisConfidence = 0.9;
        } else {
            result.analysisSourceMode = "heuristic";
            result.analysisConfidence = 0.7;
        }
    } else {
        // Parse succeeded with original source
        result.analysisSourceMode = "ast";
        result.analysisConfidence = 1.0;
    }

    result.symbols = extractSymbols(path, analysisSource, language);
    if (language == "script" || language == "jsx" || language == "ts" || language == "tsx") {
        annotateScriptCallTopology(result.symbols, analysisSource, language);
    } else if (language == "python") {
        annotatePythonCallTopology(result.symbols, analysisSource);
    } else if (language == "rust") {
        annotateRustCallTopology(result.symbols, analysisSource);
    }
    result.quickLinks = extractQuickLinks(path, analysisSource, language);
    result.dependencies = extractDependencies(path, analysisSource, language);
    result.routes = extractRoutes(path, analysisSource, language);
    result.relatedFiles = extractRelatedFiles(path);
    result.packageSummary = extractPackageSummary(path, analysisSource);
    result.cssSummary = extractCssSummary(path, analysisSource, language);
    result.summary = summarize(result);

    // Extract index facts for project index (Phase 3)
    // Collect definitions and call sites from symbols
    std::function<void(const std::vector<Symbol>&, const std::string&)> collectSymbolFacts =
        [&](const std::vector<Symbol> &symbols, const std::string &owner) {
            for (const auto &sym : symbols) {
                if (sym.kind == "function" || sym.kind == "method" ||
                    sym.kind == "class" || sym.kind == "type" || sym.kind == "interface") {
                    IndexFact def;
                    def.kind = "definition";
                    def.name = sym.name;
                    def.owner = owner.empty() ? "" : owner;
                    def.file = result.path;
                    def.line = sym.line;
                    result.indexFacts.definitions.push_back(def);
                }

                // Collect call sites from this symbol
                for (const auto &call : sym.calls) {
                    IndexFact callFact;
                    callFact.kind = "call";
                    callFact.name = call.name;
                    callFact.receiver = call.detail;  // receiver type/name if available
                    callFact.file = result.path;
                    callFact.line = call.line;
                    result.indexFacts.callSites.push_back(callFact);
                }

                // Recurse into members, passing current symbol as owner
                collectSymbolFacts(sym.members, sym.name);
            }
        };
    collectSymbolFacts(result.symbols, "");

    // Collect imports from dependencies
    for (const auto &dep : result.dependencies) {
        if (dep.type == "import") {
            IndexFact fact;
            fact.kind = "import";
            fact.name = dep.target;
            fact.binding = dep.label;
            fact.file = result.path;
            fact.line = dep.line;
            result.indexFacts.imports.push_back(fact);
        }
    }

    return result;
}

static void appendSignatureJson(std::ostringstream &out, const SignatureInfo &sig) {
    out << "\"signature\":{"
        << "\"parameters\":[";
    for (std::size_t i = 0; i < sig.parameters.size(); ++i) {
        const auto &param = sig.parameters[i];
        if (i > 0) out << ",";
        out << "{"
            << "\"name\":" << quote(param.name) << ","
            << "\"type\":" << (param.type.empty() ? "null" : quote(param.type)) << ","
            << "\"defaultValue\":" << (param.defaultValue.empty() ? "null" : quote(param.defaultValue)) << ","
            << "\"isVariadic\":" << (param.isVariadic ? "true" : "false") << ","
            << "\"isOptional\":" << (param.isOptional ? "true" : "false")
            << "}";
    }
    out << "],"
        << "\"returnType\":" << (sig.returnType.empty() ? "null" : quote(sig.returnType)) << ","
        << "\"isAsync\":" << (sig.isAsync ? "true" : "false") << ","
        << "\"isThrows\":" << (sig.isThrows ? "true" : "false") << ","
        << "\"typeParameters\":[";
    for (std::size_t i = 0; i < sig.typeParameters.size(); ++i) {
        if (i > 0) out << ",";
        out << quote(sig.typeParameters[i]);
    }
    out << "]}";
}

void appendSymbolJson(std::ostringstream &out, const Symbol &symbol, const std::string &path, const std::string &language, std::size_t index)
{
    out << "{"
        << "\"id\":" << quote(makeId("00000000-0000-0000-0000-", index)) << ","
        << "\"kind\":" << quote(symbol.kind) << ","
        << "\"name\":" << quote(symbol.name) << ","
        << "\"line\":" << symbol.line << ","
        << "\"detail\":" << quote(symbol.detail) << ","
        << "\"members\":[";
    for (std::size_t memberIndex = 0; memberIndex < symbol.members.size(); ++memberIndex) {
        if (memberIndex > 0) out << ",";
        appendSymbolJson(out, symbol.members[memberIndex], path, language, index * 1000 + memberIndex + 1);
    }
    out << "],"
        << "\"calls\":[";
    for (std::size_t relationIndex = 0; relationIndex < symbol.calls.size(); ++relationIndex) {
        const auto &relation = symbol.calls[relationIndex];
        if (relationIndex > 0) out << ",";
        out << "{"
            << "\"id\":" << quote(makeId("00000000-0000-0000-0007-", index * 1000 + relationIndex)) << ","
            << "\"kind\":" << quote(relation.kind) << ","
            << "\"name\":" << quote(relation.name) << ","
            << "\"line\":" << relation.line << ","
            << "\"detail\":" << quote(relation.detail) << ","
            << "\"snippet\":" << quote(relation.snippet) << ","
            << "\"path\":" << quote(path) << ","
            << "\"language\":" << quote(language) << ","
            << "\"snippetKind\":\"line_excerpt\","
            << "\"diagnosticsMode\":\"none\""
            << "}";
    }
    out << "],"
        << "\"calledBy\":[";
    for (std::size_t relationIndex = 0; relationIndex < symbol.calledBy.size(); ++relationIndex) {
        const auto &relation = symbol.calledBy[relationIndex];
        if (relationIndex > 0) out << ",";
        out << "{"
            << "\"id\":" << quote(makeId("00000000-0000-0000-0008-", index * 1000 + relationIndex)) << ","
            << "\"kind\":" << quote(relation.kind) << ","
            << "\"name\":" << quote(relation.name) << ","
            << "\"line\":" << relation.line << ","
            << "\"detail\":" << quote(relation.detail) << ","
            << "\"snippet\":" << quote(relation.snippet) << ","
            << "\"path\":" << quote(path) << ","
            << "\"language\":" << quote(language) << ","
            << "\"snippetKind\":\"line_excerpt\","
            << "\"diagnosticsMode\":\"none\""
            << "}";
    }
    out << "],"
        << "\"snippet\":" << quote(symbol.snippet) << ","
        << "\"path\":" << quote(path) << ","
        << "\"language\":" << quote(language) << ","
        << "\"sourcePath\":" << quote(path) << ","
        << "\"sourceLanguage\":" << quote(language) << ","
        << "\"snippetKind\":\"line_excerpt\","
        << "\"diagnosticsMode\":\"none\","
        << "\"skipDiagnostics\":true,";

    // Add signature if available
    appendSignatureJson(out, symbol.signature);

    out << "}";
}

std::string buildJson(const AnalysisResult &result)
{
    std::ostringstream out;
    out << "{";
    out << "\"path\":" << quote(result.path) << ",";
    out << "\"fileName\":" << quote(result.fileName) << ",";
    out << "\"language\":" << quote(result.language) << ",";

    out << "\"symbols\":[";
    for (std::size_t index = 0; index < result.symbols.size(); ++index) {
        if (index > 0) out << ",";
        appendSymbolJson(out, result.symbols[index], result.path, result.language, index);
    }
    out << "],";

    out << "\"quickLinks\":[";
    for (std::size_t index = 0; index < result.quickLinks.size(); ++index) {
        const auto &link = result.quickLinks[index];
        if (index > 0) out << ",";
        out << "{"
            << "\"id\":" << quote(makeId("00000000-0000-0000-0002-", index)) << ","
            << "\"type\":" << quote(link.type) << ","
            << "\"label\":" << quote(link.label) << ","
            << "\"target\":" << quote(link.target) << ","
            << "\"exists\":" << (link.exists ? "true" : "false") << ","
            << "\"line\":" << link.line << ","
            << "\"snippet\":" << quote(link.snippet) << ","
            << "\"detail\":" << quote(link.detail)
            << "}";
    }
    out << "],";

    out << "\"dependencies\":[";
    for (std::size_t index = 0; index < result.dependencies.size(); ++index) {
        const auto &dependency = result.dependencies[index];
        if (index > 0) out << ",";
        out << "{"
            << "\"id\":" << quote(makeId("00000000-0000-0000-0001-", index)) << ","
            << "\"type\":" << quote(dependency.type) << ","
            << "\"label\":" << quote(dependency.label) << ","
            << "\"target\":" << quote(dependency.target) << ","
            << "\"line\":" << dependency.line << ","
            << "\"snippet\":" << quote(dependency.snippet) << ","
            << "\"detail\":" << quote(dependency.detail) << ","
            << "\"path\":" << (dependency.path.empty() ? "null" : quote(dependency.path)) << ","
            << "\"exists\":" << (dependency.exists ? "true" : "false") << ","
            << "\"snippetKind\":\"line_excerpt\","
            << "\"diagnosticsMode\":\"none\""
            << "}";
    }
    out << "],";

    out << "\"routes\":[";
    for (std::size_t index = 0; index < result.routes.size(); ++index) {
        const auto &route = result.routes[index];
        if (index > 0) out << ",";
        out << "{"
            << "\"id\":" << quote(makeId("00000000-0000-0000-0003-", index)) << ","
            << "\"method\":" << quote(route.method) << ","
            << "\"path\":" << quote(route.path) << ","
            << "\"line\":" << route.line << ","
            << "\"label\":" << quote(route.label) << ","
            << "\"snippet\":" << quote(route.snippet) << ","
            << "\"detail\":" << quote(route.detail) << ","
            << "\"snippetKind\":\"line_excerpt\","
            << "\"diagnosticsMode\":\"none\""
            << "}";
    }
    out << "],";

    out << "\"relatedFiles\":[";
    for (std::size_t index = 0; index < result.relatedFiles.size(); ++index) {
        const auto &related = result.relatedFiles[index];
        if (index > 0) out << ",";
        out << "{"
            << "\"id\":" << quote(makeId("00000000-0000-0000-0004-", index)) << ","
            << "\"type\":" << quote(related.type) << ","
            << "\"label\":" << quote(related.label) << ","
            << "\"path\":" << quote(related.path) << ","
            << "\"exists\":" << (related.exists ? "true" : "false")
            << "}";
    }
    out << "],";

    out << "\"packageSummary\":{"
        << "\"name\":" << quote(result.packageSummary.name) << ","
        << "\"version\":" << quote(result.packageSummary.version) << ","
        << "\"main\":" << quote(result.packageSummary.main) << ","
        << "\"dependencies\":[";
    for (std::size_t index = 0; index < result.packageSummary.dependencies.size(); ++index) {
        if (index > 0) out << ",";
        out << quote(result.packageSummary.dependencies[index]);
    }
    out << "],"
        << "\"scripts\":{";
    bool firstScript = true;
    for (const auto &entry : result.packageSummary.scripts) {
        if (!firstScript) out << ",";
        firstScript = false;
        out << quote(entry.first) << ":" << quote(entry.second);
    }
    out << "}},";

    out << "\"cssSummary\":{"
        << "\"usedClasses\":[";
    for (std::size_t index = 0; index < result.cssSummary.usedClasses.size(); ++index) {
        if (index > 0) out << ",";
        out << quote(result.cssSummary.usedClasses[index]);
    }
    out << "],"
        << "\"availableClasses\":[";
    for (std::size_t index = 0; index < result.cssSummary.availableClasses.size(); ++index) {
        if (index > 0) out << ",";
        out << quote(result.cssSummary.availableClasses[index]);
    }
    out << "],"
        << "\"matchedClasses\":[";
    for (std::size_t index = 0; index < result.cssSummary.matchedClasses.size(); ++index) {
        const auto &item = result.cssSummary.matchedClasses[index];
        if (index > 0) out << ",";
        out << "{"
            << "\"id\":" << quote(makeId("00000000-0000-0000-0005-", index)) << ","
            << "\"name\":" << quote(item.name) << ","
            << "\"kind\":" << quote(item.kind) << ","
            << "\"matched\":true,"
            << "\"path\":" << quote(item.path) << ","
            << "\"line\":" << item.line << ","
            << "\"snippet\":" << quote(item.snippet)
            << "}";
    }
    out << "],"
        << "\"missingClasses\":[";
    for (std::size_t index = 0; index < result.cssSummary.missingClasses.size(); ++index) {
        const auto &item = result.cssSummary.missingClasses[index];
        if (index > 0) out << ",";
        out << "{"
            << "\"id\":" << quote(makeId("00000000-0000-0000-0006-", index)) << ","
            << "\"name\":" << quote(item.name) << ","
            << "\"kind\":" << quote(item.kind) << ","
            << "\"matched\":false,"
            << "\"path\":" << quote(item.path) << ","
            << "\"line\":" << item.line << ","
            << "\"snippet\":" << quote(item.snippet)
            << "}";
    }
    out << "]},";

    out << "\"summary\":" << quote(result.summary) << ",";

    // Add recovery metadata
    out << "\"analysisSourceMode\":" << quote(result.analysisSourceMode) << ",";
    out << "\"analysisConfidence\":" << result.analysisConfidence << ",";
    out << "\"analysisDamagedLines\":[";
    for (std::size_t index = 0; index < result.analysisDamagedLines.size(); ++index) {
        if (index > 0) out << ",";
        out << result.analysisDamagedLines[index];
    }
    out << "],";

    // Add index facts (Phase 3)
    out << "\"indexFacts\":{";
    out << "\"definitions\":[";
    for (std::size_t i = 0; i < result.indexFacts.definitions.size(); ++i) {
        const auto &fact = result.indexFacts.definitions[i];
        if (i > 0) out << ",";
        out << "{"
            << "\"kind\":" << quote(fact.kind) << ","
            << "\"name\":" << quote(fact.name) << ","
            << "\"owner\":" << (fact.owner.empty() ? "null" : quote(fact.owner)) << ","
            << "\"file\":" << quote(fact.file) << ","
            << "\"line\":" << fact.line
            << "}";
    }
    out << "],";
    out << "\"imports\":[";
    for (std::size_t i = 0; i < result.indexFacts.imports.size(); ++i) {
        const auto &fact = result.indexFacts.imports[i];
        if (i > 0) out << ",";
        out << "{"
            << "\"kind\":" << quote(fact.kind) << ","
            << "\"name\":" << quote(fact.name) << ","
            << "\"binding\":" << (fact.binding.empty() ? "null" : quote(fact.binding)) << ","
            << "\"file\":" << quote(fact.file) << ","
            << "\"line\":" << fact.line
            << "}";
    }
    out << "],";
    out << "\"callSites\":[]},"
        << "\"callEdges\":[]";

    out << "}";
    return out.str();
}

std::string makeErrorJson(const std::filesystem::path &path, const std::string &message)
{
    AnalysisResult result;
    result.path = path.string();
    result.fileName = path.filename().string();
    result.summary = message;
    return buildJson(result);
}

} // namespace

extern "C" char *lumencode_analyze_file_json(const char *pathCString)
{
    const std::filesystem::path path(pathCString == nullptr ? "" : pathCString);
    if (path.empty()) {
        const std::string json = makeErrorJson(path, "No path provided.");
        return ::strdup(json.c_str());
    }

    const std::string source = readFile(path);
    if (source.empty() && !std::filesystem::exists(path)) {
        const std::string json = makeErrorJson(path, "File not found.");
        return ::strdup(json.c_str());
    }

    const std::string language = detectLanguage(path);
    const AnalysisResult result = analyzeFile(path, source, language);
    const std::string json = buildJson(result);
    return ::strdup(json.c_str());
}

extern "C" void lumencode_free_string(char *value)
{
    std::free(value);
}
