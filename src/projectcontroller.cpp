#include "projectcontroller.h"

#include "filesystemmodel.h"
#include "symbolparser.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <cstring>
#include <functional>
#include <tree_sitter/api.h>

extern "C" {
const TSLanguage *tree_sitter_javascript(void);
const TSLanguage *tree_sitter_typescript(void);
const TSLanguage *tree_sitter_tsx(void);
const TSLanguage *tree_sitter_php(void);
const TSLanguage *tree_sitter_css(void);
const TSLanguage *tree_sitter_rust(void);
const TSLanguage *tree_sitter_python(void);
const TSLanguage *tree_sitter_java(void);
const TSLanguage *tree_sitter_c_sharp(void);
}

namespace {

constexpr qint64 kMaxPreviewBytes = 256 * 1024;
const QString kSnippetKindFilePreview = QStringLiteral("file_preview");
const QString kSnippetKindExactConstruct = QStringLiteral("exact_construct");
const QString kSnippetKindBlockExcerpt = QStringLiteral("block_excerpt");
const QString kSnippetKindLineExcerpt = QStringLiteral("line_excerpt");
const QString kSnippetKindContextExcerpt = QStringLiteral("context_excerpt");
const QString kDiagnosticsModeParseSnippet = QStringLiteral("parse_snippet");
const QString kDiagnosticsModeNone = QStringLiteral("none");

bool isContextualSnippetKind(const QString &snippetKind)
{
    return snippetKind == kSnippetKindFilePreview
        || snippetKind == kSnippetKindLineExcerpt
        || snippetKind == kSnippetKindContextExcerpt;
}

bool isNonStandaloneMemberKind(const QString &kind)
{
    static const QSet<QString> kinds = {
        QStringLiteral("method"),
        QStringLiteral("constructor"),
        QStringLiteral("property"),
        QStringLiteral("field"),
        QStringLiteral("constant"),
        QStringLiteral("variant")
    };
    return kinds.contains(kind);
}

bool shouldParseSnippetByDefault(const QString &language, const QString &kind, const QString &snippetKind)
{
    if (isContextualSnippetKind(snippetKind)) {
        return false;
    }

    if (isNonStandaloneMemberKind(kind)) {
        if (language == QStringLiteral("php")
            || language == QStringLiteral("java")
            || language == QStringLiteral("csharp")
            || language == QStringLiteral("rust")
            || language == QStringLiteral("objc")
            || language == QStringLiteral("cpp")) {
            return false;
        }
    }

    return true;
}

bool isScriptLikeLanguage(const QString &language)
{
    return language == QStringLiteral("script")
        || language == QStringLiteral("jsx")
        || language == QStringLiteral("ts")
        || language == QStringLiteral("tsx")
        || language == QStringLiteral("code");
}

bool shouldSkipRelationshipDirectory(const QString &name)
{
    const QString lower = name.toLower();
    return lower == QStringLiteral(".git")
        || lower == QStringLiteral("node_modules")
        || lower == QStringLiteral("dist")
        || lower == QStringLiteral("build")
        || lower == QStringLiteral(".next")
        || lower == QStringLiteral(".nuxt")
        || lower == QStringLiteral("coverage");
}

bool isRelationshipCandidateFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("js")
        || suffix == QStringLiteral("jsx")
        || suffix == QStringLiteral("ts")
        || suffix == QStringLiteral("tsx")
        || suffix == QStringLiteral("mjs")
        || suffix == QStringLiteral("cjs");
}

