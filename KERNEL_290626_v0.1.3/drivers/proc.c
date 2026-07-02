#include "drivers/proc.h"
#include "drivers/video.h"
#include "drivers/timer.h"
#include "util/string.h"
#include "mem/heap.h"
#include "mem/vmm.h"
#include "drivers/shell/shell_commands.h"
#include "drivers/keyboard.h"

// Seletores da GDT
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x1B 
#define GDT_USER_DATA   0x23 
#define USER_STACK_VADDR 0x2000000

process_t* head = NULL;            
process_t* current_process = NULL; 
static uint64_t next_pid = 1; 
process_t* foreground_process = NULL; 

extern void tss_set_stack(uint64_t stack);
extern void elf_unload_failed_pml4(uint64_t* pml4_phys);
extern uint64_t read_cr3();
extern void write_cr3(uint64_t cr3);
extern volatile uint64_t system_ticks;
extern uint32_t global_ui_color;

/* --- Referências Externas do Driver de Vídeo --- */
extern int term_cursor_x;
extern int term_cursor_y;
extern uint32_t term_color;
extern uint32_t screen_w;
extern uint32_t screen_h;
extern process_t* head;

// Aloque no topo do proc.c (sem o static)
char pendente_exec_path[128] = {0};
volatile bool tem_pedido_exec = false;

bool detectou_r3_na_janela = false;
int monitor_ticks = 0; // testar se é Ring3 monitorar

extern volatile uint64_t pai_solicitante_pid;
// ====================================================================
// HELPERS DE IMPRESSÃO (VESA/LFB via terminal_putc)
// ====================================================================

static void proc_print(const char* str) {
    if (!str) return;
    while (*str) terminal_putc(*str++);
}

static void proc_print_dec(uint64_t val) {
    char buf[32];
    itoa(val, buf, 10);
    proc_print(buf);
}

static void proc_print_hex(uint64_t val) {
    char buf[32];
    itoa(val, buf, 16);
    proc_print("0x");
    proc_print(buf);
}

// ====================================================================
// GERENCIAMENTO DE PROCESSOS
// ====================================================================

void scheduler_init() {
    head = NULL;
    current_process = NULL;
    next_pid = 1; 
}

/*void monitor_process_switch(process_t* proc) {
    // SALVAR o estado do cursor para não quebrar o Shell
    int saved_x = term_cursor_x;
    int saved_y = term_cursor_y;
    uint32_t saved_color = term_color;

    // Área fixa de debug (Linha 0, bem no topo)
    term_cursor_x = 0;
    term_cursor_y = 0;
    
    // Desenha um fundo preto atrás do texto de debug para não sobrepor
    // (Opcional: desenhar um retângulo aqui)

    terminal_print(" [M] PID:");
    draw_dec(term_cursor_x, term_cursor_y, proc->pid, 0x00FFFF);
    terminal_print(" RING:");
    draw_dec(term_cursor_x, term_cursor_y, proc->privilege, 0x00FF00);
    terminal_print(" NAME:");
    terminal_print(proc->name);
    terminal_print("      "); // Limpa rastro

    // RESTAURAR o cursor para onde o Shell estava escrevendo
    term_cursor_x = saved_x;
    term_cursor_y = saved_y;
    term_color = saved_color;

    // IMPORTANTE: Não dê video_flush() aqui se o schedule for muito rápido!
    // Deixe o flush para o sys_write ou para o final do loop do shell.
}*/

