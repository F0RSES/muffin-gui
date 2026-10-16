#pragma once

#include "IZapretBackend.h"
#include "Strategy.h"

#include <QObject>
#include <QString>

class QAbstractEventDispatcher;

namespace zapret {

// Platform-agnostic front door the GUI talks to.
//
// ZapretManager owns exactly one IZapretBackend — chosen at construction from the host OS — and
// forwards its state/log/error signals unchanged, so a view never needs to know which backend is in
// play. All the "what is on disk and how do we launch it" resolution lives in StrategyParser; all the
// OS-specific process/elevation detail lives in the backend. This class is deliberately thin: it is
// the seam that keeps the UI free of #ifdefs and lets tests inject a backend of their choosing.
//
// Ownership: the backend is a QObject child of the manager and dies with it. A backend belongs to the
// manager's thread's event loop; everything here is asynchronous (no blocking calls).
class ZapretManager : public QObject
{
    Q_OBJECT

public:
    explicit ZapretManager(QObject *parent = nullptr);
    ~ZapretManager() override;

    // Build a backend for the current host platform. Returns an uninitialized/unsupported stub
    // (nullptr) on platforms with no backend. Exposed so callers/tests can construct a specific
    // backend directly instead of relying on the compile-time choice.
    static IZapretBackend *createBackend(QObject *parent = nullptr);

    // Bring the resolved strategy up. Delegates to the active backend; returns false (and fills
    // *error) immediately when there is no backend or the backend rejects the launch synchronously.
    bool start(const Strategy &s, QString *error = nullptr);

    // Begin shutting the managed process down.
    void stop();

    IZapretBackend::State state() const;
    bool isRunning() const;

    // The live backend (may be nullptr on unsupported platforms). The manager keeps ownership.
    IZapretBackend *backend() const { return m_backend; }

    QString backendName() const;                 // "Windows" / "Linux" / "None"
    bool hasBackend() const { return m_backend != nullptr; }

signals:
    void stateChanged(IZapretBackend::State s);
    void logMessage(const QString &line, int level);
    void errorOccurred(const QString &message);

private:
    IZapretBackend *m_backend = nullptr;
};

} // namespace zapret
