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
    // Added for the dashboard redesign: the sidebar rail, the recessed fields/log well, the two
    // accents and the three state colours. Every one of these is mirrored by a hex in both .qss
    // sheets — see the token comment at the top of each sheet, and change the two together.
    QColor sidebar, recessed, accent, accent2, ok, err, warn;
};

Tokens tokensFor(bool dark)
{
    if (dark)
        return {QColor(0x0B, 0x0C, 0x10), QColor(0x15, 0x16, 0x1C), QColor(0x1F, 0x22, 0x2D),
                QColor(0x26, 0x2A, 0x36), QColor(0xE7, 0xE9, 0xF2), QColor(0x8A, 0x91, 0xA6),
                QColor(0xE7, 0xE9, 0xF2),
                QColor(0x10, 0x12, 0x19), QColor(0x09, 0x0A, 0x0D), QColor(0xC0, 0x84, 0xFC),
                QColor(0x22, 0xD3, 0xEE), QColor(0x34, 0xD3, 0x99), QColor(0xF8, 0x71, 0x71),
                QColor(0xFB, 0xBF, 0x24)};
    return {QColor(0xF4, 0xF5, 0xFA), QColor(0xFF, 0xFF, 0xFF), QColor(0xEE, 0xF0, 0xF8),
            QColor(0xDD, 0xE0, 0xEC), QColor(0x1F, 0x29, 0x37), QColor(0x6B, 0x72, 0x80),
            QColor(0x1F, 0x29, 0x37),
            QColor(0xE9, 0xEB, 0xF3), QColor(0xFF, 0xFF, 0xFF), QColor(0x7C, 0x3A, 0xED),
            QColor(0x08, 0x91, 0xB2), QColor(0x05, 0x96, 0x69), QColor(0xDC, 0x26, 0x26),
            QColor(0xD9, 0x77, 0x06)};
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
    p.setColor(QPalette::Base, t.recessed);
    p.setColor(QPalette::AlternateBase, t.surface);
    p.setColor(QPalette::ToolTipBase, t.surface);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Button, t.surface);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::BrightText, t.err);
    p.setColor(QPalette::Highlight, t.accent);
    // White text on #C084FC is a 2.4:1 contrast ratio; the neon accents are dark-on-light by design,
    // so the highlighted pair inverts to the app background instead of to white.
    p.setColor(QPalette::HighlightedText, dark ? t.window : QColor(Qt::white));
    p.setColor(QPalette::PlaceholderText, t.muted);
    p.setColor(QPalette::Link, t.accent2);
    // Disabled variants: dimmed text on the same surfaces, never a light-on-light grey box.
    p.setColor(QPalette::Disabled, QPalette::Text, t.muted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.muted);
    p.setColor(QPalette::Disabled, QPalette::WindowText, t.muted);
    p.setColor(QPalette::Disabled, QPalette::Highlight, t.border);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, t.muted);
    return p;
}

// The two greys the sheet uses for icons and for the quietest labels. Kept as literals rather than
// Tokens members because they are derived, not chosen: an icon must sit between text and muted text
// in luminance so a toolbar glyph never reads as body copy.
QColor iconColorFor(bool dark)
{
    return dark ? QColor(0xB8, 0xBE, 0xD0) : QColor(0x40, 0x46, 0x55);
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
QColor Theme::accent()           { return tokensFor(isDark()).accent; }
QColor Theme::accentSecondary()  { return tokensFor(isDark()).accent2; }
QColor Theme::success()          { return tokensFor(isDark()).ok; }
QColor Theme::error()            { return tokensFor(isDark()).err; }
QColor Theme::warning()          { return tokensFor(isDark()).warn; }
QColor Theme::pingUnknown()      { return QColor(0x6B, 0x72, 0x80); }
QColor Theme::pingWaiting()      { return tokensFor(isDark()).warn; }
QColor Theme::pingOk()           { return tokensFor(isDark()).ok; }
QColor Theme::pingFail()         { return tokensFor(isDark()).err; }

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
    return iconColorFor(isDark());
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
