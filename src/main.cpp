#include "MainWindow.h"
#include "LanguageManager.h"
#include "Settings.h"
#include "Theme.h"

#include "ZapretPaths.h"

#include <QApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>

namespace {

// One window per running application, and one window per signed-in user, and those are not the same
// rule. The channel is machine-global on every platform, so the username has to be in the name to keep
// two sessions apart. On Windows it is a named pipe under \\.\pipe\, a namespace shared by the whole
// box. On Unix Qt resolves a relative QLocalServer name against the temp directory (verified on Qt
// 6.10: the endpoint lands at /tmp/<name>, honoring $TMPDIR) — a directory all users read and write,
// not a per-user runtime dir. Either way, without the user in the name a second session's Muffin would
// answer an activation request meant for the first user's, and the visible symptom would be a taskbar
// click or a re-launch raising somebody else's window. So the suffix is load-bearing on both platforms,
// not a no-op on either: do not "simplify" it away because Unix hides the collision behind $USER.
//
// USERNAME is the Windows variable and USER the Unix one. Neither is guaranteed present (a service
// account, a stripped cron environment); the fallback name still gives one instance per machine, which
// is not the behaviour we want across sessions but is better than nothing running at all.
QString instanceChannel()
{
    const QString user = qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER"));
    return user.isEmpty() ? QStringLiteral("Muffin")
                          : QStringLiteral("Muffin-%1").arg(user);
}

// If another Muffin is listening, tell it to raise its window and let the caller exit. Two hundred
// milliseconds is generous for a loopback local socket and short enough that a wedged peer cannot hang
// the second launch: the whole point of the probe is to close the app before a second window appears,
// not to hold a conversation with a process that stopped answering. The return value is deliberately
// conservative — a "no" on timeout lets this launch start its own instance rather than silently
// refusing to run because the first one got stuck.
bool handOffToFirstInstance()
{
    QLocalSocket probe;
    probe.connectToServer(instanceChannel());
    if (!probe.waitForConnected(200))
        return false;
    probe.write("show");
    probe.flush();
    probe.waitForBytesWritten(200);
    probe.disconnectFromServer();
    return true;
}

} // namespace

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

    // Single-instance probe goes before anything that reads or writes user state. Both halves of the
    // migration below relocate files on disk, and a duplicate launch that skipped past this guard
    // would race the first one through them: the "upgrade looks like a fresh install with a lost
    // download" failure the migration code documents is exactly what two writers on the same tree
    // produce. Doing the check here — cheap, no state touched — means the second launch exits before
    // it can corrupt anything, and the first launch's window comes forward instead of a second
    // appearing beside it.
    if (handOffToFirstInstance())
        return 0;

    // Take the channel. removeServer is what makes this survive the case the probe above cannot
    // detect: the previous instance was killed without closing cleanly, so the endpoint is registered
    // but nobody is listening on it. Qt documents removeServer as the correct recovery — on Unix it
    // unlinks the stale socket, on Windows it is a no-op because named pipes disappear with the
    // process — and it is a safe thing to call when there is nothing to remove.
    QLocalServer::removeServer(instanceChannel());
    auto *singleInstance = new QLocalServer(&app);
    if (!singleInstance->listen(instanceChannel())) {
        // Best-effort. Losing the guard is not a reason to refuse to start: the app still works, only
        // "double-click a shortcut twice and get two windows" comes back. The stderr line is what a
        // support bundle needs to see; the reason is deliberately generic because the OS-level cause
        // (a leftover file that could not be unlinked, an unexpected permission problem) varies too
        // much to be worth a user-facing message that would only confuse.
        qWarning("Muffin: single-instance channel unavailable: %s",
                 qPrintable(singleInstance->errorString()));
    }

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

    // Drain every pending connection, not just one: two shortcuts clicked back-to-back queue up behind
    // each other on the channel, and only a loop makes sure the second launch's request is picked up on
    // the same wake. We do not look at the payload — the probe writes "show" as documentation and
    // nothing else speaks the channel, so "bytes arrived" and "raise the window" are the same event
    // here. deleteLater is the right disposal: the peer may still be finishing its write, and closing
    // synchronously can surface as a broken-pipe toast in the caller's short-lived process just as it
    // exits. The lambda captures both `singleInstance` and `window` by pointer/reference; window's
    // lifetime is main()'s scope, which is the whole of app.exec(), so the reference stays valid.
    QObject::connect(singleInstance, &QLocalServer::newConnection, &window,
                     [singleInstance, &window] {
                         while (QLocalSocket *peer = singleInstance->nextPendingConnection())
                             peer->deleteLater();
                         window.bringToFront();
                     });

    window.show();

    return app.exec();
}
