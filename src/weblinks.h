#pragma once

// Cross-language links for web pages: HTML <-> CSS <-> JavaScript.
//
// An HtmlPage is the structural model of one HTML file (built with the
// tree-sitter-html grammar): its element ids and classes, inline event
// handlers, inline <script>/<style> blocks and the local assets it links.
// JavaScript files contribute DomReferences (getElementById, querySelector,
// classList.add, $('#x') ...). SymbolParser combines the two so that:
//   - an HTML page links to the JS functions its handlers call and to the
//     CSS rules / JS code that use its ids and classes,
//   - a JS file links to the HTML elements and CSS rules it touches,
//   - a CSS file knows which of its classes are only applied from JS.
//
// Pages are cached per path + size + mtime, so repeated lookups during one
// analysis (or one CLI/GUI session) do not re-read or re-parse them.

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

namespace WebLinks {

struct HtmlElement
{
    QString tag;
    QString id;
    QStringList classes;
    int line = 0;
    QString snippet; // the start tag
};

struct HtmlHandler
{
    QString tag;
    QString attribute; // onclick, onsubmit, ...
    QString code;
    QStringList calledNames; // bare function names called by the handler code
    int line = 0;
    QString snippet;
};

struct HtmlInlineBlock
{
    QString kind; // "script" or "style"
    QString type; // type attribute (module, text/babel, ...)
    int startLine = 0; // line of the first content character
    int startOffset = 0; // byte offset of the content in the file
    QString content;
};

struct HtmlAsset
{
    QString kind; // script, stylesheet, page, form, image, frame
    QString target; // attribute value as written
    QString resolvedPath; // absolute path for local targets, empty otherwise
    bool local = false;
    bool exists = false;
    int line = 0;
    QString snippet;
};

struct HtmlPage
{
    QString path;
    bool valid = false;
    QList<HtmlElement> elements; // elements carrying an id and/or classes
    QList<HtmlElement> customElements; // tags containing '-' (web components)
    QList<HtmlHandler> handlers;
    QList<HtmlInlineBlock> inlineBlocks;
    QList<HtmlAsset> assets;

    const HtmlElement *elementWithId(const QString &id) const;
    QList<const HtmlElement *> elementsWithClass(const QString &className) const;
    QStringList usedClasses() const; // in first-use order
    bool linksAsset(const QString &absolutePath, const QString &kind) const;
};

// Load (or fetch from cache) the model for an HTML (or PHP template) file.
HtmlPage loadHtmlPage(const QString &path);
// Same, from text already in memory (not cached).
HtmlPage parseHtmlPage(const QString &path, const QString &text);

// HTML pages that link the given asset (kind: "script" or "stylesheet").
// Looks in the asset's folder and up to three ancestor folders, since pages
// usually sit above their js/ and css/ folders.
QList<HtmlPage> pagesReferencingAsset(const QString &assetPath, const QString &kind);

struct DomReference
{
    QString kind; // "id", "class", "custom-element"
    QString name;
    QString via; // getElementById, querySelector, classList.add, ...
    int line = 0;
    QString snippet;
};

// DOM references made by JavaScript source (parsed with the JS grammar).
QList<DomReference> extractDomReferences(const QString &jsText);

// Custom elements registered with customElements.define('tag-name', Class):
// tag name -> class name.
QHash<QString, QString> extractCustomElementDefinitions(const QString &jsText);

// Top-level (global) functions of a script file: name -> line. Cached.
QHash<QString, int> globalFunctionsOfScript(const QString &path);

// Selector helpers.
QStringList idsInSelector(const QString &selector);
QStringList classesInSelector(const QString &selector);

} // namespace WebLinks
