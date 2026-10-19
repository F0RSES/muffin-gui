#include "LanguageManager.h"

#include <QAbstractButton>
#include <QAction>
#include <QDebug>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QWidget>
#include <QIcon>
#include <QVector>

namespace zapret {

namespace {

// ---- the catalog ------------------------------------------------------------
//
// Column order, everywhere below: ru, en, es, de, zh, fr, hi. One row per key is deliberate — there are
// no separate per-language lists a translation can be dropped from, and uitest asserts that every row
// is complete and that every key the UI asks for exists in the catalog.
//
// Text that is a design token rather than a sentence (the header's "CONNECTED", the footer's "Updated
// every 5s", the BAT/PowerShell kind tags) stays untranslated in the source's own words, the way the
// mock spells them.
constexpr int kLangCount = 7;

struct Row
{
    QString key;
    QString text[kLangCount];
};

#define ROW(K, RU, EN, ES, DE, ZH, FR, HI)                                  \
    Row { QStringLiteral(K), { QStringLiteral(RU), QStringLiteral(EN),      \
                               QStringLiteral(ES), QStringLiteral(DE),      \
                               QStringLiteral(ZH), QStringLiteral(FR),      \
                               QStringLiteral(HI) } }

const QStringList &orderedCodes()
{
    // Menu order = the order every row below is written in.
    static const QStringList codes = { QStringLiteral("ru"), QStringLiteral("en"),
                                       QStringLiteral("es"), QStringLiteral("de"),
                                       QStringLiteral("zh"), QStringLiteral("fr"),
                                       QStringLiteral("hi") };
    return codes;
}

// Each language's name, in that language. Not part of the translatable catalog: "Deutsch" must read as
// Deutsch inside a Russian UI too, or the picker fails the one person who needs it.
const QHash<QString, QString> &nativeNames()
{
    static const QHash<QString, QString> names = {
        { QStringLiteral("ru"), QStringLiteral("Русский") },
        { QStringLiteral("en"), QStringLiteral("English") },
        { QStringLiteral("es"), QStringLiteral("Español") },
        { QStringLiteral("de"), QStringLiteral("Deutsch") },
        { QStringLiteral("zh"), QStringLiteral("中文") },
        { QStringLiteral("fr"), QStringLiteral("Français") },
        { QStringLiteral("hi"), QStringLiteral("हिन्दी") },
    };
    return names;
}

const QVector<Row> &catalog()
{
    static const QVector<Row> rows = {

        // ---- rail / navigation ----
        ROW("nav.logs", "Логи", "Logs", "Registros", "Protokolle", "日志", "Journaux", "लॉग"),
        ROW("nav.profiles", "Профили", "Profiles", "Perfiles", "Profile", "档案",
            "Profils", "प्रोफ़ाइलें"),
        ROW("nav.settings", "Настройки", "Settings", "Ajustes", "Einstellungen", "设置",
            "Paramètres", "सेटिंग्स"),
        ROW("nav.settings_tip", "Настройки (Ctrl+S)", "Settings (Ctrl+S)", "Ajustes (Ctrl+S)",
            "Einstellungen (Strg+S)", "设置（Ctrl+S）", "Paramètres (Ctrl+S)", "सेटिंग्स (Ctrl+S)"),
        ROW("nav.language", "Язык интерфейса", "Interface language", "Idioma de la interfaz",
            "Sprache der Oberfläche", "界面语言", "Langue de l'interface", "इंटरफ़ेस भाषा"),

        // ---- header ----
        ROW("header.status_tip", "Состояние подключения", "Connection status",
            "Estado de la conexión", "Verbindungsstatus", "连接状态", "État de la connexion",
            "कनेक्शन स्थिति"),
        // The window has no system frame, so its two chrome controls are the app's own and have to
        // name themselves.
        ROW("window.minimize", "Свернуть", "Minimize", "Minimizar", "Minimieren", "最小化",
            "Réduire", "घटाएँ"),
        ROW("window.close", "Закрыть", "Close", "Cerrar", "Schließen", "关闭", "Fermer",
            "बंद करें"),

        // ---- pages: shared chrome ----
        ROW("page.back", "←  Muffin", "←  Muffin", "←  Muffin",
            "←  Muffin", "←  Muffin", "←  Muffin", "←  Muffin"),
        ROW("page.back_tip", "Вернуться на главный экран (Esc)",
            "Back to the main screen (Esc)", "Volver a la pantalla principal (Esc)",
            "Zurück zur Hauptansicht (Esc)", "返回主界面（Esc）",
            "Retour à l'écran principal (Échap)", "मुख्य स्क्रीन पर लौटें (Esc)"),
        ROW("common.ok", "OK", "OK", "Aceptar", "OK", "确定", "OK", "ठीक है"),
        ROW("common.cancel", "Отмена", "Cancel", "Cancelar", "Abbrechen", "取消", "Annuler",
            "रद्द करें"),
        ROW("common.or", "или", "or", "o", "oder", "或", "ou", "या"),

        // ---- strategies page ----
        ROW("action.refresh", "Обновить", "Refresh", "Actualizar", "Aktualisieren", "刷新",
            "Actualiser", "रीफ़्रेश करें"),
        ROW("action.refresh_tip", "Пересканировать папку zapret (Ctrl+R)",
            "Rescan the zapret folder (Ctrl+R)", "Volver a escanear la carpeta de zapret (Ctrl+R)",
            "Ordner zapret erneut scannen (Strg+R)", "重新扫描 zapret 文件夹（Ctrl+R）",
            "Rescanner le dossier zapret (Ctrl+R)", "zapret फ़ोल्डर फिर से स्कैन करें (Ctrl+R)"),
        ROW("action.download_zapret", "Скачать zapret", "Download zapret", "Descargar zapret",
            "zapret herunterladen", "下载 zapret", "Télécharger zapret", "zapret डाउनलोड करें"),
        ROW("action.download_zapret_tip", "Загрузить последнюю версию zapret с GitHub",
            "Fetch the latest zapret release from GitHub",
            "Obtener la última versión de zapret desde GitHub",
            "Die neueste zapret-Version von GitHub laden", "从 GitHub 获取最新 zapret 版本",
            "Récupérer la dernière version de zapret depuis GitHub",
            "GitHub से नवीनतम zapret संस्करण लाएँ"),
        ROW("action.ping_test", "Тест пинга", "Ping test", "Prueba de ping", "Ping-Test",
            "Ping 测试", "Test de ping", "पिंग परीक्षण"),
        // The same button while its round is in flight. Wording it as a state rather than as a disabled
        // control is what lets the row of cards answer the press visibly.
        ROW("action.ping_checking", "Проверка…", "Checking…", "Comprobando…", "Prüfung läuft…",
            "检测中…", "Vérification…", "जाँच हो रही है…"),
        ROW("action.ping_test_tip",
            "Измерить доступность сервисов прямо сейчас",
            "Measure the services' reachability right now",
            "Medir ahora mismo la disponibilidad de los servicios",
            "Die Erreichbarkeit der Dienste jetzt neu messen",
            "立即重新测量服务可达性",
            "Mesurer maintenant la joignabilité des services",
            "सेवाओं की पहुँच अभी दोबारा नापें"),

        // ---- the profile row's menu ----
        ROW("profile.copy", "Скопировать", "Copy", "Copiar", "Kopieren", "复制", "Copier",
            "कॉपी करें"),
        ROW("profile.delete", "Удалить", "Delete", "Eliminar", "Löschen", "删除", "Supprimer",
            "हटाएँ"),
        ROW("profile.menu_tip", "Действия с профилем", "Actions for this profile",
            "Acciones de este perfil", "Aktionen für dieses Profil", "此档案的操作",
            "Actions pour ce profil", "इस प्रोफ़ाइल की क्रियाएँ"),
        ROW("profile.copied", "Команда скопирована: %1", "Command copied: %1",
            "Comando copiado: %1", "Befehl kopiert: %1", "命令已复制：%1",
            "Commande copiée : %1", "कमांड कॉपी हुई: %1"),
        ROW("profile.delete_title", "Удалить профиль", "Delete profile", "Eliminar perfil",
            "Profil löschen", "删除档案", "Supprimer le profil", "प्रोफ़ाइल हटाएँ"),
        ROW("profile.delete_confirm",
            "Профиль — это файл в папке zapret. Удалить %1?",
            "A profile is a file in the zapret folder. Delete %1?",
            "Un perfil es un archivo de la carpeta de zapret. ¿Eliminar %1?",
            "Ein Profil ist eine Datei im zapret-Ordner. %1 löschen?",
            "档案就是 zapret 文件夹中的一个文件。删除 %1？",
            "Un profil est un fichier du dossier zapret. Supprimer %1 ?",
            "प्रोफ़ाइल zapret फ़ोल्डर की एक फ़ाइल है। %1 हटाएँ?"),
        ROW("profile.deleted", "Профиль удалён: %1", "Profile deleted: %1",
            "Perfil eliminado: %1", "Profil gelöscht: %1", "档案已删除：%1",
            "Profil supprimé : %1", "प्रोफ़ाइल हटाई गई: %1"),
        ROW("profile.delete_failed", "Не удалось удалить файл: %1", "Could not delete the file: %1",
            "No se pudo eliminar el archivo: %1", "Die Datei ließ sich nicht löschen: %1",
            "无法删除文件：%1", "Impossible de supprimer le fichier : %1",
            "फ़ाइल हटाई नहीं जा सकी: %1"),
        // ---- the profiles page ----
        ROW("placeholder.no_strategies",
            "Стратегии не найдены.\n\nСкачайте zapret сами — или укажите папку, которая уже есть на диске.",
            "No strategies found.\n\nDownload zapret yourself — or point to a folder that is already on this disk.",
            "No se encontraron estrategias.\n\nDescargue zapret usted mismo, o indique una carpeta que ya esté en el disco.",
            "Keine Strategien gefunden.\n\nLaden Sie zapret selbst herunter — oder wählen Sie einen Ordner, der bereits auf der Platte liegt.",
            "未找到策略。\n\n请自行下载 zapret，或指向磁盘上已有的文件夹。",
            "Aucune stratégie trouvée.\n\nTéléchargez zapret vous-même — ou indiquez un dossier déjà présent sur le disque.",
            "कोई रणनीति नहीं मिली।\n\nखुद zapret डाउनलोड करें — या डिस्क पर पहले से मौजूद फ़ोल्डर चुनें।"),

        // ---- the journal (LogPanel) ----
        ROW("log.copy", "Копировать", "Copy", "Copiar", "Kopieren", "复制", "Copier", "कॉपी करें"),
        ROW("log.copy_tip", "Скопировать весь лог в буфер обмена",
            "Copy the whole log to the clipboard", "Copiar todo el registro al portapapeles",
            "Das gesamte Protokoll in die Zwischenablage kopieren", "将完整日志复制到剪贴板",
            "Copier tout le journal dans le presse-papiers", "पूरा लॉग क्लिपबोर्ड पर कॉपी करें"),
        ROW("log.clear", "Очистить", "Clear", "Limpiar", "Leeren", "清空", "Effacer", "साफ़ करें"),
        ROW("log.clear_tip", "Очистить лог", "Clear the log", "Borrar el registro",
            "Protokoll löschen", "清空日志", "Effacer le journal", "लॉग साफ़ करें"),
        ROW("log.save", "Сохранить в файл", "Save to file", "Guardar en archivo",
            "In Datei speichern", "保存到文件", "Enregistrer dans un fichier", "फ़ाइल में सहेजें"),
        ROW("log.save_tip", "Сохранить лог в файл", "Save the log to a file",
            "Guardar el registro en un archivo", "Protokoll in eine Datei speichern",
            "将日志保存到文件", "Enregistrer le journal dans un fichier", "लॉग को फ़ाइल में सहेजें"),
        ROW("log.level", "Уровень", "Level", "Nivel", "Stufe", "级别", "Niveau", "स्तर"),
        ROW("log.filter.all", "Все", "All", "Todos", "Alle", "全部", "Tous", "सभी"),
        ROW("log.filter.info", "Инфо", "Info", "Info", "Info", "信息", "Info", "जानकारी"),
        ROW("log.filter.warn", "Предупреждение", "Warning", "Advertencia", "Warnung", "警告",
            "Avertissement", "चेतावनी"),
        ROW("log.filter.error", "Ошибка", "Error", "Error", "Fehler", "错误", "Erreur", "त्रुटि"),
        ROW("log.save_title", "Сохранить логи", "Save logs", "Guardar registros",
            "Protokolle speichern", "保存日志", "Enregistrer les journaux", "लॉग सहेजें"),
        ROW("log.save_filters", "Логи (*.log);;Текст (*.txt);;Все файлы (*)",
            "Logs (*.log);;Text (*.txt);;All files (*)",
            "Registros (*.log);;Texto (*.txt);;Todos los archivos (*)",
            "Protokolle (*.log);;Text (*.txt);;Alle Dateien (*)",
            "日志 (*.log);;文本 (*.txt);;所有文件 (*)",
            "Journaux (*.log);;Texte (*.txt);;Tous les fichiers (*)",
            "लॉग (*.log);; टेक्स्ट (*.txt);; सभी फ़ाइलें (*)"),
        ROW("log.save_failed_title", "Сохранение логов", "Saving logs", "Guardando registros",
            "Protokolle werden gespeichert", "正在保存日志", "Enregistrement des journaux",
            "लॉग सहेजे जा रहे हैं"),
        ROW("log.save_failed", "Не удалось записать файл: %1", "Could not write the file: %1",
            "No se pudo escribir el archivo: %1", "Die Datei konnte nicht geschrieben werden: %1",
            "无法写入文件：%1", "Impossible d'écrire le fichier : %1",
            "फ़ाइल लिखी नहीं जा सकी: %1"),

        // ---- strategy cards ----
        ROW("card.start", "Запустить", "Start", "Iniciar", "Starten", "启动", "Démarrer", "चलाएँ"),
        ROW("card.stop", "Остановить", "Stop", "Detener", "Stoppen", "停止", "Arrêter", "रोकें"),
        ROW("card.retry", "Повторить", "Retry", "Reintentar", "Wiederholen", "重试", "Réessayer",
            "पुनः प्रयास करें"),
        ROW("card.need_root", "нужен root", "root required", "se requiere root",
            "root erforderlich", "需要 root", "root requis", "root आवश्यक है"),
        ROW("card.need_admin", "нужен администратор", "administrator required",
            "se requiere administrador", "Administrator erforderlich", "需要管理员",
            "administrateur requis", "प्रशासक आवश्यक है"),
        ROW("card.kind.shell", "init-скрипт", "init script", "script init", "init-Skript",
            "init 脚本", "script init", "init स्क्रिप्ट"),
        ROW("card.kind.binary", "бинарник", "binary", "binario", "Binary", "二进制程序", "binaire",
            "बाइनरी"),
        ROW("card.desc.binary", "Прямой запуск %1", "Runs %1 directly", "Ejecuta %1 directamente",
            "Direktstart von %1", "直接运行 %1", "Exécution directe de %1", "%1 को सीधे चलाएँ"),
        ROW("card.desc.args", ", %1 арг.", ", %1 args", ", %1 arg.", ", %1 Argumente",
            "，%1 个参数", ", %1 arg.", ", %1 तर्क"),
        ROW("card.desc.batch", "Обёртка cmd.exe /c %1", "cmd.exe /c wrapper for %1",
            "Envoltorio cmd.exe /c para %1", "cmd.exe-/c-Wrapper für %1", "cmd.exe /c 封装：%1",
            "Enveloppe cmd.exe /c pour %1", "cmd.exe /c रैपर: %1"),
        ROW("card.desc.powershell", "Запуск через PowerShell: %1", "Runs through PowerShell: %1",
            "Ejecución mediante PowerShell: %1", "Start über PowerShell: %1",
            "通过 PowerShell 运行：%1", "Exécution via PowerShell : %1",
            "PowerShell से चलाएँ: %1"),
        ROW("card.desc.shell", "Init-скрипт zapret (%1 start/stop)",
            "zapret init script (%1 start/stop)", "Script init de zapret (%1 start/stop)",
            "zapret-Init-Skript (%1 start/stop)", "zapret init 脚本（%1 start/stop）",
            "Script init zapret (%1 start/stop)", "zapret init स्क्रिप्ट (%1 start/stop)"),
        ROW("card.desc.systemd", "Unit-файл systemd: %1", "systemd unit file: %1",
            "Archivo de unidad systemd: %1", "systemd-Unit-Datei: %1", "systemd 单元文件：%1",
            "Fichier d'unité systemd : %1", "systemd यूनिट फ़ाइल: %1"),
        ROW("card.menu.copy_command", "Скопировать команду", "Copy command", "Copiar el comando",
            "Befehl kopieren", "复制命令", "Copier la commande", "कमांड कॉपी करें"),
        ROW("card.menu.open_folder", "Открыть папку со стратегией", "Open the strategy's folder",
            "Abrir la carpeta de la estrategia", "Ordner der Strategie öffnen", "打开策略所在文件夹",
            "Ouvrir le dossier de la stratégie", "रणनीति का फ़ोल्डर खोलें"),

        // ---- backend status + action gating ----
        ROW("status.stopped", "Остановлено", "Stopped", "Detenido", "Gestoppt", "已停止", "Arrêté",
            "रोका गया"),
        ROW("status.starting", "Запуск: %1", "Starting: %1", "Iniciando: %1", "Startet: %1",
            "正在启动：%1", "Démarrage : %1", "चलाया जा रहा है: %1"),
        ROW("status.running", "Работает: %1", "Running: %1", "En ejecución: %1", "Läuft: %1",
            "运行中：%1", "En cours : %1", "चल रहा है: %1"),
        ROW("status.stopping", "Остановка…", "Stopping…", "Deteniendo…", "Stoppen…", "正在停止…",
            "Arrêt…", "रोका जा रहा है…"),
        ROW("status.error", "Ошибка", "Error", "Error", "Fehler", "错误", "Erreur", "त्रुटि"),
        ROW("gate.starting", "Идёт запуск — дождитесь его завершения.",
            "A start is in progress — wait for it to finish.",
            "Se está iniciando — espere a que termine.",
            "Ein Start läuft — bitte den Abschluss abwarten.", "正在启动—请等待其完成。",
            "Un démarrage est en cours — attendez sa fin.",
            "शुरू हो रहा है — पूरा होने की प्रतीक्षा करें।"),
        ROW("gate.stopping", "Идёт остановка — дождитесь её завершения.",
            "A stop is in progress — wait for it to finish.",
            "Se está deteniendo — espere a que termine.",
            "Ein Stopp läuft — bitte den Abschluss abwarten.", "正在停止—请等待其完成。",
            "Un arrêt est en cours — attendez sa fin.", "रोका जा रहा है — पूरा होने की प्रतीक्षा करें।"),
        ROW("gate.running", "Стратегия уже запущена — сначала остановите её.",
            "A strategy is already running — stop it first.",
            "Ya hay una estrategia en ejecución — deténgala primero.",
            "Es läuft bereits eine Strategie — stoppen Sie sie zuerst.", "已有策略在运行—请先停止它。",
            "Une stratégie est déjà en cours — arrêtez-la d'abord.",
            "पहले से एक रणनीति चल रही है — पहले उसे रोकें।"),
        ROW("hint.stop", "Остановить запущенную стратегию (Ctrl+Shift+X)",
            "Stop the running strategy (Ctrl+Shift+X)",
            "Detener la estrategia en ejecución (Ctrl+Shift+X)",
            "Laufende Strategie stoppen (Strg+Umschalt+X)", "停止正在运行的策略（Ctrl+Shift+X）",
            "Arrêter la stratégie en cours (Ctrl+Maj+X)",
            "चल रही रणनीति रोकें (Ctrl+Shift+X)"),
        ROW("hint.start", "Запустить выбранную стратегию (Ctrl+Shift+S)",
            "Start the chosen strategy (Ctrl+Shift+S)",
            "Iniciar la estrategia elegida (Ctrl+Shift+S)",
            "Gewählte Strategie starten (Strg+Umschalt+S)", "启动所选策略（Ctrl+Shift+S）",
            "Démarrer la stratégie choisie (Ctrl+Maj+S)", "चुनी रणनीति चलाएँ (Ctrl+Shift+S)"),

        // ---- tray + window ----
        ROW("tray.show", "Показать", "Show", "Mostrar", "Anzeigen", "显示", "Afficher", "दिखाएँ"),
        ROW("tray.start_last", "Запустить последнюю", "Start the last one", "Iniciar la última",
            "Zuletzt gestartet", "启动上一个", "Démarrer la dernière", "पिछली रणनीति चलाएँ"),
        ROW("tray.stop", "Остановить", "Stop", "Detener", "Stoppen", "停止", "Arrêter", "रोकें"),
        ROW("tray.quit", "Выход", "Quit", "Salir", "Beenden", "退出", "Quitter", "बाहर निकलें"),
        ROW("tray.minimized",
            "Окно свёрнуто в трей — zapret продолжает работать.\nДвойной клик по иконке открывает окно.",
            "The window is hidden in the tray — zapret keeps running.\nDouble-click the icon to open it again.",
            "La ventana está en la bandeja — zapret sigue funcionando.\nHaga doble clic en el icono para abrirla.",
            "Das Fenster liegt im Tray — zapret läuft weiter.\nDoppelklick auf das Symbol öffnet es wieder.",
            "窗口已缩到托盘—zapret 继续运行。\n双击图标即可重新打开窗口。",
            "La fenêtre est dans la barre système — zapret continue de tourner.\nDouble-cliquez sur l'icône pour l'ouvrir.",
            "विंडो ट्रे में छिपी है — zapret चलता रहेगा।\nखोलने के लिए आइकन पर डबल-क्लिक करें।"),
        ROW("zapret.not_found", "zapret не найден", "zapret not found", "no se encontró zapret",
            "zapret nicht gefunden", "未找到 zapret", "zapret introuvable", "zapret नहीं मिला"),





        // ---- MainWindow log lines + transient messages ----
        ROW("log.startup", "Muffin %1 запущен (бэкенд: %2)", "Muffin %1 started (backend: %2)",
            "Muffin %1 iniciado (backend: %2)", "Muffin %1 gestartet (Backend: %2)",
            "Muffin %1 已启动（后端：%2）", "Muffin %1 démarré (backend : %2)",
            "Muffin %1 शुरू हुआ (बैकएंड: %2)"),
        ROW("log.found_in", "Найдено стратегий: %1 в %2", "%1 strategies found in %2",
            "%1 estrategias encontradas en %2", "%1 Strategien in %2 gefunden",
            "在 %2 中找到 %1 个策略", "%1 stratégies trouvées dans %2", "%2 में %1 रणनीतियाँ मिलीं"),
        ROW("log.no_strategies", "Стратегии не найдены — укажите путь к zapret в настройках.",
            "No strategies found — set the zapret path in settings.",
            "No hay estrategias — indique la ruta de zapret en los ajustes.",
            "Keine Strategien gefunden — Pfad zu zapret in den Einstellungen setzen.",
            "未找到策略—请在设置中指定 zapret 路径。",
            "Aucune stratégie trouvée — indiquez le chemin de zapret dans les paramètres.",
            "कोई रणनीति नहीं मिली — सेटिंग्स में zapret पथ दें।"),
        ROW("log.tray_no_env",
            "Трей недоступен в этой среде: закрытие окна остановит zapret и выйдет из приложения.",
            "This desktop has no notification area: closing the window stops zapret and quits the application.",
            "Esta sesión no tiene área de notificación: cerrar la ventana detendrá zapret y saldrá de la aplicación.",
            "In dieser Umgebung gibt es keinen Benachrichtigungsbereich: Schließen stoppt zapret und beendet die Anwendung.",
            "此环境没有通知区域：关闭窗口会停止 zapret 并退出程序。",
            "Cet environnement n'a pas de zone de notification : fermer la fenêtre arrête zapret et quitte l'application.",
            "इस वातावरण में सूचना क्षेत्र नहीं है: विंडो बंद करने पर zapret रुकेगा और ऐप बंद होगा।"),
        ROW("msg.tray_unavailable", "Трей недоступен — закрытие окна завершит работу",
            "No tray here — closing the window will quit",
            "No hay bandeja: cerrar la ventana cerrará la aplicación",
            "Kein Tray: Schließen beendet die Anwendung", "无托盘—关闭窗口将退出程序",
            "Pas de zone de notification : fermer la fenêtre quittera l'application",
            "ट्रे नहीं है — विंडो बंद करने पर ऐप बंद होगा"),
        ROW("log.tray_available", "Трей доступен — сворачивание при закрытии включено.",
            "The tray is available — hide-on-close is on.",
            "La bandeja está disponible: ocultar al cerrar está activo.",
            "Tray verfügbar — Minimieren beim Schließen ist aktiv.", "托盘可用—关闭时缩到托盘已开启。",
            "La zone de notification est disponible : masquer à la fermeture est actif.",
            "ट्रे उपलब्ध है — बंद करते समय छिपाना चालू है।"),
        ROW("log.rescanned", "Пересканировано: %1 стратегий в %2", "Rescanned: %1 strategies in %2",
            "Reescaneado: %1 estrategias en %2", "Erneut gescannt: %1 Strategien in %2",
            "重新扫描：%2 中 %1 个策略", "Rescan effectué : %1 stratégies dans %2",
            "फिर से स्कैन: %2 में %1 रणनीतियाँ"),
        ROW("log.no_dir", "(не найдено)", "(not found)", "(no encontrada)", "(nicht gefunden)",
            "（未找到）", "(introuvable)", "(नहीं मिला)"),
        ROW("log.empty_dir", "В этой папке нет стратегий — проверьте путь в настройках.",
            "This folder holds no strategies — check the path in settings.",
            "Esta carpeta no tiene estrategias — revise la ruta en los ajustes.",
            "Dieser Ordner enthält keine Strategien — Pfad in den Einstellungen prüfen.",
            "此文件夹中没有策略—请检查设置中的路径。",
            "Ce dossier ne contient aucune stratégie — vérifiez le chemin dans les paramètres.",
            "इस फ़ोल्डर में कोई रणनीति नहीं — सेटिंग्स में पथ जाँचें।"),
        ROW("log.installed", "Установлен zapret: %1", "zapret installed: %1",
            "zapret instalado en: %1", "zapret installiert: %1", "已安装 zapret：%1",
            "zapret installé : %1", "zapret इंस्टॉल किया गया: %1"),
        ROW("log.start_requested", "Запрошен запуск: %1", "Start requested: %1",
            "Inicio solicitado: %1", "Start angefragt: %1", "已请求启动：%1",
            "Démarrage demandé : %1", "शुरू करने का अनुरोध: %1"),
        ROW("log.strategy_stopped", "Стратегия остановлена", "Strategy stopped",
            "Estrategia detenida", "Strategie gestoppt", "策略已停止", "Stratégie arrêtée",
            "रणनीति रोक दी गई"),
        ROW("log.close_stops",
            "Трей недоступен: закрытие окна останавливает zapret и завершает приложение.",
            "No tray available: closing the window stops zapret and ends the application.",
            "Sin bandeja: cerrar la ventana detiene zapret y cierra la aplicación.",
            "Kein Tray verfügbar: Schließen stoppt zapret und beendet die Anwendung.",
            "没有可用托盘：关闭窗口会停止 zapret 并结束应用。",
            "Aucune zone de notification : fermer la fenêtre arrête zapret et termine l'application.",
            "ट्रे उपलब्ध नहीं: विंडो बंद करने पर zapret और ऐप दोनों बंद होंगे।"),
        ROW("msg.no_saved_strategy", "Нет сохранённой стратегии для запуска",
            "There is no saved strategy to start", "No hay una estrategia guardada que iniciar",
            "Keine gespeicherte Strategie zum Starten vorhanden", "没有可启动的已保存策略",
            "Aucune stratégie enregistrée à démarrer", "चलाने के लिए कोई सहेजी रणनीति नहीं"),
        ROW("msg.pick_profile", "Выберите профиль, затем нажмите кнопку",
            "Pick a profile, then press the button", "Elija un perfil y pulse el botón",
            "Bitte ein Profil wählen und dann die Taste drücken", "请先选择档案，再按按钮",
            "Choisissez un profil puis appuyez sur le bouton", "पहले प्रोफ़ाइल चुनें, फिर बटन दबाएँ"),
        ROW("msg.found_count", "Найдено стратегий: %1", "Strategies found: %1",
            "Estrategias encontradas: %1", "Gefundene Strategien: %1", "找到策略：%1",
            "Stratégies trouvées : %1", "मिली रणनीतियाँ: %1"),
        ROW("error.strategy_not_found", "Стратегия не найдена: %1", "Strategy not found: %1",
            "Estrategia no encontrada: %1", "Strategie nicht gefunden: %1", "未找到策略：%1",
            "Stratégie introuvable : %1", "रणनीति नहीं मिली: %1"),
        ROW("error.backend_refused", "Бэкенд отклонил запуск.", "The backend refused the start.",
            "El backend rechazó el inicio.", "Das Backend hat den Start abgelehnt.",
            "后端拒绝了本次启动。", "Le backend a refusé le démarrage.",
            "बैकएंड ने शुरू करने से इनकार किया।"),

        // ---- settings dialog ----
        ROW("settings.title", "Настройки Muffin", "Muffin settings", "Ajustes de Muffin",
            "Muffin-Einstellungen", "Muffin 设置", "Paramètres de Muffin", "Muffin सेटिंग्स"),
        ROW("settings.path", "Путь к zapret", "zapret path", "Ruta de zapret", "Pfad zu zapret",
            "zapret 路径", "Chemin de zapret", "zapret पथ"),
        ROW("settings.browse", "Обзор…", "Browse…", "Examinar…", "Durchsuchen…", "浏览…",
            "Parcourir…", "ब्राउज़ करें…"),
        ROW("settings.autodetect", "Автопоиск", "Auto-detect", "Búsqueda automática",
            "Automatisch suchen", "自动搜索", "Recherche auto", "स्वतः खोज"),
        ROW("settings.autodetect_tip",
            "Найти дерево zapret в $ZAPRET_ROOT, ~/Documents/zapret, /opt/zapret",
            "Find a zapret tree in $ZAPRET_ROOT, ~/Documents/zapret, /opt/zapret",
            "Buscar un árbol de zapret en $ZAPRET_ROOT, ~/Documents/zapret, /opt/zapret",
            "zapret-Baum in $ZAPRET_ROOT, ~/Documents/zapret, /opt/zapret finden",
            "在 $ZAPRET_ROOT、~/Documents/zapret、/opt/zapret 中查找 zapret 目录树",
            "Chercher une arborescence zapret dans $ZAPRET_ROOT, ~/Documents/zapret, /opt/zapret",
            "$ZAPRET_ROOT, ~/Documents/zapret, /opt/zapret में zapret ट्री खोजें"),
        ROW("settings.download", "Скачать…", "Download…", "Descargar…", "Herunterladen…",
            "下载…", "Télécharger…", "डाउनलोड करें…"),
        ROW("settings.download_tip",
            "Загрузить последнюю версию zapret с GitHub в папку приложения",
            "Fetch the latest zapret release from GitHub into the application folder",
            "Obtener la última versión de zapret desde GitHub y guardarla en la carpeta de la aplicación",
            "Die neueste zapret-Version von GitHub in den Anwendungsordner laden",
            "从 GitHub 获取最新 zapret 版本并存入应用文件夹",
            "Récupérer la dernière version de zapret depuis GitHub dans le dossier de l'application",
            "GitHub से नवीनतम zapret संस्करण ऐप फ़ोल्डर में लाएँ"),
        ROW("settings.start_last", "Запускать последнюю стратегию при старте",
            "Start the last strategy on launch", "Iniciar la última estrategia al arrancar",
            "Zuletzt verwendete Strategie beim Start ausführen", "启动时运行上次使用的策略",
            "Démarrer la dernière stratégie au lancement", "लॉन्च पर पिछली रणनीति चलाएँ"),
        ROW("settings.tray", "Сворачивать в трей при закрытии", "Hide to tray on close",
            "Ocultar en la bandeja al cerrar", "Beim Schließen in den Tray minimieren",
            "关闭窗口时缩到托盘", "Masquer dans la barre système à la fermeture",
            "बंद करते समय ट्रे में छिपाएँ"),
        ROW("settings.tray_note",
            "В этой среде нет области уведомлений: закрытие окна остановит zapret и выйдет из приложения.\n"
            "Настройка сохранена — если панель запустится позже, трей появится сам.",
            "This desktop has no notification area: closing the window stops zapret and quits the application.\n"
            "The setting is saved — if the panel starts later, the tray appears on its own.",
            "Esta sesión no tiene área de notificación: cerrar la ventana detendrá zapret y saldrá.\n"
            "El ajuste queda guardado: si el panel arranca más tarde, la bandeja aparecerá sola.",
            "In dieser Umgebung gibt es keinen Benachrichtigungsbereich: Schließen stoppt zapret und beendet die Anwendung.\n"
            "Die Einstellung ist gespeichert — startet die Leiste später, erscheint der Tray von allein.",
            "此环境没有通知区域：关闭窗口会停止 zapret 并退出程序。\n设置已保存——若面板稍后启动，托盘会自动出现。",
            "Cet environnement n'a pas de zone de notification : fermer la fenêtre arrête zapret et quitte l'application.\n"
            "Le réglage est enregistré — si le panneau démarre plus tard, la zone de notification apparaîtra.",
            "इस वातावरण में सूचना क्षेत्र नहीं है: विंडो बंद करने पर zapret रुकेगा और ऐप बंद होगा।\n"
            "सेटिंग सहेजी गई — पैनल बाद में चालू हुआ तो ट्रे खुद दिख जाएगा।"),
        ROW("settings.theme", "Тема", "Theme", "Tema", "Thema", "主题", "Thème", "थीम"),
        ROW("theme.dark", "Тёмная", "Dark", "Oscura", "Dunkel", "深色", "Sombre", "डार्क"),
        ROW("theme.light", "Светлая", "Light", "Clara", "Hell", "浅色", "Clair", "लाइट"),
        ROW("theme.system", "Системная", "System", "Del sistema", "System", "跟随系统",
            "Système", "सिस्टम"),
        ROW("settings.ping", "Интервал пинга", "Ping interval", "Intervalo de ping",
            "Ping-Intervall", "Ping 间隔", "Intervalle de ping", "पिंग अंतराल"),
        ROW("settings.ping_suffix", " сек", " s", " s", " s", " 秒", " s", " सेकंड"),
        ROW("settings.save", "Сохранить", "Save", "Guardar", "Speichern", "保存", "Enregistrer",
            "सहेजें"),
        // The acknowledgement the Save button puts beside itself for two seconds. The tick is the whole
        // sentence: a page has no frame to close, so "it worked" has to be said where the press was.
        ROW("settings.saved", "Сохранено ✓", "Saved ✓", "Guardado ✓", "Gespeichert ✓",
            "已保存 ✓", "Enregistré ✓", "सहेजा गया ✓"),
        ROW("path.none", "Путь не выбран.", "No path selected.", "Sin ruta seleccionada.",
            "Kein Pfad gewählt.", "未选择路径。", "Aucun chemin sélectionné.",
            "कोई पथ नहीं चुना गया।"),
        ROW("path.found", "Найдено: %1, стратегий: %2", "Found: %1, strategies: %2",
            "Encontrado: %1, estrategias: %2", "Gefunden: %1, Strategien: %2",
            "找到：%1，策略数：%2", "Trouvé : %1, stratégies : %2", "मिला: %1, रणनीतियाँ: %2"),
        ROW("path.base", "Базовая папка: найдены %1", "Base folder: found %1",
            "Carpeta base: encontrados %1", "Basisordner: %1 gefunden", "基础文件夹：找到 %1",
            "Dossier de base : %1 trouvés", "बेस फ़ोल्डर: %1 मिले"),
        ROW("path.not_zapret",
            "Это не дерево zapret: нет ни bin/winws.exe + *.bat, ни init.d/nfqws.",
            "This is not a zapret tree: neither bin/winws.exe + *.bat nor init.d/nfqws.",
            "No es un árbol de zapret: ni bin/winws.exe + *.bat ni init.d/nfqws.",
            "Das ist kein zapret-Baum: weder bin/winws.exe + *.bat noch init.d/nfqws.",
            "这不是 zapret 目录树：既没有 bin/winws.exe + *.bat，也没有 init.d/nfqws。",
            "Ce n'est pas une arborescence zapret : ni bin/winws.exe + *.bat, ni init.d/nfqws.",
            "यह zapret ट्री नहीं है: न bin/winws.exe + *.bat, न init.d/nfqws।"),
        ROW("path.not_found_manual", "Zapret не найден — укажите путь вручную.",
            "Zapret not found — choose the path manually.",
            "Zapret no encontrado — indique la ruta manualmente.",
            "Zapret nicht gefunden — Pfad manuell wählen.", "未找到 Zapret—请手动指定路径。",
            "Zapret introuvable — indiquez le chemin manuellement.",
            "Zapret नहीं मिला — पथ स्वयं चुनें।"),
        ROW("settings.folder_title", "Папка с zapret", "Folder containing zapret",
            "Carpeta con zapret", "Ordner mit zapret", "包含 zapret 的文件夹",
            "Dossier contenant zapret", "zapret वाला फ़ोल्डर"),

        // ---- download dialog ----
        ROW("dl.info", "Сведения о релизе появятся после проверки GitHub.",
            "Release details appear once GitHub has been checked.",
            "Los datos del lanzamiento aparecerán tras consultar GitHub.",
            "Release-Details erscheinen nach der Abfrage bei GitHub.",
            "检查 GitHub 后会显示版本信息。",
            "Les informations de la version s'afficheront après vérification sur GitHub.",
            "GitHub जाँच के बाद रिलीज़ जानकारी दिखेगी।"),
        ROW("dl.waiting", "Ожидание.", "Waiting.", "En espera.", "Warten.", "等待中。",
            "En attente.", "प्रतीक्षा।"),
        ROW("dl.checking", "Проверяю последнюю версию на GitHub…",
            "Checking GitHub for the latest version…", "Consultando la última versión en GitHub…",
            "Neueste Version auf GitHub wird geprüft…", "正在 GitHub 上检查最新版本…",
            "Vérification de la dernière version sur GitHub…",
            "GitHub पर नवीनतम संस्करण जाँचा जा रहा है…"),
        ROW("dl.ready", "Готово к скачиванию.", "Ready to download.", "Listo para descargar.",
            "Bereit zum Herunterladen.", "可以下载了。", "Prêt à télécharger.",
            "डाउनलोड के लिए तैयार।"),
        ROW("dl.downloading", "Скачиваю…", "Downloading…", "Descargando…",
            "Wird heruntergeladen…", "正在下载…", "Téléchargement…", "डाउनलोड हो रहा है…"),
        ROW("dl.downloading_short", "Скачано %1…", "Downloaded %1…", "Descargados %1…",
            "%1 geladen…", "已下载 %1…", "Téléchargé %1…", "%1 डाउनलोड हुआ…"),
        ROW("dl.downloading_of", "Скачано %1 из %2…", "Downloaded %1 of %2…",
            "Descargados %1 de %2…", "%1 von %2 geladen…", "已下载 %2 中的 %1…",
            "Téléchargé %1 sur %2…", "%2 में से %1 डाउनलोड हुआ…"),
        ROW("dl.sha_ok", "SHA-256 совпал. Распаковываю…", "SHA-256 matched. Extracting…",
            "SHA-256 coincide. Extrayendo…", "SHA-256 passt. Entpacken…", "SHA-256 校验通过，正在解压…",
            "SHA-256 vérifié. Extraction…", "SHA-256 मेल खाया, निकाला जा रहा है…"),
        ROW("dl.installed_in", "Установлено в %1", "Installed in %1", "Instalado en %1",
            "Installiert in %1", "已安装到 %1", "Installé dans %1", "%1 में इंस्टॉल किया गया"),
        ROW("dl.cancelled", "Отменено.", "Cancelled.", "Cancelado.", "Abgebrochen.", "已取消。",
            "Annulé.", "रद्द किया गया।"),
        ROW("dl.cancelling", "Отменяю…", "Cancelling…", "Cancelando…", "Abbruch läuft…",
            "正在取消…", "Annulation…", "रद्द किया जा रहा है…"),
        ROW("dl.download_button", "Скачать", "Download", "Descargar", "Herunterladen", "下载",
            "Télécharger", "डाउनलोड करें"),
        ROW("dl.release", "Релиз %1\nФайл: %2, %3\nУстановить в: %4",
            "Release %1\nFile: %2, %3\nInstall into: %4",
            "Lanzamiento %1\nArchivo: %2, %3\nInstalar en: %4",
            "Release %1\nDatei: %2, %3\nInstallieren nach: %4", "版本 %1\n文件：%2，%3\n安装到：%4",
            "Version %1\nFichier : %2, %3\nInstaller dans : %4", "रिलीज़ %1\nफ़ाइल: %2, %3\nयहाँ इंस्टॉल करें: %4"),
        ROW("dl.unnamed", "(без имени)", "(unnamed)", "(sin nombre)", "(namenlos)", "（未命名）",
            "(sans nom)", "(बिना नाम)"),
        ROW("dl.no_sha", "\nSHA-256 для этого файла не опубликован — сверка невозможна.",
            "\nNo SHA-256 is published for this file — it cannot be verified.",
            "\nNo se ha publicado SHA-256 para este archivo: no se puede verificar.",
            "\nFür diese Datei ist kein SHA-256 veröffentlicht — Vergleich nicht möglich.",
            "\n此文件未公布 SHA-256，无法核对。",
            "\nAucun SHA-256 publié pour ce fichier — vérification impossible.",
            "\nइस फ़ाइल का SHA-256 प्रकाशित नहीं है — जाँच संभव नहीं।"),
        ROW("dl.sha_will_check", "\nSHA-256 будет сверен с опубликованным.",
            "\nThe SHA-256 will be checked against the published one.",
            "\nEl SHA-256 se comparará con el publicado.",
            "\nDer SHA-256 wird mit dem veröffentlichten abgeglichen.", "\nSHA-256 将与已公布值核对。",
            "\nLe SHA-256 sera comparé à celui publié.", "\nSHA-256 की जाँच प्रकाशित मान से होगी।"),
        ROW("dl.version_failed", "Не удалось узнать последнюю версию:\n\n%1",
            "Could not determine the latest version:\n\n%1",
            "No se pudo obtener la última versión:\n\n%1",
            "Neueste Version konnte nicht ermittelt werden:\n\n%1", "无法获取最新版本：\n\n%1",
            "Impossible d'obtenir la dernière version :\n\n%1", "नवीनतम संस्करण ज्ञात नहीं हुआ:\n\n%1"),
        ROW("dl.failed", "Скачивание не удалось:\n\n%1", "The download failed:\n\n%1",
            "La descarga falló:\n\n%1", "Herunterladen fehlgeschlagen:\n\n%1",
            "下载失败：\n\n%1", "Le téléchargement a échoué :\n\n%1", "डाउनलोड विफल:\n\n%1"),
        ROW("dl.installed_msg", "zapret установлен в:\n%1", "zapret was installed in:\n%1",
            "zapret se instaló en:\n%1", "zapret wurde installiert in:\n%1",
            "zapret 已安装到：\n%1", "zapret a été installé dans :\n%1",
            "zapret यहाँ इंस्टॉल हुआ:\n%1"),
        ROW("dl.root_note",
            "\n\nСистемная копия принадлежит root, поэтому запуск стратегий не будет требовать пароль.",
            "\n\nThe system copy belongs to root, so starting a strategy will not ask for a password.",
            "\n\nLa copia del sistema pertenece a root, así que iniciar estrategias no pedirá contraseña.",
            "\n\nDie Systemkopie gehört root, daher fragt der Start einer Strategie kein Passwort mehr ab.",
            "\n\n系统副本属于 root，因此启动策略不会再询问密码。",
            "\n\nLa copie système appartient à root : lancer une stratégie ne demandera plus de mot de passe.",
            "\n\nसिस्टम कॉपी root की है, इसलिए रणनीति चलाते समय पासवर्ड नहीं माँगा जाएगा।"),
        ROW("dl.replace_question",
            "В %1 уже есть файлы (%2).\n\n"
            "Скачивание установит туда новую копию zapret; старые файлы будут удалены после того, как "
            "проверка подтвердит новое дерево.\n\nПродолжить?",
            "%1 already holds files (%2).\n\n"
            "The download will put a new copy of zapret there; the old files are removed once the new "
            "tree has been verified.\n\nContinue?",
            "%1 ya contiene archivos (%2).\n\n"
            "La descarga instalará allí una copia nueva de zapret; los archivos viejos se borrarán cuando "
            "la verificación confirme el árbol nuevo.\n\n¿Continuar?",
            "In %1 liegen bereits Dateien (%2).\n\n"
            "Der Download installiert dort eine neue Kopie von zapret; die alten Dateien werden entfernt, "
            "sobald die Prüfung den neuen Baum bestätigt hat.\n\nFortfahren?",
            "%1 中已有文件（%2）。\n\n下载会在此安装新的 zapret 副本；校验确认新目录树后，旧文件将被删除。\n\n是否继续？",
            "%1 contient déjà des fichiers (%2).\n\n"
            "Le téléchargement y installera une nouvelle copie de zapret ; les anciens fichiers seront "
            "supprimés une fois le nouvel arbre vérifié.\n\nContinuer ?",
            "%1 में पहले से फ़ाइलें हैं (%2)。\n\n"
            "डाउनलोड वहाँ zapret की नई कॉपी इंस्टॉल करेगा; नई ट्री की जाँच होने पर पुरानी फ़ाइलें हटा दी "
            "जाएँगी।\n\nजारी रखें?"),
        ROW("dl.no_data_dir",
            "Эта система не даёт доступный каталог данных приложения, куда можно установить zapret.",
            "This system provides no accessible application data directory to install zapret into.",
            "Este sistema no ofrece un directorio de datos de la aplicación accesible donde instalar zapret.",
            "Dieses System stellt keinen zugänglichen Anwendungsdaten-Ordner bereit, in den zapret installiert werden könnte.",
            "此系统没有可用于安装 zapret 的应用数据目录。",
            "Ce système ne fournit aucun dossier de données d'application accessible où installer zapret.",
            "इस सिस्टम में zapret इंस्टॉल करने के लिए कोई सुलभ ऐप डेटा डायरेक्टरी नहीं है।"),

        // ---- sizes, ping chips ----
        ROW("size.unknown", "неизвестно", "unknown", "desconocido", "unbekannt", "未知",
            "inconnu", "अज्ञात"),
        ROW("size.bytes", "%1 Б", "%1 B", "%1 B", "%1 B", "%1 B", "%1 o", "%1 B"),
        ROW("size.kib", "%1 КиБ", "%1 KiB", "%1 KiB", "%1 KiB", "%1 KiB", "%1 Kio", "%1 KiB"),
        ROW("size.mib", "%1 МиБ", "%1 MiB", "%1 MiB", "%1 MiB", "%1 MiB", "%1 Mio", "%1 MiB"),
        ROW("size.gib", "%1 ГиБ", "%1 GiB", "%1 GiB", "%1 GiB", "%1 GiB", "%1 Gio", "%1 GiB"),
        ROW("ping.ms", "%1 мс", "%1 ms", "%1 ms", "%1 ms", "%1 ms", "%1 ms", "%1 ms"),
        ROW("ping.timeout", "таймаут", "timeout", "tiempo de espera agotado", "Zeitüberschreitung",
            "超时", "expiration du délai", "समय-आउट"),

    };
    return rows;
}

// lang -> (key -> text), built once from the rows above.
const QHash<QString, QHash<QString, QString>> &tables()
{
    static const QHash<QString, QHash<QString, QString>> built = [] {
        QHash<QString, QHash<QString, QString>> byLang;
        for (const QString &code : orderedCodes())
            byLang.insert(code, QHash<QString, QString>());
        for (const Row &r : catalog()) {
            for (int i = 0; i < kLangCount; ++i)
                byLang[orderedCodes().at(i)].insert(r.key, r.text[i]);
        }
        return byLang;
    }();
    return built;
}

} // namespace

LanguageManager &LanguageManager::instance()
{
    static LanguageManager manager;
    return manager;
}

LanguageManager::LanguageManager(QObject *parent)
    : QObject(parent)
{
}

QAction *i18nText(QAction *action, const QString &key)
{
    action->setProperty("i18nKey", key);
    action->setText(LanguageManager::tr(key));
    return action;
}

int retranslateTree(QObject *root)
{
    int rewritten = 0;
    if (!root)
        return 0;

    const auto widgets = root->findChildren<QWidget *>();
    for (QWidget *w : widgets) {
        const QString key = w->property("i18nKey").toString();
        if (!key.isEmpty()) {
            if (auto *button = qobject_cast<QAbstractButton *>(w)) {
                button->setText(LanguageManager::tr(key));
                ++rewritten;
            } else if (auto *label = qobject_cast<QLabel *>(w)) {
                label->setText(LanguageManager::tr(key));
                ++rewritten;
            }
        }
        const QString tip = w->property("i18nTip").toString();
        if (!tip.isEmpty()) {
            w->setToolTip(LanguageManager::tr(tip));
            ++rewritten;
        }
        const QString acc = w->property("i18nAcc").toString();
        if (!acc.isEmpty()) {
            w->setAccessibleName(LanguageManager::tr(acc));
            ++rewritten;
        }

        // A combo's entries carry their data (a level int, a theme name) independently of their text, so
        // the labels can be rewritten in place: indices and the current selection survive.
        if (auto *combo = qobject_cast<QComboBox *>(w)) {
            const QStringList keys = w->property("i18nItems").toStringList();
            for (int i = 0; i < keys.size() && i < combo->count(); ++i) {
                combo->setItemText(i, LanguageManager::tr(keys.at(i)));
                ++rewritten;
            }
        }
        if (auto *spin = qobject_cast<QSpinBox *>(w)) {
            const QString suffixKey = w->property("i18nSuffix").toString();
            if (!suffixKey.isEmpty()) {
                spin->setSuffix(LanguageManager::tr(suffixKey));
                ++rewritten;
            }
        }
        if (auto *edit = qobject_cast<QLineEdit *>(w)) {
            const QString ph = w->property("i18nPlaceholder").toString();
            if (!ph.isEmpty()) {
                edit->setPlaceholderText(LanguageManager::tr(ph));
                ++rewritten;
            }
        }
    }

    const auto actions = root->findChildren<QAction *>();
    for (QAction *action : actions) {
        const QString key = action->property("i18nKey").toString();
        if (!key.isEmpty()) {
            action->setText(LanguageManager::tr(key));
            ++rewritten;
        }
    }
    return rewritten;
}

const QStringList &LanguageManager::codes()
{
    return orderedCodes();
}

bool LanguageManager::isSupported(const QString &code)
{
    return orderedCodes().contains(code);
}

QString LanguageManager::nativeName(const QString &code)
{
    return nativeNames().value(code, code);
}

QIcon LanguageManager::flagIcon(const QString &code)
{
    // Flags are colour artwork, not currentColor glyphs: they are loaded straight from the resource so
    // both themes get the same recognisable stripes. Theme::icon() would flatten them to one tint.
    return QIcon(QStringLiteral(":/icons/flags/%1.svg").arg(code));
}

QString LanguageManager::currentCode() const
{
    return m_code;
}

void LanguageManager::setCode(const QString &code)
{
    if (!isSupported(code)) {
        qWarning("LanguageManager: unknown language '%s' — keeping '%s'",
                 qUtf8Printable(code), qUtf8Printable(m_code));
        return;
    }
    if (code == m_code)
        return;
    m_code = code;
    emit languageChanged(m_code);
}

QString LanguageManager::translate(const QString &language, const QString &key)
{
    const QHash<QString, QString> &table = tables().value(language);
    const QString found = table.value(key);
    if (!found.isEmpty())
        return found;
    // English is the pivot language of the catalog: a row added for one language only still says
    // something readable everywhere else, and uitest is what catches that it should not stay that way.
    return tables().value(QStringLiteral("en")).value(key, key);
}

QString LanguageManager::tr(const QString &key)
{
    return translate(instance().currentCode(), key);
}

QString LanguageManager::tr(const QString &key, const QString &arg1)
{
    return translate(instance().currentCode(), key).replace(QStringLiteral("%1"), arg1);
}

QStringList LanguageManager::keys()
{
    QStringList out;
    out.reserve(catalog().size());
    for (const Row &r : catalog())
        out << r.key;
    out.sort();
    out.removeDuplicates();
    return out;
}

} // namespace zapret
