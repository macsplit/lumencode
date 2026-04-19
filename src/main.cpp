#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QCommandLineParser>
#include <QFileInfo>

#include "projectcontroller.h"

int main(int argc, char *argv[])
{
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("LumenCode"));
    app.setOrganizationName(QStringLiteral("OpenSource"));
    app.setDesktopFileName(QStringLiteral("lumencode"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/lumencode.svg")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Structural Code Explorer"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("path"), QStringLiteral("Folder or file to open"), QStringLiteral("[path]"));
    parser.process(app);

    QString initialPath;
    QString initialFile;
    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) {
        QFileInfo info(args.first());
        if (info.exists()) {
            if (info.isDir()) {
                initialPath = info.absoluteFilePath();
            } else {
                initialPath = info.absolutePath();
                initialFile = info.absoluteFilePath();
            }
        }
    }

    qmlRegisterType<ProjectController>("Lumencode", 1, 0, "ProjectController");

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cliInitialPath"), initialPath);
    engine.rootContext()->setContextProperty(QStringLiteral("cliInitialFile"), initialFile);

    engine.load(QUrl(QStringLiteral("qrc:/contents/ui/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    return app.exec();
}
