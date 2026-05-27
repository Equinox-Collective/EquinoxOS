#include "rtl8139.h"
#include "net.h"
#include "../../../mem/memory.h"
#include "../../../core/io.h"
#include "../../../fs/vfs.h"
#include "../../../../syslibc/string.h"
#include "../../../mem/vmm.h"

extern void term_print(const char* str);

// --- RTL8139 REGISTERS ---
#define REG_MAC         0x00
#define REG_TSD0        0x10 
#define REG_TSAD0       0x20 
#define REG_RBSTART     0x30 
#define REG_COMMAND     0x37
#define REG_CAPR        0x38 
#define REG_IMR         0x3C 
#define REG_ISR         0x3E 
#define REG_TCR         0x40 
#define REG_RCR         0x44 
#define REG_CONFIG1     0x52

#define TX_BUFFER_BASE  0x80000

/* RTL8139 RX ring sizing
 * ---------------------
 * The chip supports four ring sizes via RCR bits 11..12 (RBLEN):
 *   00 → 8K  + 16
 *   01 → 16K + 16
 *   10 → 32K + 16
 *   11 → 64K + 16
 *
 * In WRAP=1 mode (RCR bit 7, set below) the chip is allowed to write
 * up to one maximum-Ethernet-frame (1500 B + 4 B FCS + 4 B status =
 * ~1508 B) PAST the nominal ring end. That tail must be backed by
 * memory or the next packet straddling the boundary scribbles over
 * whatever lives there.
 *
 * We bumped the ring from 8K → 16K because the 8K ring overflowed
 * during back-to-back TLS records (BearSSL hands us bursts of 4–6
 * MTU-sized packets per handshake step) and the resulting CAPR
 * underflow left the RX engine wedged — subsequent DNS queries
 * timed out because the response sat in the FIFO but never made it
 * into our buffer. 16K + 1500 = 17916 B reserved at 0x90000, well
 * below TX_BUFFER_BASE (0x80000 + 4 × 512 = 0x80800 fits the TX
 * descriptors; the RX region 0x90000 … 0x944FB doesn't overlap). */
#define RX_BUFFER_BASE  0x90000
#define RX_BUFFER_SIZE  16384
#define RX_WRAP_PAD     1500       /* extra room when WRAP=1            */
#define RCR_RBLEN_16K   (1u << 11) /* RCR bits 11..12 = 01              */

static uint32_t rtl_io_base = 0;
static int tx_cur_desc = 0;
static uint8_t* rx_buffer_phys = (uint8_t*)RX_BUFFER_BASE; 
static uint16_t rx_offset = 0;

static net_interface_t rtl_iface;

void rtl8139_init(uint32_t bar0) {
    rtl_io_base = bar0 & ~3;

    /* Zero the RX area up to the wrap pad so leftover bytes from the
     * boot loader / Limine don't get parsed as packet headers if the
     * chip happens to read CAPR before our first ROK. */
    memset((void*)VIRT(RX_BUFFER_BASE), 0, RX_BUFFER_SIZE + RX_WRAP_PAD);

    outb(rtl_io_base + REG_CONFIG1, 0x00);
    /* AAP+APM+AM+AB + WRAP + RBLEN=16K. Same bit semantics as the old
     * 0x8F write, just with the 16K bit OR'd in. */
    outl(rtl_io_base + REG_RCR, 0x0000000F | (1 << 7) | RCR_RBLEN_16K);

    outb(rtl_io_base + REG_COMMAND, 0x10);
    while((inb(rtl_io_base + REG_COMMAND) & 0x10) != 0);

    outl(rtl_io_base + REG_RBSTART, (uint32_t)(uintptr_t)rx_buffer_phys);
    outl(rtl_io_base + REG_RCR, 0x0F | (1 << 7) | RCR_RBLEN_16K);

    outb(rtl_io_base + REG_COMMAND, 0x0C);
    outl(rtl_io_base + REG_TCR, 0x03000000);
    outw(rtl_io_base + REG_IMR, 0x0005); // Enable ROK and TOK bits

    memset(&rtl_iface, 0, sizeof(net_interface_t));
    // ... rest of init ...
    strcpy(rtl_iface.name, "eth0");
    for (int i = 0; i < 6; i++) {
        rtl_iface.mac[i] = inb(rtl_io_base + REG_MAC + i);
    }
    rtl_iface.ip = 0x0A00020F; // 10.0.2.15
    rtl_iface.subnet_mask = 0xFFFFFF00;
    rtl_iface.gateway_ip = 0x0A000202;
    rtl_iface.send = rtl8139_send_packet;

    net_register_interface(&rtl_iface);

    term_print("[NET] RTL8139 Hardware Ready (eth0).\n");
}

