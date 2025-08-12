// GRANT WANG 7/30/2025 
// ============================================================================
// ESP32-C3 土壤濕度監測系統 - 完整註解版
// 功能：透過ADC讀取土壤濕度，透過WiFi/MQTT發送資料，遠端控制泵浦
// ============================================================================

// ============================================================================
// 標準 C 函式庫引入區
// ============================================================================
#include <stdio.h>      // 標準輸入輸出函式庫，提供 printf, sprintf 等函數
#include <string.h>     // 字串處理函式庫，提供 strcmp, strlen, strncmp 等函數
#include <stdlib.h>     // 標準函式庫，提供 malloc, free, atoi 等函數

// ============================================================================
// FreeRTOS 即時作業系統相關函式庫
// ============================================================================
#include "freertos/FreeRTOS.h"     // FreeRTOS 核心函式庫，提供即時作業系統基礎功能
#include "freertos/task.h"         // 任務管理函式庫，提供 xTaskCreate, vTaskDelay 等函數
#include "freertos/event_groups.h" // 事件群組函式庫，用於同步多個任務，提供事件標誌位操作

// ============================================================================
// ESP32 系統相關函式庫
// ============================================================================
#include "esp_wifi.h"    // WiFi 函式庫，提供 WiFi 連接、配置、事件處理功能
#include "esp_event.h"   // 事件處理函式庫，提供系統事件循環機制
#include "esp_log.h"     // 日誌系統函式庫，提供 ESP_LOGI, ESP_LOGE 等日誌函數
#include "esp_system.h"  // 系統函式庫，提供系統資訊、重啟等功能
#include "esp_timer.h"   // 高精度計時器函式庫，提供微秒級時間戳
#include "nvs_flash.h"   // 非揮發性儲存函式庫，用於儲存 WiFi 配置等持久資料

// ============================================================================
// 硬體驅動相關函式庫
// ============================================================================
#include "driver/gpio.h"           // GPIO 驅動函式庫，提供數位輸入輸出控制
#include "esp_adc/adc_oneshot.h"   // ADC 單次採樣函式庫 (ESP-IDF v5.0+ 新API)
#include "esp_adc/adc_cali.h"      // ADC 校準函式庫，用於電壓轉換的精度校正
#include "esp_adc/adc_cali_scheme.h" // ADC 校準方案函式庫，提供不同的校準演算法

// ============================================================================
// 網路通訊相關函式庫
// ============================================================================
#include "mqtt_client.h" // MQTT 客戶端函式庫，提供 MQTT 協定實作
#include "cJSON.h"       // JSON 處理函式庫，用於建立和解析 JSON 格式資料
#include "esp_netif.h"   // 網路介面函式庫，提供網路配置功能
#include "lwip/inet.h"   // LwIP 網路函式庫，提供 IP 位址轉換
#include "lwip/netdb.h"  // 網路資料庫函式庫，提供 gethostbyname 等函數
#include "lwip/sockets.h" // Socket 函式庫，提供網路通訊功能


// ============================================================================
// 自定義模組函式庫
// ============================================================================
#include "command_handler.h"  // 指令處理模組
#include "ota_update.h"       // OTA 韌體更新模組

// ============================================================================
// WiFi 連接設定區 - 使用者需要修改的部分
// ============================================================================
#define WIFI_SSID "Grant"       // WiFi 網路名稱 (SSID)，需要修改為你的 WiFi 名稱
#define WIFI_PASS "grant891004" // WiFi 密碼，需要修改為你的 WiFi 密碼

// ============================================================================
// MQTT 伺服器設定區 - 與樹莓派版本保持一致的通訊協定
// ============================================================================
#define BROKER_HOST "test.mosquitto.org" // MQTT Broker 主機名稱 (Eclipse Mosquitto 公共測試服務)
#define BROKER_PORT 1883                // MQTT 標準埠號 (非加密連接)
#define CLIENT_ID "soilsensorcapture_esp32c3" // MQTT 客戶端 ID，必須唯一
#define MQTT_BROKER "mqtt://test.mosquitto.org:1883" // 完整的 MQTT 連接 URI

