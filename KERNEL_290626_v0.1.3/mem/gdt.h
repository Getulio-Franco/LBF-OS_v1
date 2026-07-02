#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/**
 * @struct gdt_entry_t
 * @brief Entrada padrão de 8 bytes da GDT.
 */
struct gdt_entry_struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));
typedef struct gdt_entry_struct gdt_entry_t;

/**
 * @struct gdt_ptr_t
 * @brief Estrutura passada para a instrução LGDT.
 */
struct gdt_ptr_struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));
typedef struct gdt_ptr_struct gdt_ptr_t;

/**
 * @struct tss_t
 * @brief Estrutura TSS de 64 bits (Task State Segment).
 * No x86_64, seu papel principal é fornecer o RSP0 durante interrupções.
 */
struct tss_struct {
    uint32_t reserved0;
    uint64_t rsp0;      // Pilha de Kernel (Ring 0) para interrupções vindo do Ring 3
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];    // Interrupt Stack Table (opcional, mas potente)
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset; // Offset para o Bitmap de permissão de E/S
} __attribute__((packed));
typedef struct tss_struct tss_t;

// Protótipos de Inicialização
void gdt_init();
void tss_set_stack(uint64_t stack);
void enter_user_mode(uint64_t entry_point, uint64_t user_stack);

#endif
