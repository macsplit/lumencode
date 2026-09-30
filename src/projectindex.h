#pragma once

// Project index: a project-wide view built from per-file facts, used for
// cross-file Calls / Called By in every language.
//
// Facts per file (definitions, resolved imports, call sites, routes) come from
// `lumencode-cli --index-facts`, run in batches in a helper process so a
// parser crash only costs that file. Facts are cached on disk per project and
// reused while a file's size and modification time are unchanged.
//
// Each call site is resolved once, most specific evidence first:
//   1. the callee is defined in a file this file imports (incl. aliases,
//      namespace and require bindings)                     -> high
//   2. the call's qualifier names the callee's owning type
//      (Mailer::send, new Mailer, Mailer.create)           -> medium
//   3. the callee is in the same directory / package       -> medium
//   4. the name is defined exactly once in the project and is not generic
//                                                         -> low
// Ambiguous names stay unresolved rather than guessed. Every resolved edge is
// stored in both directions, so cross-file relations are reciprocal by
// construction.

#include <QHash>
#include <QMultiHash>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

namespace ProjectIndex {

struct Definition
{
    QString path;
    QString language;
    QString name;
    QString kind;
    QString owner; // enclosing type / scope name, if any
    QString key; // kind|name|line, as in analysis payloads
    int line = 0;
    bool declaration = false; // C/C++ prototype (the body is elsewhere)
};

struct Import
{
    QString target;
    QString path; // resolved local file, if any
    QHash<QString, QString> bindings; // local name -> imported name ('*' / 'default' for whole-module)
};

struct CallSite
{
    QString fromKey; // empty for file-level code
    QString fromName;
    QString fromKind;
    int fromLine = 0;
    QString name;
    QString qualifier;
    int line = 0;
};

struct FileFacts
{
    QString path;
    QString language;
    qint64 size = 0;
    qint64 modified = 0;
    bool failed = false;
    QVector<Definition> definitions;
    QVector<Import> imports;
    QVector<CallSite> callSites;
    QVariantList routes;
    QVariantList httpCalls; // {method, url, line, via, fromKey?, fromName?}
};

struct Edge
{
    QString fromPath;
    QString fromKey;
    QString fromName;
    QString fromKind;
    int fromLine = 0;
    int siteLine = 0;
    Definition to;
    QString via; // import / qualifier / package / unique-name
    QString confidence; // high / medium / low
};

class Snapshot
{
public:
    QString root;
    QHash<QString, FileFacts> files;
    QMultiHash<QString, int> definitionsByName; // name -> index into allDefinitions
    QVector<Definition> allDefinitions;
    QVector<Edge> edges;
    QHash<QString, QVector<int>> outgoingByFromKey; // path|key -> edge indexes
    QHash<QString, QVector<int>> incomingByToKey; // path|kind|name -> edge indexes
    QHash<QString, int> definitionForDeclaration; // path|kind|name of a prototype -> allDefinitions index
    QHash<QString, QVector<int>> declarationsForDefinition; // path|kind|name of a body -> prototypes
    QVector<Edge> httpEdges; // client call -> route (to.kind == "route", to.line = route line)
    QHash<QString, QVector<int>> httpIncomingByRoute; // path|routeLine -> httpEdges indexes
    QHash<QString, QStringList> mountPrefixes; // router file -> prefixes it is mounted under
    QVariantMap stats;

    bool contains(const QString &path) const { return files.contains(path); }

    // Resolve one call site made from `path` (used for the live, currently
    // analysed file as well as during the build).
    bool resolve(const QString &path, const CallSite &site, Edge *edge) const;
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

struct BuildOptions
{
    QString root;
    QString helperPath; // lumencode-cli
    QString cachePath; // empty: default under the user cache dir
    int maxFiles = 20000;
    int batchSize = 150;
    std::function<void(int done, int total)> progress;
    const std::atomic_bool *cancel = nullptr;
};

// Build (or refresh from cache) the index for a project root.
SnapshotPtr build(const BuildOptions &options);

// Candidate files under a root (supported languages, skipping dependency and
// build folders).
QStringList candidateFiles(const QString &root, int maxFiles);

// Facts for one analysed file (what `--index-facts` prints).
FileFacts factsFromAnalysis(const QVariantMap &analysis, qint64 size, qint64 modified);
QVariantMap factsToVariant(const FileFacts &facts);
FileFacts factsFromVariant(const QVariantMap &map);

// Add cross-file calls / calledBy from the index to an analysis payload:
// outgoing edges are resolved live from the analysis' own call sites, incoming
// edges come from the snapshot.
QVariantMap augmentAnalysis(const QVariantMap &analysis, const SnapshotPtr &snapshot);

// Routes an HTTP client call can reach (best matches only).
QVector<Edge> resolveHttpCall(const Snapshot &snapshot, const QString &fromPath, const QVariantMap &call);

} // namespace ProjectIndex
