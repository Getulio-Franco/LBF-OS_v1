#ifndef WM_H
#define WM_H

#include "../gui/gui.h"
#include <stdint.h> 

// --- PROTOCOLO DE COMPOSIÇÃO (NOVO IPC) ---
#define MAX_EXTERNAL_APPS 10

#define GET_FB_ADDR      ((uint64_t)(*((volatile uint32_t*)0x508)))
#define IPC_SHARED_ADDR  (GET_FB_ADDR + (uint64_t)0xA00000)

typedef struct {
    int is_active;        
    uint64_t buffer_ptr_0; 
    uint64_t buffer_ptr_1; 
    int active_buffer;     
    int pid;               
    int x;                 
    int y;                 
    int width;             
    int height;            
    char title[32];       
    
    // === NOVOS MEMBROS: INTERAÇÃO IPC ===
    int local_click_x;    
    int local_click_y;    
    int has_click_event;  
} AppWindowInfo;

typedef struct {
    volatile int lock;     // 🔑 NOVO: Cadeado atômico para exclusão mútua (Spinlock)
    int active_focus_slot; 
    uint64_t shared_fb_ptr; 
    int screen_width;
    int screen_height;
    int screen_pitch;  
    int reserved[3];       // Reduzido de 4 para 3 para manter o tamanho alinhado
} IPC_Global_Control;

#define IPC_CTRL_ADDR    (IPC_SHARED_ADDR + (uintptr_t)(sizeof(AppWindowInfo) * MAX_EXTERNAL_APPS))

#define IPC_WINDOW_LIST ((volatile AppWindowInfo*)(uintptr_t)IPC_SHARED_ADDR)
#define IPC_CONTROL      ((volatile IPC_Global_Control*)(uintptr_t)IPC_CTRL_ADDR)

extern int wm_mouse_x;
extern int wm_mouse_y;
extern int wm_mouse_show;

// ============================================================================
// 🔒 FUNÇÕES ATÔMICAS DE LOCK PARA RING 3 (Evita Condição de Corrida)
// ============================================================================
static inline void ipc_lock(volatile int* lock_ptr) {
    // Seta o valor para 1 atomicamente. Se já era 1, espera no loop.
    while (__sync_lock_test_and_set(lock_ptr, 1)) {
        __asm__ volatile("pause"); // Otimiza o consumo de CPU no loop
    }
}

static inline void ipc_unlock(volatile int* lock_ptr) {
    // Libera o cadeado voltando para 0 atomicamente
    __sync_lock_release(lock_ptr);
}

/* --- Funções do Ciclo Gráfico --- */
void wm_init(void);
void wm_set_desktop(TForm* form); 
void wm_add_window(TForm* form);
void wm_remove_window(TForm* form);
void wm_handle_mouse_event(int x, int y, int event);
void wm_handle_keyboard_event(char key);
void wm_render_pipeline(void);

#endif
