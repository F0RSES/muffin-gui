#include "LogPanel.h"
#include "LanguageManager.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFormat>
#include <QTime>
#include <QVariant>
#include <QVBoxLayout>

namespace zapret {

namespace {
// Per-block stash of that line's level, so the filter can classify a block without keeping a second
// index into the document.
constexpr int kLevelProperty = QTextFormat::UserProperty + 1;

int normalizedLevel(int level)
{
    return (level == LogPanel::Warn || level == LogPanel::Error) ? level : LogPanel::Info;
}
} // namespace

LogPanel::LogPanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("logPanel"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto *header = new QFrame(this);
    header->setObjectName(QStringLiteral("logHeader"));

    // One row: the level filter on the left, the three actions on the right.
    //
    // The panel used to open with a title, an auto-scroll checkbox and a
    // second row underneath. The title named a box of logs that is already unmistakable, the grey line
    // below it explained what the rows show, and nobody chooses auto-scroll twice — so the journal now
    // starts where the text starts and always follows a new line.
    auto *levelCaption = new QLabel(header);
    levelCaption->setObjectName(QStringLiteral("muted"));
    i18nText(levelCaption, QStringLiteral("log.level"));

    m_levelBox = new QComboBox(header);
    // Display text only — the filter reads itemData() (the int level), so nothing keys off these words.
    m_levelBox->addItem(LanguageManager::tr(QStringLiteral("log.filter.all")), int(All));
    m_levelBox->addItem(LanguageManager::tr(QStringLiteral("log.filter.info")), int(Info));
    m_levelBox->addItem(LanguageManager::tr(QStringLiteral("log.filter.warn")), int(Warn));
    m_levelBox->addItem(LanguageManager::tr(QStringLiteral("log.filter.error")), int(Error));
    m_levelBox->setCurrentIndex(0);
    i18nItems(m_levelBox, { QStringLiteral("log.filter.all"), QStringLiteral("log.filter.info"),
                            QStringLiteral("log.filter.warn"), QStringLiteral("log.filter.error") });

    m_copyButton = new QPushButton(header);
    m_clearButton = new QPushButton(header);
    m_saveButton = new QPushButton(header);
    i18nText(m_copyButton, QStringLiteral("log.copy"));
    i18nText(m_clearButton, QStringLiteral("log.clear"));
    i18nText(m_saveButton, QStringLiteral("log.save"));
    i18nTip(m_copyButton, QStringLiteral("log.copy_tip"));
    i18nTip(m_clearButton, QStringLiteral("log.clear_tip"));
    i18nTip(m_saveButton, QStringLiteral("log.save_tip"));

    auto *row = new QHBoxLayout(header);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(levelCaption);
    row->addWidget(m_levelBox);
    row->addStretch(1);
    row->addWidget(m_copyButton);
    row->addWidget(m_clearButton);
    row->addWidget(m_saveButton);

    m_view = new QPlainTextEdit(this);
    m_view->setObjectName(QStringLiteral("logView"));
    m_view->setReadOnly(true);
    m_view->setMaximumBlockCount(kMaxLines);
    // Wrap rather than clip: zapret's own output lines (list paths, winws banners) routinely exceed
    // the pane width, and a horizontal scrollbar is easy to miss behind a long-running log.
    m_view->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_view->setUndoRedoEnabled(false);

    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(12, 10, 12, 12);
    column->setSpacing(8);
    column->addWidget(header);
    column->addWidget(m_view, 1);

    connect(m_levelBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        setFilterLevel(m_levelBox->itemData(index).toInt());
    });
    connect(m_copyButton, &QPushButton::clicked, this, &LogPanel::copyToClipboard);
    connect(m_clearButton, &QPushButton::clicked, this, &LogPanel::clear);
    connect(m_saveButton, &QPushButton::clicked, this,
            static_cast<void (LogPanel::*)()>(&LogPanel::saveToFile));
    m_copyButton->setIcon(Theme::themedIcon(QStringLiteral("copy"), 16));
    m_clearButton->setIcon(Theme::themedIcon(QStringLiteral("clear"), 16));
}

LogPanel::~LogPanel() = default;

QString LogPanel::levelTag(int level)
{
    switch (normalizedLevel(level))
    {
    case Warn:  return QStringLiteral("WARN");
    case Error: return QStringLiteral("ERR");
    default:    return QStringLiteral("INFO");
    }
}

QString LogPanel::formatted(const Entry &entry) const
{
    return QStringLiteral("[%1] [%2] %3").arg(entry.clock, levelTag(entry.level), entry.message);
}

