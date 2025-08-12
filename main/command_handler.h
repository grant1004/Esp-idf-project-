// ============================================================================
// command_handler.h - MQTT 指令處理模組標頭檔
// 功能：定義指令類型、結構體和函數原型
// ============================================================================

#ifndef COMMAND_HANDLER_H
#define COMMAND_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

// ============================================================================
// 指令類型定義
// ============================================================================
typedef enum {
    CMD_WATER,          // 澆水指令 (自動開啟幫浦1.5秒)
    CMD_GET_STATUS,     // 取得系統狀態
    CMD_GET_READING,    // 取得即時讀數
    CMD_OTA_UPDATE,     // OTA 韌體更新指令
    CMD_OTA_STATUS,     // 取得 OTA 狀態
    CMD_OTA_CANCEL,     // 取消 OTA 更新
    CMD_UNKNOWN         // 未知指令
} command_type_t;

// ============================================================================
// 指令結構體定義
// ============================================================================
typedef struct {
    command_type_t type;        // 指令類型
    char data[64];              // 指令資料 (如有需要)
    uint32_t timestamp;         // 接收時間戳
} mqtt_command_t;

// ============================================================================
// 事件群組位元定義 - 用於狀態通知
// ============================================================================
#define CMD_PROCESSED_BIT   BIT0    // 指令處理完成
#define CMD_ERROR_BIT       BIT1    // 指令處理錯誤

// ============================================================================
// 全域變數宣告 (在 command_handler.c 中定義)
// ============================================================================
extern QueueHandle_t command_queue;        // 指令佇列句柄
extern EventGroupHandle_t cmd_event_group; // 指令事件群組句柄

// ============================================================================
// 函數原型宣告
// ============================================================================

/**
 * @brief 初始化指令處理模組
 * 
 * 建立指令佇列、事件群組和處理任務
 * 
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t command_handler_init(void);

/**
 * @brief 解析 MQTT 指令字串
 * 
 * @param command_str 指令字串
 * @param cmd_len 指令長度
 * @return command_type_t 解析後的指令類型
 */
command_type_t parse_command(const char* command_str, int cmd_len);

/**
 * @brief 將指令加入處理佇列
 * 
 * @param cmd_type 指令類型
 * @param data 指令資料 (可為 NULL)
 * @return esp_err_t ESP_OK 表示成功加入佇列
 */
esp_err_t enqueue_command(command_type_t cmd_type, const char* data);

/**
 * @brief 執行澆水指令
 * 
 * 開啟幫浦1.5秒後自動關閉
 * 
 * @return esp_err_t ESP_OK 表示執行成功
 */
esp_err_t execute_water_command(void);

/**
 * @brief 執行狀態查詢指令
 * 
 * @return esp_err_t ESP_OK 表示執行成功
 */
esp_err_t execute_status_command(void);

/**
 * @brief 執行讀數查詢指令
 * 
 * @return esp_err_t ESP_OK 表示執行成功
 */
esp_err_t execute_reading_command(void);

/**
 * @brief 取得目前泵浦狀態
 * 
 * @return bool true=開啟, false=關閉
 */
bool get_pump_status(void);

/**
 * @brief 設定泵浦狀態
 * 
 * @param enabled 泵浦狀態
 */
void set_pump_status(bool enabled);

/**
 * @brief 取得指令處理統計資訊
 * 
 * @param processed_count 已處理指令數量指標
 * @param error_count 錯誤指令數量指標
 */
void get_command_stats(uint32_t* processed_count, uint32_t* error_count);

/**
 * @brief 取得澆水次數統計
 * 
 * @return uint32_t 總澆水次數
 */
uint32_t get_water_count(void);

/**
 * @brief 執行 OTA 更新指令
 * 
 * @param firmware_url 韌體下載 URL
 * @return esp_err_t ESP_OK 表示執行成功
 */
esp_err_t execute_ota_update_command(const char* firmware_url);

/**
 * @brief 執行 OTA 狀態查詢指令
 * 
 * @return esp_err_t ESP_OK 表示執行成功
 */
esp_err_t execute_ota_status_command(void);

/**
 * @brief 執行取消 OTA 更新指令
 * 
 * @return esp_err_t ESP_OK 表示執行成功
 */
esp_err_t execute_ota_cancel_command(void);


esp_mqtt_client_handle_t get_mqtt_client(void);

#endif // COMMAND_HANDLER_H