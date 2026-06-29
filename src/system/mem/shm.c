// src/system/shm.c
#include "shm.h"
#include "../../syslibc/string.h"
#include "../mem/pmm.h"
#include "../usr/task.h"
#include "../mem/vmm.h"

extern void term_print(const char *str);

#define MAX_SHM_SEGMENTS 128
#define SHM_START_VADDR 0xD0000000000 // Высокий адрес в юзерспейсе для SHM

typedef struct {
  uint32_t key;       // Уникальный ID (например, PID процесса)
  uint64_t phys_addr; // Начальный физический адрес
  uint32_t page_count;
  bool used;
} shm_segment_t;

static shm_segment_t shm_table[MAX_SHM_SEGMENTS];

void shm_init() {
  memset(shm_table, 0, sizeof(shm_table));
  term_print("[SHM] Shared Memory Subsystem initialized.\n");
}

// Поиск или создание сегмента
uint64_t sys_shm_get(uint32_t key, uint32_t size) {
  uint32_t pages = (size + 4095) / 4096;
  int slot = -1;

  // 1. Ищем, не создан ли уже такой ключ
  for (int i = 0; i < MAX_SHM_SEGMENTS; i++) {
    if (shm_table[i].used && shm_table[i].key == key) {
      slot = i;
      break;
    }
  }

  // 2. Если не нашли — создаем новый сегмент
  if (slot == -1) {
    for (int i = 0; i < MAX_SHM_SEGMENTS; i++) {
      if (!shm_table[i].used) {
        void *phys = pmm_alloc_continuous(pages);
        if (!phys)
          return 0;

        shm_table[i].key = key;
        shm_table[i].phys_addr = (uint64_t)phys;
        shm_table[i].page_count = pages;
        shm_table[i].used = true;

        // Обнуляем память, чтобы не было мусора от прошлых программ
        memset((void *)VIRT(phys), 0, pages * 4096);
        slot = i;
        break;
      }
    }
  }

  if (slot == -1)
    return 0;

  // 3. Мапим этот сегмент в текущий процесс
  // Мы выделяем виртуальный адрес внутри процесса
  uint64_t virt_addr =
      SHM_START_VADDR +
      (slot * 0x1000000); // Даем по 16МБ зазора между сегментами

  page_table_t *pml4 = (page_table_t *)VIRT(current_task->process->cr3);
  for (uint32_t p = 0; p < shm_table[slot].page_count; p++) {
    vmm_map(pml4, virt_addr + (p * 4096),
            shm_table[slot].phys_addr + (p * 4096),
            PTE_PRESENT | PTE_USER | PTE_WRITABLE);
  }

  return virt_addr;
}