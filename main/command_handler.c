// ============================================================================
// command_handler.c - MQTT 指令處理模組實作
// 功能：處理來自 MQTT 的指令，控制硬體和系統狀態
// ============================================================================

#include "command_handler.h"
#include "ota_update.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

// ============================================================================
// 外部變數引用 (來自 main.c)
// ============================================================================
// extern esp_mqtt_client_handle_t mqtt_client;  // MQTT 客戶端句柄
extern esp_mqtt_client_handle_t get_mqtt_client(void);

// ============================================================================
// 硬體定義 (與 main.c 保持一致)
// ============================================================================
#define PUMP_GPIO GPIO_NUM_6
#define LED_GPIO GPIO_NUM_8
#define TOPIC_RESPONSE "soilsensorcapture/response"

// ============================================================================
// 模組內部常數定義
// ============================================================================
#define COMMAND_QUEUE_SIZE 10           // 指令佇列大小
#define COMMAND_TASK_STACK_SIZE 3072    // 指令處理任務堆疊大小
#define COMMAND_TASK_PRIORITY 4         // 指令處理任務優先順序

// ============================================================================
// 日誌標籤
// ============================================================================
static const char *TAG = "CMD_HANDLER";

// ============================================================================
// 全域變數定義
// ============================================================================
QueueHandle_t command_queue = NULL;        // 指令佇列句柄
EventGroupHandle_t cmd_event_group = NULL; // 指令事件群組句柄

// ============================================================================
// 模組內部狀態變數
// ============================================================================
static bool pump_enabled = false;          // 泵浦狀態 (澆水時暫時為 true)
static uint32_t processed_count = 0;       // 已處理指令計數
static uint32_t error_count = 0;           // 錯誤指令計數
static uint32_t water_count = 0;           // 澆水次數統計

// ============================================================================
// 內部函數宣告
// ============================================================================
static void command_handler_task(void *pvParameters);
static esp_err_t send_mqtt_response(const char* message);

// ============================================================================
// 初始化指令處理模組
// ============================================================================
esp_err_t command_handler_init(void)
{
    ESP_LOGI(TAG, "初始化指令處理模組...");
    
    // 建立指令佇列
    command_queue = xQueueCreate(COMMAND_QUEUE_SIZE, sizeof(mqtt_command_t));
    if (command_queue == NULL) {
        ESP_LOGE(TAG, "無法建立指令佇列");
        return ESP_ERR_NO_MEM;
    }
    
    // 建立事件群組
    cmd_event_group = xEventGroupCreate();
    if (cmd_event_group == NULL) {
        ESP_LOGE(TAG, "無法建立事件群組");
        vQueueDelete(command_queue);
        return ESP_ERR_NO_MEM;
    }
    
    // 建立指令處理任務
    BaseType_t task_result = xTaskCreate(
        command_handler_task,       // 任務函數
        "cmd_handler",              // 任務名稱
        COMMAND_TASK_STACK_SIZE,    // 堆疊大小
        NULL,                       // 任務參數
        COMMAND_TASK_PRIORITY,      // 優先順序
        NULL                        // 任務句柄
    );
    
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "無法建立指令處理任務");
        vQueueDelete(command_queue);
        vEventGroupDelete(cmd_event_group);
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "✅ 指令處理模組初始化完成");
    return ESP_OK;
}

// ============================================================================
// 解析 MQTT 指令字串
// ============================================================================
command_type_t parse_command(const char* command_str, int cmd_len)
{
    if (command_str == NULL || cmd_len <= 0) {
        return CMD_UNKNOWN;
    }
    
    // 使用 strncmp 比較指令，避免緩衝區溢位
    if (strncmp(command_str, "澆水", cmd_len) == 0 || 
        strncmp(command_str, "WATER", cmd_len) == 0) {
        return CMD_WATER;
    } else if (strncmp(command_str, "GET_STATUS", cmd_len) == 0) {
        return CMD_GET_STATUS;
    } else if (strncmp(command_str, "GET_READING", cmd_len) == 0) {
        return CMD_GET_READING;
    } else if (strncmp(command_str, "OTA_UPDATE", cmd_len) == 0) {
        return CMD_OTA_UPDATE;
    } else if (strncmp(command_str, "OTA_STATUS", cmd_len) == 0) {
        return CMD_OTA_STATUS;
    } else if (strncmp(command_str, "OTA_CANCEL", cmd_len) == 0) {
        return CMD_OTA_CANCEL;
    }
    
    return CMD_UNKNOWN;
}

