#pragma once

#include <QVector>
#include <QWidget>

class QComboBox;
class QPlainTextEdit;
class QPushButton;

namespace zapret {

// The log view: coloured, timestamped lines with a level filter.
//
// Lines are kept twice on purpose: once as document blocks (what the user reads) and once as a small
// structured record (what copy/save and the filter operate on). The filter hides blocks in place via
// QTextBlock::setVisible rather than rebuilding the document, so scrolling position and the
// 5000-block ring buffer survive a level change.
//
// Line colouring uses QTextCharFormat per insertion (not HTML), which is what keeps a plain,
// searchable log view with monospace metrics.
class LogPanel : public QWidget
{
    Q_OBJECT

public:
    enum Level
    {
        All   = -1,
        Info  = 0,
        Warn  = 1,
        Error = 2
    };
    Q_ENUM(Level)

    // Mirrors the backend log levels (0 info / 1 warn / 2 error); anything else is treated as info.
    static constexpr int kMaxLines = 5000;

    explicit LogPanel(QWidget *parent = nullptr);
    ~LogPanel() override;

    void appendLine(const QString &line, int level = Info);

    void setFilterLevel(int level);
    int filterLevel() const { return m_filter; }

    void setAutoScroll(bool on);
    bool autoScroll() const;

    void clear();
    void copyToClipboard();

    // Programmatic save (used by tests) and the interactive one (used by the button).
    bool saveToFile(const QString &path) const;
    void saveToFile();

    // All stored lines, formatted exactly as displayed.
    QString text() const;
    int storedLineCount() const { return static_cast<int>(m_entries.size()); }
    int visibleLineCount() const;

    QPlainTextEdit *view() const { return m_view; }

signals:
    void filterLevelChanged(int level);

private:
    struct Entry
    {
        QString clock;     // HH:mm:ss
        QString message;
        int level = Info;
    };

    static QString levelTag(int level);
    QString formatted(const Entry &entry) const;
    void insertIntoView(const Entry &entry);
    void applyFilterToDocument();
    void scrollToBottom();

    QVector<Entry> m_entries;
    QPlainTextEdit *m_view = nullptr;
    QComboBox *m_levelBox = nullptr;
    bool m_autoScrollOn = true;   // always on since the checkbox left the toolbar
    QPushButton *m_copyButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    int m_filter = All;
    bool m_hasFirstLine = false;
};

} // namespace zapret
