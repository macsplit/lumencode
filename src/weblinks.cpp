#include "weblinks.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>

#include <cstring>
#include <functional>
#include <tree_sitter/api.h>

extern "C" {
const TSLanguage *tree_sitter_html(void);
const TSLanguage *tree_sitter_javascript(void);
}

namespace WebLinks {

namespace {

constexpr qint64 kMaxPageBytes = 1024 * 1024;
constexpr qint64 kMaxScriptBytes = 512 * 1024;
constexpr int kAncestorLevels = 3;
constexpr int kMaxPagesPerFolder = 60;

QString typeOf(TSNode node)
{
    return ts_node_is_null(node) ? QString() : QString::fromUtf8(ts_node_type(node));
}

QString textOf(TSNode node, const QByteArray &source)
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

int lineOf(TSNode node)
{
    return static_cast<int>(ts_node_start_point(node).row) + 1;
}

TSNode field(TSNode node, const char *name)
{
    return ts_node_child_by_field_name(node, name, static_cast<uint32_t>(strlen(name)));
}

QString firstLine(const QString &text, int maxChars = 240)
{
    QString line = text.section(QLatin1Char('\n'), 0, 0).trimmed();
    if (line.size() > maxChars) {
        line = line.left(maxChars) + QStringLiteral("…");
    }
    return line;
}

bool isLocalTarget(const QString &target)
{
    if (target.isEmpty() || target.startsWith(QLatin1Char('#')) || target.startsWith(QStringLiteral("//"))
        || target.contains(QStringLiteral("://")) || target.startsWith(QStringLiteral("data:"))
        || target.startsWith(QStringLiteral("mailto:")) || target.startsWith(QStringLiteral("javascript:"))
        || target.startsWith(QStringLiteral("tel:")) || target.contains(QStringLiteral("{{"))
        || target.contains(QStringLiteral("<?")) || target.contains(QStringLiteral("${"))) {
        return false;
    }
    return true;
}

QString stripQueryAndFragment(QString target)
{
    static const QRegularExpression queryOrFragment(QStringLiteral(R"([?#])"));
    const int cut = target.indexOf(queryOrFragment);
    if (cut >= 0) {
        target = target.left(cut);
    }
    return target;
}

QString resolveTarget(const QString &pagePath, const QString &target)
{
    const QString cleanTarget = stripQueryAndFragment(target.trimmed());
    if (cleanTarget.isEmpty()) {
        return {};
    }
    const QDir pageDir = QFileInfo(pagePath).dir();
    if (cleanTarget.startsWith(QLatin1Char('/'))) {
        // Site-root relative: try the page folder and its ancestors as the root.
        QDir dir = pageDir;
        for (int level = 0; level <= kAncestorLevels + 1; ++level) {
            const QString candidate = QDir::cleanPath(dir.absolutePath() + cleanTarget);
            if (QFileInfo::exists(candidate)) {
                return candidate;
            }
            if (!dir.cdUp()) {
                break;
            }
        }
        return QDir::cleanPath(pageDir.absolutePath() + cleanTarget);
    }
    return QDir::cleanPath(pageDir.absoluteFilePath(cleanTarget));
}

QString attributeValue(TSNode attribute, const QByteArray &source)
{
    const uint32_t count = ts_node_named_child_count(attribute);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode child = ts_node_named_child(attribute, index);
        const QString type = typeOf(child);
        if (type == QStringLiteral("quoted_attribute_value")) {
            if (ts_node_named_child_count(child) > 0) {
                return textOf(ts_node_named_child(child, 0), source);
            }
            return {};
        }
        if (type == QStringLiteral("attribute_value")) {
            return textOf(child, source);
        }
    }
    return {};
}

QString attributeName(TSNode attribute, const QByteArray &source)
{
    const uint32_t count = ts_node_named_child_count(attribute);
    for (uint32_t index = 0; index < count; ++index) {
        TSNode child = ts_node_named_child(attribute, index);
        if (typeOf(child) == QStringLiteral("attribute_name")) {
            return textOf(child, source).toLower();
        }
    }
    return {};
}

QStringList calledNamesInCode(const QString &code)
{
    // Bare calls such as `toggle()`, `app.save(this)` (-> save) or
    // `return validate(event)`. Keywords and common DOM/event noise are skipped.
    static const QRegularExpression callPattern(QStringLiteral(R"(([A-Za-z_$][\w$]*)\s*\()"));
    static const QSet<QString> skip = {
        QStringLiteral("if"), QStringLiteral("for"), QStringLiteral("while"), QStringLiteral("switch"),
        QStringLiteral("return"), QStringLiteral("function"), QStringLiteral("alert"),
        QStringLiteral("confirm"), QStringLiteral("preventDefault"), QStringLiteral("stopPropagation"),
        QStringLiteral("setTimeout"), QStringLiteral("parseInt"), QStringLiteral("parseFloat"),
        QStringLiteral("getElementById"), QStringLiteral("querySelector"), QStringLiteral("typeof"),
    };
    QStringList names;
    auto it = callPattern.globalMatch(code);
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        if (!skip.contains(name) && !names.contains(name)) {
            names.append(name);
        }
    }
    return names;
}

