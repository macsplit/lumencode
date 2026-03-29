#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVariantList>

class FileSystemModel final : public QAbstractItemModel
{
    Q_OBJECT
    Q_PROPERTY(QVariantList visibleEntries READ visibleEntries NOTIFY visibleEntriesChanged)
    Q_PROPERTY(QString rootPath READ rootPath NOTIFY rootPathChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PathRole,
        IsDirRole,
        DepthRole,
        ExpandedRole,
        HasChildrenRole,
        FileTypeRole
    };
    Q_ENUM(Roles)

    explicit FileSystemModel(QObject *parent = nullptr);
    ~FileSystemModel() override;

    QModelIndex index(int row, int column, const QModelIndex &parent) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent) const override;
    int columnCount(const QModelIndex &parent) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString rootPath() const;
    QVariantList visibleEntries() const;

    Q_INVOKABLE void setRootPath(const QString &path);
    Q_INVOKABLE void toggleExpanded(const QString &path);
    Q_INVOKABLE bool isExpanded(const QString &path) const;

signals:
    void visibleEntriesChanged();
    void rootPathChanged();

private:
    struct Node {
        QString name;
        QString path;
        QString fileType;
        bool isDir = false;
        Node *parent = nullptr;
        QList<Node *> children;
    };

    Node *m_root = nullptr;
    QString m_rootPath;
    QSet<QString> m_expandedPaths;
    QVariantList m_visibleEntries;
    QHash<QString, Node *> m_nodesByPath;

    void resetTree(Node *newRoot);
    void deleteNode(Node *node);
    Node *scanDirectory(const QString &path, Node *parent);
    void refreshVisibleEntries();
    void appendVisible(Node *node, int depth);
    static QString detectFileType(const QString &path, bool isDir);
    static bool shouldIgnoreDirectory(const QString &name);
    static bool shouldIncludeFile(const QString &suffix);
    Node *nodeForIndex(const QModelIndex &index) const;
};
