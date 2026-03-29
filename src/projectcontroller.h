#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

class FileSystemModel;
class SymbolParser;

class ProjectController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *fileSystemModel READ fileSystemModel CONSTANT)
    Q_PROPERTY(QString rootPath READ rootPath WRITE setRootPath NOTIFY rootPathChanged)
    Q_PROPERTY(QString selectedPath READ selectedPath NOTIFY selectedPathChanged)
    Q_PROPERTY(QString selectedRelativePath READ selectedRelativePath NOTIFY selectedPathChanged)
    Q_PROPERTY(QVariantMap selectedFileData READ selectedFileData NOTIFY selectedFileDataChanged)
    Q_PROPERTY(QVariantMap selectedSymbol READ selectedSymbol NOTIFY selectedSymbolChanged)
    Q_PROPERTY(QVariantList selectedSymbolMembers READ selectedSymbolMembers NOTIFY selectedSymbolChanged)

public:
    explicit ProjectController(QObject *parent = nullptr);
    ~ProjectController() override;

    QObject *fileSystemModel() const;
    QString rootPath() const;
    QString selectedPath() const;
    QString selectedRelativePath() const;
    QVariantMap selectedFileData() const;
    QVariantMap selectedSymbol() const;
    QVariantList selectedSymbolMembers() const;
    Q_INVOKABLE QString lastOpenedPath() const;

    Q_INVOKABLE void setRootPath(const QString &path);
    Q_INVOKABLE void selectPath(const QString &path);
    Q_INVOKABLE void selectSymbol(int index);
    Q_INVOKABLE void selectSymbolByData(const QVariantMap &symbol);
    Q_INVOKABLE QString pickFolder() const;
    Q_INVOKABLE bool restoreLastOpenedPath();

signals:
    void rootPathChanged();
    void selectedPathChanged();
    void selectedFileDataChanged();
    void selectedSymbolChanged();

private:
    void saveLastOpenedPath(const QString &path) const;

    FileSystemModel *m_fileSystemModel = nullptr;
    SymbolParser *m_symbolParser = nullptr;
    QString m_rootPath;
    QString m_selectedPath;
    QVariantMap m_selectedFileData;
    QVariantMap m_selectedSymbol;
};
