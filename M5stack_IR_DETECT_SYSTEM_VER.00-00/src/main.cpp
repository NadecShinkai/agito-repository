/*******************************************************************************
  Copyright (c) 2023 by Nagaoka-shi 
                   Equipped with M5stack Core2 source code
  Create by Nagaokashi - Satsuki Shinkai
  Date: 2023/11/13
*******************************************************************************/
#include <stdio.h>
#include <string.h>
#include <M5Core2.h>
#include <AXP192.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

/* SYSTEM INFO ************************************************************/
#define VER_INFO    "M5Stack_IR_DETECT_SYSYTEM_VER.00-00"

/* GPIO  ******************************************************************/
#define GPIO_INP_IR 19

/* M5Stack Core2 **********************************************************/
AXP192 power;

/* TIMER SETTING **********************************************************/
hw_timer_t *timer1 = NULL;
hw_timer_t *timer2 = NULL;
volatile int32_t timer1Counter = 0;
volatile int32_t timer2Counter = 0;
portMUX_TYPE timer1Mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE timer2Mux = portMUX_INITIALIZER_UNLOCKED;
const uint32_t AUTO_REBOOT_TIME = 24 * 60 * 60;

/* DISCORD SETTING ********************************************************/
#define DISCORD_WEBHOOK_URL  "https://discord.com/api/webhooks/1157831581415780423/pBz4JI1_IRbBj1Ql_Jkr8vPKyUMsgA5P0OKHaznntR5wJI-T4PGQmdsQaOVrmERi1XBg"
#define DISCORD_SEND_MSG "誰かがアジトに入ったぞ 小林"
#define JST     3600* 9

/* FILE SYSTEM SETTING ****************************************************/
const char* fname = "/wifi.csv";
File fp;
char ssid[32];
char pass[32];

/* DI FLITER **************************************************************/
#define IR_DETECTED     1
#define IR_NOT_DETECTED 0
const uint8_t DI_BUF_SIZE = 5;
typedef struct{
  uint16_t  diPtr;
  uint8_t   diBuf[DI_BUF_SIZE];
}StGpioDiData_t;

/* GLOBAL VAL DECLARATION *************************************************/
TaskHandle_t xMainTaskHandler;
TaskHandle_t xWifiTaskHandler;
TaskHandle_t xGpioTaskHandler;
SemaphoreHandle_t xTimer1Semaphore;
SemaphoreHandle_t xTimer2Semaphore;
QueueHandle_t xIRDetectQueue;
uint32_t gAccessCounter;

/* PRIVATE PROTOTYPES *****************************************************/
void setup(void);
void loop(void);
static void IRAM_ATTR onTimer1(void);
static void IRAM_ATTR onTimer2(void);
static void mainTask(void*);
static void wifiTask(void*);
static void gpioTask(void*);
static void SetwifiSD(const char*);
static void gpioInit(void);
static void readIrDetectState(StGpioDiData_t*);
static void clearDiData(StGpioDiData_t*);
static uint8_t isAllDiBufTrue(StGpioDiData_t*);
static void wifiConectRecovery(void);
static void sendDiscordMessage(const char*, const char*);

//------------------------------------------------------------------
// TIMER1 1000ms
//------------------------------------------------------------------
static void IRAM_ATTR onTimer1(void){
  portENTER_CRITICAL_ISR(&timer1Mux);
  timer1Counter++;
  portEXIT_CRITICAL_ISR(&timer1Mux);
  xSemaphoreGiveFromISR(xTimer1Semaphore, NULL);
}

//------------------------------------------------------------------
// TIMER2 100ms
//------------------------------------------------------------------
static void IRAM_ATTR onTimer2(void){
  portENTER_CRITICAL_ISR(&timer2Mux);
  timer2Counter--;
  if(timer2Counter <= 0){
    timer2Counter = 0;
  }
  portEXIT_CRITICAL_ISR(&timer2Mux);
  xSemaphoreGiveFromISR(xTimer2Semaphore, NULL);
}


//------------------------------------------------------------------
// Main Task
//------------------------------------------------------------------
static void mainTask(void *pvParameters){
  (void)pvParameters;
  while(1){
    /* check timer1 */
    if (xSemaphoreTake(xTimer1Semaphore, 0) == pdTRUE){
    portENTER_CRITICAL(&timer1Mux);
    if(timer1Counter > AUTO_REBOOT_TIME){
      ESP.restart();
    }
    portEXIT_CRITICAL(&timer1Mux);
  }
    delay(1);
  }
}

//------------------------------------------------------------------
// gpio Task
//------------------------------------------------------------------
static void gpioTask(void *pvParameters){
  (void)pvParameters;
  StGpioDiData_t GpioDiData;
  uint8_t dataToSend = 0; //空データー
  clearDiData(&GpioDiData);
  while(1){
    /*set ir detect status */
    readIrDetectState(&GpioDiData);
    /* check di filter and send queue*/
    if(isAllDiBufTrue(&GpioDiData)){
      xQueueSend(xIRDetectQueue, &dataToSend, portMAX_DELAY);
      clearDiData(&GpioDiData);
      Serial.printf("\r\n>Detect");
    }
    delay(50);
  }
}
//
static void readIrDetectState(StGpioDiData_t *data){
    data->diBuf[data->diPtr] = !digitalRead(GPIO_INP_IR);
    if(data->diPtr < DI_BUF_SIZE -1){
      data->diPtr++;
    }else{
      data->diPtr = 0;
    }
    //Serial.printf("\r\n>diBuf[data->ptr]=%d", data->diBuf[data->diPtr]);
    //Serial.printf("\r\n>data->ptr=%d",data->diPtr);
}
//
static void clearDiData(StGpioDiData_t *data){
  data->diPtr = 0;
  for(int i = 0; i < DI_BUF_SIZE; i++){
    data->diBuf[i] = 0;
  }
}
//
static uint8_t isAllDiBufTrue(StGpioDiData_t* data){
  for(int i = 0; i < DI_BUF_SIZE; i++){
    if(data->diBuf[i] == IR_DETECTED){
      return 0;
    } 
  }
  return 1;
}


