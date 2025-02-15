/*
 * This file is part of Chorus32-ESP32LapTimer
 * (see https://github.com/AlessandroAU/Chorus32-ESP32LapTimer).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>

#include <esp_task_wdt.h>
#include <rom/rtc.h>

#include "Comms.h"
#include "ADC.h"
#include "HardwareConfig.h"
#include "RX5808.h"
#include "Bluetooth.h"
#include "settings_eeprom.h"
#ifdef OLED
#include "OLED.h"
#endif
#include "TimerWebServer.h"
#ifdef BEEPER
#include "Beeper.h"
#endif
#include "Calibration.h"
#include "Output.h"
#ifdef USE_BUTTONS
#include "Buttons.h"
#endif
#include "TimerWebServer.h"
#include "Watchdog.h"
#include "Utils.h"
#include "Laptime.h"
#include "Wireless.h"
#include "Logging.h"

#include "CrashDetection.h"
#ifdef USE_ARDUINO_OTA
#include <ArduinoOTA.h>
#endif

static TaskHandle_t adc_task_handle = NULL;

void IRAM_ATTR adc_read() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  /* un-block the interrupt processing task now */
  vTaskNotifyGiveFromISR(adc_task_handle, &xHigherPriorityTaskWoken);
  if(xHigherPriorityTaskWoken) {
    portYIELD_FROM_ISR();
  }
}

void IRAM_ATTR adc_task(void* args) {
  watchdog_add_task();
  while(42) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if(LIKELY(!isCalibrating())) {
      nbADCread(NULL);
    } else {
      rssiCalibrationUpdate();
    }
    watchdog_feed();
  }
}

void eeprom_task(void* args) {
  const TickType_t xDelay = EEPROM_COMMIT_DELAY_MS / portTICK_PERIOD_MS;
  while(42) {
    EepromSettings.save();
    vTaskDelay(xDelay);
  }
}


void setup() {
  init_crash_detection();
  Serial.begin(SERIAL_BAUD_RATE);
  Serial.println("Booting....");
#ifdef USE_ARDUINO_OTA
  if(is_crash_mode()) {
    log_e("Detected crashing. Starting ArduinoOTA only!");
    InitWifiAP();
    ArduinoOTA.begin();
    return;
  }
#endif

  InitSPI();
  InitHardwarePins();
  //RXPowerDownAll(); // Powers down all RX5808's, already called in InitSPI()

#ifdef OLED
  oledSetup();
#endif

  bool all_modules_off = false;
  if (rtc_get_reset_reason(0) == 15 || rtc_get_reset_reason(1) == 15) {
    all_modules_off = true;
    Serial.println("Rebooted from brownout...disabling all modules...");
  }
#ifdef USE_BUTTONS
  newButtonSetup();
#endif
  resetLaptimes();
#ifdef BEEPER
  beeper_init();
#endif

  EepromSettings.setup();
  setRXADCfilterCutoff(EepromSettings.RXADCfilterCutoff);
  setADCVBATmode(EepromSettings.ADCVBATmode);
  setVbatCal(EepromSettings.VBATcalibration);

  for(int i = 0; i < MAX_NUM_PILOTS; ++i) {
    setRXBandPilot(i, EepromSettings.RXBand[i]);
    setRXChannelPilot(i, EepromSettings.RXChannel[i]);
  }
  delay(30);
  ConfigureADC(all_modules_off);
  delay(250);

  InitWifi();

  if (!EepromSettings.SanityCheck()) {
    EepromSettings.defaults();
    Serial.println("Detected That EEPROM corruption has occured.... \n Resetting EEPROM to Defaults....");
  }

  commsSetup();

  for (int i = 0; i < MAX_NUM_PILOTS; i++) {
    setRSSIThreshold(i, EepromSettings.RSSIthresholds[i]);
  }

  // inits modules with defaults.  Loops 10 times  because some Rx modules dont initiate correctly.
  for (int i = 0; i < getNumReceivers()*10; i++) {
    setModuleChannelBand(i % getNumReceivers());
    delayMicroseconds(MIN_TUNE_TIME_US);
  }

  beeper_add_to_queue(); // beep once at the beginning to know its working 

  init_outputs();
  Serial.println("Starting ADC reading task on core 0");

  xTaskCreatePinnedToCore(adc_task, "ADCreader", 4096, NULL, 1, &adc_task_handle, 0);
  hw_timer_t* adc_task_timer = timerBegin(0, 8, true);
  timerAttachInterrupt(adc_task_timer, &adc_read, true);
  timerAlarmWrite(adc_task_timer, 1667, true); // 6khz -> 1khz per adc channel
  timerAlarmEnable(adc_task_timer);

  xTaskCreatePinnedToCore(eeprom_task, "eepromSave", 4096, NULL, tskIDLE_PRIORITY, NULL, 1);
}

void loop() {
#ifdef USE_ARDUINO_OTA
  ArduinoOTA.handle();
  if(is_crash_mode()) return;
#endif
  if(millis() > CRASH_COUNT_RESET_TIME_MS) {
    reset_crash_count();
  }
#ifdef USE_BUTTONS
  newButtonUpdate();
#endif
#ifdef OLED
  // We need to pause the OLED during update otherwise we crash due to I2C
  if(!isUpdating()) {
    OLED_CheckIfUpdateReq();
  }
#endif
  sendNewLaps();
  update_outputs();
  update_comms();

#ifdef WIFI_MODE_ACCESSPOINT
  handleDNSRequests();
#endif

#ifdef BEEPER
  beeperUpdate();
#endif

  if(UNLIKELY(!isInRaceMode())) {
    thresholdModeStep();
  }
}
