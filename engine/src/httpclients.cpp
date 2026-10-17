#include "httpclients.h"

#include <QRegularExpression>
#include <QSet>
#include <QVariantList>

#include <algorithm>
#include <functional>

namespace HttpClients {

namespace {

int lineAt(const QString &text, int offset)
{
    return text.left(offset).count(QLatin1Char('\n')) + 1;
}

// The balanced (...) or {...} starting at `open`, clipped to `limit` chars.
QString balancedFrom(const QString &text, int open, int limit = 1200)
{
    if (open < 0 || open >= text.size()) {
        return QString();
    }
    const QChar openChar = text.at(open);
    const QChar closeChar = openChar == QLatin1Char('(') ? QLatin1Char(')') : QLatin1Char('}');
    int depth = 0;
    QChar quote;
    for (int index = open; index < text.size() && index < open + limit; ++index) {
        const QChar ch = text.at(index);
        if (!quote.isNull()) {
            if (ch == QLatin1Char('\\')) {
                ++index;
            } else if (ch == quote) {
                quote = QChar();
            }
            continue;
        }
        if (ch == QLatin1Char('\'') || ch == QLatin1Char('"') || ch == QLatin1Char('`')) {
            quote = ch;
        } else if (ch == openChar) {
            ++depth;
        } else if (ch == closeChar && --depth == 0) {
            return text.mid(open, index - open + 1);
        }
    }
    return text.mid(open, limit);
}

// A URL expression's static shape: 'a/' + id -> "a/*", `a/${id}/b` -> "a/*/b".
// Returns an empty string when the expression starts with no literal.
QString urlFromExpression(const QString &expression)
{
    const QString trimmed = expression.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    const QChar quote = trimmed.at(0);
    if (quote != QLatin1Char('\'') && quote != QLatin1Char('"') && quote != QLatin1Char('`')) {
        return QString();
    }
    QString url;
    int index = 1;
    for (; index < trimmed.size(); ++index) {
        const QChar ch = trimmed.at(index);
        if (ch == QLatin1Char('\\') && index + 1 < trimmed.size()) {
            url += trimmed.at(++index);
        } else if (ch == quote) {
            break;
        } else if (quote == QLatin1Char('`') && ch == QLatin1Char('$') && index + 1 < trimmed.size()
                   && trimmed.at(index + 1) == QLatin1Char('{')) {
            int depth = 0;
            for (; index < trimmed.size(); ++index) {
                if (trimmed.at(index) == QLatin1Char('{')) {
                    ++depth;
                } else if (trimmed.at(index) == QLatin1Char('}') && --depth == 0) {
                    break;
                }
            }
            url += QStringLiteral("*");
        } else {
            url += ch;
        }
    }
    // Concatenation after the literal: the rest is dynamic.
    const QString rest = trimmed.mid(index + 1).trimmed();
    if (rest.startsWith(QLatin1Char('+'))) {
        url += QStringLiteral("*");
    }
    return url;
}

QString firstArgument(const QString &call)
{
    // call is "(...)" ; take text up to the first top-level comma.
    int depth = 0;
    QChar quote;
    for (int index = 1; index < call.size(); ++index) {
        const QChar ch = call.at(index);
        if (!quote.isNull()) {
            if (ch == QLatin1Char('\\')) {
                ++index;
            } else if (ch == quote) {
                quote = QChar();
            }
            continue;
        }
        if (ch == QLatin1Char('\'') || ch == QLatin1Char('"') || ch == QLatin1Char('`')) {
            quote = ch;
        } else if (ch == QLatin1Char('(') || ch == QLatin1Char('[') || ch == QLatin1Char('{')) {
            ++depth;
        } else if (ch == QLatin1Char(')') || ch == QLatin1Char(']') || ch == QLatin1Char('}')) {
            if (depth == 0) {
                return call.mid(1, index - 1);
            }
            --depth;
        } else if (ch == QLatin1Char(',') && depth == 0) {
            return call.mid(1, index - 1);
        }
    }
    return call.mid(1);
}

QString optionValue(const QString &options, const QString &keys)
{
    const QRegularExpression pattern(QStringLiteral(R"(\b(?:%1)\s*:\s*)").arg(keys));
    const auto match = pattern.match(options);
    if (!match.hasMatch()) {
        return QString();
    }
    return options.mid(match.capturedEnd());
}

struct ClientCall
{
    QString method;
    QString url;
    QString via;
    int line = 0;
};

void scanScript(const QString &text, int baseOffset, const QString &fullText, QVector<ClientCall> &calls)
{
    auto add = [&](const QString &method, const QString &url, const QString &via, int offset) {
        if (url.isEmpty() || url.startsWith(QStringLiteral("data:")) || url.startsWith(QStringLiteral("blob:"))) {
            return;
        }
        calls.append({method.toUpper(), url, via, lineAt(fullText, baseOffset + offset)});
    };

    static const QRegularExpression fetchCall(QStringLiteral(R"((?<![\w.$])fetch\s*\()"));
    auto it = fetchCall.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString call = balancedFrom(text, match.capturedEnd() - 1);
        const QString url = urlFromExpression(firstArgument(call));
        QString method = QStringLiteral("GET");
        static const QRegularExpression methodOption(QStringLiteral(R"(\bmethod\s*:\s*['"`](\w+)['"`])"));
        const auto methodMatch = methodOption.match(call);
        if (methodMatch.hasMatch()) {
            method = methodMatch.captured(1);
        }
        add(method, url, QStringLiteral("fetch"), match.capturedStart());
    }

