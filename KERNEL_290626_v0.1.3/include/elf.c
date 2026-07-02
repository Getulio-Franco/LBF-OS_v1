#include "elf.h"
#include "fs/fat32_file.h"
#include "mem/vmm.h"
#include "mem/pmm.h"
#include "mem/heap.h"
#include "util/string.h"
#include "drivers/video.h"
#include "drivers/proc.h"
#include <stdbool.h> 

#define USER_STACK_VADDR 0x2000000
#define USER_STACK_PAGES 4 

void elf_unload_failed_pml4(uint64_t* pml4_phys) {
    if (!pml4_phys || (uint64_t)pml4_phys == (read_cr3() & ~0xFFFULL)) return; 
    
    uint64_t* pml4 = (uint64_t*)pml4_phys;
    
    // 1. Limpa a área recém-isolada que criamos na entrada 0
    if (pml4[0] & PAGE_PRESENT) {
        uint64_t* pdpt = (uint64_t*)(pml4[0] & ~0xFFFULL);
        if (pdpt[0] & PAGE_PRESENT) {
            uint64_t* pd = (uint64_t*)(pdpt[0] & ~0xFFFULL);
            
            // Libera as tabelas de páginas do usuário (índices 2 a 15)
            for (int j = 2; j <= 15; j++) {
                if (pd[j] & PAGE_PRESENT) {
                    uint64_t* pt = (uint64_t*)(pd[j] & ~0xFFFULL);
                    
                    // =========================================================================
                    // CORREÇÃO CRÍTICA: LIBERA AS PÁGINAS FÍSICAS DE RAM (FIM DO VAZAMENTO!)
                    // =========================================================================
                    for (int k = 0; k < 512; k++) {
                        if (pt[k] & PAGE_PRESENT) {
                            uint64_t phys_page = pt[k] & ~0xFFFULL;
                            // Só libera se não for endereço nulo e estiver na área de usuário
                            if (phys_page != 0) {
                                pmm_free_block((void*)phys_page); 
                            }
                        }
                    }
                    
                    pmm_free_block(pt); // Libera a tabela de páginas em si
                }
            }
            pmm_free_block(pd);
        }
        pmm_free_block(pdpt);
    }

    // 2. Limpa as outras áreas de usuário
    for (int i = 1; i < 256; i++) {
        if (pml4[i] & PAGE_PRESENT) {
            uint64_t* pdpt = (uint64_t*)(pml4[i] & ~0xFFFULL);
            
            // Se houver diretórios aninhados aqui, o ideal é limpá-los também.
            // Para garantir que o PDPT seja liberado:
            pmm_free_block(pdpt);
        }
    }
    pmm_free_block(pml4_phys);
}

uint64_t elf_load_file(const char* path, uint64_t* pml4_dest) {
    Elf64_Ehdr elf_header;
    
    // Debug de leitura
    vga_print_string("[ELF] Abrindo: ", 0, 18);
    
    if (fat32_read_file_at_offset(path, (uint8_t*)&elf_header, sizeof(Elf64_Ehdr), 0) != FS_SUCCESS) {
        vga_print_string(" -> Erro: FSTAB (Arquivo nao existe)", 30, 18);
        return 0;
    }

    if (!elf_is_valid(&elf_header)) {
        vga_print_string(" -> Erro: Header Invalido", 30, 18);
        return 0;
    }

    uint32_t phdr_size = elf_header.e_phnum * elf_header.e_phentsize;
    Elf64_Phdr* phdr = (Elf64_Phdr*)kmalloc(phdr_size);
    fat32_read_file_at_offset(path, (uint8_t*)phdr, phdr_size, elf_header.e_phoff);

    // 1. Mapeamento dos Segmentos (Apenas prepara as tabelas de página)
    vga_print_string("[ELF] Mapeando Segmentos...", 0, 19);
    for (int i = 0; i < elf_header.e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            uint64_t page_start = phdr[i].p_vaddr & ~0xFFFULL;
            uint64_t page_end = (phdr[i].p_vaddr + phdr[i].p_memsz + 0xFFF) & ~0xFFFULL;
            for (uint64_t curr = page_start; curr < page_end; curr += 4096) {
                if (curr < 0x100000) continue; 

                void* phys = pmm_alloc_block();
                memset(phys, 0, 4096);
                vmm_map_page_to_pml4(pml4_dest, curr, (uint64_t)phys, PAGE_PRESENT | PAGE_USER | PAGE_WRITE);
            }
        }
    }

    // 2. Pilha de Usuário
    for (uint64_t i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t vaddr = (USER_STACK_VADDR - (USER_STACK_PAGES * 4096)) + (i * 4096);
        vmm_map_page_to_pml4(pml4_dest, vaddr, (uint64_t)pmm_alloc_block(), PAGE_PRESENT | PAGE_USER | PAGE_WRITE);
    }

    // --- BLOCO DE ESCRITA SEGURA ---
    // Desativamos interrupções para que o scheduler não interrompa a troca de CR3
