#include "Theme.h"

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QPalette>
#include <QStyleHints>

namespace zapret {

namespace {
// The single mutable piece of state: which mode is installed. QSS + palette are derived from it.
Theme::Mode g_mode = Theme::Mode::Dark;

// Tokens shared with the QSS files. Kept here so C++-painted widgets (PingIndicator, log text
// formats, tray tint) never hardcode a colour that could drift from the sheet.
struct Tokens
{
    QColor window, surface, hover, border, text, muted, logText;
};

Tokens tokensFor(bool dark)
{
    if (dark)
        return {QColor(0x1E, 0x1E, 0x2E), QColor(0x2A, 0x2A, 0x3C), QColor(0x34, 0x34, 0x4A),
                QColor(0x3A, 0x3A, 0x4E), QColor(0xE4, 0xE4, 0xE7), QColor(0x9C, 0xA3, 0xAF),
                QColor(0xE4, 0xE4, 0xE7)};
    return {QColor(0xF4, 0xF5, 0xFA), QColor(0xFF, 0xFF, 0xFF), QColor(0xE4, 0xE6, 0xF1),
            QColor(0xDC, 0xDE, 0xEA), QColor(0x1F, 0x29, 0x37), QColor(0x6B, 0x72, 0x80),
            QColor(0x1F, 0x29, 0x37)};
}

bool resolvingDark(Theme::Mode mode)
{
    return mode == Theme::Mode::Dark || (mode == Theme::Mode::System && Theme::systemPrefersDark());
}

QPalette paletteFor(bool dark)
{
    const Tokens t = tokensFor(dark);
    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::Base, dark ? QColor(0x22, 0x22, 0x2F) : QColor(Qt::white));
    p.setColor(QPalette::AlternateBase, t.surface);
    p.setColor(QPalette::ToolTipBase, t.surface);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Button, t.surface);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::BrightText, QColor(0xEF, 0x44, 0x44));
    p.setColor(QPalette::Highlight, QColor(0x7C, 0x3A, 0xED));
    p.setColor(QPalette::HighlightedText, QColor(Qt::white));
    p.setColor(QPalette::PlaceholderText, t.muted);
    p.setColor(QPalette::Link, QColor(0x8B, 0x5C, 0xF6));
    // Disabled variants: dimmed text on the same surfaces, never a light-on-light grey box.
    p.setColor(QPalette::Disabled, QPalette::Text, t.muted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.muted);
    p.setColor(QPalette::Disabled, QPalette::WindowText, t.muted);
    p.setColor(QPalette::Disabled, QPalette::Highlight, t.border);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, t.muted);
    return p;
}
} // namespace

void Theme::apply(QApplication &app, bool dark)
{
    apply(app, dark ? Mode::Dark : Mode::Light);
}

void Theme::apply(QApplication &app, Theme::Mode mode)
{
    g_mode = mode;
    const bool dark = resolvingDark(mode);

    QPalette pal = paletteFor(dark);
    // Qt >= 6.4 exposes an accent colour to native parts (focus rings, checkmarks).
    pal.setColor(QPalette::Accent, accent());

    app.setFont(uiFont());
    app.setPalette(pal);
    app.setStyleSheet(styleSheet(dark));
}

Theme::Mode Theme::modeFromName(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("light"))
        return Theme::Mode::Light;
    if (n == QLatin1String("system"))
        return Theme::Mode::System;
    return Theme::Mode::Dark;
}

QString Theme::nameForMode(Theme::Mode mode)
{
    switch (mode)
    {
    case Theme::Mode::Light: return QStringLiteral("light");
    case Theme::Mode::System: return QStringLiteral("system");
    case Theme::Mode::Dark: break;
    }
    return QStringLiteral("dark");
}

Theme::Mode Theme::currentMode()
{
    return g_mode;
}

bool Theme::isDark()
{
    return resolvingDark(g_mode);
}

bool Theme::systemPrefersDark()
{
    if (const QStyleHints *hints = QGuiApplication::styleHints())
        return hints->colorScheme() == Qt::ColorScheme::Dark;
    return true;
}

QString Theme::resourcePathFor(bool dark)
{
    return dark ? QStringLiteral(":/theme_dark.qss") : QStringLiteral(":/theme_light.qss");
}

QString Theme::styleSheet(bool dark)
{
    QFile f(resourcePathFor(dark));
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

QFont Theme::uiFont()
{
    QFont f;
    QStringList families;
#if defined(Q_OS_WIN)
    families << QStringLiteral("Segoe UI Variable Text") << QStringLiteral("Segoe UI");
#else
    families << QStringLiteral("Inter") << QStringLiteral("Segoe UI")
             << QStringLiteral("Noto Sans") << QStringLiteral("Cantarell");
#endif
    // Qt walks the family list per-script and falls back to the style default if none resolve.
    f.setFamilies(families);
    f.setPointSize(10);
    return f;
}

QColor Theme::windowBackground() { return tokensFor(isDark()).window; }
QColor Theme::surface()          { return tokensFor(isDark()).surface; }
QColor Theme::hover()            { return tokensFor(isDark()).hover; }
QColor Theme::border()           { return tokensFor(isDark()).border; }
QColor Theme::text()             { return tokensFor(isDark()).text; }
QColor Theme::mutedText()        { return tokensFor(isDark()).muted; }
QColor Theme::accent()           { return QColor(0x7C, 0x3A, 0xED); }
QColor Theme::success()          { return QColor(0x10, 0xB9, 0x81); }
QColor Theme::error()            { return QColor(0xEF, 0x44, 0x44); }
QColor Theme::warning()          { return QColor(0xF5, 0x9E, 0x0B); }
QColor Theme::pingUnknown()      { return QColor(0x6B, 0x72, 0x80); }
QColor Theme::pingWaiting()      { return QColor(0xF5, 0x9E, 0x0B); }
QColor Theme::pingOk()           { return QColor(0x10, 0xB9, 0x81); }
QColor Theme::pingFail()         { return QColor(0xEF, 0x44, 0x44); }

QColor Theme::logColor(int level)
{
    switch (level)
    {
    case 1:  return warning();
    case 2:  return error();
    default: return tokensFor(isDark()).logText;
    }
}

QColor Theme::iconColor()
{
    return isDark() ? QColor(0xC7, 0xCA, 0xD6) : QColor(0x40, 0x46, 0x55);
}

QIcon Theme::icon(const QString &name, const QColor &color, int size)
{
    const QString path = QStringLiteral(":/icons/%1.svg").arg(name);
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray base = file.readAll();
        const QByteArray tint = color.name(QColor::HexRgb).toLatin1();

        QIcon out;
        for (int px : {size, 16, 24, 32}) {
            // Rewrite the glyph's declared size so the SVG plugin rasterises exactly at that
            // resolution instead of scaling a 24 px bitmap up.
            QByteArray data = base;
            data.replace("currentColor", tint);
            data.replace("width=\"24\"", QByteArray("width=\"") + QByteArray::number(px) + "\"");
            data.replace("height=\"24\"", QByteArray("height=\"") + QByteArray::number(px) + "\"");
            QPixmap pm;
            if (pm.loadFromData(data, "SVG"))
                out.addPixmap(pm);
        }
        if (!out.isNull())
            return out;
    }
    // No SVG handler on this platform: fall back to whatever QIcon/the icon theme can offer.
    QIcon fallback(path);
    if (fallback.isNull())
        fallback = QIcon::fromTheme(name);
    return fallback;
}

QIcon Theme::themedIcon(const QString &name, int size)
{
    return icon(name, iconColor(), size);
}

} // namespace zapret
