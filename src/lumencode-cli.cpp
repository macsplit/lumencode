#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QThread>

#include <iostream>

#include "filesystemmodel.h"
#include "projectcontroller.h"
#include "projectindex.h"
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

    QCommandLineOption debugAstOption(QStringList() << "debug-ast",
                                      QStringLiteral("Print the Tree-sitter error nodes and the repair pass for a file (parser development aid)."),
                                      QStringLiteral("path"));
    parser.addOption(debugAstOption);

    QCommandLineOption indexFactsOption(QStringList() << "index-facts",
                                        QStringLiteral("Read file paths from stdin and print one line of project-index facts (JSON) per file."));
    parser.addOption(indexFactsOption);

    QCommandLineOption indexProjectOption(QStringList() << "index-project",
                                          QStringLiteral("Build (or refresh) the project index for a folder and print its statistics."),
                                          QStringLiteral("root"));
    parser.addOption(indexProjectOption);

    QCommandLineOption indexRelationsOption(QStringList() << "index-relations",
                                            QStringLiteral("With --index-project: print the cross-file Calls / Called By of one file's symbols."),
                                            QStringLiteral("path"));
    parser.addOption(indexRelationsOption);

    QCommandLineOption indexEdgesOption(QStringList() << "index-edges",
                                        QStringLiteral("With --index-project: print every cross-file call edge, one JSON object per line."));
    parser.addOption(indexEdgesOption);

    QCommandLineOption noIndexOption(QStringList() << "no-index",
                                     QStringLiteral("Do not build the project index (cross-file relations fall back to the per-file JS/TS crawl)."));
    parser.addOption(noIndexOption);

    QCommandLineOption cppPrepassOption(QStringList() << "cpp-prepass",
                                        QStringLiteral("Print a C/C++ file as the grammar sees it, after the macro pre-pass (parser development aid)."),
                                        QStringLiteral("path"));
    parser.addOption(cppPrepassOption);

    parser.process(app);

    if (parser.isSet(cppPrepassOption)) {
        QFile file(resolveCliPath(parser.value(cppPrepassOption), QDir::currentPath()));
        if (!file.open(QIODevice::ReadOnly)) {
            return 1;
        }
        std::cout << SymbolParser::cppPrepassed(file.readAll()).toStdString();
        return 0;
    }

    if (parser.isSet(indexFactsOption)) {
        QTextStream qin(stdin);
        SymbolParser symbolParser;
        while (true) {
            const QString line = qin.readLine();
            if (line.isNull()) {
                break;
            }
            const QString path = line.trimmed();
            if (path.isEmpty()) {
                continue;
            }
            const QFileInfo info(path);
            QVariantMap analysis = symbolParser.parseFile(info.absoluteFilePath());
            analysis.insert(QStringLiteral("path"), info.absoluteFilePath());
            const ProjectIndex::FileFacts facts = ProjectIndex::factsFromAnalysis(analysis, info.size(),
                                                                                  info.lastModified().toMSecsSinceEpoch());
            QVariantMap payload = ProjectIndex::factsToVariant(facts);
            payload.insert(QStringLiteral("path"), info.absoluteFilePath());
            std::cout << QJsonDocument(QJsonObject::fromVariantMap(payload)).toJson(QJsonDocument::Compact).toStdString()
                      << std::endl;
        }
        return 0;
    }

    if (parser.isSet(indexProjectOption)) {
        ProjectIndex::BuildOptions options;
        options.root = resolveCliPath(parser.value(indexProjectOption), QDir::currentPath());
        options.helperPath = QCoreApplication::applicationFilePath();
        const ProjectIndex::SnapshotPtr snapshot = ProjectIndex::build(options);
        if (parser.isSet(indexEdgesOption)) {
            const QDir root(snapshot->root);
            for (const ProjectIndex::Edge &edge : snapshot->edges) {
                const QVariantMap item{
                    {QStringLiteral("from"), root.relativeFilePath(edge.fromPath)},
                    {QStringLiteral("caller"), edge.fromKey.isEmpty() ? QStringLiteral("(top level)") : edge.fromName},
                    {QStringLiteral("line"), edge.siteLine},
                    {QStringLiteral("to"), root.relativeFilePath(edge.to.path)},
                    {QStringLiteral("callee"), edge.to.owner.isEmpty() ? edge.to.name : edge.to.owner + QLatin1Char('.') + edge.to.name},
                    {QStringLiteral("calleeKind"), edge.to.kind},
                    {QStringLiteral("calleeLine"), edge.to.line},
                    {QStringLiteral("via"), edge.via},
                    {QStringLiteral("confidence"), edge.confidence},
                };
                std::cout << QJsonDocument(QJsonObject::fromVariantMap(item)).toJson(QJsonDocument::Compact).toStdString() << '\n';
            }
            std::cout.flush();
            return 0;
        }
        if (!parser.isSet(indexRelationsOption)) {
            std::cout << QJsonDocument(QJsonObject::fromVariantMap(snapshot->stats)).toJson(QJsonDocument::Indented).toStdString()
                      << std::endl;
            return 0;
        }
        const QString targetPath = resolveCliPath(parser.value(indexRelationsOption), QDir::currentPath());
        SymbolParser symbolParser;
        QVariantMap analysis = symbolParser.parseFile(targetPath);
        analysis.insert(QStringLiteral("path"), targetPath);
        analysis = ProjectIndex::augmentAnalysis(analysis, snapshot);
        // Compact view: symbol -> cross-file relation names.
        QVariantList out;
        std::function<void(const QVariantList &, const QString &)> collect = [&](const QVariantList &symbols, const QString &owner) {
            for (const QVariant &entry : symbols) {
                const QVariantMap symbol = entry.toMap();
                const QString name = owner.isEmpty() ? symbol.value(QStringLiteral("name")).toString()
                                                     : owner + QLatin1Char('.') + symbol.value(QStringLiteral("name")).toString();
                QVariantMap item;
                for (const QString &field : {QStringLiteral("calls"), QStringLiteral("calledBy")}) {
                    QVariantList relations;
                    for (const QVariant &relationEntry : symbol.value(field).toList()) {
                        const QVariantMap relation = relationEntry.toMap();
                        const QString relationPath = relation.value(QStringLiteral("path")).toString();
                        if (relationPath.isEmpty() || QFileInfo(relationPath).absoluteFilePath() == targetPath) {
                            continue;
                        }
                        relations.append(QVariantMap{
                            {QStringLiteral("name"), relation.value(QStringLiteral("name"))},
                            {QStringLiteral("kind"), relation.value(QStringLiteral("kind"))},
                            {QStringLiteral("path"), QDir(options.root).relativeFilePath(relationPath)},
                            {QStringLiteral("line"), relation.value(QStringLiteral("line"))},
                            {QStringLiteral("confidence"), relation.value(QStringLiteral("confidence"))},
                        });
                    }
                    if (!relations.isEmpty()) {
                        item.insert(field, relations);
                    }
                }
                if (symbol.contains(QStringLiteral("definition"))) {
                    const QVariantMap body = symbol.value(QStringLiteral("definition")).toMap();
                    item.insert(QStringLiteral("definition"), QStringLiteral("%1:%2").arg(
                        QDir(options.root).relativeFilePath(body.value(QStringLiteral("path")).toString()),
                        body.value(QStringLiteral("line")).toString()));
                }
                QStringList declarations;
                for (const QVariant &entry : symbol.value(QStringLiteral("declaredIn")).toList()) {
                    const QVariantMap declaration = entry.toMap();
                    declarations.append(QStringLiteral("%1:%2").arg(
                        QDir(options.root).relativeFilePath(declaration.value(QStringLiteral("path")).toString()),
                        declaration.value(QStringLiteral("line")).toString()));
                }
                if (!declarations.isEmpty()) {
                    item.insert(QStringLiteral("declaredIn"), declarations);
                }
                if (!item.isEmpty()) {
                    item.insert(QStringLiteral("symbol"), name);
                    item.insert(QStringLiteral("line"), symbol.value(QStringLiteral("line")));
                    if (symbol.contains(QStringLiteral("calledByTotal"))) {
                        item.insert(QStringLiteral("calledByTotal"), symbol.value(QStringLiteral("calledByTotal")));
                    }
                    out.append(item);
                }
                collect(symbol.value(QStringLiteral("members")).toList(), name);
            }
        };
        collect(analysis.value(QStringLiteral("symbols")).toList(), QString());
        // Routes this file serves, with the client calls that reach them, and
        // this file's own HTTP calls with the routes they reach.
        for (const QVariant &entry : analysis.value(QStringLiteral("routes")).toList()) {
            const QVariantMap route = entry.toMap();
            QVariantList clients;
            for (const QVariant &clientEntry : route.value(QStringLiteral("calledFrom")).toList()) {
                const QVariantMap client = clientEntry.toMap();
                clients.append(QVariantMap{{QStringLiteral("name"), client.value(QStringLiteral("name"))},
                                           {QStringLiteral("path"), QDir(options.root).relativeFilePath(client.value(QStringLiteral("path")).toString())},
                                           {QStringLiteral("line"), client.value(QStringLiteral("line"))},
                                           {QStringLiteral("confidence"), client.value(QStringLiteral("confidence"))}});
            }
            if (!clients.isEmpty()) {
                out.append(QVariantMap{{QStringLiteral("route"), route.value(QStringLiteral("label"), route.value(QStringLiteral("path")))},
                                       {QStringLiteral("line"), route.value(QStringLiteral("line"))},
                                       {QStringLiteral("calledFrom"), clients}});
            }
        }
        for (const QVariant &entry : analysis.value(QStringLiteral("httpCalls")).toList()) {
            const QVariantMap call = entry.toMap();
            QVariantList routes;
            for (const QVariant &routeEntry : call.value(QStringLiteral("routes")).toList()) {
                const QVariantMap route = routeEntry.toMap();
                routes.append(QVariantMap{{QStringLiteral("name"), route.value(QStringLiteral("name"))},
                                          {QStringLiteral("path"), QDir(options.root).relativeFilePath(route.value(QStringLiteral("path")).toString())},
                                          {QStringLiteral("line"), route.value(QStringLiteral("line"))},
                                          {QStringLiteral("confidence"), route.value(QStringLiteral("confidence"))}});
            }
            out.append(QVariantMap{{QStringLiteral("httpCall"), QStringLiteral("%1 %2").arg(call.value(QStringLiteral("method")).toString(),
                                                                                         call.value(QStringLiteral("url")).toString())},
                                   {QStringLiteral("line"), call.value(QStringLiteral("line"))},
                                   {QStringLiteral("routes"), routes}});
        }
        std::cout << QJsonDocument(QJsonArray::fromVariantList(out)).toJson(QJsonDocument::Indented).toStdString() << std::endl;
        return 0;
    }

    if (parser.isSet(debugAstOption)) {
        const QString targetPath = resolveCliPath(parser.value(debugAstOption), QDir::currentPath());
        const QJsonDocument doc(QJsonObject::fromVariantMap(SymbolParser::debugAst(targetPath)));
        std::cout << doc.toJson(QJsonDocument::Indented).toStdString() << std::endl;
        return 0;
    }

    if (parser.isSet(dumpFileOption)) {
        const QString targetPath = resolveCliPath(parser.value(dumpFileOption), QDir::currentPath());
        SymbolParser symbolParser;
        const QVariantMap parsed = symbolParser.parseFile(targetPath);
        const QJsonDocument doc(QJsonObject::fromVariantMap(parsed));
        std::cout << doc.toJson(QJsonDocument::Compact).toStdString() << std::endl;
        return 0;
    }

    ProjectController controller;
    // Deterministic output: the CLI builds the project index before analysing.
    controller.setIndexMode(parser.isSet(noIndexOption) ? ProjectController::IndexMode::Off
                                                        : ProjectController::IndexMode::Synchronous);

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
