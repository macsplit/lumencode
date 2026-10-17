#include "projectindex.h"
#include "httpclients.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentMap>

namespace ProjectIndex {

namespace {

constexpr int kCacheVersion = 3;
constexpr qint64 kMaxIndexedFileBytes = 2 * 1024 * 1024;
constexpr int kMaxListedCallers = 400;

const QSet<QString> &indexedSuffixes()
{
    static const QSet<QString> suffixes = {
        QStringLiteral("php"), QStringLiteral("js"), QStringLiteral("mjs"), QStringLiteral("cjs"), QStringLiteral("jsx"),
        QStringLiteral("ts"), QStringLiteral("mts"), QStringLiteral("cts"), QStringLiteral("tsx"), QStringLiteral("py"),
        QStringLiteral("java"), QStringLiteral("cs"), QStringLiteral("rs"), QStringLiteral("swift"), QStringLiteral("go"),
        QStringLiteral("c"), QStringLiteral("cc"), QStringLiteral("cpp"), QStringLiteral("cxx"), QStringLiteral("h"),
        QStringLiteral("hh"), QStringLiteral("hpp"), QStringLiteral("hxx"), QStringLiteral("m"), QStringLiteral("mm"),
        QStringLiteral("vb"), QStringLiteral("sql"), QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("qml"),
        QStringLiteral("html"), QStringLiteral("htm"),
        QStringLiteral("aspx"), QStringLiteral("ascx"), QStringLiteral("master"), QStringLiteral("ashx"), QStringLiteral("asmx"), QStringLiteral("asax"),
        QStringLiteral("kt"), QStringLiteral("kts"), QStringLiteral("rb"), QStringLiteral("scss"), QStringLiteral("less"),
    };
    return suffixes;
}

bool isSkippedDirectory(const QString &name)
{
    static const QSet<QString> names = {
        QStringLiteral(".git"), QStringLiteral("node_modules"), QStringLiteral("dist"), QStringLiteral("build"),
        QStringLiteral(".next"), QStringLiteral(".nuxt"), QStringLiteral("coverage"), QStringLiteral("vendor"),
        QStringLiteral("__pycache__"), QStringLiteral(".venv"), QStringLiteral("venv"), QStringLiteral("target"),
        QStringLiteral(".build"), QStringLiteral("DerivedData"), QStringLiteral("bower_components"), QStringLiteral(".gradle"),
        QStringLiteral("bin"), QStringLiteral("obj"),
    };
    return names.contains(name) || (name.startsWith(QLatin1Char('.')) && name.size() > 1);
}

bool isDefinitionKind(const QString &kind)
{
    static const QSet<QString> kinds = {
        QStringLiteral("function"), QStringLiteral("method"), QStringLiteral("constructor"), QStringLiteral("hook"),
        QStringLiteral("component"), QStringLiteral("class"), QStringLiteral("struct"), QStringLiteral("interface"),
        QStringLiteral("trait"), QStringLiteral("enum"), QStringLiteral("type"), QStringLiteral("protocol"),
        QStringLiteral("procedure"), QStringLiteral("operator"), QStringLiteral("destructor"), QStringLiteral("scope"),
        QStringLiteral("module"), QStringLiteral("union"), QStringLiteral("record"), QStringLiteral("object"),
        QStringLiteral("mixin"), QStringLiteral("placeholder"),
    };
    return kinds.contains(kind);
}

bool isTypeKind(const QString &kind)
{
    static const QSet<QString> kinds = {
        QStringLiteral("class"), QStringLiteral("struct"), QStringLiteral("interface"), QStringLiteral("trait"),
        QStringLiteral("enum"), QStringLiteral("type"), QStringLiteral("protocol"), QStringLiteral("union"),
        QStringLiteral("scope"), QStringLiteral("component"), QStringLiteral("record"), QStringLiteral("object"),
    };
    return kinds.contains(kind);
}

bool isCallableKind(const QString &kind)
{
    static const QSet<QString> kinds = {
        QStringLiteral("function"), QStringLiteral("method"), QStringLiteral("constructor"), QStringLiteral("hook"),
        QStringLiteral("component"), QStringLiteral("procedure"), QStringLiteral("operator"), QStringLiteral("mixin"),
    };
    return kinds.contains(kind);
}

// Names too common to link by uniqueness alone.
bool isGenericName(const QString &name)
{
    static const QSet<QString> names = {
        QStringLiteral("get"), QStringLiteral("set"), QStringLiteral("add"), QStringLiteral("remove"), QStringLiteral("run"),
        QStringLiteral("init"), QStringLiteral("main"), QStringLiteral("new"), QStringLiteral("create"), QStringLiteral("update"),
        QStringLiteral("delete"), QStringLiteral("close"), QStringLiteral("open"), QStringLiteral("start"), QStringLiteral("stop"),
        QStringLiteral("load"), QStringLiteral("save"), QStringLiteral("send"), QStringLiteral("read"), QStringLiteral("write"),
        QStringLiteral("call"), QStringLiteral("apply"), QStringLiteral("render"), QStringLiteral("handle"), QStringLiteral("execute"),
        QStringLiteral("process"), QStringLiteral("build"), QStringLiteral("parse"), QStringLiteral("toString"), QStringLiteral("equals"),
        QStringLiteral("hashCode"), QStringLiteral("dispose"), QStringLiteral("reset"), QStringLiteral("clear"), QStringLiteral("push"),
        QStringLiteral("pop"), QStringLiteral("map"), QStringLiteral("filter"), QStringLiteral("reduce"), QStringLiteral("find"),
        QStringLiteral("log"), QStringLiteral("print"), QStringLiteral("format"), QStringLiteral("value"), QStringLiteral("next"),
        QStringLiteral("test"), QStringLiteral("describe"), QStringLiteral("assert"), QStringLiteral("Error"), QStringLiteral("String"),
        QStringLiteral("append"), QStringLiteral("insert"), QStringLiteral("length"), QStringLiteral("size"), QStringLiteral("count"),
        QStringLiteral("then"), QStringLiteral("catch"), QStringLiteral("resolve"), QStringLiteral("reject"), QStringLiteral("emit"),
        QStringLiteral("dispatch"), QStringLiteral("validate"), QStringLiteral("serialize"), QStringLiteral("handler"),
        QStringLiteral("callback"), QStringLiteral("check"), QStringLiteral("setup"), QStringLiteral("teardown"),
        // Standard-library method names that projects also define.
        QStringLiteral("unwrap"), QStringLiteral("expect"), QStringLiteral("clone"), QStringLiteral("iter"),
        QStringLiteral("is_empty"), QStringLiteral("to_string"), QStringLiteral("as_str"), QStringLiteral("as_ref"),
        QStringLiteral("into"), QStringLiteral("from"), QStringLiteral("default"), QStringLiteral("config"),
        QStringLiteral("contains"), QStringLiteral("remove"), QStringLiteral("keys"), QStringLiteral("values"),
        QStringLiteral("items"), QStringLiteral("join"), QStringLiteral("split"), QStringLiteral("trim"),
        QStringLiteral("replace"), QStringLiteral("close"), QStringLiteral("flush"), QStringLiteral("finish"),
        QStringLiteral("forEach"), QStringLiteral("indexOf"), QStringLiteral("slice"), QStringLiteral("splice"),
        QStringLiteral("concat"), QStringLiteral("toJSON"), QStringLiteral("json"), QStringLiteral("text"),
        QStringLiteral("status"), QStringLiteral("append"), QStringLiteral("extend"), QStringLiteral("startswith"),
        QStringLiteral("endswith"), QStringLiteral("encode"), QStringLiteral("decode"), QStringLiteral("lower"),
        QStringLiteral("upper"), QStringLiteral("strip"), QStringLiteral("Equals"), QStringLiteral("ToString"),
        QStringLiteral("GetHashCode"), QStringLiteral("Dispose"), QStringLiteral("Add"), QStringLiteral("Remove"),
        QStringLiteral("Contains"), QStringLiteral("Clear"), QStringLiteral("Invoke"), QStringLiteral("Execute"),
        QStringLiteral("Create"), QStringLiteral("Build"), QStringLiteral("Should"), QStringLiteral("Be"),
        QStringLiteral("Returns"), QStringLiteral("Setup"), QStringLiteral("Verify"), QStringLiteral("Error"),
        QStringLiteral("Errorf"), QStringLiteral("Println"), QStringLiteral("Printf"), QStringLiteral("Sprintf"),
        QStringLiteral("Fatal"), QStringLiteral("Fatalf"), QStringLiteral("Run"), QStringLiteral("Get"),
        QStringLiteral("Set"), QStringLiteral("String"), QStringLiteral("Close"), QStringLiteral("Write"),
        QStringLiteral("Read"), QStringLiteral("Len"), QStringLiteral("description"), QStringLiteral("resume"),
        QStringLiteral("cancel"), QStringLiteral("assertEquals"), QStringLiteral("assertTrue"), QStringLiteral("toBe"),
        QStringLiteral("toEqual"), QStringLiteral("isEmpty"), QStringLiteral("getName"), QStringLiteral("getValue"),
        QStringLiteral("containsKey"), QStringLiteral("getKey"), QStringLiteral("hasNext"), QStringLiteral("valueOf"),
        QStringLiteral("getClass"), QStringLiteral("addAll"), QStringLiteral("isPresent"), QStringLiteral("orElse"),
        QStringLiteral("getMessage"), QStringLiteral("toLowerCase"), QStringLiteral("toUpperCase"), QStringLiteral("asList"),
        QStringLiteral("hasOwnProperty"), QStringLiteral("addEventListener"), QStringLiteral("removeEventListener"),
        QStringLiteral("querySelector"), QStringLiteral("getElementById"), QStringLiteral("setTimeout"),
        QStringLiteral("toHaveBeenCalled"), QStringLiteral("toHaveBeenCalledWith"), QStringLiteral("toThrow"),
        QStringLiteral("toMatchObject"), QStringLiteral("toStrictEqual"), QStringLiteral("isTrue"), QStringLiteral("isFalse"),
        QStringLiteral("isEqualTo"), QStringLiteral("isNull"), QStringLiteral("isNotNull"), QStringLiteral("assertThat"),
        QStringLiteral("assertNull"), QStringLiteral("assertNotNull"), QStringLiteral("assertFalse"), QStringLiteral("assertRaises"),
        QStringLiteral("assertEqual"), QStringLiteral("ToList"), QStringLiteral("ToArray"), QStringLiteral("FirstOrDefault"),
        QStringLiteral("ConfigureAwait"), QStringLiteral("GetType"), QStringLiteral("ShouldBe"), QStringLiteral("Throw"),
        QStringLiteral("WriteLine"), QStringLiteral("is_some"), QStringLiteral("is_none"), QStringLiteral("is_ok"),
        QStringLiteral("is_err"), QStringLiteral("unwrap_or"), QStringLiteral("map_err"), QStringLiteral("ok_or"),
        QStringLiteral("as_bytes"), QStringLiteral("to_owned"), QStringLiteral("to_vec"), QStringLiteral("push_str"),
        QStringLiteral("push_back"), QStringLiteral("emplace_back"), QStringLiteral("floatValue"),
        QStringLiteral("intValue"), QStringLiteral("longValue"), QStringLiteral("doubleValue"), QStringLiteral("shortValue"),
        QStringLiteral("byteValue"), QStringLiteral("compareTo"), QStringLiteral("iterator"), QStringLiteral("subList"),
        QStringLiteral("entrySet"), QStringLiteral("keySet"), QStringLiteral("putAll"), QStringLiteral("removeAll"),
        // Ruby core methods that classes override.
        QStringLiteral("each_key"), QStringLiteral("each_value"), QStringLiteral("each_pair"), QStringLiteral("each_with_index"),
        QStringLiteral("each_with_object"), QStringLiteral("to_hash"), QStringLiteral("to_h"), QStringLiteral("to_s"),
        QStringLiteral("to_a"), QStringLiteral("to_i"), QStringLiteral("to_f"), QStringLiteral("to_sym"), QStringLiteral("to_str"),
        QStringLiteral("to_proc"), QStringLiteral("to_json"), QStringLiteral("setdefault"), QStringLiteral("respond_to?"), QStringLiteral("merge!"),
        QStringLiteral("fetch"), QStringLiteral("key?"), QStringLiteral("include?"), QStringLiteral("empty?"),
        QStringLiteral("nil?"), QStringLiteral("is_a?"), QStringLiteral("kind_of?"), QStringLiteral("freeze"),
        QStringLiteral("inspect"), QStringLiteral("instance_eval"), QStringLiteral("class_eval"), QStringLiteral("define_method"),
        QStringLiteral("method_missing"), QStringLiteral("respond_to_missing?"), QStringLiteral("initialize_copy"),
        // Kotlin standard library.
        QStringLiteral("toLong"), QStringLiteral("toInt"), QStringLiteral("toShort"), QStringLiteral("toByte"),
        QStringLiteral("toChar"), QStringLiteral("toDouble"), QStringLiteral("toFloat"), QStringLiteral("toList"),
        QStringLiteral("toMutableList"), QStringLiteral("toSet"), QStringLiteral("toMap"), QStringLiteral("toTypedArray"),
        QStringLiteral("toByteArray"), QStringLiteral("toUByte"), QStringLiteral("toUInt"), QStringLiteral("toULong"),
        QStringLiteral("also"), QStringLiteral("takeIf"), QStringLiteral("orEmpty"), QStringLiteral("isNullOrEmpty"),
        QStringLiteral("isNotEmpty"), QStringLiteral("getOrNull"), QStringLiteral("getOrElse"), QStringLiteral("getOrPut"),
        QStringLiteral("sumOf"), QStringLiteral("forEach"), QStringLiteral("mapNotNull"), QStringLiteral("firstOrNull"),
        QStringLiteral("lastOrNull"), QStringLiteral("assertFailsWith"), QStringLiteral("hashCode"), QStringLiteral("compareTo"),
        QStringLiteral("coerceAtLeast"), QStringLiteral("coerceAtMost"), QStringLiteral("coerceIn"), QStringLiteral("copyInto"),
        QStringLiteral("copyOf"), QStringLiteral("contentEquals"), QStringLiteral("contentHashCode"), QStringLiteral("readBytes"),
        // NSObject / Foundation selectors that classes override.
        QStringLiteral("class"), QStringLiteral("superclass"), QStringLiteral("conformsToProtocol:"),
        QStringLiteral("respondsToSelector:"), QStringLiteral("isKindOfClass:"), QStringLiteral("isMemberOfClass:"),
        QStringLiteral("isEqual:"), QStringLiteral("hash"), QStringLiteral("alloc"), QStringLiteral("copy"),
        QStringLiteral("mutableCopy"), QStringLiteral("dealloc"), QStringLiteral("isProxy"),
        QStringLiteral("forwardInvocation:"), QStringLiteral("methodSignatureForSelector:"),
        QStringLiteral("forwardingTargetForSelector:"), QStringLiteral("performSelector:"),
        QStringLiteral("copyWithZone:"), QStringLiteral("initWithCoder:"), QStringLiteral("encodeWithCoder:"),
        QStringLiteral("debugDescription"), QStringLiteral("objectForKey:"), QStringLiteral("setObject:forKey:"),
        QStringLiteral("removeObjectForKey:"), QStringLiteral("addObject:"), QStringLiteral("removeObject:"),
        QStringLiteral("objectAtIndex:"), QStringLiteral("initWithFrame:"), QStringLiteral("layoutSubviews"),
        QStringLiteral("viewDidLoad"), QStringLiteral("setUp"), QStringLiteral("tearDown"), QStringLiteral("c_str"), QStringLiteral("to_bytes"),
    };
    return name.size() < 4 || names.contains(name);
}

// A method name distinctive enough to link a call on an unknown receiver
// (obj.name()) to the project's only definition of it.
bool isDistinctiveName(const QString &name)
{
    if (name.size() >= 8 || name.contains(QLatin1Char('_'))) {
        return true;
    }
    for (int index = 1; index < name.size(); ++index) {
        if (name.at(index).isUpper()) {
            return true;
        }
    }
    return false;
}

QString lastQualifierSegment(QString qualifier)
{
    qualifier = qualifier.trimmed();
    if (qualifier.endsWith(QStringLiteral("::"))) {
        qualifier.chop(2); // scoped call marker (ns::f, Type::new)
    }
    static const QRegularExpression newExpression(QStringLiteral(R"(^\(?\s*new\s+([A-Za-z_\\][\w\\]*))"));
    const auto newMatch = newExpression.match(qualifier);
    if (newMatch.hasMatch()) {
        qualifier = newMatch.captured(1);
    }
    for (const QString &separator : {QStringLiteral("::"), QStringLiteral("->"), QStringLiteral("\\"), QStringLiteral(".")}) {
        const int at = qualifier.lastIndexOf(separator);
        if (at >= 0) {
            qualifier = qualifier.mid(at + separator.size());
        }
    }
    qualifier.remove(QLatin1Char('$'));
    return qualifier.trimmed();
}

QString symbolKey(const QVariantMap &symbol)
{
    return QStringLiteral("%1|%2|%3")
        .arg(symbol.value(QStringLiteral("kind")).toString(), symbol.value(QStringLiteral("name")).toString())
        .arg(symbol.value(QStringLiteral("line")).toInt());
}

QString defaultCachePath(const QString &root)
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/lumencode/index");
    QDir().mkpath(base);
    const QByteArray hash = QCryptographicHash::hash(QFileInfo(root).absoluteFilePath().toUtf8(), QCryptographicHash::Sha1).toHex();
    return base + QLatin1Char('/') + QString::fromLatin1(hash.left(16)) + QStringLiteral(".json");
}

} // namespace

