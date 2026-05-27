#ifndef DNS_H
#define DNS_H

#include "net.h"
#include <stdint.h>

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t questions;
    uint16_t answers;
    uint16_t authority;
    uint16_t additional;
} __attribute__((packed)) dns_header_t;

/* server_ip is in "a in MSB" form, the same convention used everywhere else
 * in the kernel network stack. Pass e.g. 0x08080808 for 8.8.8.8,
 * 0x01010101 for 1.1.1.1, 0x09090909 for 9.9.9.9.
 *
 * Resets the per-call result before sending so the caller can poll
 * dns_get_result() and trust that a non-zero value is from THIS query. */
void dns_query(net_interface_t* iface, const char* hostname, uint32_t server_ip);
uint32_t dns_get_result(const char* hostname); // Returns 0 if not resolved yet

#endif