QSet<QString> relationNamesFromSnippet(const QString &snippet)
{
    static const QVector<QRegularExpression> patterns = {
        QRegularExpression(QStringLiteral(R"(\b([A-Za-z_][A-Za-z0-9_]*)\s*\()")),
        QRegularExpression(QStringLiteral(R"(\.\s*([A-Za-z_][A-Za-z0-9_]*)\s*\()"))
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

QVariantMap makeRelationFromSymbol(const QVariantMap &symbol,
                                   const QString &fallbackPath,
                                   const QString &fallbackLanguage)
{
    QVariantMap relation;
    relation.insert(QStringLiteral("kind"), symbol.value(QStringLiteral("kind")).toString());
    relation.insert(QStringLiteral("name"), symbol.value(QStringLiteral("name")).toString());
    relation.insert(QStringLiteral("line"), symbol.value(QStringLiteral("line")).toInt());
    relation.insert(QStringLiteral("detail"), symbol.value(QStringLiteral("detail")).toString());
    relation.insert(QStringLiteral("snippet"), symbol.value(QStringLiteral("snippet")).toString());
    relation.insert(QStringLiteral("path"),
                    symbol.value(QStringLiteral("path")).toString().isEmpty()
                        ? fallbackPath
                        : symbol.value(QStringLiteral("path")).toString());
    relation.insert(QStringLiteral("language"),
                    symbol.value(QStringLiteral("language")).toString().isEmpty()
                        ? fallbackLanguage
                        : symbol.value(QStringLiteral("language")).toString());
    relation.insert(QStringLiteral("snippetKind"), symbol.value(QStringLiteral("snippetKind")).toString());
    relation.insert(QStringLiteral("diagnosticsMode"), symbol.value(QStringLiteral("diagnosticsMode")).toString());
    return relation;
}

int relationKindPriority(const QString &kind)
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

void collectSymbolRelations(const QVariantList &symbols,
                            const QString &fallbackPath,
                            const QString &fallbackLanguage,
                            QHash<QString, QVariantMap> &relationsByName)
{
    for (const QVariant &entry : symbols) {
        const QVariantMap symbol = entry.toMap();
        const QString name = symbol.value(QStringLiteral("name")).toString();
        if (!name.isEmpty()) {
            const QVariantMap candidate = makeRelationFromSymbol(symbol, fallbackPath, fallbackLanguage);
            const QVariantMap existing = relationsByName.value(name);
            if (existing.isEmpty()
                || relationKindPriority(candidate.value(QStringLiteral("kind")).toString())
                    < relationKindPriority(existing.value(QStringLiteral("kind")).toString())) {
                relationsByName.insert(name, candidate);
            }
        }

        const QVariantList members = symbol.value(QStringLiteral("members")).toList();
        if (!members.isEmpty()) {
            collectSymbolRelations(members, fallbackPath, fallbackLanguage, relationsByName);
        }
    }
}

QVariantList augmentSymbolsWithRelationships(const QVariantList &symbols,
                                             const QHash<QString, QVariantMap> &importedSymbols,
                                             const QVector<QPair<QString, QVariantMap>> &incomingSymbols)
{
    QVariantList augmented;
    augmented.reserve(symbols.size());

    for (const QVariant &entry : symbols) {
        QVariantMap symbol = entry.toMap();
        symbol.insert(QStringLiteral("members"),
                      augmentSymbolsWithRelationships(symbol.value(QStringLiteral("members")).toList(),
                                                      importedSymbols,
                                                      incomingSymbols));

        QVariantList calls = symbol.value(QStringLiteral("calls")).toList();
        QVariantList calledBy = symbol.value(QStringLiteral("calledBy")).toList();
        QSet<QString> existingCalls;
        QSet<QString> existingCallers;

        for (const QVariant &callEntry : std::as_const(calls)) {
            existingCalls.insert(callEntry.toMap().value(QStringLiteral("name")).toString());
        }
        for (const QVariant &callerEntry : std::as_const(calledBy)) {
            existingCallers.insert(callerEntry.toMap().value(QStringLiteral("name")).toString());
        }

        const QSet<QString> outgoingNames = relationNamesFromSnippet(symbol.value(QStringLiteral("snippet")).toString());
        for (const QString &name : outgoingNames) {
            if (existingCalls.contains(name) || !importedSymbols.contains(name)) {
                continue;
            }
            QVariantMap relation = importedSymbols.value(name);
            relation.insert(QStringLiteral("detail"), QStringLiteral("calls imported symbol"));
            calls.append(relation);
            existingCalls.insert(name);
        }

        const QString symbolName = symbol.value(QStringLiteral("name")).toString();
        if (!symbolName.isEmpty()) {
            for (const auto &incoming : incomingSymbols) {
                const QVariantMap relation = incoming.second;
                const QString relationName = relation.value(QStringLiteral("name")).toString();
                if (existingCallers.contains(relationName)) {
                    continue;
                }
                if (!relationNamesFromSnippet(relation.value(QStringLiteral("snippet")).toString()).contains(symbolName)) {
                    continue;
                }
                QVariantMap caller = relation;
                caller.insert(QStringLiteral("detail"), QStringLiteral("called by imported usage"));
                calledBy.append(caller);
                existingCallers.insert(relationName);
            }
        }

        symbol.insert(QStringLiteral("calls"), calls);
        symbol.insert(QStringLiteral("calledBy"), calledBy);
        augmented.append(symbol);
    }

    return augmented;
}

QVariantMap augmentAnalysisWithRelationships(const QVariantMap &analysis,
                                             const QString &rootPath,
                                             const std::function<QVariantMap(const QString &)> &loadAnalysis)
{
    if (rootPath.isEmpty()) {
        return analysis;
    }

    const QString language = analysis.value(QStringLiteral("language")).toString();
    const QString selectedPath = analysis.value(QStringLiteral("path")).toString();
    if (!isScriptLikeLanguage(language) || selectedPath.isEmpty()) {
        return analysis;
    }

    QHash<QString, QVariantMap> cache;
    cache.insert(selectedPath, analysis);

    auto cachedAnalysis = [&](const QString &path) -> QVariantMap {
        const QString absolute = QFileInfo(path).absoluteFilePath();
        if (cache.contains(absolute)) {
            return cache.value(absolute);
        }
        const QVariantMap loaded = loadAnalysis(absolute);
        if (!loaded.isEmpty()) {
            cache.insert(absolute, loaded);
        }
        return loaded;
    };

    QHash<QString, QVariantMap> importedSymbols;
    const QVariantList dependencies = analysis.value(QStringLiteral("dependencies")).toList();
    for (const QVariant &entry : dependencies) {
        const QVariantMap dependency = entry.toMap();
        const QString type = dependency.value(QStringLiteral("type")).toString();
        const QString dependencyPath = dependency.value(QStringLiteral("path")).toString();
        if ((type != QStringLiteral("import") && type != QStringLiteral("require")) || dependencyPath.isEmpty()) {
            continue;
        }

        const QVariantMap importedAnalysis = cachedAnalysis(dependencyPath);
        if (importedAnalysis.isEmpty()) {
            continue;
        }

        collectSymbolRelations(importedAnalysis.value(QStringLiteral("symbols")).toList(),
                               importedAnalysis.value(QStringLiteral("path")).toString(),
                               importedAnalysis.value(QStringLiteral("language")).toString(),
                               importedSymbols);
    }

    QVector<QPair<QString, QVariantMap>> incomingSymbols;
    QDirIterator it(rootPath,
                    QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString candidatePath = QFileInfo(it.next()).absoluteFilePath();
        if (candidatePath == selectedPath) {
            continue;
        }
        if (!isRelationshipCandidateFile(candidatePath)) {
            continue;
        }

        bool skip = false;
        QString relative = QDir(rootPath).relativeFilePath(candidatePath);
        const QStringList segments = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString &segment : segments) {
            if (shouldSkipRelationshipDirectory(segment)) {
                skip = true;
                break;
            }
        }
        if (skip) {
            continue;
        }

        const QVariantMap candidateAnalysis = cachedAnalysis(candidatePath);
        if (candidateAnalysis.isEmpty()) {
            continue;
        }

        bool dependsOnSelected = false;
        const QVariantList candidateDependencies = candidateAnalysis.value(QStringLiteral("dependencies")).toList();
        for (const QVariant &dependencyEntry : candidateDependencies) {
            if (QFileInfo(dependencyEntry.toMap().value(QStringLiteral("path")).toString()).absoluteFilePath() == selectedPath) {
                dependsOnSelected = true;
                break;
            }
        }
        if (!dependsOnSelected) {
            continue;
        }

        QHash<QString, QVariantMap> relations;
        collectSymbolRelations(candidateAnalysis.value(QStringLiteral("symbols")).toList(),
                               candidateAnalysis.value(QStringLiteral("path")).toString(),
                               candidateAnalysis.value(QStringLiteral("language")).toString(),
                               relations);
        for (auto relIt = relations.cbegin(); relIt != relations.cend(); ++relIt) {
            incomingSymbols.append(qMakePair(candidatePath, relIt.value()));
        }
    }

    QVariantMap augmented = analysis;
    augmented.insert(QStringLiteral("symbols"),
                     augmentSymbolsWithRelationships(analysis.value(QStringLiteral("symbols")).toList(),
                                                     importedSymbols,
                                                     incomingSymbols));
    return augmented;
}

QVariantMap findSymbolInList(const QVariantList &symbols, const QVariantMap &needle)
{
    const QString targetName = needle.value(QStringLiteral("name")).toString();
    const QString targetKind = needle.value(QStringLiteral("kind")).toString();
    const int targetLine = needle.value(QStringLiteral("line")).toInt();

    for (const QVariant &entry : symbols) {
        const QVariantMap symbol = entry.toMap();
        if (symbol.value(QStringLiteral("name")).toString() == targetName
            && symbol.value(QStringLiteral("kind")).toString() == targetKind
            && symbol.value(QStringLiteral("line")).toInt() == targetLine) {
            return symbol;
        }

        const QVariantMap nested = findSymbolInList(symbol.value(QStringLiteral("members")).toList(), needle);
        if (!nested.isEmpty()) {
            return nested;
        }
    }

    return {};
}

QString symbolTargetPath(const QVariantMap &symbol, const QString &fallbackPath)
{
    const QString sourcePath = symbol.value(QStringLiteral("sourcePath")).toString();
    if (!sourcePath.isEmpty()) {
        return QFileInfo(sourcePath).absoluteFilePath();
    }

    const QString path = symbol.value(QStringLiteral("path")).toString();
    if (!path.isEmpty()) {
        return QFileInfo(path).absoluteFilePath();
    }

    return QFileInfo(fallbackPath).absoluteFilePath();
}

struct SnippetAnalysis {
    QString displayCode;
    QString lintCode;
    QString truncationMarker;
    bool truncated = false;
};

QString normalizeSnippetLanguage(const QString &language)
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
    if (language == QStringLiteral("php")) {
        return QStringLiteral("php");
    }
    if (language == QStringLiteral("css")) {
        return QStringLiteral("css");
    }
    if (language == QStringLiteral("html")) {
        return QStringLiteral("html");
    }
    if (language == QStringLiteral("json")) {
        return QStringLiteral("json");
    }
    if (language == QStringLiteral("python")) {
        return QStringLiteral("python");
    }
    if (language == QStringLiteral("cpp")) {
        return QStringLiteral("cpp");
    }
    if (language == QStringLiteral("java")) {
        return QStringLiteral("java");
    }
    if (language == QStringLiteral("csharp")) {
        return QStringLiteral("csharp");
    }
    if (language == QStringLiteral("rust")) {
        return QStringLiteral("rust");
    }
    if (language == QStringLiteral("objc")) {
        return QStringLiteral("objc");
    }
    return QString();
}

