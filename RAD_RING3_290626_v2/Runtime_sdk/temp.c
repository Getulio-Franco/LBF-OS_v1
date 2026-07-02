#include "sdk/libgui.h"
#include "../system/graphics.h"
#include "../gui/wm.h"
#include "../system/string.h"
#include "../system/liblib.h"

// Componentes do Sistema encapsulados
#include "components/TOS_IPC.h"     
#include "components/TOSSerial.h"   

// Protótipos obrigatórios de renderização gráfica do subsistema
void gui_draw_form(TForm* form);
void gui_render_form(TForm* form);

// Inclusão de funções de controle mapeadas diretamente no subsistema do Kernel
extern void events_process_mouse(int x, int y, int pressed, int button);
extern void* g_focused_control;

// Protótipos das funções do TMemo encapsuladas na SDK
extern void GUI_Memo_AddStr(TGUIControl* memo, const char* str);
extern void GUI_Memo_Clear(TGUIControl* memo);

// Variáveis de controle do ambiente da aplicação
int my_app_slot = -1;
TGUIEnvironment MyApp;

// Configurações Globais de Dimensão baseadas no layout padrão estável
const int winWidth = 550;
const int winHeight = 410;

// Ponteiros Globais para Referência de Objetos RAD adaptados à nova SDK
TGUIControl* ExeButton    = NULL;
TGUIControl* ExeEdit      = NULL;
TGUIControl* ExeComboBox  = NULL;
TGUIControl* ExeCheckBox  = NULL;
TGUIControl* ExeRadio1    = NULL;
TGUIControl* ExeRadio2    = NULL;
TGUIControl* CloseButton   = NULL;
TGUIControl* ExeMemo      = NULL;

/* ============================================================================
 * FUNÇÃO: Flush_Grafico_Janela
 * Sincroniza buffers e renderiza a interface na tela de forma limpa
 * ============================================================================ */
void Flush_Grafico_Janela(void) {
    gui_draw_form((TForm*)MyApp.MainWindow);
    gui_render_form((TForm*)MyApp.MainWindow);
        
    int back_idx = (IPC_WINDOW_LIST[my_app_slot].active_buffer == 0) ? 1 : 0;
    uint8_t* shared_ptr = (back_idx == 0) 
        ? (uint8_t*)(uintptr_t)IPC_WINDOW_LIST[my_app_slot].buffer_ptr_0 
        : (uint8_t*)(uintptr_t)IPC_WINDOW_LIST[my_app_slot].buffer_ptr_1;
        
    uint8_t* local_ptr = graphics_get_buffer();
    if (shared_ptr && local_ptr) {
        memcpy(shared_ptr, local_ptr, winWidth * winHeight * 4); 
    }
    IPC_WINDOW_LIST[my_app_slot].active_buffer = back_idx;
}

/* ============================================================================
 * FUNÇÃO: Tratar_Fechamento_Software
 * ============================================================================ */
void Tratar_Fechamento_Software(void) {
    if (MyApp.MainWindow) {
        gui_set_prop(MyApp.MainWindow, PROP_VISIBLE, 0);
    }
    uint32_t* b0 = (uint32_t*)(uintptr_t)IPC_WINDOW_LIST[my_app_slot].buffer_ptr_0;
    uint32_t* b1 = (uint32_t*)(uintptr_t)IPC_WINDOW_LIST[my_app_slot].buffer_ptr_1;
    if (b0) memset(b0, 0, winWidth * winHeight * 4);
    if (b1) memset(b1, 0, winWidth * winHeight * 4);
    IPC_WINDOW_LIST[my_app_slot].is_active = 0;
    sys_sleep(50); 
}

/* ============================================================================
 * EVENTOS (CALLBACKS): Acionados automaticamente pela SDK
 * ============================================================================ */
void OnBtnPrincipalClick(void* sender) {
    char* texto_edit = GUI_Edit_GetText(ExeEdit);
    
    GUI_Memo_AddStr(ExeMemo, "Botao principal clicado!\n");
    if (texto_edit && texto_edit[0] != '\0') {
        GUI_Memo_AddStr(ExeMemo, "Conteudo Edit: ");
        GUI_Memo_AddStr(ExeMemo, texto_edit);
        GUI_Memo_AddStr(ExeMemo, "\n");
    }
}

