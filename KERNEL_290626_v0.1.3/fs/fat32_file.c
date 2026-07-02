/**
 * ============================================================================
 * FAT32 FILE OPERATIONS - NATIVE AHCI/SATA MANIPULATION
 * ============================================================================
 * Descrição: Implementação de leitura, escrita, append e exibição em disco SATA.
 * Localização: fs/fat32_file.c
 * ============================================================================
 */

#include "fs/fat32_file.h" 
#include "fs/fat32.h"
#include "fs/fat32_logic.h"
#include "drivers/hw/ahci_hal.h"  
#include "drivers/pd/storage.h"
#include "drivers/video.h"
#include "util/string.h"
#include "mem/heap.h"

extern uint32_t current_dir_cluster;
extern fat32_bpb_t disk_bpb;
extern uint8_t fat32_current_dev_id;

/**
 * @brief Lê e exibe o conteúdo de um arquivo no console VESA.
 */
int fat32_display_file(const char* target_name) {
    fat32_directory_entry_t entry;
    
    if (fat32_find_entry(target_name, &entry) != 0) {
        return FS_NOT_FOUND;
    }

    uint8_t* file_data = (uint8_t*)kmalloc(entry.file_size + 1);
    if (!file_data) return FS_ERROR_READ;

    if (fat32_read_file(target_name, file_data, entry.file_size) == FS_SUCCESS) {
        file_data[entry.file_size] = '\0'; 

        uint32_t cur_x = 20; 
        uint32_t cur_y = 150; 
        uint32_t color = 0x00FFFFFF; // Branco

        for (uint32_t i = 0; i < entry.file_size; i++) {
            char c = (char)file_data[i];
            
            if (c == '\n') {
                cur_x = 20;
                cur_y += 16; 
            } else if (c >= 32 && c <= 126) {
                draw_char(cur_x, cur_y, c, color, 1);
                cur_x += 8;
            }

            if (cur_x > 1000) { cur_x = 20; cur_y += 16; }
            if (cur_y > 740) break;
        }
    }

    kfree(file_data);
    return FS_SUCCESS;
}

/**
 * @brief Lê um arquivo com suporte a offset (Fundamental para o ELF Loader).
 */
int fat32_read_file_at_offset(const char* name, uint8_t* buffer, uint32_t size, uint32_t offset) {
    if (!buffer || size == 0) return -1;

    fat32_directory_entry_t entry;
    if (fat32_find_entry(name, &entry) != 0) return FS_NOT_FOUND;

    if (offset >= entry.file_size) return -1;
    if (offset + size > entry.file_size) size = entry.file_size - offset;

    uint32_t bytes_per_cluster = disk_bpb.sectors_per_cluster * 512;
    uint32_t current_c = ((uint32_t)entry.cluster_high << 16) | entry.cluster_low;

    uint32_t skip = offset / bytes_per_cluster;
    for (uint32_t j = 0; j < skip; j++) {
        current_c = fat32_get_next_cluster(current_c);
        if (current_c >= FAT32_EOC) return -1;
    }

    uint32_t internal_offset = offset % bytes_per_cluster;
    uint32_t total_read = 0;
    uint8_t* temp_c = (uint8_t*)kmalloc(bytes_per_cluster);
    if (!temp_c) return FS_ERROR_READ;

    while (current_c >= 2 && current_c < FAT32_EOC && total_read < size) {
        // UNIFICAÇÃO: Lendo clusters inteiros de forma segura através da camada Storage
        for (uint32_t s = 0; s < disk_bpb.sectors_per_cluster; s++) {
            storage_read_sectors(fat32_current_dev_id, fat32_cluster_to_lba(current_c) + s, 1, temp_c + (s * 512));
        }

        uint32_t avail = bytes_per_cluster - internal_offset;
        uint32_t to_copy = (size - total_read > avail) ? avail : (size - total_read);

        memcpy(buffer + total_read, temp_c + internal_offset, to_copy);
        total_read += to_copy;
        internal_offset = 0;
        current_c = fat32_get_next_cluster(current_c);
    }

    kfree(temp_c);
    return FS_SUCCESS;
}

int fat32_read_file(const char* name, uint8_t* buffer, uint32_t max_size) {
    return fat32_read_file_at_offset(name, buffer, max_size, 0);
}

/**
 * @brief Grava um arquivo (Suporta caminhos simples como subpasta/arquivo.ext e sobrescreve se existir).
 */