// ============================================================================
// MQTT Topic 定義區 - 訊息主題設計，與樹莓派版本互相兼容
// ============================================================================
#define TOPIC_DATA "soilsensorcapture/esp/data"      // 感測器資料發布主題
#define TOPIC_COMMAND "soilsensorcapture/esp/command" // 接收遠端指令主題
#define TOPIC_STATUS "soilsensorcapture/esp/status"   // 系統狀態發布主題
#define TOPIC_RESPONSE "soilsensorcapture/esp/response" // 指令回應發布主題

// ============================================================================
// 硬體腳位定義區 - ESP32-C3 Super Mini 專用設定
// ============================================================================
#define SOIL_SENSOR_ADC_CHANNEL ADC_CHANNEL_0 // 土壤感測器 ADC 通道 (對應 GPIO0)
#define PUMP_GPIO GPIO_NUM_6                  // 泵浦控制腳位 (GPIO6)
#define LED_GPIO GPIO_NUM_8                   // 內建 LED 腳位 (GPIO8，反向邏輯)

// ============================================================================
// 感測器校準參數區 - 根據實際測試調整
// ============================================================================
#define AIR_VALUE 3000      // 感測器在乾燥空氣中的 ADC 讀值 (12-bit ADC: 0-4095)
#define WATER_VALUE 1400   // 感測器完全浸在水中的 ADC 讀值
#define SAMPLE_COUNT 10   // 每次讀取的採樣次數，用於平均化以提高精度

// ============================================================================
// 日誌系統設定
// ============================================================================
static const char *TAG = "SOIL_SENSOR"; // 日誌標籤，用於識別此模組的日誌輸出

// ============================================================================
// FreeRTOS 事件群組 - 用於任務間同步
// ============================================================================
static EventGroupHandle_t s_wifi_event_group; // WiFi 事件群組句柄
#define WIFI_CONNECTED_BIT BIT0                // WiFi 連接成功事件位元 (第0位)

// ============================================================================
// 全域變數區 - 系統狀態和硬體句柄
// ============================================================================
static esp_mqtt_client_handle_t mqtt_client;   // MQTT 客戶端句柄
static adc_oneshot_unit_handle_t adc1_handle;  // ADC1 單元句柄 (ESP-IDF v5.0+ 新API)
static adc_cali_handle_t adc1_cali_handle = NULL; // ADC 校準句柄，用於電壓轉換
// static bool pump_enabled = false;              // 泵浦開關狀態 (false=關閉, true=開啟)
static int data_counter = 0;                   // 資料發送計數器，用於統計

// ============================================================================
// WiFi 事件處理函數
// 功能：處理 WiFi 連接、斷線、取得 IP 等事件
// 參數：arg - 使用者參數, event_base - 事件類型, event_id - 事件 ID, event_data - 事件資料
// ============================================================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data)
{
    // 檢查是否為 WiFi 事件且為啟動事件
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // 呼叫 esp_wifi_connect() 開始連接 WiFi (來自 esp_wifi.h)
        esp_wifi_connect();
    } 
    // 檢查是否為 WiFi 斷線事件
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // 取得斷線原因
        wifi_event_sta_disconnected_t* disconnected_event = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG, "⚠️ WiFi 斷線 (原因碼: %d)，重新連接中...", disconnected_event->reason);
        
        // 自動重新連接 WiFi
        esp_wifi_connect();
        
        // 清除 WiFi 連接事件位元 (來自 freertos/event_groups.h)
        // 參數：事件群組句柄, 要清除的位元
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } 
    // 檢查是否為取得 IP 事件
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // 將 event_data 轉型為 IP 事件結構指標 (來自 esp_event.h)
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        // 輸出完整的網路配置資訊
        ESP_LOGI(TAG, "✅ WiFi 連接成功！");
        ESP_LOGI(TAG, "📍 IP位址: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "🌐 子網遮罩: " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "🚪 預設閘道: " IPSTR, IP2STR(&event->ip_info.gw));
        
        // 檢查DNS設定
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_dns_info_t dns_info;
            if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
                ESP_LOGI(TAG, "🔍 主DNS: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
            }
        }
        
        // 設定 WiFi 連接成功事件位元 (來自 freertos/event_groups.h)
        // 參數：事件群組句柄, 要設定的位元
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        // 執行網路診斷
        network_diagnostics();
    }
}