TSLanguage *languageForSnippet(const QString &language)
{
    if (language == QStringLiteral("php")) {
        return const_cast<TSLanguage *>(tree_sitter_php());
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
    if (language == QStringLiteral("js") || language == QStringLiteral("jsx")) {
        return const_cast<TSLanguage *>(tree_sitter_javascript());
    }
    if (language == QStringLiteral("ts")) {
        return const_cast<TSLanguage *>(tree_sitter_typescript());
    }
    return nullptr;
}

QString expandTabs(const QString &text, int tabWidth)
{
    QString result;
    result.reserve(text.size());
    int column = 0;
    for (const QChar ch : text) {
        if (ch == QLatin1Char('\t')) {
            const int spaces = qMax(1, tabWidth - (column % tabWidth));
            result += QString(spaces, QLatin1Char(' '));
            column += spaces;
        } else {
            result += ch;
            if (ch == QLatin1Char('\n')) {
                column = 0;
            } else {
                ++column;
            }
        }
    }
    return result;
}

SnippetAnalysis analyzeSnippet(const QString &snippet)
{
    SnippetAnalysis analysis;
    analysis.displayCode = snippet;
    analysis.lintCode = snippet;

    const QRegularExpression blockEllipsis(QStringLiteral(R"((\r?\n)\s*\.\.\.\s*$)"));
    QRegularExpressionMatch blockMatch = blockEllipsis.match(analysis.displayCode);
    if (blockMatch.hasMatch()) {
        analysis.truncated = true;
        analysis.truncationMarker = QStringLiteral("...");
        analysis.displayCode.chop(analysis.displayCode.size() - blockMatch.capturedStart(0));
        analysis.lintCode = analysis.displayCode;
        return analysis;
    }

    const QRegularExpression inlineEllipsis(QStringLiteral(R"(\s*\{\s*\.\.\.\s*\}\s*$)"));
    QRegularExpressionMatch inlineMatch = inlineEllipsis.match(analysis.displayCode);
    if (inlineMatch.hasMatch()) {
        analysis.truncated = true;
        analysis.truncationMarker = QStringLiteral("{ ... }");
        analysis.displayCode.chop(analysis.displayCode.size() - inlineMatch.capturedStart(0));
        analysis.lintCode = analysis.displayCode;
        return analysis;
    }

    const QRegularExpression trailingEllipsis(QStringLiteral(R"(\s+\.\.\.\s*$)"));
    QRegularExpressionMatch trailingMatch = trailingEllipsis.match(analysis.displayCode);
    if (trailingMatch.hasMatch()) {
        analysis.truncated = true;
        analysis.truncationMarker = QStringLiteral("...");
        analysis.displayCode.chop(analysis.displayCode.size() - trailingMatch.capturedStart(0));
        analysis.lintCode = analysis.displayCode;
    }

    return analysis;
}

QVariantMap makeDiagnostic(const QString &severity, const QString &message, int line = 0, int column = 0)
{
    QVariantMap diagnostic;
    diagnostic.insert(QStringLiteral("severity"), severity);
    diagnostic.insert(QStringLiteral("message"), message);
    diagnostic.insert(QStringLiteral("line"), line);
    diagnostic.insert(QStringLiteral("column"), column);
    return diagnostic;
}

QVariantList htmlDiagnostics(const QString &code, int baseLine)
{
    struct TagInfo {
        QString name;
        int line = 0;
        int column = 0;
    };

    static const QSet<QString> voidTags = {
        QStringLiteral("area"), QStringLiteral("base"), QStringLiteral("br"), QStringLiteral("col"),
        QStringLiteral("embed"), QStringLiteral("hr"), QStringLiteral("img"), QStringLiteral("input"),
        QStringLiteral("link"), QStringLiteral("meta"), QStringLiteral("param"), QStringLiteral("source"),
        QStringLiteral("track"), QStringLiteral("wbr")
    };

    QVariantList diagnostics;
    QVector<TagInfo> stack;
    const QRegularExpression tagPattern(QStringLiteral(R"(<\s*(/)?\s*([A-Za-z][A-Za-z0-9:-]*)[^>]*(/)?\s*>)"));
    auto it = tagPattern.globalMatch(code);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString full = match.captured(0);
        if (full.startsWith(QStringLiteral("<!--"))) {
            continue;
        }

        const QString tagName = match.captured(2).toLower();
        const int offset = match.capturedStart(0);
        const int line = baseLine + code.left(offset).count(QLatin1Char('\n'));
        const int lastBreak = code.left(offset).lastIndexOf(QLatin1Char('\n'));
        const int column = offset - lastBreak;

        if (!match.captured(1).isEmpty()) {
            if (stack.isEmpty()) {
                diagnostics.append(makeDiagnostic(QStringLiteral("warning"),
                                                  QStringLiteral("Closing tag </%1> has no matching opener.").arg(tagName),
                                                  line, column));
                continue;
            }

            const TagInfo opener = stack.takeLast();
            if (opener.name != tagName) {
                diagnostics.append(makeDiagnostic(QStringLiteral("warning"),
                                                  QStringLiteral("Closing tag </%1> does not match <%2>.").arg(tagName, opener.name),
                                                  line, column));
            }
            continue;
        }

        if (voidTags.contains(tagName) || !match.captured(3).isEmpty()) {
            continue;
        }

        stack.append(TagInfo{tagName, line, column});
    }

    for (const TagInfo &tag : stack) {
        diagnostics.append(makeDiagnostic(QStringLiteral("warning"),
                                          QStringLiteral("Tag <%1> is not closed in this snippet.").arg(tag.name),
                                          tag.line, tag.column));
    }

    return diagnostics;
}

