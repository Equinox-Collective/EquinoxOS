#ifndef OHCI_H
#define OHCI_H

#include <stdint.h>

// --- ОФСЕТЫ РЕГИСТРОВ OHCI ---
#define OHCI_REG_REVISION          0x00
#define OHCI_REG_CONTROL           0x04
#define OHCI_REG_COMMAND_STATUS    0x08
#define OHCI_REG_INTERRUPT_STATUS  0x0C
#define OHCI_REG_INTERRUPT_ENABLE  0x10
#define OHCI_REG_INTERRUPT_DISABLE 0x14
#define OHCI_REG_HCCA              0x18
#define OHCI_REG_CONTROL_HEAD_ED   0x20
#define OHCI_REG_BULK_HEAD_ED      0x28
#define OHCI_REG_FM_INTERVAL       0x34
#define OHCI_REG_PERIODIC_START    0x40
#define OHCI_REG_LS_THRESHOLD      0x44
#define OHCI_REG_RH_DESCRIPTOR_A   0x48
#define OHCI_REG_RH_STATUS         0x50
#define OHCI_REG_RH_PORT_STATUS1   0x54

// Флаги сброса и управления
#define OHCI_CMD_HCR               (1 << 0)  // HostControllerReset
#define OHCI_CTRL_USB_OPERATIONAL  (2 << 6)  // Operational state (bits 7-6)

void ohci_init(uint8_t bus, uint8_t slot, uint8_t func, uintptr_t mmio_base);

#endif