int fat32_write_file(const char* name, uint8_t* input_buffer, uint32_t size) {
    uint32_t dir_c = current_dir_cluster; // Padrão: diretório atual
    const char* file_name_part = name;    // Padrão: o nome inteiro é o arquivo

// 1. PATH PARSER: Encontra a última barra '/' para separar o caminho do nome do arquivo
    int last_slash = -1;
    for (int i = 0; name[i] != '\0'; i++) {
        if (name[i] == '/') {
            last_slash = i;
        }
    }

    if (last_slash != -1) {
        // Encontrou um caminho! Vamos isolar o nome da pasta destino
        char dir_name[128];
        if (last_slash < 127) {
            memcpy(dir_name, name, last_slash);
            dir_name[last_slash] = '\0'; // Garante a finalização da string
            
            // Avança o ponteiro do nome do arquivo para depois da barra
            file_name_part = name + last_slash + 1;

            // Busca a entrada dessa subpasta no diretório atual
            fat32_directory_entry_t dir_entry;
            
            // CORREÇÃO CRÍTICA: Garante que a comparação de sucesso use a regra padrão do seu VFS (0 para achado)
            if (fat32_find_entry(dir_name, &dir_entry) == 0) {
                // Se a entrada existe e tem o atributo de diretório (0x10)
                if (dir_entry.attributes & 0x10) {
                    dir_c = ((uint32_t)dir_entry.cluster_high << 16) | dir_entry.cluster_low;
                    if (dir_c == 0) dir_c = 2; // Se for zero, força ir para a raiz (Cluster 2)
                } else {
                    return FS_NOT_FOUND; // Encontrou, mas é um arquivo e não uma pasta!
                }
            } else {
                return FS_NOT_FOUND; // A pasta de destino não existe!
            }
        }
    }

    // 2. Transforma APENAS a parte do nome do arquivo em formato 8.3
    char fat_name[11];
    fat32_to_83_filename(file_name_part, fat_name);

    // Se o arquivo já existia neste diretório específico, remove o antigo
    fat32_directory_entry_t dummy;
    if (fat32_find_entry(name, &dummy) == 0) {
        fat32_delete_file(name);
    }

    uint32_t bytes_per_cluster = disk_bpb.sectors_per_cluster * 512;
    uint32_t bytes_left = size;
    uint32_t current_c, prev_c = 0, first_c = 0;
    uint8_t* data_ptr = input_buffer;

    // Loop de Alocação e Escrita Física nos Clusters de Dados (Mantido seu código estável)
    while (bytes_left > 0) {
        current_c = fat32_find_free_cluster();
        if (current_c == 0) return FS_DISK_FULL;
        if (first_c == 0) first_c = current_c;
        if (prev_c != 0) fat32_set_cluster_entry(prev_c, current_c);

        uint8_t* cluster_buf = (uint8_t*)kmalloc(bytes_per_cluster);
        memset(cluster_buf, 0, bytes_per_cluster);
        uint32_t to_write = (bytes_left > bytes_per_cluster) ? bytes_per_cluster : bytes_left;
        memcpy(cluster_buf, data_ptr, to_write);

        for (uint32_t s = 0; s < disk_bpb.sectors_per_cluster; s++) {
            storage_write_sectors(fat32_current_dev_id, fat32_cluster_to_lba(current_c) + s, 1, cluster_buf + (s * 512));
        }
        
        fat32_set_cluster_entry(current_c, FAT32_EOC);
        kfree(cluster_buf);
        data_ptr += to_write;
        bytes_left -= to_write;
        prev_c = current_c;
    }

    // 3. Criar entrada no diretório pai (Agora usando a variável dir_c atualizada dinamicamente!)
    __attribute__((aligned(16))) uint8_t sect[512];
    
    while (dir_c < FAT32_EOC) {
        uint32_t lba = fat32_cluster_to_lba(dir_c);
        for (uint32_t s = 0; s < disk_bpb.sectors_per_cluster; s++) {
            
            storage_read_sectors(fat32_current_dev_id, lba + s, 1, sect);
            
            for (int i = 0; i < 512; i += 32) {
                if (sect[i] == 0x00 || sect[i] == 0xE5) {
                    fat32_directory_entry_t* e = (fat32_directory_entry_t*)&sect[i];
                    memcpy(e->filename, fat_name, 11);
                    e->attributes = 0x20; 
                    e->cluster_high = (first_c >> 16) & 0xFFFF;
                    e->cluster_low = first_c & 0xFFFF;
                    e->file_size = size;
                    
                    storage_write_sectors(fat32_current_dev_id, lba + s, 1, sect);
                    return FS_SUCCESS;
                }
            }
        }
        dir_c = fat32_get_next_cluster(dir_c);
    }
    return FS_DISK_FULL;
}

