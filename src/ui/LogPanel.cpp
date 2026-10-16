#include "LogPanel.h"
#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
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

    auto *title = new QLabel(QStringLiteral("Логи"), header);
    title->setObjectName(QStringLiteral("cardTitle"));

    m_autoScroll = new QCheckBox(QStringLiteral("Автопрокрутка"), header);
    m_autoScroll->setChecked(true);

    auto *levelCaption = new QLabel(QStringLiteral("Уровень"), header);
    levelCaption->setObjectName(QStringLiteral("muted"));

    m_levelBox = new QComboBox(header);
    m_levelBox->addItem(QStringLiteral("All"), int(All));
    m_levelBox->addItem(QStringLiteral("Info"), int(Info));
    m_levelBox->addItem(QStringLiteral("Warn"), int(Warn));
    m_levelBox->addItem(QStringLiteral("Error"), int(Error));
    m_levelBox->setCurrentIndex(0);

    m_copyButton = new QPushButton(QStringLiteral("Копировать"), header);
    m_clearButton = new QPushButton(QStringLiteral("Очистить"), header);
    m_saveButton = new QPushButton(QStringLiteral("Сохранить в файл"), header);
    m_copyButton->setToolTip(QStringLiteral("Скопировать весь лог в буфер обмена"));
    m_clearButton->setToolTip(QStringLiteral("Очистить лог"));
    m_saveButton->setToolTip(QStringLiteral("Сохранить лог в файл"));

    // Two rows, not one: the full set of labels needs ~560 px on a single line, which is wider than
    // the panel's 400 px minimum and clipped the buttons. Row 1 is identity + scroll behaviour,
    // row 2 is the filter plus the three actions.
    auto *rows = new QVBoxLayout(header);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(6);

    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(8);
    top->addWidget(title);
    top->addSpacing(6);
    top->addWidget(m_autoScroll);
    top->addStretch(1);

    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(0, 0, 0, 0);
    bottom->setSpacing(8);
    bottom->addWidget(levelCaption);
    bottom->addWidget(m_levelBox);
    bottom->addStretch(1);
    bottom->addWidget(m_copyButton);
    bottom->addWidget(m_clearButton);
    bottom->addWidget(m_saveButton);

    rows->addLayout(top);
    rows->addLayout(bottom);

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
    return m_autoScroll->isChecked();
}

void LogPanel::setAutoScroll(bool on)
{
    m_autoScroll->setChecked(on);
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
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Сохранить логи"),
                                                      suggested,
                                                      QStringLiteral("Логи (*.log);;Текст (*.txt);;Все файлы (*)"));
    if (path.isEmpty())
        return;
    if (!saveToFile(path))
        QMessageBox::warning(this, QStringLiteral("Сохранение логов"),
                             QStringLiteral("Не удалось записать файл: %1").arg(path));
}

} // namespace zapret