// PHP templates: blank <?php ... ?> regions (keeping newlines) so the HTML
// grammar parses the markup with unchanged line numbers.
QString blankPhpRegions(QString text)
{
    int searchFrom = 0;
    while (true) {
        const int open = text.indexOf(QStringLiteral("<?"), searchFrom);
        if (open < 0) {
            break;
        }
        const int close = text.indexOf(QStringLiteral("?>"), open + 2);
        const int end = close < 0 ? text.size() : close + 2;
        for (int index = open; index < end; ++index) {
            if (text.at(index) != QLatin1Char('\n')) {
                text[index] = QLatin1Char(' ');
            }
        }
        searchFrom = end;
    }
    return text;
}

struct PageCacheEntry
{
    qint64 size = -1;
    QDateTime modified;
    HtmlPage page;
};

QMutex &cacheMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, PageCacheEntry> &pageCache()
{
    static QHash<QString, PageCacheEntry> cache;
    return cache;
}

struct ScriptCacheEntry
{
    qint64 size = -1;
    QDateTime modified;
    QHash<QString, int> functions;
};

QHash<QString, ScriptCacheEntry> &scriptCache()
{
    static QHash<QString, ScriptCacheEntry> cache;
    return cache;
}

const QRegularExpression &whitespacePattern()
{
    static const QRegularExpression pattern(QStringLiteral(R"(\s+)"));
    return pattern;
}

bool fileMentions(const QString &path, const QByteArray &needle)
{
    QFile file(path);
    if (file.size() > kMaxPageBytes || !file.open(QIODevice::ReadOnly)) {
        return false;
    }
    return file.readAll().contains(needle);
}

QString stringLiteralValue(TSNode node, const QByteArray &source)
{
    const QString type = typeOf(node);
    if (type == QStringLiteral("string")) {
        QString text = textOf(node, source);
        if (text.size() >= 2) {
            text = text.mid(1, text.size() - 2);
        }
        return text;
    }
    if (type == QStringLiteral("template_string")) {
        QString text = textOf(node, source);
        if (text.contains(QStringLiteral("${"))) {
            return {};
        }
        if (text.size() >= 2) {
            text = text.mid(1, text.size() - 2);
        }
        return text;
    }
    return {};
}

} // namespace

const HtmlElement *HtmlPage::elementWithId(const QString &id) const
{
    for (const HtmlElement &element : elements) {
        if (element.id == id) {
            return &element;
        }
    }
    return nullptr;
}

QList<const HtmlElement *> HtmlPage::elementsWithClass(const QString &className) const
{
    QList<const HtmlElement *> result;
    for (const HtmlElement &element : elements) {
        if (element.classes.contains(className)) {
            result.append(&element);
        }
    }
    return result;
}

QStringList HtmlPage::usedClasses() const
{
    QStringList classes;
    for (const HtmlElement &element : elements) {
        for (const QString &className : element.classes) {
            if (!classes.contains(className)) {
                classes.append(className);
            }
        }
    }
    return classes;
}

bool HtmlPage::linksAsset(const QString &absolutePath, const QString &kind) const
{
    const QFileInfo wanted(absolutePath);
    const QString canonical = wanted.canonicalFilePath();
    for (const HtmlAsset &asset : assets) {
        if (asset.kind != kind || !asset.local || asset.resolvedPath.isEmpty()) {
            continue;
        }
        if (asset.resolvedPath == wanted.absoluteFilePath()) {
            return true;
        }
        const QString assetCanonical = QFileInfo(asset.resolvedPath).canonicalFilePath();
        if (!canonical.isEmpty() && assetCanonical == canonical) {
            return true;
        }
    }
    return false;
}

