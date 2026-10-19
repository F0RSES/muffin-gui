#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QIcon;
class QAbstractButton;
class QLabel;
class QAction;

namespace zapret {

// The UI's one dictionary, in memory, with no .qm files.
//
// Every screen is built from tr("key") calls, so a language switch never needs a restart: the manager
// stores one flat map per language (QHash<code, QHash<key, text>>) and emits languageChanged() when the
// code moves. Windows that live longer than the click — MainWindow, its rail, the log panel, the
// strategy cards — re-run their own retranslateUi() in answer to that signal; dialogs are constructed
// when they open, so they simply read the new language.
//
// Why a hand-written catalog instead of Qt's tr()/QTranslator pipeline: the .ts/.qm round trip needs
// lupdate in the build (a toolchain piece this project does not carry, and one CI would have to install
// on three platforms), and half of the strings here are format-bearing ("Найдено стратегий: %1 в %2")
// where a missed translation is a crash-shaped hole. A table compiled into the binary cannot be stale,
// cannot be missing at runtime, and is checkable in a test — which is exactly what uitest does: it
// asserts that every language defines every key.
//
// A missing key falls back to English and then to the key itself, so the worst a forgotten row can do is
// show an identifier — never an empty label.
//
// The default language is Russian ("ru"), the language every screen was drawn and tested in; Settings
// persists the choice under ui/language and main.cpp installs it before the first widget is built.
class LanguageManager : public QObject
{
    Q_OBJECT

public:
    // The singleton. Languages are data, not per-window state, so one instance owns the maps and every
    // widget asks it for text; the static tr() is a shorthand for that instance.
    static LanguageManager &instance();

    // The codes the catalog defines, in menu order: ru, en, es, de, zh, fr, hi.
    static const QStringList &codes();
    // Whether `code` is one the app can actually display.
    static bool isSupported(const QString &code);
    // The language's own name, written in that language ("Deutsch", "हिन्दी"): never translated,
    // because a user who cannot read the current UI is the one who has to recognise their language.
    static QString nativeName(const QString &code);
    // :/icons/flags/<code>.svg as an icon, or a null icon if the resource is missing.
    static QIcon flagIcon(const QString &code);

    // The language the UI is written in right now.
    QString currentCode() const;
    // Unknown codes are refused (and reported) rather than silently showing an empty UI; on success
    // emits languageChanged().
    void setCode(const QString &code);

    // ---- the catalog ----
    // Text for `key` in the current language. Declared here rather than inherited: naming tr() in this
    // class hides QObject::tr() to derived scopes, so there is no overload ambiguity to fall into.
    static QString tr(const QString &key);
    // Same, with %1 substituted — the shape most UI strings take.
    static QString tr(const QString &key, const QString &arg1);
    // Direct lookups, exposed for tests: current language, then English, then the key itself.
    static QString translate(const QString &language, const QString &key);
    // Every key the catalog defines (sorted, so a test's diff is stable).
    static QStringList keys();

signals:
    void languageChanged(const QString &code);

private:
    explicit LanguageManager(QObject *parent = nullptr);

    // The tables themselves live in LanguageManager.cpp, built once from the compiled catalog; the only
    // state here is which of them the UI is reading.
    QString m_code = QStringLiteral("ru");
};

// ---- tagging a widget with the catalog key it displays ----
//
// A widget that carries text states that text twice: once as the string it shows right now, and once as
// the key it came from, in a dynamic property. MainWindow::retranslateUi() walks those properties when
// the language moves, which is why adding a label needs no second list anywhere — and why a widget
// built after a language switch (any dialog, any strategy card) is already in the right language: the
// builders call tr() through these same helpers.
//
// Buttons and labels set their text; the other helpers cover the tooltip, the accessible name a
// screen reader reads for an icon-only control, a combo's entries, a spin box's suffix and a line
// edit's placeholder.

template <typename Widget>
inline Widget *i18nText(Widget *w, const QString &key)
{
    w->setProperty("i18nKey", key);
    if (auto *button = qobject_cast<QAbstractButton *>(w))
        button->setText(LanguageManager::tr(key));
    else if (auto *label = qobject_cast<QLabel *>(w))
        label->setText(LanguageManager::tr(key));
    return w;
}

// A menu action carries text the same way, but a QAction is not a QWidget.
QAction *i18nText(QAction *action, const QString &key);

template <typename Widget>
inline Widget *i18nTip(Widget *w, const QString &key)
{
    w->setProperty("i18nTip", key);
    w->setToolTip(LanguageManager::tr(key));
    return w;
}

template <typename Widget>
inline Widget *i18nAcc(Widget *w, const QString &key)
{
    w->setProperty("i18nAcc", key);
    w->setAccessibleName(LanguageManager::tr(key));
    return w;
}

// The entries of a combo box, in index order. The values behind them (itemData) are not text and stay.
template <typename Widget>
inline Widget *i18nItems(Widget *w, const QStringList &keys)
{
    w->setProperty("i18nItems", keys);
    return w;
}

template <typename Widget>
inline Widget *i18nSuffix(Widget *w, const QString &key)
{
    w->setProperty("i18nSuffix", key);
    return w;
}

template <typename Widget>
inline Widget *i18nPlaceholder(Widget *w, const QString &key)
{
    w->setProperty("i18nPlaceholder", key);
    return w;
}


} // namespace zapret
