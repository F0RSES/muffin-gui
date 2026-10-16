#include "ZapretManager.h"

#include "WindowsBackend.h"
#include "LinuxBackend.h"

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
        emit errorOccurred(msg);
        if (error)
            *error = msg;
        return false;
    }
    return m_backend->start(s, error);
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

} // namespace zapret