HtmlPage parseHtmlPage(const QString &path, const QString &text)
{
    HtmlPage page;
    page.path = QFileInfo(path).absoluteFilePath();
    const QByteArray source = text.toUtf8();

    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, tree_sitter_html())) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return page;
    }
    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    if (!tree) {
        ts_parser_delete(parser);
        return page;
    }
    page.valid = true;

    auto addAsset = [&](const QString &kind, const QString &target, TSNode tagNode) {
        const QString trimmed = target.trimmed();
        if (trimmed.isEmpty()) {
            return;
        }
        HtmlAsset asset;
        asset.kind = kind;
        asset.target = trimmed;
        asset.local = isLocalTarget(trimmed);
        if (asset.local) {
            asset.resolvedPath = resolveTarget(page.path, trimmed);
            asset.exists = QFileInfo::exists(asset.resolvedPath);
        } else {
            asset.exists = true;
        }
        asset.line = lineOf(tagNode);
        asset.snippet = firstLine(textOf(tagNode, source));
        page.assets.append(asset);
    };

    std::function<void(TSNode)> visitTag = [&](TSNode tag) {
        // tag is a start_tag or self_closing_tag
        QString tagName;
        QHash<QString, QString> attributes;
        QList<QPair<QString, QString>> ordered;
        const uint32_t count = ts_node_named_child_count(tag);
        for (uint32_t index = 0; index < count; ++index) {
            TSNode child = ts_node_named_child(tag, index);
            const QString type = typeOf(child);
            if (type == QStringLiteral("tag_name")) {
                tagName = textOf(child, source).toLower();
            } else if (type == QStringLiteral("attribute")) {
                const QString name = attributeName(child, source);
                const QString value = attributeValue(child, source);
                if (!name.isEmpty()) {
                    attributes.insert(name, value);
                    ordered.append({name, value});
                }
            }
        }
        if (tagName.isEmpty()) {
            return;
        }
        const QString snippet = firstLine(textOf(tag, source));
        const int line = lineOf(tag);

        HtmlElement element;
        element.tag = tagName;
        element.line = line;
        element.snippet = snippet;
        element.id = attributes.value(QStringLiteral("id")).trimmed();
        const QString classAttr = attributes.value(QStringLiteral("class"));
        if (!classAttr.contains(QStringLiteral("{{")) && !classAttr.contains(QStringLiteral("<?"))) {
            static const QRegularExpression whitespace(QStringLiteral(R"(\s+)"));
            static const QRegularExpression validClass(QStringLiteral(R"(^-?[A-Za-z_][\w-]*$)"));
            for (const QString &className : classAttr.split(whitespace, Qt::SkipEmptyParts)) {
                if (validClass.match(className).hasMatch() && !element.classes.contains(className)) {
                    element.classes.append(className);
                }
            }
        }
        if (!element.id.isEmpty() || !element.classes.isEmpty()) {
            page.elements.append(element);
        }
        if (tagName.contains(QLatin1Char('-'))) {
            page.customElements.append(element);
        }

        for (const auto &attribute : std::as_const(ordered)) {
            if (attribute.first.startsWith(QStringLiteral("on")) && attribute.first.size() > 2
                && !attribute.second.trimmed().isEmpty()) {
                HtmlHandler handler;
                handler.tag = tagName;
                handler.attribute = attribute.first;
                handler.code = attribute.second.trimmed();
                handler.calledNames = calledNamesInCode(handler.code);
                handler.line = line;
                handler.snippet = snippet;
                page.handlers.append(handler);
            }
        }

        if (tagName == QStringLiteral("script") && attributes.contains(QStringLiteral("src"))) {
            addAsset(QStringLiteral("script"), attributes.value(QStringLiteral("src")), tag);
        } else if (tagName == QStringLiteral("link")) {
            const QString rel = attributes.value(QStringLiteral("rel")).toLower();
            const QString href = attributes.value(QStringLiteral("href"));
            if (rel.contains(QStringLiteral("stylesheet"))
                || (rel.isEmpty() && href.toLower().endsWith(QStringLiteral(".css")))) {
                addAsset(QStringLiteral("stylesheet"), href, tag);
            } else if (rel.contains(QStringLiteral("import"))) {
                addAsset(QStringLiteral("page"), href, tag);
            }
        } else if (tagName == QStringLiteral("a") || tagName == QStringLiteral("area")) {
            const QString href = attributes.value(QStringLiteral("href"));
            const QString lower = stripQueryAndFragment(href).toLower();
            if (isLocalTarget(href)
                && (lower.endsWith(QStringLiteral(".html")) || lower.endsWith(QStringLiteral(".htm"))
                    || lower.endsWith(QStringLiteral(".php")))) {
                addAsset(QStringLiteral("page"), href, tag);
            }
        } else if (tagName == QStringLiteral("form") && attributes.contains(QStringLiteral("action"))) {
            addAsset(QStringLiteral("form"), attributes.value(QStringLiteral("action")), tag);
        } else if (tagName == QStringLiteral("iframe") || tagName == QStringLiteral("frame")) {
            addAsset(QStringLiteral("frame"), attributes.value(QStringLiteral("src")), tag);
        }
    };

    std::function<void(TSNode)> visit = [&](TSNode node) {
        const QString type = typeOf(node);
        if (type == QStringLiteral("start_tag") || type == QStringLiteral("self_closing_tag")) {
            visitTag(node);
            return;
        }
        if (type == QStringLiteral("script_element") || type == QStringLiteral("style_element")) {
            QString blockType;
            bool hasSrc = false;
            const uint32_t count = ts_node_named_child_count(node);
            for (uint32_t index = 0; index < count; ++index) {
                TSNode child = ts_node_named_child(node, index);
                const QString childType = typeOf(child);
                if (childType == QStringLiteral("start_tag")) {
                    visitTag(child);
                    const uint32_t attrCount = ts_node_named_child_count(child);
                    for (uint32_t a = 0; a < attrCount; ++a) {
                        TSNode attribute = ts_node_named_child(child, a);
                        const QString name = attributeName(attribute, source);
                        if (name == QStringLiteral("type")) {
                            blockType = attributeValue(attribute, source).trimmed().toLower();
                        } else if (name == QStringLiteral("src")) {
                            hasSrc = true;
                        }
                    }
                } else if (childType == QStringLiteral("raw_text") && !hasSrc) {
                    const bool isScript = type == QStringLiteral("script_element");
                    const bool scriptLike = blockType.isEmpty() || blockType.contains(QStringLiteral("javascript"))
                        || blockType == QStringLiteral("module") || blockType.contains(QStringLiteral("babel"))
                        || blockType.contains(QStringLiteral("jsx"));
                    if (isScript && !scriptLike) {
                        continue; // templates, JSON data blocks, ...
                    }
                    HtmlInlineBlock block;
                    block.kind = isScript ? QStringLiteral("script") : QStringLiteral("style");
                    block.type = blockType;
                    block.startLine = lineOf(child);
                    block.startOffset = static_cast<int>(ts_node_start_byte(child));
                    block.content = textOf(child, source);
                    if (!block.content.trimmed().isEmpty()) {
                        page.inlineBlocks.append(block);
                    }
                }
            }
            return;
        }
        const uint32_t count = ts_node_named_child_count(node);
        for (uint32_t index = 0; index < count; ++index) {
            visit(ts_node_named_child(node, index));
        }
    };
    visit(ts_tree_root_node(tree));

    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return page;
}

