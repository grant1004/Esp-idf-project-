// ============================================================================
// ota_update.c - OTA (Over The Air) 遠端韌體更新模組實作
// 功能：透過 HTTP/HTTPS 下載並安裝新韌體
// ============================================================================

#include "ota_update.h"
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "mqtt_client.h"

// ============================================================================
// 外部函數引用
// ============================================================================
extern esp_mqtt_client_handle_t get_mqtt_client(void);

// ============================================================================
// 常數定義
// ============================================================================
#define OTA_RECV_TIMEOUT        5000    // HTTP 接收超時 (毫秒)
#define OTA_BUFFER_SIZE         1024    // OTA 緩衝區大小
#define OTA_TASK_STACK_SIZE     8192    // OTA 任務堆疊大小
#define OTA_TASK_PRIORITY       5       // OTA 任務優先順序
#define FIRMWARE_VERSION        "1.0.0" // 目前韌體版本

// ============================================================================
// 日誌標籤
// ============================================================================
static const char *TAG = "OTA_UPDATE";

// ============================================================================
// 全域變數
// ============================================================================
static ota_state_t current_state = OTA_STATE_IDLE;
static int current_progress = 0;
static ota_progress_callback_t progress_callback = NULL;
static TaskHandle_t ota_task_handle = NULL;
static ota_statistics_t ota_stats = {0};
static bool cancel_requested = false;

// ============================================================================
// 內部結構定義
// ============================================================================
typedef struct {
    ota_config_t config;
    esp_ota_handle_t update_handle;
    const esp_partition_t *update_partition;
    int binary_file_length;
    int image_header_was_checked;
    esp_app_desc_t new_app_info;
} ota_context_t;

// ============================================================================
// 內部函數宣告
// ============================================================================
static void ota_task(void *pvParameter);
static void ota_update_progress(int percentage, ota_state_t state, const char* message);
static esp_err_t ota_validate_image_header(esp_app_desc_t *new_app_info);
static void ota_send_mqtt_status(const char* message);

// ============================================================================
// 初始化 OTA 更新模組
// ============================================================================
esp_err_t ota_update_init(void)
{
    ESP_LOGI(TAG, "🚀 初始化 OTA 更新模組");
    
    // 清除統計資料
    memset(&ota_stats, 0, sizeof(ota_statistics_t));
    
    // 取得目前韌體版本資訊
    const esp_app_desc_t *app_desc = esp_app_get_description();
    strncpy(ota_stats.last_version, app_desc->version, sizeof(ota_stats.last_version) - 1);
    
    ESP_LOGI(TAG, "✅ OTA 模組初始化完成 - 目前版本: %s", app_desc->version);
    return ESP_OK;
}

// ============================================================================
// 啟動 OTA 韌體更新
// ============================================================================
esp_err_t ota_start_update(const ota_config_t* config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "❌ OTA 配置為空");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (current_state != OTA_STATE_IDLE) {
        ESP_LOGW(TAG, "⚠️ OTA 更新已在進行中");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "🔄 啟動 OTA 更新: %s", config->firmware_url);
    
    // 複製配置
    static ota_config_t local_config;
    memcpy(&local_config, config, sizeof(ota_config_t));
    
    // 重置取消標誌
    cancel_requested = false;
    
    // 建立 OTA 任務
    BaseType_t task_created = xTaskCreate(
        ota_task,
        "ota_task",
        OTA_TASK_STACK_SIZE,
        &local_config,
        OTA_TASK_PRIORITY,
        &ota_task_handle
    );
    
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "❌ 無法建立 OTA 任務");
        return ESP_ERR_NO_MEM;
    }
    
    ota_stats.total_updates++;
    current_state = OTA_STATE_DOWNLOADING;
    ota_send_mqtt_status("🔄 OTA 更新已啟動");
    
    return ESP_OK;
}

