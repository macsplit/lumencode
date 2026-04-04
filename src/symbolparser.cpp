#include "symbolparser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <cstring>
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

static bool shouldSkipFileBySize(const QFileInfo &info, qint64 limit)
{
    return info.exists() && info.isFile() && info.size() > limit;
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
        const QString currentType = QString::fromUtf8(ts_node_type(current));
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
    relation.insert(QStringLiteral("calls"), QVariantList{});
    relation.insert(QStringLiteral("calledBy"), QVariantList{});
    return relation;
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
        if (existing.value(QStringLiteral("name")).toString() == adjusted.value(QStringLiteral("name")).toString()
            && existing.value(QStringLiteral("line")).toInt() == adjusted.value(QStringLiteral("line")).toInt()) {
            return;
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

static QString swiftDeclarationKind(TSNode node, const QByteArray &source)
{
    const QString type = QString::fromUtf8(ts_node_type(node));
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
    const QString type = QString::fromUtf8(ts_node_type(node));
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
    const QString type = QString::fromUtf8(ts_node_type(node));
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
    if (QString::fromUtf8(ts_node_type(node)) != QStringLiteral("call_expression")) {
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
    const QString type = QString::fromUtf8(ts_node_type(node));
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
    const QString type = QString::fromUtf8(ts_node_type(node));
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

static QString pythonCallableKeyForNode(TSNode node,
                                        const QByteArray &source,
                                        const QHash<QString, QVariantMap> &byKey)
{
    if (QString::fromUtf8(ts_node_type(node)) != QStringLiteral("function_definition")) {
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
    if (QString::fromUtf8(ts_node_type(node)) != QStringLiteral("call")) {
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
    const QString type = QString::fromUtf8(ts_node_type(node));
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
    const QString type = QString::fromUtf8(ts_node_type(node));
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
    const QString type = QString::fromUtf8(ts_node_type(node));
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
    const QString type = QString::fromUtf8(ts_node_type(node));
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
    if (QString::fromUtf8(ts_node_type(node)) != QStringLiteral("function_item")) {
        return QString();
    }

    const QString name = nodeText(fieldNode(node, "name"), source).trimmed();
    const int line = nodeLine(node);

    QStringList candidateKinds;
    const TSNode parent = ts_node_parent(node);
    const QString parentType = QString::fromUtf8(ts_node_type(parent));
    if (parentType == QStringLiteral("declaration_list")) {
        const TSNode grandParent = ts_node_parent(parent);
        const QString grandParentType = QString::fromUtf8(ts_node_type(grandParent));
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
    if (QString::fromUtf8(ts_node_type(node)) != QStringLiteral("call_expression")) {
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
    const QString type = QString::fromUtf8(ts_node_type(node));
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
        const QString valueType = QString::fromUtf8(ts_node_type(valueNode));
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
        const QString valueType = QString::fromUtf8(ts_node_type(valueNode));
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
        const QString valueType = QString::fromUtf8(ts_node_type(valueNode));
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
    const QString type = QString::fromUtf8(ts_node_type(node));
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

static QVariantMap findCssClassSummaryEntry(const QString &cssPath, const QString &cssText, const QString &name)
{
    const QString escapedName = QRegularExpression::escape(name);
    QRegularExpression selectorPattern(QStringLiteral(R"(\.%1\b[^{\n]*\{)").arg(escapedName));
    const QRegularExpressionMatch match = selectorPattern.match(cssText);
    if (!match.hasMatch()) {
        return makeCssClassSummaryEntry(name, true, cssPath, 0, QStringLiteral(".%1 { ... }").arg(name));
    }

    const int start = match.capturedStart(0);
    const int line = cssText.left(start).count(QLatin1Char('\n')) + 1;
    const QString snippet = cssText.mid(start, 100).split(QLatin1Char('\n')).at(0).trimmed()
        + QStringLiteral(" ...");
    return makeCssClassSummaryEntry(name, true, cssPath, line, snippet);
}

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
    QVariantMap item;
    item.insert(QStringLiteral("sourcePath"), sourcePath);
    item.insert(QStringLiteral("sourceLanguage"), sourceLanguage);
    item.insert(QStringLiteral("line"), line);
    item.insert(QStringLiteral("snippet"), snippet);
    item.insert(QStringLiteral("snippetKind"), snippetKind);
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

    const QString snippet = symbol.value(QStringLiteral("snippet")).toString();
    if (snippet.trimmed().isEmpty()) {
        return symbol;
    }

    const QString normalizedLanguage = normalizeSignatureLanguage(language);
    const QString symbolName = symbol.value(QStringLiteral("name")).toString();
    QString parameterText;
    QString explicitReturn;
    const QString head = signatureHeadForSnippet(snippet);
    const QString kind = symbol.value(QStringLiteral("kind")).toString();

    if (head.startsWith(QLatin1Char(':'))) {
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
                        QStringLiteral("extern"), QStringLiteral("sealed")
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
            appendReturnDetail(returns, returnIt.next().captured(1));
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

SymbolParser::SymbolParser(QObject *parent)
    : QObject(parent)
{
}

QVariantMap SymbolParser::parseFile(const QString &path) const
{
    QFile file(path);
    const QFileInfo info(path);
    const QString language = detectLanguage(path);
    QVariantMap result = makeResultSkeleton(path, info.fileName(), language);
    auto finalizeResult = [](QVariantMap analysis) {
        return enrichAnalysisSignatures(analysis);
    };

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.insert(QStringLiteral("summary"), QStringLiteral("Unable to read file"));
        return finalizeResult(result);
    }

    if (shouldSkipFileBySize(info, kMaxParsableFileBytes)) {
        return finalizeResult(makeOversizedFileResult(path, language, info.size(), kMaxParsableFileBytes));
    }

    const QString text = QString::fromUtf8(file.readAll());

    if (language == QStringLiteral("php")) {
        const QVariantMap treeSitterResult = parsePhpTreeSitter(path, text);
        if (!treeSitterResult.value(QStringLiteral("symbols")).toList().isEmpty()) {
            return finalizeResult(treeSitterResult);
        }
        return finalizeResult(parsePhp(path, text));
    }
    if (language == QStringLiteral("swift")) {
        const QVariantMap treeSitterResult = parseSwiftTreeSitter(path, text);
        if (!treeSitterResult.value(QStringLiteral("symbols")).toList().isEmpty()) {
            return finalizeResult(treeSitterResult);
        }
        QVariantMap fallback = makeResultSkeleton(path, info.fileName(), language);
        fallback.insert(QStringLiteral("summary"), QStringLiteral("No Swift symbols extracted"));
        return finalizeResult(fallback);
    }
    if (language == QStringLiteral("html")) {
        return finalizeResult(parseHtml(path, text));
    }
    if (language == QStringLiteral("qml")) {
        return finalizeResult(parseQml(path, text));
    }
    if (language == QStringLiteral("css")) {
        const QVariantMap treeSitterResult = parseCssTreeSitter(path, text);
        if (!treeSitterResult.value(QStringLiteral("symbols")).toList().isEmpty()) {
            return finalizeResult(treeSitterResult);
        }
        return finalizeResult(parseCss(path, text));
    }
    if (language == QStringLiteral("script")) {
        return finalizeResult(parseScriptLike(path, text, false));
    }
    if (language == QStringLiteral("jsx")) {
        return finalizeResult(parseScriptLike(path, text, true));
    }
    if (language == QStringLiteral("tsx")
        || language == QStringLiteral("ts")) {
        const QVariantMap treeSitterResult = parseScriptLikeTreeSitter(path, text, language);
        if (!treeSitterResult.value(QStringLiteral("symbols")).toList().isEmpty()) {
            return finalizeResult(treeSitterResult);
        }
        if (language == QStringLiteral("tsx")) {
            return finalizeResult(parseScriptLike(path, text, true));
        }
        return finalizeResult(parseScriptLike(path, text, false));
    }
    if (language == QStringLiteral("json")) {
        return finalizeResult(parseJson(path, text));
    }
    if (language == QStringLiteral("python")) {
        const QVariantMap treeSitterResult = parsePythonTreeSitter(path, text);
        if (!treeSitterResult.value(QStringLiteral("symbols")).toList().isEmpty()) {
            return finalizeResult(treeSitterResult);
        }
        return finalizeResult(parsePython(path, text));
    }
    if (language == QStringLiteral("cpp")) {
        return finalizeResult(parseCppLike(path, text, language));
    }
    if (language == QStringLiteral("java")) {
        const QVariantMap treeSitterResult = parseJavaTreeSitter(path, text);
        if (!treeSitterResult.value(QStringLiteral("symbols")).toList().isEmpty()) {
            return finalizeResult(treeSitterResult);
        }
        return finalizeResult(parseJava(path, text));
    }
    if (language == QStringLiteral("csharp")) {
        const QVariantMap treeSitterResult = parseCSharpTreeSitter(path, text);
        if (!treeSitterResult.value(QStringLiteral("symbols")).toList().isEmpty()) {
            return finalizeResult(treeSitterResult);
        }
        return finalizeResult(parseCSharp(path, text));
    }
    if (language == QStringLiteral("rust")) {
        const QVariantMap treeSitterResult = parseRustTreeSitter(path, text);
        if (!treeSitterResult.value(QStringLiteral("symbols")).toList().isEmpty()) {
            return finalizeResult(treeSitterResult);
        }
        return finalizeResult(parseRust(path, text));
    }
    if (language == QStringLiteral("objc")) {
        return finalizeResult(parseObjectiveC(path, text, language));
    }
    return finalizeResult(result);
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

    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    TSNode root = ts_tree_root_node(tree);

    std::function<QVariantList(TSNode)> parseSwiftBodyMembers = [&](TSNode body) {
        QVariantList members;
        if (ts_node_is_null(body)) {
            return members;
        }

        const uint32_t count = ts_node_named_child_count(body);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(body, i);
            const QString type = QString::fromUtf8(ts_node_type(child));
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
        const QString type = QString::fromUtf8(ts_node_type(child));
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

            if (!activeKey.isEmpty() && QString::fromUtf8(ts_node_type(node)) == QStringLiteral("call_expression")) {
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

    symbols = applySnippetCallRelations(symbols);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QStringLiteral("swift"));
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

    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    TSNode root = ts_tree_root_node(tree);

    auto parsePhpMembers = [&](TSNode declarationList) {
        QVariantList members;
        const uint32_t count = ts_node_named_child_count(declarationList);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(declarationList, i);
            const QString type = QString::fromUtf8(ts_node_type(child));
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
        const QString type = QString::fromUtf8(ts_node_type(child));

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

            const QString nodeType = QString::fromUtf8(ts_node_type(node));
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

    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    TSNode root = ts_tree_root_node(tree);

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
            const QString type = QString::fromUtf8(ts_node_type(child));
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
            const QString type = QString::fromUtf8(ts_node_type(child));
            if (type == QStringLiteral("pair")) {
                const TSNode keyNode = fieldNode(child, "key");
                const TSNode valueNode = fieldNode(child, "value");
                const QString key = nodeText(keyNode, source);
                const QString valueType = QString::fromUtf8(ts_node_type(valueNode));
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
        const QString type = QString::fromUtf8(ts_node_type(node));
        if (type != QStringLiteral("export_statement")) {
            return node;
        }
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            const QString childType = QString::fromUtf8(ts_node_type(child));
            if (childType != QStringLiteral("comment")) {
                return child;
            }
        }
        return node;
    };

    const uint32_t count = ts_node_named_child_count(root);
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

    for (uint32_t i = 0; i < count; ++i) {
        TSNode originalNode = ts_node_named_child(root, i);
        const QString originalType = QString::fromUtf8(ts_node_type(originalNode));
        bool isExported = originalType == QStringLiteral("export_statement");
        TSNode child = unwrapExport(originalNode);
        const QString type = QString::fromUtf8(ts_node_type(child));

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
            }
        }

        if (type == QStringLiteral("function_declaration")
            || type == QStringLiteral("generator_function_declaration")) {
            const QString name = nodeText(fieldNode(child, "name"), source);
            QVariantMap symbol = makeSymbol(classifyFunctionName(name), name, nodeLine(child), QString(), {}, nodeSnippet(child, source));
            if (isExported) symbol.insert(QStringLiteral("detail"), QStringLiteral("exported"));
            appendSymbolIfNew(symbol);
        } else if (type == QStringLiteral("identifier") && isExported) {
            const QString name = nodeText(child, source);
            appendSymbolIfNew(makeSymbol(QStringLiteral("variable"), name, nodeLine(child), QStringLiteral("exported"), {}, nodeSnippet(child, source)));
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
                if (QString::fromUtf8(ts_node_type(declarator)) == QStringLiteral("variable_declarator")) {
                    const QString name = nodeText(fieldNode(declarator, "name"), source);
                    const TSNode valueNode = fieldNode(declarator, "value");
                    const QString valueType = QString::fromUtf8(ts_node_type(valueNode));
                    
                    QVariantMap symbol;
                    if (valueType == QStringLiteral("arrow_function")
                        || valueType == QStringLiteral("function")
                        || valueType == QStringLiteral("function_expression")) {
                        symbol = makeSymbol(classifyFunctionName(name), name, nodeLine(declarator),
                                                     valueType == QStringLiteral("function_expression")
                                                         ? QStringLiteral("function expression")
                                                         : (isExported ? QStringLiteral("exported") : QString()),
                                                     {}, nodeSnippet(child, source));
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
                    appendSymbolIfNew(symbol);

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
            const QString exprType = QString::fromUtf8(ts_node_type(expression));
            if (exprType == QStringLiteral("assignment_expression")) {
                const QString left = nodeText(fieldNode(expression, "left"), source);
                TSNode valueNode = fieldNode(expression, "right");
                const QString valueType = QString::fromUtf8(ts_node_type(valueNode));
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
            } else if (exprType == QStringLiteral("call_expression")) {
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
    }

    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), language);
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
            QString activeKey = currentKey;
            const QString nodeKey = jsCallableKeyForNode(node, source, byKey, reactMode);
            if (!nodeKey.isEmpty()) {
                activeKey = nodeKey;
            }

            const QString nodeType = QString::fromUtf8(ts_node_type(node));
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

    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    TSNode root = ts_tree_root_node(tree);

    std::function<void(TSNode)> visit = [&](TSNode node) {
        const QString type = QString::fromUtf8(ts_node_type(node));
        if (type == QStringLiteral("class_selector")) {
            QString name = nodeText(node, source);
            if (name.startsWith(QLatin1Char('.'))) {
                name.remove(0, 1);
            }
            const TSNode snippetNode = firstAncestorOfType(node, {"rule_set", "block"});
            const TSNode effectiveNode = ts_node_is_null(snippetNode) ? node : snippetNode;
            symbols.append(makeSymbol(QStringLiteral("class"), name, nodeLine(effectiveNode), QString(), {}, nodeSnippet(effectiveNode, source)));
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
    result.insert(QStringLiteral("symbols"), symbols);
    enrichCssAnalysisWithHtmlUsage(result, path, text);
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

    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), extractPythonDependencies(text));
    result.insert(QStringLiteral("routes"), extractPythonRoutes(path, text));
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
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

    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    TSNode root = ts_tree_root_node(tree);

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
            if (QString::fromUtf8(ts_node_type(child)) == QStringLiteral("decorator")) {
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
            QString childType = QString::fromUtf8(ts_node_type(child));
            if (childType == QStringLiteral("decorated_definition")) {
                detail = decorationDetail(child);
                child = fieldNode(child, "definition");
                childType = QString::fromUtf8(ts_node_type(child));
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
            members.append(makeSymbol(kind, name, nodeLine(child), detail, {}, nodeSnippet(snippetNode, source)));
        }
        return members;
    };

    parsePythonDefinition = [&](TSNode rawNode, TSNode snippetNode, const QString &detail) -> QVariantMap {
        const QString type = QString::fromUtf8(ts_node_type(rawNode));
        if (type == QStringLiteral("class_definition")) {
            const QString name = nodeText(fieldNode(rawNode, "name"), source);
            const TSNode body = fieldNode(rawNode, "body");
            return makeSymbol(QStringLiteral("class"), name, nodeLine(rawNode), detail,
                              parsePythonClassMembers(body), nodeSnippet(snippetNode, source));
        }
        if (type == QStringLiteral("function_definition")) {
            const QString name = nodeText(fieldNode(rawNode, "name"), source);
            return makeSymbol(QStringLiteral("function"), name, nodeLine(rawNode), detail, {},
                              nodeSnippet(snippetNode, source));
        }
        return {};
    };

    const uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_named_child(root, i);
        const QString type = QString::fromUtf8(ts_node_type(child));
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

            if (!activeKey.isEmpty() && QString::fromUtf8(ts_node_type(node)) == QStringLiteral("call")) {
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
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), extractPythonDependencies(text));
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

    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    TSNode root = ts_tree_root_node(tree);

    auto hasModifier = [&](TSNode node, const QString &modifier) {
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            if (QString::fromUtf8(ts_node_type(child)) == QStringLiteral("modifiers")
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
            if (QString::fromUtf8(ts_node_type(child)) != QStringLiteral("variable_declarator")) {
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
            const QString type = QString::fromUtf8(ts_node_type(child));
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
        const QString type = QString::fromUtf8(ts_node_type(child));
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

            const QString nodeType = QString::fromUtf8(ts_node_type(node));
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

    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    TSNode root = ts_tree_root_node(tree);

    auto hasModifier = [&](TSNode node, const QString &modifier) {
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            if (QString::fromUtf8(ts_node_type(child)) == QStringLiteral("modifier")
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
            const QString type = QString::fromUtf8(ts_node_type(child));
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
                    if (QString::fromUtf8(ts_node_type(fieldChild)) != QStringLiteral("variable_declaration")) {
                        continue;
                    }
                    const uint32_t varCount = ts_node_named_child_count(fieldChild);
                    for (uint32_t varIndex = 0; varIndex < varCount; ++varIndex) {
                        TSNode declarator = ts_node_named_child(fieldChild, varIndex);
                        if (QString::fromUtf8(ts_node_type(declarator)) != QStringLiteral("variable_declarator")) {
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
            const QString type = QString::fromUtf8(ts_node_type(child));
            if (type == QStringLiteral("using_directive")) {
                appendDependency(child);
            } else if (type == QStringLiteral("namespace_declaration")
                       || type == QStringLiteral("file_scoped_namespace_declaration")) {
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

            const QString nodeType = QString::fromUtf8(ts_node_type(node));
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

    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    TSNode root = ts_tree_root_node(tree);

    auto hasVisibilityModifier = [](TSNode node) {
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            if (QString::fromUtf8(ts_node_type(ts_node_named_child(node, i))) == QStringLiteral("visibility_modifier")) {
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
            const QString type = QString::fromUtf8(ts_node_type(child));
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
        const QString type = QString::fromUtf8(ts_node_type(child));

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
                if (QString::fromUtf8(ts_node_type(variant)) != QStringLiteral("enum_variant")) {
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

            if (!activeKey.isEmpty() && QString::fromUtf8(ts_node_type(node)) == QStringLiteral("call_expression")) {
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
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), dependencies);
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 top-level symbols").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parseObjectiveC(const QString &path, const QString &text, const QString &language) const
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

    QRegularExpression implementationPattern(QStringLiteral(R"(^\s*@implementation\s+([A-Za-z_]\w*)[\s\S]*?^\s*@end\s*$)"),
                                             QRegularExpression::MultilineOption);
    auto implementationIt = implementationPattern.globalMatch(text);
    while (implementationIt.hasNext()) {
        const auto match = implementationIt.next();
        const QString className = match.captured(1);
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        const QString block = match.captured(0);
        QVariantList members;
        QRegularExpression methodPattern(QStringLiteral(R"(^\s*[-+]\s*\([^)]*\)\s*([A-Za-z_]\w*(?::[A-Za-z_]\w*)*:?)?)"),
                                         QRegularExpression::MultilineOption);
        auto methodIt = methodPattern.globalMatch(block);
        while (methodIt.hasNext()) {
            const auto method = methodIt.next();
            const QString methodName = method.captured(1).trimmed();
            if (methodName.isEmpty()) {
                continue;
            }
            const int methodLine = lineNumberAtOffset(text, match.capturedStart(0) + method.capturedStart(0));
            members.append(makeSymbol(QStringLiteral("method"), methodName, methodLine, QString(), {},
                                      snippetFromLine(text, methodLine, 1)));
        }
        appendSymbol(makeSymbol(QStringLiteral("class"), className, line, QString(), members,
                                snippetFromLine(text, line, 3)));
    }

    QRegularExpression functionPattern(
        QStringLiteral(R"(^\s*(?:static\s+|extern\s+)?(?:[\w<>*]+\s+)+([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{)"),
        QRegularExpression::MultilineOption);
    auto functionIt = functionPattern.globalMatch(text);
    while (functionIt.hasNext()) {
        const auto match = functionIt.next();
        const QString name = match.captured(1);
        if (name == QStringLiteral("if") || name == QStringLiteral("while")
            || name == QStringLiteral("for") || name == QStringLiteral("switch")) {
            continue;
        }
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        appendSymbol(makeSymbol(QStringLiteral("function"), name, line, QString(), {},
                                snippetFromLine(text, line, 2)));
    }

    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("dependencies"), extractObjectiveCDependencies(path, text));
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
        QStringLiteral(R"((?:export\s+)?(?:const|let|var)\s+([A-Za-z_]\w*)\s*=\s*\{)"),
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
        QStringLiteral(R"((?:export\s+)?const\s+([A-Za-z_]\w*)\s*=\s*(?:async\s*)?\([^)]*\)\s*=>)"),
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
        QStringLiteral(R"((?:export\s+)?(?:const|let|var)\s+([A-Za-z_]\w*)\s*=\s*(?:async\s*)?function\s*\()"),
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
        QStringLiteral(R"((?:export\s+)?interface\s+([A-Za-z_]\w*))"),
        QRegularExpression::MultilineOption);
    auto interfaces = interfacePattern.globalMatch(text);
    while (interfaces.hasNext()) {
        const auto match = interfaces.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        symbols.append(makePartialScriptSymbol(QStringLiteral("props"), match.captured(1), line, QString(), {},
                                               snippetFromLine(text, line, 1),
                                               QStringLiteral("line_excerpt")));
    }

    QRegularExpression typePattern(
        QStringLiteral(R"((?:export\s+)?type\s+([A-Za-z_]\w*)\s*=)"),
        QRegularExpression::MultilineOption);
    auto types = typePattern.globalMatch(text);
    while (types.hasNext()) {
        const auto match = types.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        symbols.append(makePartialScriptSymbol(QStringLiteral("type"), match.captured(1), line, QString(), {},
                                               snippetFromLine(text, line, 1),
                                               QStringLiteral("line_excerpt")));
    }

    QRegularExpression variablePattern(
        QStringLiteral(R"((?:export\s+)?(?:const|let|var)\s+([A-Za-z_]\w*)\s*=)"),
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
    QVariantList links;
    QDir dir = QFileInfo(path).dir();
    QSet<QString> linkedCssFiles;
    bool hasLocalCssSource = false;

    auto collectLink = [&](const QString &target, const QString &type, int line, const QString &snippet) {
        const bool isLocalTarget = target.startsWith(QStringLiteral("./"))
            || target.startsWith(QStringLiteral("../"))
            || target.startsWith(QLatin1Char('/'))
            || (!target.contains(QStringLiteral("://")) && !target.startsWith(QStringLiteral("//")));
        const QString resolved = isLocalTarget ? QDir::cleanPath(dir.filePath(target)) : QString();
        QVariantMap item = makeSourceContextItem(path, QStringLiteral("html"), line, snippet,
                                                 QStringLiteral("%1 link").arg(type));
        item.insert(QStringLiteral("label"), QFileInfo(target).fileName().isEmpty() ? target : QFileInfo(target).fileName());
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("path"), resolved);
        item.insert(QStringLiteral("targetPath"), resolved);
        item.insert(QStringLiteral("exists"), isLocalTarget ? QFileInfo::exists(resolved) : true);
        links.append(item);
        if (type == QStringLiteral("stylesheet") && isLocalTarget && QFileInfo::exists(resolved)) {
            linkedCssFiles.insert(resolved);
            hasLocalCssSource = true;
        }
    };

    const QStringList scriptTargets = extractHtmlLinkedAssets(text, QStringLiteral("script"));
    for (const QString &target : scriptTargets) {
        const int line = lineNumberAtOffset(text, text.indexOf(target));
        collectLink(target, QStringLiteral("script"), line, snippetFromLine(text, line, 1));
    }

    const QStringList stylesheetTargets = extractHtmlLinkedAssets(text, QStringLiteral("stylesheet"));
    for (const QString &target : stylesheetTargets) {
        const int line = lineNumberAtOffset(text, text.indexOf(target));
        collectLink(target, QStringLiteral("stylesheet"), line, snippetFromLine(text, line, 1));
    }

    const QStringList usedClasses = extractHtmlClasses(text);
    QSet<QString> availableClasses;
    QVariantMap availableClassEntries;

    for (const QString &cssPath : linkedCssFiles) {
        QFile cssFile(cssPath);
        if (!cssFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        if (shouldSkipFileBySize(QFileInfo(cssPath), kMaxAuxiliaryFileBytes)) {
            continue;
        }
        const QString cssText = QString::fromUtf8(cssFile.readAll());
        for (const QString &name : extractCssClasses(cssText)) {
            availableClasses.insert(name);
            if (!availableClassEntries.contains(name)) {
                availableClassEntries.insert(name, findCssClassSummaryEntry(cssPath, cssText, name));
            }
        }
    }

    QRegularExpression siblingCssPattern(QStringLiteral(R"(.*\.css$)"), QRegularExpression::CaseInsensitiveOption);
    const QFileInfoList siblings = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : siblings) {
        if (!siblingCssPattern.match(entry.fileName()).hasMatch()) {
            continue;
        }
        hasLocalCssSource = true;
        QFile cssFile(entry.absoluteFilePath());
        if (!cssFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        if (shouldSkipFileBySize(entry, kMaxAuxiliaryFileBytes)) {
            continue;
        }
        const QString cssText = QString::fromUtf8(cssFile.readAll());
        for (const QString &name : extractCssClasses(cssText)) {
            availableClasses.insert(name);
            if (!availableClassEntries.contains(name)) {
                availableClassEntries.insert(name, findCssClassSummaryEntry(entry.absoluteFilePath(), cssText, name));
            }
        }
    }

    QVariantList matchedClasses;
    QVariantList missingClasses;
    for (const QString &name : usedClasses) {
        if (availableClasses.contains(name)) {
            matchedClasses.append(availableClassEntries.value(name).toMap());
        } else {
            missingClasses.append(makeCssClassSummaryEntry(name, false));
        }
    }

    QVariantMap cssSummary;
    if (hasLocalCssSource && (!usedClasses.isEmpty() || !availableClasses.isEmpty())) {
        cssSummary.insert(QStringLiteral("usedClasses"), toVariantList(usedClasses));
        cssSummary.insert(QStringLiteral("matchedClasses"), matchedClasses);
        cssSummary.insert(QStringLiteral("missingClasses"), missingClasses);
        cssSummary.insert(QStringLiteral("availableClasses"), toVariantList(availableClasses.values()));
    }

    QVariantMap result;
    result.insert(QStringLiteral("path"), path);
    result.insert(QStringLiteral("fileName"), QFileInfo(path).fileName());
    result.insert(QStringLiteral("language"), QStringLiteral("html"));
    result.insert(QStringLiteral("symbols"), QVariantList{});
    result.insert(QStringLiteral("quickLinks"), links);
    result.insert(QStringLiteral("cssSummary"), cssSummary);
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 quick links, %2 classes used")
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
        appendRootMember(makeSymbol(QStringLiteral("method"), match.captured(1), line,
                                    QStringLiteral("signal handler"), {}, snippetFromLine(text, line, 1)));
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
    if (suffix == QStringLiteral("html")) {
        return QStringLiteral("html");
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
    return QStringLiteral("script");
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
            members.append(makeSymbol(QStringLiteral("method"), name, line));
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
        members.append(makeSymbol(QStringLiteral("method"), name, line));
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
        members.append(makeSymbol(QStringLiteral("function"), name, line));
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
        members.append(makeSymbol(QStringLiteral("method"), name, line));
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
    cleaned.remove(QRegularExpression(QStringLiteral(R"(<script\b[^>]*>[\s\S]*?</script>)"),
                                      QRegularExpression::CaseInsensitiveOption));
    cleaned.remove(QRegularExpression(QStringLiteral(R"(<style\b[^>]*>[\s\S]*?</style>)"),
                                      QRegularExpression::CaseInsensitiveOption));

    QRegularExpression htmlClassPattern(QStringLiteral(R"(class\s*=\s*["']([^"']+)["'])"),
                                        QRegularExpression::CaseInsensitiveOption);
    auto htmlIt = htmlClassPattern.globalMatch(cleaned);
    while (htmlIt.hasNext()) {
        const auto match = htmlIt.next();
        const QStringList parts = match.captured(1).split(QRegularExpression(QStringLiteral(R"(\s+)")),
                                                          Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            classes.insert(part.trimmed());
        }
    }

    QStringList values = classes.values();
    values.sort(Qt::CaseInsensitive);
    return values;
}

static QStringList extractHtmlLinkedAssets(const QString &htmlText, const QString &assetType)
{
    QStringList assets;
    QSet<QString> seen;

    const QRegularExpression scriptPattern(QStringLiteral(R"(<script[^>]+src=["']([^"']+)["'])"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression stylePattern(QStringLiteral(R"(<link[^>]+href=["']([^"']+)["'][^>]*rel=["'][^"']*stylesheet[^"']*["'])"),
                                          QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression alternateStylePattern(QStringLiteral(R"(<link[^>]+rel=["'][^"']*stylesheet[^"']*["'][^>]+href=["']([^"']+)["'])"),
                                                   QRegularExpression::CaseInsensitiveOption);

    auto appendUnique = [&](const QString &value) {
        if (!value.isEmpty() && !seen.contains(value)) {
            seen.insert(value);
            assets.append(value);
        }
    };

    if (assetType == QStringLiteral("script")) {
        auto scripts = scriptPattern.globalMatch(htmlText);
        while (scripts.hasNext()) {
            appendUnique(scripts.next().captured(1));
        }
        return assets;
    }

    auto styles = stylePattern.globalMatch(htmlText);
    while (styles.hasNext()) {
        appendUnique(styles.next().captured(1));
    }

    auto alternateStyles = alternateStylePattern.globalMatch(htmlText);
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
    QSet<QString> seenQuickLinkPaths;
    const QStringList extractedClasses = extractCssClasses(text);
    const QSet<QString> cssClassNames(extractedClasses.cbegin(), extractedClasses.cend());
    const QFileInfo cssInfo(path);
    const QString absoluteCssPath = cssInfo.absoluteFilePath();
    const QFileInfoList siblings = cssInfo.dir().entryInfoList({QStringLiteral("*.html"), QStringLiteral("*.htm")},
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
        const QStringList stylesheetTargets = extractHtmlLinkedAssets(htmlText, QStringLiteral("stylesheet"));
        bool referencesThisCss = false;
        for (const QString &target : stylesheetTargets) {
            const bool isLocalTarget = target.startsWith(QStringLiteral("./"))
                || target.startsWith(QStringLiteral("../"))
                || target.startsWith(QLatin1Char('/'))
                || (!target.contains(QStringLiteral("://")) && !target.startsWith(QStringLiteral("//")));
            if (!isLocalTarget) {
                continue;
            }
            const QString resolved = QDir::cleanPath(htmlInfo.dir().filePath(target));
            if (QFileInfo(resolved).absoluteFilePath() == absoluteCssPath) {
                referencesThisCss = true;
                break;
            }
        }

        if (!referencesThisCss) {
            continue;
        }

        const QString htmlPath = htmlInfo.absoluteFilePath();
        if (!seenQuickLinkPaths.contains(htmlPath)) {
            seenQuickLinkPaths.insert(htmlPath);
            int line = 1;
            const int offset = htmlText.indexOf(cssInfo.fileName());
            if (offset >= 0) {
                line = lineNumberAtOffset(htmlText, offset);
            }
            QVariantMap link = makeSourceContextItem(path, QStringLiteral("css"), line,
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

        const QStringList htmlClasses = extractHtmlClasses(htmlText);
        for (const QString &className : htmlClasses) {
            if (!usedClasses.contains(className)) {
                usedClasses.append(className);
            }
            if (cssClassNames.contains(className)) {
                QVariantMap entry = findCssClassSummaryEntry(path, text, className);
                entry.insert(QStringLiteral("language"), QStringLiteral("css"));
                bool exists = false;
                for (const QVariant &existing : std::as_const(matchedClasses)) {
                    if (existing.toMap().value(QStringLiteral("name")).toString() == className) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    matchedClasses.append(entry);
                }
            } else {
                QVariantMap missing = makeCssClassSummaryEntry(className, false, htmlPath, 0,
                                                               QStringLiteral("Used in %1").arg(htmlInfo.fileName()));
                missing.insert(QStringLiteral("language"), QStringLiteral("html"));
                bool exists = false;
                for (const QVariant &existing : std::as_const(missingClasses)) {
                    if (existing.toMap().value(QStringLiteral("name")).toString() == className) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    missingClasses.append(missing);
                }
            }
        }
    }

    QVariantMap cssSummary;
    if (!quickLinks.isEmpty() || !matchedClasses.isEmpty() || !missingClasses.isEmpty()) {
        QStringList availableClasses = extractedClasses;
        availableClasses.sort(Qt::CaseInsensitive);
        usedClasses.sort(Qt::CaseInsensitive);
        cssSummary.insert(QStringLiteral("usedClasses"), toVariantList(usedClasses));
        cssSummary.insert(QStringLiteral("matchedClasses"), matchedClasses);
        cssSummary.insert(QStringLiteral("missingClasses"), missingClasses);
        cssSummary.insert(QStringLiteral("availableClasses"), toVariantList(availableClasses));
    }

    result.insert(QStringLiteral("quickLinks"), quickLinks);
    result.insert(QStringLiteral("cssSummary"), cssSummary);

    const int symbolCount = result.value(QStringLiteral("symbols")).toList().size();
    result.insert(QStringLiteral("summary"),
                  QStringLiteral("%1 selectors and variables, %2 HTML consumers")
                      .arg(symbolCount)
                      .arg(quickLinks.size()));
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
    QSet<QString> classes;

    QRegularExpression cssClassPattern(QStringLiteral(R"(\.([A-Za-z_-][\w-]*))"));
    auto cssIt = cssClassPattern.globalMatch(text);
    while (cssIt.hasNext()) {
        classes.insert(cssIt.next().captured(1));
    }

    QStringList values = classes.values();
    values.sort(Qt::CaseInsensitive);
    return values;
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
        if (seen.contains(type + QLatin1Char('|') + target)) {
            return;
        }
        seen.insert(type + QLatin1Char('|') + target);

        QVariantMap item = makeSourceContextItem(path, language, line, snippetFromLine(text, line, 0),
                                                 QStringLiteral("%1 dependency").arg(type));
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("label"), target);
        if (!bindings.isEmpty()) {
            item.insert(QStringLiteral("bindings"), bindings);
        }

        if (target.startsWith(QStringLiteral("./")) || target.startsWith(QStringLiteral("../"))) {
            QString resolved = QDir::cleanPath(dir.filePath(target));
            QString chosenPath = resolved;
            const QStringList candidates = {
                resolved,
                resolved + QStringLiteral(".js"),
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

QVariantList SymbolParser::extractPythonDependencies(const QString &text)
{
    QVariantList links;
    QSet<QString> seen;
    auto appendDependency = [&](const QString &target, int line) {
        if (target.isEmpty() || seen.contains(target)) {
            return;
        }
        seen.insert(target);
        QVariantMap item;
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), QStringLiteral("import"));
        item.insert(QStringLiteral("line"), line);
        item.insert(QStringLiteral("label"), target);
        item.insert(QStringLiteral("path"), QString());
        item.insert(QStringLiteral("exists"), true);
        item.insert(QStringLiteral("snippet"), snippetFromLine(text, line, 0));
        links.append(item);
    };

    QRegularExpression importPattern(QStringLiteral(R"(^\s*import\s+([A-Za-z0-9_., ]+))"),
                                     QRegularExpression::MultilineOption);
    auto importIt = importPattern.globalMatch(text);
    while (importIt.hasNext()) {
        const auto match = importIt.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        const QStringList names = match.captured(1).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &name : names) {
            appendDependency(name.trimmed().section(QLatin1String(" as "), 0, 0), line);
        }
    }

    QRegularExpression fromPattern(QStringLiteral(R"(^\s*from\s+([A-Za-z0-9_.]+)\s+import\s+)"),
                                   QRegularExpression::MultilineOption);
    auto fromIt = fromPattern.globalMatch(text);
    while (fromIt.hasNext()) {
        const auto match = fromIt.next();
        appendDependency(match.captured(1), lineNumberAtOffset(text, match.capturedStart(0)));
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
        QVariantMap item;
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), QStringLiteral("import"));
        item.insert(QStringLiteral("line"), lineNumberAtOffset(text, match.capturedStart(0)));
        item.insert(QStringLiteral("label"), target);
        item.insert(QStringLiteral("path"), QString());
        item.insert(QStringLiteral("exists"), true);
        item.insert(QStringLiteral("snippet"), snippetFromLine(text, item.value(QStringLiteral("line")).toInt(), 0));
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
        QVariantMap item;
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), QStringLiteral("using"));
        item.insert(QStringLiteral("line"), line);
        item.insert(QStringLiteral("label"), target);
        item.insert(QStringLiteral("path"), QString());
        item.insert(QStringLiteral("exists"), true);
        item.insert(QStringLiteral("snippet"), snippetFromLine(text, line, 0));
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
        QVariantMap item;
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), QStringLiteral("use"));
        item.insert(QStringLiteral("line"), line);
        item.insert(QStringLiteral("label"), rustDependencyLabel(target));
        item.insert(QStringLiteral("path"), QString());
        item.insert(QStringLiteral("exists"), true);
        item.insert(QStringLiteral("snippet"), snippetFromLine(text, line, 0));
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

QVariantList SymbolParser::extractExpressRoutes(const QString &text)
{
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
        QStringLiteral(R"(^\s*@(?:\w+\.)?route\s*\(\s*['"]([^'"]+)['"](?:\s*,\s*methods\s*=\s*\[([^\]]*)\])?)"),
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
        QVariantMap route = makeSourceContextItem(path, QStringLiteral("python"), line, snippetFromLine(text, line, 1),
                                                  QStringLiteral("route"));
        route.insert(QStringLiteral("owner"), QStringLiteral("app"));
        route.insert(QStringLiteral("method"), method);
        route.insert(QStringLiteral("path"), match.captured(1));
        route.insert(QStringLiteral("label"), method + QStringLiteral(" ") + match.captured(1));
        routes.append(route);
    }

    QRegularExpression fastApiPattern(
        QStringLiteral(R"(^\s*@(?:\w+\.)?(get|post|put|patch|delete|options|head)\s*\(\s*['"]([^'"]+)['"])"),
        QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    auto fastApiIt = fastApiPattern.globalMatch(text);
    while (fastApiIt.hasNext()) {
        const auto match = fastApiIt.next();
        const int line = lineNumberAtOffset(text, match.capturedStart(0));
        const QString method = match.captured(1).toUpper();
        QVariantMap route = makeSourceContextItem(path, QStringLiteral("python"), line, snippetFromLine(text, line, 1),
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