//------------------------------------------------------------------
// WiFi Task
//------------------------------------------------------------------
static void wifiTask(void *pvParameters){
  (void)pvParameters;
  uint8_t dataToReceive = 0;
  while(1){
    uint8_t disocrdMsgSendFlag = false;
    /* REQUEST DISCORD API */
    xQueueReceive(xIRDetectQueue, &dataToReceive, portMAX_DELAY);
    portENTER_CRITICAL(&timer2Mux); //ここのタイマー処理は、gpioの入力処理でやった方がいいかも
    if(timer2Counter == 0){
      disocrdMsgSendFlag = true;
      timer2Counter = 30;
    }
    portEXIT_CRITICAL(&timer2Mux);
    if(disocrdMsgSendFlag){
      sendDiscordMessage(DISCORD_WEBHOOK_URL, DISCORD_SEND_MSG);
      Serial.printf("\r\n>Send Msg to Discord");
    }
    /* Check WiFi Status */
    wifiConectRecovery();
    delay(1);
  }
}
//
static void wifiConectRecovery(void){
  if(WiFi.status() != WL_CONNECTED){
    WiFi.disconnect();
    WiFi.reconnect();
  }
}
//
static void sendDiscordMessage(const char* url, const char* content){
  //
  DynamicJsonDocument doc(JSON_OBJECT_SIZE(3));
  doc["username"] = "Doorbell";
  doc["avatar_url"] = "https://i.imgur.com/6YSCfLa.png";
  doc["content"] = content;
  String json;
  serializeJson(doc, json);
  //
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.POST(json);
  http.end();
}

/* FUNCTIONS **************************************************************/
void SetwifiSD(const char *file){
  unsigned int cnt = 0;
  char data[64];
  char *str;
  //
  fp = SD.open(fname, FILE_READ);
  while(fp.available()){
    data[cnt++] = fp.read();
  }
  strtok(data,",");
  str = strtok(NULL,"\r");    // CR
  strncpy(&ssid[0], str, strlen(str));
  //
  strtok(NULL,",");
  str = strtok(NULL,"\r");    // CR
  strncpy(&pass[0], str, strlen(str));
  //
  Serial.printf("\r\n>SID = %s",ssid);
  Serial.printf("\r\n>PASS = %s",pass);
  // STA設定
  WiFi.mode(WIFI_STA);     // STAモードで動作
  WiFi.begin(ssid, pass);
  unsigned long timeStamp = millis();
  while (WiFi.status() != WL_CONNECTED) {
      delay(100);
      Serial.printf("\r\n>.");
      if(millis() - timeStamp > 10 * 1000){
        Serial.printf("\r\n>Error:WiFi Time Out");
        delay(1000);
        ESP.restart();
      }
  }
  Serial.printf("\r\n>WiFi Connected");
  //
  configTime( JST, 0, "ntp.nict.jp", "ntp.jst.mfeed.ad.jp");
  //
  fp.close();
}



//------------------------------------------------------------------
// Arduino setup
//------------------------------------------------------------------
void setup() {
  M5.begin();
  //
  pinMode(GPIO_INP_IR, INPUT);
  //
  SD.begin();
  SetwifiSD(fname); // Get ssid
  // binary semaphore init
  xTimer1Semaphore = xSemaphoreCreateBinary();
  xSemaphoreGive(xTimer1Semaphore);
  xTimer2Semaphore = xSemaphoreCreateBinary();
  xSemaphoreGive(xTimer2Semaphore);
  // queue init
  xIRDetectQueue = xQueueCreate(1, sizeof(uint16_t));
  // timer init
  timer1 = timerBegin(0, 80, true);
  timerAttachInterrupt(timer1, &onTimer1, true);
  timerAlarmWrite(timer1, 1000000, true);
  timerAlarmEnable(timer1);
  //
  timer2 = timerBegin(1, 80, true);
  timerAttachInterrupt(timer2, &onTimer2, true);
  timerAlarmWrite(timer2, 100000, true);
  timerAlarmEnable(timer2);
  // create tasks
  xTaskCreatePinnedToCore(mainTask, "mainTask", 4096,  NULL,  1,  &xMainTaskHandler, 0);
  xTaskCreatePinnedToCore(wifiTask, "wifiTask", 8192,  NULL,  1,  &xWifiTaskHandler, 1);
  xTaskCreatePinnedToCore(gpioTask, "gpioTask", 4096,  NULL,  1,  &xGpioTaskHandler, 0);
}

//------------------------------------------------------------------
// Arduino Loop
//------------------------------------------------------------------
void loop() {

}