QVariantList treeSitterDiagnostics(const QString &language, const QString &code, int baseLine)
{
    QVariantList diagnostics;

    TSLanguage *tsLanguage = languageForSnippet(language);
    if (!tsLanguage) {
        return diagnostics;
    }

    QByteArray source = code.toUtf8();
    int lineAdjustment = 0;
    if (language == QStringLiteral("php") && !code.startsWith(QStringLiteral("<?php"))) {
        source = QStringLiteral("<?php\n").toUtf8() + source;
        lineAdjustment = 1;
    }

    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, tsLanguage)) {
        if (parser) {
            ts_parser_delete(parser);
        }
        diagnostics.append(makeDiagnostic(QStringLiteral("info"),
                                          QStringLiteral("Parser diagnostics are unavailable for this language on this build.")));
        return diagnostics;
    }

    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    if (!tree) {
        ts_parser_delete(parser);
        diagnostics.append(makeDiagnostic(QStringLiteral("warning"),
                                          QStringLiteral("Unable to parse this snippet for diagnostics.")));
        return diagnostics;
    }

    QSet<QString> seen;
    std::function<void(TSNode)> walk = [&](TSNode node) {
        if (diagnostics.size() >= 8 || ts_node_is_null(node)) {
            return;
        }

        const bool isError = ts_node_is_error(node);
        const bool isMissing = ts_node_is_missing(node);
        if (isError || isMissing) {
            const TSPoint point = ts_node_start_point(node);
            const int line = qMax(baseLine, baseLine + static_cast<int>(point.row) - lineAdjustment);
            const int column = static_cast<int>(point.column) + 1;
            QString message;
            if (isMissing) {
                message = QStringLiteral("Missing syntax element near %1.").arg(QString::fromUtf8(ts_node_type(node)));
            } else {
                message = QStringLiteral("Parser error near %1.").arg(QString::fromUtf8(ts_node_type(node)));
            }

            const QString key = QStringLiteral("%1:%2:%3").arg(message).arg(line).arg(column);
            if (!seen.contains(key)) {
                diagnostics.append(makeDiagnostic(QStringLiteral("warning"), message, line, column));
                seen.insert(key);
            }
        }

        const uint32_t childCount = ts_node_child_count(node);
        for (uint32_t i = 0; i < childCount; ++i) {
            walk(ts_node_child(node, i));
            if (diagnostics.size() >= 8) {
                break;
            }
        }
    };

    walk(ts_tree_root_node(tree));

    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return diagnostics;
}

QVariantList snippetDiagnostics(const QString &language, const QString &code, int baseLine, bool truncated)
{
    if (code.trimmed().isEmpty()) {
        return {};
    }

    if (truncated) {
        return {};
    }

    if (language == QStringLiteral("json")) {
        QJsonParseError error;
        QJsonDocument::fromJson(code.toUtf8(), &error);
        if (error.error == QJsonParseError::NoError) {
            return {};
        }

        const QString prefix = code.left(error.offset);
        const int line = baseLine + prefix.count(QLatin1Char('\n'));
        const int lastBreak = prefix.lastIndexOf(QLatin1Char('\n'));
        const int column = static_cast<int>(error.offset) - lastBreak;
        return {makeDiagnostic(QStringLiteral("warning"), error.errorString(), line, column)};
    }

    if (language == QStringLiteral("html")) {
        return htmlDiagnostics(code, baseLine);
    }

    return treeSitterDiagnostics(language, code, baseLine);
}

