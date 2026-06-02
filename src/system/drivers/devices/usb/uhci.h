#ifndef UHCI_H
#define UHCI_H

#include <stdint.h>

// --- I/O РЕГИСТРЫ UHCI (Смещения относительно io_base) ---
#define UHCI_REG_USBCMD    0x00 // Command (2 bytes)
#define UHCI_REG_USBSTS    0x02 // Status (2 bytes)
#define UHCI_REG_USBINTR   0x04 // Interrupt Enable (2 bytes)
#define UHCI_REG_FRNUM     0x06 // Frame Number (2 bytes)
#define UHCI_REG_FRBASEADD 0x08 // Frame List Base Address (4 bytes)
#define UHCI_REG_SOFMOD    0x0C // Start of Frame Modify (1 byte)
#define UHCI_REG_PORTSC1   0x10 // Port 1 Status/Control (2 bytes)
#define UHCI_REG_PORTSC2   0x12 // Port 2 Status/Control (2 bytes)

// --- КОМАНДЫ USBCMD ---
#define USBCMD_RS          (1 << 0) // Run/Stop
#define USBCMD_HCRESET     (1 << 1) // Host Controller Reset
#define USBCMD_GRESET      (1 << 2) // Global Reset
#define USBCMD_EGPR        (1 << 3) // Enter Global Suspend Mode
#define USBCMD_FGR         (1 << 4) // Force Global Resume
#define USBCMD_SWDBG       (1 << 5) // Software Debug
#define USBCMD_CF          (1 << 6) // Configure Flag (1 = Max packets)
#define USBCMD_MAXP        (1 << 7) // Max Packet (0 = 32 bytes, 1 = 64 bytes)

// --- СТАТУСЫ USBSTS ---
#define USBSTS_USBINT      (1 << 0) // USB Interrupt
#define USBSTS_ERROR       (1 << 1) // USB Error Interrupt
#define USBSTS_RD          (1 << 2) // Resume Detect
#define USBSTS_HCHALTED    (1 << 5) // Host Controller Halted
#define USBSTS_HCPROCESSOR (1 << 4) // Host Controller Process Error

void uhci_init(uint8_t bus, uint8_t slot, uint8_t func, uint32_t io_base);

#endif