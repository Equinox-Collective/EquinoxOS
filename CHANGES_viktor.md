# EquinoxOS — Patch set: HAL · GPT · Sync · IPC · PSF2 · UI widgets

This patch adds the following Phase 1–4 roadmap items without touching any
existing files except `kernel.c`, `syscall.c`, and `equos.h` (additive only):

| Roadmap item                                | Files                                   |
| ------------------------------------------- | --------------------------------------- |
| **HAL skeleton** (display/input/block)      | `src/system/hal/hal.{c,h}`              |
| **GPT partition parsing**                   | `src/system/fs/gpt.{c,h}`               |
| **Mutex / semaphore / spinlock / waitq**    | `src/system/usr/sync.{c,h}`             |
| **IPC: pipes + message queues**             | `src/system/usr/ipc.{c,h}` + syscalls   |
| **PSF2 font rendering (kernel-side)**       | `src/system/drivers/vesa/psf2.{c,h}`    |
| **Advanced UI widgets + animations**        | `sdk/include/eid_ext.h`, `sdk/lib/eid_ext.c` |
| **Widget demo app**                         | `app/widget_demo.c`                     |

Build system is **NOT changed** — both `Makefile-linux` and `Makefile` glob
sources with `$(shell find src -name "*.c")` and `$(wildcard sdk/lib/*.c)`
/ `$(wildcard app/*.c)`, so the new files are picked up automatically.

---

## What each piece does

### 1. HAL (`src/system/hal/hal.{c,h}`)

Decouples the rest of the kernel from concrete device drivers. Three tables:

```c
typedef struct hal_display_ops { ... }  /* VESA today, VirtIO-GPU tomorrow  */
typedef struct hal_input_ops   { ... }  /* PS/2 today, USB-HID later        */
typedef struct hal_block_ops   { ... }  /* ATA PIO today, AHCI/NVMe later   */
```

Adapters for the existing VESA / PS/2 / ATA-PIO are registered automatically
by `hal_init()`, called from `kmain()`. **No existing call site is changed** —
the old drivers keep working. New code (e.g. AHCI, GPT) goes through HAL.

### 2. GPT (`src/system/fs/gpt.{c,h}`)

```c
int gpt_parse(int block_id, gpt_partition_t *out, int max_out);
void gpt_dump(int block_id);
```

- Reads LBA 0 (protective MBR check), LBA 1 (GPT header), then the entry
  table.
- Validates signature `"EFI PART"` and revision >= 1.0.
- Fills a friendly `gpt_partition_t` for each used slot, including UTF-16 →
  ASCII name conversion.
- Returns the number of valid partitions, or `-1` on error.

CRC32 is **not** verified (kernel has no CRC32 yet). All other geometry checks
are in place — adding CRC32 later is a 30-line addition.

Usage example (from the shell or a debug command):

```c
#include "system/fs/gpt.h"
gpt_dump(0);    /* dump partitions on hal_block(0) */
```

### 3. Sync primitives (`src/system/usr/sync.{c,h}`)

- `spinlock_t` — IRQ-disabling atomic spinlock. Use for tiny critical
  sections that may touch interrupt context.
- `waitqueue_t` — sleep/wake building block. `wq_sleep()` parks the current
  task (`running=false`) and yields. `wq_wake_one/_all()` flips it back.
- `kmutex_t` — sleeping mutex. FIFO waiter queue.
- `ksem_t` — counting semaphore.

The scheduler in `task.c` already skips tasks where `running == false`, so no
scheduler changes are required.

### 4. IPC (`src/system/usr/ipc.{c,h}` + syscalls 60–67)

- **Pipe** — 4 KB ring buffer, blocking `read` / `write` with both reader
  and writer wait queues, drained correctly on close.
- **Message queue** — fixed-size datagrams up to 256 B, max 32 messages,
  priority-sorted (higher prio dequeued first).

Syscalls added (see `sdk/include/equos.h`):

```
60 SYS_PIPE_CREATE  -> id or -1
61 SYS_PIPE_READ    rdi=id rsi=buf rdx=size -> bytes
62 SYS_PIPE_WRITE   rdi=id rsi=buf rdx=size -> bytes
63 SYS_PIPE_CLOSE   rdi=id
64 SYS_MQ_CREATE    rdi=msg_size            -> id or -1
65 SYS_MQ_SEND      rdi=id rsi=buf rdx=prio
66 SYS_MQ_RECV      rdi=id rsi=buf          -> bytes
67 SYS_MQ_CLOSE     rdi=id
```

Userspace example:

