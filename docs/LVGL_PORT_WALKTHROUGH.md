# LVGL Port to EquinoxOS sysgui - Walkthrough

We have successfully integrated the LVGL graphics library (v9.5.0) into the windowed system (`sysgui`) of EquinoxOS.

## Changes Implemented

### 1. Configuration
- **[NEW] [lv_conf.h](file:///c:/Users/ewasion/Documents/!DEVprojects/EquinoxOS/app/sysgui/lv_conf.h)**:
  - Custom configurations optimized for the EquinoxOS 32-bit framebuffer.
  - Set `LV_COLOR_DEPTH` to `32`.
  - Configured memory management to use LVGL's built-in heap allocator with a `256 KB` pool.
  - Disabled OS wrappers (`LV_USE_OS LV_OS_NONE`), running entirely in single-threaded cooperative mode.
  - Enabled all standard widgets needed by the demo widgets app (such as canvas, chart, scale, textarea, keyboard, calendar, etc.).
  - Disabled third-party libraries, hardware accelerators, and native drivers.

### 2. Build System
- **[MODIFY] [Makefile](file:///c:/Users/ewasion/Documents/!DEVprojects/EquinoxOS/app/sysgui/Makefile)**:
  - Added include directories: `../../third_party/lvgl`, `.` (for `lv_conf.h`).
  - Added `-DLV_CONF_INCLUDE_SIMPLE` to the compiler flags.
  - Implemented GNU Make recursive wildcard traversal to compile all `.c` files in `third_party/lvgl/src` recursively while filtering out platform-specific code under `src/drivers`.
  - Gathered and added LVGL widgets demo source files to the build.
  - Suppressed third-party warnings by tailoring specific compiler flags (`LV_CFLAGS`).

### 3. Core Initialization & Ticks
- **[MODIFY] [main.cpp](file:///c:/Users/ewasion/Documents/!DEVprojects/EquinoxOS/app/sysgui/main.cpp)**:
  - Included `lvgl.h`.
  - Registered a custom tick reader callback (`equos_tick_get_cb`) utilizing System Call 6 (returns elapsed milliseconds) via `lv_tick_set_cb` to drive animations and widget timers.
  - Initialized the LVGL library on startup with `lv_init()`.

### 4. Window Manager and Application Integration
- **[NEW] [lvgl_app.h](file:///c:/Users/ewasion/Documents/!DEVprojects/EquinoxOS/app/sysgui/gui/apps/lvgl_app.h)** & **[lvgl_app.cpp](file:///c:/Users/ewasion/Documents/!DEVprojects/EquinoxOS/app/sysgui/gui/apps/lvgl_app.cpp)**:
  - Implemented the `LvglApp` class subclassing `App` from the native GUI system.
  - Allocates a dedicated local `client_w * client_h * 4` draw buffer for the LVGL screen canvas.
  - Creates a new `lv_display_t` matching the window client area with color format `LV_COLOR_FORMAT_XRGB8888`.
  - Directs LVGL drawing to render to a partial buffer using `equos_flush_cb`, which copies compiled pixels to our local window buffer.
  - Configures a pointer input device callback `equos_mouse_read_cb` that translates screen mouse coordinates `(mx, my)` to client-local window coordinates.
  - Launches the standard interactive `lv_demo_widgets()`.
  - On each rendering tick, calls `lv_timer_handler()` and transfers the updated local framebuffer into the main `sysgui` backbuffer.
- **[MODIFY] [win_manager.cpp](file:///c:/Users/ewasion/Documents/!DEVprojects/EquinoxOS/app/sysgui/gui/win_manager.cpp)**:
  - Registered `"LVGL Demo"` in the window manager to support spawning the `LvglApp` instances.
- **[MODIFY] [dock.cpp](file:///c:/Users/ewasion/Documents/!DEVprojects/EquinoxOS/app/sysgui/gui/dock.cpp)**:
  - Added the launch shortcut with a custom purple vector icon representation labeled `"LVGL Demo"` directly in the Sonoma dock.

---

## Verification and Testing

1. **Compilation Check**:
   The full code compiles and links without error into a single ~5.2 MB ELF binary (`sysgui.elf`) containing the entire LVGL engine and widgets demo:
   ```powershell
   make SKIP=bearssl,doom,quickjs,sdl2 create_hdd
   make SKIP=bearssl,doom,quickjs,sdl2 iso
   ```
2. **QEMU Emulator Execution**:
   To launch the operating system and test the port:
   ```powershell
   make run
   # Or for USB mouse support:
   make run-usb
   ```
3. **Application Interaction**:
   - Locate the purple **LVGL Demo** icon in the dock and double-click to launch it.
   - Interact with the tabs, sliders, charts, text fields, and calendar elements.
   - Drag the window around and close it to verify memory cleanup.
