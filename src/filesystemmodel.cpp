#include "filesystemmodel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <limits>

namespace {

QString normalizeProjectEntry(const QString &rootPath, const QString &value)
{
    if (value.isEmpty()) {
        return {};
    }

    QString cleaned = value.trimmed();
    if (cleaned.isEmpty()) {
        return {};
    }

    if (cleaned.startsWith(QStringLiteral("./"))) {
        cleaned.remove(0, 2);
    }

    const QFileInfo info(QDir(rootPath).filePath(cleaned));
    return info.absoluteFilePath();
}

int entryScore(const QString &rootPath, const QString &path, const QString &name, bool isDir)
{
    if (isDir) {
        return std::numeric_limits<int>::min();
    }

    const QString relativePath = QDir(rootPath).relativeFilePath(path);
    const QString lowerRelative = relativePath.toLower();
    const QString lowerName = name.toLower();

    if (lowerRelative.startsWith(QStringLiteral("third_party/"))
        || lowerRelative.startsWith(QStringLiteral("vendor/"))
        || lowerRelative.startsWith(QStringLiteral("node_modules/"))
        || lowerRelative.startsWith(QStringLiteral("dist/"))
        || lowerRelative.startsWith(QStringLiteral("build/"))
        || lowerRelative.startsWith(QStringLiteral("coverage/"))
        || lowerRelative.startsWith(QStringLiteral("tests/"))
        || lowerRelative.startsWith(QStringLiteral("test/"))
        || lowerRelative.startsWith(QStringLiteral("docs/"))) {
        return std::numeric_limits<int>::min() / 2;
    }

    int score = 0;

    const QStringList preferredRoots = {
        QStringLiteral("src/main.cpp"),
        QStringLiteral("src/main.cc"),
        QStringLiteral("src/main.cxx"),
        QStringLiteral("src/main.c"),
        QStringLiteral("src/main.py"),
        QStringLiteral("src/main.java"),
        QStringLiteral("src/main.rs"),
        QStringLiteral("src/main.m"),
        QStringLiteral("src/main.mm"),
        QStringLiteral("src/program.cs"),
        QStringLiteral("src/main.tsx"),
        QStringLiteral("src/main.ts"),
        QStringLiteral("src/main.jsx"),
        QStringLiteral("src/main.js"),
        QStringLiteral("src/index.tsx"),
        QStringLiteral("src/index.ts"),
        QStringLiteral("src/index.jsx"),
        QStringLiteral("src/index.js"),
        QStringLiteral("main.ts"),
        QStringLiteral("main.js"),
        QStringLiteral("main.cpp"),
        QStringLiteral("main.cc"),
        QStringLiteral("main.cxx"),
        QStringLiteral("main.c"),
        QStringLiteral("main.py"),
        QStringLiteral("main.java"),
        QStringLiteral("main.rs"),
        QStringLiteral("main.m"),
        QStringLiteral("main.mm"),
        QStringLiteral("program.cs"),
        QStringLiteral("app.ts"),
        QStringLiteral("app.js"),
        QStringLiteral("app.py"),
        QStringLiteral("index.ts"),
        QStringLiteral("index.js")
    };

    const int preferredIndex = preferredRoots.indexOf(lowerRelative);
    if (preferredIndex >= 0) {
        score += 200 - preferredIndex;
    }

    if (lowerRelative.startsWith(QStringLiteral("src/"))) {
        score += 80;
    }
    if (!lowerRelative.contains(QLatin1Char('/'))) {
        score += 30;
    }

    const int slashCount = lowerRelative.count(QLatin1Char('/'));
    score -= slashCount * 8;

    if (lowerName == QStringLiteral("main.ts") || lowerName == QStringLiteral("main.js")
        || lowerName == QStringLiteral("main.tsx") || lowerName == QStringLiteral("main.jsx")) {
        score += 120;
    } else if (lowerName == QStringLiteral("main.cpp") || lowerName == QStringLiteral("main.cc")
               || lowerName == QStringLiteral("main.cxx") || lowerName == QStringLiteral("main.c")) {
        score += 120;
    } else if (lowerName == QStringLiteral("program.cs")) {
        score += 115;
    } else if (lowerName == QStringLiteral("main.rs")) {
        score += 112;
    } else if (lowerName == QStringLiteral("main.m") || lowerName == QStringLiteral("main.mm")) {
        score += 110;
    } else if (lowerName == QStringLiteral("main.py") || lowerName == QStringLiteral("app.py")) {
        score += 105;
    } else if (lowerName == QStringLiteral("main.java")) {
        score += 100;
    } else if (lowerName == QStringLiteral("app.ts") || lowerName == QStringLiteral("app.js")) {
        score += 90;
    } else if (lowerName == QStringLiteral("index.ts") || lowerName == QStringLiteral("index.js")
               || lowerName == QStringLiteral("index.tsx") || lowerName == QStringLiteral("index.jsx")) {
        score += 40;
    }

    return score;
}

QString detectPackageEntry(const QString &rootPath)
{
    QFile packageFile(QDir(rootPath).filePath(QStringLiteral("package.json")));
    if (!packageFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    const QJsonDocument doc = QJsonDocument::fromJson(packageFile.readAll());
    if (!doc.isObject()) {
        return {};
    }

    const QJsonObject root = doc.object();
    const QStringList directFields = {
        QStringLiteral("browser"),
        QStringLiteral("module"),
        QStringLiteral("main")
    };

    for (const QString &field : directFields) {
        const QString value = root.value(field).toString();
        const QString candidate = normalizeProjectEntry(rootPath, value);
        if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
            return candidate;
        }
    }

    const QJsonObject scripts = root.value(QStringLiteral("scripts")).toObject();
    if (scripts.contains(QStringLiteral("dev")) || scripts.contains(QStringLiteral("build"))) {
        const QStringList frontendCandidates = {
            QStringLiteral("src/main.tsx"),
            QStringLiteral("src/main.ts"),
            QStringLiteral("src/main.jsx"),
            QStringLiteral("src/main.js"),
            QStringLiteral("src/index.tsx"),
            QStringLiteral("src/index.ts"),
            QStringLiteral("src/index.jsx"),
            QStringLiteral("src/index.js"),
            QStringLiteral("index.html")
        };

        for (const QString &entry : frontendCandidates) {
            const QString candidate = normalizeProjectEntry(rootPath, entry);
            if (QFileInfo::exists(candidate)) {
                return candidate;
            }
        }
    }

    return {};
}

QString detectConventionalEntry(const QString &rootPath)
{
    const QStringList preferredEntries = {
        QStringLiteral("src/main.cpp"),
        QStringLiteral("src/main.cc"),
        QStringLiteral("src/main.cxx"),
        QStringLiteral("src/main.c"),
        QStringLiteral("src/main.py"),
        QStringLiteral("src/main.java"),
        QStringLiteral("src/main.rs"),
        QStringLiteral("src/main.m"),
        QStringLiteral("src/main.mm"),
        QStringLiteral("src/Program.cs"),
        QStringLiteral("src/program.cs"),
        QStringLiteral("src/main.tsx"),
        QStringLiteral("src/main.ts"),
        QStringLiteral("src/main.jsx"),
        QStringLiteral("src/main.js"),
        QStringLiteral("src/index.tsx"),
        QStringLiteral("src/index.ts"),
        QStringLiteral("src/index.jsx"),
        QStringLiteral("src/index.js"),
        QStringLiteral("main.cpp"),
        QStringLiteral("main.cc"),
        QStringLiteral("main.cxx"),
        QStringLiteral("main.c"),
        QStringLiteral("main.py"),
        QStringLiteral("main.java"),
        QStringLiteral("main.rs"),
        QStringLiteral("main.m"),
        QStringLiteral("main.mm"),
        QStringLiteral("Program.cs"),
        QStringLiteral("program.cs"),
        QStringLiteral("app.py"),
        QStringLiteral("app.js"),
        QStringLiteral("app.ts"),
        QStringLiteral("index.js"),
        QStringLiteral("index.ts"),
        QStringLiteral("index.html"),
    };

    for (const QString &entry : preferredEntries) {
        const QString candidate = normalizeProjectEntry(rootPath, entry);
        if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
            return candidate;
        }
    }