HtmlPage loadHtmlPage(const QString &path)
{
    const QFileInfo info(path);
    const QString key = info.absoluteFilePath();
    if (!info.exists() || info.size() > kMaxPageBytes) {
        HtmlPage empty;
        empty.path = key;
        return empty;
    }
    {
        QMutexLocker locker(&cacheMutex());
        const auto it = pageCache().constFind(key);
        if (it != pageCache().constEnd() && it->size == info.size() && it->modified == info.lastModified()) {
            return it->page;
        }
    }
    QFile file(key);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        HtmlPage empty;
        empty.path = key;
        return empty;
    }
    QString text = QString::fromUtf8(file.readAll());
    if (key.endsWith(QStringLiteral(".php"), Qt::CaseInsensitive)) {
        text = blankPhpRegions(text);
    }
    const HtmlPage page = parseHtmlPage(key, text);
    QMutexLocker locker(&cacheMutex());
    PageCacheEntry entry;
    entry.size = info.size();
    entry.modified = info.lastModified();
    entry.page = page;
    pageCache().insert(key, entry);
    return page;
}

QList<HtmlPage> pagesReferencingAsset(const QString &assetPath, const QString &kind)
{
    QList<HtmlPage> pages;
    const QString absolute = QFileInfo(assetPath).absoluteFilePath();
    const QByteArray needle = QFileInfo(assetPath).fileName().toUtf8();
    QDir dir = QFileInfo(assetPath).dir();
    QSet<QString> seen;
    for (int level = 0; level <= kAncestorLevels; ++level) {
        const QFileInfoList candidates = dir.entryInfoList(
            {QStringLiteral("*.html"), QStringLiteral("*.htm"), QStringLiteral("*.php")},
            QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        int inspected = 0;
        for (const QFileInfo &candidate : candidates) {
            if (++inspected > kMaxPagesPerFolder) {
                break;
            }
            const QString candidatePath = candidate.absoluteFilePath();
            if (seen.contains(candidatePath) || candidatePath == absolute) {
                continue;
            }
            seen.insert(candidatePath);
            if (!fileMentions(candidatePath, needle)) {
                continue; // cheap byte search before a full parse
            }
            const HtmlPage page = loadHtmlPage(candidatePath);
            if (page.valid && page.linksAsset(absolute, kind)) {
                pages.append(page);
            }
        }
        if (!pages.isEmpty() || !dir.cdUp()) {
            break; // nearest folder with consumers wins
        }
    }
    return pages;
}

QStringList idsInSelector(const QString &selector)
{
    static const QRegularExpression idPattern(QStringLiteral(R"(#(-?[A-Za-z_][\w-]*))"));
    QStringList ids;
    auto it = idPattern.globalMatch(selector);
    while (it.hasNext()) {
        const QString id = it.next().captured(1);
        if (!ids.contains(id)) {
            ids.append(id);
        }
    }
    return ids;
}

QStringList classesInSelector(const QString &selector)
{
    // Strip attribute selectors and strings so `[href$=".pdf"]` is not a class.
    QString cleaned = selector;
    static const QRegularExpression attributeSelector(QStringLiteral(R"(\[[^\]]*\])"));
    cleaned.remove(attributeSelector);
    static const QRegularExpression classPattern(QStringLiteral(R"((?:^|[^\w-])\.(-?[A-Za-z_][\w-]*))"));
    QStringList classes;
    auto it = classPattern.globalMatch(cleaned);
    while (it.hasNext()) {
        const QString className = it.next().captured(1);
        if (!classes.contains(className)) {
            classes.append(className);
        }
    }
    return classes;
}

namespace {

void walkJs(const QString &jsText,
            const std::function<void(TSNode, const QByteArray &)> &visitor)
{
    const QByteArray source = jsText.toUtf8();
    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, tree_sitter_javascript())) {
        if (parser) {
            ts_parser_delete(parser);
        }
        return;
    }
    TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
    if (tree) {
        std::function<void(TSNode)> visit = [&](TSNode node) {
            visitor(node, source);
            const uint32_t count = ts_node_named_child_count(node);
            for (uint32_t index = 0; index < count; ++index) {
                visit(ts_node_named_child(node, index));
            }
        };
        visit(ts_tree_root_node(tree));
        ts_tree_delete(tree);
    }
    ts_parser_delete(parser);
}

} // namespace