// ============================================================================
// 網路診斷函數
// 功能：測試DNS解析和基本連通性
// ============================================================================
static void network_diagnostics(void)
{
    ESP_LOGI(TAG, "🔧 開始網路診斷...");
    
    // 測試DNS解析
    struct hostent *he = gethostbyname("test.mosquitto.org");
    if (he != NULL) {
        struct in_addr **addr_list = (struct in_addr **)he->h_addr_list;
        if (addr_list[0] != NULL) {
            ESP_LOGI(TAG, "✅ DNS解析成功: test.mosquitto.org -> %s", 
                     inet_ntoa(*addr_list[0]));
        }
    } else {
        ESP_LOGE(TAG, "❌ DNS解析失敗: test.mosquitto.org");
        return;
    }
    
    // 測試Google DNS
    he = gethostbyname("google.com");
    if (he != NULL) {
        struct in_addr **addr_list = (struct in_addr **)he->h_addr_list;
        if (addr_list[0] != NULL) {
            ESP_LOGI(TAG, "✅ Google DNS測試成功: google.com -> %s", 
                     inet_ntoa(*addr_list[0]));
        }
    } else {
        ESP_LOGE(TAG, "❌ Google DNS測試失敗");
    }
    
    ESP_LOGI(TAG, "🔧 網路診斷完成");
}

// ============================================================================
// MQTT 事件處理函數
// 功能：處理 MQTT 連接、斷線、接收訊息等事件
// 參數：handler_args - 使用者參數, base - 事件基底, event_id - 事件 ID, event_data - 事件資料
// ============================================================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "✅ MQTT 已連接到 %s", BROKER_HOST);
        esp_mqtt_client_subscribe(client, TOPIC_COMMAND, 1);
        ESP_LOGI(TAG, "📝 已訂閱指令主題: %s", TOPIC_COMMAND);
        break;
        
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "⚠️ MQTT 斷線，將自動重連...");
        break;
        
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "❌ MQTT 錯誤: error_type=%d", event->error_handle->error_type);
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "TCP 傳輸錯誤: 0x%x", event->error_handle->esp_tls_last_esp_err);
        }
        break;
        
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "收到 MQTT 指令: %.*s", event->data_len, event->data);
        
        // 🔄 新的處理方式：使用指令處理模組
        // 解析指令類型
        command_type_t cmd_type = parse_command(event->data, event->data_len);
        
        if (cmd_type != CMD_UNKNOWN) {
            // 將指令加入處理佇列
            esp_err_t result = enqueue_command(cmd_type, NULL);
            
            if (result == ESP_OK) {
                ESP_LOGI(TAG, "✅ 指令已加入處理佇列");
            } else {
                ESP_LOGW(TAG, "⚠️ 指令佇列忙碌，請稍後重試");
                // 可以選擇發送錯誤回應
                esp_mqtt_client_publish(client, TOPIC_RESPONSE, 
                                      "系統忙碌，請稍後重試", 0, 1, 0);
            }
        } else {
            ESP_LOGW(TAG, "⚠️ 未知的 MQTT 指令");
            esp_mqtt_client_publish(client, TOPIC_RESPONSE, 
                                  "未知指令", 0, 1, 0);
        }
        break;
        
    default:
        break;
    }
}