QStringList candidateFiles(const QString &root, int maxFiles)
{
    QStringList files;
    std::function<void(const QString &, int)> walk = [&](const QString &dirPath, int depth) {
        if (depth > 24 || files.size() >= maxFiles) {
            return;
        }
        const QFileInfoList entries = QDir(dirPath).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &entry : entries) {
            if (files.size() >= maxFiles) {
                return;
            }
            if (entry.isSymLink()) {
                continue;
            }
            if (entry.isDir()) {
                if (!isSkippedDirectory(entry.fileName())) {
                    walk(entry.absoluteFilePath(), depth + 1);
                }
            } else if (indexedSuffixes().contains(entry.suffix().toLower()) && entry.size() <= kMaxIndexedFileBytes
                       && !entry.fileName().contains(QStringLiteral(".min."))) {
                files.append(entry.absoluteFilePath());
            }
        }
    };
    walk(QFileInfo(root).absoluteFilePath(), 0);
    return files;
}

FileFacts factsFromAnalysis(const QVariantMap &analysis, qint64 size, qint64 modified)
{
    FileFacts facts;
    facts.path = QFileInfo(analysis.value(QStringLiteral("path")).toString()).absoluteFilePath();
    facts.language = analysis.value(QStringLiteral("language")).toString();
    facts.size = size;
    facts.modified = modified;

    auto signatureFor = [](const QVariantMap &symbol) {
        if (!symbol.contains(QStringLiteral("parameters"))) return QString();
        QStringList parameters;
        for (const QVariant &entry : symbol.value(QStringLiteral("parameters")).toList()) {
            const QVariantMap parameter = entry.toMap();
            QString text = parameter.value(QStringLiteral("text")).toString();
            if (text.isEmpty()) {
                const QString name = parameter.value(QStringLiteral("name")).toString();
                const QString type = parameter.value(QStringLiteral("type")).toString();
                text = type.isEmpty() ? name : name.isEmpty() ? type : name + QStringLiteral(": ") + type;
            }
            if (parameter.contains(QStringLiteral("default"))) text += QStringLiteral(" = ") + parameter.value(QStringLiteral("default")).toString();
            parameters.append(text);
        }
        return QLatin1Char('(') + parameters.join(QStringLiteral(", ")) + QLatin1Char(')');
    };

    std::function<void(const QVariantList &, const QString &, int)> visit = [&](const QVariantList &symbols,
                                                                               const QString &owner, int depth) {
        for (const QVariant &entry : symbols) {
            const QVariantMap symbol = entry.toMap();
            const QString kind = symbol.value(QStringLiteral("kind")).toString();
            const QString name = symbol.value(QStringLiteral("name")).toString();
            const QString key = symbolKey(symbol);
            if (isDefinitionKind(kind) && !name.isEmpty()) {
                Definition definition;
                definition.path = facts.path;
                definition.language = facts.language;
                definition.name = name;
                definition.kind = kind;
                definition.owner = owner;
                static const QRegularExpression namespaceDetail(QStringLiteral(R"(in namespace ([\w:]+))"));
                definition.scope = namespaceDetail.match(symbol.value(QStringLiteral("detail")).toString()).captured(1);
                definition.declaration = (facts.language == QStringLiteral("cpp") || facts.language == QStringLiteral("c"))
                    && symbol.value(QStringLiteral("detail")).toString().contains(QStringLiteral("declaration"))
                    && !symbol.value(QStringLiteral("detail")).toString().contains(QStringLiteral("declared elsewhere"));
                definition.key = key;
                definition.line = symbol.value(QStringLiteral("line")).toInt();
                definition.signature = signatureFor(symbol);
                facts.definitions.append(definition);
            }
            for (const QVariant &siteEntry : symbol.value(QStringLiteral("callSites")).toList()) {
                const QVariantMap site = siteEntry.toMap();
                CallSite callSite;
                callSite.fromKey = key;
                callSite.fromName = name;
                callSite.fromKind = kind;
                callSite.fromLine = symbol.value(QStringLiteral("line")).toInt();
                callSite.name = site.value(QStringLiteral("name")).toString();
                callSite.qualifier = site.value(QStringLiteral("qualifier")).toString();
                callSite.line = site.value(QStringLiteral("line")).toInt();
                facts.callSites.append(callSite);
            }
            if (depth < 3) {
                QString memberOwner = owner;
                if (kind == QStringLiteral("object") && name == QStringLiteral("Companion") && !owner.isEmpty()) {
                    memberOwner = owner; // Kotlin companion members are called as Owner.member()
                } else if (isTypeKind(kind) || kind == QStringLiteral("record") || kind == QStringLiteral("object")
                    || kind == QStringLiteral("module")) {
                    memberOwner = name;
                } else if (kind == QStringLiteral("impl") || kind == QStringLiteral("extension")
                           || kind == QStringLiteral("category")) {
                    // impl Display for Tokenizer / extension String: the type extended.
                    memberOwner = name.section(QStringLiteral(" for "), -1).trimmed();
                    memberOwner = memberOwner.section(QLatin1Char('<'), 0, 0).section(QLatin1Char(' '), 0, 0).trimmed();
                    memberOwner = memberOwner.section(QLatin1Char('('), 0, 0).trimmed();
                }
                visit(symbol.value(QStringLiteral("members")).toList(), memberOwner, depth + 1);
            }
        }
    };
    visit(analysis.value(QStringLiteral("symbols")).toList(), QString(), 0);

    for (const QVariant &siteEntry : analysis.value(QStringLiteral("moduleCallSites")).toList()) {
        const QVariantMap site = siteEntry.toMap();
        CallSite callSite;
        callSite.name = site.value(QStringLiteral("name")).toString();
        callSite.qualifier = site.value(QStringLiteral("qualifier")).toString();
        callSite.line = site.value(QStringLiteral("line")).toInt();
        facts.callSites.append(callSite);
    }
    for (const QVariant &entry : analysis.value(QStringLiteral("dependencies")).toList()) {
        const QVariantMap dependency = entry.toMap();
        Import import;
        import.target = dependency.value(QStringLiteral("target")).toString();
        const QString resolved = dependency.value(QStringLiteral("path")).toString();
        if (!resolved.isEmpty() && QFileInfo(resolved).isFile()) {
            import.path = QFileInfo(resolved).absoluteFilePath();
        }
        for (const QVariant &bindingEntry : dependency.value(QStringLiteral("bindings")).toList()) {
            const QVariantMap binding = bindingEntry.toMap();
            const QString local = binding.value(QStringLiteral("local")).toString().trimmed();
            const QString imported = binding.value(QStringLiteral("imported")).toString().trimmed();
            if (!local.isEmpty()) {
                import.bindings.insert(local, imported.isEmpty() ? local : imported);
            }
        }
        if (!import.path.isEmpty() || !import.bindings.isEmpty()) {
            facts.imports.append(import);
        }
    }
    for (const QVariant &entry : analysis.value(QStringLiteral("httpCalls")).toList()) {
        QVariantMap call = entry.toMap();
        if (call.contains(QStringLiteral("fromName"))) {
            call.insert(QStringLiteral("fromKey"), QStringLiteral("%1|%2|%3")
                                                       .arg(call.value(QStringLiteral("fromKind")).toString(),
                                                            call.value(QStringLiteral("fromName")).toString())
                                                       .arg(call.value(QStringLiteral("fromLine")).toInt()));
        }
        facts.httpCalls.append(call);
    }
    for (const QVariant &entry : analysis.value(QStringLiteral("routes")).toList()) {
        const QVariantMap route = entry.toMap();
        QVariantMap fact{{QStringLiteral("method"), route.value(QStringLiteral("method"))},
                         {QStringLiteral("path"), route.value(QStringLiteral("path"))},
                         {QStringLiteral("line"), route.value(QStringLiteral("line"))}};
        // app.use('/api/orders', ordersRouter): the router a prefix is mounted on.
        if (route.value(QStringLiteral("method")).toString() == QStringLiteral("USE")) {
            static const QRegularExpression mounted(QStringLiteral(R"(\buse\s*\(\s*['"][^'"]+['"]\s*,\s*([A-Za-z_$][\w$]*)\s*[,)])"));
            const QString router = mounted.match(route.value(QStringLiteral("snippet")).toString()).captured(1);
            if (!router.isEmpty()) {
                fact.insert(QStringLiteral("mounts"), router);
            }
        }
        facts.routes.append(fact);
    }
    return facts;
}

