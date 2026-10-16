#include "ZapretManager.h"

#include "LinuxBackend.h"
#include "StrategyParser.h"
#include "WindowsBackend.h"
#include "ZapretLocator.h"
#include "ZapretPaths.h"

#include <QDir>
#include <QString>

namespace zapret {

ZapretManager::ZapretManager(QObject *parent)
    : QObject(parent)
{
    m_backend = createBackend(this);
    if (m_backend)
    {
        // Forward every backend signal to our own so a single subscription point serves the GUI.
        connect(m_backend, &IZapretBackend::stateChanged, this, &ZapretManager::stateChanged);
        connect(m_backend, &IZapretBackend::logMessage, this, &ZapretManager::logMessage);
        connect(m_backend, &IZapretBackend::errorOccurred, this, &ZapretManager::errorOccurred);

        // Track "did something of ours go away?" so the UI can drop the active strategy without
        // inspecting state transitions itself: both Stopped and Error mean nothing is running.
        connect(m_backend, &IZapretBackend::stateChanged, this, [this](IZapretBackend::State st) {
            if (st != IZapretBackend::State::Stopped && st != IZapretBackend::State::Error)
                return;
            if (m_activeId.isEmpty())
                return;
            m_activeId.clear();
            m_hasActiveStrategy = false;
            emit strategyStopped();
        });
    }
}

ZapretManager::~ZapretManager() = default;

IZapretBackend *ZapretManager::createBackend(QObject *parent)
{
    // The GUI build targets exactly one OS per compile. Backends are pure Qt + a thin guarded WinAPI
    // shim, so both classes always compile; only the selection below is OS-specific.
#if defined(Q_OS_WIN)
    return new WindowsBackend(parent);
#elif defined(Q_OS_LINUX)
    return new LinuxBackend(parent);
#else
    // macOS / *BSD: not yet implemented. Returning nullptr lets the caller show a clear
    // "unsupported platform" state instead of a half-working backend.
    Q_UNUSED(parent);
    return nullptr;
#endif
}

bool ZapretManager::start(const Strategy &s, QString *error)
{
    if (!m_backend)
    {
        const QString msg = QStringLiteral("ZapretManager: no execution backend for this platform");
        // The backends log their own failures before emitting errorOccurred; the manager has to keep that
        // contract for the rejections it makes itself.
        emit logMessage(msg, 2);
        emit errorOccurred(msg);
        if (error)
            *error = msg;
        return false;
    }

    if (!m_backend->start(s, error))
        return false;

    m_activeId = s.id.isEmpty() ? s.filePath : s.id;
    m_activeStrategy = s;
    m_hasActiveStrategy = true;
    emit strategyStarted(m_activeId, s.name);
    return true;
}

void ZapretManager::stop()
{
    if (m_backend)
        m_backend->stop();
}

IZapretBackend::State ZapretManager::state() const
{
    return m_backend ? m_backend->state() : IZapretBackend::State::Stopped;
}

bool ZapretManager::isRunning() const
{
    return m_backend && m_backend->isRunning();
}

QString ZapretManager::backendName() const
{
    return m_backend ? m_backend->platformName() : QStringLiteral("None");
}

// ---- strategy catalog ----

void ZapretManager::setZapretDir(const QString &dir)
{
    m_zapretDir = dir.trimmed();
    reload();
}

QString ZapretManager::resolveRoot(const QString &dir, Strategy::Platform *platformOut) const
{
    auto result = [platformOut](const QString &root, Strategy::Platform platform) {
        if (platformOut)
            *platformOut = platform;
        return root;
    };

#if defined(Q_OS_WIN)
    const Strategy::Platform host = Strategy::Platform::Windows;
#elif defined(Q_OS_LINUX)
    const Strategy::Platform host = Strategy::Platform::Linux;
#else
    const Strategy::Platform host = Strategy::Platform::Unknown;
#endif

    auto probe = [](const QString &candidate) { return ZapretPaths::detectPlatform(candidate); };
    auto preferHost = [host](const QString &winRoot, Strategy::Platform win,
                             const QString &linRoot, Strategy::Platform lin) {
        const bool winOk = win != Strategy::Platform::Unknown && !winRoot.isEmpty();
        const bool linOk = lin != Strategy::Platform::Unknown && !linRoot.isEmpty();
        if (winOk && win == host)
            return winRoot;
        if (linOk && lin == host)
            return linRoot;
        if (winOk)
            return winRoot;
        if (linOk)
            return linRoot;
        return QString();
    };

    if (!dir.isEmpty()) {
        // Given the root itself (…/windows or …/linux).
        const Strategy::Platform direct = probe(dir);
        if (direct != Strategy::Platform::Unknown)
            return result(dir, direct);

        // Given the base (…/zapret): probe exactly its two known children. Scoped on purpose —
        // ZapretLocator::locate() also walks the well-known bases, and a path the user typed must
        // never silently resolve to a different tree.
        const QString winRoot = QDir(dir).filePath(QStringLiteral("windows"));
        const QString linRoot = QDir(dir).filePath(QStringLiteral("linux"));
        const QString picked = preferHost(winRoot, probe(winRoot), linRoot, probe(linRoot));
        if (!picked.isEmpty())
            return result(picked, probe(picked));

        // Configured, but not a zapret tree: hand it to the parser, which yields nothing.
        return result(dir, Strategy::Platform::Unknown);
    }

    // Nothing configured: search the well-known places.
    const QList<ZapretLocator::Found> found = ZapretLocator::locate();
    if (!found.isEmpty()) {
        for (const ZapretLocator::Found &f : found)
            if (f.platform == host)
                return result(f.path, f.platform);
        return result(found.first().path, found.first().platform);
    }
    return result(QString(), Strategy::Platform::Unknown);
}

int ZapretManager::reload()
{
    Strategy::Platform platform = Strategy::Platform::Unknown;
    const QString root = resolveRoot(m_zapretDir, &platform);

    QList<Strategy> parsed;
    if (!root.isEmpty()) {
        if (platform == Strategy::Platform::Unknown)
            platform = StrategyParser::guessPlatform(root);
        parsed = StrategyParser::parseTree(root, platform);
        // Turn the windows wrappers into their underlying winws.exe invocations; on failure the
        // wrapper (cmd.exe /c) form is kept, which is still runnable.
        StrategyParser::resolveWindowsCommands(parsed);
    }

    m_resolvedRoot = root;
    m_strategies = parsed;
    emit strategiesLoaded(static_cast<int>(m_strategies.size()));
    return static_cast<int>(m_strategies.size());
}

const Strategy *ZapretManager::findStrategy(const QString &id) const
{
    if (id.isEmpty())
        return nullptr;
    for (const Strategy &s : m_strategies) {
        const QString sid = s.id.isEmpty() ? s.filePath : s.id;
        if (sid == id)
            return &s;
    }
    return nullptr;
}

bool ZapretManager::startById(const QString &id, QString *error)
{
    const Strategy *s = findStrategy(id);
    if (!s) {
        const QString msg = QStringLiteral("Стратегия не найдена: %1").arg(id);
        if (error)
            *error = msg;
        emit logMessage(msg, 2);   // same log-before-notify contract the backends follow
        emit errorOccurred(msg);
        return false;
    }
    return start(*s, error);
}

const Strategy *ZapretManager::activeStrategy() const
{
    return m_hasActiveStrategy ? &m_activeStrategy : nullptr;
}

} // namespace zapret