/**
 * @brief Deleta um arquivo e limpa a tabela FAT.
 */
int fat32_delete_file(const char* filename) {
    char fat_name[11];
    fat32_to_83_filename(filename, fat_name);
    __attribute__((aligned(16))) uint8_t sect[512];
    uint32_t dir_c = current_dir_cluster;

    while (dir_c < FAT32_EOC) {
        uint32_t lba = fat32_cluster_to_lba(dir_c);
        for (uint32_t s = 0; s < disk_bpb.sectors_per_cluster; s++) {
            
            storage_read_sectors(fat32_current_dev_id, lba + s, 1, sect);
            
            fat32_directory_entry_t* e = (fat32_directory_entry_t*)sect;
            for (int i = 0; i < 16; i++) {
                if (e[i].filename[0] == 0x00) return FS_NOT_FOUND;
                if (memcmp(e[i].filename, fat_name, 11) == 0) {
                    uint32_t c = ((uint32_t)e[i].cluster_high << 16) | e[i].cluster_low;
                    e[i].filename[0] = 0xE5; 
                    
                    // CORREÇÃO CRÍTICA: Escrita unificada ao marcar arquivo como deletado
                    storage_write_sectors(fat32_current_dev_id, lba + s, 1, sect);
                    
                    while (c >= 2 && c < FAT32_EOC) {
                        uint32_t next = fat32_get_next_cluster(c);
                        fat32_set_cluster_entry(c, 0); 
                        c = next;
                    }
                    return FS_SUCCESS;
                }
            }
        }
        dir_c = fat32_get_next_cluster(dir_c);
    }
    return FS_NOT_FOUND;
}

/**
 * @brief Adiciona dados ao final de um arquivo existente.
 */
int fat32_append_file(const char* filename, uint8_t* data, uint32_t data_len) {
    if (!filename || !data || data_len == 0) return FS_SUCCESS;

    fat32_directory_entry_t entry;
    uint32_t entry_lba = 0;
    int entry_idx = 0;
    
    uint32_t dir_c = current_dir_cluster;
    __attribute__((aligned(16))) uint8_t sect[512];
    char fat_name[11];
    fat32_to_83_filename(filename, fat_name);
    int found = 0;

    while (dir_c < FAT32_EOC && !found) {
        uint32_t lba = fat32_cluster_to_lba(dir_c);
        for (uint32_t s = 0; s < disk_bpb.sectors_per_cluster; s++) {
            
            storage_read_sectors(fat32_current_dev_id, lba + s, 1, sect);
            
            fat32_directory_entry_t* es = (fat32_directory_entry_t*)sect;
            for (int i = 0; i < 16; i++) {
                if (memcmp(es[i].filename, fat_name, 11) == 0) {
                    entry = es[i]; 
                    entry_lba = lba + s; 
                    entry_idx = i;
                    found = 1; 
                    break;
                }
            }
            if (found) break;
        }
        if (!found) dir_c = fat32_get_next_cluster(dir_c);
    }
    if (!found) return FS_NOT_FOUND;

    uint32_t bytes_per_cluster = disk_bpb.sectors_per_cluster * 512;
    uint32_t last_c = ((uint32_t)entry.cluster_high << 16) | entry.cluster_low;
    while (fat32_get_next_cluster(last_c) < FAT32_EOC) 
        last_c = fat32_get_next_cluster(last_c);

    uint32_t pos = entry.file_size % bytes_per_cluster;
    uint32_t written = 0;
    uint8_t* buf = (uint8_t*)kmalloc(bytes_per_cluster);
    if (!buf) return FS_ERROR_WRITE;

    if (pos != 0 || entry.file_size == 0) {
        uint32_t space = bytes_per_cluster - pos;
        for (uint32_t s = 0; s < disk_bpb.sectors_per_cluster; s++) {
            storage_read_sectors(fat32_current_dev_id, fat32_cluster_to_lba(last_c) + s, 1, buf + (s * 512));
        }
        
        uint32_t to_copy = (data_len < space) ? data_len : space;
        memcpy(buf + pos, data, to_copy);
        
        // CORREÇÃO CRÍTICA: Escrita unificada no append
        for (uint32_t s = 0; s < disk_bpb.sectors_per_cluster; s++) {
            storage_write_sectors(fat32_current_dev_id, fat32_cluster_to_lba(last_c) + s, 1, buf + (s * 512));
        }
        written = to_copy;
    }

    uint32_t prev = last_c;
    while (written < data_len) {
        uint32_t new_c = fat32_find_free_cluster();
        if (!new_c) { kfree(buf); return FS_DISK_FULL; }
        fat32_set_cluster_entry(prev, new_c);
        fat32_set_cluster_entry(new_c, FAT32_EOC);
        memset(buf, 0, bytes_per_cluster);
        uint32_t to_c = (data_len - written > bytes_per_cluster) ? bytes_per_cluster : (data_len - written);
        memcpy(buf, data + written, to_c);
        
        // CORREÇÃO CRÍTICA: Escrita unificada nos novos clusters alocados
        for (uint32_t s = 0; s < disk_bpb.sectors_per_cluster; s++) {
            storage_write_sectors(fat32_current_dev_id, fat32_cluster_to_lba(new_c) + s, 1, buf + (s * 512));
        }
        written += to_c; 
        prev = new_c;
    }

    storage_read_sectors(fat32_current_dev_id, entry_lba, 1, sect);
    ((fat32_directory_entry_t*)sect)[entry_idx].file_size += data_len;
    
    // CORREÇÃO CRÍTICA: Atualização unificada de tamanho
    storage_write_sectors(fat32_current_dev_id, entry_lba, 1, sect);

    kfree(buf);
    return FS_SUCCESS;
}

