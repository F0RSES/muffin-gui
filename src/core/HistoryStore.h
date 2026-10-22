#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

class QThread;
class QTimer;

namespace zapret {

// One finished host check, as the history remembers it.
//
// The six fields are the whole file format on purpose: a record of what was asked and what came back,
// nothing else. `latency` keeps BlockcheckEngine's own sentinel — the average milliseconds from the
// first SYN to "transport up", or -1 when no attempt connected — so a row read out of the file means
// exactly what the engine reported, and "we never measured a time" stays distinguishable from "we
// measured zero".
struct HistoryEntry
{
    QDateTime timestamp;   // when the check finished. Invalid means "stamp it now" at append().
    QString preset;        // display name of the strategy that was up ("nfqws (portable)")
    QString host;          // the service that was probed ("discord.com")
    bool success = false;  // the engine's majority verdict
    int latency = -1;      // ms to transport up; -1 when nothing connected
    // The TLS half of that verdict, and the reason an OK row is worth keeping: a censor that lets the
    // connection through and hands back a substituted certificate passes a purely TCP check, so without
    // this the history would record an interception as a success. False means the engine saw ssl errors
    // on at least one attempt. True is also what a check that never attempted TLS records — there was
    // nothing to falsify — which is why it, and not false, is the default: a plain-port pass must not
    // read back as an interception.
    bool certValid = true;

    QJsonObject toJson() const;
    // False when the object cannot be made into a row a view could show: no host, or a timestamp that
    // does not read back. Everything else degrades rather than failing — a missing "preset" is an empty
    // name, a missing or non-numeric "latency" is -1, a missing or non-boolean "cert" is valid — because
    // a record whose latency was never written is a record about a check that never timed one, not a
    // corrupt one, and a file written before the "cert" key existed says nothing about certificates
    // rather than accusing someone of substituting one.
    static bool fromJson(const QJsonObject &o, HistoryEntry *out);
};

// The bounded, JSON-on-disk record of past bypass checks.
//
// History is the one thing a censorship tool owes its user: "it worked yesterday" is the only evidence
// that today's FAIL is a new problem rather than a long-standing one, and without it every failed
// check is a fresh argument with the provider. This class keeps that record as a plain array in the
// per-user data directory — no database engine, no schema version, no migration — because the question
// it answers ("when did this host last pass, under which preset") is one a scan of a thousand small
// objects answers just as well, while QtSql would put megabytes of driver into a tray app that ships
// none.
//
// Three rules the design is built around:
//
//   * Bounded. At most maxEntries() rows (1000 by default) are kept and the oldest go first, so the
//     file can never grow into the thing that makes the application slow. append(), setMaxEntries() and
//     load() all enforce it: a hand-edited or stale oversized file is trimmed on the way in, not on
//     the way out.
//   * Cheap on the caller's thread. append() touches only memory. What it schedules is a *debounced*
//     write, because a blockcheck round appends one row per host and twenty hosts must mean one file
//     write, not twenty. See setSaveDelayMs().
//   * Off the GUI thread. The debounce timer hands a snapshot of the rows to a private worker thread
//     that serializes and replaces the file, landing it through QSaveFile — write a temporary, rename
//     over the target — so an interrupted write leaves the previous history intact instead of a
//     half-written file the next load cannot parse. Never more than one worker runs at a time, and a
//     change that lands mid-write is remembered and written after: the newest state is the one that
//     survives.
//
// Threading: the object lives on the calling thread (the GUI thread in the app) and every mutation
// goes through it; only the file work moves. `entries()` hands out a reference into live memory, so it
// is for the owning thread.
class HistoryStore : public QObject
{
    Q_OBJECT

public:
    // The retention default. 1000 rows is on the order of 130 KB of indented JSON: weeks of watchdog
    // checks at the widest cadence, small enough that parsing it at startup is nothing a user notices,
    // and large enough that no realistic session outlives it. Widening it is setMaxEntries(), not here.
    static constexpr int kDefaultMaxEntries = 1000;

    // How long a change waits before it is written. Long enough that a complete round — every host of a
    // preset, appended back to back as the probe worker delivers them — collapses into one write, short
    // enough that the file is on disk before the user has finished reading the verdict sheet. 0 means
    // "hand it to the worker at the next change" (still off-thread, just not coalesced); tests and
    // anything that wants the bytes down promptly use that.
    static constexpr int kDefaultSaveDelayMs = 1000;

    explicit HistoryStore(QObject *parent = nullptr);
    // Same store with its file already chosen, so the caller does not have to spell two calls.
    HistoryStore(const QString &filePath, QObject *parent = nullptr);
    // Joins any writer in flight and writes what has not been saved yet, so nothing the user recorded is
    // lost by quitting. No signals are emitted from here.
    ~HistoryStore() override;

    // ---- where the file lives ----
    // Empty means "nowhere": the store still works in memory and every save attempt says so.
    QString filePath() const;
    void setFilePath(const QString &path);

    // ---- how many rows are kept ----
    // Never below 1: "keep nothing" is not a retention policy, it is a broken store, and a caller that
    // passes 0 or a negative means "the smallest real history", which is one row.
    int maxEntries() const;
    void setMaxEntries(int n);

