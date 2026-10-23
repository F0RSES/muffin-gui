#pragma once

#include "HistoryStore.h"

#include <QAbstractTableModel>
#include <QColor>
#include <QVector>
#include <QWidget>

class QLabel;
class QPushButton;
class QStackedWidget;
class QTableView;

namespace zapret {

class HistoryTableModel;

// What one recorded check says about the bypass, as the table reads it.
//
// Three verdicts and not two, because the certificate field exists precisely to split the rows that
// passed the TCP half of a check from the rows that passed it while the TLS handshake was intercepted.
// A censor that lets the connection through and hands back somebody else's certificate answers "yes" to
// a plain reachability question; calling that row an OK would put the one event worth noticing in the
// same colour as the boring ones.
enum class HistoryVerdict
{
    Passed,       // connected, and the certificate was the service's own
    Intercepted,  // connected, but the certificate was rejected — traffic is being met in the middle
    Failed        // never connected at all
};

// The history and statistics page: what has been checked, what came back, and how often.
//
// The journal answers "what happened, line by line"; this page answers the question a journal cannot —
// "is the bypass getting worse?" — so it shows two things and only two: the aggregate (how many checks
// came back clean since the record began) and the records themselves, newest first, coloured by verdict.
// Every row in the file is shown; the store's own cap bounds the table, so no second limit belongs here.
//
// The page owns the store pointer it was handed and listens to changed() by itself: rows arrive one per
// host as a cycle reports them, and a page that needed its window to tell it when to re-read would be a
// page that shows a stale table whenever a call site forgets the call. Same reason it reads the store
// instead of keeping a copy of its own — one owner of the rows, and the owner is the store.
//
// Like SettingsPage this is the content, not the navigation: the window puts it in the page stack and
// adds the "back" button around it, so the page can be built, filled and checked without a window.
class HistoryPage : public QWidget
{
    Q_OBJECT

public:
    // The store is not owned. The window constructs one, loads it, and hands it here for the life of
    // both; a null store is not supported, because a page with nothing to read only looks broken.
    explicit HistoryPage(HistoryStore *history, QWidget *parent = nullptr);
    ~HistoryPage() override;

    // ---- the verdict policy, pure and static ----
    // The row's verdict from its two booleans. Failed outranks Intercepted on purpose: a row that never
    // connected has no certificate opinion to report (the engine records "no TLS attempt" as valid), and
    // a check that timed out is not evidence about interception.
    static HistoryVerdict verdictFor(const HistoryEntry &entry);
    // The word the status column shows. Derived text, so it is re-read on every language change.
    static QString verdictText(HistoryVerdict verdict);
    // The colour of both the tint and the word: one token drives the row and its label, so the two
    // cannot drift apart when the theme moves.
    static QColor verdictColor(HistoryVerdict verdict);
    // The row background: that colour made faint enough to read over but not to wash out.
    static QColor verdictTint(HistoryVerdict verdict);

    // The numbers behind the summary, in one pass over the rows. Static and pure so the arithmetic —
    // and the rounding of the headline percentage — is checkable without a table or a store.
    struct Tally
    {
        int total = 0;
        int passed = 0;
        int intercepted = 0;
        int failed = 0;
        // The clean share, rounded to whole percent, or -1 when there is nothing to divide. -1 rather
        // than 0 because "no checks yet" and "every check failed" are different sentences, and the page
        // has to be able to tell them apart.
        int successRate() const;
    };
    static Tally tallyOf(const QVector<HistoryEntry> &rows);

