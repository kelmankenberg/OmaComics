// omacomics — one line on what it does.

#include <QGuiApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QKeySequence>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>
#include <cstring>

#include "backend.h"
#include "folderpicker.h"
#include "theme.h"
#include "translator.h"

namespace {

void prepareHeadlessArguments(int argc, char *argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0
            || std::strcmp(argv[i], "--version") == 0) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            return;
        }
    }
}

} // namespace

int main(int argc, char *argv[]) {
    prepareHeadlessArguments(argc, argv);
    QGuiApplication app(argc, argv);
    app.setApplicationName("omacomics");
    app.setApplicationVersion("0.1.0");

    // Matches the window to omacomics.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon. The compiled-in copy covers
    // a run from build/, and needs qt6-svg's icon engine.
    app.setDesktopFileName("omacomics");
    app.setWindowIcon(QIcon::fromTheme("omacomics", QIcon(":/omacomics.svg")));

    QQuickStyle::setStyle("Material");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("resume"), QCoreApplication::translate("main", "Open the last-read comic.")});
    parser.addPositionalArgument(QStringLiteral("comic"),
                                 QCoreApplication::translate("main", "Comic archive or PDF to open."),
                                 QStringLiteral("[comic]"));
    parser.process(app);

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;
    FolderPicker folderPicker;

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("folderPicker", &folderPicker);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    QObject::connect(&folderPicker, &FolderPicker::folderSelected,
                     &backend, &Backend::addLibraryFolder);
    QObject::connect(&folderPicker, &FolderPicker::failed, &backend,
                     [&backend](const QString &message) { backend.reportError(message); });
    if (!qEnvironmentVariableIsSet("OMARCHY_LOOK_SCREENSHOT"))
        QTimer::singleShot(0, &backend, &Backend::loadComicVineKey);
    if (!parser.positionalArguments().isEmpty()) {
        const QString path = parser.positionalArguments().first();
        QTimer::singleShot(0, &backend, [&backend, path] { backend.openExternalComic(path); });
    } else if (parser.isSet(QStringLiteral("resume"))) {
        QTimer::singleShot(0, &backend, &Backend::resumeComic);
    } else {
        QTimer::singleShot(0, &backend, &Backend::applyStartupBehavior);
    }

    // `omarchy agent look app` runs the app offscreen with these set, to see
    // the window without a display: size it to OMARCHY_LOOK_SIZE when given,
    // press the keys in OMARCHY_LOOK_KEYS through the window's Shortcuts,
    // save it once it has settled, and quit.
    if (const QString shot = qEnvironmentVariable("OMARCHY_LOOK_SCREENSHOT"); !shot.isEmpty()) {
        const QStringList size = qEnvironmentVariable("OMARCHY_LOOK_SIZE").split('x');
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()); window && size.size() == 2)
            window->resize(size[0].toInt(), size[1].toInt());
        const QStringList keys = qEnvironmentVariable("OMARCHY_LOOK_KEYS").split(' ', Qt::SkipEmptyParts);
        for (int i = 0; i < keys.size(); ++i) {
            QTimer::singleShot(800 + i * 300, &app, [&engine, key = QKeySequence(keys[i])] {
                for (QObject *shortcut : engine.rootObjects().constFirst()->findChildren<QObject *>()) {
                    if (qstrcmp(shortcut->metaObject()->className(), "QQuickShortcut") == 0
                        && shortcut->property("enabled").toBool()
                        && QKeySequence(shortcut->property("sequence").toString()) == key) {
                        QMetaObject::invokeMethod(shortcut, "activated");
                        break;
                    }
                }
            });
        }
        const int delay = qEnvironmentVariableIsSet("OMARCHY_LOOK_DELAY_MS") ? qEnvironmentVariableIntValue("OMARCHY_LOOK_DELAY_MS") : 1500;
        QTimer::singleShot(800 + keys.size() * 300 + delay, &app, [&engine, shot] {
            if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()))
                window->grabWindow().save(shot);
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
