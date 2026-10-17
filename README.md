# Muffin
Cross-platform Qt6 GUI for zapret (Windows winws + Linux nfqws).

[![CI](https://github.com/F0RSES/muffin-gui/actions/workflows/ci.yml/badge.svg)](https://github.com/F0RSES/muffin-gui/actions/workflows/ci.yml)

## Что это
Графическая оболочка для запуска и управления стратегиями zapret.
Тёмная/светлая тема, карточки стратегий, live-логи, ping-индикаторы,
системный трей.

## Зависимости
- Qt 6.6+ (Widgets, Network, Test; без Concurrent — модуль не используется)
- CMake 3.21+
- Windows: MSVC 2022 (Visual Studio 17) — проверено в CI
- Linux: GCC 11+ или Clang 15+ — проверено в CI (ubuntu-22.04, GCC 11)

Протестировано на Qt 6.7.3 (CI: Ubuntu 22.04 и Windows Server 2022)
и Qt 6.10.2 (локальная разработка на Kali Linux).

## Сборка
    ./scripts/build.sh
    ctest --output-on-failure

## Источники zapret
- Windows: https://github.com/Flowseal/zapret-discord-youtube
- Linux:   https://github.com/bol-van/zapret

Кнопка «Скачать zapret» (тулбар, настройки, экран «стратегии не найдены») запрашивает последний
релиз у GitHub, сверяет опубликованный SHA-256, распаковывает архив системным средством (`tar`/`tar.exe`,
`unzip` как запасной вариант — сторонних библиотек нет) и устанавливает дерево в
`<AppDataLocation>/zapret`, то есть на Linux в `~/.local/share/Muffin/zapret`, на Windows в
`%APPDATA%\Muffin\Muffin\zapret`. Эта папка добавлена в автопоиск, поэтому она будет найдена после
перезапуска GUI даже при пустых настройках.

Существующая папка заменяется только после проверки новой: при несовпадении хеша, обрыве скачивания,
отсутствии архиватора или неожиданной структуре релиза прежняя установка остаётся на месте. `.rar`
из релиза не берётся — его не умеет распаковать ни один архиватор, входящий в ОС.

## Лицензия
MIT (см. LICENSE)
