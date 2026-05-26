# enGUI — Equinox Desktop Environment

A lightweight, freestanding desktop environment for [EquinoxOS](https://github.com/Equinox-Collective), written in C with a Lua scripting layer on top. enGUI runs as a Ring 3 user-space process: it talks to the kernel through syscalls, draws into its own backbuffer, and exposes a small immediate-mode GUI API to Lua so that windows, widgets and animations can be authored as plain `.lua` files.

> Easily extensible system GUI for EquinoxOS — write your apps in Lua, render with the built-in EID drawing API.

---

## Features

- **Double-buffered framebuffer rendering** at the host display resolution (defaults to 1024×768, autodetected from the kernel via syscall `32`).
- **Lua 5.4** scripting layer (vendored under [`lua/`](lua/)) for all UI logic — no recompilation needed to change the desktop.
- **Immediate-mode widgets**: buttons, checkboxes, sliders, text input, with hit-testing driven by mouse state.
- **Animation system** built on `eid_anim_t` with easing curves (`animCreate` / `animTo` / `animStep` / `animEval`).
- **Software cursor** composited on top of the backbuffer every frame.
- **File I/O bindings** so Lua apps can read, save and enumerate files through EquinoxOS' VFS.
- **Smooth ~50 FPS main loop** using `sys_yield()` and the PIT-driven preemptive scheduler — no fixed `sys_sleep` throttle.

## Repository layout

```
.
├── main.c         # Entry point: framebuffer setup, main loop, cursor blit
├── api_gui.c/.h   # C → Lua bindings (drawing, widgets, animations, IO)
├── lua/           # Vendored Lua 5.4 interpreter sources
├── scripts/
│   └── init.lua   # Default desktop: window manager, terminal, notepad, …
├── Makefile       # Builds sysgui.elf with the EquinoxOS x86_64-elf toolchain
└── LICENSE        # MIT
```

## Building

enGUI is built as a freestanding ELF for the EquinoxOS user-space ABI. You will need:

- `x86_64-elf-gcc` and `x86_64-elf-ld` cross-compiler toolchain
- A checkout of the EquinoxOS SDK in a sibling `../../sdk` directory (the Makefile picks up `$(SDK_DIR)/lib/*.o` and `$(SDK_DIR)/include`)

Then:

```bash
make            # produces sysgui.elf
make clean      # removes object files and the ELF
```

On Windows the Makefile automatically swaps `rm -f` for `del`.

The resulting `sysgui.elf` is loaded by EquinoxOS at virtual address `0x1000000` with entry point `_start`. At startup it expects the default desktop script at `res/sysgui/init.lua` on the target filesystem — copy `scripts/init.lua` there (or to wherever your boot image expects it).

## How it works

```
┌───────────────────────┐    syscall 32     ┌────────────────────┐
│  main.c (Ring 3)      │ ────────────────► │  EquinoxOS kernel  │
│  • maps framebuffer   │ ◄──── phys fb ─── │  • PIT @ 50 Hz     │
│  • allocates backbuf  │                   │  • mouse / keyboard│
│  • runs Lua VM        │ ──── sys_yield ─► │  • VFS             │
└─────────┬─────────────┘                   └────────────────────┘
          │ register_gui_api(L)
          ▼
┌───────────────────────┐
│  scripts/init.lua     │  draws windows, handles input, animates
└───────────────────────┘
```

Each frame the C loop:

1. Polls mouse (`int 0x80`, syscall `7`) and keyboard (`SYS_GET_SCANCODE`).
2. Decides whether a redraw is needed (input changed, animation active, or forced first frames).
3. Computes `dt` in ms from `SYS_GET_TIME` (1 tick = 2 ms at the kernel's 50 Hz PIT).
4. Invokes `on_tick(dt)` in Lua, which redraws everything into the backbuffer.
5. Composites the 8×8 software cursor on top and `memcpy`s the backbuffer to VRAM.
6. Yields to the scheduler.

## Lua API

`api_gui.c` registers the following globals into every Lua state via `register_gui_api(L)`:

| Function | Purpose |
| --- | --- |
| `drawText(str, x, y, color)` | Draw a string at `(x, y)` in 0xRRGGBB. |
| `drawRect(x, y, w, h, color)` | Filled rectangle. |
| `drawGradient(x, y, w, h, c1, c2, vertical)` | Two-stop linear gradient. |
| `drawLine(x1, y1, x2, y2, color)` | Single-pixel line. |
| `button(...)` / `checkbox(...)` / `slider(...)` | Immediate-mode widgets returning input state. |
| `animCreate(duration, ease)` | Allocate an animation slot, returns id. |
| `animTo(id, target)` / `animStep(id, dt)` / `animEval(id)` | Drive and sample animations. |
| `getMouse()` / `getLastKey()` / `scancodeToAscii(sc, shift)` | Input helpers. |
| `getUptime()` / `getMemInfo()` | System info. |
| `readFile(path)` / `saveFile(path, data)` / `getFiles(dir)` | VFS access. |
| `exec(path)` | Launch another program. |

A minimal example (see `scripts/init.lua` for the full desktop):

```lua
function on_tick(dt)
    drawGradient(0, 0, 1024, 768, 0x0F1020, 0x1A1C2E, true)
    drawText("Hello, enGUI!", 32, 32, 0xFFFFFF)

    if button("Click me", 32, 64, 120, 28) then
        print("clicked!")
    end
end
```

## Status

enGUI already boots a full desktop with a window manager, a terminal and a notepad app — see `scripts/init.lua`. Animations, mouse focus and file I/O all work end-to-end on real EquinoxOS builds. Expect rough edges: it is research/hobby OS code.

## Contributing

Pull requests are welcome. Please keep:

- C code freestanding (no libc beyond what the SDK provides).
- Lua code compatible with the vendored Lua 5.4 build.
- Comments either in English or matching the surrounding style (existing code mixes English and Russian).

---

## Русская версия

**enGUI** — легковесная рабочая среда для [EquinoxOS](https://github.com/Equinox-Collective). Ядро написано на C, вся логика рабочего стола, окна и виджеты — на Lua 5.4. Процесс работает в пользовательском кольце (Ring 3), общается с ядром через syscalls, рисует в собственный бэкбуфер и поверх кладёт программный курсор.

### Возможности

- Двойная буферизация на разрешении хоста (по умолчанию 1024×768).
- Встроенный интерпретатор **Lua 5.4** — UI пишется без перекомпиляции.
- Immediate-mode виджеты: `button`, `checkbox`, `slider`, текстовый ввод.
- Система анимаций с easing-кривыми (`animCreate / animTo / animStep / animEval`).
- Биндинги к VFS EquinoxOS: `readFile`, `saveFile`, `getFiles`, `exec`.
- Плавный цикл рендера ~50 FPS на базе `sys_yield()` (вместо `sys_sleep`, который из-за PIT 50 Hz фактически давал ≥40 мс задержки).

### Сборка

Нужен кросс-тулчейн `x86_64-elf-gcc` / `x86_64-elf-ld` и SDK EquinoxOS в `../../sdk` рядом с репозиторием. Дальше:

```bash
make            # собирает sysgui.elf
make clean
```

`sysgui.elf` грузится по адресу `0x1000000` с точкой входа `_start`. При старте он ищет `res/sysgui/init.lua` в файловой системе EquinoxOS — положите туда `scripts/init.lua`.

### Структура

| Файл / каталог | Назначение |
| --- | --- |
| `main.c` | точка входа, главный цикл, отрисовка курсора |
| `api_gui.c` / `api_gui.h` | биндинги C → Lua: рисование, виджеты, анимации, IO |
| `lua/` | исходники Lua 5.4 (vendored) |
| `scripts/init.lua` | дефолтный рабочий стол: оконный менеджер, терминал, блокнот |
| `Makefile` | сборка под `x86_64-elf` |
| `LICENSE` | MIT |

### Lua API

Полный список функций — в таблице выше. Минимальный пример:

```lua
function on_tick(dt)
    drawGradient(0, 0, 1024, 768, 0x0F1020, 0x1A1C2E, true)
    drawText("Привет, enGUI!", 32, 32, 0xFFFFFF)

    if button("Кнопка", 32, 64, 120, 28) then
        print("нажали!")
    end
end
```