// ============================================================================
// WiFi 初始化函數 (Station 模式)
// 功能：設定 WiFi 為 Station 模式，連接到指定的 WiFi 網路
// 無參數，無返回值
// ============================================================================
static void wifi_init_sta(void)
{
    // 建立事件群組 (來自 freertos/event_groups.h)
    // 返回值：事件群組句柄，用於任務間同步
    s_wifi_event_group = xEventGroupCreate();
    
    // 初始化網路介面 (來自 esp_netif.h，透過 esp_wifi.h 間接引入)
    // 必須在使用任何網路功能前呼叫，返回 ESP_OK 表示成功
    ESP_ERROR_CHECK(esp_netif_init());
    
    // 建立預設事件循環 (來自 esp_event.h)
    // 系統事件處理的核心，必須在事件註冊前呼叫
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // 建立預設的 WiFi Station 網路介面 (來自 esp_wifi.h)
    // 返回網路介面句柄，用於後續網路操作
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    
    // 暫時移除自定義DNS設定，使用DHCP提供的DNS
    // 讓路由器的DNS設定決定DNS伺服器

    // WiFi 初始化配置結構，使用預設值 (來自 esp_wifi.h)
    // WIFI_INIT_CONFIG_DEFAULT() 是一個巨集，提供標準的初始化參數
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // 初始化 WiFi 驅動，分配記憶體和資源
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 註冊 WiFi 事件處理器 (來自 esp_event.h)
    esp_event_handler_instance_t instance_any_id;   // 事件處理器實例句柄
    esp_event_handler_instance_t instance_got_ip;   // IP 事件處理器實例句柄
    
    // 註冊處理所有 WiFi 事件的處理器
    // 參數：事件基底, 事件ID(ESP_EVENT_ANY_ID表示所有), 處理函數, 使用者參數, 實例句柄
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    // 註冊處理取得 IP 事件的處理器
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    // WiFi 配置結構 (來自 esp_wifi.h)
    wifi_config_t wifi_config = {
        .sta = {                                    // Station 模式配置
            .ssid = WIFI_SSID,                     // 網路名稱
            .password = WIFI_PASS,                 // 網路密碼
            .threshold.authmode = WIFI_AUTH_WPA2_PSK, // 認證模式：WPA2-PSK
        },
    };
    
    // 設定 WiFi 為 Station 模式 (來自 esp_wifi.h)
    // WIFI_MODE_STA 表示客戶端模式，連接到其他 WiFi 網路
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    // 設定 WiFi 配置 (來自 esp_wifi.h)
    // 參數：介面類型(WIFI_IF_STA), 配置結構指標
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // 啟動 WiFi 驅動 (來自 esp_wifi.h)
    // 此時會觸發 WIFI_EVENT_STA_START 事件
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi 初始化完成");
}

// ============================================================================
// MQTT 初始化函數
// 功能：設定 MQTT 客戶端配置，註冊事件處理器，啟動 MQTT 客戶端
// 無參數，無返回值
// ============================================================================
static void mqtt_init(void)
{
    // MQTT 客戶端配置結構 (來自 mqtt_client.h)
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {                               // Broker 相關配置
            .address.uri = MQTT_BROKER,          // Broker URI (包含協定、主機、埠號)
        },
        .credentials = {                         // 認證相關配置
            .client_id = CLIENT_ID,              // 客戶端 ID，必須在 Broker 中唯一
        },
        .network = {                             // 網路相關配置
            .timeout_ms = 30000,                 // 連接超時時間 30 秒
            .refresh_connection_after_ms = 300000, // 5 分鐘後刷新連接
            .reconnect_timeout_ms = 10000,       // 重連間隔 10 秒
        },
        .session = {                             // 會話相關配置
            .keepalive = 60,                     // 心跳間隔 60 秒
            .disable_clean_session = false,      // 啟用清潔會話
        }
    };
    
    // 初始化 MQTT 客戶端 (來自 mqtt_client.h)
    // 參數：配置結構指標
    // 返回值：MQTT 客戶端句柄，用於後續所有 MQTT 操作
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    
    // 註冊 MQTT 事件處理器 (來自 mqtt_client.h)
    // 參數：客戶端句柄, 事件ID(ESP_EVENT_ANY_ID表示所有), 處理函數, 使用者參數
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    
    // 啟動 MQTT 客戶端 (來自 mqtt_client.h)
    // 開始連接到 MQTT Broker
    esp_mqtt_client_start(mqtt_client);
    
    ESP_LOGI(TAG, "MQTT 初始化完成");
}

