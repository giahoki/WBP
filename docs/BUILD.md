# Сборка и оптимизация

## Что нужно

[MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`) с пакетами CLANG64 из
[packaging/windows/README.md](../packaging/windows/README.md). Python не нужен.

## Команды

| Команда | Что делает |
|---|---|
| `build.bat` | игра (`bb-probe.exe`), лаунчер (`Bloodborne.exe`), пакет `dist\bbport-windows\` и zip |
| `build.bat launcher` | только лаунчер и пакет |
| `bash build.sh` (в CLANG64) | игра и лаунчер в `out\` без упаковки |
| `bash build.sh --launcher` | только лаунчер |
| `bash packaging/windows/package.sh` | пакет из `out\` |

Перед `build.bat` закройте лаунчер и игру, запущенные из `dist\`: иначе папка не обновится
(zip соберётся всё равно).

Из исходников всё запускается так же: `out\Bloodborne.exe` сам находит `bin\` или `out\` и DLL MSYS2.

## Оптимизация сборки

- **ThinLTO** включён по умолчанию (`BB_LTO=OFF` — выключить).
- **PGO** (оптимизация по профилю): если есть `pgo\bbport.profdata`, сборка использует его сама.
  Профиль один для всех игроков — в нём записано, какой код горячий, а не какое железо.
  - Записать новый: `set BB_PGO=generate`, `build.bat`, поиграть 10–15 минут как обычно (бег, бой,
    разные места). Инструментированная игра пишет `pgo\default_*.profraw` каждые 30 с.
  - Затем обычный `build.bat`: он сливает записи в `pgo\bbport.profdata` (сырые уходят в
    `pgo\recorded\`) и собирает с профилем. `bbport.profdata` стоит закоммитить.
  - Перезаписывать стоит раз в несколько месяцев или после больших изменений графики; код,
    изменённый после записи, просто собирается без профиля. `BB_PGO=off` — сборка без него.

## Пакет

`dist\bbport-windows\`:

```
Bloodborne.exe          лаунчер и подготовка игры
Play Bloodborne.exe     игра без окна лаунчера
bin\                    bb-probe.exe, bb-gpu-capabilities.exe, DLL MSYS2 (SDL3, FFmpeg, …), DLSS
launcher\lang\          переводы
patches\                патчи сообщества
fsr4_shaders\           модель FSR 4 (если была при сборке)
licenses\               лицензии сторонних компонентов
```

При обновлении `dist\` сохраняются `user\`, `mods\`, `bbport.ini`, `mods.json`, `patches.json`.

## Переменные запуска

Лаунчер передаёт настройки игре переменными окружения; их можно задать и вручную перед
`Bloodborne.exe --run`.

| Переменная | Что |
|---|---|
| `BB_GAME_DIR`, `BB_USER_DIR`, `BB_DATA_DIR`, `BB_CONFIG` | папки игры, пользователя, данных, путь к `bbport.ini` |
| `BB_FPS` | `uncap`, `60`, `90`, `30` |
| `BB_FPS_LIMIT` | лимит FPS в режиме без ограничения (0 — без лимита) |
| `BB_PRESENT_MODE`, `BB_FULLSCREEN`, `BB_HDR` | показ кадров, полный экран, HDR |
| `BB_RENDER_RES`, `BB_OUTPUT_RES`, `BB_LIVE_RES` | разрешение сцены и вывода, смена на лету |
| `BB_LANGUAGE`, `BB_USER_NAME` | язык игры, имя игрока |
| `BB_MODS_DIR`, `BB_MODS_ENABLED`, `BB_PATCHES_DIR`, `BB_PATCHES`, `BB_FORCE_PATCHES` | моды и патчи |
| `BB_ONLINE`, `BB_SHADNET_*`, `BB_COMMUNITY_SERVER`, `BB_UPNP` | онлайн |
| `BB_MOUSE`, `BB_MOUSE_INVERT_Y`, `BB_MOUSE_SENS` | мышь |
| `BB_DRAW_PIPE`, `BB_READBACKS`, `BB_FRAMES_AHEAD` | производительность графики |
| `BB_FRAME_STATS`, `BB_GPU_PROFILE`, `BB_VK_VALIDATION` | диагностика |

Остальные отладочные переменные описаны в [RENDER.md](RENDER.md), [INPUT.md](INPUT.md) и
[MODS.md](MODS.md).
