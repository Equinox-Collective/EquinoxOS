#ifndef EHCI_H
#define EHCI_H

#include <stdint.h>

void ehci_init(uint8_t bus, uint8_t slot, uint8_t func, uintptr_t mmio_base);

#endif