QList<DomReference> extractDomReferences(const QString &jsText)
{
    QList<DomReference> refs;
    QSet<QString> seen;
    auto add = [&](const QString &kind, const QString &name, const QString &via, TSNode node, const QByteArray &source) {
        if (name.isEmpty()) {
            return;
        }
        const QString key = kind + QLatin1Char('|') + name + QLatin1Char('|') + QString::number(lineOf(node));
        if (seen.contains(key)) {
            return;
        }
        seen.insert(key);
        DomReference ref;
        ref.kind = kind;
        ref.name = name;
        ref.via = via;
        ref.line = lineOf(node);
        ref.snippet = firstLine(textOf(node, source));
        refs.append(ref);
    };
    auto addSelector = [&](const QString &selector, const QString &via, TSNode node, const QByteArray &source) {
        for (const QString &id : idsInSelector(selector)) {
            add(QStringLiteral("id"), id, via, node, source);
        }
        for (const QString &className : classesInSelector(selector)) {
            add(QStringLiteral("class"), className, via, node, source);
        }
    };
    static const QSet<QString> classListMethods = {
        QStringLiteral("add"), QStringLiteral("remove"), QStringLiteral("toggle"),
        QStringLiteral("contains"), QStringLiteral("replace"),
    };
    static const QSet<QString> jqueryClassMethods = {
        QStringLiteral("addClass"), QStringLiteral("removeClass"), QStringLiteral("toggleClass"),
        QStringLiteral("hasClass"),
    };
    static const QSet<QString> selectorMethods = {
        QStringLiteral("querySelector"), QStringLiteral("querySelectorAll"), QStringLiteral("closest"),
        QStringLiteral("matches"), QStringLiteral("find"), QStringLiteral("children"),
    };

    walkJs(jsText, [&](TSNode node, const QByteArray &source) {
        const QString type = typeOf(node);
        if (type == QStringLiteral("call_expression")) {
            TSNode callee = field(node, "function");
            TSNode arguments = field(node, "arguments");
            TSNode firstArg = ts_node_named_child(arguments, 0);
            const QString first = ts_node_named_child_count(arguments) > 0 ? stringLiteralValue(firstArg, source) : QString();
            QString method;
            QString object;
            if (typeOf(callee) == QStringLiteral("member_expression")) {
                method = textOf(field(callee, "property"), source).trimmed();
                object = textOf(field(callee, "object"), source).trimmed();
            } else {
                method = textOf(callee, source).trimmed();
            }
            if (method == QStringLiteral("getElementById")) {
                add(QStringLiteral("id"), first.trimmed(), method, node, source);
            } else if (method == QStringLiteral("getElementsByClassName")) {
                for (const QString &className : first.split(whitespacePattern(), Qt::SkipEmptyParts)) {
                    add(QStringLiteral("class"), className, method, node, source);
                }
            } else if (selectorMethods.contains(method) && !first.isEmpty()
                       && (method.startsWith(QStringLiteral("query")) || first.contains(QLatin1Char('#'))
                           || first.contains(QLatin1Char('.')))) {
                addSelector(first, method, node, source);
            } else if ((method == QStringLiteral("$") || method == QStringLiteral("jQuery"))
                       && !first.isEmpty() && !first.trimmed().startsWith(QLatin1Char('<'))) {
                addSelector(first, method, node, source);
            } else if (classListMethods.contains(method) && object.endsWith(QStringLiteral("classList"))) {
                const uint32_t count = ts_node_named_child_count(arguments);
                for (uint32_t index = 0; index < count; ++index) {
                    const QString value = stringLiteralValue(ts_node_named_child(arguments, index), source).trimmed();
                    if (!value.isEmpty() && !value.contains(QLatin1Char(' '))) {
                        add(QStringLiteral("class"), value, QStringLiteral("classList.") + method, node, source);
                    }
                }
            } else if (jqueryClassMethods.contains(method)) {
                for (const QString &className : first.split(whitespacePattern(), Qt::SkipEmptyParts)) {
                    add(QStringLiteral("class"), className, method, node, source);
                }
            } else if (method == QStringLiteral("setAttribute") && first == QStringLiteral("class")
                       && ts_node_named_child_count(arguments) > 1) {
                const QString value = stringLiteralValue(ts_node_named_child(arguments, 1), source);
                for (const QString &className : value.split(whitespacePattern(), Qt::SkipEmptyParts)) {
                    add(QStringLiteral("class"), className, method, node, source);
                }
            } else if (method == QStringLiteral("createElement") && first.contains(QLatin1Char('-'))) {
                add(QStringLiteral("custom-element"), first.trimmed(), method, node, source);
            }
        } else if (type == QStringLiteral("assignment_expression")) {
            const QString left = textOf(field(node, "left"), source).trimmed();
            if (left.endsWith(QStringLiteral(".className"))) {
                const QString value = stringLiteralValue(field(node, "right"), source);
                for (const QString &className : value.split(whitespacePattern(), Qt::SkipEmptyParts)) {
                    add(QStringLiteral("class"), className, QStringLiteral("className"), node, source);
                }
            }
        }
    });
    return refs;
}

