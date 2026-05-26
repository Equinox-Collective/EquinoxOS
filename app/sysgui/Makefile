CC = x86_64-elf-gcc
LD = x86_64-elf-ld

SDK_DIR = ../../sdk
SDK_OBJS = $(wildcard $(SDK_DIR)/lib/*.o)

CFLAGS = -ffreestanding -mcmodel=small -mno-red-zone -fno-stack-protector -fno-pic -g \
		-fno-omit-frame-pointer -I$(SDK_DIR)/include -Ilua -O2 -MMD -MP

LDFLAGS = -nostdlib -Ttext=0x1000000 -e _start

LUA_SRCS = $(wildcard lua/l*.c)
GUI_SRCS = main.c api_gui.c

LUA_OBJS = $(LUA_SRCS:.c=.o)
GUI_OBJS = $(GUI_SRCS:.c=.o)

all: sysgui.elf

sysgui.elf: $(LUA_OBJS) $(GUI_OBJS)
	$(LD) $(LDFLAGS) $(SDK_OBJS) $(LUA_OBJS) $(GUI_OBJS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

ifeq ($(OS),Windows_NT)
    RM = del /f /q
    CLEAN_FILES = $(subst /,\,$(LUA_OBJS) $(GUI_OBJS) sysgui.elf)
else
    RM = rm -f
    CLEAN_FILES = $(LUA_OBJS) $(GUI_OBJS) sysgui.elf
endif

clean:
	-$(RM) $(CLEAN_FILES)