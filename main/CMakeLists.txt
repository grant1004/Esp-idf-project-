# main/CMakeLists.txt
# ESP-IDF 專案的主要模組建置配置

# 使用現代的 idf_component_register 語法
idf_component_register(
    SRCS "command_handler.c" "main.c" "ota_update.c"
    REQUIRES 
        driver          # GPIO, ADC 驅動
        esp_wifi        # WiFi 功能
        mqtt            # MQTT 客戶端
        nvs_flash       # 非揮發性儲存
        esp_event       # 事件處理
        json            # JSON 處理 (cJSON)
        esp_adc         # ADC 驅動
        esp_timer       # 高精度計時器
        freertos        # FreeRTOS (佇列、任務、事件群組)
        app_update      # OTA 應用程式更新
        esp_http_client # HTTP 客戶端 (用於 OTA 下載)
        bootloader_support # 啟動載入器支援
        spi_flash       # SPI Flash 和映像格式支援
    INCLUDE_DIRS "."    # 明確指定當前目錄
)