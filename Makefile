CC = x86_64-elf-gcc
LD = x86_64-elf-ld
ASM = nasm

# --- НАСТРОЙКИ ПУТЕЙ ---
OBJ_DIR = obj
SDK_LIB_DIR = sdk/lib
ISO_ROOT = iso_root
APP_BINARY = $(ISO_ROOT)/app.elf

# --- ФЛАГИ ЯДРА ---
CFLAGS = -ffreestanding -O2 -Wall -Wextra -fno-exceptions -std=c11 \
         -Isrc -Isrc/drivers -Isrc/shell -Isrc/boot/limine \
         -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -fno-stack-protector -fno-pic -g

LDFLAGS = -nostdlib -T src/linker.ld -z max-page-size=0x1000
ASMFLAGS = -f elf64

# --- ФЛАГИ SDK (ПРИЛОЖЕНИЯ) ---
SDK_INC = -I./sdk/include
USER_CFLAGS = -ffreestanding -mcmodel=small -mno-red-zone -fno-stack-protector -fno-pic -g -fno-omit-frame-pointer $(SDK_INC)
# Объекты SDK и Ядра
SDK_OBJS = $(SDK_LIB_DIR)/crt0.o $(SDK_LIB_DIR)/stdio.o $(SDK_LIB_DIR)/string.o $(SDK_LIB_DIR)/eid.o  $(SDK_LIB_DIR)/posix.o $(SDK_LIB_DIR)/malloc.o
OBJ = $(OBJ_DIR)/kernel.o $(OBJ_DIR)/io.o $(OBJ_DIR)/keyboard.o $(OBJ_DIR)/rtl8139.o $(OBJ_DIR)/vfs.o $(OBJ_DIR)/gui.o $(OBJ_DIR)/syscall.o $(OBJ_DIR)/ac97.o \
      $(OBJ_DIR)/gdt_flush.o $(OBJ_DIR)/idt.o $(OBJ_DIR)/stdio.o $(OBJ_DIR)/pci.o $(OBJ_DIR)/pmm.o $(OBJ_DIR)/shell.o $(OBJ_DIR)/eqstart.o \
      $(OBJ_DIR)/pic.o $(OBJ_DIR)/interrupt.o $(OBJ_DIR)/timer.o $(OBJ_DIR)/ata.o $(OBJ_DIR)/bmp.o $(OBJ_DIR)/task.o $(OBJ_DIR)/fat32.o $(OBJ_DIR)/serial.o \
      $(OBJ_DIR)/memory.o $(OBJ_DIR)/fs.o $(OBJ_DIR)/vesa.o $(OBJ_DIR)/mouse.o $(OBJ_DIR)/string.o $(OBJ_DIR)/panic.o $(OBJ_DIR)/vmm.o $(OBJ_DIR)/gdt.o \
      $(OBJ_DIR)/pcspeaker.o $(OBJ_DIR)/terminal.o $(OBJ_DIR)/ext2.o $(OBJ_DIR)/ext2_tests.o

all: setup kernel.elf compile_app create_hdd

setup:
	@if not exist $(OBJ_DIR) mkdir $(OBJ_DIR)

kernel.elf: $(OBJ)
	$(LD) $(LDFLAGS) $(OBJ) -o kernel.elf

# --- СБОРКА ЯДРА ---
$(OBJ_DIR)/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/system/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/drivers/screen/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/drivers/keyboard/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/shell/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/drivers/disk/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/fs/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/drivers/vga/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/drivers/serial/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/drivers/audio/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/libc/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/io/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/gui/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/drivers/mouse/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/drivers/pci/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/drivers/net/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o: src/drivers/pcspeaker/%.c
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/gdt_flush.o: src/system/gdt_flush.asm
	$(ASM) $(ASMFLAGS) $< -o $@
$(OBJ_DIR)/interrupt.o: src/system/interrupt.asm
	$(ASM) $(ASMFLAGS) $< -o $@

# --- СБОРКА SDK ---
$(SDK_LIB_DIR)/%.o: $(SDK_LIB_DIR)/%.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SDK_LIB_DIR)/%.o: $(SDK_LIB_DIR)/%.asm
	$(ASM) -f elf64 $< -o $@

# --- СБОРКА ПРИЛОЖЕНИЯ ---

compile_app: $(SDK_OBJS)
	$(CC) $(USER_CFLAGS) -c app/snake.c -o app/snake.o
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) app/snake.o -o $(ISO_ROOT)/snake.elf
	$(CC) $(USER_CFLAGS) -c app/htmlview.c -o app/htmlview.o
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) app/htmlview.o -o $(ISO_ROOT)/htmlview.elf
	$(CC) $(USER_CFLAGS) -c app/niplay.c -o app/niplay.o
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) app/niplay.o -o $(ISO_ROOT)/niplay.elf
	$(CC) $(USER_CFLAGS) -c app/bmpview.c -o app/bmpview.o
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) app/bmpview.o -o $(ISO_ROOT)/bmpview.elf
	$(CC) $(USER_CFLAGS) -c app/niplay.c -o app/niplay.o
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) app/niplay.o -o $(ISO_ROOT)/niplay.elf

# --- ОЧИСТКА ---
clean:
	@if exist $(OBJ_DIR) rmdir /s /q $(OBJ_DIR)
	@if exist app\snake.o del /q app\snake.o
	@if exist app\htmlview.o del /q app\htmlview.o
	@if exist sdk\lib\*.o del /q sdk\lib\*.o
	@if exist kernel.elf del /q kernel.elf
	@if exist iso_root\app.elf del /q iso_root\app.elf
	@if exist equos.iso del /q equos.iso
	@if exist packets.pcap del /q packets.pcap
	@if exist app\niplay.o del /q app\niplay.o
	@if exist app\bmpview.o del /q app\bmpview.o
	@if exist app\htmlview.o del /q app\htmlview.o


cleanrun: clean all copykernel compile_app create_hdd iso run

create_hdd:
	@echo --- Generating EXT2 hdd.img ---
	python WINDOWS_ext2.py

copykernel:
	copy /Y kernel.elf $(ISO_ROOT)\kernel.elf

iso:
	xorriso -as mkisofs -b limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot limine-bios-cd.bin -efi-boot-part --efi-boot-image -o equos.iso $(ISO_ROOT)

run:
	qemu-system-x86_64 -m 512M -boot d -drive file=hdd.img,format=raw,index=0,media=disk -cdrom equos.iso -serial stdio -netdev user,id=n0,hostfwd=tcp::2222-:22 -device rtl8139,netdev=n0 -device ac97,audiodev=snd0 -audiodev dsound,id=snd0 -d int,guest_errors,mmu -D qemu.log 

DOOM_DIR = app/doom
DOOM_SRCS = $(wildcard $(DOOM_DIR)/*.c)
DOOM_OBJS = $(patsubst $(DOOM_DIR)/%.c, $(OBJ_DIR)/doom/%.o, $(DOOM_SRCS))

# Создаем папку для объектников дума
setup_doom:
	@if not exist $(OBJ_DIR)\doom mkdir $(OBJ_DIR)\doom

# Правило компиляции каждого файла Дума
$(OBJ_DIR)/doom/%.o: $(DOOM_DIR)/%.c
	$(CC) $(USER_CFLAGS) -DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400 -DFEATURE_SOUND -c $< -o $@

# Линковка Doom
doom.elf: setup_doom $(SDK_OBJS) $(DOOM_OBJS)
	$(LD) -nostdlib -Ttext=0x1000000 -e _start $(SDK_OBJS) $(DOOM_OBJS) -o $(ISO_ROOT)/doom.elf
