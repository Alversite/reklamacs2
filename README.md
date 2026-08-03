# Reklama (CS2)

Metamod:Source плагин для Counter-Strike 2, рассылающий рекламные сообщения в
**чат** и по **центру экрана** по таймеру. Переписан с нуля под актуальный
`hl2sdk-cs2` + Metamod:Source, чтобы работать на свежих версиях игры.

Конфиг — **тот же самый** `settings.ini` (формат Valve KeyValues), что и в
оригинальном плагине. Менять его не нужно.

## Возможности

- `ChatInterval` — период (сек) между сообщениями в чат.
- `CenterInterval` — период (сек) между сообщениями по центру.
- `ChatMessages` — группы сообщений; каждая группа = несколько строк, каждая
  строка отправляется отдельной строкой чата. Группы идут по кругу.
- `CenterMessages` — сообщения по центру экрана, по кругу.
- Цветовые теги в чате (`{LIGHTRED}`, `{WHITE}`, `{LIGHTPURPLE}` и т.д.)
  переводятся в цветовые управляющие байты CS2.
- Команда сервера `mm_reklama_reload` — перечитать конфиг без перезапуска.

Сообщения отправляются через сетевую подсистему движка
(`CUserMessageTextMsg` → `IGameEventSystem::PostEventAbstract`), **без
хардкод-сигнатур**, поэтому плагин переживает обновления CS2.

## Установка

1. Убедитесь, что установлен Metamod:Source для CS2.
2. Скопируйте на сервер (в `game/csgo/`):
   - `addons/Reklama/Reklama.so`
   - `addons/metamod/Reklama.vdf`
   - `addons/configs/Reklama/settings.ini` (или оставьте свой существующий)
3. Перезапустите сервер (или `meta load addons/Reklama/Reklama`).

Готовую сборку берите из вкладки **Actions** этого репозитория (артефакт
`Reklama-linux` или `Reklama-so`).

## Сборка

Собирается в CI (GitHub Actions, контейнер SteamRT sniper). Локально:

```bash
export HL2SDKCS2=/path/to/hl2sdk-cs2
export MMSOURCE_DEV=/path/to/metamod-source
export HL2SDKMANIFESTS=/path/to/hl2sdk-manifests
mkdir build && cd build
python ../configure.py --enable-optimize --sdks cs2
ambuild
```

Результат: `build/package/addons/Reklama/Reklama.so`.
