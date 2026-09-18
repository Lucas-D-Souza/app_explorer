#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "SdUsbManager.hpp"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/param.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_heap_caps.h"

LV_IMAGE_DECLARE(icon_cloud); // Usa a mesma imagem da nuvem do Menu de Fábrica
static lv_obj_t * scr_splash = NULL;

static void show_splash_screen(const char* version) {
    // 1. Cria a tela de Splash
    scr_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_splash, lv_color_black(), 0);
    lv_obj_remove_flag(scr_splash, LV_OBJ_FLAG_SCROLLABLE);

    // 2. Container Transparente (Flexbox para alinhar Ícone + Texto)
    lv_obj_t * cont_center = lv_obj_create(scr_splash);
    lv_obj_set_size(cont_center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(cont_center, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_bg_opa(cont_center, LV_OPA_TRANSP, 0); 
    lv_obj_set_style_border_width(cont_center, 0, 0); 
    
    // Configura o Flexbox Lado a Lado
    lv_obj_set_flex_flow(cont_center, LV_FLEX_FLOW_ROW); 
    lv_obj_set_flex_align(cont_center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont_center, 0, 0);
    lv_obj_set_style_pad_column(cont_center, 25, 0); // Espaço entre a nuvem e o texto

    // 3. Ícone da Nuvem
    lv_obj_t * logo = lv_image_create(cont_center);
    lv_image_set_src(logo, &icon_cloud);
    
    // Escala para dobrar o tamanho e pinta com a cor Azul do tema
    lv_image_set_scale(logo, 512);
    lv_obj_set_size(logo, 100, 100); 
    lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor(logo, lv_color_hex(0x0C85AD), 0); // Azul claro

    // 4. Texto do App
    lv_obj_t * title = lv_label_create(cont_center);
    lv_label_set_text(title, "Web\nExplorer"); // Quebra de linha para ficar proporcional
    lv_obj_set_style_text_font(title, &lv_font_montserrat_30, 0); 
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    // 5. Versão no rodapé
    lv_obj_t * lbl_version = lv_label_create(scr_splash);
    lv_label_set_text_fmt(lbl_version, "v%s", version);
    lv_obj_set_style_text_font(lbl_version, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_version, lv_color_hex(0x555555), 0); 
    lv_obj_align(lbl_version, LV_ALIGN_BOTTOM_MID, 0, -25);

    // 6. Carrega na tela imediatamente
    lv_screen_load(scr_splash);
}

static const char *TAG = "ExplorerFW";
#define BOOT_BTN_PIN GPIO_NUM_0

static lv_obj_t *lbl_status = NULL;

extern "C" const uint8_t index_html_start[] asm("_binary_index_html_start");
extern "C" const uint8_t index_html_end[]   asm("_binary_index_html_end");

static void return_to_factory() {
    ESP_LOGI(TAG, "Retornando ao Factory Firmware...");
    if (lvgl_port_lock(0)) {
        bsp_display_brightness_set(0); 
        lvgl_port_unlock();
    }
    const esp_partition_t *factory_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL
    );
    if (factory_part) {
        esp_ota_set_boot_partition(factory_part);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        esp_restart();
    }
}

// ---------------------------------------------------------
// FUNÇÃO EXCLUSÃO RECURSIVA AGRESSIVA
// ---------------------------------------------------------
static int delete_recursively(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) return -1;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            
            char *full_path = (char*)malloc(768);
            if (full_path) {
                snprintf(full_path, 768, "%s/%s", path, ent->d_name);
                
                struct stat child_st;
                if (stat(full_path, &child_st) == 0 && S_ISDIR(child_st.st_mode)) {
                    delete_recursively(full_path);
                } else {
                    remove(full_path); 
                }
                free(full_path);
            }
        }
        closedir(dir);
        // Desbloqueio do FATFS: unlink serve tanto para arquivo quanto para forçar diretório vazio
        int res = rmdir(path);
        if (res != 0) res = unlink(path);
        return res;
    } else {
        return remove(path); 
    }
}

static char *urldecode(const char *str) {
    char *new_string = strdup(str);
    char *ptr = new_string;
    while (ptr[0] && ptr[1] && ptr[2]) {
        if (ptr[0] == '%' && isxdigit((unsigned char)ptr[1]) && isxdigit((unsigned char)ptr[2])) {
            char hex[] = {ptr[1], ptr[2], 0};
            *ptr = strtol(hex, NULL, 16);
            memmove(ptr + 1, ptr + 3, strlen(ptr + 3) + 1);
        }
        ptr++;
    }
    return new_string;
}

