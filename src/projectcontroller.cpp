#include "projectcontroller.h"

#include "filesystemmodel.h"
#include "symbolparser.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>

ProjectController::ProjectController(QObject *parent)
    : QObject(parent)
    , m_fileSystemModel(new FileSystemModel(this))
    , m_symbolParser(new SymbolParser(this))
{
    m_rootPath = QDir::currentPath();
}

ProjectController::~ProjectController() = default;

QObject *ProjectController::fileSystemModel() const
{
    return m_fileSystemModel;
}

QString ProjectController::rootPath() const
{
    return m_rootPath;
}

QString ProjectController::selectedPath() const
{
    return m_selectedPath;
}

QString ProjectController::selectedRelativePath() const
{
    if (m_selectedPath.isEmpty()) {
        return QString();
    }

    if (m_rootPath.isEmpty() || m_selectedPath == m_rootPath) {
        return QStringLiteral(".");
    }

    QDir rootDir(m_rootPath);
    const QString relative = rootDir.relativeFilePath(m_selectedPath);
    return relative.isEmpty() ? QStringLiteral(".") : relative;
}

QVariantMap ProjectController::selectedFileData() const
{
    return m_selectedFileData;
}

QVariantMap ProjectController::selectedSymbol() const
{
    return m_selectedSymbol;
}

QVariantList ProjectController::selectedSymbolMembers() const
{
    return m_selectedSymbol.value(QStringLiteral("members")).toList();
}

QString ProjectController::lastOpenedPath() const
{
    QSettings settings;
    return settings.value(QStringLiteral("session/lastOpenedPath")).toString();
}

void ProjectController::setRootPath(const QString &path)
{
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (!QFileInfo(absolute).isDir()) {
        return;
    }

    m_rootPath = absolute;
    m_fileSystemModel->setRootPath(absolute);
    saveLastOpenedPath(absolute);
    emit rootPathChanged();

    selectPath(absolute);
}

void ProjectController::selectPath(const QString &path)
{
    m_selectedPath = path;
    emit selectedPathChanged();

    const QFileInfo info(path);
    if (!info.exists()) {
        m_selectedFileData = {};
        m_selectedSymbol = {};
        emit selectedFileDataChanged();
        emit selectedSymbolChanged();
        return;
    }

    if (info.isDir()) {
        QVariantMap folderData;
        folderData.insert(QStringLiteral("path"), path);
        folderData.insert(QStringLiteral("fileName"), info.fileName().isEmpty() ? path : info.fileName());
        folderData.insert(QStringLiteral("language"), QStringLiteral("folder"));
        folderData.insert(QStringLiteral("symbols"), QVariantList{});
        folderData.insert(QStringLiteral("quickLinks"), QVariantList{});
        folderData.insert(QStringLiteral("dependencies"), QVariantList{});
        folderData.insert(QStringLiteral("routes"), QVariantList{});
        folderData.insert(QStringLiteral("relatedFiles"), QVariantList{});
        folderData.insert(QStringLiteral("packageSummary"), QVariantMap{});
        folderData.insert(QStringLiteral("cssSummary"), QVariantMap{});
        folderData.insert(QStringLiteral("summary"), QStringLiteral("Directory"));
        m_selectedFileData = folderData;
    } else {
        m_selectedFileData = m_symbolParser->parseFile(path);
    }

    m_selectedSymbol = {};
    emit selectedFileDataChanged();
    emit selectedSymbolChanged();
}

void ProjectController::selectSymbol(int index)
{
    const QVariantList symbols = m_selectedFileData.value(QStringLiteral("symbols")).toList();
    if (index < 0 || index >= symbols.size()) {
        m_selectedSymbol = {};
    } else {
        m_selectedSymbol = symbols.at(index).toMap();
    }
    emit selectedSymbolChanged();
}

void ProjectController::selectSymbolByData(const QVariantMap &symbol)
{
    m_selectedSymbol = symbol;
    emit selectedSymbolChanged();
}

QString ProjectController::pickFolder() const
{
    return QFileDialog::getExistingDirectory(nullptr,
                                             QStringLiteral("Select a project folder"),
                                             !lastOpenedPath().isEmpty() ? lastOpenedPath()
                                                                         : (m_rootPath.isEmpty() ? QDir::homePath() : m_rootPath),
                                             QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
}

bool ProjectController::restoreLastOpenedPath()
{
    const QString path = lastOpenedPath();
    if (path.isEmpty() || !QFileInfo(path).isDir()) {
        return false;
    }

    setRootPath(path);
    return true;
}

void ProjectController::saveLastOpenedPath(const QString &path) const
{
    QSettings settings;
    settings.setValue(QStringLiteral("session/lastOpenedPath"), path);
}
