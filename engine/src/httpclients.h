#pragma once

// HTTP client calls in browser-side code, and matching them to backend routes.
//
// Client calls are found in JS/TS (fetch, axios, jQuery $.ajax / $.get /
// $.post / $.getJSON, XMLHttpRequest.open, Angular-style http.get<T>(...))
// and in HTML / PHP templates (<form action method>). Each call records its
// method, a normalised URL (dynamic parts become `*`) and the enclosing
// callable, and is stored on the analysis as `httpCalls`.
//
// Routes and client URLs are compared segment by segment; route parameters
// (:id, <int:id>, {id}, {id:[0-9]+}, *rest) match any one segment. A full
// match is medium confidence; a match of the route against the tail of the
// URL (routers mounted under a prefix) is low.

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace HttpClients {

QVariantMap applyHttpClientCalls(QVariantMap analysis, const QString &text, const QString &language);

// Normalised path segments ('*' for a dynamic or parameter segment); empty
// when the text is not a usable URL / route path.
QStringList urlSegments(const QString &url);
QStringList routeSegments(const QString &routePath);

// 0 = no match, 2 = full match, 1 = the route matches the URL's tail.
int matchRoute(const QStringList &urlParts, const QStringList &routeParts, bool urlHasOpenTail);

bool methodsCompatible(const QString &clientMethod, const QString &routeMethod);

} // namespace HttpClients
