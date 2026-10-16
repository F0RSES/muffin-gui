#pragma once

#include "IZapretBackend.h"

class QProcess;

namespace zapret {

// Windows execution backend.
//
// It owns a single QProcess running winws.exe (or, for an unresolved wrapper, cmd.exe /c "<bat>").
// winws.exe is the payload binary; because StrategyParser::resolveWindowsCommands() has already
// expanded the *.bat wrapper into an explicit argv, this class normally execs winws.exe directly
// with CREATE_NO_WINDOW rather than shelling out through cmd.exe.
//
// Privileges: loading the WinDivert driver requires an elevated token. QProcess inherits the caller
// token, so the GUI is expected to relaunch itself elevated (PrivilegeHelper::runElevated) at
// startup; the backend checks isElevated() and, when not elevated, reports a clear error instead of
// letting winws.exe die silently deep in the driver-load path.
//
// Threading: single-threaded (the GUI thread), no internal locking; one instance == one process.
class WindowsBackend : public IZapretBackend
{
    Q_OBJECT

public:
    explicit WindowsBackend(QObject *parent = nullptr);
    ~WindowsBackend() override;

    bool start(const Strategy &s, QString *error) override;
    void stop() override;

    State state() const override { return m_state; }
    bool isRunning() const override;

    QString platformName() const override { return QStringLiteral("Windows"); }

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, bool crashed);
    void onProcessError();
    void onReadyReadStdout();
    void onReadyReadStderr();

private:
    void setState(State s);
    void resetProcess();

    QProcess *m_process = nullptr;
    State m_state = State::Stopped;
    QString m_lastError;
};

} // namespace zapret