// ============================================================================
// OTA 更新任務
// ============================================================================
static void ota_task(void *pvParameter)
{
    ota_config_t *config = (ota_config_t*)pvParameter;
    esp_err_t err = ESP_OK;
    ota_context_t ota_ctx = {0};
    
    // 複製配置
    memcpy(&ota_ctx.config, config, sizeof(ota_config_t));
    
    ESP_LOGI(TAG, "🚀 OTA 任務開始執行");
    ota_update_progress(0, OTA_STATE_DOWNLOADING, "開始下載韌體");
    
    // 設定 HTTP 客戶端配置
    esp_http_client_config_t http_config = {
        .url = ota_ctx.config.firmware_url,
        .timeout_ms = ota_ctx.config.timeout_ms > 0 ? ota_ctx.config.timeout_ms : OTA_RECV_TIMEOUT,
        .keep_alive_enable = true,
        .user_data = &ota_ctx,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        ESP_LOGE(TAG, "❌ 無法初始化 HTTP 客戶端");
        current_state = OTA_STATE_ERROR;
        ota_stats.failed_updates++;
        ota_stats.last_result = OTA_RESULT_NETWORK_ERROR;
        goto ota_end;
    }
    
    // 取得 OTA 分區
    ota_ctx.update_partition = esp_ota_get_next_update_partition(NULL);
    if (ota_ctx.update_partition == NULL) {
        ESP_LOGE(TAG, "❌ 無法找到 OTA 更新分區");
        current_state = OTA_STATE_ERROR;
        ota_stats.failed_updates++;
        ota_stats.last_result = OTA_RESULT_INSTALL_ERROR;
        goto ota_end;
    }
    
    ESP_LOGI(TAG, "📦 寫入分區: %s (0x%08lx, %lu bytes)",
             ota_ctx.update_partition->label,
             ota_ctx.update_partition->address,
             ota_ctx.update_partition->size);
    
    // 開始 OTA 更新
    err = esp_ota_begin(ota_ctx.update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_ctx.update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ esp_ota_begin 失敗: %s", esp_err_to_name(err));
        current_state = OTA_STATE_ERROR;
        ota_stats.failed_updates++;
        ota_stats.last_result = OTA_RESULT_INSTALL_ERROR;
        goto ota_end;
    }
    
    // 執行 HTTP GET 請求
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ 無法連接到伺服器: %s", esp_err_to_name(err));
        current_state = OTA_STATE_ERROR;
        ota_stats.failed_updates++;
        ota_stats.last_result = OTA_RESULT_NETWORK_ERROR;
        goto ota_end;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "❌ HTTP 客戶端取得檔案長度失敗");
        current_state = OTA_STATE_ERROR;
        ota_stats.failed_updates++;
        ota_stats.last_result = OTA_RESULT_DOWNLOAD_ERROR;
        goto ota_end;
    }
    
    ESP_LOGI(TAG, "📊 韌體大小: %d bytes", content_length);
    ota_ctx.binary_file_length = content_length;
    
    int binary_file_downloaded = 0;
    char *ota_write_data = malloc(OTA_BUFFER_SIZE);
    if (!ota_write_data) {
        ESP_LOGE(TAG, "❌ 無法分配 OTA 緩衝區記憶體");
        current_state = OTA_STATE_ERROR;
        ota_stats.failed_updates++;
        ota_stats.last_result = OTA_RESULT_MEMORY_ERROR;
        goto ota_end;
    }
    
    // 下載和寫入韌體數據
    while (1) {
        if (cancel_requested) {
            ESP_LOGW(TAG, "⚠️ 使用者取消 OTA 更新");
            current_state = OTA_STATE_ERROR;
            ota_stats.failed_updates++;
            ota_stats.last_result = OTA_RESULT_DOWNLOAD_ERROR;
            break;
        }
        
        int data_read = esp_http_client_read(client, ota_write_data, OTA_BUFFER_SIZE);
        if (data_read < 0) {
            ESP_LOGE(TAG, "❌ HTTP 下載資料錯誤");
            current_state = OTA_STATE_ERROR;
            ota_stats.failed_updates++;
            ota_stats.last_result = OTA_RESULT_DOWNLOAD_ERROR;
            break;
        } else if (data_read > 0) {
            // 檢查韌體標頭 (僅第一次)
            if (ota_ctx.image_header_was_checked == false) {
                esp_app_desc_t new_app_info;
                if (data_read > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                    memcpy(&new_app_info, &ota_write_data[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
                    ESP_LOGI(TAG, "🔍 新韌體版本: %s", new_app_info.version);
                    
                    esp_err_t validate_err = ota_validate_image_header(&new_app_info);
                    if (validate_err != ESP_OK) {
                        current_state = OTA_STATE_ERROR;
                        ota_stats.failed_updates++;
                        ota_stats.last_result = OTA_RESULT_VERIFY_ERROR;
                        break;
                    }
                    memcpy(&ota_ctx.new_app_info, &new_app_info, sizeof(esp_app_desc_t));
                    ota_ctx.image_header_was_checked = true;
                }
            }
            
            // 寫入韌體資料到 flash
            err = esp_ota_write(ota_ctx.update_handle, (const void *)ota_write_data, data_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "❌ esp_ota_write 失敗: %s", esp_err_to_name(err));
                current_state = OTA_STATE_ERROR;
                ota_stats.failed_updates++;
                ota_stats.last_result = OTA_RESULT_INSTALL_ERROR;
                break;
            }
            
            binary_file_downloaded += data_read;
            
            // 更新進度
            int progress = (binary_file_downloaded * 100) / ota_ctx.binary_file_length;
            current_progress = progress;
            
            if (progress % 10 == 0 && progress > 0) {
                char progress_msg[64];
                snprintf(progress_msg, sizeof(progress_msg), "下載進度: %d%%", progress);
                ota_update_progress(progress, OTA_STATE_DOWNLOADING, progress_msg);
            }
            
        } else if (data_read == 0) {
            ESP_LOGI(TAG, "✅ 韌體下載完成 (%d bytes)", binary_file_downloaded);
            break;
        }
    }
    
    free(ota_write_data);
    
    if (current_state != OTA_STATE_ERROR) {
        current_state = OTA_STATE_VERIFYING;
        ota_update_progress(95, OTA_STATE_VERIFYING, "驗證韌體完整性");
        
        // 結束 OTA 程序
        err = esp_ota_end(ota_ctx.update_handle);
        if (err != ESP_OK) {
            if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
                ESP_LOGE(TAG, "❌ 韌體驗證失敗");
                ota_stats.last_result = OTA_RESULT_VERIFY_ERROR;
            } else {
                ESP_LOGE(TAG, "❌ esp_ota_end 失敗: %s", esp_err_to_name(err));
                ota_stats.last_result = OTA_RESULT_INSTALL_ERROR;
            }
            current_state = OTA_STATE_ERROR;
            ota_stats.failed_updates++;
        } else {
            current_state = OTA_STATE_INSTALLING;
            ota_update_progress(98, OTA_STATE_INSTALLING, "安裝新韌體");
            
            // 設定新的啟動分區
            err = esp_ota_set_boot_partition(ota_ctx.update_partition);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "❌ esp_ota_set_boot_partition 失敗: %s", esp_err_to_name(err));
                current_state = OTA_STATE_ERROR;
                ota_stats.failed_updates++;
                ota_stats.last_result = OTA_RESULT_INSTALL_ERROR;
            } else {
                current_state = OTA_STATE_SUCCESS;
                ota_stats.successful_updates++;
                ota_stats.last_result = OTA_RESULT_SUCCESS;
                ota_stats.last_update_time = esp_timer_get_time() / 1000000;
                strncpy(ota_stats.last_version, ota_ctx.new_app_info.version, sizeof(ota_stats.last_version) - 1);
                
                ota_update_progress(100, OTA_STATE_SUCCESS, "更新完成！");
                ESP_LOGI(TAG, "✅ OTA 更新成功！準備重啟...");
                
                if (ota_ctx.config.auto_reboot) {
                    ota_send_mqtt_status("✅ OTA 更新成功！將在 3 秒後重啟...");
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    esp_restart();
                }
            }
        }
    }