    // axios.get(url) / this.http.get<T>(url) / http.post(url, body) / $.get(url)
    static const QRegularExpression verbCall(QStringLiteral(
        R"((?<![\w$])(axios|\$|jQuery|(?:this\.)?(?:http|httpClient|\$http|api|client))\s*\.\s*(get|post|put|patch|delete|head|getJSON)\s*(?:<[^>()]*>)?\s*\()"));
    it = verbCall.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString receiver = match.captured(1);
        QString verb = match.captured(2);
        if (verb == QStringLiteral("getJSON")) {
            verb = QStringLiteral("GET");
        }
        const QString call = balancedFrom(text, match.capturedEnd() - 1);
        const QString url = urlFromExpression(firstArgument(call));
        // Server-side route definitions look the same (app.get('/x', handler)):
        // only known client receivers are accepted, and api/client only with
        // an absolute path.
        if ((receiver.endsWith(QStringLiteral("api")) || receiver.endsWith(QStringLiteral("client")))
            && !url.startsWith(QLatin1Char('/'))) {
            continue;
        }
        add(verb, url, receiver == QStringLiteral("$") || receiver == QStringLiteral("jQuery") ? QStringLiteral("jquery")
                                                                                              : QStringLiteral("http client"),
            match.capturedStart());
    }

    // axios({ url, method }) / $.ajax({ url, type }) / $.ajax(url, { ... })
    static const QRegularExpression configCall(QStringLiteral(R"((?<![\w$.])(axios|\$\.ajax|jQuery\.ajax)\s*\()"));
    it = configCall.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString call = balancedFrom(text, match.capturedEnd() - 1);
        QString url = urlFromExpression(firstArgument(call));
        if (url.isEmpty()) {
            url = urlFromExpression(optionValue(call, QStringLiteral("url")));
        }
        QString method = QStringLiteral("GET");
        static const QRegularExpression methodOption(QStringLiteral(R"(\b(?:method|type)\s*:\s*['"`](\w+)['"`])"));
        const auto methodMatch = methodOption.match(call);
        if (methodMatch.hasMatch()) {
            method = methodMatch.captured(1);
        }
        add(method, url, match.captured(1).contains(QStringLiteral("ajax")) ? QStringLiteral("jquery") : QStringLiteral("axios"),
            match.capturedStart());
    }

    // xhr.open('POST', url)
    static const QRegularExpression xhrOpen(QStringLiteral(R"(\.open\s*\(\s*['"](GET|POST|PUT|PATCH|DELETE|HEAD)['"]\s*,\s*)"),
                                            QRegularExpression::CaseInsensitiveOption);
    it = xhrOpen.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString rest = text.mid(match.capturedEnd(), 400);
        add(match.captured(1), urlFromExpression(rest), QStringLiteral("XMLHttpRequest"), match.capturedStart());
    }
}

