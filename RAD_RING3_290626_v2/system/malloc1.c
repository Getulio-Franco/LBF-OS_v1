#include "malloc.h"
#include "liblib.h"
#include "string.h"

static block_header_t* heap_start = NULL;
static block_header_t* heap_end   = NULL;

/* ========================================================================
 * FUNÇÕES AUXILIARES (Garantem a organização do Heap) v0.0.2 220426
 * ======================================================================== */

// Divide um bloco grande em dois, se sobrar espaço suficiente
static void split_block(block_header_t* block, size_t size) {
    if (block->size >= size + HEADER_SIZE + MIN_SPLIT) {
        block_header_t* new_free = (block_header_t*)((uint8_t*)block + HEADER_SIZE + size);
        
        new_free->size = block->size - size - HEADER_SIZE;
        new_free->is_free = 1;
        new_free->padding = 0;
        
        new_free->next = block->next;
        new_free->prev = block;
        
        if (new_free->next) {
            new_free->next->prev = new_free;
        } else {
            heap_end = new_free;
        }

        block->size = size;
        block->next = new_free;
    }
}

// Une blocos livres adjacentes para evitar fragmentação
static void coalesce(block_header_t* block) {
    if (!block) return;

    // Tenta unir com o próximo
    if (block->next && block->next->is_free) {
        block->size += HEADER_SIZE + block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        } else {
            heap_end = block;
        }
    }

    // Tenta unir com o anterior
    if (block->prev && block->prev->is_free) {
        block_header_t* p = block->prev;
        p->size += HEADER_SIZE + block->size;
        p->next = block->next;
        if (block->next) {
            block->next->prev = p;
        } else {
            heap_end = p;
        }
    }
}

/* ========================================================================
 * API PRINCIPAL
 * ======================================================================== */

void* malloc(size_t size) {
    if (size == 0) return NULL;

    size = ALIGN16(size);

    // 1. Busca em lista ligada (First-fit)
    block_header_t* curr = heap_start;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            split_block(curr, size);
            curr->is_free = 0;
            return (void*)((uint8_t*)curr + HEADER_SIZE);
        }
        curr = curr->next;
    }

    // 2. Expansão do Heap via Syscall 45 (SBRK)
    size_t total_needed = size + HEADER_SIZE;
    size_t req_amount = (total_needed > CHUNK_SIZE) ? ALIGN16(total_needed) : CHUNK_SIZE;

    void* ptr = sys_sbrk((intptr_t)req_amount);
    if (ptr == (void*)-1 || ptr == NULL) return NULL;

    // --- CORREÇÃO DE SEGURANÇA APLICADA ---
    // Zera o cabeçalho para evitar ponteiros 'next/prev' com lixo
    memset(ptr, 0, HEADER_SIZE); 

    block_header_t* new_b = (block_header_t*)ptr;
    new_b->size = req_amount - HEADER_SIZE;
    new_b->is_free = 0;

    // Lógica de ancoragem na lista duplamente ligada
    if (heap_start == NULL) {
        heap_start = new_b;
        new_b->prev = NULL;
    } else {
        new_b->prev = heap_end;
        if (heap_end) heap_end->next = new_b;
    }
    heap_end = new_b;

    split_block(new_b, size);

    return (void*)((uint8_t*)new_b + HEADER_SIZE);
}

void free(void* ptr) {
    if (!ptr) return;

    // Volta 32 bytes para achar o cabeçalho
    block_header_t* block = (block_header_t*)((uint8_t*)ptr - HEADER_SIZE);
    block->is_free = 1;
    
    // Limpa a memória por segurança (opcional, ajuda no debug)
    // memset(ptr, 0, block->size);

    coalesce(block);
}

void* calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void* ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    size = ALIGN16(size);
    block_header_t* block = (block_header_t*)((uint8_t*)ptr - HEADER_SIZE);

    // Caso 1: O bloco atual já é grande o suficiente
    if (block->size >= size) {
        split_block(block, size);
        return ptr;
    }

    // Caso 2: Tenta expandir combinando com o próximo bloco livre
    if (block->next && block->next->is_free && 
        (block->size + HEADER_SIZE + block->next->size) >= size) {
        
        block->size += HEADER_SIZE + block->next->size;
        block->next = block->next->next;
        
        if (block->next) block->next->prev = block;
        else heap_end = block;

        split_block(block, size);
        return ptr;
    }

    // Caso 3: Realmente precisa de um novo lugar
    void* new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size);
        free(ptr);
    }
    return new_ptr;
}
