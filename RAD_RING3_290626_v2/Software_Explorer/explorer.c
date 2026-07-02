#include "../gui/gui.h"
#include "../system/graphics.h" 
#include "../system/liblib.h"
#include "../gui/wm.h"
#include "../system/string.h" 
#include "../events/cursor_engine.h"

// Protótipos para renderização manual de overlays (Menu Iniciar)
extern void gui_draw_form(TForm* form);
extern void gui_render_form(TForm* form);

extern uint8_t* ram_buffer; 

int z_stack[MAX_EXTERNAL_APPS];
int dragging_slot = -1;
int offX = 0, offY = 0;

// Elementos Nativos da Interface
TForm* frmTaskbar  = NULL;
TLabel* lblStatus  = NULL;
TButton* btnStart  = NULL;

// Novos Elementos: Menu Iniciar e Atalhos
TForm* frmStartMenu = NULL;
TButton* btnAppTarefa = NULL;
TButton* btnAppGpci = NULL;
TButton* btnAppSerial = NULL;
TButton* btnAppTerminal = NULL;
TButton* btnAppGfile = NULL;

int is_start_menu_open = 0;

/* ============================================================================
 * GERENCIAMENTO DE Z-INDEX (PILHA DE CAMADAS)
 * ============================================================================ */
void init_z_stack() {
    for(int i = 0; i < MAX_EXTERNAL_APPS; i++) z_stack[i] = i;
}

void bring_to_front(int slot_index) {
    int current_pos = -1;
    for(int i = 0; i < MAX_EXTERNAL_APPS; i++) {
        if(z_stack[i] == slot_index) { current_pos = i; break; }
    }
    if(current_pos == -1) return;
    for(int i = current_pos; i < MAX_EXTERNAL_APPS - 1; i++) z_stack[i] = z_stack[i+1];
    z_stack[MAX_EXTERNAL_APPS - 1] = slot_index;
}

/* ============================================================================
 * MOTOR DO COMPOSITOR (CLIPPING DE VIDEO GRÁFICO)
 * ============================================================================ */
void compose_app_window(int slot) {
    int active_idx = IPC_WINDOW_LIST[slot].active_buffer;
    uint32_t* app_fb = (active_idx == 0)
        ? (uint32_t*)(uintptr_t)IPC_WINDOW_LIST[slot].buffer_ptr_0
        : (uint32_t*)(uintptr_t)IPC_WINDOW_LIST[slot].buffer_ptr_1;
    
    if (!app_fb || !ram_buffer) return;

    int win_x = IPC_WINDOW_LIST[slot].x;
    int win_y = IPC_WINDOW_LIST[slot].y;
    int win_w = IPC_WINDOW_LIST[slot].width;
    int win_h = IPC_WINDOW_LIST[slot].height;

    int start_y = (win_y < 0) ? -win_y : 0;
    int end_y   = (win_y + win_h > screen_h) ? (screen_h - win_y) : win_h;

    int start_x = (win_x < 0) ? -win_x : 0;
    int end_x   = (win_x + win_w > screen_w) ? (screen_w - win_x) : win_w;

    int copy_w = end_x - start_x;
    if (copy_w <= 0) return; 

    for (int y = start_y; y < end_y; y++) {
        int target_y = win_y + y;
        uint8_t* dest_line = ram_buffer + (target_y * screen_pitch) + ((win_x + start_x) * bpp_bytes);
        uint32_t* src_line = app_fb + (y * win_w) + start_x;

        if (bpp_bytes == 4) {
            memcpy(dest_line, src_line, copy_w * 4);
        } else if (bpp_bytes == 3) {
            for (int x = 0; x < copy_w; x++) {
                uint32_t color = src_line[x];
                int off = x * 3;
                dest_line[off]   = color & 0xFF;
                dest_line[off+1] = (color >> 8) & 0xFF;
                dest_line[off+2] = (color >> 16) & 0xFF;
            }
        }
    }
}

/* ============================================================================
 * SUBFUNÇÃO: Coletar_Slots_Inativos (Garbage Collector de Janelas)
 * ============================================================================ */