    return {};
}

}

FileSystemModel::FileSystemModel(QObject *parent)
    : QAbstractItemModel(parent)
{
}

FileSystemModel::~FileSystemModel()
{
    deleteNode(m_root);
}

QModelIndex FileSystemModel::index(int row, int column, const QModelIndex &parentIndex) const
{
    if (column != 0 || row < 0) {
        return {};
    }

    Node *parentNode = nodeForIndex(parentIndex);
    if (!parentNode) {
        parentNode = m_root;
    }
    if (!parentNode || row >= parentNode->children.size()) {
        return {};
    }

    return createIndex(row, column, parentNode->children.at(row));
}

QModelIndex FileSystemModel::parent(const QModelIndex &childIndex) const
{
    if (!childIndex.isValid()) {
        return {};
    }

    auto *childNode = static_cast<Node *>(childIndex.internalPointer());
    if (!childNode || !childNode->parent || childNode->parent == m_root) {
        return {};
    }

    Node *parentNode = childNode->parent;
    Node *grandParent = parentNode->parent;
    const int row = grandParent ? grandParent->children.indexOf(parentNode) : 0;
    return createIndex(row, 0, parentNode);
}

int FileSystemModel::rowCount(const QModelIndex &parentIndex) const
{
    Node *parentNode = nodeForIndex(parentIndex);
    if (!parentNode) {
        parentNode = m_root;
    }
    return parentNode ? parentNode->children.size() : 0;
}

