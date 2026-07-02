#include <stddef.h>
#include <stdbool.h>
#include "vmm.h"
#include "pmm.h" 
#include "util/string.h" 
#include <stdint.h>

/**
 * @brief Navega ou cria tabelas de páginas conforme necessário.
 * @note Proteção contra sobreposição de Huge Pages incluída.
 */
/**
 * @brief Navega ou cria tabelas de páginas conforme necessário.
 * @note Versão corrigida para suportar múltiplos processos em Ring 3.
 */
uint64_t* get_next_table(uint64_t* table, uint64_t index) {
    if (table[index] & PAGE_PRESENT) {
        // SEGURANÇA: Se for uma Huge Page, não podemos criar sub-tabelas nela.
        if (table[index] & PAGE_HUGE) {
            return 0; 
        }

        /**
         * CORREÇÃO CRUCIAL:
         * Se a tabela já existe, garantimos que ela tenha as permissões USER e WRITE.
         * Isso permite que processos de usuário "atravessem" essa tabela para acessar
         * suas próprias páginas lá no final da hierarquia (PT).
         */
        table[index] |= PAGE_USER | PAGE_WRITE;

        // Retorna o endereço físico (que no Identity Mapping é igual ao virtual)
        return (uint64_t*)(table[index] & 0x000FFFFFFFFFF000ULL);

    } else {
        // Aloca nova página para a tabela via PMM
        void* new_table_phys = pmm_alloc_block(); 
        if (!new_table_phys) return 0; 

        uint64_t* new_table_ptr = (uint64_t*)new_table_phys;
        
        /**
         * OTIMIZAÇÃO: 
         * Usar memset é mais rápido que um loop 'for' manual para zerar a tabela.
         */
        memset(new_table_ptr, 0, 4096);

        /**
         * Define as flags na tabela PAI:
         * PAGE_USER: Essencial para que o Ring 3 acesse qualquer coisa abaixo deste nível.
         * PAGE_WRITE: Permite escrita (o controle fino fica na última tabela, a PT).
         */
        table[index] = ((uint64_t)new_table_phys) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        
        return new_table_ptr;
    }
}

/**
 * @brief Mapeia 2MB de memória virtual para física.
 */
void vmm_map_page_2mb_ext(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    uint64_t pml4_idx = (virt_addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt_addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt_addr >> 21) & 0x1FF;

    uint64_t* pml4 = (uint64_t*)PML4_ADDR;

    uint64_t* pdpt = get_next_table(pml4, pml4_idx);
    if (!pdpt) return;

    uint64_t* pd = get_next_table(pdpt, pdpt_idx);
    if (!pd) return;

    // Aplica o mapeamento de 2MB
    pd[pd_idx] = (phys_addr & ~0x1FFFFF) | flags | PAGE_HUGE;

    __asm__ volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");
}

/**
 * @brief Mapeia 4KB de memória virtual para física.
 */
void vmm_map_page_4kb(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    virt_addr &= ~0xFFF; // Alinhamento 4KB
    phys_addr &= ~0xFFF; 

    uint64_t pml4_idx = (virt_addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt_addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt_addr >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt_addr >> 12) & 0x1FF; 

    uint64_t* pml4 = (uint64_t*)PML4_ADDR;

    uint64_t* pdpt = get_next_table(pml4, pml4_idx);
    if (!pdpt) return;

    uint64_t* pd = get_next_table(pdpt, pdpt_idx);
    if (!pd) return; // Retornará 0 se encontrar uma Huge Page de 2MB aqui

    uint64_t* pt = get_next_table(pd, pd_idx);
    if (!pt) return;

    pt[pt_idx] = phys_addr | flags;

    __asm__ volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");
}

/**
 * @brief NOVO: Mapeia uma área contígua inteira (Excelente para Buffers de Vídeo)
 */
void vmm_map_area(uint64_t virt_addr, uint64_t phys_addr, size_t size, uint64_t flags) {
    // Alinha o tamanho para múltiplos de 4KB
    size_t aligned_size = (size + 0xFFF) & ~0xFFFULL;
    
    for (size_t i = 0; i < aligned_size; i += 4096) {
        vmm_map_page_4kb(virt_addr + i, phys_addr + i, flags);
    }
}

