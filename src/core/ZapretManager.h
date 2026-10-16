#pragma once

#include "IZapretBackend.h"
#include "Strategy.h"

#include <QList>
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

    // ---- strategy catalog (UI-facing, additive) -------------------------------------
    //
    // The GUI needs "what can I run here?" without importing the parser, the locator and the
    // Windows-command resolver separately. setZapretDir() remembers the root and reloads; reload()
    // runs the same pipeline the core tests use (parseTree -> applyDefaults -> dedup ->
    // resolveWindowsCommands) and caches the result. An empty directory means "auto-locate", and a
    // directory that is a *base* (contains windows/ and linux/) is expanded to the root matching this
    // host, so one setting works for both layouts.

    void setZapretDir(const QString &dir);
    QString zapretDir() const { return m_zapretDir; }
    QString resolvedRoot() const { return m_resolvedRoot; }

    // Re-scan the configured directory. Returns the number of strategies now available.
    int reload();

    QList<Strategy> strategies() const { return m_strategies; }

    // The directory reload() actually scanned (a windows/ or linux/ root), empty when nothing was
    // found. Differs from zapretDir() when the setting holds a base folder or is unset.
    int strategyCount() const { return static_cast<int>(m_strategies.size()); }
    const Strategy *findStrategy(const QString &id) const;

    // Start the catalog entry with this id (see start()).
    bool startById(const QString &id, QString *error = nullptr);

    // The entry the last accepted start() referred to, nullptr when nothing is active. The manager keeps
    // its own copy, so this also answers for a strategy handed straight to start() instead of resolved
    // from the catalog (that is what the tests and any embedder do).
    const Strategy *activeStrategy() const;
    QString activeStrategyId() const { return m_activeId; }

signals:
    void stateChanged(IZapretBackend::State s);
    void strategiesLoaded(int count);
    void strategyStarted(const QString &id, const QString &name);
    void strategyStopped();
    void logMessage(const QString &line, int level);
    void errorOccurred(const QString &message);

private:
    // Resolve a user directory into (root, platform). Falls back to auto-location.
    QString resolveRoot(const QString &dir, Strategy::Platform *platformOut) const;

    IZapretBackend *m_backend = nullptr;
    QString m_zapretDir;
    QString m_resolvedRoot;
    QString m_activeId;
    // A copy of what is running, not a lookup into the catalog: a reload() (or a start() of a strategy
    // that never came from the catalog) must not make the running process unanswerable.
    Strategy m_activeStrategy;
    bool m_hasActiveStrategy = false;
    QList<Strategy> m_strategies;
};

} // namespace zapret