int FileSystemModel::columnCount(const QModelIndex &) const
{
    return 1;
}

QVariant FileSystemModel::data(const QModelIndex &index, int role) const
{
    auto *node = nodeForIndex(index);
    if (!node) {
        return {};
    }

    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return node->name;
    case PathRole:
        return node->path;
    case IsDirRole:
        return node->isDir;
    case FileTypeRole:
        return node->fileType;
    default:
        return {};
    }
}

QHash<int, QByteArray> FileSystemModel::roleNames() const
{
    return {
        {NameRole, "name"},
        {PathRole, "path"},
        {IsDirRole, "isDir"},
        {DepthRole, "depth"},
        {ExpandedRole, "expanded"},
        {HasChildrenRole, "hasChildren"},
        {FileTypeRole, "fileType"},
    };
}

QString FileSystemModel::rootPath() const
{
    return m_rootPath;
}

QVariantList FileSystemModel::visibleEntries() const
{
    return m_visibleEntries;
}

void FileSystemModel::setRootPath(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        return;
    }

    beginResetModel();
    m_nodesByPath.clear();
    m_expandedPaths.clear();
    Node *newRoot = scanDirectory(info.absoluteFilePath(), nullptr);
    resetTree(newRoot);
    m_rootPath = info.absoluteFilePath();
    m_expandedPaths.insert(m_rootPath);
    endResetModel();

    refreshVisibleEntries();
    emit rootPathChanged();
}

void FileSystemModel::toggleExpanded(const QString &path)
{
    if (!m_nodesByPath.contains(path)) {
        return;
    }

    if (m_expandedPaths.contains(path)) {
        m_expandedPaths.remove(path);
    } else {
        m_expandedPaths.insert(path);
    }

    refreshVisibleEntries();
}

bool FileSystemModel::isExpanded(const QString &path) const
{
    return m_expandedPaths.contains(path);
}

void FileSystemModel::resetTree(Node *newRoot)
{
    deleteNode(m_root);
    m_root = newRoot;
}

void FileSystemModel::deleteNode(Node *node)
{
    if (!node) {
        return;
    }
    for (Node *child : std::as_const(node->children)) {
        deleteNode(child);
    }
    delete node;
}

FileSystemModel::Node *FileSystemModel::scanDirectory(const QString &path, Node *parent)
{
    QFileInfo info(path);
    auto *node = new Node;
    node->name = parent ? info.fileName() : QStringLiteral("/");
    node->path = info.absoluteFilePath();
    node->isDir = true;
    node->parent = parent;
    node->fileType = QStringLiteral("folder");
    m_nodesByPath.insert(node->path, node);

    QDir dir(node->path);
    const QFileInfoList entries = dir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllDirs | QDir::Files,
                                                    QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &entry : entries) {
        if (entry.isDir()) {
            if (shouldIgnoreDirectory(entry.fileName())) {
                continue;
            }
            node->children.append(scanDirectory(entry.absoluteFilePath(), node));
            continue;
        }

        const QString suffix = entry.suffix().toLower();
        if (!shouldIncludeFile(suffix)) {
            continue;
        }

        auto *child = new Node;
        child->name = entry.fileName();
        child->path = entry.absoluteFilePath();
        child->isDir = false;
        child->parent = node;
        child->fileType = detectFileType(child->path, false);
        m_nodesByPath.insert(child->path, child);
        node->children.append(child);
    }

    return node;
}

void FileSystemModel::refreshVisibleEntries()
{
    m_visibleEntries = {};
    if (m_root) {
        appendVisible(m_root, 0);
    }
    emit visibleEntriesChanged();
}

