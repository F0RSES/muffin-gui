#include "HistoryPage.h"

#include "LanguageManager.h"
#include "Theme.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace zapret {

namespace {

// A row's background is its verdict at low alpha over the page, and its status word is the same colour at
// full strength: one hue, two intensities, so a scan of the column reads as colour before it reads as
// text. The alpha differs by theme because the dark canvas swallows a faint tint that the light one would
// shout — the same reason the sheets carry two values for every state colour.
constexpr int kTintAlphaDark = 36;
constexpr int kTintAlphaLight = 26;

// The headline's band breaks. "Nothing to report" is not a number and is not coloured at all, so the bar
// only moves once there is something to divide; above that, a bypass that passes 90+% of its checks is
// green, one that passes 60% is not working well, and below that it is not working.
constexpr int kRateGoodMin = 90;
constexpr int kRateFairMin = 60;

// The QSS dynamic property the rate bar's colour keys off. Same mechanism as the rail's [alert] and a
// card's [selected]: the sheet owns the hue, the code owns the decision, and every write has to
// unpolish/repolish or the style never re-reads it.
const char kRateLevelProperty[] = "rateLevel";

// How far the table sits inside its panel. The panel rounds its corners with a 12 px arc (both sheets'
// #historyTableCard), and a rounded rectangle only clears its own corner 12 - 12/sqrt(2) ≈ 3.5 px in —
// so 4 px of panel between the border and the table is the smallest inset that guarantees no square
// corner of the header band or a row tint ever lands on the arc. Rounding them instead is not available:
// QSS has no positional state for header sections, a radius on the header widget loses to the sections
// painting over it, and a mask clips in stair-steps. See the sheet's comment for the same arithmetic.
constexpr int kTableInsetPx = 4;

// The three counts' colour band, stamped the same way and for the same reason: "neutral" is a number that
// is not a problem, "danger" the checks that never connected, "warning" the certificates that came back
// somebody else's. The two hues are the sheet's own state colours — the same red and amber the rows and
// the rate bar already paint with — so "3" on a red chip and three red rows are one claim, not two.
const char kBadgeKindProperty[] = "badgeKind";
const char kBadgeNeutral[] = "neutral";
const char kBadgeDanger[] = "danger";
const char kBadgeWarning[] = "warning";

// The digits' font, resolved once. The theme's UI family is proportional by design — it is a text font —
// and the two columns a reader scans downward are numbers, so they take the system's fixed face instead.
// Asking QFontDatabase rather than naming a family is what keeps this working on a machine that has none
// of the mono fonts the sheet's log rule lists.
const QFont &monoFont()
{
    static const QFont font = [] {
        QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        f.setPointSize(9);
        return f;
    }();
    return font;
}

// QSS never re-reads a dynamic property on its own, so a chip whose count crossed zero has to be taken
// apart and put back together — the same three lines ProfileCard's ping chips and this page's rate bar
// pay. Skipping the dance when the band has not moved keeps a refresh (one per host, per cycle) from
// re-polishing three labels for nothing.
void applyBadgeKind(QLabel *badge, const char *kind)
{
    if (!badge)
        return;
    const QString token = QString::fromLatin1(kind);
    if (badge->property(kBadgeKindProperty).toString() == token)
        return;
    badge->setProperty(kBadgeKindProperty, token);
    badge->style()->unpolish(badge);
    badge->style()->polish(badge);
    badge->update();
}

// The stored format is ISO to the second (HistoryStore::toJson); the displayed one drops the T and the
// seconds. A history row is read as "when", not parsed as a timestamp, and 17:33:04 claims a precision
// the record does not have — the file itself cannot hold better than whole seconds anyway.
QString displayTime(const QDateTime &at)
{
    return at.toString(QStringLiteral("dd.MM.yyyy  HH:mm"));
}

// The order the status column sorts by. Ranked rather than alphabetical, because sorting by status is a
// way of putting the bad rows next to each other, and "Failed, Intercepted, Passed" is the order in which
// a reader wants to see them.
int verdictRank(HistoryVerdict verdict)
{
    switch (verdict)
    {
    case HistoryVerdict::Failed:      return 0;
    case HistoryVerdict::Intercepted: return 1;
    case HistoryVerdict::Passed:      return 2;
    }
    return 2;
}

// The latency the column sorts by. -1 is the engine's "nothing connected", not a fast answer, and
// leaving it as -1 would put the unmeasured rows at the top of an ascending sort — the one order a reader
// clicks for when they want to know what is slowest.
qint64 latencySortKey(const HistoryEntry &entry)
{
    return entry.latency < 0 ? std::numeric_limits<qint64>::max() : qint64(entry.latency);
}

// The row's tint, painted by hand rather than trusted to the style.
//
// The model carries it as Qt::BackgroundRole, which is where an item view is supposed to look — but a
// QSS rule for `::item` (this sheet has one, for the cell padding) moves item painting into the
// stylesheet style, and that paints what the rule declares and nothing else. The result is silent: the
// table keeps its words, its columns and its order, and loses exactly the thing that makes a failed
// night findable at a glance. So the fill happens here, before the base class draws the text.
//
// The selected state is left alone on purpose. A highlighted row is being looked at as a row, and the
// sheet's selection colour is the right answer for that; the verdict stays visible in its status word.
class VerdictDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (index.isValid() && !(option.state & QStyle::State_Selected)) {
            const QVariant tint = index.data(Qt::BackgroundRole);
            if (tint.canConvert<QBrush>())
                p->fillRect(option.rect, tint.value<QBrush>());
        }
        QStyledItemDelegate::paint(p, option, index);
    }

