#ifndef AC97_H
#define AC97_H

#include <stdint.h>

// Регистры NAB (Native Audio Bus)
#define AC97_PCM_IN_BUF     0x00
#define AC97_PCM_OUT_BUF    0x10   // Нам нужен этот для вывода
#define AC97_MIC_IN_BUF     0x20

// Смещения внутри PCM_OUT (относительно BAR1)
#define AC97_PO_BDBAR       0x10   // Buffer Descriptor List Base Address
#define AC97_PO_LVI         0x15   // Last Valid Index
#define AC97_PO_SR          0x16   // Status Register
#define AC97_PO_CR          0x1B   // Control Register

// Регистры NAM (Native Audio Mixer - BAR0)
#define AC97_MASTER_VOLUME  0x02
#define AC97_PCM_VOLUME     0x18

// Структура дескриптора буфера (8 байт)
typedef struct {
    uint32_t pointer;   // Физический адрес буфера с семплами
    uint16_t length;    // Количество семплов (не байт!)
    uint16_t flags;     // Бит 15: прерывание по завершению, Бит 14: последний в списке
} __attribute__((packed)) ac97_bdl_t;

void ac97_init(uint32_t nam, uint32_t nab);
void ac97_play_at_idx(int idx, void* phys_addr, uint32_t len);
void ac97_stop();
int  ac97_is_ready(void);


#endif