void rtl8139_send_packet(void* data, uint32_t len) {
    uint32_t send_len = (len < 60) ? 60 : len;
    
    // CPU access via VIRT
    uint8_t* current_tx_virt = (uint8_t*)VIRT(TX_BUFFER_BASE + (tx_cur_desc * 512));
    uint32_t current_tx_phys = TX_BUFFER_BASE + (tx_cur_desc * 512);

    memcpy(current_tx_virt, data, len);
    if (len < 60) memset(current_tx_virt + len, 0, 60 - len);

    __asm__ volatile("" : : : "memory");

    uint32_t tsad_reg = REG_TSAD0 + (tx_cur_desc * 4);
    uint32_t tsd_reg  = REG_TSD0  + (tx_cur_desc * 4);

    outl(rtl_io_base + tsad_reg, current_tx_phys); 
    outl(rtl_io_base + tsd_reg, send_len | 0x0000); 

    tx_cur_desc = (tx_cur_desc + 1) % 4; 
}

void rtl8139_receive() {
    uint16_t isr_status = inw(rtl_io_base + REG_ISR);
    if (!(isr_status & 0x01)) return;

    /* Ack ROK + RER (so a bad CRC frame doesn't latch the line). */
    outw(rtl_io_base + REG_ISR, 0x05);

    uint8_t* rx_buffer_virt = (uint8_t*)VIRT(rx_buffer_phys);

    /* BUFE bit (REG_COMMAND bit 0) — set when CAPR == CBR, i.e. the
     * software pointer caught up with the chip's write pointer. */
    while (!(inb(rtl_io_base + REG_COMMAND) & 0x01)) {
        uint16_t* header = (uint16_t*)(rx_buffer_virt + rx_offset);
        uint16_t status  = header[0];
        uint16_t length  = header[1];

        /* Header status: bit 0 ROK, others = error flags. If the chip
         * dropped a malformed frame in the ring, reset the receiver
         * rather than walking past garbage that will look like a
         * 1536+ length on the next iteration. */
        if (!(status & 0x01) || length < 20 || length > 1518 + 4) {
            term_print("[NET] rtl8139 RX desync — resetting receiver\n");
            outb(rtl_io_base + REG_COMMAND, 0x10); /* RX reset */
            while ((inb(rtl_io_base + REG_COMMAND) & 0x10) != 0) { }
            outl(rtl_io_base + REG_RBSTART,
                 (uint32_t)(uintptr_t)rx_buffer_phys);
            outl(rtl_io_base + REG_RCR,
                 0x0F | (1 << 7) | RCR_RBLEN_16K);
            outb(rtl_io_base + REG_COMMAND, 0x0C);
            rx_offset = 0;
            return;
        }

        uint8_t* packet = rx_buffer_virt + rx_offset + 4;

        net_handle_packet(&rtl_iface, packet, length - 4);

        /* Advance over 4-byte header + payload + FCS, 4-byte aligned. */
        rx_offset = (rx_offset + length + 4 + 3) & ~3;
        if (rx_offset >= RX_BUFFER_SIZE) rx_offset -= RX_BUFFER_SIZE;

        /* CAPR convention: chip uses CAPR + 16 = read pointer. So we
         * write (rx_offset - 16) modulo RX_BUFFER_SIZE. The old code
         * just wrote `rx_offset - 16`, which underflows to 0xFFF0
         * the instant rx_offset reaches zero — the chip then thinks
         * the read pointer is at 0xFFE0 (past end of any plausible
         * buffer) and stops delivering. */
        uint16_t capr = (uint16_t)((rx_offset + RX_BUFFER_SIZE - 16)
                                   % RX_BUFFER_SIZE);
        outw(rtl_io_base + REG_CAPR, capr);
    }
}

uint32_t rtl8139_vfs_write(struct vfs_node* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    (void)node; (void)offset; 
    rtl8139_send_packet(buffer, size);
    return size;
}

void rtl8139_install_vfs() {
    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    memset(node, 0, sizeof(vfs_node_t));
    
    strcpy(node->name, "net");
    node->write = rtl8139_vfs_write;
    node->flags = 2; 
    
    vfs_register_device(node);
}

bool rtl8139_has_data() {
    uint16_t status = inw(rtl_io_base + 0x3E); 
    return (status & 0x01); 
}