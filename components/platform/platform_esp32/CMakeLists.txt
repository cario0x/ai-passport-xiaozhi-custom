# components/platform_esp32/CMakeLists.txt
idf_component_register(
    SRCS "src/disp_driver.c" "src/lvgl_port_setup.c" "src/screen_capture_lvgl.c"
         "src/btn_iot_button.c"
         "src/audio_es8311.c" "src/platform_sys.c" "src/ble_config.c"
         "src/ble_scan.c" "src/kv_nvs.c" "src/jpeg_store.c" "src/jpeg_view.c"
         "src/png_decode.c" 
         "src/battery_cw2017.c" "src/battery_m5pm1.c" "src/stopwatch_hardware.cpp"
         "src/identity_nvs.c" "src/provision_console.c" "src/token_mac_esp.c"
         "src/audio_store.c" "src/platform_clicker.c" "src/clicker_device_info.c" "src/clicker_bond_store.c"
         "src/avatar_store.c" "src/platform_pet_art.c"
    INCLUDE_DIRS "include"
    REQUIRES ports services driver esp_lcd
    PRIV_REQUIRES presentation esp_lvgl_port button esp_codec_dev esp_hw_support bt nvs_flash
                  esp_driver_i2c esp_driver_i2s esp_partition esp_new_jpeg esp_adc
                  mbedtls esp_driver_usb_serial_jtag esp_rom spiffs
                  m5stack__m5ioe1 m5stack__m5pm1 espressif__esp_lcd_co5300)