    // ---- how long a change waits before it is written ----
    int saveDelayMs() const;
    void setSaveDelayMs(int ms);

    // Reads filePath() and makes its rows the whole content of the store.
    //
    // A file that does not exist is a successful load of nothing — a first run is not an error.
    // Anything else that goes wrong (unreadable, not JSON, not an array at the top level) fails, reports
    // why through *error/lastError() and leaves the store empty; the file itself is left exactly as it
    // was, so the user can still look at it. Records inside the array that do not read back are skipped
    // and counted into lastError() while the good ones load: one hand-corrupted row must not cost the
    // other 999.
    //
    // Synchronous by design: it runs once, at startup, on a bounded file.
    bool load(QString *error = nullptr);

    // The rows, oldest first, in the order the file holds them. A reference into live memory: read it,
    // do not keep it across an append().
    const QVector<HistoryEntry> &entries() const;
    int count() const;
    // The newest `n` rows, newest first — the order a history list shows them in. Asking for more than
    // count() simply means "all of them".
    QVector<HistoryEntry> recent(int n) const;

    // Records one finished check: appended at the end (the newest) and trimmed to the cap, stamped now
    // when the entry carries no timestamp. True when the row was taken; false when it cannot be — a
    // blank host is a row no view could name and no later check could be compared against, so it is
    // refused rather than stored.
    bool append(const HistoryEntry &entry);
    // The shape the caller actually holds after one host verdict, without the struct. `certValid` is the
    // TLS half of that verdict — BlockcheckEngine's HostCheckResult::isCertificateValid — and defaults to
    // true, so a caller with no TLS opinion still records an honest row instead of a false accusation.
    bool appendResult(const QString &preset, const QString &host, bool success, int latencyMs,
                      bool certValid = true, const QDateTime &at = QDateTime());

    // Forgets every row and schedules the empty file. In-memory only until the write lands.
    void clear();

    // ---- the write ----
    // True while a worker thread owns a write.
    bool isWriting() const;
    // True when memory holds rows the file does not: waiting for the debounce, in flight, or left
    // unsaved by a write that failed. A rejected write does not clear this, so a destination that
    // cannot be taken (a full disk, a read-only home) keeps its rows pending for the next change, the
    // next flush() and the destructor rather than reporting them as saved.
    bool isDirty() const;
    // Forces what is waiting to be written now, cancelling the debounce wait, and waits for the result.
    // Used at shutdown and by tests: after a true return the file holds exactly the current rows. A
    // write already in flight is joined first (up to timeoutMs), then anything still dirty is written on
    // this thread — blocking here is the point, because the caller asked for bytes on disk.
    bool flush(int timeoutMs = 5000);
    // Why the last load or write failed ("" when neither did). Diagnostics only, never a toast: a lost
    // history file is worth a log line, and saved() is the signal a view listens to.
    QString lastError() const;

    // The file's whole content: an indented JSON array of toJson() objects, in the order given. Exposed
    // because the format is a promise to the user (the file is theirs to read and to edit) and because a
    // test can then assert the bytes rather than only the round-trip.
    static QByteArray toJson(const QVector<HistoryEntry> &rows);

signals:
    // The rows changed — appended, trimmed, cleared or loaded. Views re-read entries()/recent().
    void changed();
    // The outcome of the last attempted write, emitted on the thread that owns the store. ok is true
    // only when the file now matches the rows it was handed.
    void saved(bool ok, const QString &error);

private:
    // What one worker run produced. Shared so the thread can report into it without ever touching the
    // QObject: the writer's lambda captures a copy of this pointer and of the row snapshot, never the
    // store itself, so the only thing crossing threads is plain data that the join orders.
    struct WriteSlot
    {
        bool ok = false;
        QString error;
    };

    // Starts the debounce clock for a pending write, or writes now when the delay is 0.
    void scheduleSave();
    // Hands the current rows to a worker. A worker already in flight owns the request: whatever is
    // dirty when it lands is written then, so the newest state is the one that ends up on disk.
    void startWrite();
    void launchWriter();
    void onWriteFinished();
    // Joins the worker in flight, if any, and reaps it.
    void reapWriter(int timeoutMs);
    // Serializes and replaces the file. Static and free of any member, so it is safe to run on a worker
    // thread and is the same code path the synchronous flush() takes. An empty error means success.
    static WriteSlot writeSnapshot(const QVector<HistoryEntry> &rows, const QString &path);
    // Drops the oldest rows down to the cap. True when anything went.
    bool trimToCap();

    QString m_filePath;
    int m_maxEntries = kDefaultMaxEntries;
    int m_saveDelayMs = kDefaultSaveDelayMs;
    QVector<HistoryEntry> m_entries;

    QTimer *m_debounce = nullptr;
    QThread *m_writer = nullptr;                      // owned; one at a time, joined before teardown
    std::shared_ptr<WriteSlot> m_slot;                // the in-flight write's result, or null
    bool m_dirty = false;                             // rows the file does not hold yet
    QString m_lastError;
};

} // namespace zapret