void scanForms(const QString &text, QVector<ClientCall> &calls)
{
    static const QRegularExpression formTag(QStringLiteral(R"(<form\b([^>]*)>)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression actionAttr(QStringLiteral(R"(\baction\s*=\s*(["'])(.*?)\1)"),
                                               QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression methodAttr(QStringLiteral(R"(\bmethod\s*=\s*["']?(\w+))"),
                                               QRegularExpression::CaseInsensitiveOption);
    auto it = formTag.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString attributes = match.captured(1);
        QString action = actionAttr.match(attributes).captured(2);
        if (action.isEmpty()) {
            continue;
        }
        // Template expressions (<?= ... ?>, {{ ... }}, {% ... %}) are dynamic.
        static const QRegularExpression templated(QStringLiteral(R"(<\?.*?\?>|\{\{.*?\}\}|\{%.*?%\})"));
        action.replace(templated, QStringLiteral("*"));
        const QString method = methodAttr.match(attributes).captured(1);
        calls.append({method.isEmpty() ? QStringLiteral("GET") : method.toUpper(), action, QStringLiteral("form"),
                      lineAt(text, match.capturedStart())});
    }
}

bool isCallableKind(const QString &kind)
{
    static const QSet<QString> kinds = {
        QStringLiteral("function"), QStringLiteral("method"), QStringLiteral("constructor"), QStringLiteral("hook"),
        QStringLiteral("component"), QStringLiteral("handler"), QStringLiteral("test"), QStringLiteral("test hook"),
    };
    return kinds.contains(kind);
}

} // namespace

QVariantMap applyHttpClientCalls(QVariantMap analysis, const QString &text, const QString &language)
{
    static const QSet<QString> scripts = {QStringLiteral("script"), QStringLiteral("ts"), QStringLiteral("tsx"),
                                          QStringLiteral("jsx")};
    QVector<ClientCall> calls;
    if (scripts.contains(language)) {
        scanScript(text, 0, text, calls);
    } else if (language == QStringLiteral("html") || language == QStringLiteral("php")) {
        scanForms(text, calls);
        // Inline scripts.
        static const QRegularExpression scriptBlock(QStringLiteral(R"(<script\b[^>]*>(.*?)</script>)"),
                                                    QRegularExpression::CaseInsensitiveOption
                                                        | QRegularExpression::DotMatchesEverythingOption);
        auto it = scriptBlock.globalMatch(text);
        while (it.hasNext()) {
            const auto match = it.next();
            scanScript(match.captured(1), match.capturedStart(1), text, calls);
        }
    } else {
        return analysis;
    }
    if (calls.isEmpty()) {
        return analysis;
    }

    // Enclosing callable per call (innermost by line range). Script symbols
    // carry no end line, so the body is brace-matched from the declaration.
    const QStringList lines = text.split(QLatin1Char('\n'));
    QVector<int> lineStarts;
    int offset = 0;
    for (const QString &line : lines) {
        lineStarts.append(offset);
        offset += line.size() + 1;
    }
    auto inferEndLine = [&](int line) {
        if (line < 1 || line > lineStarts.size()) {
            return line;
        }
        const int start = lineStarts.at(line - 1);
        int brace = -1;
        QChar quote;
        for (int index = start; index < text.size() && index < start + 2000; ++index) {
            const QChar ch = text.at(index);
            if (!quote.isNull()) {
                if (ch == QLatin1Char('\\')) {
                    ++index;
                } else if (ch == quote) {
                    quote = QChar();
                }
                continue;
            }
            if (ch == QLatin1Char('\'') || ch == QLatin1Char('"') || ch == QLatin1Char('`')) {
                quote = ch;
            } else if (ch == QLatin1Char('{')) {
                brace = index;
                break;
            } else if (ch == QLatin1Char(';') || (ch == QLatin1Char('\n') && index > start && text.mid(start, index - start).contains(QStringLiteral("=>")))) {
                break;
            }
        }
        if (brace < 0) {
            return line;
        }
        const QString body = balancedFrom(text, brace, 400000);
        return lineAt(text, brace + body.size() - 1);
    };
    struct Range { int start; int end; QString name; QString kind; int line; };
    QVector<Range> ranges;
    std::function<void(const QVariantList &)> collect = [&](const QVariantList &symbols) {
        for (const QVariant &entry : symbols) {
            const QVariantMap symbol = entry.toMap();
            const int line = symbol.value(QStringLiteral("line")).toInt();
            int endLine = symbol.value(QStringLiteral("endLine")).toInt();
            if (endLine < line && isCallableKind(symbol.value(QStringLiteral("kind")).toString())) {
                endLine = inferEndLine(line);
            }
            if (isCallableKind(symbol.value(QStringLiteral("kind")).toString()) && endLine >= line) {
                ranges.append({line, endLine, symbol.value(QStringLiteral("name")).toString(),
                               symbol.value(QStringLiteral("kind")).toString(), line});
            }
            collect(symbol.value(QStringLiteral("members")).toList());
        }
    };
    collect(analysis.value(QStringLiteral("symbols")).toList());

    QVariantList list;
    QSet<QString> seen;
    for (const ClientCall &call : std::as_const(calls)) {
        const QString id = QStringLiteral("%1|%2|%3").arg(call.method, call.url).arg(call.line);
        if (seen.contains(id) || urlSegments(call.url).isEmpty()) {
            continue;
        }
        seen.insert(id);
        QVariantMap entry{{QStringLiteral("method"), call.method}, {QStringLiteral("url"), call.url},
                          {QStringLiteral("line"), call.line}, {QStringLiteral("via"), call.via}};
        const Range *best = nullptr;
        for (const Range &range : std::as_const(ranges)) {
            if (range.start <= call.line && call.line <= range.end
                && (!best || range.end - range.start < best->end - best->start)) {
                best = &range;
            }
        }
        if (best) {
            entry.insert(QStringLiteral("fromName"), best->name);
            entry.insert(QStringLiteral("fromKind"), best->kind);
            entry.insert(QStringLiteral("fromLine"), best->line);
        }
        list.append(entry);
        if (list.size() >= 200) {
            break;
        }
    }
    if (!list.isEmpty()) {
        analysis.insert(QStringLiteral("httpCalls"), list);
    }
    return analysis;
}

QStringList urlSegments(const QString &url)
{
    QString path = url.trimmed();
    if (path.contains(QLatin1Char('\'')) || path.contains(QLatin1Char('"')) || path.contains(QStringLiteral(".$"))) {
        return {}; // built inside a server-side string ('.$action.')
    }
    static const QRegularExpression origin(QStringLiteral(R"(^(?:https?:)?//([^/]*))"));
    const auto originMatch = origin.match(path);
    if (originMatch.hasMatch()) {
        // Another host is an external API; only local / templated hosts can
        // reach this project's routes.
        const QString host = originMatch.captured(1).section(QLatin1Char(':'), 0, 0).toLower();
        static const QSet<QString> localHosts = {
            QStringLiteral("localhost"), QStringLiteral("127.0.0.1"), QStringLiteral("0.0.0.0"), QStringLiteral("*"),
            QStringLiteral("[::1]"), QString(),
        };
        if (!localHosts.contains(host)) {
            return {};
        }
        path = path.mid(originMatch.capturedLength());
    }
    const int cut = path.indexOf(QRegularExpression(QStringLiteral(R"([?#])")));
    if (cut >= 0) {
        path = path.left(cut);
    }
    if (path.isEmpty() || path.startsWith(QLatin1Char('*')) && path.size() == 1) {
        return {};
    }
    // Relative URLs ('api/users', './x') are taken from the site root.
    if (path.startsWith(QStringLiteral("./"))) {
        path = path.mid(1);
    }
    if (path.contains(QStringLiteral("..")) || path.contains(QLatin1Char(' ')) || path.contains(QLatin1Char('<'))) {
        return {};
    }
    static const QRegularExpression assetLike(QStringLiteral(R"(\.(?:html?|css|js|mjs|json|png|jpe?g|gif|svg|webp|ico|woff2?|ttf|map|txt|xml)$)"),
                                              QRegularExpression::CaseInsensitiveOption);
    if (assetLike.match(path).hasMatch()) {
        return {}; // static files, not routes
    }
    QStringList parts;
    for (QString part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        parts.append(part.contains(QLatin1Char('*')) ? QStringLiteral("*") : part);
    }
    if (parts.isEmpty()) {
        parts.append(QString()); // the site root
    }
    if (!parts.contains(QString()) && std::all_of(parts.cbegin(), parts.cend(), [](const QString &part) {
            return part == QStringLiteral("*");
        })) {
        return {}; // nothing literal to match on
    }
    return parts;
}

QStringList routeSegments(const QString &routePath)
{
    QString path = routePath.trimmed();
    if (path.startsWith(QLatin1Char('^'))) {
        path.remove(0, 1);
    }
    if (path.endsWith(QLatin1Char('$'))) {
        path.chop(1);
    }
    if (!path.startsWith(QLatin1Char('/')) && !path.isEmpty()) {
        // Spring/ASP.NET/Laravel routes are often written without the slash;
        // anything with spaces or brackets is not a path (e.g. USE logger()).
        if (path.contains(QLatin1Char(' ')) || path.contains(QLatin1Char('('))) {
            return {};
        }
        path.prepend(QLatin1Char('/'));
    }
    if (path.isEmpty()) {
        return {};
    }
    QStringList parts;
    static const QRegularExpression parameter(QStringLiteral(R"(^(?::\w+\??|<[^>]+>|\{[^}]+\}|\*\w*|\[[^\]]+\])$)"));
    for (const QString &part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        parts.append(parameter.match(part).hasMatch() ? QStringLiteral("*") : part);
    }
    if (parts.isEmpty()) {
        parts.append(QString());
    }
    return parts;
}

int matchRoute(const QStringList &urlParts, const QStringList &routeParts, bool urlHasOpenTail)
{
    if (urlParts.isEmpty() || routeParts.isEmpty()) {
        return 0;
    }
    auto segmentMatches = [](const QString &url, const QString &route) {
        return url == QStringLiteral("*") || route == QStringLiteral("*") || url == route;
    };
    auto matchesFrom = [&](int urlStart) {
        const int length = urlParts.size() - urlStart;
        if (length != routeParts.size() && !(urlHasOpenTail && length < routeParts.size())) {
            return false;
        }
        for (int index = 0; index < routeParts.size(); ++index) {
            if (urlStart + index >= urlParts.size()) {
                return urlHasOpenTail; // 'api/' + rest: the rest may be several segments
            }
            if (!segmentMatches(urlParts.at(urlStart + index), routeParts.at(index))) {
                return false;
            }
        }
        return true;
    };
    int literals = 0;
    for (const QString &part : routeParts) {
        literals += (part != QStringLiteral("*") && !part.isEmpty()) ? 1 : 0;
    }
    if (matchesFrom(0)) {
        // An all-parameter route (/:slug) is too vague to trust on its own.
        return literals > 0 || routeParts.size() == 1 && routeParts.first().isEmpty() ? 2 : 0;
    }
    // Mounted routers: the route matches the URL's tail. Needs two or more
    // literal segments to be worth a guess.
    if (literals >= 2) {
        for (int start = 1; start < urlParts.size(); ++start) {
            if (urlParts.size() - start == routeParts.size() && matchesFrom(start)) {
                return 1;
            }
        }
    }
    return 0;
}

bool methodsCompatible(const QString &clientMethod, const QString &routeMethod)
{
    const QString route = routeMethod.toUpper();
    if (route.isEmpty() || route == QStringLiteral("ALL") || route == QStringLiteral("ANY")
        || route == QStringLiteral("*") || route == QStringLiteral("ROUTE") || route == QStringLiteral("MAP")
        || route == QStringLiteral("MATCH")) {
        return true;
    }
    return route.split(QRegularExpression(QStringLiteral(R"([|,/ ]+)")), Qt::SkipEmptyParts).contains(clientMethod.toUpper());
}

} // namespace HttpClients
