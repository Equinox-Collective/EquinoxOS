CC = x86_64-elf-gcc
LD = x86_64-elf-ld
AR = x86_64-elf-ar
ASM = nasm

# Pin the default goal to 'all' so a bare `make` always does a full build.
# Otherwise GNU Make picks the first concrete file target it sees, which after
# the BearSSL block below is libbearssl.a — making `make clean && make` quietly
# leave the build with no kernel.elf / no equos.iso.
.DEFAULT_GOAL := all

ifeq ($(OS),Windows_NT)
  # GitHub's Windows image adds MSYS2 to PATH for xorriso, which also exposes
  # sh.exe. GNU Make will otherwise pick that POSIX shell and then fail on the
  # cmd.exe-style Windows recipes below (`if not exist ...`). Force cmd.exe for
  # Windows builds so local mingw/choco make and CI use the same recipe syntax.
  SHELL := cmd.exe
  .SHELLFLAGS := /C
endif

# --- HOST SHELL ABSTRACTION ----------------------------------------------------
# Чтобы Makefile собирался И на Windows (cmd.exe, mingw32-make), И на Linux/CI,
# обёртываем все file-ops в макросы и переключаем их по $(OS). Все pattern-rules
# ниже общие — отличаются только setup/clean/iso/link recipe-ы.
ifeq ($(OS),Windows_NT)
  # cmd.exe ветка — поведение как было раньше.
  E := $(strip)
  bs := \$E
  # to_win = заменить '/' на '\' (cmd.exe такого не любит)
  to_win = $(subst /,$(bs),$1)
  MKDIR_P  = if not exist "$(call to_win,$1)" mkdir "$(call to_win,$1)"
  RM_F     = if exist "$(call to_win,$1)" del /f /q "$(call to_win,$1)"
  RM_RF    = if exist "$(call to_win,$1)" rmdir /s /q "$(call to_win,$1)"
  CP_F     = copy /Y "$(call to_win,$1)" "$(call to_win,$2)"
  NULL_OUT = 2>nul
else
  # POSIX (Linux CI, macOS).
  MKDIR_P  = mkdir -p "$1"
  RM_F     = rm -f "$1"
  RM_RF    = rm -rf "$1"
  CP_F     = cp -f "$1" "$2"
  NULL_OUT = 2>/dev/null || true
endif

# --- PATHS ---
OBJ_DIR = obj
SDK_LIB_DIR = sdk/lib
ISO_ROOT = iso_root

# --- KERNEL FLAGS ---
CFLAGS = -ffreestanding -O2 -Wall -Wextra -fno-exceptions -std=c11 \
         -Isrc -Isrc/system -Isrc/system/core -Isrc/syslibc -Isrc/boot/limine \
         -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -fno-stack-protector -fno-pic -g -MMD -MP

LDFLAGS = -nostdlib -T src/linker.ld -z max-page-size=0x1000
ASMFLAGS = -f elf64

# --- SDK FLAGS (APPS) ---
SDK_INC = -I./sdk/include
USER_CFLAGS = -ffreestanding -mcmodel=small -mno-red-zone -fno-stack-protector -fno-pic -g \
              -fno-omit-frame-pointer $(SDK_INC) -MMD -MP

# --- KERNEL SOURCES ---
SRC_DIRS = src src/boot src/syslibc \
           src/system/core \
           src/system/drivers/devices/audio \
           src/system/drivers/devices/keyboard \
           src/system/drivers/devices/mouse \
           src/system/drivers/devices/pci \
           src/system/drivers/devices/pcspeaker \
           src/system/drivers/hardware/disk \
           src/system/drivers/hardware/net \
           src/system/drivers/hardware/serial \
           src/system/drivers/vesa \
           src/system/fs \
           src/system/mem \
           src/system/misc \
           src/system/shell \
           src/system/usr \
           src/system/hal

