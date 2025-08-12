// ============================================================================
// ota_update.h - OTA (Over The Air) 遠端韌體更新模組頭檔
// 功能：透過 WiFi 進行韌體遠端更新
// ============================================================================

#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"

// ============================================================================
// OTA 更新狀態定義
// ============================================================================
typedef enum {
    OTA_STATE_IDLE,         // 閒置狀態
    OTA_STATE_DOWNLOADING,  // 正在下載韌體
    OTA_STATE_VERIFYING,    // 正在驗證韌體
    OTA_STATE_INSTALLING,   // 正在安裝韌體
    OTA_STATE_SUCCESS,      // 更新成功
    OTA_STATE_ERROR         // 更新錯誤
} ota_state_t;

// ============================================================================
// OTA 更新結果定義
// ============================================================================
typedef enum {
    OTA_RESULT_SUCCESS = 0,     // 更新成功
    OTA_RESULT_URL_ERROR,       // URL 錯誤
    OTA_RESULT_DOWNLOAD_ERROR,  // 下載錯誤
    OTA_RESULT_VERIFY_ERROR,    // 驗證錯誤
    OTA_RESULT_INSTALL_ERROR,   // 安裝錯誤
    OTA_RESULT_MEMORY_ERROR,    // 記憶體錯誤
    OTA_RESULT_NETWORK_ERROR    // 網路錯誤
} ota_result_t;

// ============================================================================
// OTA 進度回調函數類型定義
// ============================================================================
typedef void (*ota_progress_callback_t)(int percentage, ota_state_t state, const char* message);

// ============================================================================
// OTA 配置結構
// ============================================================================
typedef struct {
    char firmware_url[256];         // 韌體下載 URL
    char version[32];               // 目標版本號
    bool auto_reboot;               // 更新完成後是否自動重啟
    uint32_t timeout_ms;            // 下載超時時間 (毫秒)
    ota_progress_callback_t callback; // 進度回調函數
} ota_config_t;

// ============================================================================
// OTA 統計資訊
// ============================================================================
typedef struct {
    uint32_t total_updates;         // 總更新次數
    uint32_t successful_updates;    // 成功更新次數
    uint32_t failed_updates;        // 失敗更新次數
    uint32_t last_update_time;      // 上次更新時間戳
    char last_version[32];          // 上次更新版本
    ota_result_t last_result;       // 上次更新結果
} ota_statistics_t;

// ============================================================================
// 函數原型宣告
// ============================================================================

/**
 * @brief 初始化 OTA 更新模組
 * 
 * @return esp_err_t ESP_OK 表示初始化成功
 */
esp_err_t ota_update_init(void);

/**
 * @brief 啟動 OTA 韌體更新
 * 
 * @param config OTA 配置結構指標
 * @return esp_err_t ESP_OK 表示啟動成功
 */
esp_err_t ota_start_update(const ota_config_t* config);

/**
 * @brief 取得目前 OTA 狀態
 * 
 * @return ota_state_t 目前的 OTA 狀態
 */
ota_state_t ota_get_state(void);

/**
 * @brief 取得 OTA 統計資訊
 * 
 * @param stats 統計資訊結構指標
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t ota_get_statistics(ota_statistics_t* stats);

/**
 * @brief 取得目前韌體版本
 * 
 * @param version_buffer 版本字串緩衝區
 * @param buffer_size 緩衝區大小
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t ota_get_current_version(char* version_buffer, size_t buffer_size);

/**
 * @brief 檢查是否有 OTA 更新正在進行
 * 
 * @return bool true 表示正在更新
 */
bool ota_is_updating(void);

/**
 * @brief 取得 OTA 更新進度百分比
 * 
 * @return int 進度百分比 (0-100)
 */
int ota_get_progress(void);

/**
 * @brief 取消正在進行的 OTA 更新
 * 
 * @return esp_err_t ESP_OK 表示取消成功
 */
esp_err_t ota_cancel_update(void);

/**
 * @brief 重置 OTA 統計資訊
 * 
 * @return esp_err_t ESP_OK 表示重置成功
 */
esp_err_t ota_reset_statistics(void);

/**
 * @brief 設定 OTA 進度回調函數
 * 
 * @param callback 回調函數指標
 */
void ota_set_progress_callback(ota_progress_callback_t callback);

#endif // OTA_UPDATE_H