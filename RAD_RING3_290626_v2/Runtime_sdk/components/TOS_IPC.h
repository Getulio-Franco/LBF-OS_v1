#ifndef TOS_IPC_H
#define TOS_IPC_H

#include "../system/liblib.h"
#include "../gui/wm.h"

// Mantemos as structs encapsuladas aqui
typedef struct {
    uint32_t is_active;
    uint32_t width;
    uint32_t height;
    uint32_t x;
    uint32_t y;
    uint32_t has_click_event;
    int32_t local_click_x;
    int32_t local_click_y;
    uint64_t buffer_ptr_0;
    uint64_t buffer_ptr_1;
    uint32_t active_buffer;
    uint64_t pid;
    char title[64];
} TIPCWindow;

typedef struct {
    int32_t active_focus_slot;
} TIPCControl;

// Assinaturas das funções que o software vai usar
int  OS_IPC_RegisterApp(const char* title, int width, int height);
void OS_IPC_FlipBuffers(int slot, int width, int height);

#endif