KERNEL_C_SRCS = $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
KERNEL_ASM_SRCS = $(wildcard src/system/core/*.asm)

KERNEL_OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(filter %.c,$(KERNEL_C_SRCS))) \
              $(patsubst src/%.asm,$(OBJ_DIR)/%.o,$(KERNEL_ASM_SRCS))

# Все obj-подкаталоги, которые надо создать в setup (после $(OBJ_DIR)/...).
# Берём src-каталоги, отрезаем "src/" префикс, кладём под $(OBJ_DIR)/.
KERNEL_OBJ_SUBDIRS = $(OBJ_DIR) $(addprefix $(OBJ_DIR)/,$(patsubst src/%,%,$(filter-out src,$(SRC_DIRS))))

# --- SDK OBJECTS ---
SDK_C_SRCS = $(wildcard $(SDK_LIB_DIR)/*.c)
SDK_ASM_SRCS = $(wildcard $(SDK_LIB_DIR)/*.asm)
SDK_OBJS = $(patsubst $(SDK_LIB_DIR)/%.c,$(SDK_LIB_DIR)/%.o,$(SDK_C_SRCS)) \
           $(patsubst $(SDK_LIB_DIR)/%.asm,$(SDK_LIB_DIR)/%.o,$(SDK_ASM_SRCS))

# --- BEARSSL (vendored under third_party/bearssl) -----------------------------
# Built once as a static library; userspace apps link against it for TLS.
# Sources are NEVER patched — all platform tweaks happen via -D flags below
# and via sdk/include/limits.h. See third_party/bearssl/README.equos.md.
BEARSSL_DIR     := third_party/bearssl
BEARSSL_INC     := -I./$(BEARSSL_DIR)/inc -I./$(BEARSSL_DIR)/src
BEARSSL_SRC_DIRS := \
    $(BEARSSL_DIR)/src \
    $(BEARSSL_DIR)/src/aead \
    $(BEARSSL_DIR)/src/codec \
    $(BEARSSL_DIR)/src/ec \
    $(BEARSSL_DIR)/src/hash \
    $(BEARSSL_DIR)/src/int \
    $(BEARSSL_DIR)/src/kdf \
    $(BEARSSL_DIR)/src/mac \
    $(BEARSSL_DIR)/src/rand \
    $(BEARSSL_DIR)/src/rsa \
    $(BEARSSL_DIR)/src/ssl \
    $(BEARSSL_DIR)/src/symcipher \
    $(BEARSSL_DIR)/src/x509
BEARSSL_C_SRCS  := $(foreach d,$(BEARSSL_SRC_DIRS),$(wildcard $(d)/*.c))
BEARSSL_OBJS    := $(BEARSSL_C_SRCS:.c=.o)
BEARSSL_LIB     := $(BEARSSL_DIR)/libbearssl.a

# BR_USE_URANDOM / BR_USE_WIN32_RAND : disable platform-specific seeders;
#   we seed BearSSL ourselves from sys_getrandom() (phase 3b shim).
# BR_64                              : force the 64-bit codepath (we're x86_64).
BEARSSL_CFLAGS  := $(USER_CFLAGS) $(BEARSSL_INC) \
                   -DBR_USE_URANDOM=0 -DBR_USE_WIN32_RAND=0 -DBR_64=1 \
                   -W -Wall -Os

$(BEARSSL_DIR)/src/%.o: $(BEARSSL_DIR)/src/%.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@

$(BEARSSL_LIB): $(BEARSSL_OBJS)
	@echo === Building libbearssl.a ===
	$(AR) -rcs $@ $(BEARSSL_OBJS)

libbearssl: $(BEARSSL_LIB)

# --- QUICKJS (vendored under third_party/quickjs) -----------------------------
# Built once as a static library; userspace apps link against it to execute
# JavaScript. Sources are NEVER patched — all EquinoxOS adaptation happens
# through SDK headers (sdk/include/pthread.h, alloca.h, sys/time.h),
# sdk/lib/qjs_time.c (gettimeofday + clock_gettime + gmtime_r), and the
# -D defines below. See third_party/quickjs/README.equos.md.
QUICKJS_DIR     := third_party/quickjs
QUICKJS_INC     := -I./$(QUICKJS_DIR)
QUICKJS_C_SRCS  := $(QUICKJS_DIR)/quickjs.c \
                   $(QUICKJS_DIR)/dtoa.c \
                   $(QUICKJS_DIR)/libregexp.c \
                   $(QUICKJS_DIR)/libunicode.c
QUICKJS_OBJS    := $(QUICKJS_C_SRCS:.c=.o)
QUICKJS_LIB     := $(QUICKJS_DIR)/libquickjs.a

# NO_TM_GMTOFF      : our struct tm has no tm_gmtoff field — fall back to
#                     the mktime(gmtime_r) - mktime(localtime_r) path
#                     (both return UTC on EquinoxOS, so offset = 0).
# _GNU_SOURCE       : enables a few GNU-isms QuickJS' cutils.h expects.
# -Wno-*            : QuickJS upstream is warning-clean on its own
#                     toolchain but not against -Wall -Wextra of our
#                     freestanding cross; silence the noise without
#                     patching sources.
QUICKJS_CFLAGS  := $(USER_CFLAGS) $(QUICKJS_INC) \
                   -DNO_TM_GMTOFF -D_GNU_SOURCE \
                   -Wno-unused -Wno-sign-compare -Wno-pointer-sign \
                   -Wno-implicit-fallthrough -Wno-unused-parameter \
                   -Wno-format -Wno-format-extra-args -Wno-cast-function-type \
                   -Os

$(QUICKJS_DIR)/%.o: $(QUICKJS_DIR)/%.c
	$(CC) $(QUICKJS_CFLAGS) -c $< -o $@

$(QUICKJS_LIB): $(QUICKJS_OBJS)
	@echo === Building libquickjs.a ===
	$(AR) -rcs $@ $(QUICKJS_OBJS)

libquickjs: $(QUICKJS_LIB)

# --- DOOM ---
DOOM_DIR = app/doom
DOOM_SRCS = $(wildcard $(DOOM_DIR)/*.c)
DOOM_OBJS = $(patsubst $(DOOM_DIR)/%.c, $(OBJ_DIR)/doom/%.o, $(DOOM_SRCS))

# --- MAIN RULES ---

# Full local build: compile everything, generate hdd.img, then build the ISO.
# Keep these targets behind explicit dependencies so `make -j` cannot run
# `create_hdd` before the app binaries (notably bin/doom.elf) are ready.
all: create_hdd iso

# CI variant: build the ISO artifacts without generating hdd.img.
ci: iso

# --- SETUP -------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
setup:
	@if not exist $(OBJ_DIR) mkdir $(OBJ_DIR)
	@if not exist $(OBJ_DIR)\doom mkdir $(OBJ_DIR)\doom
	@if not exist $(OBJ_DIR)\system mkdir $(OBJ_DIR)\system
	@if not exist $(ISO_ROOT)\sys mkdir $(ISO_ROOT)\sys
	@if not exist $(ISO_ROOT)\bin mkdir $(ISO_ROOT)\bin
	@if not exist $(ISO_ROOT)\res mkdir $(ISO_ROOT)\res
	@if not exist $(ISO_ROOT)\EFI\BOOT mkdir $(ISO_ROOT)\EFI\BOOT
	@for %%d in ($(subst /,\\,$(SRC_DIRS))) do @if not exist $(OBJ_DIR)\%%d mkdir $(OBJ_DIR)\%%d 2>nul
else
setup:
	@mkdir -p $(OBJ_DIR) $(OBJ_DIR)/doom $(OBJ_DIR)/system
	@mkdir -p $(ISO_ROOT)/sys $(ISO_ROOT)/bin $(ISO_ROOT)/res $(ISO_ROOT)/EFI/BOOT
	@mkdir -p $(KERNEL_OBJ_SUBDIRS)
endif

# --- KERNEL LINK -------------------------------------------------------------
kernel.elf: setup $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o kernel.elf
	@$(call CP_F,kernel.elf,$(ISO_ROOT)/sys/kernel.elf)

# --- KERNEL BUILD RULES ------------------------------------------------------
$(OBJ_DIR)/%.o: src/%.c
	@$(call MKDIR_P,$(@D))
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: src/%.asm
	@$(call MKDIR_P,$(@D))
	$(ASM) $(ASMFLAGS) $< -o $@

# --- SDK BUILD RULES ---------------------------------------------------------
$(SDK_LIB_DIR)/%.o: $(SDK_LIB_DIR)/%.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SDK_LIB_DIR)/%.o: $(SDK_LIB_DIR)/%.asm
	$(ASM) -f elf64 $< -o $@

# --- DOOM BUILD RULES --------------------------------------------------------
$(OBJ_DIR)/doom/%.o: $(DOOM_DIR)/%.c
	@$(call MKDIR_P,$(OBJ_DIR)/doom)
	$(CC) $(USER_CFLAGS) -DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400 -DFEATURE_SOUND -c $< -o $@

doom.elf: setup $(SDK_OBJS) $(DOOM_OBJS)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) $(DOOM_OBJS) -o $(ISO_ROOT)/bin/doom.elf

# --- APPS BUILD RULES --------------------------------------------------------
APP_SRCS = $(wildcard app/*.c)
APP_OBJS = $(patsubst app/%.c,app/%.o,$(APP_SRCS))
APP_ELFS_SIMPLE = $(ISO_ROOT)/bin/snake.elf $(ISO_ROOT)/bin/bmpview.elf $(ISO_ROOT)/bin/htmlview.elf $(ISO_ROOT)/bin/niplay.elf $(ISO_ROOT)/bin/widget_demo.elf $(ISO_ROOT)/bin/ipc_test.elf $(ISO_ROOT)/bin/randtest.elf $(ISO_ROOT)/bin/socktest.elf

# Apps that link against libbearssl.a (phase 3b+). These get their own
# explicit rules below because they need (a) BearSSL public headers in the
# include path and (b) libbearssl.a appended at link time.
APP_ELFS_TLS    = $(ISO_ROOT)/bin/tlsboot.elf $(ISO_ROOT)/bin/tlstest.elf $(ISO_ROOT)/bin/catest.elf $(ISO_ROOT)/bin/httpsget.elf $(ISO_ROOT)/bin/urlget.elf $(ISO_ROOT)/bin/browser.elf
APP_ELFS_QJS    = $(ISO_ROOT)/bin/jstest.elf $(ISO_ROOT)/bin/domtest.elf

# Phase 5: HTTP/HTTPS client library. Lives in its own directory so it
# isn't auto-folded into $(SDK_OBJS) — apps that need it append
# $(HTTP_CLIENT_OBJ) explicitly. Built with bearssl includes because
# the .c #includes <bearssl.h> / <bearssl_io.h>.
HTTP_CLIENT_OBJ := sdk/lib_http/http_client.o

# Object builds need the Windows directory tree from setup before they start;
# this matters when users run `make -j`.
$(KERNEL_OBJS) $(SDK_OBJS) $(APP_OBJS) $(DOOM_OBJS): | setup

apps: setup $(SDK_OBJS) $(BEARSSL_LIB) $(QUICKJS_LIB) $(APP_ELFS_SIMPLE) $(APP_ELFS_TLS) $(APP_ELFS_QJS) sysgui_app

$(ISO_ROOT)/bin/%.elf: app/%.o $(SDK_OBJS)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) $< -o $@

app/%.o: app/%.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

# TLS apps: bearssl headers visible at compile, libbearssl.a appended at link.
app/tlsboot.o: app/tlsboot.c
	$(CC) $(USER_CFLAGS) -I./third_party/bearssl/inc -c $< -o $@

$(ISO_ROOT)/bin/tlsboot.elf: app/tlsboot.o $(SDK_OBJS) $(BEARSSL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) $< $(BEARSSL_LIB) -o $@

app/tlstest.o: app/tlstest.c app/ca_anchors.h
	$(CC) $(USER_CFLAGS) -I./third_party/bearssl/inc -c $< -o $@

$(ISO_ROOT)/bin/tlstest.elf: app/tlstest.o $(SDK_OBJS) $(BEARSSL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) $< $(BEARSSL_LIB) -o $@

app/catest.o: app/catest.c third_party/ca_bundle/ca_bundle.h
	$(CC) $(USER_CFLAGS) -I./third_party/bearssl/inc -c $< -o $@

$(ISO_ROOT)/bin/catest.elf: app/catest.o $(SDK_OBJS) $(BEARSSL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) $< $(BEARSSL_LIB) -o $@

# httpsget — real-internet HTTPS smoke test (phase 4c). Same toolchain as
# catest (needs the Mozilla TA bundle header) plus the bearssl public
# headers; otherwise just a normal TLS-linked app.
app/httpsget.o: app/httpsget.c third_party/ca_bundle/ca_bundle.h
	$(CC) $(USER_CFLAGS) -I./third_party/bearssl/inc -c $< -o $@

$(ISO_ROOT)/bin/httpsget.elf: app/httpsget.o $(SDK_OBJS) $(BEARSSL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) $< $(BEARSSL_LIB) -o $@

# urlget — phase 5 wrapper around the new http_client library. Same link
# soup as the other TLS apps + the dedicated http_client object.
sdk/lib_http/http_client.o: sdk/lib_http/http_client.c sdk/include/http_client.h sdk/include/url.h
	$(CC) $(USER_CFLAGS) -I./third_party/bearssl/inc -c $< -o $@

app/urlget.o: app/urlget.c sdk/include/http_client.h sdk/include/url.h third_party/ca_bundle/ca_bundle.h
	$(CC) $(USER_CFLAGS) -I./third_party/bearssl/inc -c $< -o $@

$(ISO_ROOT)/bin/urlget.elf: app/urlget.o $(HTTP_CLIENT_OBJ) $(SDK_OBJS) $(BEARSSL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) $< $(HTTP_CLIENT_OBJ) $(BEARSSL_LIB) -o $@

# browser.elf — phase 6 GUI browser. Compiles app/htmlview.c a SECOND time
# with -DBROWSER_BUILD, which swaps its load_page() for the eq_http_get()
# variant (full HTTP/HTTPS via the phase-5 client). htmlview.elf is built
# from the same source without the define and keeps its original local-file
# loading path, so both binaries coexist.
app/htmlview_browser.o: app/htmlview.c sdk/include/http_client.h sdk/include/url.h third_party/ca_bundle/ca_bundle.h
	$(CC) $(USER_CFLAGS) -DBROWSER_BUILD -I./third_party/bearssl/inc -c $< -o $@

$(ISO_ROOT)/bin/browser.elf: app/htmlview_browser.o $(HTTP_CLIENT_OBJ) $(SDK_OBJS) $(BEARSSL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) $< $(HTTP_CLIENT_OBJ) $(BEARSSL_LIB) -o $@

# jstest — smoke test for the vendored QuickJS engine. Covers phases
# J1 (bytecode + string allocator) and J2 (console.log + Math/Date/JSON).
#
# sdk/lib_qjs/qjs_helpers.c lives in its own directory so it isn't
# auto-folded into $(SDK_OBJS) — apps that don't embed QuickJS shouldn't
# pay for these helpers. Same pattern as sdk/lib_http/http_client.o.
QJS_HELPERS_OBJ := sdk/lib_qjs/qjs_helpers.o

sdk/lib_qjs/qjs_helpers.o: sdk/lib_qjs/qjs_helpers.c sdk/include/qjs_helpers.h
	$(CC) $(USER_CFLAGS) -I./third_party/quickjs -c $< -o $@

app/jstest.o: app/jstest.c sdk/include/qjs_helpers.h
	$(CC) $(USER_CFLAGS) -I./third_party/quickjs -c $< -o $@

$(ISO_ROOT)/bin/jstest.elf: app/jstest.o $(QJS_HELPERS_OBJ) $(SDK_OBJS) $(QUICKJS_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) $< $(QJS_HELPERS_OBJ) $(QUICKJS_LIB) -o $@

# DOM tree library — used by domtest, htmlview, and (later) the JS DOM
# bindings. Lives in its own directory so it isn't auto-folded into
# $(SDK_OBJS); apps opt in by linking $(DOM_OBJ).
DOM_OBJ := sdk/lib_dom/dom.o

sdk/lib_dom/dom.o: sdk/lib_dom/dom.c sdk/include/dom.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

app/domtest.o: app/domtest.c sdk/include/dom.h
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(ISO_ROOT)/bin/domtest.elf: app/domtest.o $(DOM_OBJ) $(SDK_OBJS)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) $< $(DOM_OBJ) -o $@

# enGUI's app/sysgui/Makefile links sysgui.elf via `$(wildcard ../../sdk/lib/*.o)`,
# so under parallel make (`make -j` on Linux CI) sysgui_app would race against the
# SDK_OBJS pattern rule and link against an empty/partial set — failing with a wall
# of `undefined reference to memcpy / floor / eid_*`. Windows CI runs serially so
# it doesn't hit this. Declare the dependency explicitly so -j is safe.
sysgui_app: $(SDK_OBJS)
	@echo "=== Building sysgui (enGUI) ==="
	$(MAKE) -C app/sysgui
	@$(call CP_F,app/sysgui/sysgui.elf,iso_root/bin/sysgui.elf)
	@$(call MKDIR_P,iso_root/res/sysgui)
	@$(call CP_F,app/sysgui/scripts/init.lua,iso_root/res/sysgui/init.lua)
	@$(call CP_F,app/sysgui/scripts/window.lua,iso_root/res/sysgui/window.lua)
	@$(call CP_F,app/sysgui/scripts/monitor.lua,iso_root/res/sysgui/monitor.lua)
	@$(call CP_F,app/sysgui/scripts/terminal.lua,iso_root/res/sysgui/terminal.lua)
	@$(call CP_F,app/sysgui/scripts/paint.lua,iso_root/res/sysgui/paint.lua)
	@$(call CP_F,app/sysgui/scripts/explorer.lua,iso_root/res/sysgui/explorer.lua)
	@$(call CP_F,app/sysgui/scripts/notepad.lua,iso_root/res/sysgui/notepad.lua)

# --- SYSTEM RULES ------------------------------------------------------------
ifeq ($(OS),Windows_NT)
clean:
	@if exist $(OBJ_DIR) rmdir /s /q $(OBJ_DIR)
	@if exist sdk\lib\*.o del /q sdk\lib\*.o
	@if exist sdk\lib\*.d del /q sdk\lib\*.d
	@if exist app\*.o del /q app\*.o
	@if exist app\*.d del /q app\*.d
	@if exist kernel.elf del /q kernel.elf
	@if exist equos.iso del /q equos.iso
	@if exist app\sysgui\sysgui.elf del /q app\sysgui\sysgui.elf
	@for /R third_party\bearssl %%f in (*.o *.d) do @if exist "%%f" del /q "%%f"
	@if exist third_party\bearssl\libbearssl.a del /q third_party\bearssl\libbearssl.a
else
clean:
	@rm -rf $(OBJ_DIR)
	@rm -f sdk/lib/*.o sdk/lib/*.d
	@rm -f app/*.o app/*.d
	@rm -f kernel.elf equos.iso
	@rm -f app/sysgui/sysgui.elf
	@find third_party/bearssl -name '*.o' -delete -o -name '*.d' -delete
	@rm -f third_party/bearssl/libbearssl.a
endif

create_hdd: kernel.elf apps doom.elf
	@echo --- Generating EXT2 hdd.img ---
	python WINDOWS_ext2.py

iso: kernel.elf apps doom.elf
	@$(call RM_F,equos.iso)
	xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot EFI/BOOT/limine-bios-cd.bin -efi-boot-part --efi-boot-image -o equos.iso $(ISO_ROOT)

# --- QEMU ---
#
# Раньше run/cleanrun жёстко включали `-d int,guest_errors,mmu -D qemu.log`,
# из-за чего QEMU писал КАЖДОЕ прерывание (включая каждый int $0x80 и каждый
# тик таймера на 1 кГц) в файл — это легко режет производительность в 5-10
# раз и визуально превращает рабочий стол в "10 FPS".
#
# Делим на два таргета:
#   make run        — обычный запуск, никакого логирования, пытаемся включить
#                     железное ускорение (whpx на Windows, kvm на Linux, hvf
#                     на macOS) с откатом на TCG, если ничего не доступно.
#   make run-debug  — диагностический запуск с записью всех прерываний/MMU
#                     в qemu.log. Использовать только при отладке падений.

QEMU       := qemu-system-x86_64
# Базовый CPU = qemu64 (стабильно работает на WHPX), плюс явно
# включаем RDRAND/RDSEED/AES-NI поверх. Чистый `-cpu max` с WHPX
# валится с "Unexpected VP exit code 4" — гипервизор не умеет
# часть фичей, которые max объявляет. qemu64+флаги — самый
# совместимый способ дать ядру RDRAND под WHPX/KVM/HVF/TCG.
QEMU_BASE  := -m 512M -boot d \
              -cpu qemu64,+rdrand,+rdseed,+aes \
              -drive file=hdd.img,format=raw,index=0,media=disk \
              -cdrom equos.iso \
              -serial stdio \
              -netdev user,id=n0,hostfwd=tcp::2222-:22 \
              -device rtl8139,netdev=n0 \
              -device ac97,audiodev=snd0 -audiodev dsound,id=snd0
# Перебор акселераторов: первый рабочий используется, иначе TCG.
QEMU_ACCEL := -accel whpx,kernel-irqchip=off -accel kvm -accel hvf -accel tcg

run:
	$(QEMU) $(QEMU_BASE) $(QEMU_ACCEL)

# Run with pure software emulation (no hypervisor). Slower but more
# deterministic — useful when WHPX/KVM behave oddly with network I/O.
run-tcg:
	$(QEMU) $(QEMU_BASE) -accel tcg

run-debug:
	$(QEMU) $(QEMU_BASE) -d int,guest_errors,mmu -D qemu.log

cleanrun: clean all run

# Include dependency files
-include $(KERNEL_OBJS:.o=.d)
-include $(SDK_OBJS:.o=.d)
-include $(APP_SRCS:.c=.d)
