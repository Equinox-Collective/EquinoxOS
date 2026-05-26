CC = x86_64-elf-gcc
LD = x86_64-elf-ld
ASM = nasm

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
SRC_DIRS = src src/boot src/gui src/syslibc \
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

# --- DOOM ---
DOOM_DIR = app/doom
DOOM_SRCS = $(wildcard $(DOOM_DIR)/*.c)
DOOM_OBJS = $(patsubst $(DOOM_DIR)/%.c, $(OBJ_DIR)/doom/%.o, $(DOOM_SRCS))

# --- MAIN RULES ---

# Полный билд (как раньше) — собирает всё и генерит hdd.img.
all: setup kernel.elf apps doom.elf create_hdd iso

# CI-вариант — без hdd.img. Нужен PR-чекам, чтобы они не ждали 64 МБ ext2
# и не зависели от состояния iso_root/. Артефактом всё равно остаётся .iso.
ci: setup kernel.elf apps doom.elf iso

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
kernel.elf: $(KERNEL_OBJS)
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

doom.elf: $(SDK_OBJS) $(DOOM_OBJS)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) $(DOOM_OBJS) -o $(ISO_ROOT)/bin/doom.elf

# --- APPS BUILD RULES --------------------------------------------------------
APP_SRCS = $(wildcard app/*.c)
APP_OBJS = $(patsubst app/%.c,app/%.o,$(APP_SRCS))
APP_ELFS_SIMPLE = $(ISO_ROOT)/bin/snake.elf $(ISO_ROOT)/bin/bmpview.elf $(ISO_ROOT)/bin/htmlview.elf $(ISO_ROOT)/bin/niplay.elf $(ISO_ROOT)/bin/widget_demo.elf $(ISO_ROOT)/bin/ipc_test.elf

apps: $(SDK_OBJS) $(APP_ELFS_SIMPLE) sysgui_app

$(ISO_ROOT)/bin/%.elf: app/%.o $(SDK_OBJS)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) $< -o $@

app/%.o: app/%.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

sysgui_app:
	@echo "=== Building sysgui (enGUI) ==="
	$(MAKE) -C app/sysgui
	@$(call CP_F,app/sysgui/sysgui.elf,iso_root/bin/sysgui.elf)
	@$(call MKDIR_P,iso_root/res/sysgui)
	@$(call CP_F,app/sysgui/scripts/init.lua,iso_root/res/sysgui/init.lua)

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
else
clean:
	@rm -rf $(OBJ_DIR)
	@rm -f sdk/lib/*.o sdk/lib/*.d
	@rm -f app/*.o app/*.d
	@rm -f kernel.elf equos.iso
	@rm -f app/sysgui/sysgui.elf
endif

create_hdd:
	@echo --- Generating EXT2 hdd.img ---
	python WINDOWS_ext2.py

iso:
	@$(call RM_F,equos.iso)
	xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot EFI/BOOT/limine-bios-cd.bin -efi-boot-part --efi-boot-image -o equos.iso $(ISO_ROOT)

run:
	qemu-system-x86_64 -m 512M -boot d -drive file=hdd.img,format=raw,index=0,media=disk -cdrom equos.iso -serial stdio -netdev user,id=n0,hostfwd=tcp::2222-:22 -device rtl8139,netdev=n0 -device ac97,audiodev=snd0 -audiodev dsound,id=snd0 -d int,guest_errors,mmu -D qemu.log 

cleanrun: clean all run

# Include dependency files
-include $(KERNEL_OBJS:.o=.d)
-include $(SDK_OBJS:.o=.d)
-include $(APP_SRCS:.c=.d)
