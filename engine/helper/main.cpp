// Transport only. Language behavior belongs to the unmodified upstream engine.
#include "symbolparser.h"
#include "projectindex.h"
#include "agentqueries.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <iostream>

static void print(const QVariantMap &payload)
{
    std::cout << QJsonDocument(QJsonObject::fromVariantMap(payload)).toJson(QJsonDocument::Compact).toStdString() << std::endl;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("lumencode-engine"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({
        {QStringList{"dump-file"}, "Analyse one file", "path"},
        {QStringList{"index-facts"}, "Read paths and emit original engine facts"},
        {QStringList{"root"}, "Project root", "path"},
        {QStringList{"path"}, "File to analyse with project relations", "path"},
        {QStringList{"build-index"}, "Refresh original project index"},
        {QStringList{"cache"}, "Explicit index cache path", "path"},
        {QStringList{"search"}, "Search the project index", "query"},
        {QStringList{"kind"}, "Filter search kinds", "kinds"}
    });
    parser.process(app);
    SymbolParser engine;
    if (parser.isSet("index-facts")) {
        QTextStream input(stdin);
        input.setCodec("UTF-8");
        while (!input.atEnd()) {
            const QString line = input.readLine().trimmed();
            if (line.isEmpty()) continue;
            QFileInfo info(line);
            const auto analysis = engine.parseFile(info.absoluteFilePath());
            print(ProjectIndex::factsToVariant(ProjectIndex::factsFromAnalysis(analysis, info.size(), info.lastModified().toMSecsSinceEpoch())));
        }
        return 0;
    }
    if (parser.isSet("dump-file")) {
        print(engine.parseFile(QFileInfo(parser.value("dump-file")).absoluteFilePath()));
        return 0;
    }
    if (!parser.isSet("root")) parser.showHelp(2);
    ProjectIndex::BuildOptions options;
    options.root = QFileInfo(parser.value("root")).absoluteFilePath();
    options.helperPath = QCoreApplication::applicationFilePath();
    options.cachePath = parser.value("cache");
    const auto snapshot = ProjectIndex::build(options);
    if (parser.isSet("build-index")) {
        print(snapshot->stats);
    } else if (parser.isSet("search")) {
        print({{"results", AgentQueries::searchSymbols(snapshot, parser.value("search"), {parser.value("kind")}, {}, {}, 50)}});
    } else if (parser.isSet("path")) {
        const QString path = QFileInfo(parser.value("path")).absoluteFilePath();
        print(ProjectIndex::augmentAnalysis(engine.parseFile(path), snapshot));
    } else {
        parser.showHelp(2);
    }
}