esp_mqtt_client_handle_t get_mqtt_client(void)
{
    return mqtt_client;
}
// ============================================================================
// ADC 初始化函數
// 功能：設定 ADC1 單元，配置通道，初始化校準
// 無參數，無返回值
// 注意：使用 ESP-IDF v5.0+ 的新 ADC API
// ============================================================================
static void adc_init(void)
{
    // ADC1 單元初始化配置 (來自 esp_adc/adc_oneshot.h)
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,              // 指定 ADC1 單元
        .ulp_mode = ADC_ULP_MODE_DISABLE,   // 停用超低功耗模式
    };
    // 建立新的 ADC 單元 (來自 esp_adc/adc_oneshot.h)
    // 參數：初始化配置, ADC 句柄指標
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
    
    // ADC 通道配置 (來自 esp_adc/adc_oneshot.h)
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,    // 12位元解析度 (0-4095)
        .atten = ADC_ATTEN_DB_12,       // 11dB 衰減，測量範圍 0-3.3V
    };
    // 配置 ADC 通道 (來自 esp_adc/adc_oneshot.h)
    // 參數：ADC 句柄, 通道編號, 通道配置
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, SOIL_SENSOR_ADC_CHANNEL, &config));
    
    // ADC 校準初始化 - 提高電壓轉換精度
    // 檢查是否支援曲線擬合校準 (來自 esp_adc/adc_cali_scheme.h)
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    // 曲線擬合校準配置
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,                    // ADC 單元
        .chan = SOIL_SENSOR_ADC_CHANNEL,          // ADC 通道
        .atten = ADC_ATTEN_DB_12,                 // 衰減設定
        .bitwidth = ADC_BITWIDTH_12,              // 位元寬度
    };
    // 建立曲線擬合校準方案 (來自 esp_adc/adc_cali_scheme.h)
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_config, &adc1_cali_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC 校準方案：Curve Fitting");
    }
#endif

    // 如果曲線擬合不支援，嘗試線性擬合校準
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!adc1_cali_handle) {  // 如果還沒有校準句柄
        // 線性擬合校準配置
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT_1,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        // 建立線性擬合校準方案 (來自 esp_adc/adc_cali_scheme.h)
        esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_config, &adc1_cali_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "ADC 校準方案：Line Fitting");
        }
    }
#endif
    
    ESP_LOGI(TAG, "ADC 初始化完成");
}

// ============================================================================
// 土壤濕度讀取函數
// 功能：執行多次 ADC 採樣，計算平均值，轉換為電壓和濕度百分比
// 參數：raw_adc - 原始 ADC 值指標, voltage - 電壓值指標, moisture - 濕度百分比指標
// 無返回值，結果透過指標參數返回
// ============================================================================
static void read_soil_moisture(int *raw_adc, float *voltage, float *moisture)
{
    // 多次採樣累加器
    uint32_t adc_sum = 0;
    
    // 執行多次採樣以提高精度
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        int raw_value;  // 單次 ADC 讀值
        
        // 執行 ADC 單次讀取 (來自 esp_adc/adc_oneshot.h)
        // 參數：ADC 句柄, 通道, 讀值指標
        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, SOIL_SENSOR_ADC_CHANNEL, &raw_value));
        
        adc_sum += raw_value;  // 累加讀值
        
        // 短暫延遲以允許 ADC 穩定 (來自 freertos/task.h)
        // pdMS_TO_TICKS(10) 將 10 毫秒轉換為系統時鐘滴答數
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // 計算平均值
    *raw_adc = adc_sum / SAMPLE_COUNT;
    
    // 將 ADC 原始值轉換為電壓
    int voltage_mv;  // 電壓(毫伏)
    
    if (adc1_cali_handle) {  // 如果有校準句柄
        // 使用校準函數轉換 (來自 esp_adc/adc_cali.h)
        // 參數：校準句柄, 原始值, 電壓指標(mV)
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_handle, *raw_adc, &voltage_mv));
        *voltage = voltage_mv / 1000.0;  // 轉換為伏特
    } else {
        // 如果沒有校準，使用線性近似計算
        // ADC 12位元: 0-4095 對應 0-3.3V
        *voltage = (*raw_adc * 3.3) / 4095.0;
    }
    
    // 計算濕度百分比
    // 公式：(乾燥值 - 目前值) / (乾燥值 - 濕潤值) * 100
    *moisture = (float)(AIR_VALUE - *raw_adc) * 100.0 / (AIR_VALUE - WATER_VALUE);
    
    // 限制濕度值在 0-100% 範圍內
    if (*moisture > 100.0) *moisture = 100.0;
    if (*moisture < 0.0) *moisture = 0.0;
}

