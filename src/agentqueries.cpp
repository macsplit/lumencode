#include "agentqueries.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace AgentQueries {

namespace {

QString relative(const QString &root, const QString &path)
{
    if (root.isEmpty() || path.isEmpty()) {
        return path;
    }
    const QString relativePath = QDir(root).relativeFilePath(path);
    return relativePath.startsWith(QStringLiteral("../")) ? path : relativePath;
}

QString signatureOf(const QVariantMap &symbol)
{
    if (!symbol.contains(QStringLiteral("parameters"))) {
        return QString();
    }
    QStringList parameters;
    for (const QVariant &entry : symbol.value(QStringLiteral("parameters")).toList()) {
        const QVariantMap parameter = entry.toMap();
        QString text = parameter.value(QStringLiteral("text")).toString();
        if (text.isEmpty()) {
            const QString name = parameter.value(QStringLiteral("name")).toString();
            const QString type = parameter.value(QStringLiteral("type")).toString();
            text = type.isEmpty() ? name : name.isEmpty() ? type : name + QStringLiteral(": ") + type;
        }
        if (parameter.contains(QStringLiteral("default"))) {
            text += QStringLiteral(" = ") + parameter.value(QStringLiteral("default")).toString();
        }
        parameters.append(text);
    }
    QStringList returns;
    for (const QVariant &entry : symbol.value(QStringLiteral("returns")).toList()) {
        const QString text = entry.toMap().value(QStringLiteral("text")).toString();
        if (!text.isEmpty() && !text.contains(QStringLiteral("(line "))) {
            returns.append(text);
        }
    }
    QString signature = QLatin1Char('(') + parameters.join(QStringLiteral(", ")) + QLatin1Char(')');
    if (!returns.isEmpty()) {
        signature += QStringLiteral(" -> ") + returns.first();
    }
    return signature;
}

QStringList relationNames(const QVariantList &relations, const QString &filePath, const QString &root)
{
    QStringList names;
    for (const QVariant &entry : relations) {
        const QVariantMap relation = entry.toMap();
        const QString name = relation.value(QStringLiteral("name")).toString();
        const QString path = relation.value(QStringLiteral("path")).toString();
        if (!path.isEmpty() && QFileInfo(path).absoluteFilePath() != QFileInfo(filePath).absoluteFilePath()) {
            names.append(QStringLiteral("%1 @ %2:%3").arg(name, relative(root, path), relation.value(QStringLiteral("line")).toString()));
        } else {
            names.append(name);
        }
    }
    names.removeDuplicates();
    return names;
}

QVariantList outlineSymbols(const QVariantList &symbols, const QString &filePath, const QString &root, int depth)
{
    QVariantList out;
    for (const QVariant &entry : symbols) {
        const QVariantMap symbol = entry.toMap();
        QVariantMap item{{QStringLiteral("kind"), symbol.value(QStringLiteral("kind"))},
                         {QStringLiteral("name"), symbol.value(QStringLiteral("name"))},
                         {QStringLiteral("line"), symbol.value(QStringLiteral("line"))}};
        if (symbol.value(QStringLiteral("endLine")).toInt() > symbol.value(QStringLiteral("line")).toInt()) {
            item.insert(QStringLiteral("endLine"), symbol.value(QStringLiteral("endLine")));
        }
        const QString signature = signatureOf(symbol);
        if (!signature.isEmpty()) {
            item.insert(QStringLiteral("signature"), signature);
        }
        const QString detail = symbol.value(QStringLiteral("detail")).toString();
        if (!detail.isEmpty() && detail.size() <= 80) {
            item.insert(QStringLiteral("detail"), detail);
        }
        const QStringList calls = relationNames(symbol.value(QStringLiteral("calls")).toList(), filePath, root);
        if (!calls.isEmpty()) {
            item.insert(QStringLiteral("calls"), calls);
        }
        const QStringList calledBy = relationNames(symbol.value(QStringLiteral("calledBy")).toList(), filePath, root);
        if (!calledBy.isEmpty()) {
            item.insert(QStringLiteral("calledBy"), calledBy);
        }
        if (symbol.contains(QStringLiteral("calledByTotal"))) {
            item.insert(QStringLiteral("calledByTotal"), symbol.value(QStringLiteral("calledByTotal")));
        }
        const QString confidence = symbol.value(QStringLiteral("confidence")).toString();
        if (!confidence.isEmpty() && confidence != QStringLiteral("high")) {
            item.insert(QStringLiteral("confidence"), confidence);
        }
        if (depth < 3) {
            const QVariantList members = outlineSymbols(symbol.value(QStringLiteral("members")).toList(), filePath, root, depth + 1);
            if (!members.isEmpty()) {
                item.insert(QStringLiteral("members"), members);
            }
        }
        out.append(item);
    }
    return out;
}

void appendText(QStringList &lines, const QVariantList &symbols, int depth)
{
    for (const QVariant &entry : symbols) {
        const QVariantMap symbol = entry.toMap();
        QString line = QString(depth * 2, QLatin1Char(' ')) + symbol.value(QStringLiteral("kind")).toString() + QLatin1Char(' ')
            + symbol.value(QStringLiteral("name")).toString() + symbol.value(QStringLiteral("signature")).toString()
            + QStringLiteral("  L") + symbol.value(QStringLiteral("line")).toString();
        if (symbol.contains(QStringLiteral("endLine"))) {
            line += QLatin1Char('-') + symbol.value(QStringLiteral("endLine")).toString();
        }
        if (symbol.contains(QStringLiteral("detail"))) {
            line += QStringLiteral("  [") + symbol.value(QStringLiteral("detail")).toString() + QLatin1Char(']');
        }
        lines.append(line);
        const QString indent(depth * 2 + 4, QLatin1Char(' '));
        if (symbol.contains(QStringLiteral("calls"))) {
            lines.append(indent + QStringLiteral("calls: ") + symbol.value(QStringLiteral("calls")).toStringList().join(QStringLiteral(", ")));
        }
        if (symbol.contains(QStringLiteral("calledBy"))) {
            QString callers = symbol.value(QStringLiteral("calledBy")).toStringList().join(QStringLiteral(", "));
            if (symbol.contains(QStringLiteral("calledByTotal"))) {
                callers += QStringLiteral(" (%1 in total)").arg(symbol.value(QStringLiteral("calledByTotal")).toInt());
            }
            lines.append(indent + QStringLiteral("called by: ") + callers);
        }
        appendText(lines, symbol.value(QStringLiteral("members")).toList(), depth + 1);
    }
}

// The symbol of `analysis` matching a definition (kind, name, line).
QVariantMap findSymbol(const QVariantList &symbols, const ProjectIndex::Definition &definition)
{
    for (const QVariant &entry : symbols) {
        const QVariantMap symbol = entry.toMap();
        if (symbol.value(QStringLiteral("name")).toString() == definition.name
            && symbol.value(QStringLiteral("line")).toInt() == definition.line) {
            return symbol;
        }
        const QVariantMap nested = findSymbol(symbol.value(QStringLiteral("members")).toList(), definition);
        if (!nested.isEmpty()) {
            return nested;
        }
    }
    return {};
}

} // namespace