protected:
    // The same trap one level down: the status word's colour is a ForegroundRole, and the inherited
    // `QWidget { color: … }` rule is a perfectly plausible answer for the style to give it. Stamping
    // the role into the option's palette is what makes the word the verdict's colour in both themes.
    void initStyleOption(QStyleOptionViewItem *opt, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(opt, index);
        const QVariant foreground = index.data(Qt::ForegroundRole);
        if (foreground.canConvert<QBrush>())
            opt->palette.setBrush(QPalette::Text, foreground.value<QBrush>());
    }
};

} // namespace

// ---- the verdict policy ----

HistoryVerdict HistoryPage::verdictFor(const HistoryEntry &entry)
{
    if (!entry.success)
        return HistoryVerdict::Failed;
    return entry.certValid ? HistoryVerdict::Passed : HistoryVerdict::Intercepted;
}

QString HistoryPage::verdictText(HistoryVerdict verdict)
{
    switch (verdict)
    {
    case HistoryVerdict::Passed:
        return LanguageManager::tr(QStringLiteral("history.verdict.passed"));
    case HistoryVerdict::Intercepted:
        return LanguageManager::tr(QStringLiteral("history.verdict.intercepted"));
    case HistoryVerdict::Failed:
        return LanguageManager::tr(QStringLiteral("history.verdict.failed"));
    }
    return LanguageManager::tr(QStringLiteral("history.verdict.passed"));
}

QColor HistoryPage::verdictColor(HistoryVerdict verdict)
{
    switch (verdict)
    {
    case HistoryVerdict::Passed:      return Theme::success();
    case HistoryVerdict::Intercepted: return Theme::warning();
    case HistoryVerdict::Failed:      return Theme::error();
    }
    return Theme::success();
}

QColor HistoryPage::verdictTint(HistoryVerdict verdict)
{
    QColor tint = verdictColor(verdict);
    tint.setAlpha(Theme::isDark() ? kTintAlphaDark : kTintAlphaLight);
    return tint;
}

// ---- the aggregate ----

int HistoryPage::Tally::successRate() const
{
    if (total <= 0)
        return -1;
    // Rounded, not truncated: 2 clean checks out of 3 is 67% to a reader, and 66% reads as a claim the
    // numbers do not support.
    return int((qint64(passed) * 100 + total / 2) / total);
}

