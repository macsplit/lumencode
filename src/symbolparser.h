#pragma once

#include <QObject>
#include <QVariantMap>

class SymbolParser final : public QObject
{
    Q_OBJECT

public:
    explicit SymbolParser(QObject *parent = nullptr);

    QVariantMap parseFile(const QString &path) const;

    static QVariantMap makeResultSkeleton(const QString &path, const QString &fileName, const QString &language);

private:
    QVariantMap parsePhpTreeSitter(const QString &path, const QString &text) const;
    QVariantMap parseScriptLikeTreeSitter(const QString &path, const QString &text, const QString &language) const;
    QVariantMap parseCssTreeSitter(const QString &path, const QString &text) const;
    QVariantMap parsePythonTreeSitter(const QString &path, const QString &text) const;
    QVariantMap parseRustTreeSitter(const QString &path, const QString &text) const;
    QVariantMap parsePython(const QString &path, const QString &text) const;
    QVariantMap parseCppLike(const QString &path, const QString &text, const QString &language) const;
    QVariantMap parseJava(const QString &path, const QString &text) const;
    QVariantMap parseCSharp(const QString &path, const QString &text) const;
    QVariantMap parseRust(const QString &path, const QString &text) const;
    QVariantMap parseObjectiveC(const QString &path, const QString &text, const QString &language) const;

    QVariantMap parsePhp(const QString &path, const QString &text) const;
    QVariantMap parseScriptLike(const QString &path, const QString &text, bool reactMode) const;
    QVariantMap parseHtml(const QString &path, const QString &text) const;
    QVariantMap parseCss(const QString &path, const QString &text) const;
    QVariantMap parseJson(const QString &path, const QString &text) const;

    static QString detectLanguage(const QString &path);
    static QVariantMap makeSymbol(const QString &kind, const QString &name, int line,
                                  const QString &detail = QString(),
                                  const QVariantList &members = {},
                                  const QString &snippet = QString());
    static QVariantList parseClassMembers(const QString &body, const QString &language);
    static QVariantList parseObjectMembers(const QString &body);
    static QStringList extractHtmlClasses(const QString &text);
    static QStringList extractCssClasses(const QString &text);
    static QVariantList extractDependencyLinks(const QString &path, const QString &text);
    static QVariantList extractPythonDependencies(const QString &text);
    static QVariantList extractCppDependencies(const QString &path, const QString &text);
    static QVariantList extractJavaDependencies(const QString &text);
    static QVariantList extractCSharpDependencies(const QString &text);
    static QVariantList extractRustDependencies(const QString &text);
    static QVariantList extractObjectiveCDependencies(const QString &path, const QString &text);
    static QVariantList extractExpressRoutes(const QString &text);
    static QVariantList extractPythonRoutes(const QString &path, const QString &text);
    static QVariantList extractAspNetRoutes(const QString &path, const QString &text);
    static QVariantList findRelatedFiles(const QString &path);
    static QVariantMap extractPackageSummary(const QString &path, const QString &text);
};