QVariantMap outline(const QVariantMap &analysis, const QString &root)
{
    const QString path = analysis.value(QStringLiteral("path")).toString();
    QVariantMap out{{QStringLiteral("path"), relative(root, path)},
                    {QStringLiteral("language"), analysis.value(QStringLiteral("language"))},
                    {QStringLiteral("summary"), analysis.value(QStringLiteral("summary"))},
                    {QStringLiteral("mode"), analysis.value(QStringLiteral("analysisSourceMode"))}};
    if (analysis.contains(QStringLiteral("analysisDamagedLines"))) {
        out.insert(QStringLiteral("damagedLines"), analysis.value(QStringLiteral("analysisDamagedLines")));
    }
    out.insert(QStringLiteral("symbols"), outlineSymbols(analysis.value(QStringLiteral("symbols")).toList(), path, root, 0));
    QStringList imports;
    for (const QVariant &entry : analysis.value(QStringLiteral("dependencies")).toList()) {
        const QVariantMap dependency = entry.toMap();
        const QString resolved = dependency.value(QStringLiteral("path")).toString();
        imports.append(resolved.isEmpty() ? dependency.value(QStringLiteral("target")).toString()
                                          : dependency.value(QStringLiteral("target")).toString() + QStringLiteral(" -> ") + relative(root, resolved));
    }
    if (!imports.isEmpty()) {
        out.insert(QStringLiteral("imports"), imports);
    }
    QStringList routes;
    for (const QVariant &entry : analysis.value(QStringLiteral("routes")).toList()) {
        const QVariantMap route = entry.toMap();
        QString text = route.value(QStringLiteral("label")).toString();
        if (text.isEmpty()) {
            text = route.value(QStringLiteral("method")).toString() + QLatin1Char(' ') + route.value(QStringLiteral("path")).toString();
        }
        text += QStringLiteral("  L") + route.value(QStringLiteral("line")).toString();
        const QStringList clients = relationNames(route.value(QStringLiteral("calledFrom")).toList(), path, root);
        if (!clients.isEmpty()) {
            text += QStringLiteral("  <- ") + clients.join(QStringLiteral(", "));
        }
        routes.append(text);
    }
    if (!routes.isEmpty()) {
        out.insert(QStringLiteral("routes"), routes);
    }
    QStringList httpCalls;
    for (const QVariant &entry : analysis.value(QStringLiteral("httpCalls")).toList()) {
        const QVariantMap call = entry.toMap();
        QString text = call.value(QStringLiteral("method")).toString() + QLatin1Char(' ') + call.value(QStringLiteral("url")).toString()
            + QStringLiteral("  L") + call.value(QStringLiteral("line")).toString();
        const QStringList targets = relationNames(call.value(QStringLiteral("routes")).toList(), path, root);
        if (!targets.isEmpty()) {
            text += QStringLiteral("  -> ") + targets.join(QStringLiteral(", "));
        }
        httpCalls.append(text);
    }
    if (!httpCalls.isEmpty()) {
        out.insert(QStringLiteral("httpCalls"), httpCalls);
    }
    return out;
}

