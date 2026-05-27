#include "dns.h"
#include "udp.h"
#include "../../../../syslibc/string.h"
#include "../../../../syslibc/stdio.h"

extern void term_print(const char* str);

uint32_t resolved_ip = 0;
char last_queried_name[128];

// Converts "google.com" to "\x06google\x03com\x00"
static void dns_format_name(uint8_t* qname, const char* host) {
    int lock = 0;
    char temp[256];
    
    if (strlen(host) > 250) return;
    
    strcpy(temp, host);
    strcat(temp, ".");
    
    for (int i = 0; i < (int)strlen(temp); i++) {
        if (temp[i] == '.') {
            *qname++ = i - lock;
            for (; lock < i; lock++) {
                *qname++ = temp[lock];
            }
            lock++;
        }
    }
    *qname++ = '\0';
}

/* Walk one DNS name in wire format starting at `ptr`. Handles both raw
 * label sequences ("\x07example\x03com\x00") and compression pointers
 * (the two-byte form starting with 0b11xxxxxx). Returns a pointer to
 * the byte immediately past the name, OR `end` if the name walks off
 * the packet. Does NOT follow the pointer destination — we don't need
 * the decoded name, only its on-wire length. */
static uint8_t* dns_skip_name(uint8_t* ptr, uint8_t* end) {
    while (ptr < end) {
        uint8_t b = *ptr;
        if (b == 0) {
            return ptr + 1;            /* end of name */
        }
        if ((b & 0xC0) == 0xC0) {
            /* Compression pointer: 2-byte form, no further labels follow. */
            return (ptr + 2 <= end) ? ptr + 2 : end;
        }
        /* Plain label: 1 length byte + `b` label bytes. */
        if (ptr + 1 + b >= end) return end;
        ptr += 1 + b;
    }
    return end;
}

static void dns_callback(uint32_t src_ip, uint16_t src_port, uint8_t* data, uint32_t len) {
    (void)src_ip; (void)src_port;
    if (!data || len < sizeof(dns_header_t)) return;
    dns_header_t* dns = (dns_header_t*)data;

    uint16_t nq  = HTONS(dns->questions);
    uint16_t nan = HTONS(dns->answers);
    if (nan == 0) {
        term_print("[DNS] Error: No answers in response.\n");
        return;
    }

    uint8_t* ptr = data + sizeof(dns_header_t);
    uint8_t* end = data + len;

    /* Skip the question section: QNAME + QTYPE(2) + QCLASS(2) per record.
     * Old code assumed exactly one question and used `while (*ptr != 0)`
     * which mis-handles a question that starts with a compression pointer
     * (legal per RFC 1035, even if 8.8.8.8 doesn't currently do it). */
    for (uint16_t i = 0; i < nq && ptr < end; i++) {
        ptr = dns_skip_name(ptr, end);
        ptr += 4;                      /* QTYPE + QCLASS */
    }

    /* Iterate ALL answer records and pick the first A/IN with rdlen 4.
     * Real-world responses for popular hosts (example.com, anything
     * behind a CDN) often start with a CNAME chain before the A records,
     * and EDNS / OPT pseudo-RRs may appear later. The previous version
     * blindly decoded the first answer as an A record, which is how we
     * ended up dialling 8.6.112.6 for example.com — those four bytes
     * happened to land inside a CNAME's RDATA. */
    for (uint16_t i = 0; i < nan && ptr + 10 <= end; i++) {
        ptr = dns_skip_name(ptr, end);
        if (ptr + 10 > end) break;
        uint16_t type  = ((uint16_t)ptr[0] << 8) | ptr[1];
        /* uint16_t class = ((uint16_t)ptr[2] << 8) | ptr[3]; */
        uint16_t rdlen = ((uint16_t)ptr[8] << 8) | ptr[9];
        ptr += 10;
        if (ptr + rdlen > end) break;
        if (type == 1 /* A */ && rdlen == 4) {
            resolved_ip = ((uint32_t)ptr[0] << 24) | ((uint32_t)ptr[1] << 16) |
                          ((uint32_t)ptr[2] << 8)  |  (uint32_t)ptr[3];
            char buf[128];
            sprintf(buf, "[DNS] Resolved %s to %d.%d.%d.%d\n", last_queried_name,
                    (resolved_ip >> 24) & 0xFF, (resolved_ip >> 16) & 0xFF,
                    (resolved_ip >> 8) & 0xFF, resolved_ip & 0xFF);
            term_print(buf);
            return;
        }
        ptr += rdlen;
    }

    /* If we got here, response had answers but none were A records. */
    term_print("[DNS] No A record in response.\n");
}

void dns_query(net_interface_t* iface, const char* hostname) {
    if (!iface) return;
    
    static bool bound = false;
    if (!bound) {
        udp_bind(53535, dns_callback); // Ephemeral port for DNS responses
        bound = true;
    }
    
    resolved_ip = 0;
    strcpy(last_queried_name, hostname);
    
    uint8_t buffer[512];
    memset(buffer, 0, 512);
    
    dns_header_t* dns = (dns_header_t*)buffer;
    dns->id = HTONS(0xBEEF);
    dns->flags = HTONS(0x0100); // Standard Query, Recursion Desired
    dns->questions = HTONS(1);
    
    uint8_t* qname = buffer + sizeof(dns_header_t);
    dns_format_name(qname, hostname);
    
    int qname_len = strlen((char*)qname) + 1;
    uint8_t* qinfo = qname + qname_len;
    qinfo[1] = 1; // Type A
    qinfo[3] = 1; // Class IN
    
    uint32_t total_len = sizeof(dns_header_t) + qname_len + 4;
    
    // Send to Google DNS (8.8.8.8)
    udp_send_packet(iface, 0x08080808, 53535, 53, buffer, total_len);
    
    char buf[128];
    sprintf(buf, "[DNS] Querying 8.8.8.8 for %s...\n", hostname);
    term_print(buf);
}

uint32_t dns_get_result(const char* hostname) {
    if (strcmp(last_queried_name, hostname) == 0) return resolved_ip;
    return 0;
}