// ============================================================================
// 將指令加入處理佇列
// ============================================================================
esp_err_t enqueue_command(command_type_t cmd_type, const char* data)
{
    if (command_queue == NULL) {
        ESP_LOGE(TAG, "指令佇列未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    mqtt_command_t command = {
        .type = cmd_type,
        .timestamp = esp_timer_get_time() / 1000000  // 轉換為秒
    };
    
    // 複製資料 (如果有提供)
    if (data != NULL) {
        strncpy(command.data, data, sizeof(command.data) - 1);
        command.data[sizeof(command.data) - 1] = '\0';  // 確保字串結尾
    } else {
        command.data[0] = '\0';
    }
    
    // 嘗試將指令加入佇列 (非阻塞)
    BaseType_t result = xQueueSend(command_queue, &command, 0);
    
    if (result == pdPASS) {
        ESP_LOGI(TAG, "指令已加入佇列: 類型=%d", cmd_type);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "指令佇列已滿，無法加入指令");
        return ESP_ERR_TIMEOUT;
    }
}

// ============================================================================
// 執行澆水指令 - 自動開啟幫浦1.5秒後關閉
// ============================================================================
esp_err_t execute_water_command(void)
{
    ESP_LOGI(TAG, "🚿 執行澆水指令 - 開啟幫浦1.5秒");
    
    // 開啟幫浦
    gpio_set_level(PUMP_GPIO, 1);
    pump_enabled = true;
    
    // 開啟指示 LED (ESP32-C3 內建 LED 為反向邏輯)
    gpio_set_level(LED_GPIO, 0);
    
    // 發送開始澆水的 MQTT 回應
    esp_err_t result = send_mqtt_response("🚿 開始澆水 - 幫浦已啟動");
    
    // 延遲1.5秒 (1500毫秒)
    vTaskDelay(pdMS_TO_TICKS(1500));
    
    // 關閉幫浦
    gpio_set_level(PUMP_GPIO, 0);
    pump_enabled = false;
    
    // 關閉指示 LED
    gpio_set_level(LED_GPIO, 1);
    
    // 增加澆水次數統計
    water_count++;
    
    // 發送完成澆水的 MQTT 回應
    char completion_msg[128];
    snprintf(completion_msg, sizeof(completion_msg), 
             "✅ 澆水完成 - 幫浦已關閉 (總澆水次數: %lu)", water_count);
    result = send_mqtt_response(completion_msg);
    
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "✅ 澆水指令執行完成 - 幫浦運行1.5秒後已關閉 (總次數: %lu)", water_count);
    } else {
        ESP_LOGW(TAG, "澆水指令執行完成但回應發送失敗");
    }
    
    return ESP_OK;
}

// ============================================================================
// 執行狀態查詢指令
// ============================================================================
esp_err_t execute_status_command(void)
{
    ESP_LOGI(TAG, "執行狀態查詢指令");
    
    // 建立狀態回應訊息
    char status_msg[200];
    snprintf(status_msg, sizeof(status_msg), 
             "🌱 系統狀態: 運行中\n"
             "🚿 澆水次數: %lu\n"
             "📊 已處理指令: %lu\n"
             "❌ 錯誤指令: %lu\n"
             "💧 幫浦狀態: %s",
             water_count,
             processed_count,
             error_count,
             pump_enabled ? "運行中" : "待機中");
    
    esp_err_t result = send_mqtt_response(status_msg);
    
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "✅ 狀態查詢完成 - 澆水次數: %lu", water_count);
    } else {
        ESP_LOGW(TAG, "狀態查詢完成但回應發送失敗");
    }
    
    return ESP_OK;
}