/**
 * @brief Cria um novo subdiretório no diretório atual.
 */
int fat32_create_dir(const char* name) {
    char fat_name[11];
    fat32_to_83_filename(name, fat_name);

    // 1. MELHORIA/PROTEÇÃO: Garante que não há colisão de nomes no diretório atual
    fat32_directory_entry_t dummy;
    if (fat32_find_entry(name, &dummy) == 0) {
        // Se retornar 0, significa que achou uma entrada com o mesmo nome.
        // Retornamos um erro para o Shell saber que a pasta já existe.
        return FS_NOT_FOUND; 
    }

    // 2. Aloca um cluster livre para ser o corpo da nova pasta
    uint32_t new_cluster = fat32_find_free_cluster();
    if (new_cluster == 0) return FS_DISK_FULL;

    // Salva na tabela FAT que este cluster está ocupado (Fim de Cadeia)
    fat32_set_cluster_entry(new_cluster, FAT32_EOC);

    // 3. INICIALIZAÇÃO DO CLUSTER DA NOVA PASTA (Gravando '.' e '..')
    uint32_t bytes_per_cluster = disk_bpb.sectors_per_cluster * 512;
    uint8_t* cluster_buf = (uint8_t*)kmalloc(bytes_per_cluster);
    if (!cluster_buf) return FS_DISK_FULL;
    memset(cluster_buf, 0, bytes_per_cluster);

    // Configura a entrada "." (Aponta para o próprio cluster da subpasta)
    fat32_directory_entry_t* dot = (fat32_directory_entry_t*)&cluster_buf[0];
    memset(dot->filename, ' ', 11); // Preenche os 11 caracteres com espaço padrão (0x20)
    dot->filename[0] = '.';         // Coloca o ponto na primeira posição
    dot->attributes = 0x10;         // Atributo: Diretório
    dot->cluster_high = (new_cluster >> 16) & 0xFFFF;
    dot->cluster_low = new_cluster & 0xFFFF;
    dot->file_size = 0;

    // Configura a entrada ".." (Aponta para o cluster do pai: current_dir_cluster)
    uint32_t parent_c = current_dir_cluster;
    if (parent_c == 2) parent_c = 0;

    fat32_directory_entry_t* dotdot = (fat32_directory_entry_t*)&cluster_buf[32];
    memset(dotdot->filename, ' ', 11); // Preenche os 11 caracteres com espaço padrão (0x20)
    dotdot->filename[0] = '.';         // Primeiro ponto
    dotdot->filename[1] = '.';         // Segundo ponto
    dotdot->attributes = 0x10;         // Atributo: Diretório
    dotdot->cluster_high = (parent_c >> 16) & 0xFFFF;
    dotdot->cluster_low = parent_c & 0xFFFF;
    dotdot->file_size = 0;

    // Escreve os setores do novo cluster usando seu loop de armazenamento
    uint32_t start_lba = fat32_cluster_to_lba(new_cluster);
    for (uint32_t s = 0; s < disk_bpb.sectors_per_cluster; s++) {
        storage_write_sectors(fat32_current_dev_id, start_lba + s, 1, cluster_buf + (s * 512));
    }
    kfree(cluster_buf);

    // 4. REGISTRA A NOVA ENTRADA DA PASTA DENTRO DO DIRETÓRIO PAI
    uint32_t dir_c = current_dir_cluster;
    __attribute__((aligned(16))) uint8_t sect[512];

    while (dir_c < FAT32_EOC) {
        uint32_t lba = fat32_cluster_to_lba(dir_c);
        for (uint32_t s = 0; s < disk_bpb.sectors_per_cluster; s++) {
            
            storage_read_sectors(fat32_current_dev_id, lba + s, 1, sect);

            // Varre o setor procurando uma entrada vazia (0x00) ou deletada (0xE5)
            fat32_directory_entry_t* e = (fat32_directory_entry_t*)sect;
            for (int i = 0; i < 16; i++) { // 16 entradas de 32 bytes por setor
                if (e[i].filename[0] == 0x00 || e[i].filename[0] == 0xE5) {
                    
                    memcpy(e[i].filename, fat_name, 11);
                    e[i].attributes = 0x10; // INDICA DIRETÓRIO
                    e[i].cluster_high = (new_cluster >> 16) & 0xFFFF;
                    e[i].cluster_low = new_cluster & 0xFFFF;
                    e[i].file_size = 0;     // Pastas possuem tamanho nominal em bytes zero

                    // Salva as alterações de volta no disco com sua assinatura estável
                    storage_write_sectors(fat32_current_dev_id, lba + s, 1, sect);
                    return FS_SUCCESS; // Sucesso absoluto
                }
            }
        }
        dir_c = fat32_get_next_cluster(dir_c);
    }

    return FS_DISK_FULL;
}