void Coletar_Slots_Inativos(void) {
    // 🔒 TRANCA: Bloqueia qualquer app de tentar se registrar enquanto limpamos
    ipc_lock(&IPC_CONTROL->lock);

    for (int i = 0; i < MAX_EXTERNAL_APPS; i++) {
        if (IPC_WINDOW_LIST[i].pid != 0 && IPC_WINDOW_LIST[i].is_active == 0) {
            
            if (IPC_CONTROL->active_focus_slot == i) {
                IPC_CONTROL->active_focus_slot = -1;
                if (lblStatus) {
                    strcpy(lblStatus->Graphic.Control.Caption, "VCL/IPC OS v1.2 [Event-Driven]");
                }
            }

            if (dragging_slot == i) {
                dragging_slot = -1;
            }

            // Faxina estrutural IPC
            IPC_WINDOW_LIST[i].pid = 0;             
            IPC_WINDOW_LIST[i].buffer_ptr_0 = 0;    
            IPC_WINDOW_LIST[i].buffer_ptr_1 = 0;    
            IPC_WINDOW_LIST[i].width = 0;
            IPC_WINDOW_LIST[i].height = 0;
            IPC_WINDOW_LIST[i].has_click_event = 0;
            IPC_WINDOW_LIST[i].title[0] = '\0';     
        }
    }

    // 🔑 LIBERA
    ipc_unlock(&IPC_CONTROL->lock);
}

/* ============================================================================
 * SUBFUNÇÃO: Processar_Clique_Janela
 * ============================================================================ */
void Processar_Clique_Janela(mouse_t* m) {
    // 🔒 TRANCA: Evita que posições de janelas mudem enquanto calculamos o hit-test
    ipc_lock(&IPC_CONTROL->lock);

    for (int i = MAX_EXTERNAL_APPS - 1; i >= 0; i--) {
        int s = z_stack[i];
        if (IPC_WINDOW_LIST[s].is_active) {
            
            if (m->x >= IPC_WINDOW_LIST[s].x && m->x <= (IPC_WINDOW_LIST[s].x + IPC_WINDOW_LIST[s].width) &&
                m->y >= IPC_WINDOW_LIST[s].y && m->y <= (IPC_WINDOW_LIST[s].y + IPC_WINDOW_LIST[s].height)) {
                
                int local_x = m->x - IPC_WINDOW_LIST[s].x;
                int local_y = m->y - IPC_WINDOW_LIST[s].y;

                if (IPC_CONTROL->active_focus_slot != s) {
                    IPC_CONTROL->active_focus_slot = s;
                    bring_to_front(s);
                    
                    if (lblStatus) {
                        char caption[128];
                        strcpy(caption, "Foco: ");
                        strcat(caption, (char*)IPC_WINDOW_LIST[s].title);
                        strcpy(lblStatus->Graphic.Control.Caption, caption);
                    }
                }

                // Interrupção do Botão Fechar [X]
                int win_w = IPC_WINDOW_LIST[s].width;
                if (local_y >= 6 && local_y <= 22 && local_x >= (win_w - 22) && local_x <= (win_w - 6)) {
                    IPC_WINDOW_LIST[s].is_active = 0; 
                    break; 
                }

                if (local_y <= 25) {
                    dragging_slot = s;
                    offX = m->x - IPC_WINDOW_LIST[s].x;
                    offY = m->y - IPC_WINDOW_LIST[s].y;
                } 
                else {
                    IPC_WINDOW_LIST[s].local_click_x = local_x;
                    IPC_WINDOW_LIST[s].local_click_y = local_y;
                    IPC_WINDOW_LIST[s].has_click_event = 1; 
                }
                
                break; 
            }
        }
    }

    // 🔑 LIBERA
    ipc_unlock(&IPC_CONTROL->lock);
}

/* ============================================================================
 * SUBFUNÇÃO: Executar_Pipeline_Composicao
 * ============================================================================ */
void Executar_Pipeline_Composicao(mouse_t* m) {
    graphics_clear(0x003A6EA5); 
    
    // 🔒 TRANCA: Garante que nenhuma janela vai sumir ou mudar de tamanho
    // ENQUANTO o loop lê os ponteiros de memória e faz o memcpy
    ipc_lock(&IPC_CONTROL->lock);
    for (int i = 0; i < MAX_EXTERNAL_APPS; i++) {
        int s = z_stack[i];
        if (IPC_WINDOW_LIST[s].is_active) {
            compose_app_window(s);
        }
    }
    ipc_unlock(&IPC_CONTROL->lock); // 🔑 LIBERA

    wm_render_pipeline();     

    if (is_start_menu_open && frmStartMenu) {
        gui_draw_form(frmStartMenu);
        gui_render_form(frmStartMenu);
    }
    
    cursor_draw(m->x, m->y, screen_w, screen_h);
    video_flip(ram_buffer);
}

