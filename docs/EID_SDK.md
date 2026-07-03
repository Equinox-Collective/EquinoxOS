***

# EquinoxOS SDK & EID v2.0 Documentation

## 1. System Architecture
EquinoxOS follows a strict **User-Kernel Separation** model. Applications are compiled as **ELF64** binaries, linked to a base address of `0x1000000`. 

### The System Call Bridge (`int 0x80`)
All hardware and system interactions are performed via the `_syscall` wrapper.
* **RAX**: Syscall Number
* **RDI, RSI, RDX, RCX, R8**: Arguments 1-5
* **Return**: Value is returned in `RAX`.

### Core System Calls Reference
| No. | Name | Description | Arguments |
| :--- | :--- | :--- | :--- |
| 1 | `SYS_PRINT` | Output string to serial/terminal. | `rdi`: char* msg |
| 2 | `SYS_READ_FILE` | Map file from VFS to User RAM. | `rdi`: name, `rsi`: size_out |
| 3 | `SYS_WRITE_FILE` | Save data to VFS (EXT2/FAT32). | `rdi`: name, `rsi`: buf, `rdx`: size |
| 5 | `SYS_DRAW_BUFFER` | Blit window buffer to Compositor. | `rdi/rsi`: x/y, `rdx/rcx`: w/h, `r8`: buf |
| 7 | `SYS_GET_MOUSE` | Get mouse state from Kernel. | `RAX`: X, `RBX`: Y, `RCX`: Buttons |
| 9 | `SYS_GET_SCANCODE` | Pop keyboard scancode. | - |
| 10 | `SYS_EXIT` | Terminate process & reclaim RAM. | `rdi`: exit_code |
| 12 | `SYS_GET_FONT` | Map system PSF font to User-space. | - |
| 20 | `SYS_AUDIO_PLAY` | Submit PCM chunk to AC97. | `rdi`: buf, `rsi`: size |

---

## 2. EID (Equinox Interface Designer)
EID v2.0 is an **Immediate Mode GUI** toolkit. It does not store widget state in the library; instead, it provides logical interaction flags and drawing primitives, giving the developer total control over the visual style.

### The EID Context (`eid_ctx_t`)
The context tracks the state of the mouse, active widgets, and keyboard focus during a single frame.

```c
typedef struct {
  uint32_t *fb;       // Target pixel buffer
  int win_w, win_h;   // Buffer dimensions
  int mx, my;         // Mouse X/Y (Relative to Window)
  bool m_down;        // Left Mouse Button held
  bool m_clicked;     // Left Mouse Button clicked this frame
  uint32_t hot_id;    // ID of widget under mouse
  uint32_t active_id; // ID of widget being held
  uint32_t focus_id;  // ID of widget with keyboard focus
} eid_ctx_t;
```

### Interaction Model
Widgets are identified by a unique **ID** generated from their label and position.
```c
uint32_t id = eid_get_id("MyButton", x, y);
uint32_t state = eid_process_interaction(&ctx, id, x, y, width, height);

if (state & EID_STATE_HOVER)   { /* Draw hover effect */ }
if (state & EID_STATE_CLICKED) { /* Perform action */ }
```

### Drawing Primitives
All drawing functions are **Safe**. They perform boundary checks against `win_w` and `win_h` to prevent memory corruption and Page Faults.

| Function | Parameters |
| :--- | :--- |
| `eid_draw_pixel` | `fb, win_w, win_h, x, y, color` |
| `eid_draw_rect` | `fb, win_w, win_h, x, y, w, h, color` |
| `eid_draw_line` | `fb, win_w, win_h, x1, y1, x2, y2, color` |
| `eid_draw_text` | `fb, win_w, win_h, x, y, text, color` |

---

## 3. Practical Example: A Neon Button
This example demonstrates how to create a custom-styled button using EID primitives and interaction logic.

```c
#include <eid.h>
#include <equos.h>

eid_ctx_t ui;
uint32_t buffer[400 * 300];

void my_app_render() {
    eid_begin(&ui, buffer, 400, 300);
    
    // Map Global Mouse to Window Space
    ui.mx -= 200; // Assuming window is at X=200
    ui.my -= 200; // Assuming window is at Y=200

    // Logic for a Button
    uint32_t id = eid_get_id("START", 50, 50);
    uint32_t state = eid_process_interaction(&ui, id, 50, 50, 100, 40);

    // Style Calculation
    uint32_t color = (state & EID_STATE_HOVER) ? 0x00FFFF : 0x008888;
    if (state & EID_STATE_ACTIVE) color = 0xFFFFFF;

    // Drawing
    eid_draw_rect(buffer, 400, 300, 50, 50, 100, 40, color);
    eid_draw_text(buffer, 400, 300, 60, 62, "START", 0xFFFFFF);

    if (state & EID_STATE_CLICKED) {
        // Handle click event...
    }

    eid_end(&ui, 200, 200);
}
```

---

## 4. Best Practices
1. **Always use `exit(0)`**: Never let `main()` return. Always terminate via the syscall to ensure the kernel reclaims the process memory.
2. **Coordinate Mapping**: Since EID v2.0 uses absolute mouse coordinates, always subtract your window's `X` and `Y` from `ui.mx` and `ui.my` after `eid_begin`.
3. **Safety First**: Use the drawing primitives provided by EID rather than writing to the buffer directly to avoid Kernel Panics when drawing outside window bounds.
4. **Yielding**: If your application is in a loop waiting for events, call `sys_yield()` to prevent consuming 100% of the CPU.