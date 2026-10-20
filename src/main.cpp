#include "MainWindow.h"
#include "LanguageManager.h"
#include "Settings.h"
#include "Theme.h"

#include "ZapretPaths.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Muffin"));
    // The organization is what 0.1.1 changed: it was "Muffin" here too, the same word as the
    // application name, so every per-user path Qt derives came out reading that name twice —
    // ~/.local/share/Muffin/Muffin on Linux, %APPDATA%\Muffin\Muffin on Windows. F0RSES is the vendor
    // the packages already name (CPACK_PACKAGE_VENDOR), and the application name stays Muffin, which
    // is what the display name, the .desktop entry and the installer all use.
    QApplication::setOrganizationName(QStringLiteral("F0RSES"));
    QApplication::setApplicationDisplayName(QStringLiteral("Muffin"));
    QApplication::setApplicationVersion(QStringLiteral("0.4.1"));
    // Linux desktop environments match a running app to its .desktop entry through this id.
    QApplication::setDesktopFileName(QStringLiteral("Muffin"));
    // Closing the window must not kill a running zapret unless the user chose "Выход" in the tray;
    // MainWindow::closeEvent decides, and Qt only quits once that handler accepted the close.
    QApplication::setQuitOnLastWindowClosed(true);

    // Both halves of the one-time identity migration, and both before anything reads a path or a
    // setting: renaming the organization silently relocated the two places 0.1.0 kept user state —
    // the downloaded zapret tree and the settings store — so an upgrade that skipped these would look
    // like a first run with a lost download. Each is a no-op on a fresh install and on every launch
    // after the first (see ZapretPaths::migrateManagedInstallDir and zapret::importLegacySettings),
    // which is what makes running them unconditionally at startup safe. Order matters: the folder move
    // has to happen first, because the settings repair only rewrites a path whose old directory is
    // gone and whose new one exists.
    zapret::ZapretPaths::migrateManagedInstallDir();
    zapret::importLegacySettings();

    // Theme first: Settings + Theme are read by MainWindow's constructor, and the palette must be in
    // place before the first widget is polished.
    zapret::Settings settings;
    zapret::Theme::apply(app, zapret::Theme::modeFromName(settings.theme()));
    // Language next: MainWindow builds its whole chrome from LanguageManager::tr(), so the stored
    // locale has to be installed before the first widget exists — a later setCode() would only be a
    // retranslate of text that was already written in the wrong language.
    zapret::LanguageManager::instance().setCode(settings.language());

    // No QApplication::setWindowIcon() on purpose. QWidget::windowIcon() falls back to the application
    // icon whenever the widget's own icon is null, and the main window's title bar is deliberately
    // empty — setting an app-wide icon here would bring the muffin back through that fallback. The
    // brand still lives where the design puts it (the rail logo, the tray, and the packaged
    // share/icons/hicolor/…/muffin.svg that the desktop entry names).

    zapret::MainWindow window;
    window.show();

    return app.exec();
}
