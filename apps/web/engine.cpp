// Browser transport only; language and graph behaviour live in engine/src.
#include "symbolparser.h"
#include "projectindex.h"
#include "agentqueries.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <emscripten/bind.h>

namespace {
ProjectIndex::SnapshotPtr snapshot;

std::string request(const std::string &json)
{
    static int argc = 1;
    static char name[] = "lumencode-web";
    static char *argv[] = {name, nullptr};
    static QCoreApplication app(argc, argv);
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    if (!document.isObject()) return R"({"error":"Expected a JSON request object"})";
    const QVariantMap input = document.object().toVariantMap();
    const QString operation = input.value("operation").toString();
    const QString path = input.value("path").toString();
    QVariantMap result;
    if (operation == "analyse") {
        SymbolParser parser;
        result = parser.parseFile(path);
        if (input.value("indexed").toBool() && snapshot)
            result = ProjectIndex::augmentAnalysis(result, snapshot);
    } else if (operation == "candidates") {
        result.insert("paths", ProjectIndex::candidateFiles(input.value("root").toString(), 20000));
    } else if (operation == "index") {
        ProjectIndex::BuildOptions options;
        options.root = input.value("root").toString();
        options.helperPath = "wasm-shared-engine-v1";
        options.cachePath = "/cache/project-index.json";
        snapshot = ProjectIndex::build(options);
        result = snapshot->stats;
    } else if (operation == "search") {
        if (!snapshot) return R"({"error":"Build the project index first"})";
        result.insert("results", AgentQueries::searchSymbols(snapshot,
            input.value("query").toString(), {}, {}, {}, 50));
    } else if (operation == "reset") {
        snapshot.reset();
        result.insert("reset", true);
    } else {
        return R"({"error":"Unknown engine operation"})";
    }
    return QJsonDocument(QJsonObject::fromVariantMap(result)).toJson(QJsonDocument::Compact).toStdString();
}
}

EMSCRIPTEN_BINDINGS(lumencode) {
    emscripten::function("request", &request);
}
