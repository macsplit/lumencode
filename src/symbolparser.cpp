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
const TSLanguage *tree_sitter_javascript(void);
const TSLanguage *tree_sitter_typescript(void);
const TSLanguage *tree_sitter_tsx(void);
const TSLanguage *tree_sitter_php(void);
const TSLanguage *tree_sitter_css(void);
}

static QVariantList toVariantList(const QStringList &values)
{
    QVariantList result;
    for (const QString &value : values) {
        result.append(value);
    }
    return result;
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
    if (language == QStringLiteral("css")) {
        return const_cast<TSLanguage *>(tree_sitter_css());
    }
    if (language == QStringLiteral("tsx")) {
        return const_cast<TSLanguage *>(tree_sitter_tsx());
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

SymbolParser::SymbolParser(QObject *parent)
    : QObject(parent)
{
}

QVariantMap SymbolParser::parseFile(const QString &path) const
{
    QFile file(path);
    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), QString());

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.insert(QStringLiteral("summary"), QStringLiteral("Unable to read file"));
        return result;
    }

    const QString text = QString::fromUtf8(file.readAll());
    const QString language = detectLanguage(path);
    result.insert(QStringLiteral("language"), language);

    if (language == QStringLiteral("php")) {
        const QVariantMap treeSitterResult = parsePhpTreeSitter(path, text);
        if (!treeSitterResult.value(QStringLiteral("symbols")).toList().isEmpty()) {
            return treeSitterResult;
        }
        return parsePhp(path, text);
    }
    if (language == QStringLiteral("html")) {
        return parseHtml(path, text);
    }
    if (language == QStringLiteral("css")) {
        const QVariantMap treeSitterResult = parseCssTreeSitter(path, text);
        if (!treeSitterResult.value(QStringLiteral("symbols")).toList().isEmpty()) {
            return treeSitterResult;
        }
        return parseCss(path, text);
    }
    if (language == QStringLiteral("jsx") || language == QStringLiteral("tsx")
        || language == QStringLiteral("ts") || language == QStringLiteral("script")) {
        const QVariantMap treeSitterResult = parseScriptLikeTreeSitter(path, text, language);
        if (!treeSitterResult.value(QStringLiteral("symbols")).toList().isEmpty()) {
            return treeSitterResult;
        }
        if (language == QStringLiteral("jsx") || language == QStringLiteral("tsx")) {
            return parseScriptLike(path, text, true);
        }
        return parseScriptLike(path, text, false);
    }
    if (language == QStringLiteral("json")) {
        return parseJson(path, text);
    }
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

    auto appendDependency = [&](const QString &target, const QString &type, int line) {
        if (seenDependencies.contains(type + QLatin1Char('|') + target)) {
            return;
        }
        seenDependencies.insert(type + QLatin1Char('|') + target);

        QVariantMap item;
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("line"), line);
        item.insert(QStringLiteral("label"), target);

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
            appendDependency(target, QStringLiteral("import"), nodeLine(originalNode));
            continue;
        }

        if (originalType == QStringLiteral("export_statement")) {
            const TSNode sourceNode = fieldNode(originalNode, "source");
            if (!ts_node_is_null(sourceNode)) {
                const QString target = nodeValueText(sourceNode, source);
                appendDependency(target, QStringLiteral("export"), nodeLine(originalNode));
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
                                appendDependency(target, QStringLiteral("require"), nodeLine(valueNode));
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
                        appendDependency(target, QStringLiteral("require"), nodeLine(expression));
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
                            QVariantMap route;
                            route.insert(QStringLiteral("owner"), functionName.section(QLatin1Char('.'), 0, 0));
                            route.insert(QStringLiteral("method"), method);
                            route.insert(QStringLiteral("path"), routePath);
                            route.insert(QStringLiteral("line"), nodeLine(expression));
                            route.insert(QStringLiteral("label"), method + QStringLiteral(" ") + routePath);
                            routes.append(route);
                        }
                    }
                }
            }
        }
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    QVariantMap result = makeResultSkeleton(path, QFileInfo(path).fileName(), language);
    result.insert(QStringLiteral("symbols"), symbols);
    
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
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 selectors and variables").arg(symbols.size()));
    return result;
}

