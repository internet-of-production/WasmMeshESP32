
/**
 * @file main.c
 * @brief main file for mesh communication 
 */

/*
If you use Platform IO, configuration for bluetooth and mesh is needed because corresponding components are disabled in the default configuration
1. Run "platformio run -t menuconfig" in the terminal
2. Enter Component menu
3. Enable Bluetooth 
4. Enable BLE-Mesh
5. Save it and quit

Partition for SPIFFS: https://docs.platformio.org/en/latest/platforms/espressif32.html#partition-tables

Configuration of main stack size: make menuconfig -> Component config -> Common ESP-related -> Main task stack size
https://github.com/espressif/esp-idf/issues/2824
Current stack size: 8192

*/


/* main.c - Application main entry point */

/*
 * SPDX-FileCopyrightText: 2017 Intel Corporation
 * SPDX-FileContributor: 2018-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_sensor_model_api.h"

#include "wifi_station.h"
#include "ble_server.h"

#include "ble_mesh_init.h"

//wasm3
#include "wasm3.h"
#include "m3_env.h"
#include "wasm3_defs.h"

#include "esp_spiffs.h"
#include "httpserver.h"
//#include <EEPROM.h>


#define WASM_STACK_SLOTS    4000
#define CALC_INPUT  2
#define FATAL(func, msg) { ESP_LOGE(TAG, "Fatal: " func " "); ESP_LOGE(TAG, "%s", msg);}
#define BASE_PATH "/spiffs"

static const char *TAG = "main";

IM3Environment env;
IM3Runtime runtime;
IM3Module module;
IM3Function calcWasm;
int wasmResult = 0;

//TODO: Keep status of Wasm file in non-volatile storage
/*
//EEPROM.read(0x00) == 1 => Wasm file is invalid
static void setWasmInvalidFlag(){
  EEPROM.write(WASM_INVALID_FLAG_OFFSET, 0x01);
  EEPROM.commit();
}

static void setWasmValidFlag(){
  EEPROM.write(WASM_INVALID_FLAG_OFFSET, 0x00);
  EEPROM.commit();
}

bool isWasmExecutable(){
  return EEPROM.read(WASM_INVALID_FLAG_OFFSET) == 0;
}

static void setWasmVersionId(uint8_t id){
  EEPROM.write(WASM_VERSION_ID_OFFSET, id);
  EEPROM.commit();
}

uint8_t getWasmVersionId(){
  return EEPROM.read(WASM_VERSION_ID_OFFSET);
}*/

/**
 * @fn 
 * WASM setup using wasm3
 */
