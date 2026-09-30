#pragma once

#include <QObject>
#include <QFutureWatcher>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <memory>

#include "projectindex.h"

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
    Q_PROPERTY(QVariantMap indexStatus READ indexStatus NOTIFY indexStatusChanged)

public:
    enum class IndexMode { Background, Synchronous, Off };

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
    QVariantMap indexStatus() const;
    void setIndexMode(IndexMode mode);

    Q_INVOKABLE void setRootPath(const QString &path);
    Q_INVOKABLE void selectPath(const QString &path);
    Q_INVOKABLE void selectSymbol(int index);
    Q_INVOKABLE void selectSymbolByData(const QVariantMap &symbol);
    // Ranked symbol / file search over the project index (empty until the index is ready).
    // `kinds` is a comma-separated filter such as "function,class" or "file".
    Q_INVOKABLE QVariantList search(const QString &query, const QString &kinds = QString(), int limit = 60) const;
    // Show a search result: reveal it in the tree and select the file, or symbol within it.
    Q_INVOKABLE void openSearchResult(const QVariantMap &result);
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
    void indexStatusChanged();

private:
    void beginAsyncAnalysis(const QString &path, const QVariantMap &pendingSymbol = QVariantMap{});
    void applyResolvedSelection(const QVariantMap &symbol);
    void startIndexBuild();
    void cancelIndexBuild();
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

    IndexMode m_indexMode = IndexMode::Background;
    ProjectIndex::SnapshotPtr m_indexSnapshot;
    QFutureWatcher<ProjectIndex::SnapshotPtr> *m_indexWatcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_indexCancel;
    QVariantMap m_indexStatus;
    int m_indexRequestId = 0;
};
