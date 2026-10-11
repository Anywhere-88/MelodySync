// ============================================================================
//  android/src/main.cpp —— MelodySync 安卓入口（骨架阶段）
//  后续阶段在此挂 SearchAggregator / DownloadEngine 桥 + WebSocket 服务。
// ============================================================================
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "BackendProbe.h"

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("MelodySync"));
    QGuiApplication::setOrganizationName(QStringLiteral("CHITSTUDIO"));

    ms::BackendProbe probe;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("backendProbe"), &probe);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("MelodySyncApp"), QStringLiteral("Main"));
    probe.run();

    return app.exec();
}