void list_processes() {
    process_t* curr = head;
    int safety_limit = 0; // Impede loop infinito se a lista corromper

    // Cabeçalho
    term_color = 0xFFFF00; 
    terminal_print("\nPID  NAME          STATE       RING  RIP                CR3\n");
    terminal_print("---------------------------------------------------------------------------\n");
    term_color = 0xFFFFFFFF;

    while (curr != NULL && safety_limit < 50) {
        safety_limit++;

        // 1. PID
        draw_dec(term_cursor_x, term_cursor_y, curr->pid, term_color);
        
        // 2. Nome (Garante que não imprima lixo se o nome estiver corrompido)
        term_cursor_x = 45;
        if (curr->name[0] >= 32 && curr->name[0] <= 126) {
            terminal_print(curr->name);
        } else {
            terminal_print("Unknown");
        }

        // 3. Estado
        term_cursor_x = 140;
        if (curr->state == 2) {
            term_color = 0x00FF00; terminal_print("Running ");
        } else {
            term_color = 0xCCCCCC; terminal_print("Ready   ");
        }
        term_color = 0xFFFFFFFF;

        // 4. Ring e RIP (A parte que faz o QEMU fechar)
        term_cursor_x = 250; 
        if (curr == current_process) {
            terminal_print("R0    [CURRENT]");
            term_cursor_x += 160;
        } else {
            // Verificação de ponteiro de stack
            uint64_t* stack = (uint64_t*)curr->stack_top;
            
            // SEGURANÇA: Se o stack_top for um endereço maluco, pula essa parte
            if ((uint64_t)stack > 0xFFFF800000000000 || (uint64_t)stack < 0x100000) {
                 terminal_print("R?    Invalid Stack");
                 term_cursor_x += 160;
            } else {
                // Tente usar o índice 15. Se continuar dando erro, use 0 para teste.
                uint64_t saved_rip = stack[15]; 
                uint64_t saved_cs  = stack[16]; 
                int ring = (int)(saved_cs & 0x03);
                
                terminal_print("R");
                draw_dec(term_cursor_x, term_cursor_y, ring, (ring == 3 ? 0x00FF00 : 0xFFFFFF));
                term_cursor_x += 20;
                draw_hex(term_cursor_x, term_cursor_y, saved_rip, 0xAAAAAA);
                term_cursor_x += 150;
            }
        }

        // 5. CR3
        draw_hex(term_cursor_x, term_cursor_y, curr->cr3, 0x00AAAA);
        
        terminal_print("\n");

        // Avança e checa se o próximo é válido
        if (curr->next == curr) break; 
        curr = curr->next;
    }

    // Atualiza a tela no modo VESA
    extern int refresh_screen;
    refresh_screen = 1; 
   // video_flush(); //atualiza a tela somente quando e requerido a leitura - 01
}

    //-----função de monitor_process_switch inicio-----
  /*  static uint64_t last_pid = -1;
    if (current_process->pid != last_pid) {
        monitor_process_switch(current_process);
        last_pid = current_process->pid;
    }*/
    //-----função de monitor_process_switch fim-------    
    