```c
int p = sys_pipe_create();
sys_pipe_write(p, "hello", 5);

char buf[16];
int n = sys_pipe_read(p, buf, sizeof(buf));   /* blocks until data */
```

### 5. PSF2 font renderer (`src/system/drivers/vesa/psf2.{c,h}`)

- Pure parser, no allocations beyond an internal 64 KB `uint16_t` Unicode
  cache embedded in `psf2_font_t`.
- Variable glyph width, arbitrary height.
- Unicode table parsed once (`flags & 1`).
- UTF-8 input string decoder included.

You can register the default font at boot once a PSF2 file is loaded from disk:

```c
uint32_t sz;
void *data = vfs_read_file("/res/font.psf", &sz);
psf2_init_default(data, sz);
```

Then anywhere in the kernel:

```c
psf2_draw_string(&kernel_psf2_font, fb, w, h, x, y, "Привет, мир!", 0xFFFFFF);
```

The legacy 8×8 font in `font8x8.h` is **not removed** — it stays as the
fallback for code that has not been migrated yet.

### 6. Widget toolkit (`sdk/include/eid_ext.h`, `sdk/lib/eid_ext.c`)

Builds on top of the existing immediate-mode `eid_*` core. Adds:

- `eid_button(ctx, label, x, y, w, h)` — hover/active visuals + click.
- `eid_checkbox(ctx, label, x, y, &bool_value)` — toggles `*value` on click.
- `eid_text_input(ctx, label, x, y, w, h, char_buf, buf_len)` — single-line
  editable text. Backspace + printable ASCII. Caret while focused.
- `eid_slider(ctx, label, x, y, w, &float_value, vmin, vmax)` — draggable
  thumb, normalized fill bar.
- `eid_anim_t` + `eid_anim_init/to/step/eval` — tiny animation helper with
  Linear / OutQuad / InOutQuad / OutCubic easing. Pair with `SYS_GET_TIME`
  to drive frame-rate-independent UI animations (see `app/widget_demo.c`).

A scancode → ASCII helper (`eid_scancode_to_ascii`) is provided so the text
field can consume `ctx->last_key` without depending on a higher-level
keymap layer.

### 7. Demo (`app/widget_demo.c`)

Drop-in app showing all widgets in one window with a slide-in animated
header. Built automatically by the existing `apps:` rule.

---

## What is **NOT** in this patch (and why)

| Item from your list                | Status                                                          |
| ---------------------------------- | --------------------------------------------------------------- |
| AHCI/SATA driver                   | Out of scope for this round — multi-day MVP. HAL is in place to slot it in cleanly. |
| SSE/AVX-accelerated blitting       | Kernel CFLAGS currently set `-mno-sse -mno-sse2`. Lifting that per-file plus saving XMM on context-switch (FXSAVE/FXRSTOR) is a scheduler change I didn't want to ship blind. Easy to add once you confirm the desired ABI. |
| TrueType / TTF                     | `sdk/lib/font_ttf.c` already ports `stb_truetype.h`. PSF2 added here as the lower-friction kernel-side path. |
| HTTPS (TLS)                        | Needs BearSSL/mbedTLS port + RNG + cert store — a project on its own. |
| USB stack                          | Same — months of work. |
| `mlibc`/`newlib` libc, self-hosting, custom FS, OS installer | Multi-week. |

---

## Sanity-checked

All new C files pass `gcc -ffreestanding -fsyntax-only -Wall -Wextra` with the
same `-mcmodel=*`, `-mno-red-zone`, `-fno-stack-protector`, `-fno-pic` flags
that the Makefile uses for kernel and userspace respectively. No cross-compile
toolchain (`x86_64-elf-gcc`) was available in my sandbox, so I have NOT been
able to actually link `kernel.elf` and boot it in QEMU. If anything fails on
your end, paste the build error here and I'll fix it.

---

## Files touched

```
Added:
  src/system/hal/hal.h
  src/system/hal/hal.c
  src/system/fs/gpt.h
  src/system/fs/gpt.c
  src/system/usr/sync.h
  src/system/usr/sync.c
  src/system/usr/ipc.h
  src/system/usr/ipc.c
  src/system/drivers/vesa/psf2.h
  src/system/drivers/vesa/psf2.c
  sdk/include/eid_ext.h
  sdk/lib/eid_ext.c
  app/widget_demo.c
  CHANGES_viktor.md       (this file)

Modified (additive):
  src/kernel.c            (+5 lines: include hal.h, ipc.h, init both)
  src/system/usr/syscall.c (+30 lines: cases 60-67 + include "ipc.h")
  sdk/include/equos.h     (+30 lines: SYS_* constants and wrappers)
```