QSet<QString> keywordSet(const QString &language)
{
    if (language == QStringLiteral("php")) {
        return {QStringLiteral("class"), QStringLiteral("function"), QStringLiteral("public"),
                QStringLiteral("private"), QStringLiteral("protected"), QStringLiteral("static"),
                QStringLiteral("return"), QStringLiteral("if"), QStringLiteral("else"),
                QStringLiteral("foreach"), QStringLiteral("for"), QStringLiteral("while"),
                QStringLiteral("extends"), QStringLiteral("implements"), QStringLiteral("interface"),
                QStringLiteral("trait"), QStringLiteral("const"), QStringLiteral("new"),
                QStringLiteral("namespace"), QStringLiteral("use"), QStringLiteral("null"),
                QStringLiteral("true"), QStringLiteral("false")};
    }
    if (language == QStringLiteral("css")) {
        return {QStringLiteral("@media"), QStringLiteral("@supports"), QStringLiteral("@keyframes"),
                QStringLiteral("display"), QStringLiteral("position"), QStringLiteral("color"),
                QStringLiteral("background"), QStringLiteral("font"), QStringLiteral("grid"),
                QStringLiteral("flex"), QStringLiteral("padding"), QStringLiteral("margin")};
    }
    if (language == QStringLiteral("json")) {
        return {QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("null")};
    }
    if (language == QStringLiteral("python")) {
        return {QStringLiteral("def"), QStringLiteral("class"), QStringLiteral("return"),
                QStringLiteral("if"), QStringLiteral("elif"), QStringLiteral("else"),
                QStringLiteral("for"), QStringLiteral("while"), QStringLiteral("import"),
                QStringLiteral("from"), QStringLiteral("as"), QStringLiteral("try"),
                QStringLiteral("except"), QStringLiteral("finally"), QStringLiteral("with"),
                QStringLiteral("lambda"), QStringLiteral("async"), QStringLiteral("await"),
                QStringLiteral("True"), QStringLiteral("False"), QStringLiteral("None")};
    }
    if (language == QStringLiteral("cpp")) {
        return {QStringLiteral("#include"), QStringLiteral("class"), QStringLiteral("struct"),
                QStringLiteral("namespace"), QStringLiteral("template"), QStringLiteral("return"),
                QStringLiteral("if"), QStringLiteral("else"), QStringLiteral("for"),
                QStringLiteral("while"), QStringLiteral("switch"), QStringLiteral("case"),
                QStringLiteral("public"), QStringLiteral("private"), QStringLiteral("protected"),
                QStringLiteral("const"), QStringLiteral("static"), QStringLiteral("virtual"),
                QStringLiteral("using"), QStringLiteral("auto"), QStringLiteral("nullptr")};
    }
    if (language == QStringLiteral("java") || language == QStringLiteral("csharp")) {
        return {QStringLiteral("class"), QStringLiteral("interface"), QStringLiteral("enum"),
                QStringLiteral("record"), QStringLiteral("public"), QStringLiteral("private"),
                QStringLiteral("protected"), QStringLiteral("static"), QStringLiteral("return"),
                QStringLiteral("if"), QStringLiteral("else"), QStringLiteral("for"),
                QStringLiteral("while"), QStringLiteral("switch"), QStringLiteral("case"),
                QStringLiteral("new"), QStringLiteral("using"), QStringLiteral("import"),
                QStringLiteral("namespace"), QStringLiteral("throws"), QStringLiteral("async"),
                QStringLiteral("await"), QStringLiteral("null"), QStringLiteral("true"),
                QStringLiteral("false")};
    }
    if (language == QStringLiteral("rust")) {
        return {QStringLiteral("fn"), QStringLiteral("struct"), QStringLiteral("enum"),
                QStringLiteral("trait"), QStringLiteral("impl"), QStringLiteral("pub"),
                QStringLiteral("let"), QStringLiteral("mut"), QStringLiteral("use"),
                QStringLiteral("mod"), QStringLiteral("crate"), QStringLiteral("self"),
                QStringLiteral("super"), QStringLiteral("match"), QStringLiteral("if"),
                QStringLiteral("else"), QStringLiteral("for"), QStringLiteral("while"),
                QStringLiteral("loop"), QStringLiteral("return"), QStringLiteral("async"),
                QStringLiteral("await"), QStringLiteral("const"), QStringLiteral("static")};
    }
    if (language == QStringLiteral("objc")) {
        return {QStringLiteral("@interface"), QStringLiteral("@implementation"), QStringLiteral("@property"),
                QStringLiteral("@protocol"), QStringLiteral("@end"), QStringLiteral("@class"),
                QStringLiteral("#import"), QStringLiteral("#include"), QStringLiteral("if"),
                QStringLiteral("else"), QStringLiteral("for"), QStringLiteral("while"),
                QStringLiteral("return"), QStringLiteral("nil"), QStringLiteral("self"),
                QStringLiteral("super"), QStringLiteral("static"), QStringLiteral("extern")};
    }
    return {QStringLiteral("function"), QStringLiteral("class"), QStringLiteral("const"),
            QStringLiteral("let"), QStringLiteral("var"), QStringLiteral("return"),
            QStringLiteral("if"), QStringLiteral("else"), QStringLiteral("for"),
            QStringLiteral("while"), QStringLiteral("switch"), QStringLiteral("case"),
            QStringLiteral("break"), QStringLiteral("continue"), QStringLiteral("import"),
            QStringLiteral("from"), QStringLiteral("export"), QStringLiteral("default"),
            QStringLiteral("extends"), QStringLiteral("implements"), QStringLiteral("interface"),
            QStringLiteral("type"), QStringLiteral("enum"), QStringLiteral("new"),
            QStringLiteral("this"), QStringLiteral("async"), QStringLiteral("await"),
            QStringLiteral("try"), QStringLiteral("catch"), QStringLiteral("finally"),
            QStringLiteral("throw"), QStringLiteral("null"), QStringLiteral("true"),
            QStringLiteral("false"), QStringLiteral("undefined")};
}

QString colorForToken(const QString &tokenType)
{
    if (tokenType == QStringLiteral("keyword")) {
        return QStringLiteral("#81a1c1");
    }
    if (tokenType == QStringLiteral("string")) {
        return QStringLiteral("#a3be8c");
    }
    if (tokenType == QStringLiteral("number")) {
        return QStringLiteral("#b48ead");
    }
    if (tokenType == QStringLiteral("comment")) {
        return QStringLiteral("#616e88");
    }
    if (tokenType == QStringLiteral("tag")) {
        return QStringLiteral("#88c0d0");
    }
    if (tokenType == QStringLiteral("attribute")) {
        return QStringLiteral("#d08770");
    }
    return QString();
}

void appendToken(QString &html, const QString &text, const QString &tokenType = QString())
{
    if (text.isEmpty()) {
        return;
    }

    const QString escaped = text.toHtmlEscaped();
    const QString color = colorForToken(tokenType);
    if (color.isEmpty()) {
        html += escaped;
        return;
    }

    html += QStringLiteral("<span style=\"color:%1;\">%2</span>").arg(color, escaped);
}

QString highlightHtmlSnippet(const QString &code)
{
    QString html;
    int i = 0;
    while (i < code.size()) {
        if (code.midRef(i, 4) == QStringLiteral("<!--")) {
            const int end = code.indexOf(QStringLiteral("-->"), i + 4);
            const int length = end >= 0 ? (end - i + 3) : (code.size() - i);
            appendToken(html, code.mid(i, length), QStringLiteral("comment"));
            i += length;
            continue;
        }

        if (code.at(i) == QLatin1Char('<')) {
            const int end = code.indexOf(QLatin1Char('>'), i + 1);
            const int length = end >= 0 ? (end - i + 1) : (code.size() - i);
            const QString tag = code.mid(i, length);
            QRegularExpression tagNamePattern(QStringLiteral(R"(^<\s*/?\s*([A-Za-z][A-Za-z0-9:-]*))"));
            const QRegularExpressionMatch tagMatch = tagNamePattern.match(tag);

            int cursor = 0;
            if (tagMatch.hasMatch()) {
                const int nameStart = tagMatch.capturedStart(1);
                const int nameEnd = tagMatch.capturedEnd(1);
                appendToken(html, tag.left(nameStart), QStringLiteral("tag"));
                appendToken(html, tag.mid(nameStart, nameEnd - nameStart), QStringLiteral("tag"));
                cursor = nameEnd;
            }

            QRegularExpression attrPattern(QStringLiteral(R"(([A-Za-z_:][-A-Za-z0-9_:.]*)(\s*=\s*(\"[^\"]*\"|'[^']*'))?)"));
            auto attrIt = attrPattern.globalMatch(tag);
            while (attrIt.hasNext()) {
                const QRegularExpressionMatch attrMatch = attrIt.next();
                if (attrMatch.capturedStart(1) < cursor) {
                    continue;
                }
                appendToken(html, tag.mid(cursor, attrMatch.capturedStart(1) - cursor), QStringLiteral("tag"));
                appendToken(html, attrMatch.captured(1), QStringLiteral("attribute"));
                if (!attrMatch.captured(2).isEmpty()) {
                    const QString equalsPart = attrMatch.captured(2);
                    const int quotePos = equalsPart.indexOf(QLatin1Char('"')) >= 0
                        ? equalsPart.indexOf(QLatin1Char('"'))
                        : equalsPart.indexOf(QLatin1Char('\''));
                    if (quotePos >= 0) {
                        appendToken(html, equalsPart.left(quotePos));
                        appendToken(html, equalsPart.mid(quotePos), QStringLiteral("string"));
                    } else {
                        appendToken(html, equalsPart);
                    }
                }
                cursor = attrMatch.capturedEnd(0);
            }
            appendToken(html, tag.mid(cursor), QStringLiteral("tag"));
            i += length;
            continue;
        }

        appendToken(html, QString(code.at(i)));
        ++i;
    }

    return html;
}