uint64_t create_process(void (*entry_point)(), int privilege_level, const char* name, uint64_t cr3) {
    void* raw = kmalloc(sizeof(process_t) + 15);
    if (!raw) return (uint64_t)-1;
    
        // --- DEBUG INICIAL ---
   /*  if  (current_process->pid >= 6) {
        if (privilege_level == 0 || privilege_level == RING0) {
            vga_print_string("1 create_process: recebeu Ring0---- ", 0, 37);
            // Exibe o CR3 que foi passado para confirmar que é o novo PML4
            // vga_print_hex(cr3, 40, 17); 
        } else {
            if (privilege_level == 3 || privilege_level == RING3){ 
               vga_print_string("2 create_process: recebeu Ring3 ", 0, 39);
            }else{
               vga_print_string("3 create_process: recebeu OUTRO NUMERO ", 0, 38);
            }
        }
    }*/

    // Alinhamento da estrutura do processo
    process_t* new_proc = (process_t*)(((uintptr_t)raw + 15) & ~0xFULL);
    memset(new_proc, 0, sizeof(process_t));
    new_proc->raw_mem_ptr = (uint64_t)raw;
    
    // Inicialização do contexto FPU/SSE (importante para evitar GPF em printf/math)
    memset(new_proc->fpu_context, 0, 512);
    *(uint32_t*)&new_proc->fpu_context[24] = 0x1F80; 
    *(uint16_t*)&new_proc->fpu_context[0]  = 0x037F; 
    
    new_proc->pid = next_pid++;
    strncpy(new_proc->name, name, MAX_PROCESS_NAME - 1);
    new_proc->privilege = privilege_level;
    new_proc->state = PROCESS_READY;
    
    // --- DEBUG CONDICIONAL (PID > 4) ---
   /* if (new_proc->pid >= 5) {
        if (privilege_level == 3) {
            vga_print_string("[NEW] Proce 4 User (PID > 4) Ring3: ", 0, 36);
            vga_print_hex_64(new_proc->pid, 40, 36);
        } else {
            vga_print_string("[NEW] Proce 5 Kernel (PID > 4) Ring0: ", 0, 35);
            vga_print_hex_64(new_proc->pid, 42, 35);
        }
    }*/
    
    if (current_process != NULL) new_proc->parent_pid = current_process->pid;
    else new_proc->parent_pid = 0;
    
    new_proc->is_foreground = 0;
    new_proc->cr3 = (cr3 == 0) ? (read_cr3() & ~0xFFFULL) : (cr3 & ~0xFFFULL);

    void* kernel_stack = kmalloc(STACK_SIZE);
    if (!kernel_stack) { kfree(raw); return (uint64_t)-1; }
    
    new_proc->stack_mem = kernel_stack;
    uint64_t kstack_top = (uint64_t)((uint8_t*)kernel_stack + STACK_SIZE);
    kstack_top &= -16LL; // Alinhamento de 16 bytes exigido pela ABI x86_64
    uint64_t* stack_ptr = (uint64_t*)kstack_top;
    
    // ====================================================================
    // MONTAGEM DA PILHA (SINCRONIZADA COM INTERRUPTS.ASM)
    // ====================================================================
    // 1. Frame de Interrupção de Hardware (IRETQ)
    if (privilege_level == 3) {
    /* if (new_proc->pid >= 5) {
        vga_print_string("6 create_process: recebeu Ring3 DENTRO DA PILHA ", 0, 33);
     }*/
        *(--stack_ptr) = (uint64_t)(0x23); //(GDT_USER_DATA | 3); // SS
        *(--stack_ptr) = (uint64_t)USER_STACK_VADDR;    // RSP
        *(--stack_ptr) = (uint64_t)0x202;               // RFLAGS (IF=1)
        *(--stack_ptr) = (uint64_t)(0x1B); //(GDT_USER_CODE | 3); // CS
        *(--stack_ptr) = (uint64_t)entry_point;         // RIP
    } else {
        if (privilege_level == 0) {
          /* if (new_proc->pid >= 5) {
               vga_print_string("7 create_process: recebeu Ring3 DENTRO DA PILHA ", 0, 29);
           }*/
           *(--stack_ptr) = (uint64_t)GDT_KERNEL_DATA;     // SS
           *(--stack_ptr) = (uint64_t)kstack_top;          // RSP
           *(--stack_ptr) = (uint64_t)0x202;               // RFLAGS (IF=1)
           *(--stack_ptr) = (uint64_t)GDT_KERNEL_CODE;     // CS
           *(--stack_ptr) = (uint64_t)entry_point;         // RIP
        }else{
        /*  if (new_proc->pid >= 5) {
              vga_print_string("8 RETORNOU SEM RING3 OU RING0 ", 0, 28);
          }*/
          return 0;
        }
    }
    
    // 2. Espaço para limpeza universal (add rsp, 16 no Assembly)
    // O interrupts.asm espera encontrar o Código de Erro e o ID aqui.
    *(--stack_ptr) = (uint64_t)0; // Simula err_code
    *(--stack_ptr) = (uint64_t)0; // Simula int_no
    
    // 3. Registradores de propósito geral (PUSH_ALL)
    // Devem ser exatamente 15 registradores conforme o macro PUSH_ALL
    for(int i = 0; i < 15; i++) {
        *(--stack_ptr) = 0; 
    }
    
    // O stack_top do processo aponta para o último registrador empurrado (R15)
    new_proc->stack_top = (uint64_t)stack_ptr;
    
    // Adiciona o processo à lista encadeada global
    __asm__ volatile("cli");
    new_proc->next = head;
    head = new_proc;
    if (current_process == NULL) current_process = new_proc;
    __asm__ volatile("sti");

    return new_proc->pid; 
   
}