static esp_err_t index_get_handler(httpd_req_t *req) {
    const size_t html_size = (index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, html_size);
    return ESP_OK;
}

static esp_err_t api_list_handler(httpd_req_t *req) {
    char dir_path[256] = "/sdcard";
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[256];
            if (httpd_query_key_value(buf, "dir", param, sizeof(param)) == ESP_OK) {
                char* decoded = urldecode(param);
                strncpy(dir_path, decoded, sizeof(dir_path));
                free(decoded);
            }
        }
        free(buf);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(root, "files");

    DIR *dir = opendir(dir_path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", ent->d_name);
            cJSON_AddBoolToObject(item, "is_dir", (ent->d_type == DT_DIR));
            if (ent->d_type != DT_DIR) {
                char *full_path = (char*)malloc(768);
                if (full_path) {
                    snprintf(full_path, 768, "%s/%s", dir_path, ent->d_name);
                    struct stat st;
                    if (stat(full_path, &st) == 0) cJSON_AddNumberToObject(item, "size", st.st_size);
                    else cJSON_AddNumberToObject(item, "size", 0);
                    free(full_path);
                }
            } else cJSON_AddNumberToObject(item, "size", 0);
            cJSON_AddItemToArray(array, item);
        }
        closedir(dir);
    }
    const char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free((void *)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_upload_handler(httpd_req_t *req) {
    char file_path[256] = "/sdcard/upload.bin";
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[256];
            if (httpd_query_key_value(buf, "path", param, sizeof(param)) == ESP_OK) {
                char* decoded = urldecode(param);
                strncpy(file_path, decoded, sizeof(file_path));
                free(decoded);
            }
        }
        free(buf);
    }

    FILE *f = fopen(file_path, "wb");
    if (!f) return ESP_FAIL;

    // O SEGREDO DOS FIRMWARES MADUROS: Buffer cravado em 8KB, forçado na RAM INTERNA (SRAM) e pronto para DMA.
    char *recv_buf = (char *)heap_caps_malloc(8192, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); 
    if (!recv_buf) { fclose(f); return ESP_FAIL; }

    int remaining = req->content_len;
    while (remaining > 0) {
        int received = httpd_req_recv(req, recv_buf, MIN(remaining, 8192));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        fwrite(recv_buf, 1, received, f);
        remaining -= received;
    }

    fclose(f);
    
    // Libera a memória usando o comando especial de hardware
    heap_caps_free(recv_buf); 
    httpd_resp_sendstr(req, "Upload Concluido");
    return ESP_OK;
}

static esp_err_t api_delete_handler(httpd_req_t *req) {
    char file_path[256] = "";
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[256];
            if (httpd_query_key_value(buf, "path", param, sizeof(param)) == ESP_OK) {
                char* decoded = urldecode(param);
                strncpy(file_path, decoded, sizeof(file_path));
                free(decoded);
            }
        }
        free(buf);
    }
    
    delete_recursively(file_path);
    httpd_resp_sendstr(req, "Deletado");
    return ESP_OK;
}

static esp_err_t api_mkdir_handler(httpd_req_t *req) {
    // DESATIVADO TEMPORARIAMENTE DEVIDO A BUG DE CORRUPÇÃO FAT32 VIA SPI
    httpd_resp_send_err(req, HTTPD_501_METHOD_NOT_IMPLEMENTED, "Criação de pastas desativada.");
    return ESP_FAIL;
}

static esp_err_t api_rename_handler(httpd_req_t *req) {
    char old_path[256] = "", new_path[256] = "";
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param_o[256], param_n[256];
            if (httpd_query_key_value(buf, "old", param_o, sizeof(param_o)) == ESP_OK) {
                char* dec = urldecode(param_o); strncpy(old_path, dec, sizeof(old_path)); free(dec);
            }
            if (httpd_query_key_value(buf, "new", param_n, sizeof(param_n)) == ESP_OK) {
                 char* dec = urldecode(param_n); strncpy(new_path, dec, sizeof(new_path)); free(dec);
            }
        }
        free(buf);
    }
    
    if (rename(old_path, new_path) == 0) httpd_resp_sendstr(req, "Renomeado");
    else httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erro");
    return ESP_OK;
}