/* ============================================================================
 * SUBFUNÇÃO: Setup_Interface_Nativa
 * ============================================================================ */
void Setup_Interface_Nativa(void) {
    // --- BARRA DE TAREFAS ---
    frmTaskbar = gui_create_form("frmTaskbar", "Taskbar", 1);
    if (frmTaskbar) {
        frmTaskbar->BorderStyle = bsNone;
        frmTaskbar->Win.Control.Left = 0;
        frmTaskbar->Win.Control.Top = screen_h - 40;
        frmTaskbar->Win.Control.Width = screen_w;
        frmTaskbar->Win.Control.Height = 40;
        frmTaskbar->Win.Control.Color = 0xC0C0C0;
        frmTaskbar->Win.Control.Visible = 1;
        
        btnStart = gui_create_button(&frmTaskbar->Win, "btnStart", "Iniciar");
        if (btnStart) {
            btnStart->Win.Control.Left = 2;
            btnStart->Win.Control.Top = 2;
            btnStart->Win.Control.Width = 80;
            btnStart->Win.Control.Height = 36;
        }
        // O set_desktop garante que a Taskbar é a "base" gerenciada pelo wm
        wm_set_desktop(frmTaskbar); 
    }
    
    lblStatus = gui_create_label((TWinControl*)frmTaskbar, "lblStatus", "VCL/IPC OS v1.2 [Event-Driven]");
    if (lblStatus) {
        lblStatus->Graphic.Control.Left = 100;
        lblStatus->Graphic.Control.Top = 12;
    }

    // --- MENU INICIAR ---
    frmStartMenu = gui_create_form("frmStartMenu", "StartMenu", 1);
    if (frmStartMenu) {
        frmStartMenu->BorderStyle = bsNone;
        frmStartMenu->Win.Control.Width = 320;  // LARGURA AUMENTADA
        frmStartMenu->Win.Control.Height = 220; // Espaço para 5 botões
        frmStartMenu->Win.Control.Left = 0;
        frmStartMenu->Win.Control.Top = screen_h - 40 - frmStartMenu->Win.Control.Height;
        frmStartMenu->Win.Control.Color = 0xC0C0C0;
        frmStartMenu->Win.Control.Visible = 0; // Começa oculto
        
        // Adicionando os aplicativos (Larguras aumentadas para 240)
        btnAppTarefa = gui_create_button(&frmStartMenu->Win, "btnAppTarefa", "Gerenciador de Tarefas");
        btnAppTarefa->Win.Control.Left = 5; btnAppTarefa->Win.Control.Top = 5;
        btnAppTarefa->Win.Control.Width = 310; btnAppTarefa->Win.Control.Height = 35;

        btnAppGpci = gui_create_button(&frmStartMenu->Win, "btnAppGpci", "Gerenciador Dispositivos");
        btnAppGpci->Win.Control.Left = 5; btnAppGpci->Win.Control.Top = 45;
        btnAppGpci->Win.Control.Width = 310; btnAppGpci->Win.Control.Height = 35;

        btnAppSerial = gui_create_button(&frmStartMenu->Win, "btnAppSerial", "Monitor Serial");
        btnAppSerial->Win.Control.Left = 5; btnAppSerial->Win.Control.Top = 85;
        btnAppSerial->Win.Control.Width = 310; btnAppSerial->Win.Control.Height = 35;

        btnAppTerminal = gui_create_button(&frmStartMenu->Win, "btnAppTerminal", "Terminal OS");
        btnAppTerminal->Win.Control.Left = 5; btnAppTerminal->Win.Control.Top = 125;
        btnAppTerminal->Win.Control.Width = 310; btnAppTerminal->Win.Control.Height = 35;

        btnAppGfile = gui_create_button(&frmStartMenu->Win, "btnAppGfile", "Gerenciador de Arquivos");
        btnAppGfile->Win.Control.Left = 5; btnAppGfile->Win.Control.Top = 165;
        btnAppGfile->Win.Control.Width = 310; btnAppGfile->Win.Control.Height = 35;

        // REMOVIDO: wm_set_desktop(frmStartMenu); 
        // O menu não é o desktop principal, ele é um overlay manual.
    }
}

/* ============================================================================
 * LOOP PRINCIPAL (Com proteção nos blocos de arrasto e holding)
 * ============================================================================ */
