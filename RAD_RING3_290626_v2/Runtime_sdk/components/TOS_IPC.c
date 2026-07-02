#include "TOS_IPC.h"
#include "../system/string.h"
#include "../system/graphics.h"
#include "../system/liblib.h"

int OS_IPC_RegisterApp(const char* title, int width, int height) {
    for (int i = 0; i < MAX_EXTERNAL_APPS; i++) {
        if (IPC_WINDOW_LIST[i].is_active == 0) {
            IPC_WINDOW_LIST[i].is_active = 1;
            IPC_WINDOW_LIST[i].width = width;
            IPC_WINDOW_LIST[i].height = height;
            IPC_WINDOW_LIST[i].x = 200; 
            IPC_WINDOW_LIST[i].y = 120;
            
            uint64_t base_mem = (uint64_t)(uintptr_t)(IPC_CTRL_ADDR + 0x1000 + (i * 0x200000));
            IPC_WINDOW_LIST[i].buffer_ptr_0 = base_mem;
            IPC_WINDOW_LIST[i].buffer_ptr_1 = base_mem + 0x100000;
            IPC_WINDOW_LIST[i].active_buffer = 0;
            
            IPC_WINDOW_LIST[i].local_click_x = 0;
            IPC_WINDOW_LIST[i].local_click_y = 0;
            IPC_WINDOW_LIST[i].has_click_event = 0;

            // =========================================================================
            // 🔥 VINCULAÇÃO DO PID REAL DO SISTEMA OPERACIONAL:
            // O software.elf descobre quem ele é via Syscall 23 e carimba no mural do IPC
            // =========================================================================
            IPC_WINDOW_LIST[i].pid = sys_get_pid(); 
            // =========================================================================

            strcpy((char*)IPC_WINDOW_LIST[i].title, title);
            return i; // Retorna o slot alocado com sucesso
        }
    }
    return -1; // Sem slots livres
}

void OS_IPC_FlipBuffers(int slot, int width, int height) {
    int back_idx = (IPC_WINDOW_LIST[slot].active_buffer == 0) ? 1 : 0;
    uint8_t* shared_ptr = (back_idx == 0) 
        ? (uint8_t*)(uintptr_t)IPC_WINDOW_LIST[slot].buffer_ptr_0 
        : (uint8_t*)(uintptr_t)IPC_WINDOW_LIST[slot].buffer_ptr_1;
    
    uint8_t* local_ptr = graphics_get_buffer();
    if (shared_ptr && local_ptr) {
        memcpy(shared_ptr, local_ptr, width * height * 4); 
    }
    IPC_WINDOW_LIST[slot].active_buffer = back_idx;
}
