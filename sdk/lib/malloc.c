#include "../include/equos.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct block_header {
  size_t size;
  int free;
  struct block_header *next;
} block_header_t;

static block_header_t *free_list = NULL;
static uint64_t current_brk = 0;

// Инициализация кучи через системный вызов BRK
static block_header_t *request_space(size_t size) {
  if (current_brk == 0) {
    current_brk = _syscall(15, 0, 0, 0, 0, 0); // Получаем текущий адрес
  }

  block_header_t *block = (block_header_t *)current_brk;
  size_t total_size = size + sizeof(block_header_t);

  // Запрашиваем у ядра новую границу памяти
  uint64_t new_brk = current_brk + total_size;
  if (_syscall(15, new_brk, 0, 0, 0, 0) == (uint64_t)-1)
    return NULL;

  block->size = size;
  block->free = 0;
  block->next = NULL;
  current_brk = new_brk;
  return block;
}

void *malloc(size_t size) {
  if (size <= 0)
    return NULL;
  size = (size + 15) & ~15; // Выравнивание 16 байт

  // Ищем свободный блок
  block_header_t *curr = free_list;
  while (curr) {
    if (curr->free && curr->size >= size) {
      curr->free = 0;
      return (void *)(curr + 1);
    }
    curr = curr->next;
  }

  // Если не нашли — просим у ядра
  block_header_t *block = request_space(size);
  if (!block)
    return NULL;

  // Добавляем в список
  if (!free_list) {
    free_list = block;
  } else {
    curr = free_list;
    while (curr->next)
      curr = curr->next;
    curr->next = block;
  }

  return (void *)(block + 1);
}

void free(void *ptr) {
  if (!ptr)
    return;
  block_header_t *block = (block_header_t *)ptr - 1;
  block->free = 1;
  // В идеале тут нужно "склеивать" соседние свободные блоки,
  // но для начала работы Lua этого уже достаточно, чтобы память не утекала в
  // бесконечность.
}

void *realloc(void *ptr, size_t size) {
  if (!ptr)
    return malloc(size);
  block_header_t *block = (block_header_t *)ptr - 1;
  if (block->size >= size)
    return ptr;

  void *new_ptr = malloc(size);
  if (new_ptr) {
    memcpy(new_ptr, ptr, block->size);
    free(ptr);
  }
  return new_ptr;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}