int main() {
    graphics_init();  
    
    if (screen_w == 0 || screen_h == 0) {
        sys_exit();
    }
    
    wm_init();
    init_z_stack();
    Setup_Interface_Nativa();

    static int was_clicked = 0;

    while(1) {
        mouse_t m; 
        get_mouse(&m);

        Coletar_Slots_Inativos();

        if (m.buttons & 1) { 
            if (!was_clicked) {
                was_clicked = 1;
                int click_handled = 0;

                int startBtn_X = frmTaskbar->Win.Control.Left + btnStart->Win.Control.Left;
                int startBtn_Y = frmTaskbar->Win.Control.Top + btnStart->Win.Control.Top;
                
                if (m.x >= startBtn_X && m.x <= (startBtn_X + btnStart->Win.Control.Width) &&
                    m.y >= startBtn_Y && m.y <= (startBtn_Y + btnStart->Win.Control.Height)) {
                    
                    is_start_menu_open = !is_start_menu_open;
                    frmStartMenu->Win.Control.Visible = is_start_menu_open;
                    click_handled = 1;
                }
                else if (is_start_menu_open && 
                         m.x >= frmStartMenu->Win.Control.Left && m.x <= (frmStartMenu->Win.Control.Left + frmStartMenu->Win.Control.Width) &&
                         m.y >= frmStartMenu->Win.Control.Top && m.y <= (frmStartMenu->Win.Control.Top + frmStartMenu->Win.Control.Height)) {
                    
                    int local_y = m.y - frmStartMenu->Win.Control.Top;

                    if (local_y >= btnAppTarefa->Win.Control.Top && local_y <= (btnAppTarefa->Win.Control.Top + btnAppTarefa->Win.Control.Height)) {
                        sys_exec("tarefa.elf");
                        sys_sleep(300);
                    }
                    else if (local_y >= btnAppGpci->Win.Control.Top && local_y <= (btnAppGpci->Win.Control.Top + btnAppGpci->Win.Control.Height)) {
                        sys_exec("gpci.elf");
                        sys_sleep(300);
                    }
                    else if (local_y >= btnAppSerial->Win.Control.Top && local_y <= (btnAppSerial->Win.Control.Top + btnAppSerial->Win.Control.Height)) {
                        sys_exec("serial.elf");
                        sys_sleep(300);
                    }
                    else if (local_y >= btnAppTerminal->Win.Control.Top && local_y <= (btnAppTerminal->Win.Control.Top + btnAppTerminal->Win.Control.Height)) {
                        sys_exec("terminal.elf");
                        sys_sleep(300);
                    }
                    else if (local_y >= btnAppGfile->Win.Control.Top && local_y <= (btnAppGfile->Win.Control.Top + btnAppGfile->Win.Control.Height)) {
                        sys_exec("gfile.elf");
                        sys_sleep(300);
                    }
                    
                    is_start_menu_open = 0;
                    frmStartMenu->Win.Control.Visible = 0;
                    click_handled = 1;
                }
                else if (is_start_menu_open) {
                    is_start_menu_open = 0;
                    frmStartMenu->Win.Control.Visible = 0;
                }

                if (!click_handled && dragging_slot == -1) {
                    Processar_Clique_Janela(&m);
                }

            } else {
                // 🔒 TRANCA: Protege a escrita de coordenadas durante movimento continuo
                ipc_lock(&IPC_CONTROL->lock);
                if (dragging_slot != -1) {
                    IPC_WINDOW_LIST[dragging_slot].x = m.x - offX;
                    IPC_WINDOW_LIST[dragging_slot].y = m.y - offY;
                } else {
                    int s = IPC_CONTROL->active_focus_slot;
                    if (s != -1 && IPC_WINDOW_LIST[s].is_active) {
                        IPC_WINDOW_LIST[s].local_click_x = m.x - IPC_WINDOW_LIST[s].x;
                        IPC_WINDOW_LIST[s].local_click_y = m.y - IPC_WINDOW_LIST[s].y;
                        IPC_WINDOW_LIST[s].has_click_event = 1; 
                    }
                }
                ipc_unlock(&IPC_CONTROL->lock); // 🔑 LIBERA
            }
        } else {
            was_clicked = 0;
            dragging_slot = -1;
        }

        Executar_Pipeline_Composicao(&m);
        
        sys_sleep(10);
    }
}