QVariantMap factsToVariant(const FileFacts &facts)
{
    QVariantMap map;
    map.insert(QStringLiteral("path"), facts.path);
    map.insert(QStringLiteral("language"), facts.language);
    map.insert(QStringLiteral("size"), facts.size);
    map.insert(QStringLiteral("modified"), facts.modified);
    if (facts.failed) {
        map.insert(QStringLiteral("failed"), true);
    }
    QVariantList definitions;
    for (const Definition &definition : facts.definitions) {
        QVariantMap item{{QStringLiteral("n"), definition.name}, {QStringLiteral("k"), definition.kind},
                         {QStringLiteral("l"), definition.line}};
        if (definition.declaration) {
            item.insert(QStringLiteral("d"), 1);
        }
        if (!definition.scope.isEmpty()) {
            item.insert(QStringLiteral("s"), definition.scope);
        }
        if (!definition.signature.isEmpty()) {
            item.insert(QStringLiteral("g"), definition.signature);
        }
        if (!definition.owner.isEmpty()) {
            item.insert(QStringLiteral("o"), definition.owner);
        }
        definitions.append(item);
    }
    map.insert(QStringLiteral("defs"), definitions);
    QVariantList imports;
    for (const Import &import : facts.imports) {
        QVariantMap bindings;
        for (auto it = import.bindings.cbegin(); it != import.bindings.cend(); ++it) {
            bindings.insert(it.key(), it.value());
        }
        imports.append(QVariantMap{{QStringLiteral("t"), import.target}, {QStringLiteral("p"), import.path},
                                   {QStringLiteral("b"), bindings}});
    }
    map.insert(QStringLiteral("imports"), imports);
    QVariantList sites;
    for (const CallSite &site : facts.callSites) {
        QVariantMap item{{QStringLiteral("n"), site.name}, {QStringLiteral("l"), site.line}};
        if (!site.fromKey.isEmpty()) {
            item.insert(QStringLiteral("f"), site.fromKey);
        }
        if (!site.qualifier.isEmpty()) {
            item.insert(QStringLiteral("q"), site.qualifier);
        }
        sites.append(item);
    }
    map.insert(QStringLiteral("sites"), sites);
    if (!facts.httpCalls.isEmpty()) {
        map.insert(QStringLiteral("http"), facts.httpCalls);
    }
    if (!facts.routes.isEmpty()) {
        map.insert(QStringLiteral("routes"), facts.routes);
    }
    return map;
}