uint64_t schedule(uint64_t current_rsp) {
    if (head == NULL) return current_rsp; 

    // --- 1. LÓGICA DE ACORDAR PROCESSOS ---
    process_t* scan = head;
    while (scan != NULL) {
        // Ajustado para olhar o estado 3 (Sleeping), que é o que o sys_sleep define
        if (scan->state == 3) { 
            if (system_ticks >= scan->wake_up_time) {
                scan->state = 1; // 1 = PROCESS_READY (Acorda!)
            }
        }
        scan = scan->next;
    }

    // --- 2. SALVAMENTO DO CONTEXTO ---
    if (current_process != NULL) {
        current_process->stack_top = current_rsp;
        
        if (current_process->state > 0) {
            if (((uintptr_t)current_process->fpu_context & 0xF) == 0) {
                __asm__ volatile("fxsave %0" : "=m" (current_process->fpu_context));
            }
        }

        // Se estava rodando (2), volta a ficar pronto (1)
        // Se estiver dormindo (3), o estado permanece 3 para o escalonador pular ele
        if (current_process->state == 2) {
            current_process->state = 1;
        }
    }

    // --- 3. SELEÇÃO DO PRÓXIMO ---
    process_t* next_proc = (current_process && current_process->next) ? current_process->next : head;
    int attempts = 0;

    // Procura por READY (1). Pula ZOMBIE (0) e SLEEPING (3).
    while (next_proc->state != 1 && attempts < 256) { // use um define ou numero fixo seguro
        next_proc = (next_proc->next) ? next_proc->next : head;
        attempts++;
    }

    if (next_proc->state != 1) {
        if (current_process && current_process->state == 1) next_proc = current_process;
        else next_proc = head;
    }

    // --- 4. PREPARAÇÃO DO CONTEXTO ---
    current_process = next_proc;
    current_process->state = 2; // 2 = RUNNING

    uint64_t next_cr3 = current_process->cr3 & ~0xFFFULL;
    if (read_cr3() != next_cr3) {
        write_cr3(next_cr3);
    }

    if (((uintptr_t)current_process->fpu_context & 0xF) == 0) {
        __asm__ volatile("fxrstor %0" : : "m" (current_process->fpu_context));
    }

    uint64_t kstack_for_cpu = (uint64_t)((uint8_t*)current_process->stack_mem + STACK_SIZE);
    tss_set_stack(kstack_for_cpu);

    // Dentro da função schedule, logo após escolher o current_process:
/*     if (current_process->pid >= 5) {
        if (current_process->privilege == 3) {
            uint64_t* ptr = (uint64_t*)current_process->stack_top; 
            vga_print_string("PULO IMINENTE P 9 RING 3! PID:", 0, 40);
            vga_print_hex_64(current_process->pid, 32, 40);
            vga_print_string("RIP:", 26, 41);
            vga_print_hex_64(ptr[17], 32, 41); 
        } else {
            vga_print_string("EXEC_KERNEL PID:", 0, 32);
            vga_print_hex_64(current_process->pid, 18, 32);
            vga_print_string("P 10:", 30, 32);
            vga_print_hex_64((uint64_t)current_process->privilege, 36, 32);
        }
    }*/
    // Chamamos o monitor para o processo que ACABAMOS de escolher (current_process)
    monitor_ring3_status(current_process);
    return current_process->stack_top;
}

void terminate_current_process() {
    if (current_process == NULL || current_process->pid <= 1) { 
        proc_print("\n[Erro] Tentativa de matar processo vital!\n");
        return; 
    }
    
    __asm__ volatile("cli");
    
    current_process->state = 0; // Apenas avisa que morreu
    
    proc_print("\n[Kernel] Processo isolado para descarte: ");
    proc_print(current_process->name);
    proc_print("\n");
    
    // Deixa o processo na lista! A task_b vai retirá-lo depois.
    
    __asm__ volatile("int $0x20");
    __asm__ volatile("sti");
    while(1) { __asm__ volatile("hlt"); }
}

void force_reschedule() {
    __asm__ volatile("int $0x20"); 
}

uint64_t sys_get_param(uint64_t id) {
    switch(id) {
        case 0: 
            return (current_process) ? current_process->pid : 0;
        case 1: 
            return system_ticks;
        case 2: 
            return (current_process) ? current_process->privilege : 0;
        default:
            return 0;
    }
}

int sys_set_param(uint64_t id, uint64_t value) {
    if (current_process->privilege > 0) {
        // Reservado para checagem de privilégios no futuro
    }

    switch(id) {
        case 100: 
            global_ui_color = value;
            return 0;
        default:
            return -1;
    }
}

void sys_sleep(uint64_t ms) {
    if (ms == 0 || current_process == NULL) return;

    // 1. Desabilita interrupções temporariamente (proteção de concorrência)
    __asm__ volatile("cli");

    // 2. Calcula quantos ticks esperar (100Hz = 10ms por tick)
    uint64_t ticks_to_wait = ms / 10;
    if (ticks_to_wait == 0) ticks_to_wait = 1;

    // 3. Define a hora de acordar e muda o estado
    current_process->wake_up_time = timer_get_ticks() + ticks_to_wait;
    current_process->state = 3; // 3 = Dormindo

    // 4. Aciona a Interrupção do Timer manualmente para forçar o Context Switch!
    // Isso equivale ao seu antigo force_reschedule(), mas mais seguro.
    __asm__ volatile("int $0x20"); 

    // O processo irá "parar" na linha de cima. 
    // Quando ele acordar e voltar a rodar, continuará a partir daqui:
    __asm__ volatile("sti");
}