void FileSystemModel::appendVisible(Node *node, int depth)
{
    QVariantMap item;
    item.insert(QStringLiteral("name"), node->name);
    item.insert(QStringLiteral("path"), node->path);
    item.insert(QStringLiteral("isDir"), node->isDir);
    item.insert(QStringLiteral("depth"), depth);
    item.insert(QStringLiteral("expanded"), m_expandedPaths.contains(node->path));
    item.insert(QStringLiteral("hasChildren"), !node->children.isEmpty());
    item.insert(QStringLiteral("fileType"), node->fileType);
    m_visibleEntries.append(item);

    if (!node->isDir || !m_expandedPaths.contains(node->path)) {
        return;
    }

    for (Node *child : node->children) {
        appendVisible(child, depth + 1);
    }
}

QString FileSystemModel::detectFileType(const QString &path, bool isDir)
{
    if (isDir) {
        return QStringLiteral("folder");
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
    if (suffix == QStringLiteral("tsx") || suffix == QStringLiteral("jsx")) {
        return QStringLiteral("react");
    }
    if (suffix == QStringLiteral("ts") || suffix == QStringLiteral("js")) {
        return QStringLiteral("script");
    }
    if (suffix == QStringLiteral("py")) {
        return QStringLiteral("python");
    }
    if (suffix == QStringLiteral("cpp") || suffix == QStringLiteral("cc")
        || suffix == QStringLiteral("cxx") || suffix == QStringLiteral("c")
        || suffix == QStringLiteral("h") || suffix == QStringLiteral("hpp")
        || suffix == QStringLiteral("hh") || suffix == QStringLiteral("hxx")) {
        return QStringLiteral("cpp");
    }
    if (suffix == QStringLiteral("java")) {
        return QStringLiteral("java");
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
    if (QFileInfo(path).fileName() == QStringLiteral("package.json")) {
        return QStringLiteral("package");
    }
    if (suffix == QStringLiteral("json")) {
        return QStringLiteral("json");
    }
    return QStringLiteral("file");
}

bool FileSystemModel::shouldIgnoreDirectory(const QString &name)
{
    const QString lower = name.toLower();
    return lower == QStringLiteral(".git")
        || lower == QStringLiteral("__pycache__")
        || lower == QStringLiteral("venv")
        || lower == QStringLiteral(".venv")
        || lower == QStringLiteral("node_modules")
        || lower == QStringLiteral("build")
        || lower == QStringLiteral("dist");
}

bool FileSystemModel::shouldIncludeFile(const QString &suffix)
{
    static const QSet<QString> allowed = {
        QStringLiteral("js"),
        QStringLiteral("jsx"),
        QStringLiteral("json"),
        QStringLiteral("ts"),
        QStringLiteral("tsx"),
        QStringLiteral("php"),
        QStringLiteral("html"),
        QStringLiteral("css"),
        QStringLiteral("py"),
        QStringLiteral("c"),
        QStringLiteral("cc"),
        QStringLiteral("cpp"),
        QStringLiteral("cxx"),
        QStringLiteral("h"),
        QStringLiteral("hh"),
        QStringLiteral("hpp"),
        QStringLiteral("hxx"),
        QStringLiteral("java"),
        QStringLiteral("cs"),
        QStringLiteral("rs"),
        QStringLiteral("m"),
        QStringLiteral("mm"),
    };
    return allowed.contains(suffix);
}

QVariantMap FileSystemModel::collectStats() const
{
    QVariantMap stats;
    QMap<QString, int> typeCounts;
    QString mainEntry = detectPackageEntry(m_rootPath);
    if (mainEntry.isEmpty()) {
        mainEntry = detectConventionalEntry(m_rootPath);
    }
    int totalFiles = 0;
    int bestEntryScore = std::numeric_limits<int>::min();

    for (Node *node : std::as_const(m_nodesByPath)) {
        if (!node->isDir) {
            totalFiles++;
            typeCounts[node->fileType]++;

            if (mainEntry.isEmpty()) {
                const int score = entryScore(m_rootPath, node->path, node->name, node->isDir);
                if (score > bestEntryScore) {
                    bestEntryScore = score;
                    mainEntry = node->path;
                }
            }
        }
    }

    QVariantMap typeCountsMap;
    for (auto it = typeCounts.begin(); it != typeCounts.end(); ++it) {
        typeCountsMap.insert(it.key(), it.value());
    }

    stats.insert(QStringLiteral("totalFiles"), totalFiles);
    stats.insert(QStringLiteral("fileTypes"), typeCountsMap);
    stats.insert(QStringLiteral("mainEntry"), mainEntry);
    stats.insert(QStringLiteral("rootPath"), m_rootPath);

    return stats;
}

FileSystemModel::Node *FileSystemModel::nodeForIndex(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return nullptr;
    }
    return static_cast<Node *>(index.internalPointer());
}
