#include "filesystemmodel.h"

#include <QDir>
#include <QFileInfo>

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
    node->name = parent ? info.fileName() : QStringLiteral(".");
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
        || lower == QStringLiteral("dist")
        || lower == QStringLiteral("cpp")
        || lower == QStringLiteral("c++")
        || lower == QStringLiteral("python");
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
    };
    return allowed.contains(suffix);
}

FileSystemModel::Node *FileSystemModel::nodeForIndex(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return nullptr;
    }
    return static_cast<Node *>(index.internalPointer());
}