FileFacts factsFromVariant(const QVariantMap &map)
{
    FileFacts facts;
    facts.path = map.value(QStringLiteral("path")).toString();
    facts.language = map.value(QStringLiteral("language")).toString();
    facts.size = map.value(QStringLiteral("size")).toLongLong();
    facts.modified = map.value(QStringLiteral("modified")).toLongLong();
    facts.failed = map.value(QStringLiteral("failed")).toBool();
    for (const QVariant &entry : map.value(QStringLiteral("defs")).toList()) {
        const QVariantMap item = entry.toMap();
        Definition definition;
        definition.path = facts.path;
        definition.language = facts.language;
        definition.name = item.value(QStringLiteral("n")).toString();
        definition.kind = item.value(QStringLiteral("k")).toString();
        definition.line = item.value(QStringLiteral("l")).toInt();
        definition.owner = item.value(QStringLiteral("o")).toString();
        definition.declaration = item.value(QStringLiteral("d")).toInt() == 1;
        definition.scope = item.value(QStringLiteral("s")).toString();
        definition.signature = item.value(QStringLiteral("g")).toString();
        definition.key = QStringLiteral("%1|%2|%3").arg(definition.kind, definition.name).arg(definition.line);
        facts.definitions.append(definition);
    }
    QHash<QString, const Definition *> byKey;
    for (const Definition &definition : std::as_const(facts.definitions)) {
        byKey.insert(definition.key, &definition);
    }
    for (const QVariant &entry : map.value(QStringLiteral("imports")).toList()) {
        const QVariantMap item = entry.toMap();
        Import import;
        import.target = item.value(QStringLiteral("t")).toString();
        import.path = item.value(QStringLiteral("p")).toString();
        const QVariantMap bindings = item.value(QStringLiteral("b")).toMap();
        for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
            import.bindings.insert(it.key(), it.value().toString());
        }
        facts.imports.append(import);
    }
    for (const QVariant &entry : map.value(QStringLiteral("sites")).toList()) {
        const QVariantMap item = entry.toMap();
        CallSite site;
        site.name = item.value(QStringLiteral("n")).toString();
        site.line = item.value(QStringLiteral("l")).toInt();
        site.qualifier = item.value(QStringLiteral("q")).toString();
        site.fromKey = item.value(QStringLiteral("f")).toString();
        if (!site.fromKey.isEmpty()) {
            const QStringList parts = site.fromKey.split(QLatin1Char('|'));
            if (parts.size() == 3) {
                site.fromKind = parts.at(0);
                site.fromName = parts.at(1);
                site.fromLine = parts.at(2).toInt();
            }
        }
        facts.callSites.append(site);
    }
    facts.routes = map.value(QStringLiteral("routes")).toList();
    facts.httpCalls = map.value(QStringLiteral("http")).toList();
    return facts;
}