    // Re-read the rows and re-say every derived string: the headers, the verdict words, the summary.
    void retranslateUi();
    // The theme moved. The baked placeholder glyph and every row tint are colours this page paints with,
    // so both have to be re-taken from the new tokens.
    void retheme();

protected:
    // The question the clear button asks, kept apart from the act for the same reason the window's
    // watchdog seams are: a modal needs a desktop and somebody to answer it, and what is worth checking
    // is the decision on either side of the answer — that a yes empties the record and a no leaves every
    // row where it was.
    virtual bool confirmClear(int totalRows);

public:
    // ---- test hooks ----
    // The store this page reads. Exposed because the window's promise is that it hands the page the one
    // record it writes to, and nothing about the page says so from the outside.
    HistoryStore *store() const { return m_history; }
    int rowCount() const;
    QTableView *table() const;
    QPushButton *clearButton() const;
    QLabel *headline() const;
    // The three counts, one chip each: the total, the failures, the substituted certificates. A chip is
    // a separate label precisely so it can be coloured on its own, and the colour is a dynamic property
    // the sheet paints — readable here, because "did the page decide to shout" is a decision, not a pixel.
    QLabel *totalBadge() const;
    QLabel *failedBadge() const;
    QLabel *certBadge() const;
    // One of "neutral", "danger", "warning": the QSS property name, for the same reason rateLevel() is —
    // it is the contract between this page and both sheets, and a band the code stops stamping would
    // leave a chip grey with nothing to say so.
    QString badgeKind(const QLabel *badge) const;
    // False while the record is empty: three grey zeroes would summarise a record that does not exist.
    bool summaryBadgesVisible() const;
    // The rounded panel the table sits inside. The panel, not the table, is what owns the corners — and
    // what the corners are therefore tested against.
    QWidget *tableCard() const;
    // True while the "nothing recorded yet" notice is what the user is shown instead of the table.
    bool showsEmptyNotice() const;
    // The headline percentage the page computed, or -1 when the history is empty.
    int successRate() const;
    // The band the indicator is currently painted in: "none", "poor", "fair" or "good". It is the QSS
    // property name, because that is the contract between the page and both sheets — a band the code
    // stops stamping leaves the bar grey with no test to say so.
    QString rateLevel() const;

public slots:
    // Re-read the store. Safe to call at any time; it is what changed() is connected to.
    void refresh();
    // Forgets every row in the store. The act only — the button goes through a confirmation first, and a
    // test that wants the act must not be blocked by a modal it cannot answer.
    void clearHistory();

signals:
    // How many rows the clear took away, said after the fact so a window can put the number in the
    // journal: forgetting the record is worth one line in the record of everything else.
    void cleared(int removedRows);

private slots:
    void askClear();

private:
    void build();
    void updateSummary(const Tally &tally);
    void setRateLevel(int rate);

    HistoryStore *m_history = nullptr;

    QPushButton *m_clearButton = nullptr;
    QLabel *m_headline = nullptr;
    QLabel *m_totalBadge = nullptr;
    QLabel *m_failedBadge = nullptr;
    QLabel *m_certBadge = nullptr;
    QWidget *m_rateBar = nullptr;       // a QProgressBar, styled: the widget is enough to poke
    QStackedWidget *m_body = nullptr;   // table panel <-> empty notice
    QWidget *m_tableCard = nullptr;     // the rounded panel; the table inside it paints nothing
    QTableView *m_table = nullptr;
    QLabel *m_emptyGlyph = nullptr;
    QLabel *m_emptyText = nullptr;

    int m_lastRate = -1;                // what the bar and the headline currently say
};

// The rows as the table shows them: five columns, newest first, each row wearing its verdict.
//
// A model rather than a QTableWidget because the whole content is derived — one row per stored entry,
// every cell a function of that entry — and because resetting a thousand rows is the cheap and honest
// answer to "the store changed". A table widget would have to be told twice: once about the items and
// once about the colours it is not allowed to derive itself.
class HistoryTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column
    {
        ColumnTime = 0,
        ColumnPreset,
        ColumnHost,
        ColumnVerdict,
        ColumnLatency,
        ColumnCount
    };

    explicit HistoryTableModel(QObject *parent = nullptr);

    // Replaces the shown rows. Given in the order they should be displayed (newest first) — the model
    // does not re-sort a snapshot handed to it, so the page decides once what "the order" is.
    void setRows(const QVector<HistoryEntry> &rows);
    const QVector<HistoryEntry> &rows() const { return m_rows; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    // A column click. The order survives setRows(), so a refresh arriving mid-cycle does not throw the
    // reader back to the default.
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    // The headers are derived from the catalog, so a language change needs them re-announced.
    void retranslate();

private:
    void applySort();

    QVector<HistoryEntry> m_rows;
    int m_sortColumn = ColumnTime;
    Qt::SortOrder m_sortOrder = Qt::DescendingOrder;   // newest first is the default reading order
};

} // namespace zapret