void OnBtnFecharClick(void* sender) {
    GUI_Memo_AddStr(ExeMemo, "Fechando software via botao interno...\n");
    Tratar_Fechamento_Software();
}

void OnComboBoxChange(void* sender) {
    GUI_Memo_AddStr(ExeMemo, "ComboBox alterado.\n");
}

void OnCheckBoxClick(void* sender) {
    GUI_Memo_AddStr(ExeMemo, "CheckBox interagido.\n");
}

void OnRadio1Click(void* sender) {
    GUI_Memo_AddStr(ExeMemo, "Modo de Operacao: Opcao 1 activa.\n");
}

void OnRadio2Click(void* sender) {
    GUI_Memo_AddStr(ExeMemo, "Modo de Operacao: Opcao 2 activa.\n");
}

/* ============================================================================
 * FUNÇÃO PRINCIPAL (MAIN)
 * ============================================================================ */
int main(int argc, char* argv[]) {
    static int ultimo_x = 0;
    static int ultimo_y = 0;
    static int mouse_hold_timer = 0; 

    // Variáveis de controle de estado para renderização inteligente de foco
    static bool primeiro_desenho = true;
    static bool ultimo_estado_foco = false;

    graphics_init_app(winWidth, winHeight);
    wm_init();
    
    my_app_slot = OS_IPC_RegisterApp("Template de Componentes LBF", winWidth, winHeight);
    if (my_app_slot == -1) return -1; 
    
    graphics_set_slot(my_app_slot);
    GUI_InitApplication(&MyApp, my_app_slot, "Template de Componentes RAD v0.0.2", winWidth, winHeight);

    // Define o fundo preto no Form principal para dar contraste
    if (MyApp.MainWindow) {
        gui_set_prop(MyApp.MainWindow, PROP_COLOR, 0x000000);
    }

    // Inicialização Opcional de Hardware
    OS_CreateSerial(1, 9600);

    /* =========================================================================
     * DESIGN LAYOUT RAD PADRONIZADO E UNIFORME
     * ========================================================================= */
    int btnW = 210;   // Largura padrão expandida para evitar estouros
    int editW = 180;  
    int ctrlH = 30;

    // --- COLUNA DA ESQUERDA ---
    ExeButton = GUI_CreateButton(&MyApp, 10, 40, btnW, ctrlH, "Exemplo de TButton", OnBtnPrincipalClick);
    
    GUI_CreateLabel(&MyApp, 10, 85, "Exemplo de TLabel:");
    ExeEdit = GUI_CreateEdit(&MyApp, 10, 105, btnW, ctrlH, "Texto do TEdit", NULL);
    
    GUI_CreateLabel(&MyApp, 10, 145, "Exemplo de TComboBox:");
    ExeComboBox = GUI_CreateComboBox(&MyApp, 10, 165, btnW, ctrlH, OnComboBoxChange);
    if (ExeComboBox) {
        GUI_ComboBox_AddItem(ExeComboBox, "Item Opcao A");
        GUI_ComboBox_AddItem(ExeComboBox, "Item Opcao B");
    }

    // --- COLUNA DA DIREITA ---
    ExeCheckBox = GUI_CreateCheckBox(&MyApp, 250, 40, "Exemplo de TCheckBox", OnCheckBoxClick);

    ExeRadio1 = GUI_CreateRadioButton(&MyApp, 250, 75, "Radio Opcao 1", OnRadio1Click);
    ExeRadio2 = GUI_CreateRadioButton(&MyApp, 250, 105, "Radio Opcao 2", OnRadio2Click);
    
    CloseButton = GUI_CreateButton(&MyApp, 250, 155, btnW, ctrlH, "Fechar Software", OnBtnFecharClick);
    
    // --- PARTE INFERIOR (MEMO EDITÁVEL NESTE TEMPLATE) ---
    GUI_CreateLabel(&MyApp, 10, 205, "Exemplo de TMemo (Terminal / Editavel):");
    ExeMemo = GUI_CreateMemo(&MyApp, 10, 225, 530, 140);
    
    // Injeta com segurança o texto inicial direto na Heap dinâmica do componente de fábrica
    GUI_Memo_AddStr(ExeMemo, "Base estavel carregada. Clique aqui para digitar diretamente!\n");

    // Único desenho obrigatório no início (Cold Start)
    Flush_Grafico_Janela();

    /* =========================================================================
     * LOOP PRINCIPAL DE EVENTOS REVISADO, ULTRA-FLUIDO E ECONÔMICO
     * ========================================================================= */
    while(1) {
        if (IPC_WINDOW_LIST[my_app_slot].is_active == 0) {
            Tratar_Fechamento_Software();
            break;
        }

        bool euTenhoFoco = (IPC_CONTROL->active_focus_slot == my_app_slot);
        if (MyApp.MainWindow) {
            ((TForm*)MyApp.MainWindow)->ActiveFocus = euTenhoFoco;
        }

        bool precisa_redesenhar = false;

        // Gatilho do Cold-Start inicial
        if (primeiro_desenho) {
            primeiro_desenho = false;
            precisa_redesenhar = true;
        }

        // SE O FOCO MUDOU (ganhou ou perdeu), força o redesenho imediatamente!
        if (euTenhoFoco != ultimo_estado_foco) {
            ultimo_estado_foco = euTenhoFoco;
            precisa_redesenhar = true;
        }

        if (euTenhoFoco) {
            // --- Teclado ---
            char key = get_key();
            if (key != 0) {
                GUI_ProcessKeyboard(&MyApp, key); 
                precisa_redesenhar = true; 
            }

            // --- Mouse Click ---
            if (IPC_WINDOW_LIST[my_app_slot].has_click_event == 1) {
                if (mouse_hold_timer == 0) {
                    int rel_x = IPC_WINDOW_LIST[my_app_slot].local_click_x;
                    int rel_y = IPC_WINDOW_LIST[my_app_slot].local_click_y;
                    ultimo_x = rel_x;
                    ultimo_y = rel_y;
                    mouse_hold_timer = 2; 

                    // Define efeito visual de pressionado nos botões se o clique foi neles
                    if (ExeButton && rel_x >= ExeButton->Left && rel_x < (ExeButton->Left + ExeButton->Width) &&
                        rel_y >= ExeButton->Top && rel_y < (ExeButton->Top + ExeButton->Height)) {
                        gui_set_prop(ExeButton, PROP_STATE, 2);
                    }
                    else if (CloseButton && rel_x >= CloseButton->Left && rel_x < (CloseButton->Left + CloseButton->Width) &&
                             rel_y >= CloseButton->Top && rel_y < (CloseButton->Top + CloseButton->Height)) {
                        gui_set_prop(CloseButton, PROP_STATE, 2);
                    }

                    events_process_mouse(rel_x, rel_y, 1, 0);
                    
                    if (GUI_ProcessMouseClick(&MyApp, rel_x, rel_y)) {
                        precisa_redesenhar = true;
                    }
                }
                IPC_WINDOW_LIST[my_app_slot].has_click_event = 0;
            }

            // --- Mouse Release ---
            if (mouse_hold_timer > 0) {
                mouse_hold_timer--; 
                if (mouse_hold_timer == 0) {
                    if (ExeButton)   gui_set_prop(ExeButton, PROP_STATE, 0); 
                    if (CloseButton) gui_set_prop(CloseButton, PROP_STATE, 0); 
                    events_process_mouse(ultimo_x, ultimo_y, 0, 0); 
                    precisa_redesenhar = true;
                }
            }
        }

        // CORREÇÃO: Fora do bloco de foco para processar a mudança visual cinza/azul
        if (precisa_redesenhar) {
            Flush_Grafico_Janela();
        }
        
        sys_sleep(euTenhoFoco ? 16 : 32);
    }

    sys_exit(); 
    return 0;
}
