#include "HistoryStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QThread>
#include <QTimer>

namespace zapret {

namespace {

// The six keys of one record, spelled once. A name that appears twice in the source is a name that
// drifts on the first rename, and this file is read by users as well as by load().
const QString kKeyTimestamp = QStringLiteral("timestamp");
const QString kKeyPreset    = QStringLiteral("preset");
const QString kKeyHost      = QStringLiteral("host");
const QString kKeySuccess   = QStringLiteral("success");
const QString kKeyLatency   = QStringLiteral("latency");
const QString kKeyCert      = QStringLiteral("cert");

} // namespace

// ---- one record ----

QJsonObject HistoryEntry::toJson() const
{
    QJsonObject o;
    // ISO-8601 to whole seconds ("2026-10-22T12:00:00"), local time: the record is a note about
    // something the user watched happen, so it has to read like their clock, and a row carrying a
    // milliseconds-or-offset tail would be the one thing a hand-edit could silently break.
    o.insert(kKeyTimestamp, (timestamp.isValid() ? timestamp : QDateTime::currentDateTime())
                                .toString(Qt::ISODate));
    o.insert(kKeyPreset, preset);
    o.insert(kKeyHost, host);
    o.insert(kKeySuccess, success);
    o.insert(kKeyLatency, latency);
    // Always written, never omitted: "the certificate was fine" is a thing the record asserts, and a key
    // that only appears on the interesting rows is a key a reader has to guess about.
    o.insert(kKeyCert, certValid);
    return o;
}

bool HistoryEntry::fromJson(const QJsonObject &o, HistoryEntry *out)
{
    if (!out)
        return false;

    // A row without a host is not a record of anything: nothing can display it, and nothing can
    // compare a later check against it. Rejecting it is also what turns a JSON value that was never an
    // object (toObject() hands back an empty one) into a skipped record rather than a bogus row.
    const QString host = o.value(kKeyHost).toString().trimmed();
    if (host.isEmpty())
        return false;

    const QDateTime at = QDateTime::fromString(o.value(kKeyTimestamp).toString(), Qt::ISODate);
    if (!at.isValid())
        return false;

    out->timestamp = at;
    out->preset = o.value(kKeyPreset).toString();
    out->host = host;
    out->success = o.value(kKeySuccess).toBool();
    // Absent or non-numeric is -1, the same "no time was measured" the engine reports — not 0, which
    // would claim a connection that answered instantly.
    out->latency = o.value(kKeyLatency).toInt(-1);
    // Absent or not-a-bool is *valid*, so the default carried to toBool() is true rather than the usual
    // false: every history file written before this key existed has no opinion on certificates, and
    // reading that silence as "the certificate was substituted" would put a false accusation of
    // interception into a user's own record. Only an explicit false says the engine saw ssl errors.
    out->certValid = o.value(kKeyCert).toBool(true);
    return true;
}

QByteArray HistoryStore::toJson(const QVector<HistoryEntry> &rows)
{
    QJsonArray arr;
    for (const HistoryEntry &e : rows)
        arr.append(e.toJson());
    // Indented rather than compact: the file is the user's own record and the one thing they can open
    // to see what the tool concluded, and the cap bounds the size either way.
    return QJsonDocument(arr).toJson(QJsonDocument::Indented);
}

// ---- construction ----

HistoryStore::HistoryStore(QObject *parent)
    : QObject(parent)
{
    // Single-shot and restarted by every change, which is the coalescing: twenty appends from one
    // blockcheck round are twenty memory writes and one file write. Parented here, so it lives and
    // dies on this object's thread — the timer only ever fires on the thread that owns the store.
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(m_saveDelayMs);
    connect(m_debounce, &QTimer::timeout, this, &HistoryStore::startWrite);
}

HistoryStore::HistoryStore(const QString &filePath, QObject *parent)
    : HistoryStore(parent)
{
    m_filePath = filePath;
}

HistoryStore::~HistoryStore()
{
    // No signals from a destructor: a dying object must not re-enter a slot that is halfway gone, and
    // disconnecting before teardown is the pattern MainWindow already follows with the engine. What
    // matters here is the bytes, so the in-flight write is joined and anything unsaved goes out
    // synchronously — a quit must not cost the user the checks of their last second.
    m_debounce->stop();
    reapWriter(3000);

    if (m_dirty && !m_filePath.isEmpty()) {
        m_dirty = false;
        m_lastError = writeSnapshot(m_entries, m_filePath).error;
    }
}

// ---- settings ----

QString HistoryStore::filePath() const { return m_filePath; }

void HistoryStore::setFilePath(const QString &path)
{
    if (path == m_filePath)
        return;
    m_filePath = path;
    // A write already in flight lands on the old path: it holds the path it was handed, which is the
    // honest behaviour — it is that snapshot's destination — and the next save goes to the new one.
    if (m_dirty)
        scheduleSave();
}

int HistoryStore::maxEntries() const { return m_maxEntries; }

void HistoryStore::setMaxEntries(int n)
{
    const int cap = qMax(1, n);
    if (cap == m_maxEntries)
        return;
    m_maxEntries = cap;
    if (trimToCap())
    {
        m_dirty = true;
        emit changed();
        scheduleSave();
    }
}

int HistoryStore::saveDelayMs() const { return m_saveDelayMs; }

void HistoryStore::setSaveDelayMs(int ms)
{
    m_saveDelayMs = qMax(0, ms);
    m_debounce->setInterval(m_saveDelayMs);
}

// ---- reading and writing the rows ----

bool HistoryStore::load(QString *error)
{
    // A fresh read replaces both the rows and the pending-write bookkeeping: whatever was waiting to be
    // written described the store the caller has just thrown away. An in-flight write is left alone —
    // it holds its own snapshot and its own destination, and landing it after the load is the same
    // ordering a caller asked for by loading before it touched anything.
    m_debounce->stop();
    m_dirty = false;
    m_entries.clear();
    m_lastError.clear();

    if (m_filePath.isEmpty()) {
        m_lastError = QStringLiteral("history: no file chosen");
        if (error)
            *error = m_lastError;
        return false;
    }

    // First run is not a failure: no file means no history, which is what an empty store is.
    if (!QFile::exists(m_filePath))
        return true;

    QFile f(m_filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        m_lastError = QStringLiteral("history: cannot read %1 (%2)")
                          .arg(m_filePath, f.errorString());
        if (error)
            *error = m_lastError;
        return false;
    }
    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
    // The format is one array and nothing else, so the two other shapes a file can have — not JSON, or
    // JSON but an object — are reported rather than half-read. The file itself is never touched here: a
    // record the user can see but this build cannot parse is still theirs to look at.
    if (pe.error != QJsonParseError::NoError || doc.isNull()) {
        m_lastError = QStringLiteral("history: %1 is not JSON (%2 at offset %3)")
                          .arg(m_filePath, pe.errorString()).arg(pe.offset);
        if (error)
            *error = m_lastError;
        return false;
    }
    if (!doc.isArray()) {
        m_lastError = QStringLiteral("history: %1 holds no array of records").arg(m_filePath);
        if (error)
            *error = m_lastError;
        return false;
    }

    const QJsonArray arr = doc.array();
    int skipped = 0;
    for (const QJsonValue &v : arr) {
        HistoryEntry e;
        if (HistoryEntry::fromJson(v.toObject(), &e))
            m_entries.append(e);
        else
            ++skipped;
    }

    // The cap is enforced on the way in as well as on append: a file written by a build that kept more
    // (or hand-grown) must not turn into a history the next save silently grows past the limit. The
    // oldest rows go, which is the same policy the user set.
    trimToCap();

    // Partial damage is not failure — the good rows loaded — but it is worth saying, and lastError() is
    // where a log line comes from. *error stays reserved for the cases that return false.
    if (skipped > 0)
        m_lastError = QStringLiteral("history: %1 of %2 records skipped")
                          .arg(skipped).arg(arr.size());

    emit changed();
    return true;
}

const QVector<HistoryEntry> &HistoryStore::entries() const { return m_entries; }

int HistoryStore::count() const { return m_entries.size(); }

QVector<HistoryEntry> HistoryStore::recent(int n) const
{
    if (n <= 0)
        return {};
    const int take = qMin(n, m_entries.size());
    // Newest first: the file is stored in the order things happened, a history list is read top-down.
    QVector<HistoryEntry> out;
    out.reserve(take);
    for (int i = m_entries.size() - 1; i >= m_entries.size() - take; --i)
        out.append(m_entries.at(i));
    return out;
}

bool HistoryStore::append(const HistoryEntry &entry)
{
    const QString host = entry.host.trimmed();
    if (host.isEmpty())
        return false;   // the header's rule: a row that names no host names no check

    HistoryEntry row = entry;
    row.host = host;
    if (!row.timestamp.isValid())
        row.timestamp = QDateTime::currentDateTime();

    m_entries.append(row);
    trimToCap();
    m_dirty = true;
    emit changed();
    scheduleSave();
    return true;
}

bool HistoryStore::appendResult(const QString &preset, const QString &host, bool success,
                                int latencyMs, bool certValid, const QDateTime &at)
{
    HistoryEntry e;
    e.timestamp = at;      // invalid means "now", which append() resolves
    e.preset = preset;
    e.host = host;
    e.success = success;
    e.latency = latencyMs;
    e.certValid = certValid;
    return append(e);
}

void HistoryStore::clear()
{
    if (m_entries.isEmpty() && !m_dirty)
        return;   // memory and file both hold nothing; there is nothing to say
    m_entries.clear();
    m_dirty = true;
    emit changed();
    scheduleSave();
}

bool HistoryStore::trimToCap()
{
    const int over = m_entries.size() - m_maxEntries;
    if (over <= 0)
        return false;
    // One batched remove rather than `over` calls to removeFirst(): the rows are append-ordered, so the
    // ones going are the front of the array and everything else shifts once instead of once per drop.
    m_entries.remove(0, over);
    return true;
}

// ---- the write ----

void HistoryStore::scheduleSave()
{
    if (m_filePath.isEmpty()) {
        // Nothing to write to, and saying so through saved() on every change would be a toast factory:
        // the rows stay in memory, dirty, and lastError() carries the reason for whoever asks.
        m_lastError = QStringLiteral("history: no file chosen; %1 record(s) live only in memory")
                          .arg(m_entries.size());
        return;
    }
    if (m_saveDelayMs == 0) {
        startWrite();
        return;
    }
    // start() on a single-shot timer restarts the clock, which is the whole coalescing: a caller that
    // appends twenty rows pays for one write. The rows cannot be lost by the restart either, because
    // the snapshot is taken when the timer fires, not when it is armed.
    m_debounce->start(m_saveDelayMs);
}

void HistoryStore::startWrite()
{
    if (!m_dirty)
        return;
    if (m_filePath.isEmpty()) {
        scheduleSave();   // same message, same decision: do not spawn a write that cannot land
        return;
    }
    if (m_writer)
        return;   // one worker at a time; onWriteFinished() takes up what is still dirty when it lands
    launchWriter();
}

void HistoryStore::launchWriter()
{
    m_dirty = false;
    m_slot = std::make_shared<WriteSlot>();

    // The snapshot is a copy-on-write handle, not a copy: writeSnapshot() only reads it, so the worker
    // keeps the shared buffer while append() on this thread detaches into its own. That is why no mutex
    // guards m_entries — the worker never reaches it.
    const QVector<HistoryEntry> rows = m_entries;
    const QString path = m_filePath;

    // QThread::create rather than a moved worker object: the job is one serialize-and-replace over data
    // handed to it by value, which is the same shape BlockcheckEngine's probe worker has. Nothing in the
    // lambda is a QObject, so there is no thread affinity to move and nothing for it to reach back into.
    m_writer = QThread::create([slot = m_slot, rows, path] {
        *slot = writeSnapshot(rows, path);
    });
    // finished() is emitted from the worker once its run() returns, so the slot below is queued onto this
    // thread and runs strictly after the write is over: reading *slot there is ordered by the join the
    // thread's own termination performs.
    connect(m_writer, &QThread::finished, this, &HistoryStore::onWriteFinished);
    m_writer->start();
}

void HistoryStore::onWriteFinished()
{
    if (!m_writer)
        return;   // flush() or the destructor already reaped this thread and reported its result

    // Read what the worker decided, then reap it the way BlockcheckEngine reaps its probe worker —
    // wait(), then delete. The thread has already returned from run(), so the wait is immediate, and
    // deleting here rather than through deleteLater() means the QThread cannot outlive the store as a
    // deferred delete that the exit never processes.
    bool ok = false;
    QString error = QStringLiteral("history: the write reported nothing");
    if (m_slot) {
        ok = m_slot->ok;
        error = m_slot->error;
    }
    reapWriter(15000);
    m_lastError = error;

    if (!ok) {
        // A write that failed is not a write that never happened: those rows are still only in memory,
        // and isDirty() has to keep saying so — that is what makes flush() and the destructor try again
        // rather than quitting over a file that is a hundred rows behind. Deliberately NOT retried here:
        // a destination that cannot be written at all (a full disk, a read-only home) would otherwise
        // be retried as fast as threads can start. The next recorded check takes it up.
        m_dirty = true;
    } else if (m_dirty) {
        // Rows changed while the file was being replaced: the newest state has to go out, and waiting
        // for a change that may never come would leave the file describing an older one.
        startWrite();
    }

    emit saved(ok, m_lastError);
}

void HistoryStore::reapWriter(int timeoutMs)
{
    if (!m_writer)
        return;

    if (!m_writer->wait(timeoutMs)) {
        // Bounded by one file write in practice; reaching here means the OS is holding the thread — a
        // wedged network home directory is the known shape — and terminate() is the same last resort the
        // probe worker documents. The temporary QSaveFile was writing to dies with the thread, and the
        // target was never touched, so the history on disk is the last complete one.
        m_writer->terminate();
        m_writer->wait(1000);
        m_lastError = QStringLiteral("history: the write did not finish within %1 ms").arg(timeoutMs);
    } else if (m_slot) {
        m_lastError = m_slot->error;
    }

    delete m_writer;
    m_writer = nullptr;
    m_slot.reset();
}

bool HistoryStore::flush(int timeoutMs)
{
    m_debounce->stop();
    if (m_writer)
        reapWriter(timeoutMs);

    if (!m_dirty) {
        // Either nothing was waiting, or the write just reaped settled it. The reaped result is the
        // answer: a complete file and an empty lastError() are the same statement.
        return m_lastError.isEmpty();
    }
    m_dirty = false;
    const WriteSlot r = writeSnapshot(m_entries, m_filePath);
    m_lastError = r.error;
    emit saved(r.ok, r.error);
    return r.ok;
}

bool HistoryStore::isWriting() const { return m_writer != nullptr; }

bool HistoryStore::isDirty() const { return m_dirty || m_writer != nullptr; }

QString HistoryStore::lastError() const { return m_lastError; }

// The only function that touches the disk, and the only one that runs on another thread. No member is
// read or written, so what the worker shares with the store is the WriteSlot it was handed.
HistoryStore::WriteSlot HistoryStore::writeSnapshot(const QVector<HistoryEntry> &rows,
                                                    const QString &path)
{
    WriteSlot r;
    if (path.isEmpty()) {
        r.error = QStringLiteral("history: no file chosen");
        return r;
    }

    // The application's own data directory exists by the time anything is recorded, but the path is a
    // caller's argument (a test's temporary tree, a future Settings key) and the rename below needs the
    // parent in place, so it is made rather than assumed. mkpath is a no-op on a live directory.
    const QFileInfo info(path);
    QDir parent = info.dir();
    if (!parent.exists() && !parent.mkpath(QStringLiteral("."))) {
        r.error = QStringLiteral("history: cannot create the directory for %1").arg(path);
        return r;
    }
    if (info.isDir()) {
        r.error = QStringLiteral("history: %1 is a directory").arg(path);
        return r;
    }

    // QSaveFile is the reason an interrupted write is survivable: the bytes go into a temporary and
    // commit() renames it over the target, so any reader — the next load(), or a crash that left a half
    // save behind — sees either the whole old file or the whole new one, never a truncated array. The
    // direct write fallback is for filesystems that refuse to rename a still-open file (SMB and other
    // network home directories are the common case): there it writes in place, giving up the atomicity
    // rather than losing the history outright.
    QSaveFile file(path);
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly)) {
        r.error = QStringLiteral("history: cannot open %1 (%2)").arg(path, file.errorString());
        return r;
    }
    const QByteArray json = toJson(rows);
    if (file.write(json) != json.size()) {
        r.error = QStringLiteral("history: short write to %1 (%2)").arg(path, file.errorString());
        file.cancelWriting();
        return r;
    }
    if (!file.commit()) {
        r.error = QStringLiteral("history: cannot replace %1 (%2)").arg(path, file.errorString());
        return r;
    }

    r.ok = true;
    return r;
}

} // namespace zapret