void LogPanel::appendLine(const QString &line, int level)
{
    Entry entry;
    entry.clock = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    entry.message = line;
    entry.level = normalizedLevel(level);
    m_entries.append(entry);

    // The widget's own ring buffer drops the oldest block; mirror that in the cache so copy/save and
    // the counts agree with what is on screen.
    while (m_entries.size() > kMaxLines)
        m_entries.removeFirst();

    // Filtered-out lines are stored but not inserted; setFilterLevel() reveals them again later.
    if (m_filter != All && entry.level < m_filter)
        return;

    insertIntoView(entry);
    if (autoScroll())
        scrollToBottom();
}

void LogPanel::insertIntoView(const Entry &entry)
{
    QTextCursor cursor(m_view->document());
    cursor.movePosition(QTextCursor::End);

    QTextBlockFormat blockFormat;
    blockFormat.setProperty(kLevelProperty, entry.level);

    if (!m_hasFirstLine) {
        // QPlainTextEdit starts with one empty block: reuse it instead of leaving a blank first line.
        m_hasFirstLine = true;
        cursor.setBlockFormat(blockFormat);
    } else {
        cursor.insertBlock(blockFormat);
    }

    QTextCharFormat charFormat;
    charFormat.setForeground(Theme::logColor(entry.level));
    cursor.insertText(formatted(entry), charFormat);
}

void LogPanel::scrollToBottom()
{
    QScrollBar *bar = m_view->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void LogPanel::applyFilterToDocument()
{
    QTextDocument *doc = m_view->document();
    for (QTextBlock block = doc->firstBlock(); block.isValid(); block = block.next()) {
        const QVariant stored = block.blockFormat().property(kLevelProperty);
        if (!stored.isValid())
            continue;                      // never a log line (e.g. a trailing empty block)
        const int level = stored.toInt();
        const bool visible = (m_filter == All) || (level >= m_filter);
        if (block.isVisible() != visible)
            block.setVisible(visible);
    }
    // Toggling block visibility does not re-run layout by itself; nudge the document so the
    // viewport, scroll range and block geometry are recomputed.
    doc->adjustSize();
}

void LogPanel::setFilterLevel(int level)
{
    if (level != All && level != Info && level != Warn && level != Error)
        level = All;
    if (level == m_filter)
        return;

    m_filter = level;

    const int index = m_levelBox->findData(level);
    if (index >= 0 && m_levelBox->currentIndex() != index)
        m_levelBox->setCurrentIndex(index);

    applyFilterToDocument();
    emit filterLevelChanged(m_filter);
    if (autoScroll())
        scrollToBottom();
}

bool LogPanel::autoScroll() const
{
    // Always true now that the checkbox is gone: a log that stops following itself is the bug every
    // reporter here actually hit. The setter stays so a caller (or a test) can pin the behaviour off
    // without reaching into the widget.
    return m_autoScrollOn;
}

void LogPanel::setAutoScroll(bool on)
{
    m_autoScrollOn = on;
    if (on)
        scrollToBottom();
}

void LogPanel::clear()
{
    m_entries.clear();
    m_view->clear();
    m_hasFirstLine = false;
}

void LogPanel::copyToClipboard()
{
    QApplication::clipboard()->setText(text());
}

QString LogPanel::text() const
{
    QStringList lines;
    lines.reserve(m_entries.size());
    for (const Entry &entry : m_entries)
        lines << formatted(entry);
    return lines.join(QLatin1Char('\n'));
}

int LogPanel::visibleLineCount() const
{
    int count = 0;
    const QTextDocument *doc = m_view->document();
    for (QTextBlock block = doc->firstBlock(); block.isValid(); block = block.next()) {
        if (block.isVisible() && block.blockFormat().property(kLevelProperty).isValid())
            ++count;
    }
    return count;
}

bool LogPanel::saveToFile(const QString &path) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    file.write(text().toUtf8());
    file.write("\n");
    return true;
}

void LogPanel::saveToFile()
{
    const QString suggested = QStringLiteral("zapret-%1.log")
                                  .arg(QTime::currentTime().toString(QStringLiteral("HH-mm-ss")));
    const QString path = QFileDialog::getSaveFileName(this,
                                                      LanguageManager::tr(QStringLiteral("log.save_title")),
                                                      suggested,
                                                      LanguageManager::tr(QStringLiteral("log.save_filters")));
    if (path.isEmpty())
        return;
    if (!saveToFile(path))
        QMessageBox::warning(this, LanguageManager::tr(QStringLiteral("log.save_failed_title")),
                             LanguageManager::tr(QStringLiteral("log.save_failed")).arg(path));
}

} // namespace zapret