QVariantMap SymbolParser::parsePhp(const QString &path, const QString &text) const
{
    QVariantList symbols;
    QRegularExpression classPattern(
        QStringLiteral(R"(((?:abstract\s+|final\s+)?(?:class|trait|interface))\s+([A-Za-z_]\w*)[\s\S]*?\{)"),
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
        symbols.append(makeSymbol(kind, name, line));
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
        symbols.append(makeSymbol(QStringLiteral("class"), name, line, QString(), parseClassMembers(body, QStringLiteral("js"))));
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
        symbols.append(makeSymbol(kind, name, line, QStringLiteral("object export"), members));
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
        symbols.append(makeSymbol(QStringLiteral("module"), QStringLiteral("module.exports"), line,
                                  QStringLiteral("CommonJS export object"), parseObjectMembers(body)));
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
        symbols.append(makeSymbol(kind, name, line));
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
        symbols.append(makeSymbol(kind, name, line, QStringLiteral("function expression")));
    }

    QRegularExpression interfacePattern(
        QStringLiteral(R"((?:export\s+)?interface\s+([A-Za-z_]\w*))"),
        QRegularExpression::MultilineOption);
    auto interfaces = interfacePattern.globalMatch(text);
    while (interfaces.hasNext()) {
        const auto match = interfaces.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        symbols.append(makeSymbol(QStringLiteral("props"), match.captured(1), line));
    }

    QRegularExpression typePattern(
        QStringLiteral(R"((?:export\s+)?type\s+([A-Za-z_]\w*)\s*=)"),
        QRegularExpression::MultilineOption);
    auto types = typePattern.globalMatch(text);
    while (types.hasNext()) {
        const auto match = types.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        symbols.append(makeSymbol(QStringLiteral("type"), match.captured(1), line));
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
        symbols.append(makeSymbol(QStringLiteral("variable"), name, line));
    }

    QVariantMap result;
    result.insert(QStringLiteral("path"), path);
    result.insert(QStringLiteral("fileName"), QFileInfo(path).fileName());
    result.insert(QStringLiteral("language"), reactMode ? QFileInfo(path).suffix().toLower() : QStringLiteral("script"));
    result.insert(QStringLiteral("symbols"), symbols);
    result.insert(QStringLiteral("quickLinks"), QVariantList{});
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

    QRegularExpression scriptPattern(QStringLiteral(R"(<script[^>]+src=["']([^"']+)["'])"),
                                     QRegularExpression::CaseInsensitiveOption);
    QRegularExpression stylePattern(QStringLiteral(R"(<link[^>]+href=["']([^"']+)["'][^>]*rel=["'][^"']*stylesheet[^"']*["'])"),
                                    QRegularExpression::CaseInsensitiveOption);
    QRegularExpression alternateStylePattern(QStringLiteral(R"(<link[^>]+rel=["'][^"']*stylesheet[^"']*["'][^>]+href=["']([^"']+)["'])"),
                                             QRegularExpression::CaseInsensitiveOption);
    QSet<QString> linkedCssFiles;
    bool hasLocalCssSource = false;

    auto collectLink = [&](const QString &target, const QString &type) {
        const bool isLocalTarget = target.startsWith(QStringLiteral("./"))
            || target.startsWith(QStringLiteral("../"))
            || target.startsWith(QLatin1Char('/'))
            || (!target.contains(QStringLiteral("://")) && !target.startsWith(QStringLiteral("//")));
        const QString resolved = isLocalTarget ? QDir::cleanPath(dir.filePath(target)) : QString();
        QVariantMap item;
        item.insert(QStringLiteral("label"), QFileInfo(target).fileName().isEmpty() ? target : QFileInfo(target).fileName());
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("path"), resolved);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("exists"), isLocalTarget ? QFileInfo::exists(resolved) : true);
        links.append(item);
        if (type == QStringLiteral("stylesheet") && isLocalTarget && QFileInfo::exists(resolved)) {
            linkedCssFiles.insert(resolved);
            hasLocalCssSource = true;
        }
    };

    auto scripts = scriptPattern.globalMatch(text);
    while (scripts.hasNext()) {
        collectLink(scripts.next().captured(1), QStringLiteral("script"));
    }

    auto styles = stylePattern.globalMatch(text);
    while (styles.hasNext()) {
        collectLink(styles.next().captured(1), QStringLiteral("stylesheet"));
    }
    auto styles2 = alternateStylePattern.globalMatch(text);
    while (styles2.hasNext()) {
        collectLink(styles2.next().captured(1), QStringLiteral("stylesheet"));
    }

    const QStringList usedClasses = extractHtmlClasses(text);
    QSet<QString> availableClasses;
    QVariantMap availableClassEntries;

    for (const QString &cssPath : linkedCssFiles) {
        QFile cssFile(cssPath);
        if (!cssFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
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
    result.insert(QStringLiteral("quickLinks"), QVariantList{});
    result.insert(QStringLiteral("dependencies"), QVariantList{});
    result.insert(QStringLiteral("routes"), QVariantList{});
    result.insert(QStringLiteral("relatedFiles"), findRelatedFiles(path));
    result.insert(QStringLiteral("packageSummary"), QVariantMap{});
    result.insert(QStringLiteral("cssSummary"), QVariantMap{});
    result.insert(QStringLiteral("summary"), QStringLiteral("%1 selectors and variables").arg(symbols.size()));
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
            QStringLiteral(R"((?:async\s+)?([A-Za-z_]\w*)\s*\([^)]*\)\s*\{)"),
            QRegularExpression::MultilineOption);
        auto methods = methodPattern.globalMatch(body);
        while (methods.hasNext()) {
            const auto match = methods.next();
            const int line = body.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
            members.append(makeSymbol(QStringLiteral("method"), match.captured(1), line));
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
        QStringLiteral(R"((?:async\s+)?([A-Za-z_]\w*)\s*\([^)]*\)\s*\{)"),
        QRegularExpression::MultilineOption);
    auto methods = methodPattern.globalMatch(body);
    while (methods.hasNext()) {
        const auto match = methods.next();
        const int line = body.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        members.append(makeSymbol(QStringLiteral("method"), match.captured(1), line));
    }

    QRegularExpression arrowPattern(
        QStringLiteral(R"(([A-Za-z_]\w*)\s*:\s*(?:async\s*)?\([^)]*\)\s*=>)"),
        QRegularExpression::MultilineOption);
    auto arrows = arrowPattern.globalMatch(body);
    while (arrows.hasNext()) {
        const auto match = arrows.next();
        const int line = body.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        members.append(makeSymbol(QStringLiteral("function"), match.captured(1), line));
    }

    QRegularExpression propertyPattern(
        QStringLiteral(R"(([A-Za-z_]\w*)\s*:)"),
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

    auto appendLink = [&](const QString &target, const QString &type, int line) {
        if (seen.contains(type + QLatin1Char('|') + target)) {
            return;
        }
        seen.insert(type + QLatin1Char('|') + target);

        QVariantMap item;
        item.insert(QStringLiteral("target"), target);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("line"), line);
        item.insert(QStringLiteral("label"), target);

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

        links.append(item);
    };

    QRegularExpression requirePattern(QStringLiteral(R"(require\s*\(\s*['"]([^'"]+)['"]\s*\))"));
    auto requireMatches = requirePattern.globalMatch(text);
    while (requireMatches.hasNext()) {
        const auto match = requireMatches.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        appendLink(match.captured(1), QStringLiteral("require"), line);
    }

    QRegularExpression importPattern(QStringLiteral(R"(import\s+[\s\S]*?\sfrom\s+['"]([^'"]+)['"])"));
    auto imports = importPattern.globalMatch(text);
    while (imports.hasNext()) {
        const auto match = imports.next();
        const int line = text.left(match.capturedStart(0)).count(QLatin1Char('\n')) + 1;
        appendLink(match.captured(1), QStringLiteral("import"), line);
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
        route.insert(QStringLiteral("label"), match.captured(2).toUpper() + QStringLiteral(" ") + match.captured(3));
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
