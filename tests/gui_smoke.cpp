// Headless GUI smoke test.
//
// Loads the real Main.qml (offscreen, software scene graph), finds the live
// ProjectController, and drives it through a set of files: open each file,
// then select every symbol, member, relation, dependency, route and quick
// link it offers (including ones that navigate to another file). Every QML
// warning raised along the way - TypeError, ReferenceError, "Unable to
// assign", binding loops - is collected, and the run fails if there is any.
//
// Usage (from the repository root):
//   QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
//       ./build/bin/lumencode-gui-smoke tests/fixtures/baseline [more roots or files]
//
// Each argument is a folder (every supported file below it is visited) or a
// single file.

#include <QApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QTimer>

#include <cstdio>
#include <iostream>

#include "projectcontroller.h"

namespace {

QStringList g_problems;
int g_messages = 0;

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    Q_UNUSED(context);
    ++g_messages;
    const bool fromQml = message.contains(QStringLiteral("qrc:/")) || message.contains(QStringLiteral("TypeError"))
        || message.contains(QStringLiteral("ReferenceError")) || message.contains(QStringLiteral("Binding loop"))
        || message.contains(QStringLiteral("Unable to assign"));
    if ((type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) && fromQml) {
        g_problems.append(message);
        std::fprintf(stderr, "QML PROBLEM: %s\n", qPrintable(message));
    }
}

