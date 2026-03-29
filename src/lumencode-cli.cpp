#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QTextStream>
#include <iostream>

#include "projectcontroller.h"
#include "filesystemmodel.h"

static QString resolveCliPath(const QString &rawPath, const QString &basePath)
{
    if (rawPath.isEmpty()) {
        return rawPath;
    }

    const QFileInfo info(rawPath);
    if (info.isAbsolute()) {
        return info.absoluteFilePath();
    }

    const QString effectiveBase = basePath.isEmpty() ? QDir::currentPath() : basePath;
    return QFileInfo(QDir(effectiveBase).filePath(rawPath)).absoluteFilePath();
}

void printState(ProjectController &controller) {
    QJsonObject output;
    output["rootPath"] = controller.rootPath();
    output["projectSummary"] = QJsonObject::fromVariantMap(controller.projectSummary());
    output["selectedPath"] = controller.selectedPath();
    output["selectedRelativePath"] = controller.selectedRelativePath();
    output["selectedFileData"] = QJsonObject::fromVariantMap(controller.selectedFileData());
    output["selectedSymbol"] = QJsonObject::fromVariantMap(controller.selectedSymbol());
    output["selectedSymbolMembers"] = QJsonArray::fromVariantList(controller.selectedSymbolMembers());

    FileSystemModel *fsModel = qobject_cast<FileSystemModel*>(controller.fileSystemModel());
    if (fsModel) {
        QJsonArray entries;
        const QVariantList visible = fsModel->visibleEntries();
        for (const QVariant &v : visible) {
            entries.append(QJsonObject::fromVariantMap(v.toMap()));
        }
        output["fileSystem"] = entries;
    }

    QJsonDocument doc(output);
    std::cout << doc.toJson(QJsonDocument::Compact).toStdString() << std::endl;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("lumencode-cli"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("CLI interface for LumenCode project analysis"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("rootPath"), QStringLiteral("The root path of the project to analyze (optional in interactive mode)"));
    
    QCommandLineOption selectPathOption(QStringList() << "p" << "path",
                                        QStringLiteral("Select a specific path for analysis."),
                                        QStringLiteral("path"));
    parser.addOption(selectPathOption);

    QCommandLineOption selectSymbolOption(QStringList() << "s" << "symbol",
                                          QStringLiteral("Select a symbol by index from the selected path."),
                                          QStringLiteral("index"));
    parser.addOption(selectSymbolOption);

    QCommandLineOption interactiveOption(QStringList() << "i" << "interactive",
                                         QStringLiteral("Start in interactive mode reading JSON commands from stdin."));
    parser.addOption(interactiveOption);

    parser.process(app);

    ProjectController controller;

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) {
        const QString rootPath = QFileInfo(args.at(0)).absoluteFilePath();
        if (QFileInfo::exists(rootPath) && QFileInfo(rootPath).isDir()) {
            controller.setRootPath(rootPath);
        } else if (!parser.isSet(interactiveOption)) {
            std::cerr << "Error: Project root path does not exist or is not a directory: " << rootPath.toStdString() << std::endl;
            return 1;
        }
    } else if (!parser.isSet(interactiveOption)) {
        std::cerr << "Error: No project root path specified" << std::endl;
        parser.showHelp(1);
    }

    if (parser.isSet(interactiveOption)) {
        QTextStream qin(stdin);
        while (true) {
            QString line = qin.readLine();
            if (line.isNull()) break;
            
            QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
            if (doc.isNull() || !doc.isObject()) {
                std::cerr << "Invalid JSON command" << std::endl;
                continue;
            }
            
            QJsonObject cmdObj = doc.object();
            QString command = cmdObj["command"].toString();
            QJsonObject params = cmdObj["params"].toObject();
            
            if (command == "setRootPath") {
                controller.setRootPath(resolveCliPath(params["path"].toString(), QDir::currentPath()));
            } else if (command == "selectPath") {
                controller.selectPath(resolveCliPath(params["path"].toString(), controller.rootPath()));
            } else if (command == "selectSymbol") {
                controller.selectSymbol(params["index"].toInt());
            } else if (command == "toggleExpanded") {
                 FileSystemModel *fsModel = qobject_cast<FileSystemModel*>(controller.fileSystemModel());
                 if (fsModel) {
                     fsModel->toggleExpanded(resolveCliPath(params["path"].toString(), controller.rootPath()));
                 }
            } else if (command == "exit") {
                break;
            } else if (command == "getState" || command == "getProjectSummary") {
                // Just print state
            } else {
                std::cerr << "Unknown command: " << command.toStdString() << std::endl;
            }
            
            printState(controller);
        }
        return 0;
    }

    // One-shot mode logic
    if (parser.isSet(selectPathOption)) {
        controller.selectPath(resolveCliPath(parser.value(selectPathOption), controller.rootPath()));
        if (parser.isSet(selectSymbolOption)) {
            bool ok;
            int symbolIndex = parser.value(selectSymbolOption).toInt(&ok);
            if (ok) {
                controller.selectSymbol(symbolIndex);
            }
        }
    }

    printState(controller);

    return 0;
}
