#include "ST77916.h"
#include "PCF85063.h"
#include "SD_MMC.h"
#include "TCA9554PWR.h"
#include "BAT_Driver.h"
#include "Audio_Driver.h"
#include "MIC_Speech.h"
#include "esp_board_init.h"
#include "nvs_flash.h"
#include "app/app_clock.h"
#include "app/music.h"
#include "app/sfx.h"
#include "app/power.h"
#include "app/settings.h"
#include "app/timers.h"
#include "app/wifi_mgr.h"
#include "app/ui.h"
void Driver_Loop(void *parameter)
{
    // WiFi stays off until it is needed (NTP / home mode later): saves power.

    while(1)
    {
        PCF85063_Loop();
        BAT_Get_Volts();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

void Driver_Init(void)
{
    Flash_Searching();
    BAT_Init();
    EXIO_Init();                    // Example Initialize EXIO
    PCF85063_Init();
    xTaskCreatePinnedToCore(
        Driver_Loop, 
        "Other Driver task",
        3072,                           // peak ~1.3 KB (STK report)
        NULL, 
        3, 
        NULL, 
        0);
}


void app_main(void)
{
    power_boot_prepare();          // release the pins held during deep sleep

    // NVS before anything else: the pet save and settings live here.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_board_init(16000, 2, 16));
    // A deep-sleep timer check-in: update the pet and sleep again, no screen.
    if (power_woke_by_timer()) power_checkin_and_sleep();   // never returns
    Driver_Init();
    SD_Init();
    LCD_Init();
    Audio_Init();

    // LVGL first: its draw buffers need internal DMA RAM, which esp-sr
    // (MIC_Speech_init) otherwise exhausts -> assert(buf2) boot loop.
    LVGL_Init();   // returns the screen object
    settings_load();               // before the speech engine: it reads the wake word
    timers_load();
    wifi_mgr_init();               // loads saved networks; WiFi itself stays off
    MIC_Speech_init();
    music_set_vol(g_settings.music_vol);   // the saved volume, for Talk as well as Music

    app_clock_init();
    sfx_init();
    ui_start();       // replaces the Waveshare demo UI

    while (1) {
        // raise the task priority of LVGL and/or reduce the handler period can improve the performance
        vTaskDelay(pdMS_TO_TICKS(10));
        // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
        lv_timer_handler();
    }

}