static void run_wasm()
{
    // load wasm from SPIFFS
    /* If default CONFIG_ARDUINO_LOOP_STACK_SIZE 8192 < wasmFile,
    a new size must be given in  \Users\<user>\.platformio\packages\framework-arduinoespressif32\tools\sdk\include\config\sdkconfig.h
    https://community.platformio.org/t/esp32-stack-configuration-reloaded/20994/2
    */
    FILE* wasmFile = fopen("/spiffs/main.wasm", "rb");
    if (wasmFile == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return;
    }

    //get file status
    struct stat sb;
    if (stat("/spiffs/main.wasm", &sb) == -1) {
        perror("stat");
        exit(EXIT_FAILURE);
    }

    // read file
    unsigned int build_main_wasm_len = sb.st_size;
    unsigned char build_main_wasm[build_main_wasm_len];
    fread(build_main_wasm, 1, sb.st_size, wasmFile);
    fclose(wasmFile);

    ESP_LOGI(TAG, "wasm_length:");
    ESP_LOGI(TAG, "%d", build_main_wasm_len);

    ESP_LOGI(TAG, "Loading WebAssembly was successful");
    //ESP_LOGI(TAG, "Wasm Version ID: ");
    //ESP_LOGI(TAG, getWasmVersionId());

    M3Result result = m3Err_none;

    //it warks also without using variable
    //uint8_t* wasm = (uint8_t*)build_app_wasm;

    env = m3_NewEnvironment ();
    if (!env) {/*setWasmInvalidFlag();*/ FATAL("NewEnvironment", "failed"); esp_restart();}

    runtime = m3_NewRuntime (env, WASM_STACK_SLOTS, NULL);
    if (!runtime) {/*setWasmInvalidFlag();*/ FATAL("m3_NewRuntime", "failed"); esp_restart();}

    #ifdef WASM_MEMORY_LIMIT
    runtime->memoryLimit = WASM_MEMORY_LIMIT;
    #endif

    result = m3_ParseModule (env, &module, build_main_wasm, build_main_wasm_len);
    if (result) {/*setWasmInvalidFlag();*/ FATAL("m3_ParseModule", result); esp_restart();}

    result = m3_LoadModule (runtime, module);
    if (result) {/*setWasmInvalidFlag();*/ FATAL("m3_LoadModule", result); esp_restart();}

    // link
    //result = LinkArduino (runtime);
    //if (result) FATAL("LinkArduino", result);


    result = m3_FindFunction (&calcWasm, runtime, "calcWasm");
    if (result) {/*setWasmInvalidFlag();*/ FATAL("m3_FindFunction(calcWasm)", result); esp_restart();}

    ESP_LOGI(TAG, "Running WebAssembly...");

}

  /**
 * @fn 
 * Call WASM task
 */

  void wasm_task(){
    const void *i_argptrs[CALC_INPUT];
    char inputBytes[CALC_INPUT] = {0x01, 0x02};
    M3Result result = m3Err_none;
    
    for(int i=0; i<CALC_INPUT ;i++){
      i_argptrs[i] = &inputBytes[i];
    }

    /*
    m3_Call(function, number_of_arguments, arguments_array)
    To get return, one have to call a function with m3_Call first, then call m3_GetResultsV(function, adress).
    (Apparently) m3_Call stores the result in the liner memory, then m3_GetResultsV accesses the address.
    */
    result = m3_Call(calcWasm,CALC_INPUT,i_argptrs);                       
    if(result){
      //setWasmInvalidFlag();
      FATAL("m3_Call(calcWasm):", result);
      esp_restart();
    }

    result = m3_GetResultsV(calcWasm, &wasmResult);
    if(result){
      //setWasmInvalidFlag();
      FATAL("m3_GetResultsV(calcWasm):", result);
      esp_restart();
    }

  }

//TODO: Implement Web Server!! Display HTML, Write Wasm into File. In which directory shall html file be saved.

void app_main(void){

    //Initialize spiffs
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true
    };

    // Use settings defined above to initialize and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    esp_err_t ret_spiffs = esp_vfs_spiffs_register(&conf);

    if (ret_spiffs != ESP_OK) {
        if (ret_spiffs == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret_spiffs == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret_spiffs));
        }
        return;
    }

    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    /* Start the server for the first time */
    ESP_ERROR_CHECK(start_server(BASE_PATH));

    //BLE-Server
    /*ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(GATTS_TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(GATTS_TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(GATTS_TAG, "%s init bluetooth failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(GATTS_TAG, "%s enable bluetooth failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret){
        ESP_LOGE(GATTS_TAG, "gatts register error, error code = %x", ret);
        return;
    }
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret){
        ESP_LOGE(GATTS_TAG, "gap register error, error code = %x", ret);
        return;
    }
    ret = esp_ble_gatts_app_register(PROFILE_A_APP_ID);
    if (ret){
        ESP_LOGE(GATTS_TAG, "gatts app register error, error code = %x", ret);
        return;
    }
    ret = esp_ble_gatts_app_register(PROFILE_B_APP_ID);
    if (ret){
        ESP_LOGE(GATTS_TAG, "gatts app register error, error code = %x", ret);
        return;
    }
    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(500);
    if (local_mtu_ret){
        ESP_LOGE(GATTS_TAG, "set local  MTU failed, error code = %x", local_mtu_ret);
    }*/

    ESP_LOGI(TAG, "Loading wasm");
    run_wasm(NULL);

    while(true){
        wasm_task();
        ESP_LOGI(TAG, "Wasm result:");
        ESP_LOGI(TAG,"%d", wasmResult);
        vTaskDelay(1000 / portTICK_RATE_MS);
    }
}

/*void app_main(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "Initializing...");

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    board_init();

    err = bluetooth_init();
    if (err) {
        ESP_LOGE(TAG, "esp32_bluetooth_init failed (err %d)", err);
        return;
    }

    ble_mesh_get_dev_uuid(dev_uuid);

    err = ble_mesh_init();
    if (err) {
        ESP_LOGE(TAG, "Bluetooth mesh init failed (err %d)", err);
    }
}*/