namespace {

// Languages where a bare call inside a class can reach an inherited method.
bool hasImplicitSelf(const QString &language)
{
    static const QSet<QString> languages = {
        QStringLiteral("java"), QStringLiteral("csharp"), QStringLiteral("cpp"), QStringLiteral("c"),
        QStringLiteral("swift"), QStringLiteral("objc"), QStringLiteral("vbnet"), QStringLiteral("kotlin"),
        QStringLiteral("qml"),
    };
    return languages.contains(language);
}

bool isMethodDefinition(const Definition &definition)
{
    return definition.kind == QStringLiteral("method") || (!definition.owner.isEmpty() && !isTypeKind(definition.kind));
}

bool startsUpper(const QString &text)
{
    return !text.isEmpty() && text.at(0).isUpper();
}

QString definitionKey(const Definition &definition)
{
    return definition.path + QLatin1Char('|') + definition.kind + QLatin1Char('|') + definition.name;
}

// The body of a C/C++ prototype: same name and owning type, not itself a
// prototype, preferring the source file named like the header
// (geometry.h -> geometry.cpp), else the only candidate in the project.
int findDefinitionFor(const Snapshot &snapshot, const Definition &declaration)
{
    QVector<int> candidates;
    const auto range = snapshot.definitionsByName.equal_range(declaration.name);
    for (auto it = range.first; it != range.second; ++it) {
        const Definition &candidate = snapshot.allDefinitions.at(it.value());
        if (!candidate.declaration && candidate.path != declaration.path && candidate.owner == declaration.owner
            && isCallableKind(candidate.kind) && (candidate.language == QStringLiteral("cpp") || candidate.language == QStringLiteral("c"))) {
            candidates.append(it.value());
        }
    }
    const QString base = QFileInfo(declaration.path).completeBaseName();
    QVector<int> sameBase;
    for (int index : std::as_const(candidates)) {
        if (QFileInfo(snapshot.allDefinitions.at(index).path).completeBaseName() == base) {
            sameBase.append(index);
        }
    }
    if (sameBase.size() == 1) {
        return sameBase.first();
    }
    return candidates.size() == 1 ? candidates.first() : -1;
}

// C/C++ `a::b::f()` reaches a free function only in a matching namespace
// (`std::string(...)` is not the project's `string`).
bool scopeMatches(const QString &qualifier, const Definition &definition)
{
    QString scope = qualifier.trimmed();
    if (scope.endsWith(QStringLiteral("::"))) {
        scope.chop(2);
    }
    if (scope.isEmpty() || definition.scope.isEmpty()) {
        return scope.isEmpty() && definition.scope.isEmpty();
    }
    return scope.section(QStringLiteral("::"), -1) == definition.scope.section(QStringLiteral("::"), -1);
}

// The closest project descriptor is a conservative identity for resolving
// same-named façade classes in a multi-project checkout. Files in a shared
// folder have no such identity and deliberately remain ambiguous.
QString nearestProjectRoot(const QString &path, const QString &indexRoot)
{
    QDir directory(QFileInfo(path).absolutePath());
    const QString stop = QFileInfo(indexRoot).absoluteFilePath();
    while (true) {
        const QStringList descriptors = directory.entryList({QStringLiteral("*.csproj"), QStringLiteral("*.vbproj"),
                                                               QStringLiteral("*.fsproj"), QStringLiteral("*.xcodeproj")},
                                                              QDir::Files | QDir::NoDotAndDotDot);
        if (!descriptors.isEmpty()) {
            return directory.absolutePath();
        }
        if (directory.absolutePath() == stop || !directory.cdUp()) {
            break;
        }
    }
    return {};
}

// Resolution with the calling file's facts given explicitly (the live file
// may not be in the snapshot, or may have changed since the build).
bool resolveWith(const Snapshot &snapshot, const FileFacts &from, const CallSite &site, Edge *edge)
{
    if (site.name.isEmpty()) {
        return false;
    }
    const bool constructs = site.qualifier == QStringLiteral("new");
    const bool isCFamily = from.language == QStringLiteral("cpp") || from.language == QStringLiteral("c");
    const QString rawQualifier = constructs ? QString() : site.qualifier.trimmed();
    const bool qualified = !rawQualifier.isEmpty();

    // Same-file callees are resolved by the parser already.
    if (!qualified) {
        for (const Definition &own : from.definitions) {
            if (own.name == site.name && (isCallableKind(own.kind) || isTypeKind(own.kind))) {
                return false;
            }
        }
    }

    // The caller's own type, for this./self./$this-> and bare calls.
    QString callerOwner;
    for (const Definition &own : from.definitions) {
        if (own.key == site.fromKey) {
            callerOwner = own.owner;
            break;
        }
    }
    QString owner = lastQualifierSegment(rawQualifier);
    const bool selfCall = owner == QStringLiteral("this") || owner == QStringLiteral("self")
        || owner == QStringLiteral("Me") || owner == QStringLiteral("MyBase") || owner == QStringLiteral("base")
        || owner == QStringLiteral("super") || owner == QStringLiteral("parent") || owner == QStringLiteral("static");
    if (selfCall) {
        owner = callerOwner;
    } else if (!owner.isEmpty()) {
        // `use Shop\Billing\InvoiceMailer as Mailer`: Mailer::send is InvoiceMailer's.
        for (const Import &import : from.imports) {
            const QString imported = import.bindings.value(owner);
            if (!imported.isEmpty() && imported != QStringLiteral("*") && imported != QStringLiteral("default")) {
                owner = imported;
                break;
            }
        }
    }
    // Qualifier head (first segment) - a module / namespace binding if imported.
    QString head = rawQualifier;
    for (const QString &separator : {QStringLiteral("."), QStringLiteral("::"), QStringLiteral("->"), QStringLiteral("(")}) {
        const int at = head.indexOf(separator);
        if (at > 0) {
            head = head.left(at);
        }
    }
    head.remove(QLatin1Char('$'));

    // A name (or receiver) bound to an external package cannot be project code.
    for (const Import &import : from.imports) {
        if (!import.path.isEmpty()) {
            continue;
        }
        if ((!qualified && import.bindings.contains(site.name)) || (qualified && !selfCall && import.bindings.contains(head))) {
            return false;
        }
    }

    auto eligible = [&](const Definition &definition) {
        if (definition.path == from.path) {
            return false;
        }
        return constructs ? (isTypeKind(definition.kind) || definition.kind == QStringLiteral("constructor"))
                          : (isCallableKind(definition.kind) || isTypeKind(definition.kind));
    };
    // Can a call of this shape reach the definition at all?
    auto shapeFits = [&](const Definition &definition) {
        if (constructs) {
            // A nested type is constructed bare only inside its owner
            // (`::Logger.new` is not Middleware::Logger).
            return definition.owner.isEmpty() || definition.owner == callerOwner;
        }
        const bool method = isMethodDefinition(definition);
        if (!qualified) {
            // Bare call: a free function / type, or a method of the caller's own
            // (possibly partial or inherited) type.
            if (!method) {
                // A nested type is named bare only inside its owner.
                return definition.owner.isEmpty() || !isTypeKind(definition.kind) || definition.owner == callerOwner;
            }
            if (!callerOwner.isEmpty() && definition.owner == callerOwner) {
                return true;
            }
            // Inherited methods: trusted in managed languages; in C/C++ a bare
            // call more often reaches a framework base class (Qt, STL).
            return hasImplicitSelf(from.language) && !isCFamily;
        }
        if (selfCall) {
            return method && (owner.isEmpty() || definition.owner == owner || hasImplicitSelf(from.language)
                              || from.language == QStringLiteral("php") || from.language == QStringLiteral("python"));
        }
        if (!owner.isEmpty() && definition.owner == owner) {
            return true; // Mailer::send, Guard.NotNull
        }
        // `ns::f()` / `Module::f`: a scope qualifier, not a runtime receiver,
        // can reach free functions (namespace members carry no owner).
        if (!method && rawQualifier.contains(QStringLiteral("::"))) {
            return !isCFamily || scopeMatches(rawQualifier, definition);
        }
        // Type-like receiver that is not the owner (Arrays.asList): not ours.
        if (startsUpper(owner) && !definition.owner.isEmpty()) {
            return false;
        }
        return method;
    };
    auto fill = [&](const Definition &target, const QString &via, const QString &confidence) {
        // A call that reaches a C/C++ prototype lands on its body.
        const Definition *resolved = &target;
        if (target.declaration) {
            const auto bodyIt = snapshot.definitionForDeclaration.constFind(definitionKey(target));
            if (bodyIt != snapshot.definitionForDeclaration.constEnd()) {
                resolved = &snapshot.allDefinitions.at(bodyIt.value());
            }
        }
        const Definition &definition = *resolved;
        if (definition.path == from.path) {
            return false; // the body is in the calling file: a same-file call
        }
        edge->fromPath = from.path;
        edge->fromKey = site.fromKey;
        edge->fromName = site.fromName;
        edge->fromKind = site.fromKind;
        edge->fromLine = site.fromLine;
        edge->siteLine = site.line;
        edge->to = definition;
        edge->via = via;
        edge->confidence = confidence;
        return true;
    };
    auto definitionsIn = [&](const QString &path, const QString &name) {
        QVector<const Definition *> found;
        const auto fileIt = snapshot.files.constFind(path);
        if (fileIt == snapshot.files.constEnd()) {
            return found;
        }
        for (const Definition &definition : fileIt->definitions) {
            if (definition.name == name && eligible(definition)) {
                found.append(&definition);
            }
        }
        return found;
    };

    // A name a module only re-exports (`from .app import Flask` in a package
    // __init__.py, `export { x } from './x'` in a barrel): follow it.
    std::function<QVector<const Definition *>(const QString &, const QString &, int)> followReexport =
        [&](const QString &modulePath, const QString &name, int hops) {
            QVector<const Definition *> found;
            const auto fileIt = snapshot.files.constFind(modulePath);
            if (hops > 3 || fileIt == snapshot.files.constEnd()) {
                return found;
            }
            for (const Import &reexport : fileIt->imports) {
                if (reexport.path.isEmpty() || reexport.path == modulePath) {
                    continue;
                }
                QString imported = name;
                if (!reexport.bindings.isEmpty()) {
                    if (!reexport.bindings.contains(name)) {
                        continue;
                    }
                    imported = reexport.bindings.value(name);
                    if (imported == QStringLiteral("*") || imported == QStringLiteral("default")) {
                        continue;
                    }
                }
                found = definitionsIn(reexport.path, imported);
                if (found.isEmpty() && !reexport.bindings.isEmpty()) {
                    found = followReexport(reexport.path, imported, hops + 1);
                }
                if (!found.isEmpty()) {
                    return found;
                }
            }
            return found;
        };
    auto reexported = [&](const QString &modulePath, const QString &name) { return followReexport(modulePath, name, 1); };

    // 1. Imports: the name (or the receiver) is bound to a project file, or the
    //    file is included / star-imported and the call's shape fits. C/C++
    //    includes count transitively (x.cpp -> x.h -> index.h).
    QVector<Import> imports = from.imports;
    if (isCFamily) {
        QSet<QString> seen;
        for (const Import &import : std::as_const(imports)) {
            seen.insert(import.path);
        }
        for (int index = 0; index < imports.size() && imports.size() < 200; ++index) {
            const auto fileIt = snapshot.files.constFind(imports.at(index).path);
            if (fileIt == snapshot.files.constEnd()) {
                continue;
            }
            for (const Import &nested : fileIt->imports) {
                if (!nested.path.isEmpty() && !seen.contains(nested.path)) {
                    seen.insert(nested.path);
                    imports.append(nested);
                }
            }
        }
    }
    for (const Import &import : std::as_const(imports)) {
        if (import.path.isEmpty()) {
            continue;
        }
        QString wanted = site.name;
        bool bound = false;
        if (!qualified && import.bindings.contains(site.name)) {
            const QString imported = import.bindings.value(site.name);
            if (imported != QStringLiteral("*") && imported != QStringLiteral("default")) {
                wanted = imported;
            }
            bound = true;
        }
        const QString receiverBinding = import.bindings.value(rawQualifier);
        const bool moduleReceiver = qualified && !selfCall
            && ((import.bindings.contains(head) && rawQualifier == head) // utils.normalizeType(), helpers.wrapper()
                || receiverBinding == QStringLiteral("*")); // import a.b -> a.b.func()
        const bool typeReceiver = qualified && !selfCall && import.bindings.contains(head); // HTTPException.x, RegexMatcher::new
        QVector<const Definition *> found = definitionsIn(import.path, wanted);
        if (found.isEmpty() && (bound || moduleReceiver)) {
            found = reexported(import.path, wanted);
        }
        for (const Definition *candidate : std::as_const(found)) {
            bool accept = false;
            if (bound || constructs) {
                accept = !isMethodDefinition(*candidate) || constructs;
            } else if (moduleReceiver && candidate->owner.isEmpty()) {
                accept = true;
            } else if (typeReceiver && !owner.isEmpty() && candidate->owner == owner) {
                accept = true;
            } else if (import.bindings.isEmpty() && !qualified && !isMethodDefinition(*candidate)) {
                accept = true; // #include / star import: free functions and types
            } else if (import.bindings.isEmpty() && qualified && !owner.isEmpty() && candidate->owner == owner) {
                accept = true;
            } else if (import.bindings.isEmpty() && rawQualifier.contains(QStringLiteral("::")) && !isMethodDefinition(*candidate)
                       && (!isCFamily || scopeMatches(rawQualifier, *candidate))) {
                accept = true; // #include "x.h" + ns::f(): a namespace member declared in the header
            }
            if (accept) {
                return fill(*candidate, QStringLiteral("import"), QStringLiteral("high"));
            }
        }
    }

    // Candidates project-wide.
    QVector<const Definition *> candidates;
    const auto range = snapshot.definitionsByName.equal_range(site.name);
    for (auto it = range.first; it != range.second; ++it) {
        const Definition &definition = snapshot.allDefinitions.at(it.value());
        if (eligible(definition) && shapeFits(definition)) {
            candidates.append(&definition);
        }
    }
    if (candidates.isEmpty()) {
        return false;
    }
    {
        // A C/C++ prototype and its body are one callee.
        QVector<const Definition *> merged;
        for (const Definition *candidate : std::as_const(candidates)) {
            if (candidate->declaration && snapshot.definitionForDeclaration.contains(definitionKey(*candidate))) {
                const Definition *body = &snapshot.allDefinitions.at(snapshot.definitionForDeclaration.value(definitionKey(*candidate)));
                if (std::find(candidates.cbegin(), candidates.cend(), body) != candidates.cend()) {
                    continue;
                }
            }
            merged.append(candidate);
        }
        candidates = merged;
    }
    if (constructs) {
        // new Invoice(...) finds the class and its constructors: keep the types.
        QVector<const Definition *> types;
        for (const Definition *candidate : std::as_const(candidates)) {
            if (isTypeKind(candidate->kind)) {
                types.append(candidate);
            }
        }
        if (!types.isEmpty()) {
            candidates = types;
        }
    }

    // 2. The receiver names the owning type (Mailer::send, Guard.NotNull,
    //    $this->x in a trait or partial class).
    if (!owner.isEmpty()) {
        QVector<const Definition *> owned;
        for (const Definition *candidate : std::as_const(candidates)) {
            if (candidate->owner == owner) {
                owned.append(candidate);
            }
        }
        if (owned.size() == 1) {
            return fill(*owned.first(), QStringLiteral("qualifier"), QStringLiteral("medium"));
        }
        if (owned.size() > 1) {
            const QString callerProject = nearestProjectRoot(from.path, snapshot.root);
            if (!callerProject.isEmpty()) {
                QVector<const Definition *> local;
                for (const Definition *candidate : std::as_const(owned)) {
                    if (nearestProjectRoot(candidate->path, snapshot.root) == callerProject) {
                        local.append(candidate);
                    }
                }
                if (local.size() == 1) {
                    return fill(*local.first(), QStringLiteral("qualifier-project"), QStringLiteral("medium"));
                }
            }
            return false;
        }
    }

    // C/C++: a callee is only reachable through the headers the file
    // includes (transitively). Without that, a same-named framework method
    // (Qt, STL) is the likelier target.
    if (isCFamily) {
        QSet<QString> reachable{from.path};
        QStringList queue;
        for (const Import &import : from.imports) {
            if (!import.path.isEmpty() && !reachable.contains(import.path)) {
                reachable.insert(import.path);
                queue.append(import.path);
            }
        }
        for (int index = 0; index < queue.size() && reachable.size() < 400; ++index) {
            const auto fileIt = snapshot.files.constFind(queue.at(index));
            if (fileIt == snapshot.files.constEnd()) {
                continue;
            }
            for (const Import &import : fileIt->imports) {
                if (!import.path.isEmpty() && !reachable.contains(import.path)) {
                    reachable.insert(import.path);
                    queue.append(import.path);
                }
            }
        }
        QVector<const Definition *> visible;
        for (const Definition *candidate : std::as_const(candidates)) {
            bool seen = reachable.contains(candidate->path);
            for (int declaration : snapshot.declarationsForDefinition.value(definitionKey(*candidate))) {
                seen = seen || reachable.contains(snapshot.allDefinitions.at(declaration).path);
            }
            if (seen) {
                visible.append(candidate);
            }
        }
        candidates = visible;
        if (candidates.isEmpty()) {
            return false;
        }
    }

    // 3. Same directory / package (bare calls and constructions only: a
    //    receiver of unknown type is no evidence of locality).
    if (!qualified || constructs) {
        const QString directory = QFileInfo(from.path).absolutePath();
        QVector<const Definition *> sameDirectory;
        for (const Definition *candidate : std::as_const(candidates)) {
            if (QFileInfo(candidate->path).absolutePath() == directory) {
                sameDirectory.append(candidate);
            }
        }
        if (sameDirectory.size() == 1) {
            return fill(*sameDirectory.first(), QStringLiteral("package"), QStringLiteral("medium"));
        }
        if (sameDirectory.size() > 1) {
            return false;
        }
    }

    // 4. Unique, non-generic name project-wide. On a receiver of unknown type
    //    only a distinctive method name is trusted.
    if (candidates.size() == 1 && !isGenericName(site.name)) {
        const Definition &candidate = *candidates.first();
        const bool ownMethod = selfCall && !owner.isEmpty() && candidate.owner == owner;
        const bool acceptable = !qualified || constructs || ownMethod || isDistinctiveName(site.name);
        if (acceptable) {
            return fill(candidate, QStringLiteral("unique-name"), QStringLiteral("low"));
        }
    }
    return false;
}

} // namespace