QString outlineText(const QVariantMap &outline)
{
    QStringList lines;
    lines.append(QStringLiteral("%1 (%2, %3): %4")
                     .arg(outline.value(QStringLiteral("path")).toString(), outline.value(QStringLiteral("language")).toString(),
                          outline.value(QStringLiteral("mode")).toString(), outline.value(QStringLiteral("summary")).toString()));
    if (outline.contains(QStringLiteral("damagedLines"))) {
        QStringList damaged;
        for (const QVariant &line : outline.value(QStringLiteral("damagedLines")).toList()) {
            damaged.append(line.toString());
        }
        lines.append(QStringLiteral("damaged lines (repaired around): ") + damaged.join(QStringLiteral(", ")));
    }
    appendText(lines, outline.value(QStringLiteral("symbols")).toList(), 0);
    for (const QString &section : {QStringLiteral("imports"), QStringLiteral("routes"), QStringLiteral("httpCalls")}) {
        const QStringList entries = outline.value(section).toStringList();
        if (!entries.isEmpty()) {
            lines.append(section + QLatin1Char(':'));
            for (const QString &entry : entries) {
                lines.append(QStringLiteral("  ") + entry);
            }
        }
    }
    return lines.join(QLatin1Char('\n'));
}

QVariantList findDefinitions(const ProjectIndex::SnapshotPtr &snapshot, const QString &name)
{
    QVariantList out;
    if (!snapshot) {
        return out;
    }
    const QString owner = name.contains(QLatin1Char('.')) ? name.section(QLatin1Char('.'), 0, -2) : QString();
    const QString member = name.section(QLatin1Char('.'), -1);
    const auto range = snapshot->definitionsByName.equal_range(member);
    for (auto it = range.first; it != range.second; ++it) {
        const ProjectIndex::Definition &definition = snapshot->allDefinitions.at(it.value());
        if (!owner.isEmpty() && definition.owner != owner) {
            continue;
        }
        QVariantMap item{{QStringLiteral("name"), definition.owner.isEmpty() ? definition.name : definition.owner + QLatin1Char('.') + definition.name},
                         {QStringLiteral("kind"), definition.kind},
                         {QStringLiteral("path"), relative(snapshot->root, definition.path)},
                         {QStringLiteral("line"), definition.line},
                         {QStringLiteral("language"), definition.language}};
        if (definition.declaration) {
            item.insert(QStringLiteral("declaration"), true);
        }
        out.append(item);
    }
    std::sort(out.begin(), out.end(), [](const QVariant &left, const QVariant &right) {
        const QVariantMap a = left.toMap();
        const QVariantMap b = right.toMap();
        return a.value(QStringLiteral("path")).toString() != b.value(QStringLiteral("path")).toString()
            ? a.value(QStringLiteral("path")).toString() < b.value(QStringLiteral("path")).toString()
            : a.value(QStringLiteral("line")).toInt() < b.value(QStringLiteral("line")).toInt();
    });
    return out;
}

