/**
 * ============================================================================
 * GERENCIADOR DE HEAP DO KERNEL - NÚCLEO RING 0 
 * ============================================================================
 */

#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

/* O Heap começa em 3GB para não bater com o resto do sistema */
#define HEAP_START_SAFE 0xC0000000ULL
#define LIMITE_HEAP     (64 * 1024 * 1024) 
#define ENDERECO_MAX    (HEAP_START_SAFE + LIMITE_HEAP)

/* Funções exportadas para o Kernel */
void heap_init();
void* kmalloc(size_t tamanho);
void kfree(void* ponteiro);
void kmalloc_stats(size_t* usado, size_t* livre);
void get_system_memory_info(size_t* used, size_t* free);

#endif
