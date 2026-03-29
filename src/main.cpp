#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "projectcontroller.h"

int main(int argc, char *argv[])
{
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("LumenCode"));
    app.setOrganizationName(QStringLiteral("OpenSource"));

    qmlRegisterType<ProjectController>("Lumencode", 1, 0, "ProjectController");

    QQmlApplicationEngine engine;
    engine.load(QUrl(QStringLiteral("qrc:/contents/ui/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    return app.exec();
}