// ============================================================================
// 發送感測器資料函數
// 功能：讀取感測器，建立 JSON 格式資料，透過 MQTT 發送
// 無參數，無返回值
// JSON 格式與樹莓派版本保持一致以確保相容性
// ============================================================================
static void send_sensor_data(void)
{
    int raw_adc;
    float voltage;
    float moisture;
    
    read_soil_moisture(&raw_adc, &voltage, &moisture);    
    // printf("⚡ REALTIME: ADC=%d, 濕度=%.1f%%\n", raw_adc, moisture);
    cJSON *json = cJSON_CreateObject();
    
    cJSON *timestamp = cJSON_CreateNumber(esp_timer_get_time() / 1000000);
    cJSON *v = cJSON_CreateNumber(voltage);
    cJSON *m = cJSON_CreateNumber(moisture);
    cJSON *adc = cJSON_CreateNumber(raw_adc);
    
    // 🔄 修改：使用模組化的狀態取得方式
    bool current_pump_status = get_pump_status();
    cJSON *gpio_status = cJSON_CreateBool(current_pump_status);
    cJSON *type = cJSON_CreateString("soil_data");
    
    cJSON_AddItemToObject(json, "timestamp", timestamp);
    cJSON_AddItemToObject(json, "voltage", v);
    cJSON_AddItemToObject(json, "moisture", m);
    cJSON_AddItemToObject(json, "raw_adc", adc);
    cJSON_AddItemToObject(json, "gpio_status", gpio_status);
    cJSON_AddItemToObject(json, "type", type);
    
    char *json_string = cJSON_Print(json);
    
    if (json_string) {
        esp_mqtt_client_publish(mqtt_client, TOPIC_DATA, json_string, 0, 1, 0);
        
        data_counter++;
        
        ESP_LOGI(TAG, "[%d] ADC:%d 電壓:%.3fV 濕度:%.1f%% GPIO:%s", 
                data_counter, raw_adc, voltage, moisture, 
                current_pump_status ? "ON" : "OFF");
        
        free(json_string);
    }
    
    cJSON_Delete(json);
}