QString highlightCodeSnippet(const QString &code, const QString &language)
{
    if (language == QStringLiteral("html")) {
        return highlightHtmlSnippet(code);
    }

    const QSet<QString> keywords = keywordSet(language);
    QString html;
    int i = 0;
    while (i < code.size()) {
        const QChar ch = code.at(i);

        if (language == QStringLiteral("php") && ch == QLatin1Char('#')) {
            const int end = code.indexOf(QLatin1Char('\n'), i);
            const int length = end >= 0 ? (end - i) : (code.size() - i);
            appendToken(html, code.mid(i, length), QStringLiteral("comment"));
            i += length;
            continue;
        }

        if (i + 1 < code.size() && code.at(i) == QLatin1Char('/') && code.at(i + 1) == QLatin1Char('/')) {
            const int end = code.indexOf(QLatin1Char('\n'), i);
            const int length = end >= 0 ? (end - i) : (code.size() - i);
            appendToken(html, code.mid(i, length), QStringLiteral("comment"));
            i += length;
            continue;
        }

        if (i + 1 < code.size() && code.at(i) == QLatin1Char('/') && code.at(i + 1) == QLatin1Char('*')) {
            const int end = code.indexOf(QStringLiteral("*/"), i + 2);
            const int length = end >= 0 ? (end - i + 2) : (code.size() - i);
            appendToken(html, code.mid(i, length), QStringLiteral("comment"));
            i += length;
            continue;
        }

        if (ch == QLatin1Char('"') || ch == QLatin1Char('\'') || ch == QLatin1Char('`')) {
            const QChar quote = ch;
            int end = i + 1;
            bool escaped = false;
            while (end < code.size()) {
                const QChar current = code.at(end);
                if (!escaped && current == quote) {
                    ++end;
                    break;
                }
                escaped = !escaped && current == QLatin1Char('\\');
                if (escaped && current != QLatin1Char('\\')) {
                    escaped = false;
                }
                ++end;
            }
            appendToken(html, code.mid(i, end - i), QStringLiteral("string"));
            i = end;
            continue;
        }

        if (ch.isDigit()) {
            int end = i + 1;
            while (end < code.size() && (code.at(end).isDigit() || code.at(end) == QLatin1Char('.'))) {
                ++end;
            }
            appendToken(html, code.mid(i, end - i), QStringLiteral("number"));
            i = end;
            continue;
        }

        if (ch.isLetter() || ch == QLatin1Char('_') || ch == QLatin1Char('$') || ch == QLatin1Char('@')) {
            int end = i + 1;
            while (end < code.size()) {
                const QChar current = code.at(end);
                if (!(current.isLetterOrNumber() || current == QLatin1Char('_')
                      || current == QLatin1Char('$') || current == QLatin1Char('-'))) {
                    break;
                }
                ++end;
            }

            const QString word = code.mid(i, end - i);
            const QString tokenType = keywords.contains(word) ? QStringLiteral("keyword") : QString();
            appendToken(html, word, tokenType);
            i = end;
            continue;
        }

        appendToken(html, QString(ch));
        ++i;
    }

    return html;
}

QString buildSnippetHtml(const QString &code, const QString &language, const QString &truncationMarker)
{
    QString html = QStringLiteral("<pre style=\"margin:0;font-family:monospace;white-space:pre;\">");
    html += highlightCodeSnippet(expandTabs(code, 2), language);
    if (!truncationMarker.isEmpty()) {
        if (!code.isEmpty() && !code.endsWith(QLatin1Char('\n'))) {
            html += QLatin1Char('\n');
        }
        html += QStringLiteral("<span style=\"color:#616e88;font-style:italic;\">%1</span>")
                    .arg(truncationMarker.toHtmlEscaped());
    }
    html += QStringLiteral("</pre>");
    return html;
}

QString loadFilePreviewSnippet(const QString &path)
{
    QFileInfo info(path);
    if (!info.exists() || info.isDir()) {
        return QString();
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }

    const bool truncatedBySize = info.size() > kMaxPreviewBytes;
    const QByteArray previewBytes = truncatedBySize ? file.read(kMaxPreviewBytes) : file.readAll();
    const QString text = QString::fromUtf8(previewBytes);
    const QStringList lines = text.split(QLatin1Char('\n'));
    const int maxLines = 40;
    if (lines.size() <= maxLines && !truncatedBySize) {
        return text;
    }

    QString preview = lines.mid(0, maxLines).join(QLatin1Char('\n'));
    if (truncatedBySize) {
        preview += QStringLiteral("\n... [preview truncated: file too large to load fully]");
        return preview;
    }
    return preview + QStringLiteral("\n...");
}

} // namespace

ProjectController::ProjectController(QObject *parent)
    : QObject(parent)
    , m_fileSystemModel(new FileSystemModel(this))
    , m_symbolParser(new SymbolParser(this))
{
    m_rootPath = QDir::currentPath();
    QSettings settings;
    m_preferredEditor = settings.value(QStringLiteral("settings/preferredEditor")).toString().trimmed();
}

ProjectController::~ProjectController() = default;

QObject *ProjectController::fileSystemModel() const
{
    return m_fileSystemModel;
}

QString ProjectController::rootPath() const
{
    return m_rootPath;
}

QVariantMap ProjectController::projectSummary() const
{
    return m_fileSystemModel->collectStats();
}

QString ProjectController::selectedPath() const
{
    return m_selectedPath;
}

QString ProjectController::selectedRelativePath() const
{
    if (m_selectedPath.isEmpty()) {
        return QString();
    }

    if (m_rootPath.isEmpty() || m_selectedPath == m_rootPath) {
        return QStringLiteral("/");
    }

    QDir rootDir(m_rootPath);
    const QString relative = rootDir.relativeFilePath(m_selectedPath);
    return relative.isEmpty() ? QStringLiteral("/") : relative;
}

QVariantMap ProjectController::selectedFileData() const
{
    return m_selectedFileData;
}

QVariantMap ProjectController::selectedSymbol() const
{
    return m_selectedSymbol;
}

QVariantList ProjectController::selectedSymbolMembers() const
{
    return m_selectedSymbol.value(QStringLiteral("members")).toList();
}

QVariantMap ProjectController::selectedSnippet() const
{
    return m_selectedSnippet;
}

QString ProjectController::preferredEditor() const
{
    return m_preferredEditor;
}

QString ProjectController::lastOpenedPath() const
{
    QSettings settings;
    return settings.value(QStringLiteral("session/lastOpenedPath")).toString();
}

