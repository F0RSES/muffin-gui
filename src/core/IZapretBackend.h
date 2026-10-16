#pragma once

#include "Strategy.h"

#include <QObject>
#include <QString>
#include <QStringList>

namespace zapret {

// Minimal contract every platform execution backend must satisfy.
//
// A backend owns exactly one live zapret process (or one delegated launcher) at a time. It is fed
// a fully-resolved Strategy (executablePath / argv / workingDirectory populated by StrategyParser)
// and drives it through its lifecycle, reporting progress asynchronously via signals so the GUI can
// stay responsive. Nothing here parses files or locates zapret trees - that is the parser's job.
//
// Threading: backends are used from the GUI thread and are NOT internally locked; a single instance
// belongs to a single thread's event loop.
class IZapretBackend : public QObject
{
    Q_OBJECT

public:
    // Coarse lifecycle state surfaced to the GUI.
    enum class State
    {
        Stopped,   // no managed process
        Starting,  // process requested, not yet confirmed up
        Running,   // process is up
        Stopping,  // termination in progress
        Error      // a start/stop failed; see errorOccurred for detail
    };
    Q_ENUM(State)

    explicit IZapretBackend(QObject *parent = nullptr) : QObject(parent) {}
    ~IZapretBackend() override = default;

    // Bring the strategy up. Returns true when the launch request was accepted (state then moves
    // to Running asynchronously). On an immediate, synchronous failure fills *error and returns
    // false. Never blocks waiting for the whole process to exit.
    virtual bool start(const Strategy &s, QString *error) = 0;

    // Begin shutting the managed process down; completion is signalled via stateChanged.
    virtual void stop() = 0;

    virtual State state() const = 0;
    virtual bool isRunning() const = 0;

    // Human name of the backend ("Windows", "Linux"), mainly for diagnostics/logging.
    virtual QString platformName() const = 0;

signals:
    void stateChanged(State s);
    // Informational output: process stdout/stderr, progress, and the text of anything that went wrong.
    // level: 0=info, 1=warn, 2=error
    void logMessage(const QString &line, int level);
    // The notification channel for a failure. A view should surface it (badge, dialog, balloon) and NOT
    // log it: every emitter here writes the same text through logMessage first, so a listener wired to
    // both signals would otherwise show the failure twice. Consumers that only watch the log still see it.
    void errorOccurred(const QString &message);

protected:
    // ---- Shared launch composition -------------------------------
    //
    // A Strategy carries the program to exec in executablePath (falling back to filePath) and the
    // argument material in argv / primaryArg. Every backend derives the same base command from a
    // strategy and then layers on its platform-specific conventions (the Windows wrapper adds
    // "cmd.exe /c" or "powershell -File"; the Linux service-controller form appends the action
    // word "start"/"stop"). These helpers centralise the parts both backends agree on.

    // The program to execute: an explicit executablePath wins, else the payload file itself.
    static QString launchProgram(const Strategy &s)
    {
        return s.executablePath.isEmpty() ? s.filePath : s.executablePath;
    }

    // The base argument vector WITHOUT any trailing action word. The fully-resolved argv (a Windows
    // winws.exe invocation, or GUI-supplied nfqws flags) takes precedence; otherwise a single
    // primaryArg (the wrapped script / unit path). Empty when neither is present.
    static QStringList baseArgs(const Strategy &s)
    {
        if (!s.argv.isEmpty())
            return s.argv;
        if (!s.primaryArg.isEmpty())
            return QStringList{s.primaryArg};
        return {};
    }
};

} // namespace zapret