// ============================================================================
// 發送系統狀態函數
// 功能：建立系統狀態 JSON，透過 MQTT 發送系統資訊
// 無參數，無返回值
// JSON 格式與樹莓派版本保持一致
// ============================================================================
static void send_system_status(void)
{
    cJSON *json = cJSON_CreateObject();
    
    cJSON *timestamp = cJSON_CreateNumber(esp_timer_get_time() / 1000000);
    cJSON *system = cJSON_CreateString("online");
    cJSON *uptime = cJSON_CreateNumber(esp_timer_get_time() / 1000000);
    cJSON *free_heap = cJSON_CreateNumber(esp_get_free_heap_size());
    
    // 🔄 新增：指令處理統計資訊
    uint32_t processed_cmds, error_cmds;
    get_command_stats(&processed_cmds, &error_cmds);
    uint32_t watering_count = get_water_count();
    
    // 🔄 新增：OTA 統計資訊
    ota_statistics_t ota_stats;
    ota_get_statistics(&ota_stats);
    char current_version[32];
    ota_get_current_version(current_version, sizeof(current_version));
    
    cJSON *gpio_status = cJSON_CreateBool(get_pump_status());
    cJSON *cmd_processed = cJSON_CreateNumber(processed_cmds);
    cJSON *cmd_errors = cJSON_CreateNumber(error_cmds);
    cJSON *water_count_json = cJSON_CreateNumber(watering_count);
    cJSON *firmware_version = cJSON_CreateString(current_version);
    cJSON *ota_updates = cJSON_CreateNumber(ota_stats.total_updates);
    cJSON *ota_success = cJSON_CreateNumber(ota_stats.successful_updates);
    cJSON *ota_state = cJSON_CreateNumber((int)ota_get_state());
    cJSON *type = cJSON_CreateString("system_status");
    
    cJSON_AddItemToObject(json, "timestamp", timestamp);
    cJSON_AddItemToObject(json, "system", system);
    cJSON_AddItemToObject(json, "uptime", uptime);
    cJSON_AddItemToObject(json, "free_heap", free_heap);
    cJSON_AddItemToObject(json, "gpio_status", gpio_status);
    cJSON_AddItemToObject(json, "commands_processed", cmd_processed);
    cJSON_AddItemToObject(json, "command_errors", cmd_errors);
    cJSON_AddItemToObject(json, "water_count", water_count_json);
    cJSON_AddItemToObject(json, "firmware_version", firmware_version);
    cJSON_AddItemToObject(json, "ota_updates", ota_updates);
    cJSON_AddItemToObject(json, "ota_success", ota_success);
    cJSON_AddItemToObject(json, "ota_state", ota_state);
    cJSON_AddItemToObject(json, "type", type);
    
    char *json_string = cJSON_Print(json);
    
    if (json_string) {
        esp_mqtt_client_publish(mqtt_client, TOPIC_STATUS, json_string, 0, 1, 0);
        ESP_LOGI(TAG, "📈 發送系統狀態 (指令統計: 成功=%lu, 錯誤=%lu, 澆水=%lu)", 
                 processed_cmds, error_cmds, watering_count);
        free(json_string);
    }
    
    cJSON_Delete(json);
}

// ============================================================================
// LED 閃爍指示函數
// 功能：控制內建 LED 閃爍指定次數，用於狀態指示
// 參數：times - 閃爍次數
// 無返回值
// ============================================================================
static void blink_led(int times)
{
    for (int i = 0; i < times; i++) {
        // ESP32-C3 Super Mini 內建 LED 使用反向邏輯
        gpio_set_level(LED_GPIO, 0);  // 0 = LED 亮
        vTaskDelay(pdMS_TO_TICKS(100));  // 延遲 100ms
        gpio_set_level(LED_GPIO, 1);  // 1 = LED 熄
        vTaskDelay(pdMS_TO_TICKS(100));  // 延遲 100ms
    }
}

// ============================================================================
// 感測器任務函數 (FreeRTOS 任務)
// 功能：主要的感測器資料讀取和發送循環
// 參數：pvParameters - 任務參數 (此處未使用)
// 無返回值，任務函數永不返回
// ============================================================================
static void sensor_task(void *pvParameters)
{
    uint32_t last_data_time = 0;    // 上次發送資料的時間
    uint32_t last_status_time = 0;  // 上次發送狀態的時間
    
    while (1) {  // 任務主循環，永不結束
        // 等待 WiFi 連接完成 (來自 freertos/event_groups.h)
        // 參數：事件群組, 等待的位元, 是否清除, 是否等待所有位元, 超時時間
        // portMAX_DELAY 表示無限等待
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                           false, true, portMAX_DELAY);
        
        // 取得目前時間 (秒)
        uint32_t now = esp_timer_get_time() / 1000000;
        
        // 每2秒發送感測器資料 (比樹莓派版本的1秒稍慢，避免過於頻繁)
        if (now - last_data_time >= 2) {
            send_sensor_data();   // 發送感測器資料
            blink_led(1);         // LED 閃爍1次表示資料發送
            last_data_time = now; // 更新發送時間
        }
        
        // 每30秒發送系統狀態 (與樹莓派版本一致)
        if (now - last_status_time >= 30) {
            send_system_status();    // 發送系統狀態
            last_status_time = now;  // 更新發送時間
        }
        
        // 短暫延遲，讓其他任務有機會執行 (來自 freertos/task.h)
        vTaskDelay(pdMS_TO_TICKS(500));  // 延遲 500ms
    }
}

