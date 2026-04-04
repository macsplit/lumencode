#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QThread>

#include <iostream>

#include "filesystemmodel.h"
#include "projectcontroller.h"
#include "symbolparser.h"

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

static bool waitForAnalysis(ProjectController &controller, int timeoutMs = 20000)
{
    if (!controller.analysisInProgress()) {
        return true;
    }

    QElapsedTimer timer;
    timer.start();
    while (controller.analysisInProgress() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return !controller.analysisInProgress();
}

static void printState(ProjectController &controller)
{
    QJsonObject output;
    output["rootPath"] = controller.rootPath();
    output["projectSummary"] = QJsonObject::fromVariantMap(controller.projectSummary());
    output["selectedPath"] = controller.selectedPath();
    output["selectedRelativePath"] = controller.selectedRelativePath();
    output["selectedFileData"] = QJsonObject::fromVariantMap(controller.selectedFileData());
    output["selectedSymbol"] = QJsonObject::fromVariantMap(controller.selectedSymbol());
    output["selectedSymbolMembers"] = QJsonArray::fromVariantList(controller.selectedSymbolMembers());
    output["selectedSnippet"] = QJsonObject::fromVariantMap(controller.selectedSnippet());
    output["analysisInProgress"] = controller.analysisInProgress();

    FileSystemModel *fsModel = qobject_cast<FileSystemModel *>(controller.fileSystemModel());
    if (fsModel) {
        QJsonArray entries;
        const QVariantList visible = fsModel->visibleEntries();
        for (const QVariant &value : visible) {
            entries.append(QJsonObject::fromVariantMap(value.toMap()));
        }
        output["fileSystem"] = entries;
    }

    const QJsonDocument doc(output);
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
    parser.addPositionalArgument(QStringLiteral("rootPath"),
                                 QStringLiteral("The root path of the project to analyze (optional in interactive mode)"));

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

    QCommandLineOption dumpFileOption(QStringList() << "dump-file",
                                      QStringLiteral("Parse a single file and print only its analysis JSON."),
                                      QStringLiteral("path"));
    parser.addOption(dumpFileOption);

    parser.process(app);

    if (parser.isSet(dumpFileOption)) {
        const QString targetPath = resolveCliPath(parser.value(dumpFileOption), QDir::currentPath());
        SymbolParser symbolParser;
        const QVariantMap parsed = symbolParser.parseFile(targetPath);
        const QJsonDocument doc(QJsonObject::fromVariantMap(parsed));
        std::cout << doc.toJson(QJsonDocument::Compact).toStdString() << std::endl;
        return 0;
    }

    ProjectController controller;

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) {
        const QString rootPath = QFileInfo(args.at(0)).absoluteFilePath();
        if (QFileInfo::exists(rootPath) && QFileInfo(rootPath).isDir()) {
            controller.setRootPath(rootPath);
            waitForAnalysis(controller);
        } else if (!parser.isSet(interactiveOption)) {
            std::cerr << "Error: Project root path does not exist or is not a directory: "
                      << rootPath.toStdString() << std::endl;
            return 1;
        }
    } else if (!parser.isSet(interactiveOption)) {
        std::cerr << "Error: No project root path specified" << std::endl;
        parser.showHelp(1);
    }

    if (parser.isSet(interactiveOption)) {
        QTextStream qin(stdin);
        while (true) {
            const QString line = qin.readLine();
            if (line.isNull()) {
                break;
            }

            const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
            if (doc.isNull() || !doc.isObject()) {
                std::cerr << "Invalid JSON command" << std::endl;
                continue;
            }

            const QJsonObject cmdObj = doc.object();
            const QString command = cmdObj["command"].toString();
            const QJsonObject params = cmdObj["params"].toObject();

            if (command == QStringLiteral("setRootPath")) {
                controller.setRootPath(resolveCliPath(params["path"].toString(), QDir::currentPath()));
                waitForAnalysis(controller);
            } else if (command == QStringLiteral("selectPath")) {
                controller.selectPath(resolveCliPath(params["path"].toString(), controller.rootPath()));
                waitForAnalysis(controller);
            } else if (command == QStringLiteral("selectSymbol")) {
                controller.selectSymbol(params["index"].toInt());
            } else if (command == QStringLiteral("selectSymbolByData")) {
                controller.selectSymbolByData(params.toVariantMap());
                waitForAnalysis(controller);
            } else if (command == QStringLiteral("toggleExpanded")) {
                FileSystemModel *fsModel = qobject_cast<FileSystemModel *>(controller.fileSystemModel());
                if (fsModel) {
                    fsModel->toggleExpanded(resolveCliPath(params["path"].toString(), controller.rootPath()));
                }
            } else if (command == QStringLiteral("exit")) {
                break;
            } else if (command == QStringLiteral("getState") || command == QStringLiteral("getProjectSummary")) {
                // Print current state below.
            } else {
                std::cerr << "Unknown command: " << command.toStdString() << std::endl;
            }

            printState(controller);
        }
        return 0;
    }

    if (parser.isSet(selectPathOption)) {
        controller.selectPath(resolveCliPath(parser.value(selectPathOption), controller.rootPath()));
        waitForAnalysis(controller);
        if (parser.isSet(selectSymbolOption)) {
            bool ok = false;
            const int symbolIndex = parser.value(selectSymbolOption).toInt(&ok);
            if (ok) {
                controller.selectSymbol(symbolIndex);
            }
        }
    }

    printState(controller);
    return 0;
}