int fat32_rename(const char* old_name, const char* new_name) {
    fat32_directory_entry_t entry;
    uint32_t setor_da_entrada = 0;
    uint32_t offset_no_setor = 0;

    if (!old_name || !new_name) return -1;

    // 1. Procura a entrada usando a nova função estendida
    if (fat32_find_entry_ext(old_name, &entry, &setor_da_entrada, &offset_no_setor) != 0) {
        return -1; // Arquivo original não encontrado
    }

    // 2. Garante que o novo nome já não esteja ocupado por outro arquivo
    fat32_directory_entry_t dummy;
    if (fat32_find_entry(new_name, &dummy) == 0) {
        return -2; // Erro: O novo nome já existe!
    }

    // 3. Lê o setor onde a entrada original está residindo
    __attribute__((aligned(16))) uint8_t setor_buffer[512];
    if (storage_read_sectors(fat32_current_dev_id, setor_da_entrada, 1, setor_buffer) == 0) {
        return -3; // Erro de leitura física
    }

    // 4. Aponta para a estrutura correta dentro do buffer lide do disco
    fat32_directory_entry_t* entry_no_disco = (fat32_directory_entry_t*)&setor_buffer[offset_no_setor];

    // 5. Converte o novo nome para o formato FAT 8.3 usando a sua própria função nativa!
    char nome_fat[11];
    fat32_to_83_filename(new_name, nome_fat);

    // 6. Altera os 11 bytes do nome no buffer
    memcpy(entry_no_disco->filename, nome_fat, 11);

    // 7. Grava de volta o setor alterado para o Pendrive/HD
    // Note: Use uma função de escrita equivalente ao seu storage_read_sectors, ex: storage_write_sectors
    if (storage_write_sectors(fat32_current_dev_id, setor_da_entrada, 1, setor_buffer) == 0) {
        return -4; // Erro ao persistir os dados no hardware
    }

    return 0; // Sucesso! Renomeou/Moveu perfeitamente
}

/**
 * @brief Obtém metadados de um arquivo ou pasta no FAT32.
 */
int fat32_stat(const char* name, file_info_t* out_info) {
    fat32_directory_entry_t entry;
    
    if (!name || !out_info) return -1;

    // Busca a entrada do arquivo usando a sua função estável
    if (fat32_find_entry(name, &entry) != 0) {
        return -1; // Não encontrado
    }

    // Preenche os dados que o Ring 3 pediu
    out_info->size = entry.file_size;        // Tamanho em bytes
    out_info->attributes = entry.attributes;  // Atributos (ex: 0x10 = pasta)

    return 0; // Sucesso!
}