static esp_err_t api_download_handler(httpd_req_t *req) {
    char file_path[256] = "";
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[256];
            if (httpd_query_key_value(buf, "path", param, sizeof(param)) == ESP_OK) {
                char* dec = urldecode(param); strncpy(file_path, dec, sizeof(file_path)); free(dec);
            }
        }
        free(buf);
    }

    FILE *fp = fopen(file_path, "rb");
    if (!fp) { httpd_resp_send_404(req); return ESP_OK; }

    const char *ext = strrchr(file_path, '.');
    if (ext) {
        if (strcasecmp(ext, ".json") == 0 || strcasecmp(ext, ".txt") == 0) httpd_resp_set_type(req, "text/plain");
        else if (strcasecmp(ext, ".png") == 0) httpd_resp_set_type(req, "image/png");
        else if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) httpd_resp_set_type(req, "image/jpeg");
        else httpd_resp_set_type(req, "application/octet-stream"); 
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
    }

    // Mesmo escudo de hardware para o download
    char *http_buf = (char *)heap_caps_malloc(8192, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (http_buf) {
        size_t len;
        while ((len = fread(http_buf, 1, 8192, fp)) > 0) {
            httpd_resp_send_chunk(req, http_buf, len);
        }
        httpd_resp_send_chunk(req, NULL, 0); 
        heap_caps_free(http_buf);
    }
    fclose(fp);
    return ESP_OK;
}

static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 15;
    config.core_id = 0; 
    
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        
        // CORREÇÃO: Declaração das rotas nativas aceitas pelo C++
        httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_index);

        httpd_uri_t uri_list = { .uri = "/api/list", .method = HTTP_GET, .handler = api_list_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_list);

        httpd_uri_t uri_upload = { .uri = "/api/upload", .method = HTTP_POST, .handler = api_upload_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_upload);

        httpd_uri_t uri_delete = { .uri = "/api/delete", .method = HTTP_POST, .handler = api_delete_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_delete);
        
        httpd_uri_t uri_mkdir = { .uri = "/api/mkdir", .method = HTTP_POST, .handler = api_mkdir_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_mkdir);

        httpd_uri_t uri_rename = { .uri = "/api/rename", .method = HTTP_POST, .handler = api_rename_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_rename);

        httpd_uri_t uri_download = { .uri = "/api/download", .method = HTTP_GET, .handler = api_download_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_download);
    }
}

static void wifi_init_softap(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.ap.ssid, "WatchOS-Explorer");
    strcpy((char *)wifi_config.ap.password, "12345678"); 
    wifi_config.ap.ssid_len = strlen("WatchOS-Explorer");
    wifi_config.ap.channel = 6;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();
}