void ProjectController::setRootPath(const QString &path)
{
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (!QFileInfo(absolute).isDir()) {
        return;
    }

    m_rootPath = absolute;
    m_fileSystemModel->setRootPath(absolute);
    saveLastOpenedPath(absolute);
    emit rootPathChanged();

    selectPath(absolute);
}

void ProjectController::selectPath(const QString &path)
{
    m_selectedPath = path;
    emit selectedPathChanged();

    const QFileInfo info(path);
    if (!info.exists()) {
        m_selectedFileData = SymbolParser::makeResultSkeleton(path, info.fileName().isEmpty() ? path : info.fileName(), QString());
        m_selectedSymbol = {};
        m_selectedSnippet = makeFileSnippet();
        emit selectedFileDataChanged();
        emit selectedSymbolChanged();
        emit selectedSnippetChanged();
        return;
    }

    if (info.isDir()) {
        m_selectedFileData = SymbolParser::makeResultSkeleton(path, info.fileName().isEmpty() ? path : info.fileName(), QStringLiteral("folder"));
        m_selectedFileData.insert(QStringLiteral("summary"), QStringLiteral("Directory"));
    } else {
        m_selectedFileData = parseFileSafely(path);
    }

    m_selectedSymbol = {};
    m_selectedSnippet = makeFileSnippet();
    emit selectedFileDataChanged();
    emit selectedSymbolChanged();
    emit selectedSnippetChanged();
}

QVariantMap ProjectController::parseFileSafely(const QString &path) const
{
    auto parseWithHelper = [](const QString &targetPath) -> QVariantMap {
        const QFileInfo info(targetPath);
        QVariantMap fallback = SymbolParser::makeResultSkeleton(targetPath,
                                                                info.fileName().isEmpty() ? targetPath : info.fileName(),
                                                                QString());

        const QString helperPath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("lumencode-cli"));
        if (!QFileInfo::exists(helperPath)) {
            fallback.insert(QStringLiteral("summary"), QStringLiteral("Analysis unavailable: parser helper not found"));
            return fallback;
        }

        QProcess helper;
        helper.start(helperPath, {QStringLiteral("--dump-file"), targetPath});
        if (!helper.waitForStarted(3000)) {
            fallback.insert(QStringLiteral("summary"), QStringLiteral("Analysis unavailable: parser helper could not start"));
            return fallback;
        }

        if (!helper.waitForFinished(15000)) {
            helper.kill();
            helper.waitForFinished(1000);
            fallback.insert(QStringLiteral("summary"), QStringLiteral("Analysis unavailable: parser helper timed out"));
            return fallback;
        }

        if (helper.exitStatus() != QProcess::NormalExit || helper.exitCode() != 0) {
            fallback.insert(QStringLiteral("summary"), QStringLiteral("Analysis unavailable: parser helper failed"));
            return fallback;
        }

        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(helper.readAllStandardOutput(), &error);
        if (doc.isNull() || !doc.isObject()) {
            fallback.insert(QStringLiteral("summary"), QStringLiteral("Analysis unavailable: parser helper returned invalid data"));
            return fallback;
        }

        QVariantMap result = doc.object().toVariantMap();
        if (!result.contains(QStringLiteral("path"))) {
            result.insert(QStringLiteral("path"), targetPath);
        }
        if (!result.contains(QStringLiteral("fileName"))) {
            result.insert(QStringLiteral("fileName"), info.fileName().isEmpty() ? targetPath : info.fileName());
        }
        return result;
    };

    const QVariantMap result = parseWithHelper(path);
    return augmentAnalysisWithRelationships(result, m_rootPath, parseWithHelper);
}

void ProjectController::selectSymbol(int index)
{
    const QVariantList symbols = m_selectedFileData.value(QStringLiteral("symbols")).toList();
    if (index < 0 || index >= symbols.size()) {
        m_selectedSymbol = {};
        m_selectedSnippet = makeFileSnippet();
    } else {
        m_selectedSymbol = symbols.at(index).toMap();
        m_selectedSnippet = makeSymbolSnippet(m_selectedSymbol, m_selectedFileData);
    }
    emit selectedSymbolChanged();
    emit selectedSnippetChanged();
}

void ProjectController::selectSymbolByData(const QVariantMap &symbol)
{
    QVariantMap resolved = symbol;
    const QString currentPath = QFileInfo(m_selectedFileData.value(QStringLiteral("path")).toString()).absoluteFilePath();
    const QString targetPath = symbolTargetPath(symbol, currentPath);

    if (!targetPath.isEmpty() && targetPath != currentPath) {
        m_selectedPath = targetPath;
        emit selectedPathChanged();
        m_selectedFileData = parseFileSafely(targetPath);
        emit selectedFileDataChanged();
    }

    const QVariantMap hydrated = findSymbolInList(m_selectedFileData.value(QStringLiteral("symbols")).toList(), symbol);
    if (!hydrated.isEmpty()) {
        resolved = hydrated;
    }

    m_selectedSymbol = resolved;
    m_selectedSnippet = m_selectedSymbol.isEmpty()
        ? makeFileSnippet()
        : makeSymbolSnippet(m_selectedSymbol, m_selectedFileData);
    emit selectedSymbolChanged();
    emit selectedSnippetChanged();
}

void ProjectController::setPreferredEditor(const QString &command)
{
    const QString normalized = command.trimmed();
    if (m_preferredEditor == normalized) {
        return;
    }
    m_preferredEditor = normalized;
    QSettings settings;
    settings.setValue(QStringLiteral("settings/preferredEditor"), m_preferredEditor);
    emit preferredEditorChanged();
}

bool ProjectController::openCurrentInFolder() const
{
    QString path = currentOpenableFilePath();
    if (path.isEmpty()) {
        path = m_selectedPath;
    }
    if (path.isEmpty()) {
        return false;
    }

    const QFileInfo info(path);
    const QString folderPath = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    return !folderPath.isEmpty() && QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
}

bool ProjectController::openCurrentInEditor() const
{
    const QString filePath = currentOpenableFilePath();
    if (filePath.isEmpty()) {
        return false;
    }

    if (m_preferredEditor.isEmpty()) {
        return QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
    }

    QStringList command = QProcess::splitCommand(m_preferredEditor);
    if (command.isEmpty()) {
        return false;
    }

    QString program = command.takeFirst();
    bool substituted = false;
    if (program.contains(QStringLiteral("%f"))) {
        program.replace(QStringLiteral("%f"), filePath);
        substituted = true;
    }
    for (QString &arg : command) {
        if (arg.contains(QStringLiteral("%f"))) {
            arg.replace(QStringLiteral("%f"), filePath);
            substituted = true;
        }
    }
    if (!substituted) {
        command.append(filePath);
    }
    return QProcess::startDetached(program, command);
}