ota_end:
    if (client) {
        esp_http_client_cleanup(client);
    }
    
    if (current_state == OTA_STATE_ERROR) {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), 
                "❌ OTA 更新失敗 (錯誤代碼: %d)", ota_stats.last_result);
        ota_send_mqtt_status(error_msg);
        ESP_LOGE(TAG, "❌ OTA 更新失敗");
    }
    
    // 清理任務句柄
    ota_task_handle = NULL;
    vTaskDelete(NULL);
}

// ============================================================================
// 驗證韌體映像標頭
// ============================================================================
static esp_err_t ota_validate_image_header(esp_app_desc_t *new_app_info)
{
    if (new_app_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    const esp_app_desc_t *running_app_info = esp_app_get_description();
    ESP_LOGI(TAG, "🔍 目前版本: %s", running_app_info->version);
    ESP_LOGI(TAG, "🔍 新版本: %s", new_app_info->version);
    
    // 檢查版本是否相同
    if (memcmp(new_app_info->version, running_app_info->version, sizeof(new_app_info->version)) == 0) {
        ESP_LOGW(TAG, "⚠️ 目前版本與新版本相同，跳過更新");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// ============================================================================
// 更新進度並呼叫回調函數
// ============================================================================
static void ota_update_progress(int percentage, ota_state_t state, const char* message)
{
    current_progress = percentage;
    current_state = state;
    
    if (progress_callback) {
        progress_callback(percentage, state, message);
    }
    
    if (message) {
        ESP_LOGI(TAG, "📊 %s (%d%%)", message, percentage);
    }
}

// ============================================================================
// 發送 MQTT 狀態消息
// ============================================================================
static void ota_send_mqtt_status(const char* message)
{
    esp_mqtt_client_handle_t client = get_mqtt_client();
    if (client && message) {
        esp_mqtt_client_publish(client, "soilsensorcapture/esp/ota_status", 
                               message, 0, 1, 0);
    }
}

// ============================================================================
// 公共函數實作
// ============================================================================

ota_state_t ota_get_state(void)
{
    return current_state;
}

esp_err_t ota_get_statistics(ota_statistics_t* stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(stats, &ota_stats, sizeof(ota_statistics_t));
    return ESP_OK;
}

esp_err_t ota_get_current_version(char* version_buffer, size_t buffer_size)
{
    if (version_buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    const esp_app_desc_t *app_desc = esp_app_get_description();
    strncpy(version_buffer, app_desc->version, buffer_size - 1);
    version_buffer[buffer_size - 1] = '\0';
    
    return ESP_OK;
}

bool ota_is_updating(void)
{
    return (current_state != OTA_STATE_IDLE && current_state != OTA_STATE_SUCCESS && current_state != OTA_STATE_ERROR);
}

int ota_get_progress(void)
{
    return current_progress;
}

esp_err_t ota_cancel_update(void)
{
    if (!ota_is_updating()) {
        return ESP_ERR_INVALID_STATE;
    }
    
    cancel_requested = true;
    ESP_LOGW(TAG, "⚠️ OTA 更新取消請求");
    return ESP_OK;
}

esp_err_t ota_reset_statistics(void)
{
    memset(&ota_stats, 0, sizeof(ota_statistics_t));
    ESP_LOGI(TAG, "🔄 OTA 統計資料已重置");
    return ESP_OK;
}

void ota_set_progress_callback(ota_progress_callback_t callback)
{
    progress_callback = callback;
}