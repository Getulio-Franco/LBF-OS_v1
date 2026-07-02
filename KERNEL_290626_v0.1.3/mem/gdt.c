/*
1. A Matemática dos SeletoresNo proc.c, definimos:User Code: 0x1BUser Data: 0x23No seu gdt.c, os slots são:Slot 3 (User Code): $3 \times 8 = 24$ (ou 0x18 em hexadecimal).Slot 4 (User Data): $4 \times 8 = 32$ (ou 0x20 em hexadecimal).O Sincronismo: Quando você faz 0x18 | 3 (índice + Ring 3), o resultado é exatamente 0x1B. O mesmo vale para o dado (0x20 | 3 = 0x23). Isso garante que o processador saiba que aqueles segmentos pertencem ao modo usuário.2. O TSS: A Ponte de RetornoA sua implementação do TSS está correta para 64 bits. Como o descritor de TSS no x86_64 tem 16 bytes (em vez dos 8 bytes normais), ele ocupa dois slots na GDT (5 e 6).Você usou o Slot 5 para a parte baixa.Manipulou o Slot 6 manualmente para a parte alta (bits 32-63 do endereço).O comando ltr 0x28 (Slot 5 * 8) no seu assembly está correto para carregar essa estrutura.Por que isso é vital? Sem o tss_set_stack funcionando em conjunto com o schedule do proc.c, o seu programa em Ring 3 daria um crash total no primeiro milissegundo em que tentasse voltar para o Kernel (via Syscall ou Interrupção), porque a CPU não saberia onde "aterrizar" a pilha.3. Pequena Observação de SegurançaNotei que você alinhou a gdt_entries e a kernel_tss com __attribute__((aligned(16))). Isso é excelente. O x86_64 adora alinhamentos de 16 bytes, e isso evita falhas de barramento imprevisíveis.Resumo da Verificação:ComponenteStatusObservaçãoKernel Code/Data✅ OKSlots 1 (0x08) e 2 (0x10).User Code/Data✅ OKSlots 3 (0x18) e 4 (0x20), compatíveis com RPL 3.TSS Setup✅ OKDescritor de 16 bytes corretamente espalhado nos slots 5 e 6.IRETQ Switch✅ OKO assembly de gdt_init recarrega os seletores de forma limpa.
*/

#include "gdt.h"
#include "util/string.h"
#include "drivers/video.h"

// 8 slots: Null(0), K-Code(1), K-Data(2), U-Code(3), U-Data(4), TSS-Low(5), TSS-High(6), Slot Extra(7)
gdt_entry_t gdt_entries[8] __attribute__((aligned(16)));
gdt_ptr_t   gdt_ptr;
tss_t       kernel_tss __attribute__((aligned(16)));

static void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entries[num].base_low    = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high   = (base >> 24) & 0xFF;

    gdt_entries[num].limit_low   = (limit & 0xFFFF);
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;
    gdt_entries[num].granularity |= gran & 0xF0;
    gdt_entries[num].access      = access;
}

void install_tss() {
    uint64_t base = (uint64_t)&kernel_tss;
    uint32_t limit = sizeof(tss_t) - 1;

    memset(&kernel_tss, 0, sizeof(tss_t));
    
    uint64_t current_rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(current_rsp));
    
    kernel_tss.rsp0 = current_rsp; 
    kernel_tss.iopb_offset = sizeof(tss_t);

    // Slot 5: Parte baixa
    gdt_set_gate(5, (uint32_t)base, limit, 0x89, 0x00);

    // Slot 6: Parte alta (descritor de 16 bytes em x86_64)
    uint32_t *tss_high = (uint32_t*)&gdt_entries[6];
    tss_high[0] = (uint32_t)(base >> 32); 
    tss_high[1] = 0; 
}

// CORREÇÃO: Usar a variável global correta 'kernel_tss'
/*void tss_set_stack(uint64_t stack) {
    kernel_tss.rsp0 = stack; 
}*/

//verifica se a variavel foi modificada para debug 260326
void tss_set_stack(uint64_t stack) {
    if (stack == 0) {
        vga_print_string("ERRO: TSS RSP0 SET TO ZERO!", 0, 0);
        return;
    }
    kernel_tss.rsp0 = stack; 
}

void gdt_init() {
    // CORREÇÃO: Usar 8 slots para garantir segurança de memória no x86_64
    gdt_ptr.limit = (sizeof(gdt_entry_t) * 8) - 1;
    gdt_ptr.base  = (uint64_t)&gdt_entries;

    memset(gdt_entries, 0, sizeof(gdt_entries));

    gdt_set_gate(0, 0, 0, 0, 0);                // 0x00: Null
    gdt_set_gate(1, 0, 0xFFFFF, 0x9A, 0xAF);    // 0x08: Kernel Code
    gdt_set_gate(2, 0, 0xFFFFF, 0x92, 0xCF);    // 0x10: Kernel Data
    gdt_set_gate(3, 0, 0xFFFFF, 0xFA, 0xAF);    // 0x18: User Code 
    gdt_set_gate(4, 0, 0xFFFFF, 0xF2, 0xCF);    // 0x20: User Data 

    install_tss();

    __asm__ volatile (
        "lgdt %0\n\t"
        "mov %%rsp, %%rax\n\t"
        "pushq $0x10\n\t"             
        "pushq %%rax\n\t"             
        "pushfq\n\t"                  
        "pushq $0x08\n\t"             
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"             
        "iretq\n\t"                   
        "1:\n\t"
        "mov $0x10, %%ax\n\t"         
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov $0x28, %%ax\n\t"         // 0x28 = (Slot 5 * 8)
        "ltr %%ax\n\t"                
        : 
        : "m"(gdt_ptr) 
        : "rax", "memory"
    );
}

void enter_user_mode(uint64_t entry_point, uint64_t user_stack) {
    __asm__ volatile (
        "cli\n\t"                     
        "mov $0x23, %%ax\n\t"         
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        
        "mov %1, %%rax\n\t"           
        "pushq $0x23\n\t"             
        "pushq %%rax\n\t"             
        "pushfq\n\t"                  
        "popq %%rax\n\t"
        "orq $0x200, %%rax\n\t"       
        "pushq %%rax\n\t"
        "pushq $0x1B\n\t"             
        "pushq %0\n\t"                
        "iretq\n\t"                   
        :
        : "r" (entry_point), "r" (user_stack)
        : "rax", "memory"
    );
}