void vmm_init_identity() {
    uint64_t* pml4 = (uint64_t*)PML4_ADDR;
    for(int i = 0; i < 512; i++) pml4[i] = 0;

    // A MÁGICA AQUI: Mapeie 2GB de RAM em vez de apenas 256MB!
    // 0x80000000 = 2 Gigabytes.
    // Usar páginas de 2MB para isso vai consumir apenas algumas dezenas de KBs
    // para as tabelas de página. É super seguro e resolve o crash do kmalloc.
    for (uint64_t addr = 0; addr < 0x80000000ULL; addr += 0x200000) {
        vmm_map_page_2mb_ext(addr, addr, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
    }

    // Mapeamento do Vídeo (Mantemos exatamente como o seu original que funciona)
    uint32_t fb_address = *((uint32_t*)0x508);
    uint64_t fb_aligned = (uint64_t)fb_address & ~0x1FFFFFULL; 
    
    // Mapeia 32MB dedicados para o Framebuffer com PCD (Sem Cache)
    for (uint64_t i = 0; i < 0x2000000; i += 0x200000) {
        uint64_t v_addr = fb_aligned + i;
       // vmm_map_page_2mb_ext(v_addr, v_addr, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
        vmm_map_page_2mb_ext(v_addr, v_addr, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD | PAGE_USER);
    }

    __asm__ volatile("mov %0, %%cr3" :: "r"((uint64_t)PML4_ADDR) : "memory");
}

// include/vmm.c - adicionado na versáo V0.0.3 V6

/// Função auxiliar para ler o CR3
uint64_t read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void write_cr3(uint64_t cr3) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

int vmm_is_mapped(uint64_t virt_addr) {
    // 1. Pega o endereço físico da tabela PML4 atual
    uint64_t* pml4 = (uint64_t*)(read_cr3() & ~0xFFFULL);

    uint64_t pml4_idx = (virt_addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt_addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt_addr >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt_addr >> 12) & 0x1FF;

    // Nível 4
    if (!(pml4[pml4_idx] & 1)) return 0;
    
    // Nível 3
    uint64_t* pdpt = (uint64_t*)(pml4[pml4_idx] & ~0xFFFULL);
    if (!(pdpt[pdpt_idx] & 1)) return 0;
    
    // Nível 2
    uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & ~0xFFFULL);
    if (!(pd[pd_idx] & 1)) return 0;

    // Se for Huge Page (2MB), o bit 7 está ligado
    if (pd[pd_idx] & (1 << 7)) return 1;

    // Nível 1
    uint64_t* pt = (uint64_t*)(pd[pd_idx] & ~0xFFFULL);
    if (!(pt[pt_idx] & 1)) return 0;

    return 1;
}

// Esta função agora recebe explicitamente o diretório de páginas (pml4)
void vmm_map_page_to_pml4(uint64_t* pml4, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    virt_addr &= ~0xFFFULL;
    phys_addr &= ~0xFFFULL;

    uint64_t pml4_idx = (virt_addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt_addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt_addr >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt_addr >> 12) & 0x1FF;

    uint64_t* pdpt = get_next_table(pml4, pml4_idx);
    if (!pdpt) return;

    uint64_t* pd   = get_next_table(pdpt, pdpt_idx);
    if (!pd) return;

    uint64_t* pt   = get_next_table(pd, pd_idx);
    if (!pt) return;

    pt[pt_idx] = phys_addr | flags;
    
    if ((uint64_t)pml4 == (read_cr3() & ~0xFFFULL)) {
        __asm__ volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");
    }
}

bool vmm_map_user(uint64_t cr3, uint64_t virt, uint64_t phys) {
    // 1. SALVA O CR3 DO USUÁRIO E MUDA PARA O KERNEL
    // Isso garante que o Kernel tenha acesso ao mapeamento de identidade (0-2GB)
    uint64_t old_cr3 = read_cr3();
    if (old_cr3 != PML4_ADDR) {
        write_cr3(PML4_ADDR);
    }

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t* pml4 = (uint64_t*)cr3;

    // --- Nível 1: PML4 -> PDPT ---
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        void* new_tab = pmm_alloc_block();
        if (!new_tab) { if (old_cr3 != PML4_ADDR) write_cr3(old_cr3); return false; }
        memset(new_tab, 0, 4096);
        pml4[pml4_idx] = (uint64_t)new_tab | 0x07; // P | W | U
    }
    uint64_t* pdpt = (uint64_t*)(pml4[pml4_idx] & ~0xFFFULL);

    // --- Nível 2: PDPT -> PD ---
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        void* new_tab = pmm_alloc_block();
        if (!new_tab) { if (old_cr3 != PML4_ADDR) write_cr3(old_cr3); return false; }
        memset(new_tab, 0, 4096);
        pdpt[pdpt_idx] = (uint64_t)new_tab | 0x07; // P | W | U
    }
    uint64_t* pd = (uint64_t*)(pdpt[pdpt_idx] & ~0xFFFULL);

    // --- Nível 3: PD -> PT ---
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        void* new_tab = pmm_alloc_block();
        if (!new_tab) { if (old_cr3 != PML4_ADDR) write_cr3(old_cr3); return false; }
        memset(new_tab, 0, 4096);
        pd[pd_idx] = (uint64_t)new_tab | 0x07; // P | W | U
    }
    uint64_t* pt = (uint64_t*)(pd[pd_idx] & ~0xFFFULL);

    // --- Nível 4: PT -> Frame Físico ---
    pt[pt_idx] = (phys & ~0xFFFULL) | 0x07; // P | W | U

    // 2. RESTAURA O CR3 DO USUÁRIO
    if (old_cr3 != PML4_ADDR) {
        write_cr3(old_cr3);
    }

    // Invalida TLB para garantir que a CPU veja a mudança
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");

    return true;
}