QString ProjectController::pickFolder() const
{
    return QFileDialog::getExistingDirectory(nullptr,
                                             QStringLiteral("Select a project folder"),
                                             !lastOpenedPath().isEmpty() ? lastOpenedPath()
                                                                         : (m_rootPath.isEmpty() ? QDir::homePath() : m_rootPath),
                                             QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
}

bool ProjectController::restoreLastOpenedPath()
{
    const QString path = lastOpenedPath();
    if (path.isEmpty() || !QFileInfo(path).isDir()) {
        return false;
    }

    setRootPath(path);
    return true;
}

void ProjectController::saveLastOpenedPath(const QString &path) const
{
    QSettings settings;
    settings.setValue(QStringLiteral("session/lastOpenedPath"), path);
}

QString ProjectController::currentOpenableFilePath() const
{
    const QStringList candidates = {
        m_selectedSnippet.value(QStringLiteral("path")).toString(),
        m_selectedPath,
        m_selectedFileData.value(QStringLiteral("path")).toString()
    };

    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.exists() && info.isFile()) {
            return info.absoluteFilePath();
        }
    }
    return {};
}

QVariantMap ProjectController::makeFileSnippet() const
{
    QVariantMap snippet;
    const QString path = m_selectedFileData.value(QStringLiteral("path")).toString();
    const QString language = m_selectedFileData.value(QStringLiteral("language")).toString();
    snippet.insert(QStringLiteral("kind"), QStringLiteral("file"));
    snippet.insert(QStringLiteral("name"), m_selectedFileData.value(QStringLiteral("fileName")).toString());
    snippet.insert(QStringLiteral("path"), path);
    snippet.insert(QStringLiteral("language"), language);
    snippet.insert(QStringLiteral("line"), 0);
    snippet.insert(QStringLiteral("snippet"), loadFilePreviewSnippet(path));
    snippet.insert(QStringLiteral("detail"), m_selectedFileData.value(QStringLiteral("summary")).toString());
    snippet.insert(QStringLiteral("snippetKind"), kSnippetKindFilePreview);
    snippet.insert(QStringLiteral("diagnosticsMode"), kDiagnosticsModeNone);
    return enrichSnippetPayload(snippet);
}

QVariantMap ProjectController::makeSymbolSnippet(const QVariantMap &symbol, const QVariantMap &fileData)
{
    QVariantMap snippet;
    snippet.insert(QStringLiteral("kind"), symbol.value(QStringLiteral("kind")).toString());
    snippet.insert(QStringLiteral("name"), symbol.value(QStringLiteral("name")).toString());
    snippet.insert(QStringLiteral("path"),
                   symbol.value(QStringLiteral("sourcePath")).toString().isEmpty()
                       ? (symbol.value(QStringLiteral("path")).toString().isEmpty()
                       ? fileData.value(QStringLiteral("path")).toString()
                       : symbol.value(QStringLiteral("path")).toString())
                       : symbol.value(QStringLiteral("sourcePath")).toString());
    snippet.insert(QStringLiteral("language"),
                   symbol.value(QStringLiteral("sourceLanguage")).toString().isEmpty()
                       ? (symbol.value(QStringLiteral("language")).toString().isEmpty()
                       ? fileData.value(QStringLiteral("language")).toString()
                       : symbol.value(QStringLiteral("language")).toString())
                       : symbol.value(QStringLiteral("sourceLanguage")).toString());
    snippet.insert(QStringLiteral("line"), symbol.value(QStringLiteral("line")).toInt());
    snippet.insert(QStringLiteral("snippet"), symbol.value(QStringLiteral("snippet")).toString());
    snippet.insert(QStringLiteral("detail"), symbol.value(QStringLiteral("detail")).toString());
    if (symbol.contains(QStringLiteral("snippetKind"))) {
        snippet.insert(QStringLiteral("snippetKind"), symbol.value(QStringLiteral("snippetKind")).toString());
    }
    if (symbol.contains(QStringLiteral("diagnosticsMode"))) {
        snippet.insert(QStringLiteral("diagnosticsMode"), symbol.value(QStringLiteral("diagnosticsMode")).toString());
    }
    if (symbol.value(QStringLiteral("skipDiagnostics")).toBool()) {
        snippet.insert(QStringLiteral("diagnosticsMode"), kDiagnosticsModeNone);
    }
    return enrichSnippetPayload(snippet);
}

QVariantMap ProjectController::enrichSnippetPayload(const QVariantMap &snippet)
{
    QVariantMap enriched = snippet;
    const QString normalizedLanguage = normalizeSnippetLanguage(snippet.value(QStringLiteral("language")).toString());
    const QString rawSnippet = snippet.value(QStringLiteral("snippet")).toString();
    const int baseLine = qMax(1, snippet.value(QStringLiteral("line")).toInt());
    const SnippetAnalysis analysis = analyzeSnippet(rawSnippet);
    QString snippetKind = snippet.value(QStringLiteral("snippetKind")).toString();
    if (snippetKind.isEmpty()) {
        snippetKind = rawSnippet.isEmpty() ? kSnippetKindFilePreview : kSnippetKindExactConstruct;
    }
    QString diagnosticsMode = snippet.value(QStringLiteral("diagnosticsMode")).toString();
    if (diagnosticsMode.isEmpty()) {
        diagnosticsMode = snippet.value(QStringLiteral("skipDiagnostics")).toBool()
            ? kDiagnosticsModeNone
            : (shouldParseSnippetByDefault(normalizedLanguage,
                                           snippet.value(QStringLiteral("kind")).toString(),
                                           snippetKind)
                ? kDiagnosticsModeParseSnippet
                : kDiagnosticsModeNone);
    }

    enriched.insert(QStringLiteral("language"), normalizedLanguage);
    enriched.insert(QStringLiteral("snippetKind"), snippetKind);
    enriched.insert(QStringLiteral("diagnosticsMode"), diagnosticsMode);
    enriched.insert(QStringLiteral("tabWidthSpaces"), 2);
    enriched.insert(QStringLiteral("isTruncated"), analysis.truncated);
    enriched.insert(QStringLiteral("displaySnippet"), expandTabs(analysis.displayCode, 2));
    enriched.insert(QStringLiteral("displayHtml"),
                    rawSnippet.isEmpty()
                        ? QStringLiteral("<pre style=\"margin:0;font-family:monospace;white-space:pre;color:#616e88;\">Select a symbol to inspect its source snippet.</pre>")
                        : buildSnippetHtml(analysis.displayCode, normalizedLanguage, analysis.truncationMarker));
    enriched.insert(QStringLiteral("diagnostics"),
                    diagnosticsMode == kDiagnosticsModeNone
                        ? QVariantList{}
                        : snippetDiagnostics(normalizedLanguage, analysis.lintCode, baseLine, analysis.truncated));
    return enriched;
}
