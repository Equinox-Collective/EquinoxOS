CC = x86_64-elf-gcc
CXX = x86_64-elf-g++
LD = x86_64-elf-ld
AR = x86_64-elf-ar
ASM = nasm

.DEFAULT_GOAL := all

# --- КРОСС-ПЛАТФОРМЕННЫЙ СЕКЦИОННЫЙ ХЕЛПЕР ---
ifeq ($(OS),Windows_NT)
  SHELL := cmd.exe
  .SHELLFLAGS := /C
  E := $(strip)
  bs := \$E
  to_win = $(subst /,$(bs),$1)
  MKDIR_P  = if not exist "$(call to_win,$1)" mkdir "$(call to_win,$1)"
  RM_F     = if exist "$(call to_win,$1)" del /f /q "$(call to_win,$1)"
  RM_RF    = if exist "$(call to_win,$1)" rmdir /s /q "$(call to_win,$1)"
  CP_F     = copy /Y "$(call to_win,$1)" "$(call to_win,$2)"
else
  MKDIR_P  = mkdir -p "$1"
  RM_F     = rm -f "$1"
  RM_RF    = rm -rf "$1"
  CP_F     = cp -f "$1" "$2"
endif

OBJ_DIR     = obj
SDK_LIB_DIR = sdk/lib
ISO_ROOT    = iso_root

CFLAGS = -ffreestanding -O2 -Wall -Wextra -fno-exceptions -std=c11 \
         -Werror=implicit-function-declaration -Werror=int-conversion \
         -Wmissing-prototypes -Wstrict-prototypes \
         -Isrc -Isrc/system -Isrc/system/core -Isrc/syslibc -Isrc/boot/limine \
         -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -fno-stack-protector -fno-pic -g -MMD -MP

LDFLAGS  = -nostdlib -T src/linker.ld -z max-page-size=0x1000
ASMFLAGS = -f elf64

SDK_INC = -I./third_party/musl/include -I./sdk/include
USER_CFLAGS = -ffreestanding -mcmodel=small -mno-red-zone -fno-stack-protector -fno-pic -g \
              -fno-omit-frame-pointer $(SDK_INC) -MMD -MP -DSDL_DYNAMIC_API=0

USER_CXXFLAGS = -ffreestanding -mcmodel=small -mno-red-zone -fno-stack-protector -fno-pic -g \
                -fno-omit-frame-pointer -fno-exceptions -fno-rtti -std=c++17 $(SDK_INC) -MMD -MP

# --- ПАРСЕР ПЕРЕМЕННЫХ SKIP И NOCLEAN ---
comma := ,
empty :=
space := $(empty) $(empty)

PARSED_SKIP    := $(subst $(comma),$(space),$(SKIP))
PARSED_NOCLEAN := $(subst $(comma),$(space),$(NOCLEAN))

is_skipped   = $(filter $1,$(PARSED_SKIP))
is_nocleaned = $(filter $1,$(PARSED_NOCLEAN))

# --- СИСТЕМНЫЕ ИСТОЧНИКИ (ЯДРО) ---
SRC_DIRS = src src/boot src/syslibc src/system/core \
           src/system/drivers/devices/audio src/system/drivers/devices/keyboard \
           src/system/drivers/devices/mouse src/system/drivers/devices/pci \
           src/system/drivers/devices/pcspeaker src/system/drivers/devices/usb \
           src/system/drivers/hardware/disk src/system/drivers/hardware/net \
           src/system/drivers/hardware/serial src/system/drivers/vesa \
           src/system/fs src/system/mem src/system/misc \
           src/system/shell src/system/usr src/system/hal