void spin(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

bool waitForAnalysis(ProjectController *controller, int timeoutMs = 20000)
{
    QElapsedTimer timer;
    timer.start();
    spin(30);
    while (controller->analysisInProgress() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    spin(30);
    return !controller->analysisInProgress();
}

void render(QApplication &app, QQmlApplicationEngine &engine, const QString &screenshot = QString())
{
    // Force a frame so delegates and bindings of the visible panes are exercised.
    for (QObject *object : engine.rootObjects()) {
        if (auto *window = qobject_cast<QQuickWindow *>(object)) {
            QImage frame = window->grabWindow();
            if (frame.isNull() && !screenshot.isEmpty()) {
                // grabWindow() can come back empty offscreen; grab the root item instead.
                QSharedPointer<QQuickItemGrabResult> grab = window->contentItem()->grabToImage();
                QElapsedTimer waited;
                waited.start();
                bool ready = false;
                QObject::connect(grab.data(), &QQuickItemGrabResult::ready, &app, [&] { ready = true; });
                while (!ready && waited.elapsed() < 3000) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                }
                frame = grab->image();
            }
            if (!screenshot.isEmpty()) {
                if (!frame.save(screenshot)) {
                    std::fprintf(stderr, "screenshot %s failed (null=%d %dx%d, window %dx%d visible=%d exposed=%d)\n", qPrintable(screenshot), frame.isNull(), frame.width(), frame.height(), window->width(), window->height(), window->isVisible(), window->isExposed());
                }
                // e.g. LUMENCODE_SMOKE_SHOTS=/tmp/shots: eyeball the offscreen render
            }
        }
    }
}

// Path for a named screenshot when LUMENCODE_SMOKE_SHOTS is set, otherwise empty.
QString shotPath(const QString &name)
{
    const QString directory = qEnvironmentVariable("LUMENCODE_SMOKE_SHOTS");
    if (directory.isEmpty()) {
        return QString();
    }
    QDir().mkpath(directory);
    return QDir(directory).filePath(name + QStringLiteral(".png"));
}

// findChild() only walks QObject parents; pages pushed on the stack live in the visual tree.
QQuickItem *findItem(QQuickItem *from, const QString &objectName)
{
    if (!from) {
        return nullptr;
    }
    if (from->objectName() == objectName) {
        return from;
    }
    for (QQuickItem *child : from->childItems()) {
        if (QQuickItem *found = findItem(child, objectName)) {
            return found;
        }
    }
    return nullptr;
}

bool isSupported(const QString &path)
{
    static const QSet<QString> suffixes = {
        QStringLiteral("php"), QStringLiteral("js"), QStringLiteral("ts"), QStringLiteral("tsx"), QStringLiteral("jsx"),
        QStringLiteral("py"), QStringLiteral("java"), QStringLiteral("cs"), QStringLiteral("rs"), QStringLiteral("swift"),
        QStringLiteral("go"), QStringLiteral("c"), QStringLiteral("h"), QStringLiteral("cpp"), QStringLiteral("vb"),
        QStringLiteral("sql"), QStringLiteral("sh"), QStringLiteral("html"), QStringLiteral("css"), QStringLiteral("qml"),
        QStringLiteral("aspx"), QStringLiteral("ascx"), QStringLiteral("master"), QStringLiteral("ashx"), QStringLiteral("asmx"), QStringLiteral("asax"),
        QStringLiteral("json"), QStringLiteral("m"), QStringLiteral("kt"), QStringLiteral("rb"), QStringLiteral("scss"), QStringLiteral("less"),
    };
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

} // namespace

int main(int argc, char *argv[])
{
    qInstallMessageHandler(messageHandler);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("LumenCodeGuiSmoke"));
    app.setOrganizationName(QStringLiteral("OpenSource"));

    QStringList targets = app.arguments().mid(1);
    if (targets.isEmpty()) {
        targets.append(QStringLiteral("tests/fixtures/baseline"));
    }
    QStringList files;
    QString root;
    for (const QString &target : std::as_const(targets)) {
        const QFileInfo info(target);
        if (info.isDir()) {
            if (root.isEmpty()) {
                root = info.absoluteFilePath();
            }
            QDirIterator it(info.absoluteFilePath(), QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString path = it.next();
                if (isSupported(path)) {
                    files.append(path);
                }
            }
        } else if (info.exists()) {
            if (root.isEmpty()) {
                root = info.absolutePath();
            }
            files.append(info.absoluteFilePath());
        }
    }
    files.sort();

    qmlRegisterType<ProjectController>("Lumencode", 1, 0, "ProjectController");
    QQmlApplicationEngine engine;

    // Self-check: a deliberately broken binding must be reported, otherwise a
    // green run would mean nothing.
    {
        QQmlComponent probe(&engine);
        probe.setData("import QtQuick 2.15\nItem { width: missingObject.size }\n", QUrl(QStringLiteral("qrc:/smoke-self-check.qml")));
        QObject *object = probe.create();
        spin(20);
        const bool caught = !g_problems.isEmpty();
        delete object;
        if (!caught) {
            std::cerr << "self-check failed: a broken QML binding was not reported" << std::endl;
            return 3;
        }
        g_problems.clear();
    }

    engine.rootContext()->setContextProperty(QStringLiteral("cliInitialPath"), root);
    engine.rootContext()->setContextProperty(QStringLiteral("cliInitialFile"), QString());
    engine.load(QUrl(QStringLiteral("qrc:/contents/ui/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        std::cerr << "Main.qml failed to load" << std::endl;
        return 2;
    }
    ProjectController *controller = engine.rootObjects().first()->findChild<ProjectController *>();
    if (!controller) {
        std::cerr << "ProjectController not found in the QML tree" << std::endl;
        return 2;
    }
    if (controller->rootPath() != root) {
        controller->setRootPath(root);
    }
    waitForAnalysis(controller);
    // Let the background project index finish, so cross-file relations (and
    // navigation through them) are exercised too.
    {
        QElapsedTimer timer;
        timer.start();
        while (controller->indexStatus().value(QStringLiteral("state")).toString() == QStringLiteral("building")
               && timer.elapsed() < 120000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
    }
    const QVariantMap indexStatus = controller->indexStatus();
    std::fprintf(stdout, "index: %s, %d files, %d cross-file edges\n",
                 qPrintable(indexStatus.value(QStringLiteral("state")).toString()),
                 indexStatus.value(QStringLiteral("files")).toInt(), indexStatus.value(QStringLiteral("crossFileEdges")).toInt());
    render(app, engine);

    // Search: type into the real search field, check the results list fills,
    // and open results (a symbol and a file) through the controller.
    int searchChecks = 0;
    int searchFailures = 0;
    if (indexStatus.value(QStringLiteral("state")).toString() == QStringLiteral("ready")) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QObject *field = window ? findItem(window->contentItem(), QStringLiteral("searchField")) : nullptr;
        QObject *results = window ? findItem(window->contentItem(), QStringLiteral("searchResults")) : nullptr;
        if (!field || !results) {
            std::fprintf(stderr, "SEARCH: search field or results list not found in the QML tree\n");
            ++searchFailures;
        } else {
            for (const QString &query : {QStringLiteral("handle"), QStringLiteral("total"), QStringLiteral("sample"), QStringLiteral("index")}) {
                field->setProperty("text", query);
                spin(400);
                render(app, engine, shotPath(QStringLiteral("search-") + query));
                const int shown = results->property("count").toInt();
                const QVariantList found = controller->search(query, QString(), 20);
                ++searchChecks;
                if (shown != found.size()) {
                    std::fprintf(stderr, "SEARCH: '%s' shows %d rows but the controller found %d\n", qPrintable(query), shown, static_cast<int>(found.size()));
                    ++searchFailures;
                }
                if (!found.isEmpty()) {
                    const QVariantMap first = found.first().toMap();
                    controller->openSearchResult(first);
                    waitForAnalysis(controller);
                    ++searchChecks;
                    if (QFileInfo(controller->selectedPath()).absoluteFilePath() != QFileInfo(first.value(QStringLiteral("absolutePath")).toString()).absoluteFilePath()) {
                        std::fprintf(stderr, "SEARCH: opening '%s' selected %s\n", qPrintable(first.value(QStringLiteral("name")).toString()),
                                     qPrintable(controller->selectedPath()));
                        ++searchFailures;
                    }
                    render(app, engine);
                }
            }
            field->setProperty("text", QString());
            spin(200);
        }
        std::fprintf(stdout, "search: %d checks, %d failures\n", searchChecks, searchFailures);
    }

    // Optional gallery (LUMENCODE_SMOKE_SHOTS=<dir>): screenshots of the states Phase F styles -
    // broken links, damaged code, SQL relation labels.
    if (!shotPath(QStringLiteral("x")).isEmpty()) {
        struct Shot
        {
            const char *file;
            const char *symbol; // empty: the file itself
            const char *name;
        };
        const Shot gallery[] = {
            {"web_app/index.html", "", "gallery-broken-links"},
            {"web_app/js/app.js", "clearCompleted", "gallery-broken-dom-id"},
            {"python_recovery/broken.py", "wrapper_broken_py", "gallery-damaged-symbol"},
            {"sql_tsql/broken.sql", "Items", "gallery-sql-relations"},
            {"sql_tsql/schema.sql", "", "gallery-sql-file"},
        };
        for (const Shot &shot : gallery) {
            const QString path = QDir(root).filePath(QString::fromUtf8(shot.file));
            if (!QFileInfo::exists(path)) {
                continue;
            }
            controller->selectPath(path);
            waitForAnalysis(controller);
            const QString wanted = QString::fromUtf8(shot.symbol);
            if (!wanted.isEmpty()) {
                std::function<QVariantMap(const QVariantList &)> find = [&](const QVariantList &symbols) -> QVariantMap {
                    for (const QVariant &entry : symbols) {
                        const QVariantMap symbol = entry.toMap();
                        if (symbol.value(QStringLiteral("name")).toString() == wanted) {
                            return symbol;
                        }
                        const QVariantMap nested = find(symbol.value(QStringLiteral("members")).toList());
                        if (!nested.isEmpty()) {
                            return nested;
                        }
                    }
                    return {};
                };
                const QVariantMap symbol = find(controller->selectedFileData().value(QStringLiteral("symbols")).toList());
                if (!symbol.isEmpty()) {
                    controller->selectSymbolByData(symbol);
                    waitForAnalysis(controller);
                }
            }
            spin(150);
            render(app, engine, shotPath(QString::fromUtf8(shot.name)));
        }
    }

    int selections = 0;
    int timeouts = 0;
    for (const QString &file : std::as_const(files)) {
        controller->selectPath(file);
        if (!waitForAnalysis(controller)) {
            ++timeouts;
            std::fprintf(stderr, "TIMEOUT opening %s\n", qPrintable(file));
            continue;
        }
        render(app, engine);
        const QVariantMap fileData = controller->selectedFileData();

        QVariantList targetsToSelect;
        std::function<void(const QVariantList &, int)> collectSymbols = [&](const QVariantList &symbols, int depth) {
            for (const QVariant &entry : symbols) {
                const QVariantMap symbol = entry.toMap();
                targetsToSelect.append(symbol);
                for (const QString &field : {QStringLiteral("calls"), QStringLiteral("calledBy")}) {
                    for (const QVariant &relation : symbol.value(field).toList()) {
                        targetsToSelect.append(relation);
                    }
                }
                if (depth < 2) {
                    collectSymbols(symbol.value(QStringLiteral("members")).toList(), depth + 1);
                }
            }
        };
        collectSymbols(fileData.value(QStringLiteral("symbols")).toList(), 0);
        for (const QString &collection : {QStringLiteral("dependencies"), QStringLiteral("routes"),
                                          QStringLiteral("quickLinks"), QStringLiteral("relatedFiles")}) {
            for (const QVariant &entry : fileData.value(collection).toList()) {
                targetsToSelect.append(entry);
            }
        }
        const QVariantMap cssSummary = fileData.value(QStringLiteral("cssSummary")).toMap();
        for (const QString &collection : {QStringLiteral("matchedClasses"), QStringLiteral("missingClasses")}) {
            for (const QVariant &entry : cssSummary.value(collection).toList()) {
                targetsToSelect.append(entry);
            }
        }

        int perFile = 0;
        for (const QVariant &entry : std::as_const(targetsToSelect)) {
            if (++perFile > 60) {
                break;
            }
            const QString before = controller->selectedPath();
            controller->selectSymbolByData(entry.toMap());
            if (!waitForAnalysis(controller)) {
                ++timeouts;
                std::fprintf(stderr, "TIMEOUT selecting in %s\n", qPrintable(file));
            }
            render(app, engine);
            ++selections;
            if (controller->selectedPath() != before) {
                // Navigated to another file (cross-file link): come back.
                controller->selectPath(file);
                waitForAnalysis(controller);
            }
        }
        std::fprintf(stdout, "ok %-70s %d selections\n", qPrintable(QDir(root).relativeFilePath(file)), perFile);
        std::fflush(stdout);
    }

    std::fprintf(stdout, "\nfiles: %lld, selections: %d, timeouts: %d, QML problems: %lld\n",
                 static_cast<long long>(files.size()), selections, timeouts, static_cast<long long>(g_problems.size()));
    QStringList unique = g_problems;
    unique.removeDuplicates();
    for (const QString &problem : std::as_const(unique)) {
        std::fprintf(stdout, "  %s\n", qPrintable(problem));
    }
    return (g_problems.isEmpty() && timeouts == 0 && searchFailures == 0) ? 0 : 1;
}