process_t* find_process_by_pid(uint64_t pid) {
    process_t* curr = head; 
    while (curr != NULL) {
        if (curr->pid == pid) return curr;
        curr = curr->next;
    }
    return NULL;
}

process_t* get_current_process() { 
    return current_process; 
}

/**
 * @brief Retorna o PID do processo que está rodando no momento.
 * Pode ser usada por qualquer parte do Kernel (GUI, Syscalls, Drivers).
 */
uint64_t get_current_pid() {
    if (current_process != NULL) {
        return current_process->pid;
    }
    return 0; // Kernel ou nenhum processo rodando
}

// No Kernel (proc.c ou similar)
int get_process_info_list(TProcessInfo* user_buffer, int max_items) {
    process_t* curr = head;
    int count = 0;

    // IMPORTANTE: O buffer está no Ring 3. 
    // Garanta que o CR3 atual é o do processo que chamou!
    
    while (curr != NULL && count < max_items) {
        
        // =========================================================================
        // FILTRO CRÍTICO: SE FOR ZUMBI (STATE == 0), PULA E NÃO MOSTRA NO TASK MGR
        // =========================================================================
        if (curr->state == 0) {
            curr = curr->next; // Avança o ponteiro para o próximo da lista encadeada
            continue;          // Volta para o topo do while sem incrementar count
        }

        // Criamos uma estrutura temporária no Ring 0 (Stack do Kernel)
        TProcessInfo k_temp;
        k_temp.pid = curr->pid;
        k_temp.state = curr->state;
        k_temp.cr3 = curr->cr3;
        
        for(int i = 0; i < 15; i++) {
            k_temp.name[i] = curr->name[i];
            if (curr->name[i] == '\0') break;
        }
        k_temp.name[15] = '\0';

        // AGORA: Copiamos da Stack do Kernel para a Stack do Usuário
        // Se o seu vmm_map_user estiver correto, o Kernel tem permissão.
        user_buffer[count] = k_temp; 

        // Avança normalmente para processos ativos
        curr = curr->next;
        count++;
    }
    return count;
}

// Função de controle para o radar de Ring3
void monitor_ring3_status(process_t* proc) {
    if (!proc) return;

    // FILTRO: Só processa se o PID for maior que 4
    if (proc->pid <= 4) {
        // Ignora os processos do sistema (1 a 4)
        return;
    }

    // 1. Pega os dados da pilha de contexto
    uint64_t* stack_ptr = (uint64_t*)proc->stack_top;
    
    // O CS (Code Segment) define o privilégio real no hardware
    uint64_t saved_cs = stack_ptr[17]; 
    int cpl = (int)(saved_cs & 3);

    // Coordenadas Y em pixels
    uint32_t y_36 = 36 * 16;
    uint32_t y_37 = 37 * 16;

    monitor_ticks++;
    if (monitor_ticks >= 500) {
        monitor_ticks = 0;

        if (cpl == 3) {
            // --- LINHA 36: APLICATIVO EM RING 3 (Trabalhando) ---
            vga_print_string("                                        ", 0, 36);
            vga_print_string("[APP R3] PID:", 0, 36);
            draw_dec(110, y_36, (uint32_t)proc->pid, 0x00FF00); // Verde
            
            vga_print_string("CS:", 160, 36);
            vga_print_hex_64(saved_cs, 190, 36);
        } 
        else {
            // --- LINHA 37: APLICATIVO EM SYSCALL/SLEEP (Ring 0 Seguro) ---
            // Limpa a linha 37 antes de escrever
            vga_print_string("                                        ", 0, 37);
            
            // O processo está rodando código do Kernel por vontade própria (ex: sys_sleep)
            vga_print_string("[SYSCALL] PID:", 0, 37);
            draw_dec(120, y_37, (uint32_t)proc->pid, 0xFFFF00); // Amarelo (Aviso Visual)
            
            vga_print_string("Aguardando em R0...", 160, 37);
        }
    }
}