bool Snapshot::resolve(const QString &path, const CallSite &site, Edge *edge) const
{
    const auto fileIt = files.constFind(path);
    return fileIt != files.constEnd() && resolveWith(*this, fileIt.value(), site, edge);
}

namespace {

struct CacheEntry
{
    qint64 size = 0;
    qint64 modified = 0;
    QVariantMap facts;
};

// Facts depend on the parser as well as the file: a rebuilt or upgraded helper
// invalidates the whole cache.
QString helperFingerprint(const QString &helperPath)
{
    const QFileInfo info(helperPath);
    return QStringLiteral("%1:%2").arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch());
}

QHash<QString, CacheEntry> loadCache(const QString &cachePath, const QString &root, const QString &fingerprint)
{
    QHash<QString, CacheEntry> cache;
    QFile file(cachePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return cache;
    }
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.value(QStringLiteral("version")).toInt() != kCacheVersion
        || object.value(QStringLiteral("root")).toString() != root
        || object.value(QStringLiteral("helper")).toString() != fingerprint) {
        return cache;
    }
    const QJsonObject files = object.value(QStringLiteral("files")).toObject();
    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        const QVariantMap facts = it.value().toObject().toVariantMap();
        CacheEntry entry;
        entry.size = facts.value(QStringLiteral("size")).toLongLong();
        entry.modified = facts.value(QStringLiteral("modified")).toLongLong();
        entry.facts = facts;
        cache.insert(it.key(), entry);
    }
    return cache;
}

void saveCache(const QString &cachePath, const QString &root, const QString &fingerprint,
               const QHash<QString, FileFacts> &files)
{
    QJsonObject filesObject;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        filesObject.insert(it.key(), QJsonObject::fromVariantMap(factsToVariant(it.value())));
    }
    QJsonObject object;
    object.insert(QStringLiteral("version"), kCacheVersion);
    object.insert(QStringLiteral("root"), root);
    object.insert(QStringLiteral("helper"), fingerprint);
    object.insert(QStringLiteral("files"), filesObject);
    QDir().mkpath(QFileInfo(cachePath).absolutePath());
    QFile file(cachePath + QStringLiteral(".tmp"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
        file.close();
        QFile::remove(cachePath);
        QFile::rename(cachePath + QStringLiteral(".tmp"), cachePath);
    }
}

// Run the helper on a batch of files; returns the facts it produced (files it
// crashed on are missing and get retried one by one by the caller).
QHash<QString, QVariantMap> runHelperBatch(const QString &helperPath, const QStringList &paths)
{
    QHash<QString, QVariantMap> produced;
    QProcess helper;
    helper.start(helperPath, {QStringLiteral("--index-facts")});
    if (!helper.waitForStarted(5000)) {
        return produced;
    }
    helper.write(paths.join(QLatin1Char('\n')).toUtf8());
    helper.write("\n");
    helper.closeWriteChannel();
    const int timeoutMs = 20000 + 400 * paths.size();
    helper.waitForFinished(timeoutMs);
    if (helper.state() != QProcess::NotRunning) {
        helper.kill();
        helper.waitForFinished(2000);
    }
    const QList<QByteArray> lines = helper.readAllStandardOutput().split('\n');
    for (const QByteArray &line : lines) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject()) {
            continue;
        }
        const QVariantMap facts = document.object().toVariantMap();
        produced.insert(facts.value(QStringLiteral("path")).toString(), facts);
    }
    return produced;
}

} // namespace

SnapshotPtr build(const BuildOptions &options)
{
    QElapsedTimer timer;
    timer.start();
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->root = QFileInfo(options.root).absoluteFilePath();
    const QString cachePath = options.cachePath.isEmpty() ? defaultCachePath(snapshot->root) : options.cachePath;

    QStringList paths = candidateFiles(snapshot->root, options.maxFiles);
    for (const QString &extraRoot : options.alsoRoots) {
        if (paths.size() >= options.maxFiles) {
            break;
        }
        paths += candidateFiles(QFileInfo(extraRoot).absoluteFilePath(), options.maxFiles - paths.size());
    }
    paths.removeDuplicates();
    const QString fingerprint = helperFingerprint(options.helperPath);
    const QHash<QString, CacheEntry> cache = loadCache(cachePath, snapshot->root, fingerprint);
    QStringList stale;
    int reused = 0;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        const auto cached = cache.constFind(path);
        if (cached != cache.constEnd() && cached->size == info.size()
            && cached->modified == info.lastModified().toMSecsSinceEpoch()) {
            snapshot->files.insert(path, factsFromVariant(cached->facts));
            ++reused;
        } else {
            stale.append(path);
        }
    }

    // Stale files: helper batches in parallel; crashes are retried per file.
    QList<QStringList> batches;
    for (int start = 0; start < stale.size(); start += options.batchSize) {
        batches.append(stale.mid(start, options.batchSize));
    }
    QMutex mutex;
    int done = reused;
    int failed = 0;
    std::function<void(const QStringList &)> runBatch = [&](const QStringList &batch) {
        if (options.cancel && options.cancel->load()) {
            return;
        }
        QHash<QString, QVariantMap> produced = runHelperBatch(options.helperPath, batch);
        for (const QString &path : batch) {
            if (!produced.contains(path) && batch.size() > 1) {
                const QHash<QString, QVariantMap> single = runHelperBatch(options.helperPath, {path});
                if (single.contains(path)) {
                    produced.insert(path, single.value(path));
                }
            }
        }
        QMutexLocker locker(&mutex);
        for (const QString &path : batch) {
            FileFacts facts;
            if (produced.contains(path)) {
                facts = factsFromVariant(produced.value(path));
                facts.path = path;
            } else {
                facts.path = path;
                facts.failed = true;
                ++failed;
            }
            const QFileInfo info(path);
            facts.size = info.size();
            facts.modified = info.lastModified().toMSecsSinceEpoch();
            snapshot->files.insert(path, facts);
        }
        done += batch.size();
        if (options.progress) {
            options.progress(done, paths.size());
        }
    };
    QtConcurrent::blockingMap(batches, runBatch);
    if (!stale.isEmpty() && !(options.cancel && options.cancel->load())) {
        saveCache(cachePath, snapshot->root, fingerprint, snapshot->files);
    }

    // Definitions and the edge graph.
    for (auto it = snapshot->files.cbegin(); it != snapshot->files.cend(); ++it) {
        for (const Definition &definition : it->definitions) {
            snapshot->definitionsByName.insert(definition.name, snapshot->allDefinitions.size());
            snapshot->allDefinitions.append(definition);
        }
    }
    for (int index = 0; index < snapshot->allDefinitions.size(); ++index) {
        const Definition &declaration = snapshot->allDefinitions.at(index);
        if (!declaration.declaration) {
            continue;
        }
        const int body = findDefinitionFor(*snapshot, declaration);
        if (body >= 0) {
            snapshot->definitionForDeclaration.insert(definitionKey(declaration), body);
            snapshot->declarationsForDefinition[definitionKey(snapshot->allDefinitions.at(body))].append(index);
        }
    }

    QHash<QString, int> viaCounts;
    QHash<QString, int> crossByLanguage;
    int sites = 0;
    int unresolved = 0;
    for (auto it = snapshot->files.cbegin(); it != snapshot->files.cend(); ++it) {
        for (const CallSite &site : it->callSites) {
            ++sites;
            Edge edge;
            if (!resolveWith(*snapshot, it.value(), site, &edge)) {
                ++unresolved;
                continue;
            }
            const int index = snapshot->edges.size();
            snapshot->edges.append(edge);
            snapshot->outgoingByFromKey[edge.fromPath + QLatin1Char('|') + edge.fromKey].append(index);
            snapshot->incomingByToKey[edge.to.path + QLatin1Char('|') + edge.to.kind + QLatin1Char('|') + edge.to.name].append(index);
            viaCounts[edge.via] += 1;
            crossByLanguage[it->language] += 1;
        }
    }

    // Router files mounted under a prefix (Express app.use('/api', router)).
    for (auto it = snapshot->files.cbegin(); it != snapshot->files.cend(); ++it) {
        for (const QVariant &entry : it->routes) {
            const QVariantMap route = entry.toMap();
            const QString router = route.value(QStringLiteral("mounts")).toString();
            if (router.isEmpty()) {
                continue;
            }
            for (const Import &import : it->imports) {
                if (!import.path.isEmpty() && import.bindings.contains(router)) {
                    snapshot->mountPrefixes[import.path].append(route.value(QStringLiteral("path")).toString());
                }
            }
        }
    }

    int httpCalls = 0;
    for (auto it = snapshot->files.cbegin(); it != snapshot->files.cend(); ++it) {
        for (const QVariant &call : it->httpCalls) {
            ++httpCalls;
            for (const Edge &edge : resolveHttpCall(*snapshot, it->path, call.toMap())) {
                snapshot->httpIncomingByRoute[edge.to.path + QLatin1Char('|') + QString::number(edge.to.line)].append(
                    snapshot->httpEdges.size());
                snapshot->httpEdges.append(edge);
            }
        }
    }

    QVariantMap via;
    for (auto it = viaCounts.cbegin(); it != viaCounts.cend(); ++it) {
        via.insert(it.key(), it.value());
    }
    QVariantMap perLanguage;
    for (auto it = crossByLanguage.cbegin(); it != crossByLanguage.cend(); ++it) {
        perLanguage.insert(it.key(), it.value());
    }
    snapshot->stats = QVariantMap{
        {QStringLiteral("files"), snapshot->files.size()},
        {QStringLiteral("reusedFromCache"), reused},
        {QStringLiteral("analysed"), stale.size()},
        {QStringLiteral("failed"), failed},
        {QStringLiteral("definitions"), snapshot->allDefinitions.size()},
        {QStringLiteral("callSites"), sites},
        {QStringLiteral("crossFileEdges"), snapshot->edges.size()},
        {QStringLiteral("unresolvedCallSites"), unresolved},
        {QStringLiteral("edgesByEvidence"), via},
        {QStringLiteral("edgesByLanguage"), perLanguage},
        {QStringLiteral("declarationsPaired"), snapshot->definitionForDeclaration.size()},
        {QStringLiteral("httpCalls"), httpCalls},
        {QStringLiteral("httpEdges"), snapshot->httpEdges.size()},
        {QStringLiteral("buildMs"), timer.elapsed()},
        {QStringLiteral("cachePath"), cachePath},
    };
    return snapshot;
}