QVariantList relationsOf(const ProjectIndex::SnapshotPtr &snapshot, const QString &name, const QString &field,
                         const std::function<QVariantMap(const QString &)> &analyse)
{
    QVariantList out;
    if (!snapshot) {
        return out;
    }
    const QString owner = name.contains(QLatin1Char('.')) ? name.section(QLatin1Char('.'), 0, -2) : QString();
    const QString member = name.section(QLatin1Char('.'), -1);
    QHash<QString, QVariantMap> analyses;
    const auto range = snapshot->definitionsByName.equal_range(member);
    for (auto it = range.first; it != range.second; ++it) {
        const ProjectIndex::Definition &definition = snapshot->allDefinitions.at(it.value());
        if (!owner.isEmpty() && definition.owner != owner) {
            continue;
        }
        if (!analyses.contains(definition.path)) {
            analyses.insert(definition.path, ProjectIndex::augmentAnalysis(analyse(definition.path), snapshot));
        }
        const QVariantMap symbol = findSymbol(analyses.value(definition.path).value(QStringLiteral("symbols")).toList(), definition);
        QVariantList relations;
        for (const QVariant &entry : symbol.value(field).toList()) {
            const QVariantMap relation = entry.toMap();
            QString relationPath = relation.value(QStringLiteral("path")).toString();
            if (relationPath.isEmpty()) {
                relationPath = definition.path;
            }
            relations.append(QVariantMap{{QStringLiteral("name"), relation.value(QStringLiteral("name"))},
                                         {QStringLiteral("kind"), relation.value(QStringLiteral("kind"))},
                                         {QStringLiteral("path"), relative(snapshot->root, relationPath)},
                                         {QStringLiteral("line"), relation.value(QStringLiteral("line"))},
                                         {QStringLiteral("confidence"), relation.value(QStringLiteral("confidence"))}});
        }
        QVariantMap item{{QStringLiteral("definition"), QStringLiteral("%1 %2 @ %3:%4")
                                                            .arg(definition.kind,
                                                                 definition.owner.isEmpty() ? definition.name : definition.owner + QLatin1Char('.') + definition.name,
                                                                 relative(snapshot->root, definition.path))
                                                            .arg(definition.line)},
                         {field, relations}};
        if (symbol.contains(QStringLiteral("calledByTotal")) && field == QStringLiteral("calledBy")) {
            item.insert(QStringLiteral("calledByTotal"), symbol.value(QStringLiteral("calledByTotal")));
        }
        out.append(item);
    }
    return out;
}

QVariantList routes(const ProjectIndex::SnapshotPtr &snapshot)
{
    QVariantList out;
    if (!snapshot) {
        return out;
    }
    for (auto it = snapshot->files.cbegin(); it != snapshot->files.cend(); ++it) {
        for (const QVariant &entry : it->routes) {
            const QVariantMap route = entry.toMap();
            const QString method = route.value(QStringLiteral("method")).toString();
            if (method == QStringLiteral("USE")) {
                continue;
            }
            const int line = route.value(QStringLiteral("line")).toInt();
            QVariantList clients;
            for (int edgeIndex : snapshot->httpIncomingByRoute.value(it->path + QLatin1Char('|') + QString::number(line))) {
                const ProjectIndex::Edge &edge = snapshot->httpEdges.at(edgeIndex);
                clients.append(QStringLiteral("%1 @ %2:%3")
                                   .arg(edge.fromName.isEmpty() ? QStringLiteral("(top level)") : edge.fromName,
                                        relative(snapshot->root, edge.fromPath))
                                   .arg(edge.siteLine));
            }
            QVariantMap item{{QStringLiteral("route"), method + QLatin1Char(' ') + route.value(QStringLiteral("path")).toString()},
                             {QStringLiteral("path"), relative(snapshot->root, it->path)},
                             {QStringLiteral("line"), line}};
            const QStringList prefixes = snapshot->mountPrefixes.value(it->path);
            if (!prefixes.isEmpty()) {
                item.insert(QStringLiteral("mountedUnder"), prefixes);
            }
            if (!clients.isEmpty()) {
                item.insert(QStringLiteral("calledFrom"), clients);
            }
            out.append(item);
        }
    }
    std::sort(out.begin(), out.end(), [](const QVariant &left, const QVariant &right) {
        const QVariantMap a = left.toMap();
        const QVariantMap b = right.toMap();
        return a.value(QStringLiteral("path")).toString() != b.value(QStringLiteral("path")).toString()
            ? a.value(QStringLiteral("path")).toString() < b.value(QStringLiteral("path")).toString()
            : a.value(QStringLiteral("line")).toInt() < b.value(QStringLiteral("line")).toInt();
    });
    return out;
}

} // namespace AgentQueries