KERNEL_C_SRCS   = $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
KERNEL_ASM_SRCS = $(wildcard src/system/core/*.asm)
KERNEL_OBJS     = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(filter %.c,$(KERNEL_C_SRCS))) \
                  $(patsubst src/%.asm,$(OBJ_DIR)/%.o,$(KERNEL_ASM_SRCS))
KERNEL_OBJ_SUBDIRS = $(OBJ_DIR) $(addprefix $(OBJ_DIR)/,$(patsubst src/%,%,$(filter-out src,$(SRC_DIRS))))

# --- SDK SOURCES & OBJECTS ---
SDK_C_SRCS   = $(wildcard $(SDK_LIB_DIR)/*.c)
SDK_ASM_SRCS = $(wildcard $(SDK_LIB_DIR)/*.asm)
SDK_CPP_SRCS = $(wildcard $(SDK_LIB_DIR)/*.cpp)
SDK_OBJS     = $(patsubst $(SDK_LIB_DIR)/%.c,$(SDK_LIB_DIR)/%.o,$(SDK_C_SRCS)) \
               $(patsubst $(SDK_LIB_DIR)/%.asm,$(SDK_LIB_DIR)/%.o,$(SDK_ASM_SRCS)) \
               $(patsubst $(SDK_LIB_DIR)/%.cpp,$(SDK_LIB_DIR)/%.o,$(SDK_CPP_SRCS))
SDK_LIB      = $(SDK_LIB_DIR)/libequos.a

# --- СТОРОННИЕ БИБЛИОТЕКИ ---
BEARSSL_DIR       := third_party/bearssl
BEARSSL_SRC_DIRS  := $(BEARSSL_DIR)/src $(foreach d,aead codec ec hash int kdf mac rand rsa ssl symcipher x509,$(BEARSSL_DIR)/src/$(d))
BEARSSL_C_SRCS    := $(foreach d,$(BEARSSL_SRC_DIRS),$(wildcard $(d)/*.c))
BEARSSL_OBJS      := $(BEARSSL_C_SRCS:.c=.o)
BEARSSL_LIB       := $(BEARSSL_DIR)/libbearssl.a
BEARSSL_CFLAGS    := $(USER_CFLAGS) -I./$(BEARSSL_DIR)/inc -I./$(BEARSSL_DIR)/src -DBR_USE_URANDOM=0 -DBR_USE_WIN32_RAND=0 -DBR_64=1 -Os

QUICKJS_DIR       := third_party/quickjs
QUICKJS_C_SRCS    := $(addprefix $(QUICKJS_DIR)/,quickjs.c dtoa.c libregexp.c libunicode.c)
QUICKJS_OBJS      := $(QUICKJS_C_SRCS:.c=.o)
QUICKJS_LIB       := $(QUICKJS_DIR)/libquickjs.a
QUICKJS_CFLAGS    := $(USER_CFLAGS) -I./$(QUICKJS_DIR) -DNO_TM_GMTOFF -D_GNU_SOURCE -Os \
                     -Wno-unused -Wno-sign-compare -Wno-pointer-sign -Wno-implicit-fallthrough \
                     -Wno-unused-parameter -Wno-format -Wno-format-extra-args -Wno-cast-function-type

SDL_DIR           := third_party/sdl2
SDL_SRCS          := $(SDL_DIR)/SDL.c $(SDL_DIR)/SDL_assert.c $(SDL_DIR)/SDL_dataqueue.c \
                     $(SDL_DIR)/SDL_error.c $(SDL_DIR)/SDL_guid.c $(SDL_DIR)/SDL_hints.c \
                     $(SDL_DIR)/SDL_log.c $(SDL_DIR)/SDL_utils.c $(SDL_DIR)/SDL_list.c \
                     $(SDL_DIR)/file/SDL_rwops.c $(SDL_DIR)/thread/SDL_thread.c \
                     $(SDL_DIR)/render/SDL_render.c $(SDL_DIR)/render/SDL_yuv_sw.c \
                     $(SDL_DIR)/timer/SDL_timer.c $(SDL_DIR)/timer/dummy/SDL_systimer.c \
                     $(wildcard $(SDL_DIR)/stdlib/*.c) $(wildcard $(SDL_DIR)/cpuinfo/*.c) \
                     $(wildcard $(SDL_DIR)/events/*.c) $(wildcard $(SDL_DIR)/video/*.c) \
                     $(wildcard $(SDL_DIR)/video/equinox/*.c) $(wildcard $(SDL_DIR)/video/yuv2rgb/*.c) \
                     $(wildcard $(SDL_DIR)/atomic/*.c) $(wildcard $(SDL_DIR)/thread/generic/*.c) \
                     $(wildcard $(SDL_DIR)/libm/*.c) $(wildcard $(SDL_DIR)/render/software/*.c)
SDL_OBJS          := $(SDL_SRCS:.c=.o)
SDL_LIB           := $(SDL_DIR)/libSDL2.a
SDL_CFLAGS        := $(USER_CFLAGS) -I./$(SDL_DIR) -I./$(SDL_DIR)/include -Os -DHAVE_FLOOR -DHAVE_CEIL -fno-strict-aliasing

LVGL_DIR          := third_party/lvgl
rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))
LVGL_ALL_C        := $(call rwildcard,$(LVGL_DIR)/src,*.c)
LVGL_EXCLUDE      := $(call rwildcard,$(LVGL_DIR)/src/drivers,*.c)
LVGL_C_SRCS       := $(filter-out $(LVGL_EXCLUDE),$(LVGL_ALL_C))
LVGL_DEMO_C       := $(call rwildcard,$(LVGL_DIR)/demos,*.c)
LVGL_C_SRCS       += $(LVGL_DEMO_C)
LVGL_OBJS         := $(LVGL_C_SRCS:.c=.o)
LVGL_LIB          := $(LVGL_DIR)/liblvgl.a
LVGL_CFLAGS       := -ffreestanding -mcmodel=small -mno-red-zone -fno-stack-protector -fno-pic -g \
                     -fno-omit-frame-pointer -I./third_party/musl/include -I./sdk/include -O2 -MMD -MP \
                     -I$(LVGL_DIR) -Iapp/sysgui -DLV_CONF_INCLUDE_SIMPLE \
                     -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare \
                     -Wno-type-limits -Wno-unused-function -Wno-missing-prototypes \
                     -Wno-strict-prototypes -Wno-implicit-fallthrough -Wno-unused-but-set-variable \
                     -std=c11

MUSL_DIR    := third_party/musl
MUSL_LIB    := $(MUSL_DIR)/lib
MUSL_CFLAGS := -ffreestanding -mcmodel=small -mno-red-zone -fno-stack-protector -fno-pic -g -nostdinc -isystem $(MUSL_DIR)/include

# --- ПРАВИЛА СБОРКИ СТОРОННИХ БИБЛИОТЕК ---
$(BEARSSL_DIR)/src/%.o: $(BEARSSL_DIR)/src/%.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@

$(BEARSSL_LIB): $(BEARSSL_OBJS)
	$(AR) -rcs $@ $(BEARSSL_OBJS)

$(QUICKJS_DIR)/%.o: $(QUICKJS_DIR)/%.c
	$(CC) $(QUICKJS_CFLAGS) -c $< -o $@

$(QUICKJS_LIB): $(QUICKJS_OBJS)
	$(AR) -rcs $@ $(QUICKJS_OBJS)

$(SDL_DIR)/%.o: $(SDL_DIR)/%.c
	$(CC) $(SDL_CFLAGS) -c $< -o $@

$(SDL_LIB): $(SDL_OBJS)
	$(AR) -rcs $@ $(SDL_OBJS)

$(LVGL_DIR)/%.o: $(LVGL_DIR)/%.c
	$(CC) $(LVGL_CFLAGS) -c $< -o $@

$(LVGL_LIB): $(LVGL_OBJS)
	$(AR) -rcs $@ $(LVGL_OBJS)

# --- ПРИЛОЖЕНИЯ И ЗАВИСИМОСТИ ---
DOOM_DIR  := app/doom
DOOM_SRCS := $(wildcard $(DOOM_DIR)/*.c)
DOOM_OBJS := $(patsubst $(DOOM_DIR)/%.c, $(OBJ_DIR)/doom/%.o, $(DOOM_SRCS))

# --- ДИНАМИЧЕСКОЕ ИСКЛЮЧЕНИЕ СБОРКИ (SKIP) ---
RELEASE ?= 0

ifeq ($(RELEASE),1)
  ACTIVE_APPS := $(ISO_ROOT)/bin/sh.elf \
                 $(ISO_ROOT)/bin/bash.elf \
                 $(ISO_ROOT)/bin/sysgui.elf \
                 $(ISO_ROOT)/bin/doom.elf \
                 $(ISO_ROOT)/bin/bmpview.elf \
                 $(ISO_ROOT)/bin/snake.elf
  USER_CFLAGS += -O3 -s
  CFLAGS += -O3 -DNDEBUG
else
  ACTIVE_APPS := $(APP_ELFS_SIMPLE) $(APP_ELFS_MUSL)
endif

ifeq ($(call is_skipped,sysgui),)
  ACTIVE_APPS += sysgui_app
endif

ifeq ($(call is_skipped,bearssl),)
  ACTIVE_LIBS += $(BEARSSL_LIB)
  ACTIVE_APPS += $(APP_ELFS_TLS)
endif

ifeq ($(call is_skipped,quickjs),)
  ACTIVE_LIBS += $(QUICKJS_LIB)
  ACTIVE_APPS += $(APP_ELFS_QJS)
endif

ifeq ($(call is_skipped,sdl2),)
  ACTIVE_LIBS += $(SDL_LIB)
  ACTIVE_APPS += $(ISO_ROOT)/bin/sdltest.elf
endif

ifeq ($(call is_skipped,lvgl),)
  ACTIVE_LIBS += $(LVGL_LIB)
endif
ifeq ($(call is_skipped,lgvl),) # на случай опечатки
  ACTIVE_LIBS += $(LVGL_LIB)
endif

ACTIVE_DOOM :=
ifeq ($(call is_skipped,doom),)
  ACTIVE_DOOM := doom.elf
endif

# --- ОСНОВНЫЕ ЦЕЛИ (all, ci, setup) ---
all: create_hdd iso
ci: iso

setup:
	@$(call MKDIR_P,$(OBJ_DIR))
	@$(call MKDIR_P,$(OBJ_DIR)/doom)
	@$(call MKDIR_P,$(OBJ_DIR)/system)
	@$(call MKDIR_P,$(ISO_ROOT)/sys)
	@$(call MKDIR_P,$(ISO_ROOT)/bin)
	@$(call MKDIR_P,$(ISO_ROOT)/res)
	@$(call MKDIR_P,$(ISO_ROOT)/EFI/BOOT)
	$(foreach dir,$(KERNEL_OBJ_SUBDIRS),$(call MKDIR_P,$(dir)) &&) @echo Setup complete.

# --- СБОРКА ЯДРА И СИСТЕМНЫХ ФАЙЛОВ ---
kernel.elf: setup $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o kernel.elf
	@$(call CP_F,kernel.elf,$(ISO_ROOT)/sys/kernel.elf)

$(OBJ_DIR)/%.o: src/%.c
	@$(call MKDIR_P,$(@D))
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: src/%.asm
	@$(call MKDIR_P,$(@D))
	$(ASM) $(ASMFLAGS) $< -o $@

# --- СБОРКА ЮЗЕРСПЕЙС SDK И ЕГО УПАКОВКА В СТАТИЧЕСКУЮ БИБЛИОТЕКУ ---
$(SDK_LIB_DIR)/%.o: $(SDK_LIB_DIR)/%.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SDK_LIB_DIR)/%.o: $(SDK_LIB_DIR)/%.asm
	$(ASM) -f elf64 $< -o $@

$(SDK_LIB_DIR)/%.o: $(SDK_LIB_DIR)/%.cpp
	$(CXX) $(USER_CXXFLAGS) -c $< -o $@

$(SDK_LIB): $(SDK_OBJS)
	$(AR) -rcs $@ $(SDK_OBJS)

# --- СБОРКА DOOM ---
$(OBJ_DIR)/doom/%.o: $(DOOM_DIR)/%.c
	@$(call MKDIR_P,$(OBJ_DIR)/doom)
	$(CC) $(USER_CFLAGS) -DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400 -DFEATURE_SOUND -c $< -o $@

doom.elf: setup $(SDK_LIB) $(DOOM_OBJS)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(DOOM_OBJS) $(SDK_LIB) third_party/musl/lib/libc.a -o $(ISO_ROOT)/bin/doom.elf

# --- ЮЗЕРСПЕЙС ПРИЛОЖЕНИЯ ---
DOM_OBJ         := sdk/lib_dom/dom.o
HTTP_CLIENT_OBJ := sdk/lib_http/http_client.o
QJS_PAGE_OBJ    := sdk/lib_qjs/qjs_page.o
QJS_WINDOW_OBJ  := sdk/lib_qjs/qjs_window.o
QJS_HELPERS_OBJ := sdk/lib_qjs/qjs_helpers.o
DOM_JS_OBJ      := sdk/lib_qjs/dom_js.o
QJS_FETCH_OBJ   := sdk/lib_qjs/qjs_fetch.o
IMAGE_DECODE_OBJ := sdk/lib_image/image_decode.o

APP_ELFS_SIMPLE := $(addprefix $(ISO_ROOT)/bin/,snake.elf bmpview.elf htmlview.elf niplay.elf widget_demo.elf ipc_test.elf randtest.elf socktest.elf forktest.elf exectest.elf pipetest.elf envtest.elf sigtest.elf ttytest.elf lxtest.elf stktest.elf fstest.elf mmfork.elf cpp_test.elf)
APP_ELFS_MUSL   := $(addprefix $(ISO_ROOT)/bin/,musltest.elf stattest.elf dirtest.elf ltsig.elf ltjob.elf ltjob2.elf bash.elf busybox.elf sh.elf)
APP_ELFS_TLS    := $(addprefix $(ISO_ROOT)/bin/,tlsboot.elf tlstest.elf catest.elf httpsget.elf urlget.elf browser.elf)
APP_ELFS_QJS    := $(addprefix $(ISO_ROOT)/bin/,jstest.elf domtest.elf jsdomtest.elf jsfetchtest.elf jspagetest.elf)

apps: setup $(SDK_LIB) $(ACTIVE_LIBS) $(ACTIVE_APPS)

$(ISO_ROOT)/bin/%.elf: app/%.o $(SDK_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $< $(SDK_LIB) third_party/musl/lib/libc.a -o $@

# --- MUSL И ШЕЛЛ ПРИЛОЖЕНИЯ ---
app/musltest.o: app/musltest.c ; $(CC) $(MUSL_CFLAGS) -c $< -o $@
$(ISO_ROOT)/bin/musltest.elf: app/musltest.o $(MUSL_LIB)/libc.a
	$(CC) -nostdlib -static -Wl,-Ttext=0x1000000 $(MUSL_LIB)/crt1.o $(MUSL_LIB)/crti.o app/musltest.o $(MUSL_LIB)/libc.a -lgcc $(MUSL_LIB)/crtn.o -o $@

app/stattest.o: app/stattest.c ; $(CC) $(MUSL_CFLAGS) -c $< -o $@
$(ISO_ROOT)/bin/stattest.elf: app/stattest.o $(MUSL_LIB)/libc.a
	$(CC) -nostdlib -static -Wl,-Ttext=0x1000000 $(MUSL_LIB)/crt1.o $(MUSL_LIB)/crti.o app/stattest.o $(MUSL_LIB)/libc.a -lgcc $(MUSL_LIB)/crtn.o -o $@

app/dirtest.o: app/dirtest.c ; $(CC) $(MUSL_CFLAGS) -c $< -o $@
$(ISO_ROOT)/bin/dirtest.elf: app/dirtest.o $(MUSL_LIB)/libc.a
	$(CC) -nostdlib -static -Wl,-Ttext=0x1000000 $(MUSL_LIB)/crt1.o $(MUSL_LIB)/crti.o app/dirtest.o $(MUSL_LIB)/libc.a -lgcc $(MUSL_LIB)/crtn.o -o $@

app/ltsig.o: app/ltsig.c ; $(CC) $(MUSL_CFLAGS) -c $< -o $@
$(ISO_ROOT)/bin/ltsig.elf: app/ltsig.o $(MUSL_LIB)/libc.a
	$(CC) -nostdlib -static -Wl,-Ttext=0x1000000 $(MUSL_LIB)/crt1.o $(MUSL_LIB)/crti.o app/ltsig.o $(MUSL_LIB)/libc.a -lgcc $(MUSL_LIB)/crtn.o -o $@

app/ltjob.o: app/ltjob.c ; $(CC) $(MUSL_CFLAGS) -c $< -o $@
$(ISO_ROOT)/bin/ltjob.elf: app/ltjob.o $(MUSL_LIB)/libc.a
	$(CC) -nostdlib -static -Wl,-Ttext=0x1000000 $(MUSL_LIB)/crt1.o $(MUSL_LIB)/crti.o app/ltjob.o $(MUSL_LIB)/libc.a -lgcc $(MUSL_LIB)/crtn.o -o $@

app/ltjob2.o: app/ltjob2.c ; $(CC) $(MUSL_CFLAGS) -c $< -o $@
$(ISO_ROOT)/bin/ltjob2.elf: app/ltjob2.o $(MUSL_LIB)/libc.a
	$(CC) -nostdlib -static -Wl,-Ttext=0x1000000 $(MUSL_LIB)/crt1.o $(MUSL_LIB)/crti.o app/ltjob2.o $(MUSL_LIB)/libc.a -lgcc $(MUSL_LIB)/crtn.o -o $@

$(ISO_ROOT)/bin/bash.elf: third_party/bash/bash.o $(MUSL_LIB)/libc.a
	$(CC) -nostdlib -static -Wl,-Ttext=0x1000000 $(MUSL_LIB)/crt1.o $(MUSL_LIB)/crti.o third_party/bash/bash.o $(MUSL_LIB)/libc.a -lgcc $(MUSL_LIB)/crtn.o -o $@

$(ISO_ROOT)/bin/busybox.elf: third_party/busybox/busybox.o $(MUSL_LIB)/libc.a
	$(CC) -nostdlib -static -Wl,-Ttext=0x1000000 $(MUSL_LIB)/crt1.o $(MUSL_LIB)/crti.o third_party/busybox/busybox.o $(MUSL_LIB)/libc.a -lgcc $(MUSL_LIB)/crtn.o -o $@

app/sh/sh.o: app/sh/sh.c ; $(CC) $(MUSL_CFLAGS) -I./sdk/include -c $< -o $@
$(ISO_ROOT)/bin/sh.elf: app/sh/sh.o $(MUSL_LIB)/libc.a
	$(CC) -nostdlib -static -Wl,-Ttext=0x1000000 $(MUSL_LIB)/crt1.o $(MUSL_LIB)/crti.o app/sh/sh.o $(MUSL_LIB)/libc.a -lgcc $(MUSL_LIB)/crtn.o -o $@

$(ISO_ROOT)/bin/stktest.elf: app/stktest.o ; $(LD) -nostdlib -Ttext=0x1000000 -e _start app/stktest.o -o $@
$(ISO_ROOT)/bin/fstest.elf: app/fstest.o   ; $(LD) -nostdlib -Ttext=0x1000000 -e _start app/fstest.o -o $@

app/%.o: app/%.c ; $(CC) $(USER_CFLAGS) -c $< -o $@
app/%.o: app/%.cpp ; $(CXX) $(USER_CXXFLAGS) -c $< -o $@

# --- TLS / QUICKJS / BROWSER ПРИЛОЖЕНИЯ ---
app/tlsboot.o: app/tlsboot.c ; $(CC) $(USER_CFLAGS) -I./$(BEARSSL_DIR)/inc -c $< -o $@
$(ISO_ROOT)/bin/tlsboot.elf: app/tlsboot.o $(SDK_LIB) $(BEARSSL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $< $(BEARSSL_LIB) $(SDK_LIB) third_party/musl/lib/libc.a -o $@

app/tlstest.o: app/tlstest.c app/ca_anchors.h ; $(CC) $(USER_CFLAGS) -I./$(BEARSSL_DIR)/inc -c $< -o $@
$(ISO_ROOT)/bin/tlstest.elf: app/tlstest.o $(SDK_LIB) $(BEARSSL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $< $(BEARSSL_LIB) $(SDK_LIB) third_party/musl/lib/libc.a -o $@

app/catest.o: app/catest.c third_party/ca_bundle/ca_bundle.h ; $(CC) $(USER_CFLAGS) -I./$(BEARSSL_DIR)/inc -c $< -o $@
$(ISO_ROOT)/bin/catest.elf: app/catest.o $(SDK_LIB) $(BEARSSL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $< $(BEARSSL_LIB) $(SDK_LIB) third_party/musl/lib/libc.a -o $@

app/httpsget.o: app/httpsget.c third_party/ca_bundle/ca_bundle.h ; $(CC) $(USER_CFLAGS) -I./$(BEARSSL_DIR)/inc -c $< -o $@
$(ISO_ROOT)/bin/httpsget.elf: app/httpsget.o $(SDK_LIB) $(BEARSSL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $< $(BEARSSL_LIB) $(SDK_LIB) third_party/musl/lib/libc.a -o $@

sdk/lib_http/http_client.o: sdk/lib_http/http_client.c ; $(CC) $(USER_CFLAGS) -I./$(BEARSSL_DIR)/inc -c $< -o $@
app/urlget.o: app/urlget.c ; $(CC) $(USER_CFLAGS) -I./$(BEARSSL_DIR)/inc -c $< -o $@
$(ISO_ROOT)/bin/urlget.elf: app/urlget.o $(HTTP_CLIENT_OBJ) $(SDK_LIB) $(BEARSSL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $< $(HTTP_CLIENT_OBJ) $(BEARSSL_LIB) $(SDK_LIB) third_party/musl/lib/libc.a -o $@

sdk/lib_image/image_decode.o: sdk/lib_image/image_decode.c ; $(CC) $(USER_CFLAGS) -I./third_party/stb_image -Wno-unused-function -Wno-implicit-fallthrough -c $< -o $@
sdk/lib_qjs/qjs_page.o: sdk/lib_qjs/qjs_page.c ; $(CC) $(USER_CFLAGS) -I./$(QUICKJS_DIR) -I./$(BEARSSL_DIR)/inc -c $< -o $@
sdk/lib_qjs/qjs_window.o: sdk/lib_qjs/qjs_window.c ; $(CC) $(USER_CFLAGS) -I./$(QUICKJS_DIR) -c $< -o $@
app/htmlview_browser.o: app/htmlview.c ; $(CC) $(USER_CFLAGS) -DBROWSER_BUILD -I./$(BEARSSL_DIR)/inc -I./$(QUICKJS_DIR) -c $< -o $@

$(ISO_ROOT)/bin/browser.elf: app/htmlview_browser.o $(HTTP_CLIENT_OBJ) $(DOM_OBJ) $(QJS_PAGE_OBJ) $(QJS_WINDOW_OBJ) $(QJS_FETCH_OBJ) $(DOM_JS_OBJ) $(QJS_HELPERS_OBJ) $(IMAGE_DECODE_OBJ) $(SDK_LIB) $(QUICKJS_LIB) $(BEARSSL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start app/htmlview_browser.o $(HTTP_CLIENT_OBJ) $(DOM_OBJ) $(QJS_PAGE_OBJ) $(QJS_WINDOW_OBJ) $(QJS_FETCH_OBJ) $(DOM_JS_OBJ) $(QJS_HELPERS_OBJ) $(IMAGE_DECODE_OBJ) $(QUICKJS_LIB) $(BEARSSL_LIB) $(SDK_LIB) third_party/musl/lib/libc.a -o $@

app/htmlview.o: app/htmlview.c
$(ISO_ROOT)/bin/htmlview.elf: app/htmlview.o $(DOM_OBJ) $(SDK_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start app/htmlview.o $(DOM_OBJ) $(SDK_LIB) third_party/musl/lib/libc.a -o $@

sdk/lib_qjs/qjs_helpers.o: sdk/lib_qjs/qjs_helpers.c ; $(CC) $(USER_CFLAGS) -I./$(QUICKJS_DIR) -c $< -o $@
app/jstest.o: app/jstest.c ; $(CC) $(USER_CFLAGS) -I./$(QUICKJS_DIR) -c $< -o $@
$(ISO_ROOT)/bin/jstest.elf: app/jstest.o $(QJS_HELPERS_OBJ) $(SDK_LIB) $(QUICKJS_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $< $(QJS_HELPERS_OBJ) $(QUICKJS_LIB) $(SDK_LIB) third_party/musl/lib/libc.a -o $@

$(ISO_ROOT)/bin/jsdomtest.elf: app/jsdomtest.o $(QJS_HELPERS_OBJ) $(DOM_JS_OBJ) $(DOM_OBJ) $(SDK_LIB) $(QUICKJS_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start app/jsdomtest.o $(QJS_HELPERS_OBJ) $(DOM_JS_OBJ) $(DOM_OBJ) $(QUICKJS_LIB) $(SDK_LIB) third_party/musl/lib/libc.a -o $@

sdk/lib_qjs/qjs_fetch.o: sdk/lib_qjs/qjs_fetch.c ; $(CC) $(USER_CFLAGS) -I./$(QUICKJS_DIR) -I./$(BEARSSL_DIR)/inc -c $< -o $@
app/jsfetchtest.o: app/jsfetchtest.c ; $(CC) $(USER_CFLAGS) -I./$(QUICKJS_DIR) -c $< -o $@

$(ISO_ROOT)/bin/jsfetchtest.elf: app/jsfetchtest.o $(QJS_HELPERS_OBJ) $(QJS_FETCH_OBJ) $(HTTP_CLIENT_OBJ) $(SDK_LIB) $(QUICKJS_LIB) $(BEARSSL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start app/jsfetchtest.o $(QJS_HELPERS_OBJ) $(QJS_FETCH_OBJ) $(HTTP_CLIENT_OBJ) $(QUICKJS_LIB) $(BEARSSL_LIB) $(SDK_LIB) third_party/musl/lib/libc.a -o $@

app/jspagetest.o: app/jspagetest.c ; $(CC) $(USER_CFLAGS) -I./$(QUICKJS_DIR) -c $< -o $@

$(ISO_ROOT)/bin/jspagetest.elf: app/jspagetest.o $(QJS_PAGE_OBJ) $(QJS_WINDOW_OBJ) $(QJS_FETCH_OBJ) $(DOM_JS_OBJ) $(QJS_HELPERS_OBJ) $(DOM_OBJ) $(HTTP_CLIENT_OBJ) $(SDK_LIB) $(QUICKJS_LIB) $(BEARSSL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start app/jspagetest.o $(QJS_PAGE_OBJ) $(QJS_WINDOW_OBJ) $(QJS_FETCH_OBJ) $(DOM_JS_OBJ) $(QJS_HELPERS_OBJ) $(DOM_OBJ) $(HTTP_CLIENT_OBJ) $(QUICKJS_LIB) $(BEARSSL_LIB) $(SDK_LIB) third_party/musl/lib/libc.a -o $@

sdk/lib_qjs/dom_js.o: sdk/lib_qjs/dom_js.c
	$(CC) $(USER_CFLAGS) -I./$(QUICKJS_DIR) -c $< -o $@

app/jsdomtest.o: app/jsdomtest.c
	$(CC) $(USER_CFLAGS) -I./$(QUICKJS_DIR) -c $< -o $@

sdk/lib_dom/dom.o: sdk/lib_dom/dom.c ; $(CC) $(USER_CFLAGS) -c $< -o $@
app/domtest.o: app/domtest.c ; $(CC) $(USER_CFLAGS) -c $< -o $@
$(ISO_ROOT)/bin/domtest.elf: app/domtest.o $(DOM_OBJ) $(SDK_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start app/domtest.o $(DOM_OBJ) $(SDK_LIB) third_party/musl/lib/libc.a -o $@

# --- SDL2 ПРИЛОЖЕНИЯ ---
app/sdltest.o: app/sdltest.c ; $(CC) $(USER_CFLAGS) -I./$(SDL_DIR)/include/ -c $< -o $@
$(ISO_ROOT)/bin/sdltest.elf: app/sdltest.o $(SDK_LIB) $(SDL_LIB)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $< $(SDL_LIB) $(SDK_LIB) third_party/musl/lib/libc.a -o $@

# --- СБОРКА SYSGUI (enGUI) ---
sysgui_app: $(SDK_LIB) $(LVGL_LIB)
	@echo "=== Building sysgui (enGUI) ==="
	$(MAKE) -C app/sysgui
	@$(call CP_F,app/sysgui/sysgui.elf,$(ISO_ROOT)/bin/sysgui.elf)
	@$(call MKDIR_P,$(ISO_ROOT)/res/sysgui)
	@$(call CP_F,app/sysgui/gui/BOOTSOUND.wav,$(ISO_ROOT)/res/sysgui/BOOTSOUND.wav)
	@echo Sysgui synced.

# --- ОЧИСТКА ВСЕХ КОМПОНЕНТОВ (CLEAN) ---
ifeq ($(OS),Windows_NT)
clean:
	@if exist $(OBJ_DIR) rmdir /s /q $(OBJ_DIR)
	@if exist sdk\lib\*.o del /q sdk\lib\*.o
	@if exist sdk\lib\*.d del /q sdk\lib\*.d
	@if exist sdk\lib\*.a del /q sdk\lib\*.a
	@if exist sdk\lib_qjs\*.o del /q sdk\lib_qjs\*.o
	@if exist sdk\lib_qjs\*.d del /q sdk\lib_qjs\*.d
	@if exist sdk\lib_dom\*.o del /q sdk\lib_dom\*.o
	@if exist sdk\lib_dom\*.d del /q sdk\lib_dom\*.d
	@if exist sdk\lib_http\*.o del /q sdk\lib_http\*.o
	@if exist sdk\lib_http\*.d del /q sdk\lib_http\*.d
	@if exist app\*.o del /q app\*.o
	@if exist app\*.d del /q app\*.d
	@if exist kernel.elf del /q kernel.elf
	@if exist equos.iso del /q equos.iso
ifeq ($(call is_nocleaned,sdl2),)
	@if exist third_party\sdl2\libSDL2.a del /q third_party\sdl2\libSDL2.a
	@for /R third_party\sdl2 %%f in (*.o *.d) do @if exist "%%f" del /q "%%f"
endif
ifeq ($(call is_nocleaned,bearssl),)
	@if exist third_party\bearssl\libbearssl.a del /q third_party\bearssl\libbearssl.a
	@for /R third_party\bearssl %%f in (*.o *.d) do @if exist "%%f" del /q "%%f"
endif
ifeq ($(call is_nocleaned,quickjs),)
	@if exist third_party\quickjs\libquickjs.a del /q third_party\quickjs\libquickjs.a
	@for /R third_party\quickjs %%f in (*.o *.d) do @if exist "%%f" del /q "%%f"
endif
ifeq ($(call is_nocleaned,sysgui),)
	$(MAKE) -C app/sysgui clean
endif
ifeq ($(call is_nocleaned,lvgl)$(call is_nocleaned,lgvl),)
	@if exist third_party\lvgl\liblvgl.a del /q third_party\lvgl\liblvgl.a
	@for /R third_party\lvgl %%f in (*.o *.d) do @if exist "%%f" del /q "%%f"
endif
else
clean:
	@rm -rf $(OBJ_DIR)
	@rm -f sdk/lib/*.o sdk/lib/*.d sdk/lib/*.a
	@rm -f sdk/lib_qjs/*.o sdk/lib_qjs/*.d
	@rm -f sdk/lib_dom/*.o sdk/lib_dom/*.d
	@rm -f sdk/lib_http/*.o sdk/lib_http/*.d
	@rm -f app/*.o app/*.d
	@rm -f kernel.elf equos.iso
ifeq ($(call is_nocleaned,sdl2),)
	@rm -f third_party/sdl2/libSDL2.a
	@find third_party/sdl2 -name '*.o' -delete -o -name '*.d' -delete
endif
ifeq ($(call is_nocleaned,bearssl),)
	@rm -f third_party/bearssl/libbearssl.a
	@find third_party/bearssl -name '*.o' -delete -o -name '*.d' -delete
endif
ifeq ($(call is_nocleaned,quickjs),)
	@rm -f third_party/quickjs/libquickjs.a
	@find third_party/quickjs -name '*.o' -delete -o -name '*.d' -delete
endif
ifeq ($(call is_nocleaned,sysgui),)
	$(MAKE) -C app/sysgui clean
endif
ifeq ($(call is_nocleaned,lvgl)$(call is_nocleaned,lgvl),)
	@rm -f third_party/lvgl/liblvgl.a
	@find third_party/lvgl -name '*.o' -delete -o -name '*.d' -delete
endif
endif

generate_manifest: apps
	python make_manifest.py

create_hdd: kernel.elf apps $(ACTIVE_DOOM)
	@echo --- Generating EXT2 hdd.img ---
	python WINDOWS_ext2.py

iso: kernel.elf apps $(ACTIVE_DOOM) generate_manifest
	@$(call RM_F,equos.iso)
	xorriso -as mkisofs -no-pad -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label -o equos.iso $(ISO_ROOT)
	limine bios-install equos.iso

QEMU       := qemu-system-x86_64
QEMU_BASE  := -m 512M -boot d -cpu qemu64,+rdrand,+rdseed,+aes \
              -drive file=hdd.img,format=raw,index=0,media=disk -cdrom equos.iso \
              -netdev user,id=n0,hostfwd=tcp::2222-:22 -device rtl8139,netdev=n0 \
              -device pci-ohci,id=ohci -device usb-ehci,id=ehci -device qemu-xhci,id=xhci \
              -device ac97,audiodev=snd0 -audiodev dsound,id=snd0
QEMU_ACCEL := -accel whpx,kernel-irqchip=off -accel kvm -accel hvf -accel tcg
QEMU_NODISK := -m 512M -boot d -cpu qemu64,+rdrand,+rdseed,+aes \
              -cdrom equos.iso \
              -netdev user,id=n0,hostfwd=tcp::2222-:22 -device rtl8139,netdev=n0 \
              -device pci-ohci,id=ohci -device usb-ehci,id=ehci -device qemu-xhci,id=xhci \
              -device ac97,audiodev=snd0 -audiodev dsound,id=snd0

run:
	$(QEMU) $(QEMU_BASE) -serial mon:stdio $(QEMU_ACCEL)

run-usb:
	$(QEMU) $(QEMU_BASE) -serial mon:stdio -device usb-mouse,bus=uhci.0 $(QEMU_ACCEL)

run-tcg:
	$(QEMU) $(QEMU_BASE) -serial mon:stdio -accel tcg

run-log:
	$(QEMU) $(QEMU_BASE) -serial file:boot_serial.log $(QEMU_ACCEL)

run-debug:
	$(QEMU) $(QEMU_BASE) -serial mon:stdio -d int,guest_errors,mmu -D qemu.log

run-nodisk:
	$(QEMU) $(QEMU_NODISK) -serial mon:stdio $(QEMU_ACCEL)

cleanrun: clean all run

-include $(KERNEL_OBJS:.o=.d)
-include $(SDK_OBJS:.o=.d)
-include $(APP_SRCS:.c=.d)
-include $(LVGL_OBJS:.o=.d)