void process_cleanup_zombies() {
    __asm__ volatile("cli"); 
    
    process_t* curr = head;
    process_t* prev = NULL;
    uint64_t kernel_cr3 = read_cr3() & ~0xFFFULL;

    while (curr != NULL) {
        // Se encontrar um zumbi que NÃO seja o processo que está rodando a task_b atualmente
        if (curr->state == 0 && curr != current_process && curr->pid > 3) {
            process_t* to_delete = curr;

            // 1. Remove da lista encadeada agora que fomos autorizados
            if (prev == NULL) {
                head = curr->next;
            } else {
                prev->next = curr->next;
            }
            curr = curr->next; 

            // 2. Libera recursos de paginação/tabela de páginas no pmm.c
            if (to_delete->cr3 != 0 && (to_delete->cr3 & ~0xFFFULL) != kernel_cr3) {
                elf_unload_failed_pml4((uint64_t*)to_delete->cr3);
            }
            
            // 3. Libera a pilha do kernel (SEGURO: A task_b usa a própria pilha!)
            if (to_delete->stack_mem != 0) {
                kfree(to_delete->stack_mem);
            }

            // 4. Devolve a estrutura de controle para o PMM
            if (to_delete->raw_mem_ptr != 0) {
                kfree((void*)to_delete->raw_mem_ptr);
            } else {
                kfree(to_delete);
            }
            
            continue; 
        }
        
        prev = curr;
        curr = curr->next;
    }
    
    __asm__ volatile("sti"); 
}

int kill_process(uint64_t pid) {
    // 1. Proteção Crítica: Impede matar o Kernel (1), Shell (2) ou gerenciadores vitais (3)
    if (pid <= 3) {
        proc_print("\n[Kernel Erro] Tentativa de matar processo vital ou protegido!\n");
        return -1; 
    }

    __asm__ volatile("cli");

    // 2. Busca o processo alvo na lista encadeada 'head'
    process_t* target = find_process_by_pid(pid);
    
    if (target == NULL) {
        __asm__ volatile("sti");
        return -1; // Processo não encontrado
    }

    // 3. Se o processo já estiver em estado zumbi, ignore
    if (target->state == 0) {
        __asm__ volatile("sti");
        return 0;
    }

    // 4. Sinaliza descarte: muda o estado para 0 (A task_b vai localizá-lo e limpá-lo)
    target->state = 0;

    proc_print("\n[Kernel] Processo finalizado via SYS_KILL: ");
    proc_print(target->name);
    proc_print("\n");

    // 5. Ajuste de Foco / Foreground (Reutilizando a sua lógica do sys_exit)
    // Se o app morto estava em primeiro plano, devolve o foco para o pai dele
    if (target->is_foreground) {
        process_t* pai = find_process_by_pid(target->parent_pid);
        if (pai) { 
            pai->is_foreground = 1; 
            foreground_process = pai; 
        }
    }

    // 6. Caso bizarro: se um processo tentar dar KILL em si mesmo,
    // ele deve ceder o processador imediatamente
    if (target == get_current_process()) {
        __asm__ volatile("int $0x20");
        __asm__ volatile("sti");
        while(1) { __asm__ volatile("hlt"); }
    }

    __asm__ volatile("sti");
    return 0; // Sucesso!
}

void process_spawn_pending() {
    if (!tem_pedido_exec) return;

    char caminho_local[128];
    uint64_t pid_do_pai = 0;
    
    __asm__ volatile("cli");
    if (!tem_pedido_exec) {
        __asm__ volatile("sti");
        return;
    }
    
    strcpy(caminho_local, pendente_exec_path);
    pid_do_pai = pai_solicitante_pid; 

    memset(pendente_exec_path, 0, sizeof(pendente_exec_path));
    pai_solicitante_pid = 0;
    tem_pedido_exec = false;
    __asm__ volatile("sti"); 

    // O disco lê o ELF de forma segura enquanto o pai está dormindo no RING3
    uint64_t pid_filho = create_elf_process(caminho_local);

    // Gestão de foco e ACORDAR O PAI:
    if (pid_filho != (uint64_t)-1) {
        process_t* proc_filho = find_process_by_pid(pid_filho);
        process_t* proc_pai = find_process_by_pid(pid_do_pai);

        if (proc_filho) {
            proc_filho->parent_pid = pid_do_pai;
            
            if (proc_pai && proc_pai->is_foreground == 1) {
                proc_filho->is_foreground = 1;
                proc_pai->is_foreground = 0;
                foreground_process = proc_filho;
                
                while(keyboard_pop_char() != 0); 
            }
        }
    }

    // 🔥 ALTERAÇÃO CRÍTICA: O filho já nasceu, o IPC já estabilizou. Hora de acordar o pai!
    process_t* proc_pai_bloqueado = find_process_by_pid(pid_do_pai);
    if (proc_pai_bloqueado) {
        // 🔥 Devolve o pai para a fila de execução!
        proc_pai_bloqueado->state = PROCESS_READY; 
    }
}
