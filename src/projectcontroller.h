#pragma once

#include <QObject>
#include <QFutureWatcher>
#include <QVariantList>
#include <QVariantMap>

class FileSystemModel;
class SymbolParser;

class ProjectController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *fileSystemModel READ fileSystemModel CONSTANT)
    Q_PROPERTY(QString rootPath READ rootPath WRITE setRootPath NOTIFY rootPathChanged)
    Q_PROPERTY(QVariantMap projectSummary READ projectSummary NOTIFY rootPathChanged)
    Q_PROPERTY(QString selectedPath READ selectedPath NOTIFY selectedPathChanged)
    Q_PROPERTY(QString selectedRelativePath READ selectedRelativePath NOTIFY selectedPathChanged)
    Q_PROPERTY(QVariantMap selectedFileData READ selectedFileData NOTIFY selectedFileDataChanged)
    Q_PROPERTY(QVariantMap selectedSymbol READ selectedSymbol NOTIFY selectedSymbolChanged)
    Q_PROPERTY(QVariantList selectedSymbolMembers READ selectedSymbolMembers NOTIFY selectedSymbolChanged)
    Q_PROPERTY(QVariantMap selectedSnippet READ selectedSnippet NOTIFY selectedSnippetChanged)
    Q_PROPERTY(bool analysisInProgress READ analysisInProgress NOTIFY analysisInProgressChanged)
    Q_PROPERTY(QString preferredEditor READ preferredEditor WRITE setPreferredEditor NOTIFY preferredEditorChanged)

public:
    explicit ProjectController(QObject *parent = nullptr);
    ~ProjectController() override;

    QObject *fileSystemModel() const;
    QString rootPath() const;
    QVariantMap projectSummary() const;
    QString selectedPath() const;
    QString selectedRelativePath() const;
    QVariantMap selectedFileData() const;
    QVariantMap selectedSymbol() const;
    QVariantList selectedSymbolMembers() const;
    QVariantMap selectedSnippet() const;
    bool analysisInProgress() const;
    QString preferredEditor() const;
    Q_INVOKABLE QString lastOpenedPath() const;

    Q_INVOKABLE void setRootPath(const QString &path);
    Q_INVOKABLE void selectPath(const QString &path);
    Q_INVOKABLE void selectSymbol(int index);
    Q_INVOKABLE void selectSymbolByData(const QVariantMap &symbol);
    Q_INVOKABLE void setPreferredEditor(const QString &command);
    Q_INVOKABLE bool openCurrentInFolder() const;
    Q_INVOKABLE bool openCurrentInEditor() const;
    Q_INVOKABLE QString pickFolder() const;
    Q_INVOKABLE bool restoreLastOpenedPath();

signals:
    void rootPathChanged();
    void selectedPathChanged();
    void selectedFileDataChanged();
    void selectedSymbolChanged();
    void selectedSnippetChanged();
    void analysisInProgressChanged();
    void preferredEditorChanged();

private:
    void beginAsyncAnalysis(const QString &path, const QVariantMap &pendingSymbol = QVariantMap{});
    void applyResolvedSelection(const QVariantMap &symbol);
    QVariantMap makeFileSnippet() const;
    QVariantMap parseFileSafely(const QString &path) const;
    static QVariantMap makeSymbolSnippet(const QVariantMap &symbol, const QVariantMap &fileData);
    static QVariantMap enrichSnippetPayload(const QVariantMap &snippet);
    void saveLastOpenedPath(const QString &path) const;
    QString currentOpenableFilePath() const;

    FileSystemModel *m_fileSystemModel = nullptr;
    SymbolParser *m_symbolParser = nullptr;
    QString m_rootPath;
    QString m_selectedPath;
    QString m_preferredEditor;
    QVariantMap m_selectedFileData;
    QVariantMap m_selectedSymbol;
    QVariantMap m_selectedSnippet;
    QVariantMap m_pendingSelectedSymbol;
    QFutureWatcher<QVariantMap> *m_analysisWatcher = nullptr;
    int m_analysisRequestId = 0;
    bool m_analysisInProgress = false;
};