// ============================================================================
// 執行讀數查詢指令
// ============================================================================
esp_err_t execute_reading_command(void)
{
    ESP_LOGI(TAG, "執行讀數查詢指令");
    
    // 這裡可以觸發即時感測器讀取
    // 或者簡單回應表示已更新
    const char* response_msg = "即時讀數已更新";
    esp_err_t result = send_mqtt_response(response_msg);
    
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "✅ 讀數查詢完成");
    } else {
        ESP_LOGW(TAG, "讀數查詢完成但回應發送失敗");
    }
    
    return ESP_OK;
}

// ============================================================================
// 取得目前泵浦狀態
// ============================================================================
bool get_pump_status(void)
{
    return pump_enabled;
}

// ============================================================================
// 設定泵浦狀態
// ============================================================================
void set_pump_status(bool enabled)
{
    pump_enabled = enabled;
}

// ============================================================================
// 取得指令處理統計資訊
// ============================================================================
void get_command_stats(uint32_t* processed_count_ptr, uint32_t* error_count_ptr)
{
    if (processed_count_ptr != NULL) {
        *processed_count_ptr = processed_count;
    }
    if (error_count_ptr != NULL) {
        *error_count_ptr = error_count;
    }
}

// ============================================================================
// 取得澆水次數統計
// ============================================================================
uint32_t get_water_count(void)
{
    return water_count;
}

// ============================================================================
// 執行 OTA 更新指令
// ============================================================================
esp_err_t execute_ota_update_command(const char* firmware_url)
{
    ESP_LOGI(TAG, "🚀 執行 OTA 更新指令");
    
    if (firmware_url == NULL || strlen(firmware_url) == 0) {
        ESP_LOGW(TAG, "⚠️ 韌體 URL 為空");
        esp_err_t result = send_mqtt_response("❌ 錯誤：韌體 URL 為空");
        return result == ESP_OK ? ESP_ERR_INVALID_ARG : result;
    }
    
    if (ota_is_updating()) {
        ESP_LOGW(TAG, "⚠️ OTA 更新已在進行中");
        esp_err_t result = send_mqtt_response("⚠️ OTA 更新已在進行中");
        return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    }
    
    // 設定 OTA 配置
    ota_config_t ota_config = {
        .auto_reboot = true,
        .timeout_ms = 30000,  // 30 秒超時
        .callback = NULL
    };
    
    // 複製 URL (確保不會超出緩衝區)
    strncpy(ota_config.firmware_url, firmware_url, sizeof(ota_config.firmware_url) - 1);
    ota_config.firmware_url[sizeof(ota_config.firmware_url) - 1] = '\0';
    
    // 取得目前版本作為參考
    ota_get_current_version(ota_config.version, sizeof(ota_config.version));
    
    esp_err_t result = ota_start_update(&ota_config);
    
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "✅ OTA 更新已啟動");
        char response_msg[200];
        snprintf(response_msg, sizeof(response_msg), 
                 "🚀 OTA 更新已啟動\nURL: %s", firmware_url);
        send_mqtt_response(response_msg);
    } else {
        ESP_LOGE(TAG, "❌ OTA 更新啟動失敗: %s", esp_err_to_name(result));
        send_mqtt_response("❌ OTA 更新啟動失敗");
    }
    
    return result;
}

// ============================================================================
// 執行 OTA 狀態查詢指令
// ============================================================================
esp_err_t execute_ota_status_command(void)
{
    ESP_LOGI(TAG, "📊 執行 OTA 狀態查詢指令");
    
    ota_state_t state = ota_get_state();
    int progress = ota_get_progress();
    ota_statistics_t stats;
    ota_get_statistics(&stats);
    
    char current_version[32];
    ota_get_current_version(current_version, sizeof(current_version));
    
    const char* state_names[] = {
        "待機中", "下載中", "驗證中", "安裝中", "更新完成", "更新錯誤"
    };
    
    char status_msg[300];
    snprintf(status_msg, sizeof(status_msg),
             "🔄 OTA 更新狀態報告\n"
             "📦 目前版本: %s\n"
             "📊 狀態: %s\n"
             "⏳ 進度: %d%%\n"
             "✅ 總更新次數: %lu\n"
             "🎯 成功次數: %lu\n"
             "❌ 失敗次數: %lu",
             current_version,
             state < sizeof(state_names)/sizeof(state_names[0]) ? state_names[state] : "未知",
             progress,
             stats.total_updates,
             stats.successful_updates,
             stats.failed_updates);
    
    esp_err_t result = send_mqtt_response(status_msg);
    
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "✅ OTA 狀態查詢完成");
    } else {
        ESP_LOGW(TAG, "OTA 狀態查詢完成但回應發送失敗");
    }
    
    return ESP_OK;
}

