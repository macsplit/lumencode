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
                         const std::function<QVariantMap(const QString &)> &analyse);

// Every route in the project with the HTTP client calls that reach it.
QVariantList routes(const ProjectIndex::SnapshotPtr &snapshot);

} // namespace AgentQueries
