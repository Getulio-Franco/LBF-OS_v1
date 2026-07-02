/**
 * ============================================================================
 * PROC.H - GERENCIAMENTO DE PROCESSOS (PCB) - V3.1 (VESA/LFB EDITION)
 * ============================================================================
 */

#ifndef PROC_H
#define PROC_H

#include <stdint.h>

// A mesma estrutura do seu Ring 3
typedef struct {
    uint64_t pid;
    char name[16];
    uint32_t state;
    uint64_t cr3;
} TProcessInfo;

// --- Configurações de Memória e Limites ---
// Aumentado para 8192 pois frames de interrupção 64-bit + variáveis locais
// podem estourar 4096 facilmente, causando GPF.
#define STACK_SIZE        8192
#define MAX_PROCESS_NAME  32
#define MAX_PROCESSES     64    

// --- Estados do Processo (Nova Lógica) ---
#define PROCESS_ZOMBIE    0    // Morto, aguardando o Reaper limpar a RAM
#define PROCESS_READY     1    // Pronto para ser executado
#define PROCESS_RUNNING   2    // Atualmente ocupando a CPU
#define PROCESS_WAITING   3    // Bloqueado aguardando I/O (Teclado)
#define PROCESS_SLEEPING  4    // Bloqueado por Tempo (Novo!)

// Mantendo compatibilidade com código antigo que usa ALIVE/DEAD
#define PROCESS_DEAD      PROCESS_ZOMBIE
#define PROCESS_ALIVE     PROCESS_READY

// --- Níveis de Privilégio (Ring) ---
#define RING0             0
#define RING3             3

// Definições de Sinais (Bitmask)
#define SIG_EVENT_MOUSE    (1 << 0)  // 0x01
#define SIG_EVENT_KEYBOARD (1 << 1)  // 0x02
#define SIG_EVENT_TERMINATE (1 << 2) // 0x04
#define SIG_EVENT_IPC      (1 << 3)  // 0x08

/**
 * @struct process_t
 * @brief Bloco de Controle de Processo (PCB)
 */
typedef struct process {
    uint64_t pid;                // Identificador único
    char     name[MAX_PROCESS_NAME];
    
    // Unificando CR3: Alguns códigos usam .cr3, outros .pml4_physical
    // Usamos um union para que ambos apontem para o mesmo lugar na memória!
    union {
        uint64_t cr3;
        uint64_t pml4_physical;
    };
    uint64_t heap_end;     // para o uso do malloc no sys_sbrk
    uint64_t stack_top;    // RSP salvo (Contexto)
    void* stack_mem;       // Endereço base da pilha do kernel (para kfree)
    int      privilege;    // RING0 ou RING3
    int      state;        // ZOMBIE, READY ou RUNNING
    uint64_t wake_up_time; // <--- ADICIONADO: Momento de acordar (em ticks)
    int is_foreground;     // 1: Tem foco do teclado, 0: Está em "background"
    uint64_t parent_pid;
    uint64_t pending_signals;  // Flags de eventos (BIT_MOUSE, BIT_KEYBOARD, BIT_IPC)
    void* message_queue;       // Ponteiro p. uma fila de mensagens opcional no futuro
    struct process* next;  // Próximo na lista
    uint64_t raw_mem_ptr;  // Ponteiro bruto para alocação
    uint8_t fpu_context[512] __attribute__((aligned(16))); // Espaço p/ FPU/SSE (512 bytes alinhados)
} process_t;

/* --- Variáveis Globais Exportadas --- */
extern process_t* head;
extern process_t* current_process;
extern process_t* foreground_process; // Adicionado para sincronizar com proc.c

/* --- Interface do Escalonador e Processos --- */

// Inicialização e Ciclo de Vida
void scheduler_init();
uint64_t create_process(void (*entry_point)(), int privilege_level, const char* name, uint64_t cr3);
uint64_t create_elf_process(const char* path);
void terminate_current_process();
void process_reaper();

// Gerenciamento, Troca de Contexto e Busca
uint64_t schedule(uint64_t current_rsp);
void force_reschedule();                       // Exportado do proc.c
process_t* get_current_process(void);          
process_t* find_process_by_pid(uint64_t pid);  
int kill_process(uint64_t pid);
void list_processes(); 

// Funções de Sistema / Parâmetros (Syscalls)
uint64_t sys_get_param(uint64_t id);
int sys_set_param(uint64_t id, uint64_t value);
void sys_sleep(uint64_t ms);
uint64_t get_current_pid();
int get_process_info_list(TProcessInfo* user_buffer, int max_items);
void monitor_ring3_status(process_t* proc);
void process_cleanup_zombies();
int kill_process(uint64_t pid);
void process_spawn_pending();

#endif // PROC_H