QVector<Edge> resolveHttpCall(const Snapshot &snapshot, const QString &fromPath, const QVariantMap &call)
{
    QVector<Edge> result;
    const QString url = call.value(QStringLiteral("url")).toString();
    const QStringList urlParts = HttpClients::urlSegments(url);
    if (urlParts.isEmpty()) {
        return result;
    }
    const bool openTail = url.endsWith(QLatin1Char('*'));
    const QString method = call.value(QStringLiteral("method")).toString();
    struct Candidate { const FileFacts *file; QVariantMap route; int score; int literals; };
    QVector<Candidate> candidates;
    int bestScore = 0;
    for (auto it = snapshot.files.cbegin(); it != snapshot.files.cend(); ++it) {
        for (const QVariant &entry : it->routes) {
            const QVariantMap route = entry.toMap();
            if (route.value(QStringLiteral("method")).toString() == QStringLiteral("USE")
                || !HttpClients::methodsCompatible(method, route.value(QStringLiteral("method")).toString())) {
                continue;
            }
            const QString routePath = route.value(QStringLiteral("path")).toString();
            QStringList variants{routePath};
            for (const QString &prefix : snapshot.mountPrefixes.value(it->path)) {
                QString mounted = prefix;
                while (mounted.endsWith(QLatin1Char('/'))) {
                    mounted.chop(1);
                }
                variants.append(mounted + (routePath.startsWith(QLatin1Char('/')) ? routePath : QLatin1Char('/') + routePath));
            }
            int score = 0;
            QStringList routeParts;
            for (const QString &variant : std::as_const(variants)) {
                const QStringList parts = HttpClients::routeSegments(variant);
                const int variantScore = HttpClients::matchRoute(urlParts, parts, openTail);
                if (variantScore > score) {
                    score = variantScore;
                    routeParts = parts;
                }
            }
            if (score == 0 || score < bestScore) {
                continue;
            }
            if (score > bestScore) {
                candidates.clear();
                bestScore = score;
            }
            // Specificity: segments where URL and route agree on a literal
            // (/search/x beats /:user/bob, where only wildcards line up).
            const int offset = score == 2 ? 0 : urlParts.size() - routeParts.size();
            int literals = 0;
            for (int index = 0; index < routeParts.size() && offset + index < urlParts.size(); ++index) {
                const QString &part = routeParts.at(index);
                literals += (part != QStringLiteral("*") && part == urlParts.at(offset + index)) ? 1 : 0;
            }
            const bool root = routeParts.size() == 1 && routeParts.first().isEmpty();
            if (literals == 0 && !root) {
                continue;
            }
            candidates.append({&it.value(), route, score, literals});
        }
    }
    // The most specific routes win (/users/me over /users/:id); too many
    // equally good tail matches means the guess is not worth showing.
    int bestLiterals = 0;
    for (const Candidate &candidate : std::as_const(candidates)) {
        bestLiterals = qMax(bestLiterals, candidate.literals);
    }
    QVector<Candidate> best;
    for (const Candidate &candidate : std::as_const(candidates)) {
        if (candidate.literals == bestLiterals) {
            best.append(candidate);
        }
    }
    // A route in the calling file itself (a test that defines and calls its
    // own routes) wins; otherwise routes defined in tests only win when
    // nothing else matches.
    QVector<Candidate> sameFile;
    for (const Candidate &candidate : std::as_const(best)) {
        if (candidate.file->path == fromPath) {
            sameFile.append(candidate);
        }
    }
    if (!sameFile.isEmpty()) {
        best = sameFile;
    }
    static const QRegularExpression testPath(QStringLiteral(R"((^|/)(tests?|__tests__|spec|specs)/|\.(test|spec)\.)"));
    QVector<Candidate> production;
    for (const Candidate &candidate : std::as_const(best)) {
        if (!testPath.match(candidate.file->path).hasMatch()) {
            production.append(candidate);
        }
    }
    if (!production.isEmpty()) {
        best = production;
    }
    if (qEnvironmentVariableIsSet("LUMENCODE_DEBUG_HTTP")) {
        for (const Candidate &candidate : std::as_const(candidates)) {
            qWarning("http %s %s -> %s %s (%s) score %d literals %d", qPrintable(method), qPrintable(url),
                     qPrintable(candidate.route.value(QStringLiteral("method")).toString()),
                     qPrintable(candidate.route.value(QStringLiteral("path")).toString()), qPrintable(candidate.file->path),
                     candidate.score, candidate.literals);
        }
    }
    if (best.isEmpty() || best.size() > (bestScore == 2 ? 3 : 2)) {
        return result;
    }
    for (const Candidate &candidate : std::as_const(best)) {
        Edge edge;
        edge.fromPath = fromPath;
        edge.fromKey = call.value(QStringLiteral("fromKey")).toString();
        edge.fromName = call.value(QStringLiteral("fromName")).toString();
        edge.fromKind = call.value(QStringLiteral("fromKind")).toString();
        edge.fromLine = call.value(QStringLiteral("fromLine")).toInt();
        edge.siteLine = call.value(QStringLiteral("line")).toInt();
        edge.to.path = candidate.file->path;
        edge.to.language = candidate.file->language;
        edge.to.kind = QStringLiteral("route");
        edge.to.name = QStringLiteral("%1 %2").arg(candidate.route.value(QStringLiteral("method")).toString().toUpper(),
                                                   candidate.route.value(QStringLiteral("path")).toString());
        edge.to.line = candidate.route.value(QStringLiteral("line")).toInt();
        edge.to.key = QStringLiteral("route|%1|%2").arg(edge.to.name).arg(edge.to.line);
        edge.via = QStringLiteral("http %1 %2").arg(method, url);
        edge.confidence = bestScore == 2 ? QStringLiteral("medium") : QStringLiteral("low");
        result.append(edge);
    }
    return result;
}