QHash<QString, QString> extractCustomElementDefinitions(const QString &jsText)
{
    QHash<QString, QString> definitions;
    walkJs(jsText, [&](TSNode node, const QByteArray &source) {
        if (typeOf(node) != QStringLiteral("call_expression")) {
            return;
        }
        const QString callee = textOf(field(node, "function"), source).trimmed();
        if (!callee.endsWith(QStringLiteral("customElements.define")) && callee != QStringLiteral("customElements.define")) {
            return;
        }
        TSNode arguments = field(node, "arguments");
        if (ts_node_named_child_count(arguments) < 2) {
            return;
        }
        const QString tag = stringLiteralValue(ts_node_named_child(arguments, 0), source).trimmed();
        TSNode classNode = ts_node_named_child(arguments, 1);
        QString className = textOf(classNode, source).trimmed();
        if (typeOf(classNode) == QStringLiteral("class")) {
            className = textOf(field(classNode, "name"), source).trimmed();
        }
        if (!tag.isEmpty() && !className.isEmpty() && className.size() < 80) {
            definitions.insert(tag, className);
        }
    });
    return definitions;
}

QHash<QString, int> globalFunctionsOfScript(const QString &path)
{
    const QFileInfo info(path);
    const QString key = info.absoluteFilePath();
    if (!info.exists() || info.size() > kMaxScriptBytes) {
        return {};
    }
    {
        QMutexLocker locker(&cacheMutex());
        const auto it = scriptCache().constFind(key);
        if (it != scriptCache().constEnd() && it->size == info.size() && it->modified == info.lastModified()) {
            return it->functions;
        }
    }
    QFile file(key);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    const QString text = QString::fromUtf8(file.readAll());
    QHash<QString, int> functions;
    const bool minified = QFileInfo(key).fileName().contains(QStringLiteral(".min."))
        || text.section(QLatin1Char('\n'), 0, 0).size() > 2000;
    if (minified) {
        QMutexLocker locker(&cacheMutex());
        ScriptCacheEntry entry;
        entry.size = info.size();
        entry.modified = info.lastModified();
        scriptCache().insert(key, entry);
        return functions; // library bundles: not worth resolving handlers into
    }
    const QByteArray source = text.toUtf8();
    TSParser *parser = ts_parser_new();
    if (parser && ts_parser_set_language(parser, tree_sitter_javascript())) {
        TSTree *tree = ts_parser_parse_string(parser, nullptr, source.constData(), source.size());
        if (tree) {
            TSNode root = ts_tree_root_node(tree);
            const uint32_t count = ts_node_named_child_count(root);
            for (uint32_t index = 0; index < count; ++index) {
                TSNode node = ts_node_named_child(root, index);
                const QString type = typeOf(node);
                if (type == QStringLiteral("function_declaration")) {
                    functions.insert(textOf(field(node, "name"), source), lineOf(node));
                } else if (type == QStringLiteral("lexical_declaration") || type == QStringLiteral("variable_declaration")) {
                    const uint32_t declarators = ts_node_named_child_count(node);
                    for (uint32_t d = 0; d < declarators; ++d) {
                        TSNode declarator = ts_node_named_child(node, d);
                        const QString valueType = typeOf(field(declarator, "value"));
                        if (valueType == QStringLiteral("function_expression") || valueType == QStringLiteral("arrow_function")
                            || valueType == QStringLiteral("function")) {
                            functions.insert(textOf(field(declarator, "name"), source), lineOf(declarator));
                        }
                    }
                } else if (type == QStringLiteral("expression_statement")) {
                    TSNode expression = ts_node_named_child(node, 0);
                    if (typeOf(expression) == QStringLiteral("assignment_expression")) {
                        const QString left = textOf(field(expression, "left"), source).trimmed();
                        const QString valueType = typeOf(field(expression, "right"));
                        if ((left.startsWith(QStringLiteral("window.")) && left.count(QLatin1Char('.')) == 1)
                            && (valueType == QStringLiteral("function_expression") || valueType == QStringLiteral("arrow_function")
                                || valueType == QStringLiteral("function"))) {
                            functions.insert(left.mid(7), lineOf(expression));
                        }
                    }
                }
            }
            ts_tree_delete(tree);
        }
    }
    if (parser) {
        ts_parser_delete(parser);
    }
    QMutexLocker locker(&cacheMutex());
    ScriptCacheEntry entry;
    entry.size = info.size();
    entry.modified = info.lastModified();
    entry.functions = functions;
    scriptCache().insert(key, entry);
    return functions;
}

} // namespace WebLinks