static lv_obj_t * build_ui() {
    lv_obj_t * scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_main, lv_color_black(), 0); // Fundo puro AMOLED

    // 1. Título Superior Simples
    lv_obj_t * title = lv_label_create(scr_main);
    lv_label_set_text(title, "Servidor de Arquivos");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    // 2. Card Central (Maior, elegante e bloqueado contra Scroll)
    lv_obj_t * card = lv_obj_create(scr_main);
    lv_obj_set_size(card, 340, 250); // Altura aumentada de 160 para 250!
    lv_obj_align(card, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x151515), 0); // Cinza escuro elegante
    lv_obj_set_style_border_color(card, lv_color_hex(0x333333), 0); // Borda cinza sutil
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 25, 0); // Bordas bem arredondadas
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE); // Mata a barra de rolagem!

    // Usamos Flexbox no Card para organizar os textos automaticamente (distribuição perfeita)
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 3. Informações da Rede
    lv_obj_t * lbl_wifi = lv_label_create(card);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI " WatchOS-Explorer");
    lv_obj_set_style_text_color(lbl_wifi, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_wifi, &lv_font_montserrat_20, 0);

    lv_obj_t * lbl_pass = lv_label_create(card);
    lv_label_set_text(lbl_pass, "Senha: 12345678");
    lv_obj_set_style_text_color(lbl_pass, lv_color_hex(0xAAAAAA), 0); // Cinza claro
    lv_obj_set_style_text_font(lbl_pass, &lv_font_montserrat_16, 0);

    // 4. Instrução + IP de Destaque
    lv_obj_t * lbl_inst = lv_label_create(card);
    lv_label_set_text(lbl_inst, "Acesse pelo navegador:");
    lv_obj_set_style_text_color(lbl_inst, lv_color_hex(0x777777), 0); // Cinza mais escuro
    lv_obj_set_style_text_font(lbl_inst, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_top(lbl_inst, 15, 0); // Dá um respiro antes do IP

    lv_obj_t * lbl_ip = lv_label_create(card);
    lv_label_set_text(lbl_ip, "192.168.4.1");
    lv_obj_set_style_text_color(lbl_ip, lv_color_hex(0x0C85AD), 0); // O mesmo Azul lindo da Logo!
    lv_obj_set_style_text_font(lbl_ip, &lv_font_montserrat_30, 0); // Fonte Gigante 30

    // 5. Botão de Saída Moderno (Estilo "Alerta Contido")
    lv_obj_t * btn_exit = lv_btn_create(scr_main);
    lv_obj_set_size(btn_exit, 260, 60);
    lv_obj_align(btn_exit, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(btn_exit, lv_color_hex(0x2A0A0A), 0); // Fundo Vermelho Quase Preto
    lv_obj_set_style_radius(btn_exit, 30, 0);
    lv_obj_set_style_border_color(btn_exit, lv_color_hex(0xAA3333), 0); // Borda vermelha escura
    lv_obj_set_style_border_width(btn_exit, 1, 0);
    
    lv_obj_t * lbl_exit = lv_label_create(btn_exit);
    lv_label_set_text(lbl_exit, LV_SYMBOL_POWER " Encerrar e Voltar");
    lv_obj_set_style_text_color(lbl_exit, lv_color_hex(0xFF5555), 0); // Texto em vermelho vivo
    lv_obj_set_style_text_font(lbl_exit, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl_exit);
    
    lv_obj_add_event_cb(btn_exit, [](lv_event_t *e){ return_to_factory(); }, LV_EVENT_CLICKED, NULL);

    return scr_main; 
}

// ---------------------------------------------------------
// HACK DE HARDWARE: Destrava o Barramento I2C após o Soft Reset (OTA)
// ---------------------------------------------------------
static void clear_i2c_bus(void) {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT_OD;
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_14) | (1ULL << GPIO_NUM_15);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    gpio_set_level(GPIO_NUM_15, 1); esp_rom_delay_us(100);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(GPIO_NUM_14, 0); esp_rom_delay_us(100);
        gpio_set_level(GPIO_NUM_14, 1); esp_rom_delay_us(100);
    }
    gpio_set_level(GPIO_NUM_15, 0); esp_rom_delay_us(100);
    gpio_set_level(GPIO_NUM_14, 1); esp_rom_delay_us(100);
    gpio_set_level(GPIO_NUM_15, 1); esp_rom_delay_us(100);

    gpio_reset_pin(GPIO_NUM_14);
    gpio_reset_pin(GPIO_NUM_15);
}

extern "C" void app_main(void) {
    clear_i2c_bus();
    esp_ota_mark_app_valid_cancel_rollback();

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BOOT_BTN_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    bsp_display_start();
    
    // ==========================================
    // 1. MOSTRA A SPLASH IMEDIATAMENTE
    // ==========================================
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        show_splash_screen("1.0.0");
        bsp_display_unlock();
    }
    
    // Dá 50ms pro display e acende a tela
    vTaskDelay(pdMS_TO_TICKS(50)); 
    bsp_display_brightness_set(80);

    // ==========================================
    // 2. INICIA TAREFAS PESADAS DE FUNDO
    // ==========================================
    // O Wi-Fi SoftAP e o Cartão SD vão ser iniciados agora!
    // A tela não vai engasgar, pois a splash já foi renderizada e o SPI está livre!
    wifi_init_softap();
    start_webserver();
    SdUsbManager::get_instance().init_local_storage();

    // ==========================================
    // 3. PREPARA A INTERFACE E AGENDA A TRANSIÇÃO
    // ==========================================
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        // Recebe a tela recém-construída na memória RAM
        lv_obj_t * scr_main = build_ui();
        
        // Pede para o LVGL substituir a Splash pela scr_main.
        // Espera de 2500ms (2.5 segundos) de Splash, Anima de 500ms e DELETA a Splash (true)
        lv_scr_load_anim(scr_main, LV_SCR_LOAD_ANIM_FADE_ON, 500, 2500, true);
        
        bsp_display_unlock();
    }

    // Loop vigilante do Botão BOOT
    while(1) {
        if (gpio_get_level(BOOT_BTN_PIN) == 0) return_to_factory();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}