// --- BLOCO DE ESCRITA SEGURA REFINADO ---
    __asm__ volatile("cli");

    for (int i = 0; i < elf_header.e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            
            // 1. Se o segmento tem dados no arquivo (Código ou Globais Inicializadas)
            if (phdr[i].p_filesz > 0) {
                uint8_t* temp_buffer = (uint8_t*)kmalloc(phdr[i].p_filesz);
                if (temp_buffer) {
                    fat32_read_file_at_offset(path, temp_buffer, phdr[i].p_filesz, phdr[i].p_offset);

                    uint64_t old_cr3 = read_cr3();
                    write_cr3((uint64_t)pml4_dest);

                    memcpy((void*)phdr[i].p_vaddr, temp_buffer, phdr[i].p_filesz);

                    write_cr3(old_cr3);
                    kfree(temp_buffer);
                }
            }

            // 2. TRATAMENTO DE VARIÁVEIS GLOBAIS (.BSS)
            // Se o tamanho na memória for maior que no arquivo, limpamos o excesso.
            if (phdr[i].p_memsz > phdr[i].p_filesz) {
                elf_clear_bss(pml4_dest, phdr[i].p_vaddr, phdr[i].p_filesz, phdr[i].p_memsz);
            }
        }
    }
    
    __asm__ volatile("sti");
    // -------------------------------

   /* vga_print_string(" -> OK", 45, 42);
    vga_print_string("Entry Point: ", 0, 43);
    vga_print_hex_64(elf_header.e_entry, 15, 43); // Use sua função de imprimir hex    
   */
    kfree(phdr);
    return elf_header.e_entry;
}

uint64_t create_elf_process(const char* filename) {
    uint64_t* new_pml4 = (uint64_t*)pmm_alloc_block();
    if (!new_pml4) return 0;
    memset(new_pml4, 0, 4096);

    uint64_t* kernel_pml4 = (uint64_t*)(read_cr3() & ~0xFFFULL);

    // 1. Mapear o Kernel (High Half) - Mantém as interrupções vivas
    for(int i = 256; i < 512; i++) {
        new_pml4[i] = kernel_pml4[i];
    }

    // 2. Isolamento do Identity Mapping (0 a 512GB)
    uint64_t* kernel_pdpt = (uint64_t*)(kernel_pml4[0] & ~0xFFFULL);
    
    uint64_t* new_pdpt = (uint64_t*)pmm_alloc_block();
    memset(new_pdpt, 0, 4096);
    new_pml4[0] = ((uint64_t)new_pdpt) | PAGE_PRESENT | PAGE_USER | PAGE_WRITE; 

    for (int i = 0; i < 512; i++) {
        if (kernel_pdpt[i] & PAGE_PRESENT) {
            
            if (i == 0) {
                // Área de 0 a 1GB (Aqui fica o Kernel, Heap e o nosso ELF)
                uint64_t* new_pd = (uint64_t*)pmm_alloc_block();
                memset(new_pd, 0, 4096);
                new_pdpt[0] = ((uint64_t)new_pd) | PAGE_PRESENT | PAGE_USER | PAGE_WRITE;

                uint64_t* kernel_pd = (uint64_t*)(kernel_pdpt[0] & ~0xFFFULL);
                
                for (int j = 0; j < 512; j++) {
                    // O ELF é carregado a partir de 4MB (índice 2). Limpamos até 32MB (índice 15).
                    if (j >= 2 && j <= 15) {
                        new_pd[j] = 0; // Isolado: Nenhuma sujeira do kernel passa pra cá
                    } else if (kernel_pd[j] & PAGE_PRESENT) {
                        // O resto (Heap, etc) recebe permissão de usuário para evitar crashes
                        new_pd[j] = kernel_pd[j] | PAGE_USER; 
                    }
                }
            } else {
                // Áreas > 1GB (Aqui fica o Framebuffer de Vídeo!)
                // Como corrigimos o vmm.c, a tabela original já tem PAGE_USER nas folhas
                new_pdpt[i] = kernel_pdpt[i] | PAGE_USER;
            }
        }
    }

    // 3. Carrega o ELF usando o nosso novo mapa limpo
    uint64_t entry = elf_load_file(filename, new_pml4);
    
    if (entry > 0) {
        // Agora todos nascem orgulhosamente no Ring 3!
        return create_process((void(*)())entry, 3, filename, (uint64_t)new_pml4);
    } else {
        elf_unload_failed_pml4(new_pml4);
        return 0; 
    }
}

bool elf_is_valid(Elf64_Ehdr* header) {
    // Verifica os 4 primeiros bytes (Magic Number: 0x7F, 'E', 'L', 'F')
    if (header->e_ident[EI_MAG0] != 0x7F ||
        header->e_ident[EI_MAG1] != 'E'  ||
        header->e_ident[EI_MAG2] != 'L'  ||
        header->e_ident[EI_MAG3] != 'F') {
        return false;
    }

    // Verifica se é um arquivo de 64 bits
    if (header->e_ident[EI_CLASS] != ELFCLASS64) {
        return false;
    }

    // Verifica se é Little Endian
    if (header->e_ident[EI_DATA] != ELFDATA2LSB) {
        return false;
    }

    // Verifica se é um executável (ET_EXEC) ou biblioteca compartilhada (ET_DYN)
    if (header->e_type != ET_EXEC && header->e_type != ET_DYN) {
        return false;
    }

    return true;
}

// Esta função limpa o resto da página se o tamanho na memória for maior que no arquivo
void elf_clear_bss(uint64_t* pml4_dest, uint64_t vaddr, uint64_t file_size, uint64_t mem_size) {
    if (mem_size <= file_size) return;

    uint64_t bss_start = vaddr + file_size;
    uint64_t bss_size = mem_size - file_size;

    // Salva o CR3 atual para voltar depois
    uint64_t old_cr3 = read_cr3();
    write_cr3((uint64_t)pml4_dest);

    // Zera explicitamente a memória global (BSS)
    // Embora o mapeamento inicial já zere páginas inteiras, 
    // o final de uma seção .data pode terminar no meio de uma página.
    memset((void*)bss_start, 0, bss_size);

    write_cr3(old_cr3);
}
