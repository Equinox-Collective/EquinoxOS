
## 📍 Current Status — Foundation + Phase 1–4 partially landed

The kernel boots to a usable desktop, runs Ring 3 ELF apps with full filesystem
and networking, plays AC97 audio, and now ships a Hardware Abstraction Layer,
in-kernel IPC, sync primitives, GPT/PSF2 parsers and an extended immediate-mode
UI toolkit (see [`CHANGES.md`](CHANGES.md) for the patch set).

### ✅ Done

- [x] **Boot:** Limine protocol, x86_64 Higher Half (HHDM), HHDM offset usage.
- [x] **Memory:** Bitmap PMM (`pmm_alloc_continuous()`), 4-level VMM with per-process PML4, 64 MB kernel heap (`kmalloc`/`kfree`).
- [x] **Multitasking:** Preemptive round-robin scheduler on IRQ0 (PIT 50 Hz).
- [x] **User mode:** Ring 3 with isolated address spaces, per-process page tables, GDT/TSS for kernel↔user transitions.
- [x] **ELF loader:** ELF64 PT_LOAD mapped into user PML4, jump to `_start` (CRT0).
- [x] **System calls:** `int 0x80` dispatch table — print, file R/W, draw buffer, mouse, keyboard, exit, yield, font, sleep, brk, audio play/rate, map_phys, shm, vesa info, window pos, net (DNS/HTTP), mem stats, exec, pipes (60–63), message queues (64–67).
- [x] **Graphics:** VESA LFB with double-buffered compositing window manager, drop shadows (alpha blend), z-ordering, hardware cursor.
- [x] **Storage:** ATA PIO sector R/W.
- [x] **Filesystems:** FAT32 R/W, EXT2 R/W (stress-tested in 4 phases), VFS abstraction, device-node registration.
- [x] **Network:** RTL8139 driver, ARP, IPv4, ICMP, UDP, TCP 3-way handshake, DNS resolve, HTTP GET, NTP time sync.
- [x] **Audio:** AC97 PCM driver via `SYS_AUDIO_PLAY` (used by DOOM port and `niplay`).
- [x] **Input:** PS/2 keyboard (scancodes) and PS/2 mouse (relative tracking).
- [x] **PCI bus scan** + **PIT 8253 timer** + **COM1 serial** debug log.
- [x] **Shared Memory** (`shm_alloc()` / `SYS_SHM_GET`).
- [x] **PSF font rendering** (loaded from `/res/font.psf` and used by VESA text output).
- [x] **GUI rebase:** the desktop / system shell is now a separate **enGUI** project, loaded as the Ring 3 init process (`bin/sysgui.elf`) from the `app/sysgui` submodule.
- [x] **Lua removed** from the SDK and the default app set (cleaned out in `aff9670` / `1546590`).

### ✅ Phase 1 — Architectural Integrity (HAL)

- [x] **HAL skeleton** in `src/system/hal/` — `hal_display_ops_t`, `hal_input_ops_t`, `hal_block_ops_t`, registry, `hal_init()` adapter registration for the existing VESA / PS/2 / ATA-PIO drivers (no callers rewritten yet, but the surface is in place for VirtIO-GPU / USB-HID / AHCI backends).

### 🟡 Phase 2 — Advanced Storage & VFS

- [x] **GPT parser** (`src/system/fs/gpt.{c,h}`) — protective-MBR check, header validation, entry-table walk, UTF-16 → ASCII partition names. CRC32 still TODO.
- [ ] **AHCI / SATA driver** — to replace ATA PIO. HAL block interface is ready to host it.
- [ ] **VFS enhancements** — proper file descriptors (`open`/`read`/`write`/`close`), seek/stat, pipe nodes wired into VFS (in-kernel pipes already exist, see Phase 4).
- [ ] **EXT2 large files** — indirect / double-indirect / triple-indirect block support (see `ext2_plan.md`).

### 🟡 Phase 3 — UI Toolkit & Visuals

- [x] **PSF2 font renderer** (`src/system/drivers/vesa/psf2.{c,h}`) — variable glyph width, Unicode table, UTF-8 input decoder. Legacy `font8x8.h` kept as fallback.
- [x] **Advanced UI widgets** in EID v2.0 extension (`sdk/include/eid_ext.h`, `sdk/lib/eid_ext.c`):
    - [x] `eid_button` (hover / active / clicked)
    - [x] `eid_checkbox` (bound to `bool *`)
    - [x] `eid_text_input` (single-line, caret, backspace, ASCII via `eid_scancode_to_ascii`)
    - [x] `eid_slider` (draggable thumb, normalized 0..1)
    - [x] Animation helper `eid_anim_t` with Linear / OutQuad / InOutQuad / OutCubic easings
    - [x] Widget showcase: `app/widget_demo.c`
- [ ] **SSE/AVX-accelerated blitting** — kernel currently builds with `-mno-sse -mno-sse2`; enabling SIMD needs FXSAVE/FXRSTOR on context switch.
- [ ] **Real-time blur** (Gaussian/Box, SSE) for window effects.
- [ ] **TrueType rendering** — `sdk/include/stb_truetype.h` + `sdk/lib/font_ttf.c` are shipped for userspace; kernel-side TTF still TODO.
- [ ] **Window animations** in the compositor itself (fade-in, slide-out) — primitive available via `eid_anim_*`, not yet hooked into the WM.

### ✅ Phase 4 — Multitasking & IPC

- [x] **Sync primitives** (`src/system/usr/sync.{c,h}`):
    - [x] `spinlock_t` (IRQ-disabling)
    - [x] `waitqueue_t` (sleep/wake)
    - [x] `kmutex_t` (sleeping FIFO mutex)
    - [x] `ksem_t` (counting semaphore)
- [x] **IPC** (`src/system/usr/ipc.{c,h}`):
    - [x] **Pipes** — 4 KB ring buffer, blocking R/W with reader+writer wait queues.
    - [x] **Message queues** — up to 32 messages × 256 B, priority-sorted.
    - [x] Syscalls **60–67** (`SYS_PIPE_*`, `SYS_MQ_*`) plus wrappers in `equos.h`, demo in `app/ipc_test.c`.
- [ ] **Shared memory across processes** (today `shm_alloc` is single-task) — secondary to pipes/MQ but useful for GUI event streaming.

### 🟡 Phase 5 — Self-Hosting

- [ ] **C standard library** — minimal stdio/stdlib/string/math/setjmp present in `sdk/lib/`; full `mlibc` / `newlib` port still TODO.
- [ ] **Shell improvements** — scripting, env vars, piping (now realistic with kernel pipes).
- [ ] **Toolchain port** — TCC or `x86_64-elf-gcc` cross to native, plus `make`.
- [ ] **Native build** — compile a Hello-World _inside_ EquinoxOS.

---

## 🌠 Long-Term Vision

* **Networking:** DHCP client + DNS cache, full POSIX socket API (`socket`/`bind`/`connect`/`send`/`recv`), TCP retransmission and full state machine (`SYN_SENT`, `FIN_WAIT`, `TIME_WAIT`) — see `network_plan.md`.
* **Security & crypto:** HTTPS (BearSSL / mbedTLS port), RNG, cert store.
* **Audio stack:** Intel HD Audio in addition to AC97; software mixer with multiple streams.
* **USB stack:** UHCI / EHCI / xHCI for keyboards, mice, storage.
* **Filesystem:** native EquinoxFS format + reusable journaling layer; mountable installer image.
* **Distribution:** installable ISO that writes the OS to a real disk (EXT2/FAT32 with GPT layout).
* **Games & ports:** native Quake / Wolf3D in addition to the existing DOOM port.
