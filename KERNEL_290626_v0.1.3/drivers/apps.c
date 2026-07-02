#include "apps.h"
#include "drivers/video.h"
#include "include/elf.h"
#include "drivers/proc.h"
#include "drivers/keyboard.h"
#include <stdint.h>

extern volatile int vga_ring0_enabled;

void task_a() {
//nunca usar
}

void task_b() {
    while(1) {
        process_cleanup_zombies(); // Ceifador
        for(volatile int i = 0; i < 500000; i++); 
        __asm__ volatile("hlt"); 
    }
}

void task_c() {
    // Aguarda o sistema estabilizar/drivers carregarem
    for(volatile int i = 0; i < 5000000; i++);

    // 1. Cria o processo do Explorer
    uint64_t pid_explorer = create_elf_process("EXPLORER.ELF");

    if (pid_explorer != (uint64_t)-1) {
        __asm__ volatile("cli");
        
        process_t* proc_explorer = find_process_by_pid(pid_explorer);
        
        if (proc_explorer) {
            // Se houver um processo anterior em foreground (Ex: o Shell do terminal), 
            // removemos o foco dele primeiro para não haver duplicidade
            if (foreground_process) {
                foreground_process->is_foreground = 0;
            }

            // 2. Entrega oficialmente o controle do hardware ao Explorer
            proc_explorer->is_foreground = 1;
            foreground_process = proc_explorer;
            
            // O Explorer está pronto e assumiu o foreground. 
            // Desligamos o motor gráfico do Ring 0 imediatamente!
            vga_ring0_enabled = 0; // deliga o db_swap_buffers(); do kernel.c

            // 3. Limpa o lixo residual do buffer do teclado antes da GUI ler
            while(keyboard_pop_char() != 0);
        }
        
        __asm__ volatile("sti");
    }

    // Loop de ociosidade da task de boot
    while(1) {
        __asm__ volatile("hlt");
    }
}

void task_d() {
    while(1) {
        process_spawn_pending(); // exec software.elf
        for(volatile int i = 0; i < 500000; i++); 
        __asm__ volatile("hlt"); 
    }
}
