#include "MainWindow.h"
#include "Settings.h"
#include "Theme.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ZapretGui"));
    QApplication::setOrganizationName(QStringLiteral("ZapretGui"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    // Closing the window must not kill a running zapret unless the user chose "Выход" in the tray;
    // MainWindow::closeEvent decides, and Qt only quits once that handler accepted the close.
    QApplication::setQuitOnLastWindowClosed(true);

    // Theme first: Settings + Theme are read by MainWindow's constructor, and the palette must be in
    // place before the first widget is polished.
    zapret::Settings settings;
    zapret::Theme::apply(app, zapret::Theme::modeFromName(settings.theme()));
    QApplication::setWindowIcon(
        zapret::Theme::icon(QStringLiteral("app"), zapret::Theme::accent(), 32));

    zapret::MainWindow window;
    window.show();

    return app.exec();
}