HistoryPage::Tally HistoryPage::tallyOf(const QVector<HistoryEntry> &rows)
{
    Tally t;
    t.total = rows.size();
    for (const HistoryEntry &e : rows)
    {
        switch (verdictFor(e))
        {
        case HistoryVerdict::Passed:      ++t.passed;       break;
        case HistoryVerdict::Intercepted: ++t.intercepted;  break;
        case HistoryVerdict::Failed:      ++t.failed;       break;
        }
    }
    return t;
}

// ---- the page ----

HistoryPage::HistoryPage(HistoryStore *history, QWidget *parent)
    : QWidget(parent)
    , m_history(history)
{
    build();

    // The record is the store's, and the store says when it moves. One connection covers every way a row
    // can arrive or leave: appendResult() from a live cycle, clear() from this page's own button, load()
    // from anything that re-points the file.
    if (m_history)
        connect(m_history, &HistoryStore::changed, this, &HistoryPage::refresh);

    refresh();
}

HistoryPage::~HistoryPage() = default;

void HistoryPage::build()
{
    setObjectName(QStringLiteral("historyPage"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto *column = new QVBoxLayout(this);
    // No horizontal margins of the window's: the page is hosted by a column that already carries the
    // 28 px gutter every other full-screen page sits inside, and a second one would push this page's
    // table out of alignment with the journal's and the profile rows'.
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(12);

    // ---- the head: the page's name, and the one destructive thing it can do ----
    auto *head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(10);

    auto *title = new QLabel(this);
    title->setObjectName(QStringLiteral("pageTitle"));
    i18nText(title, QStringLiteral("history.title"));

    m_clearButton = new QPushButton(this);
    m_clearButton->setObjectName(QStringLiteral("dangerButton"));
    // The glyph name goes in the property the window's theme pass walks rather than being baked once and
    // forgotten here: the icon is tinted from the theme's icon colour, and a button that keeps the old
    // tint after a switch belongs to a different application.
    m_clearButton->setProperty("glyph", QStringLiteral("clear"));
    m_clearButton->setProperty("glyphPx", 16);
    m_clearButton->setIcon(Theme::themedIcon(QStringLiteral("clear"), 16));
    m_clearButton->setCursor(Qt::PointingHandCursor);
    m_clearButton->setAutoDefault(false);
    i18nText(m_clearButton, QStringLiteral("history.clear"));
    i18nTip(m_clearButton, QStringLiteral("history.clear_tip"));
    connect(m_clearButton, &QPushButton::clicked, this, &HistoryPage::askClear);

    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(m_clearButton);
    column->addLayout(head);

    // ---- the summary ----
    // One raised panel with three statements: the share that came back clean in the biggest text on the
    // page, what that share was computed from, and a bar that says the same number again as a length —
    // a percentage needs a shape beside it to be read at a glance rather than read.
    auto *summary = new QFrame(this);
    summary->setObjectName(QStringLiteral("historySummary"));
    auto *summaryBox = new QVBoxLayout(summary);
    summaryBox->setContentsMargins(18, 14, 18, 14);
    summaryBox->setSpacing(10);

    auto *numbers = new QHBoxLayout;
    numbers->setContentsMargins(0, 0, 0, 0);
    numbers->setSpacing(8);

    m_headline = new QLabel(summary);
    m_headline->setObjectName(QStringLiteral("historyHeadline"));

    // The three counts are chips rather than one sentence. A sentence has one colour; a chip has its own,
    // so "Ошибок: 0" can stay quiet while "Ошибок: 7" wears the sheet's red — which is the whole point of
    // showing the numbers at all: a reader who is not looking for a number should still see the one that
    // is a problem. Their figures take the same fixed face the table's number columns use, pinned through
    // each chip's own inline sheet because the generic `QWidget` font rule in both sheets outranks
    // setFont() and would let the counts shift sideways as their digits change.
    auto chip = [summary](const QString &tipKey) {
        auto *badge = new QLabel(summary);
        badge->setObjectName(QStringLiteral("historyBadge"));
        badge->setProperty(kBadgeKindProperty, QLatin1String(kBadgeNeutral));
        badge->setAlignment(Qt::AlignCenter);
        badge->setTextFormat(Qt::PlainText);
        badge->setFont(monoFont());
        Theme::applyFontViaSheet(badge, monoFont());
        if (!tipKey.isEmpty())
            i18nTip(badge, tipKey);
        return badge;
    };

    m_totalBadge = chip(QString());
    m_failedBadge = chip(QString());
    // "Подмен: 2" is terse by necessity — the plate is small — so the phrase the column spells out in
    // full rides on it as a tooltip: what a substituted certificate says about the path in the middle.
    m_certBadge = chip(QStringLiteral("history.verdict.intercepted_tip"));

    numbers->addWidget(m_headline);
    numbers->addStretch(1);
    numbers->addWidget(m_totalBadge);
    numbers->addWidget(m_failedBadge);
    numbers->addWidget(m_certBadge);
    summaryBox->addLayout(numbers);

    auto *bar = new QProgressBar(summary);
    bar->setObjectName(QStringLiteral("historyRateBar"));
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setTextVisible(false);
    bar->setFixedHeight(8);
    summaryBox->addWidget(bar);
    m_rateBar = bar;

    column->addWidget(summary);

    // ---- the body: the table in its panel, or the reason there is no table ----
    m_body = new QStackedWidget(this);
    m_body->setObjectName(QStringLiteral("historyBody"));

    // The panel owns the rounded box and the table inside it paints nothing of its own. A radius on a
    // QTableView only rounds its FRAME — the header band and the rows are separate child widgets, and a
    // styled widget does not clip its children — so the header's square top corners used to sit on top of
    // the arc and stick out of the panel, which is the bug this shape exists to avoid. The corners are
    // kept off the arc by kTableInsetPx rather than by clipping; see the constant and the sheet's comment.
    m_tableCard = new QFrame(m_body);
    m_tableCard->setObjectName(QStringLiteral("historyTableCard"));
    m_tableCard->setAttribute(Qt::WA_StyledBackground, true);
    auto *cardBox = new QVBoxLayout(m_tableCard);
    cardBox->setContentsMargins(kTableInsetPx, kTableInsetPx, kTableInsetPx, kTableInsetPx);
    cardBox->setSpacing(0);

    m_table = new QTableView(m_tableCard);
    m_table->setObjectName(QStringLiteral("historyTable"));
    auto *model = new HistoryTableModel(this);
    m_table->setModel(model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Nothing here is editable: the rows are the record of measurements, and a cell that enters edit
    // mode on a double click invites the reader to believe the number can be changed.
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // The row tint is model data, and a styled item view does not honour it (see VerdictDelegate) —
    // the delegate is what keeps the colour coding on the screen rather than only in the model.
    m_table->setItemDelegate(new VerdictDelegate(m_table));
    m_table->setWordWrap(false);
    m_table->setShowGrid(false);
    // No zebra stripes. Every row already carries a colour that means something, and an alternating
    // grey beside it would be a second, meaningless rhythm down the same column.
    m_table->setAlternatingRowColors(false);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(30);
    QHeaderView *header = m_table->horizontalHeader();
    // The sheet styles the header by name for the same reason it styles the table: an unstyled
    // QHeaderView is the platform's own grey box inside a rounded dark panel.
    header->setObjectName(QStringLiteral("historyHeader"));
    header->setHighlightSections(false);
    // The two free-text columns take the space left over; the four whose content has a known shape size
    // themselves. A host column that could be widened by dragging is a host column that hides the third
    // one at the window's fixed 900 px.
    header->setSectionResizeMode(HistoryTableModel::ColumnTime, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(HistoryTableModel::ColumnPreset, QHeaderView::Stretch);
    header->setSectionResizeMode(HistoryTableModel::ColumnHost, QHeaderView::Stretch);
    header->setSectionResizeMode(HistoryTableModel::ColumnVerdict, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(HistoryTableModel::ColumnLatency, QHeaderView::ResizeToContents);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(HistoryTableModel::ColumnTime, Qt::DescendingOrder);
    cardBox->addWidget(m_table);
    m_body->addWidget(m_tableCard);

    // The empty state, which is the only state a first run has. A bare grid of column headings would read
    // as a page that lost its data; this names the two things that would put rows in it.
    auto *empty = new QWidget(m_body);
    empty->setObjectName(QStringLiteral("historyEmpty"));
    auto *emptyBox = new QVBoxLayout(empty);
    emptyBox->setContentsMargins(24, 40, 24, 40);
    emptyBox->setSpacing(14);
    emptyBox->addStretch(1);

    m_emptyGlyph = new QLabel(empty);
    m_emptyGlyph->setObjectName(QStringLiteral("historyEmptyGlyph"));
    m_emptyGlyph->setAlignment(Qt::AlignCenter);
    m_emptyGlyph->setPixmap(Theme::icon(QStringLiteral("clock"), Theme::mutedText(), 34)
                                .pixmap(34, 34));
    emptyBox->addWidget(m_emptyGlyph, 0, Qt::AlignHCenter);

    m_emptyText = new QLabel(empty);
    m_emptyText->setObjectName(QStringLiteral("placeholder"));
    m_emptyText->setAlignment(Qt::AlignCenter);
    m_emptyText->setWordWrap(true);
    i18nText(m_emptyText, QStringLiteral("history.empty"));
    emptyBox->addWidget(m_emptyText, 0, Qt::AlignHCenter);

    emptyBox->addStretch(1);
    m_body->addWidget(empty);

    column->addWidget(m_body, 1);
}

// ---- reading the record ----

void HistoryPage::refresh()
{
    if (!m_history)
        return;

    // One copy of the rows, newest first, is both the table's content and the summary's input — so the
    // headline and the list can never disagree about what the record says, which is the one way a
    // statistics page can lie.
    const QVector<HistoryEntry> rows = m_history->recent(m_history->count());
    static_cast<HistoryTableModel *>(m_table->model())->setRows(rows);

    const Tally tally = tallyOf(rows);
    updateSummary(tally);

    // Empty is a state of the record, not of the widget: the page has to look different when there is
    // nothing to read, because a headingless grid of five column names looks like a page that lost its
    // rows rather than like a tool that has not been used yet.
    m_body->setCurrentIndex(rows.isEmpty() ? 1 : 0);
    // Nothing to forget while there is nothing recorded, and a button that asks for confirmation before
    // saying "there was nothing to clear" is a button that lied about being pressed.
    m_clearButton->setEnabled(!rows.isEmpty());
}

void HistoryPage::updateSummary(const Tally &tally)
{
    const int rate = tally.successRate();
    m_lastRate = rate;

    if (rate < 0)
    {
        // "0%" would be a claim about checks that never happened. The empty record says so in words, and
        // the three chips go quiet with it: "Всего: 0 / Ошибок: 0 / Подмен: 0" restates the sentence above
        // in three small plates, and the placeholder below already explains what to do about it.
        m_headline->setText(LanguageManager::tr(QStringLiteral("history.headline.empty")));
        m_totalBadge->hide();
        m_failedBadge->hide();
        m_certBadge->hide();
    }
    else
    {
        m_headline->setText(LanguageManager::tr(QStringLiteral("history.headline"),
                                                QString::number(rate)));
        // Zero never wears a state colour. The red and the amber are claims that something happened, and a
        // chip that shouts about nothing would train the reader to ignore the one time it is right.
        m_totalBadge->setText(LanguageManager::tr(QStringLiteral("history.badge.total"))
                                  .arg(tally.total));
        applyBadgeKind(m_totalBadge, kBadgeNeutral);

        m_failedBadge->setText(LanguageManager::tr(QStringLiteral("history.badge.failed"))
                                   .arg(tally.failed));
        applyBadgeKind(m_failedBadge, tally.failed > 0 ? kBadgeDanger : kBadgeNeutral);

        m_certBadge->setText(LanguageManager::tr(QStringLiteral("history.badge.cert"))
                                 .arg(tally.intercepted));
        applyBadgeKind(m_certBadge, tally.intercepted > 0 ? kBadgeWarning : kBadgeNeutral);

        m_totalBadge->show();
        m_failedBadge->show();
        m_certBadge->show();
    }

    // A QProgressBar cannot show "no measurement", so an empty history moves the bar to zero without
    // painting it as a verdict: rateLevel="none" is grey, and only a real number earns a colour.
    static_cast<QProgressBar *>(m_rateBar)->setValue(qMax(0, rate));
    setRateLevel(rate);
}

// The rate bar's colour band, stamped as a dynamic property the sheet reads. Unpolish/repolish is what
// makes the style re-read it — the same three lines every dynamic-property restyle in this app pays.
void HistoryPage::setRateLevel(int rate)
{
    const QString level = rate < 0                  ? QStringLiteral("none")
                          : rate >= kRateGoodMin    ? QStringLiteral("good")
                          : rate >= kRateFairMin    ? QStringLiteral("fair")
                                                    : QStringLiteral("poor");
    if (m_rateBar->property(kRateLevelProperty).toString() == level)
        return;
    m_rateBar->setProperty(kRateLevelProperty, level);
    m_rateBar->style()->unpolish(m_rateBar);
    m_rateBar->style()->polish(m_rateBar);
    m_rateBar->update();
}

// ---- the one destructive action ----

void HistoryPage::askClear()
{
    if (!m_history)
        return;

    const int total = m_history->count();
    if (total == 0)
        return;

    if (!confirmClear(total))
        return;

    clearHistory();
}

// The question, named apart from the press that asks it. The number is the count at the moment of asking,
// not a later one: "you are about to lose 412 checks" is a different decision from "you are about to lose
// everything", and the default answer is No because the thing being destroyed is the only evidence the
// user has that today's failure is a new one.
bool HistoryPage::confirmClear(int totalRows)
{
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this,
        LanguageManager::tr(QStringLiteral("history.clear.title")),
        LanguageManager::tr(QStringLiteral("history.clear.confirm")).arg(totalRows),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return answer == QMessageBox::Yes;
}

void HistoryPage::clearHistory()
{
    if (!m_history)
        return;

    const int removed = m_history->count();
    if (removed == 0)
        return;

    // The store forgets and says so; changed() brings the table and the summary back in line, so the
    // page never has to keep its own idea of what the record holds.
    m_history->clear();
    emit cleared(removed);
}

// ---- language and theme ----

void HistoryPage::retranslateUi()
{
    // Headers and verdict words are derived rather than tagged, so they are re-announced and re-read.
    static_cast<HistoryTableModel *>(m_table->model())->retranslate();
    refresh();
}

void HistoryPage::retheme()
{
    // The tint and the status colour come from Theme::, and a model caches nothing, so the rows only have
    // to be painted again. The placeholder glyph, on the other hand, is a baked pixmap and must be
    // re-taken from the new tokens or it keeps the old theme's ink.
    m_emptyGlyph->setPixmap(Theme::icon(QStringLiteral("clock"), Theme::mutedText(), 34)
                               .pixmap(34, 34));
    if (QTableView *t = m_table)
        t->viewport()->update();
    // The bar's band is chosen from colours the sheet paints with, and the property has not moved — so
    // re-polish the one widget whose chunk hue lives in the sheet rather than in the code.
    m_rateBar->style()->unpolish(m_rateBar);
    m_rateBar->style()->polish(m_rateBar);
    m_rateBar->update();
}

// ---- test hooks ----

int HistoryPage::rowCount() const
{
    return m_table->model() ? m_table->model()->rowCount(QModelIndex()) : 0;
}

QTableView *HistoryPage::table() const { return m_table; }

QPushButton *HistoryPage::clearButton() const { return m_clearButton; }

QLabel *HistoryPage::headline() const { return m_headline; }

QLabel *HistoryPage::totalBadge() const { return m_totalBadge; }

QLabel *HistoryPage::failedBadge() const { return m_failedBadge; }

QLabel *HistoryPage::certBadge() const { return m_certBadge; }

QString HistoryPage::badgeKind(const QLabel *badge) const
{
    return badge ? badge->property(kBadgeKindProperty).toString() : QString();
}

bool HistoryPage::summaryBadgesVisible() const
{
    // The visible flag rather than isVisible(): the page is built and read without ever being shown, and
    // a chip that was hidden on purpose is the fact being asked about, not whether its window is up.
    return m_totalBadge && !m_totalBadge->isHidden();
}

QWidget *HistoryPage::tableCard() const { return m_tableCard; }

bool HistoryPage::showsEmptyNotice() const
{
    return m_body->currentIndex() == 1;
}

int HistoryPage::successRate() const { return m_lastRate; }

QString HistoryPage::rateLevel() const
{
    return m_rateBar->property(kRateLevelProperty).toString();
}

// ---- the table's model ----

HistoryTableModel::HistoryTableModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

void HistoryTableModel::setRows(const QVector<HistoryEntry> &rows)
{
    // A reset and not a diff. The rows usually arrive one per host while a cycle reports, so "the store
    // changed" is normally "one row appeared at the top" — but it is equally "the file was loaded", "the
    // cap trimmed the oldest" or "everything was cleared", and a model that classified those would spend
    // more code on the bookkeeping than a thousand-row reset ever costs in repaints.
    beginResetModel();
    m_rows = rows;
    applySort();
    endResetModel();
}

int HistoryTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int HistoryTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant HistoryTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return QVariant();

    const HistoryEntry &entry = m_rows.at(index.row());
    const HistoryVerdict verdict = HistoryPage::verdictFor(entry);

    switch (role)
    {
    case Qt::DisplayRole:
        switch (index.column())
        {
        case ColumnTime:    return displayTime(entry.timestamp);
        // A row recorded with no preset name says so with a dash instead of blank space: an empty cell
        // reads as a layout bug, and which profile ran the check is part of the answer.
        case ColumnPreset:  return entry.preset.isEmpty() ? QStringLiteral("—") : entry.preset;
        case ColumnHost:    return entry.host;
        case ColumnVerdict: return HistoryPage::verdictText(verdict);
        // The dash is the engine's own "nothing connected", kept distinguishable from a 0 that would
        // claim a connection that answered before it was asked.
        case ColumnLatency: return entry.latency < 0 ? QStringLiteral("—")
                                                     : QString::number(entry.latency);
        }
        return QVariant();

    case Qt::BackgroundRole:
        // The whole row wears the verdict, which is what makes a failed night findable by colour alone.
        return QBrush(HistoryPage::verdictTint(verdict));

    case Qt::ForegroundRole:
        // Only the status word takes the full-strength hue; the rest of the row keeps the page's text
        // colour, or a table of four colours would be a palette rather than a record.
        if (index.column() == ColumnVerdict)
            return QBrush(HistoryPage::verdictColor(verdict));
        return QVariant();

    case Qt::FontRole:
        // Digits line up when the column is monospaced: two thousand rows of 127 ms over 43 ms are read
        // down the right edge, and a proportional font puts every one of them somewhere new.
        if (index.column() == ColumnTime || index.column() == ColumnLatency)
            return monoFont();
        return QVariant();

    case Qt::TextAlignmentRole:
        if (index.column() == ColumnLatency)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);

    case Qt::ToolTipRole:
        // The one verdict that needs a sentence rather than a word: an amber row is the page saying
        // "this looked like a success and was not", and the reader deserves to know why.
        if (index.column() == ColumnVerdict)
        {
            if (verdict == HistoryVerdict::Intercepted)
                return LanguageManager::tr(QStringLiteral("history.verdict.intercepted_tip"));
            if (verdict == HistoryVerdict::Failed)
                return LanguageManager::tr(QStringLiteral("history.verdict.failed_tip"));
        }
        return QVariant();
    }
    return QVariant();
}

QVariant HistoryTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        return QVariant();

    switch (section)
    {
    case ColumnTime:    return LanguageManager::tr(QStringLiteral("history.col.time"));
    case ColumnPreset:  return LanguageManager::tr(QStringLiteral("history.col.preset"));
    case ColumnHost:    return LanguageManager::tr(QStringLiteral("history.col.host"));
    case ColumnVerdict: return LanguageManager::tr(QStringLiteral("history.col.verdict"));
    case ColumnLatency: return LanguageManager::tr(QStringLiteral("history.col.latency"));
    }
    return QVariant();
}

void HistoryTableModel::sort(int column, Qt::SortOrder order)
{
    // The view asks for an unsorted reset (column -1) before the first real click, and a model that
    // recorded that as its order would lose the newest-first reading order for the rest of the run.
    if (column < 0 || column >= ColumnCount)
        return;

    m_sortColumn = column;
    m_sortOrder = order;
    if (m_rows.size() < 2)
        return;

    // Reset rather than layoutChanged: a sort moves every row, so there is no consistent selection to
    // keep, and a record read by colour needs an unambiguously correct table more than it needs the
    // highlight it had a moment ago.
    beginResetModel();
    applySort();
    endResetModel();
}

void HistoryTableModel::retranslate()
{
    // The headers are derived from the catalog rather than tagged on a widget, so the walk the window
    // does cannot reach them and they have to be re-announced to the view.
    emit headerDataChanged(Qt::Horizontal, 0, ColumnCount - 1);
}

void HistoryTableModel::applySort()
{
    if (m_rows.size() < 2)
        return;

    const int column = m_sortColumn;
    const bool ascending = (m_sortOrder == Qt::AscendingOrder);

    std::stable_sort(m_rows.begin(), m_rows.end(),
                     [column, ascending](const HistoryEntry &a, const HistoryEntry &b) {
        int cmp = 0;
        switch (column)
        {
        case ColumnTime:
        {
            const qint64 ta = a.timestamp.toMSecsSinceEpoch();
            const qint64 tb = b.timestamp.toMSecsSinceEpoch();
            cmp = (ta == tb) ? 0 : (ta < tb ? -1 : 1);
            break;
        }
        case ColumnPreset:
            cmp = QString::compare(a.preset, b.preset, Qt::CaseInsensitive);
            break;
        case ColumnHost:
            cmp = QString::compare(a.host, b.host, Qt::CaseInsensitive);
            break;
        case ColumnVerdict:
            cmp = verdictRank(HistoryPage::verdictFor(a))
                  - verdictRank(HistoryPage::verdictFor(b));
            break;
        case ColumnLatency:
        {
            const qint64 la = latencySortKey(a);
            const qint64 lb = latencySortKey(b);
            cmp = (la == lb) ? 0 : (la < lb ? -1 : 1);
            break;
        }
        }

        if (cmp == 0)
        {
            // A tie in the clicked column falls back to the record's own order — newest first, then the
            // host — so the result is a function of the data alone and not of however many sorts the
            // reader clicked through to arrive here.
            const qint64 ta = a.timestamp.toMSecsSinceEpoch();
            const qint64 tb = b.timestamp.toMSecsSinceEpoch();
            cmp = (ta == tb) ? QString::compare(a.host, b.host, Qt::CaseInsensitive)
                             : (ta > tb ? -1 : 1);
        }
        return ascending ? (cmp < 0) : (cmp > 0);
    });
}

} // namespace zapret

