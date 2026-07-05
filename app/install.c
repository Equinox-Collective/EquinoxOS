#include "stdio.h"
#include "string.h"

// Объявляем сигнатуру системного вызова из asm-файла
extern uint64_t _syscall(uint64_t num, uint64_t rdi, uint64_t rsi, uint64_t rdx, uint64_t rcx, uint64_t r8);

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("\n=========================================\n");
    printf("      EquinoxOS Installer Trigger        \n");
    printf("=========================================\n\n");
    printf("This tool will trigger the OS installation process inside the kernel.\n");
    printf("Target partition will be formatted as EXT2.\n");
    printf("Limine bootloader will be installed to EFI System Partition (ESP).\n\n");
    printf("Starting installation... Please do not power off your PC.\n\n");

    // Дергаем наш сисколл 107
    int rc = (int)_syscall(107, 0, 0, 0, 0, 0);

    if (rc == 0) {
        printf("\n[SUCCESS] Installation finished successfully!\n");
        printf("You can now safely reboot your PC and boot from the HDD/SSD.\n");
    } else {
        printf("\n[ERROR] Installation failed during kernel execution.\n");
    }

    return 0;
}