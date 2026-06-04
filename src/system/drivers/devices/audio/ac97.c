#include "ac97.h"
#include "../../../core/io.h"
#include "../../../mem/pmm.h"
#include "../../../../syslibc/string.h"

static uint32_t bar_nam; 
static uint32_t bar_nab; 
static ac97_bdl_t* bdl;  
extern uint64_t hhdm_offset;
static int current_bdl_idx = 0;

// Готовность звуковой карты. AC'97 инициализируется внутри pci_init(), который
// при быстрой загрузке (DEFER_HW_INIT) выполняется в фоновом потоке ПОСЛЕ старта
// рабочего стола. Этот флаг позволяет sysgui дождаться готовности карты прежде
// чем проигрывать звук запуска (иначе запись шла бы в неинициализированные
// регистры и звука не было бы).
static volatile int ac97_ready_flag = 0;
int ac97_is_ready(void) { return ac97_ready_flag; }

uint8_t ac97_get_civ() {
    return inb(bar_nab + 0x14); 
}


int ac97_can_write() {
    uint8_t civ = inb(bar_nab + 0x14); // Текущий играющий индекс
    // Если мы на 2 слота впереди того, что играет карта — подождем
    int next = (current_bdl_idx);
    if (next == civ) return 0; // Очередь полна
    return 1;
}

void ac97_init(uint32_t nam, uint32_t nab) {
    bar_nam = nam & ~0x1;
    bar_nab = nab & ~0x1;

    // 1. Cold Reset
    outl(bar_nab + 0x2C, 0); 
    for(int i = 0; i < 1000; i++) inb(0x80);
    outl(bar_nab + 0x2C, 1 << 1); 

    // 2. Wait for Ready
    int timeout = 100000;
    while (!(inl(bar_nab + 0x30) & 0x100) && timeout--) inb(0x80);

    // 3. Открываем Master и PCM громкость
    outw(bar_nam + 0x02, 0x0000); 
    outw(bar_nam + 0x04, 0x0000); 
    outw(bar_nam + 0x18, 0x0000); 

    // 4. Настройка частоты (44100 Hz для Doom)
    uint16_t ext_id = inw(bar_nam + 0x28); 
    if (ext_id & 1) { 
        outw(bar_nam + 0x2A, inw(bar_nam + 0x2A) | 1); // VRA On
        outw(bar_nam + 0x2C, 0xAC44); // 44100 Hz
    }

    // 5. Инициализация BDL (32 слота)
    uint64_t bdl_phys = (uintptr_t)pmm_alloc();
    bdl = (ac97_bdl_t*)(bdl_phys + hhdm_offset);
    memset(bdl, 0, 4096);
    
    // Выделяем одну страницу тишины для безопасной инициализации BDL
    uint64_t silent_page_phys = (uintptr_t)pmm_alloc();
    memset((void*)(silent_page_phys + hhdm_offset), 0, 4096);

    for(int i=0; i<32; i++) {
        bdl[i].pointer = (uint32_t)silent_page_phys;
        bdl[i].length = 4; 
        bdl[i].flags = (1 << 15); // Только прерывание
    }

    outl(bar_nab + 0x10, (uint32_t)bdl_phys);
    outb(bar_nab + 0x15, 0); // LVI = 0

    ac97_ready_flag = 1; // карта готова — sysgui может проигрывать звук
}

// src/drivers/audio/ac97.c
void ac97_play_at_idx(int idx, void* phys_addr, uint32_t len) {
    // ФИКС МУТА: Снимаем аппаратный мут, который мог остаться после ac97_stop()
    outw(bar_nam + 0x02, 0x0000); // Размутируем Master Volume (0x0000 = макс. громкость)

    bdl[idx].pointer = (uint32_t)(uintptr_t)phys_addr;
    bdl[idx].length = (uint16_t)(len / 2); 
    bdl[idx].flags = (1 << 15); 

    outw(bar_nab + 0x16, 0x1C); // Чистим статус
    
    outb(bar_nab + 0x15, idx); 

    if (!(inb(bar_nab + 0x1B) & 0x01)) {
        outb(bar_nab + 0x1B, 0x01); 
    }
}

void ac97_set_rate(uint32_t rate) {
    uint16_t ext_id = inw(bar_nam + 0x28);
    if (ext_id & 1) {
        outw(bar_nam + 0x2A, inw(bar_nam + 0x2A) | 1); // VRA On
        outw(bar_nam + 0x2C, (uint16_t)rate);
    }
}
void ac97_stop() {
    outb(bar_nab + 0x1B, 0x00); // Stop DMA
    outw(bar_nam + 0x02, 0x8000); // Mute
}

void ac97_set_bdl_entry(int idx, void* phys_addr) {
    bdl[idx].pointer = (uint32_t)(uintptr_t)phys_addr;
    bdl[idx].length = 4; // Плеер проиграет 4 семпла тишины
    bdl[idx].flags = (1 << 15);
}