#pragma once

#include <QColor>
#include <QFont>
#include <QString>

class QApplication;
class QIcon;

namespace zapret {

// Dark/light theme bootstrap for the GUI.
//
// All visual identity lives here: the QSS sheet is loaded from the compiled resource
// (:/:/theme_dark.qss and :/theme_light.qss), a matching QPalette is installed so native pieces
// (QFileDialog, QMessageBox buttons, window chrome drawn by the platform theme) agree with the
// stylesheet, and the monochrome SVG icons are recoloured on load.
//
// The SVGs ship with `currentColor` as their paint value. Qt's stylesheet engine does not reliably
// substitute a QSS colour into an SVG loaded through image: url(), so icon colouring is done here
// instead: the SVG text is read, `currentColor` is replaced with the requested colour and the
// requested pixel size, and the result is rasterised through the SVG image format plugin. That keeps
// one source of truth for each glyph while letting the theme drive its colour.
class Theme
{
public:
    enum class Mode
    {
        Dark,
        Light,
        System
    };

    // Install font + palette + stylesheet. `apply(app, bool)` is the spec's entry point; the Mode
    // overload resolves Mode::System against the platform's reported colour scheme.
    static void apply(QApplication &app, bool dark);
    static void apply(QApplication &app, Mode mode);

    // "dark" | "light" | "system" <-> Mode. Unknown names fall back to Dark (the shipped default).
    static Mode modeFromName(const QString &name);
    static QString nameForMode(Mode mode);

    // The mode currently installed (Dark before any apply()).
    static Mode currentMode();
    static bool isDark();

    // What Mode::System would resolve to right now.
    static bool systemPrefersDark();

    static QString styleSheet(bool dark);
    static QString resourcePathFor(bool dark);
    static QFont uiFont();

    // ---- colour tokens (mirror the QSS palette so C++-drawn widgets match) ----
    static QColor windowBackground();
    static QColor surface();
    static QColor hover();
    static QColor border();
    static QColor text();
    static QColor mutedText();
    static QColor accent();
    // The secondary accent (cyan in the dark sheet, teal-blue in the light one): progress, links and
    // the "alive" edge of a card. accent() is theme-dependent for the same reason the QSS is — a
    // #C084FC glyph on a #F4F5FA dialog is unreadable, and this colour is baked into icons.
    static QColor accentSecondary();
    static QColor success();
    static QColor error();
    static QColor warning();
    // Log line colour for a level (0 info, 1 warn, 2 error).
    static QColor logColor(int level);
    // Default icon tint for the current theme.
    static QColor iconColor();
    // Ping indicator colours (spec-fixed neon, theme-independent): the ring, and the grey of its label
    // and clock glyph. pingOk is the dashboard's single "live" green — the connected chip, the running
    // power ring and a reachable service's halo are all this one colour.
    static QColor pingUnknown();
    static QColor pingWaiting();
    static QColor pingOk();
    static QColor pingFail();
    static QColor pingMuted();
    // The mark inside a dashboard ring, as opposed to the ring itself. White in the dark sheet, where
    // it sits inside a glow on a #0B0C10 canvas; near-black in the light one, where the same white
    // mark lands on a #FFFFFF page and vanishes -- which is what made the Discord, YouTube and Google
    // glyphs unreadable there. Deliberately not text(): the dark sheet's text is #E7E9F2 and the
    // light sheet's is #1F2937, while this mark has to be pure white on one and dark on the other, so
    // no single text token is the right answer for both.
    static QColor markOnRing();

    // Load :/icons/<name>.svg tinted with `color`. `size` is the primary pixel size; the icon also
    // carries 16/24/32 rasterisations so toolbars, menus and HiDPI all get a crisp glyph.
    static QIcon icon(const QString &name, const QColor &color, int size = 16);
    // icon(name, Theme::iconColor(), size).
    static QIcon themedIcon(const QString &name, int size = 16);

private:
    Theme() = delete;
};

} // namespace zapret