// ============================================================================
// 執行取消 OTA 更新指令
// ============================================================================
esp_err_t execute_ota_cancel_command(void)
{
    ESP_LOGI(TAG, "⛔ 執行取消 OTA 更新指令");
    
    if (!ota_is_updating()) {
        ESP_LOGW(TAG, "⚠️ 目前沒有進行中的 OTA 更新");
        esp_err_t result = send_mqtt_response("⚠️ 目前沒有進行中的 OTA 更新");
        return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    }
    
    esp_err_t result = ota_cancel_update();
    
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "✅ OTA 更新取消成功");
        send_mqtt_response("✅ OTA 更新已取消");
    } else {
        ESP_LOGE(TAG, "❌ OTA 更新取消失敗: %s", esp_err_to_name(result));
        send_mqtt_response("❌ OTA 更新取消失敗");
    }
    
    return result;
}

// ============================================================================
// 指令處理任務 (FreeRTOS 任務)
// ============================================================================
static void command_handler_task(void *pvParameters)
{
    mqtt_command_t command;
    
    ESP_LOGI(TAG, "🚀 指令處理任務已啟動");
    
    while (1) {
        // 等待佇列中的指令 (最多等待 1 秒)
        BaseType_t result = xQueueReceive(command_queue, &command, pdMS_TO_TICKS(1000));
        
        if (result == pdPASS) {
            ESP_LOGI(TAG, "🔄 處理指令: 類型=%d, 時間戳=%lu", 
                     command.type, command.timestamp);
            
            esp_err_t exec_result = ESP_OK;
            
            // 根據指令類型執行對應動作
            switch (command.type) {
                case CMD_WATER:
                    exec_result = execute_water_command();
                    break;
                    
                case CMD_GET_STATUS:
                    exec_result = execute_status_command();
                    break;
                    
                case CMD_GET_READING:
                    exec_result = execute_reading_command();
                    break;
                    
                case CMD_OTA_UPDATE:
                    exec_result = execute_ota_update_command(command.data);
                    break;
                    
                case CMD_OTA_STATUS:
                    exec_result = execute_ota_status_command();
                    break;
                    
                case CMD_OTA_CANCEL:
                    exec_result = execute_ota_cancel_command();
                    break;
                    
                case CMD_UNKNOWN:
                default:
                    ESP_LOGW(TAG, "⚠️ 未知指令類型: %d", command.type);
                    exec_result = ESP_ERR_INVALID_ARG;
                    break;
            }
            
            // 更新統計計數
            if (exec_result == ESP_OK) {
                processed_count++;
                xEventGroupSetBits(cmd_event_group, CMD_PROCESSED_BIT);
            } else {
                error_count++;
                xEventGroupSetBits(cmd_event_group, CMD_ERROR_BIT);
            }
            
        } else {
            // 沒有收到指令，繼續等待
            // 這裡可以執行一些定期維護工作
        }
        
        // 清除事件位元 (為下次設定做準備)
        xEventGroupClearBits(cmd_event_group, CMD_PROCESSED_BIT | CMD_ERROR_BIT);
    }
}

// ============================================================================
// 發送 MQTT 回應 (內部函數)
// ============================================================================
static esp_err_t send_mqtt_response(const char* message)
{
    if (message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_mqtt_client_handle_t client = get_mqtt_client();
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int msg_id = esp_mqtt_client_publish(client, TOPIC_RESPONSE, message, 0, 1, 0);
    
    if (msg_id >= 0) {
        ESP_LOGD(TAG, "MQTT 回應已發送: %s (msg_id=%d)", message, msg_id);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "MQTT 回應發送失敗: %s", message);
        return ESP_ERR_INVALID_RESPONSE;
    }
}