QVariantMap augmentAnalysis(const QVariantMap &analysis, const SnapshotPtr &snapshot)
{
    if (!snapshot) {
        return analysis;
    }
    const QString path = QFileInfo(analysis.value(QStringLiteral("path")).toString()).absoluteFilePath();
    const QFileInfo info(path);
    const FileFacts live = factsFromAnalysis(analysis, info.size(), info.lastModified().toMSecsSinceEpoch());

    // Outgoing: resolve this file's current call sites against the index.
    QHash<QString, QVector<Edge>> outgoing;
    for (const CallSite &site : live.callSites) {
        Edge edge;
        if (!site.fromKey.isEmpty() && resolveWith(*snapshot, live, site, &edge)) {
            outgoing[site.fromKey].append(edge);
        }
    }

    // HTTP: this file's client calls -> routes; this file's routes <- clients.
    QHash<QString, QVector<Edge>> httpOutgoing;
    QVariantList httpCalls = analysis.value(QStringLiteral("httpCalls")).toList();
    for (int index = 0; index < live.httpCalls.size() && index < httpCalls.size(); ++index) {
        const QVector<Edge> edges = resolveHttpCall(*snapshot, path, live.httpCalls.at(index).toMap());
        QVariantMap call = httpCalls.at(index).toMap();
        QVariantList routes;
        for (const Edge &edge : edges) {
            if (!edge.fromKey.isEmpty()) {
                httpOutgoing[edge.fromKey].append(edge);
            }
            routes.append(QVariantMap{{QStringLiteral("name"), edge.to.name}, {QStringLiteral("path"), edge.to.path},
                                      {QStringLiteral("line"), edge.to.line}, {QStringLiteral("confidence"), edge.confidence}});
        }
        if (!routes.isEmpty()) {
            call.insert(QStringLiteral("routes"), routes);
            httpCalls[index] = call;
        }
    }
    QHash<int, QVector<int>> incomingByRouteLine;
    QVariantList routes = analysis.value(QStringLiteral("routes")).toList();
    for (int index = 0; index < routes.size(); ++index) {
        QVariantMap route = routes.at(index).toMap();
        const int routeLine = route.value(QStringLiteral("line")).toInt();
        const QVector<int> incoming = snapshot->httpIncomingByRoute.value(path + QLatin1Char('|') + QString::number(routeLine));
        if (incoming.isEmpty()) {
            continue;
        }
        incomingByRouteLine.insert(routeLine, incoming);
        QVariantList clients;
        for (int edgeIndex : incoming) {
            const Edge &edge = snapshot->httpEdges.at(edgeIndex);
            clients.append(QVariantMap{
                {QStringLiteral("name"), edge.fromName.isEmpty() ? QStringLiteral("(top level of %1)").arg(QFileInfo(edge.fromPath).fileName())
                                                                 : edge.fromName},
                {QStringLiteral("path"), edge.fromPath}, {QStringLiteral("line"), edge.siteLine},
                {QStringLiteral("detail"), edge.via}, {QStringLiteral("confidence"), edge.confidence}});
        }
        route.insert(QStringLiteral("calledFrom"), clients);
        routes[index] = route;
    }

    auto relationTo = [&](const Definition &definition, const QString &detail, const QString &confidence) {
        return QVariantMap{
            {QStringLiteral("kind"), definition.kind}, {QStringLiteral("name"), definition.name},
            {QStringLiteral("line"), definition.line}, {QStringLiteral("path"), definition.path},
            {QStringLiteral("sourcePath"), definition.path}, {QStringLiteral("language"), definition.language},
            {QStringLiteral("sourceLanguage"), definition.language}, {QStringLiteral("detail"), detail},
            {QStringLiteral("sourceMode"), QStringLiteral("ast")}, {QStringLiteral("confidence"), confidence},
            {QStringLiteral("owner"), definition.owner},
            {QStringLiteral("calls"), QVariantList{}}, {QStringLiteral("calledBy"), QVariantList{}},
        };
    };
    auto relationFrom = [&](const Edge &edge) {
        const QString language = snapshot->files.value(edge.fromPath).language;
        const bool topLevel = edge.fromKey.isEmpty();
        return QVariantMap{
            {QStringLiteral("kind"), topLevel ? QStringLiteral("module") : edge.fromKind},
            {QStringLiteral("name"), topLevel ? QStringLiteral("(top level of %1)").arg(QFileInfo(edge.fromPath).fileName()) : edge.fromName},
            {QStringLiteral("line"), topLevel ? edge.siteLine : edge.fromLine}, {QStringLiteral("path"), edge.fromPath},
            {QStringLiteral("sourcePath"), edge.fromPath}, {QStringLiteral("language"), language},
            {QStringLiteral("sourceLanguage"), language},
            {QStringLiteral("detail"), QStringLiteral("called from %1 (%2)").arg(QFileInfo(edge.fromPath).fileName(), edge.via)},
            {QStringLiteral("sourceMode"), QStringLiteral("ast")}, {QStringLiteral("confidence"), edge.confidence},
            {QStringLiteral("calls"), QVariantList{}}, {QStringLiteral("calledBy"), QVariantList{}},
        };
    };

    std::function<QVariantList(QVariantList)> apply = [&](QVariantList symbols) {
        for (int index = 0; index < symbols.size(); ++index) {
            QVariantMap symbol = symbols.at(index).toMap();
            symbol.insert(QStringLiteral("members"), apply(symbol.value(QStringLiteral("members")).toList()));
            const QString key = symbolKey(symbol);
            const QString kind = symbol.value(QStringLiteral("kind")).toString();
            const QString name = symbol.value(QStringLiteral("name")).toString();

            QVariantList calls = symbol.value(QStringLiteral("calls")).toList();
            QSet<QString> existingCalls;
            for (const QVariant &entry : std::as_const(calls)) {
                const QVariantMap relation = entry.toMap();
                existingCalls.insert(relation.value(QStringLiteral("name")).toString() + QLatin1Char('|')
                                     + relation.value(QStringLiteral("path")).toString());
            }
            for (const Edge &edge : outgoing.value(key)) {
                const QString id = edge.to.name + QLatin1Char('|') + edge.to.path;
                if (existingCalls.contains(id)) {
                    continue;
                }
                existingCalls.insert(id);
                calls.append(relationTo(edge.to, QStringLiteral("calls into %1 (%2)").arg(QFileInfo(edge.to.path).fileName(), edge.via),
                                        edge.confidence));
            }
            for (const Edge &edge : httpOutgoing.value(key)) {
                const QString id = edge.to.name + QLatin1Char('|') + edge.to.path;
                if (!existingCalls.contains(id)) {
                    existingCalls.insert(id);
                    calls.append(relationTo(edge.to, QStringLiteral("%1 -> route in %2").arg(edge.via, QFileInfo(edge.to.path).fileName()),
                                            edge.confidence));
                }
                // The view function declared with the route (decorator / attribute).
                const auto fileIt = snapshot->files.constFind(edge.to.path);
                if (fileIt == snapshot->files.constEnd()) {
                    continue;
                }
                for (const Definition &handler : fileIt->definitions) {
                    if (isCallableKind(handler.kind) && handler.line >= edge.to.line && handler.line <= edge.to.line + 4) {
                        const QString handlerId = handler.name + QLatin1Char('|') + handler.path;
                        if (!existingCalls.contains(handlerId)) {
                            existingCalls.insert(handlerId);
                            calls.append(relationTo(handler, QStringLiteral("handles %1").arg(edge.to.name), edge.confidence));
                        }
                        break;
                    }
                }
            }
            symbol.insert(QStringLiteral("calls"), calls);

            QVariantList calledBy = symbol.value(QStringLiteral("calledBy")).toList();
            QSet<QString> existingCallers;
            auto callerId = [](const QVariantMap &relation) {
                return relation.value(QStringLiteral("name")).toString() + QLatin1Char('|')
                    + QFileInfo(relation.value(QStringLiteral("path")).toString()).absoluteFilePath() + QLatin1Char('|')
                    + relation.value(QStringLiteral("line")).toString();
            };
            for (const QVariant &entry : std::as_const(calledBy)) {
                existingCallers.insert(callerId(entry.toMap()));
            }
            // C/C++: a prototype shows its body's callers and where the body is;
            // a body lists the prototypes that declare it.
            const QString ownKey = path + QLatin1Char('|') + kind + QLatin1Char('|') + name;
            QVector<int> incoming = snapshot->incomingByToKey.value(ownKey);
            const auto bodyIt = snapshot->definitionForDeclaration.constFind(ownKey);
            if (bodyIt != snapshot->definitionForDeclaration.constEnd()) {
                const Definition &body = snapshot->allDefinitions.at(bodyIt.value());
                incoming += snapshot->incomingByToKey.value(definitionKey(body));
                symbol.insert(QStringLiteral("definition"), relationTo(body, QStringLiteral("defined in %1").arg(QFileInfo(body.path).fileName()),
                                                                      QStringLiteral("medium")));
            }
            QVariantList declaredIn;
            for (int declarationIndex : snapshot->declarationsForDefinition.value(ownKey)) {
                const Definition &declaration = snapshot->allDefinitions.at(declarationIndex);
                declaredIn.append(relationTo(declaration, QStringLiteral("declared in %1").arg(QFileInfo(declaration.path).fileName()),
                                             QStringLiteral("medium")));
            }
            if (!declaredIn.isEmpty()) {
                symbol.insert(QStringLiteral("declaredIn"), declaredIn);
            }
            // A route handler declared just below its route (Flask/FastAPI
            // decorators, Spring/ASP.NET attributes) is called by the route's clients.
            const int symbolLine = symbol.value(QStringLiteral("line")).toInt();
            if (isCallableKind(kind)) {
                for (auto routeIt = incomingByRouteLine.cbegin(); routeIt != incomingByRouteLine.cend(); ++routeIt) {
                    if (symbolLine < routeIt.key() || symbolLine > routeIt.key() + 4) {
                        continue;
                    }
                    for (int edgeIndex : routeIt.value()) {
                        const QVariantMap relation = relationFrom(snapshot->httpEdges.at(edgeIndex));
                        const QString id = callerId(relation);
                        if (!existingCallers.contains(id)) {
                            existingCallers.insert(id);
                            calledBy.append(relation);
                        }
                    }
                }
            }
            int crossFileCallers = 0;
            for (int edgeIndex : std::as_const(incoming)) {
                const Edge &edge = snapshot->edges.at(edgeIndex);
                if (edge.fromPath == path) {
                    continue;
                }
                const QVariantMap relation = relationFrom(edge);
                const QString id = callerId(relation);
                if (existingCallers.contains(id)) {
                    continue;
                }
                existingCallers.insert(id);
                ++crossFileCallers;
                if (crossFileCallers <= kMaxListedCallers) {
                    calledBy.append(relation);
                }
            }
            if (crossFileCallers > kMaxListedCallers) {
                // Widely used symbols: list the first callers, report the total.
                symbol.insert(QStringLiteral("calledByTotal"), calledBy.size() + crossFileCallers - kMaxListedCallers);
            }
            symbol.insert(QStringLiteral("calledBy"), calledBy);
            symbols[index] = symbol;
        }
        return symbols;
    };

    QVariantMap augmented = analysis;
    augmented.insert(QStringLiteral("symbols"), apply(analysis.value(QStringLiteral("symbols")).toList()));
    if (!httpCalls.isEmpty()) {
        augmented.insert(QStringLiteral("httpCalls"), httpCalls);
    }
    if (!incomingByRouteLine.isEmpty()) {
        augmented.insert(QStringLiteral("routes"), routes);
    }
    augmented.insert(QStringLiteral("projectIndex"), QVariantMap{{QStringLiteral("files"), snapshot->files.size()},
                                                                 {QStringLiteral("crossFileEdges"), snapshot->edges.size()}});
    return augmented;
}

} // namespace ProjectIndex
