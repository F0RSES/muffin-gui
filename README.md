# Muffin
Cross-platform Qt6 GUI for zapret (Windows winws + Linux nfqws).

[![CI](https://github.com/F0RSES/muffin-gui/actions/workflows/ci.yml/badge.svg)](https://github.com/F0RSES/muffin-gui/actions/workflows/ci.yml)

## Что это
Графическая оболочка для запуска и управления стратегиями zapret.
Тёмная/светлая тема, карточки стратегий, live-логи, ping-индикаторы,
системный трей.

## Зависимости
- Qt 6.7+ (Widgets, Network, Concurrent)
- CMake 3.24+
- Windows: MSVC 2022 или MinGW
- Linux: GCC 13+ / Clang 15+, libqt6-base-dev

## Сборка
    ./scripts/build.sh
    ctest --output-on-failure

## Источники zapret
- Windows: https://github.com/Flowseal/zapret-discord-youtube
- Linux:   https://github.com/bol-van/zapret

## Лицензия
MIT (см. LICENSE)
