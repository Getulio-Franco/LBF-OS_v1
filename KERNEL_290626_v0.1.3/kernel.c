/* --- Includes do Sistema e Memória --- */
#include "mem/gdt.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "mem/heap.h"
#include "mem/e820.h"

/* --- Drivers de Hardware e Interrupções --- */
#include "drivers/idt.h"
#include "drivers/timer.h"
#include "drivers/keyboard.h"
#include "drivers/syscall.h"
#include "drivers/hw/ps2.h"
//#include "drivers/hw/pic.h"
//#include "drivers/hw/serial.h"

/* --- Vídeo e Interface Gráfica --- */
#include "drivers/video.h"
#include "drivers/desktop.h"
#include "drivers/mouse.h"
#include "drivers/double_buffering.h"

/* --- Processos e Apps --- */
#include "drivers/proc.h"
#include "drivers/apps.h"
#include "drivers/shell/shell.h"
#include "include/elf.h"

/* --- Sistema de Arquivos --- */
#include "drivers/vfs.h"
#include "fs/fat32.h"
#include "util/string.h"

#include "drivers/hw/disk.h"  // Resolve o aviso da 'set_current_disk_target'
#include "drivers/hw/ahci_pci.h"
#include "drivers/hw/ahci_vmm.h"
#include "drivers/hw/ahci_reset.h"
#include "drivers/hw/ahci_mem.h"
#include "drivers/hw/ahci_cmd.h"
#include "drivers/hw/ahci_hal.h"

//#include "drivers/pd/ehci_pci.h"
//#include "drivers/pd/ehci_core.h"

#include "drivers/pd/ehci_pci.h" 
#include "drivers/pd/ehci_core.h" // Garante o acesso ao xhci_ports_init e variáveis
#include "drivers/pd/storage.h"
#include "drivers/pd/ehci_storage.h"

#include "drivers/pci/barramento_pci.h"
#include "drivers/bga.h"

extern int fat32_mount(uint8_t dev_id);

/* --- Configuração Global da Pilha do Kernel --- */
__attribute__((aligned(16))) 
uint8_t kernel_stack[16384];
uint64_t stack_top = (uint64_t)&kernel_stack[16384];

uint8_t boot_sector[512];
extern void fat32_test_early_write(void);
//extern int refresh_screen;

extern void db_swap_buffers(void);
extern void desktop_update(void);

extern volatile int vga_ring0_enabled; // Trava do db_swap_buffers(); limitando o rambuffer

// Buffer estático de 512 bytes alinhado para receber o setor lido
__attribute__((aligned(4096))) static uint8_t teste_mbr_buffer[512];

/* --- Instâncias Globais de Hardware --- */
//SerialPort com1_port;

/**
 * @brief Habilita extensões SSE/FPU. 
 * Necessário para cálculos em 64-bit e salvamento de contexto (fxsave).
 */
void init_sse() {
    uint64_t cr0, cr4;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2); // Limpa EM (Emulation)
    cr0 |= (1 << 1);  // Seta MP (Monitor Coprocessor)
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);  // OSFXSR
    cr4 |= (1 << 10); // OSXMMEXCPT
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    
   // vga_print_string("[CPU] SSE/FPU Ativo e Configurado.", 0, 10);
}

void kernel_main() {
    /* 1. Estado Inicial: Interrupções desativadas */
    __asm__ volatile ("cli"); 

    /* 2. Inicialização da Arquitetura Base */
    gdt_init(); 
    init_idt(); 

    /* 3. Subsistema de Vídeo */
    uint32_t fb_raw     = *((uint32_t*)0x508);
    uint16_t pitch_raw  = *((uint16_t*)0x50C);
    uint16_t width_raw  = *((uint16_t*)0x510);
    uint16_t height_raw = *((uint16_t*)0x514);
    uint8_t  bpp_raw    = *((uint8_t*)0x518);

    /* 4. Gestão de Memória */
    pmm_init(4294967296ULL, (void*)0x200000);
    detect_memory_from_e820();
    vmm_init_identity(); 
    heap_init(); 
    
    /* 5. Inicialização Gráfica e Interface */
   
   if (bga_is_available()) {
        // Força a janela do emulador a ir para 1900x985 nativo
        bga_set_video_mode(1900, 985, 32); 

        // Sobrescreve as variáveis do boot para casar com a nova realidade do hardware
        width_raw = 1900;
        height_raw = 985;
        pitch_raw = 1900 * 4; // 1920 pixels * 4 bytes
        bpp_raw = 32;         
    }
   
    video_init((void*)(uintptr_t)fb_raw, width_raw, height_raw, pitch_raw, bpp_raw);
    desktop_init(width_raw, height_raw); 
    mouse_set_screen_size(width_raw, height_raw);

    /* 6. Extensões da CPU */
    init_syscall_msrs();
    init_sse();
     
    /* 7. Barramento PS/2 */
    ps2_bus_init(); 

    // ========================================================================
    // SATA E SISTEMA DE ARQUIVOS NO MOMENTO DE REPOUSO - INICIO
    // ========================================================================
    storage_init();

    // 2. Inicializa e monta o HD SATA (Disco 0) normalmente no boot
    if (ahci_hal_inicializar()) {
        storage_register_device(0, "SATA HDD", 200000, 512, (storage_read_func_t)ahci_hal_ler, NULL);
        fat32_mount(0); 
    } else {
        terminal_print("Kernel: Falha no barramento SATA.\n");
    }

    // 3. Apenas prepara o terreno para o USB (Liga o barramento, mas NÃO procura pendrive ainda)
    if (ehci_pci_init() && ehci_core_init()) {
        ehci_detectar_dispositivos(); // Deixa as portas USB alimentadas e prontas
    } else {
        terminal_print("Kernel: Controlador USB/EHCI nao disponível.\n");
    }

    // 4. Começa operando no SATA (C:)
    fat32_mudar_disco_ativo(0);
    
    //----------------fim SATA E PENDRIVE---------------------
    
    // Inicializa o barramento que, por sua vez, carrega os drivers internos
    pci_iniciar_barramento();
   
    /* 8. Configuração do Escalonador e Processos */
    scheduler_init();

    /* Criando Processos (Tasks) */
    uint64_t k_cr3 = read_cr3();
    
    // Modificamos a task_a ou o loop do Kernel para bombear os eventos do xHCI
    create_process(task_a, RING0, "Kernel_Task", k_cr3);

    uint64_t shell_pid = create_process(shell_task, RING0, "Shell", k_cr3);
    process_t* shell_proc = find_process_by_pid(shell_pid);
    if (shell_proc != NULL) {
        extern process_t* foreground_process; 
        shell_proc->is_foreground = 1;
        foreground_process = shell_proc;
    }
    
    create_process(task_c, RING0, "App_Launcher", k_cr3);
    create_process(task_d, RING0, "App_Exec", k_cr3);
    /* 9. Finalização: Timer e Ativação Global do Multitasking */
    timer_init(100); 
 
    __asm__ volatile ("sti"); // Ativação total das IRQs (O Scheduler começa a rodar aqui!)

    while(1) {
        if (vga_ring0_enabled) {
            // Durante o Boot: Processa o buffer para você ver as mensagens na tela
            db_swap_buffers(); 
        }
       // db_swap_buffers(); // loop infinito do ran_buffer para a Vram - 04 - Gartalo do S.O.
        __asm__ volatile("hlt");
    }
}