// ============================================================================
// 主程式入口函數 (ESP-IDF 特有)
// 功能：系統初始化，建立任務，啟動系統
// 無參數，無返回值
// 注意：ESP-IDF 使用 app_main() 而非標準 C 的 main()
// ============================================================================
void app_main(void)
{
    // ========================================================================
    // NVS (非揮發性儲存) 初始化
    // ========================================================================
    // 初始化 NVS，用於儲存 WiFi 配置等持久資料 (來自 nvs_flash.h)
    esp_err_t ret = nvs_flash_init();
    
    // 檢查 NVS 是否需要擦除重新初始化
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());  // 擦除 NVS
        ret = nvs_flash_init();              // 重新初始化
    }
    ESP_ERROR_CHECK(ret);  // 檢查初始化結果

    // ========================================================================
    // 系統啟動資訊輸出
    // ========================================================================
    ESP_LOGI(TAG, "🚀 ESP32-C3 土壤濕度感測器啟動");
    // esp_get_free_heap_size() 取得可用記憶體大小 (來自 esp_system.h)
    ESP_LOGI(TAG, "💾 可用記憶體: %d bytes", esp_get_free_heap_size());
    
    // ========================================================================
    // GPIO 初始化
    // ========================================================================
    // GPIO 配置結構 (來自 driver/gpio.h)
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,                          // 停用中斷
        .mode = GPIO_MODE_OUTPUT,                                // 設為輸出模式
        .pin_bit_mask = (1ULL << PUMP_GPIO) | (1ULL << LED_GPIO), // 設定腳位遮罩
        .pull_down_en = 0,                                       // 停用下拉電阻
        .pull_up_en = 0,                                         // 停用上拉電阻
    };
    // 應用 GPIO 配置 (來自 driver/gpio.h)
    gpio_config(&io_conf);
    
    // 設定 GPIO 初始狀態
    gpio_set_level(PUMP_GPIO, 0);  // 泵浦關閉
    gpio_set_level(LED_GPIO, 1);   // LED 熄滅 (反向邏輯)
    
    // 開機 LED 指示 - 閃爍3次表示系統啟動
    blink_led(3);
    
    // ========================================================================
    // 各模組初始化
    // ========================================================================
    adc_init();       // 初始化 ADC
    wifi_init_sta();  // 初始化 WiFi (Station 模式)
    mqtt_init();      // 初始化 MQTT 客戶端
    
    // 🔄 初始化指令處理模組
    ret = command_handler_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ 指令處理模組初始化失敗");
        return;  // 終止程式執行
    }
    
    // 🔄 新增：初始化 OTA 更新模組
    ret = ota_update_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ OTA 更新模組初始化失敗");
        return;  // 終止程式執行
    }

    // ========================================================================
    // 建立 FreeRTOS 任務
    // ========================================================================
    // xTaskCreate 建立新任務 (來自 freertos/task.h)
    // 參數：任務函數, 任務名稱, 堆疊大小, 任務參數, 優先順序, 任務句柄
    xTaskCreate(sensor_task,    // 任務函數
                "sensor_task",  // 任務名稱 (用於除錯)
                4096,          // 堆疊大小 (bytes)
                NULL,          // 任務參數
                5,             // 優先順序 (0-24，數字越大優先順序越高)
                NULL);         // 任務句柄 (不需要時可為 NULL)
    
    // ========================================================================
    // 系統初始化完成
    // ========================================================================
    ESP_LOGI(TAG, "✅ 系統初始化完成");
    ESP_LOGI(TAG, "📊 開始監測土壤濕度...");
    
    // app_main() 函數結束後，FreeRTOS 調度器接管系統
    // sensor_task 任務將開始執行主要的感測器循環
}// ============================================================================
// ESP32-C3 土壤濕度監測系統 - 完整註解版
// 功能：透過ADC讀取土壤濕度，透過WiFi/MQTT發送資料，遠端控制泵浦
// ============================================================================
