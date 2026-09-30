#pragma once

// Compact, agent-oriented views of the analysis (lumencode-cli --outline,
// --find, --callers, --callees, --routes): names, kinds, line ranges,
// signatures and relation names only - no snippets - so a whole file's
// structure costs a few hundred tokens instead of a full dump.

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "projectindex.h"

namespace AgentQueries {

// Outline of one analysed file. `root` makes paths relative; relations into
// other files read "name @ path:line".
QVariantMap outline(const QVariantMap &analysis, const QString &root);
QString outlineText(const QVariantMap &outline);

// Definitions named `name` ("name" or "Owner.name") across the project.
QVariantList findDefinitions(const ProjectIndex::SnapshotPtr &snapshot, const QString &name);

// Calls or callers (field "calls" / "calledBy") of each definition named
// `name`, same-file and cross-file, read from the analysed definition file.
QVariantList relationsOf(const ProjectIndex::SnapshotPtr &snapshot, const QString &name, const QString &field,
                         const std::function<QVariantMap(const QString &)> &analyse, bool includeLoose = false);

// Ranked search over definition names (Owner.name) and file paths. `query` is
// matched case-insensitively: exact name, prefix, word/camelCase-initial,
// substring, then a subsequence of 4+ characters ("prjctrl" finds ProjectController). `kinds` (e.g. "function,class"; "file" selects file paths) and
// `language` narrow the result; `pathContains` restricts to a folder.
// Results: {name, kind, path, line, language, score}, best first, at most `limit`.
QVariantList searchSymbols(const ProjectIndex::SnapshotPtr &snapshot, const QString &query, const QStringList &kinds = {},
                           const QString &language = QString(), const QString &pathContains = QString(), int limit = 50);

// Every route in the project with the HTTP client calls that reach it.
QVariantList routes(const ProjectIndex::SnapshotPtr &snapshot);

} // namespace AgentQueries
