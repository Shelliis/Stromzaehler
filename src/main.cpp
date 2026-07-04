
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPClient.h>

#include "config.h"
#include "ferraris_calc.h"

#ifndef FERRARIS_CONFIG
#define FERRARIS_CONFIG

// CONFIGURATION HERE vvvvvv
// #define LED_BUILTIN_NODE 16 // LED for signalling
#ifdef WOKWI_SIM
// Wokwi simuliert nur das ESP-01-Modul (nur GPIO0 und GPIO2 herausgefuehrt).
// Pin-Umbelegung gilt nur fuer die Simulation, echte Hardware (esp12e) bleibt unveraendert.
#define FERRARIS_PIN 2
#ifndef LED_BUILTIN
#define LED_BUILTIN 0
#endif
#else
#define FERRARIS_PIN 13 // IO pin for
#endif
#define FERRARIS_DIVIDER 150 // Number of rotations for a full kWh

#define ON true
#define OFF false

#define SEND_ON LOW     // LOW == Wechsel auf LED AN
#define MSG_BUF_LEN 100 // JSON preparation
#define delayMS 25

// SSID und PSK kommen aus include/config.h (nicht in Git)

// <millis per hour> * <W instead of kW> / <rounds per kW>:
#define ROTATION_WORTH_Wms ((float)60 * 60 * 1000 * 1000 / FERRARIS_DIVIDER)
#endif

float val_ROTATION_WORTH_Wms = ROTATION_WORTH_Wms; // einmal berechnen

WiFiClient espClient;
char debugMsg[MSG_BUF_LEN];

int last_val = -1;
long last_millis = -1;

// WLAN LED Ticks
int wlanTick = 0;
boolean led_buildin_onoff = ON;
int wlan_error_counter = 0;
int httpResponseCode_lasterror;

// Signalphasendauer
long onPhaseUm = 0;
long onPhaseDauer = 0;
bool phaseFlag = false;
bool onPhaseFlag = false;
long offPhaseStartUm = 0; // OFF Phase ist LED aus == Markierung Scheibe
long offPhaseDauer = 0;
bool offPhaseFlag = false;

void setup_wifi()
{
  delay(10);
  Serial.print("\nConnecting to ");
  Serial.println(SSID);

  WiFi.begin(SSID, PSK);

  while (WiFi.status() != WL_CONNECTED)
  {
    pinMode(LED_BUILTIN, led_buildin_onoff);
    delay(500);
    Serial.print(".");
    led_buildin_onoff = !led_buildin_onoff;
  }

  digitalWrite(LED_BUILTIN, LOW); // Signal WiFi on 1st builtin LED
  Serial.println("  WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  pinMode(LED_BUILTIN, OFF);
  led_buildin_onoff = OFF; // init for WLAN LED Ticks
  wlanTick = 0;            // init for WLAN LED Ticks
  ArduinoOTA.begin();
}

void setup()
{
  Serial.begin(9600);
  setup_wifi();

  // Set IO modes
  pinMode(FERRARIS_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  // pinMode(LED_BUILTIN_NODE, OUTPUT);
}

void loop()
{
  long now = millis();
  int val = digitalRead(FERRARIS_PIN); // first read the data for precision

  // snprintf(debugMsg, MSG_BUF_LEN, "ON:%ld OFF:%ld   %d", onPhaseDauer, offPhaseDauer, val);
  // Serial.println(debugMsg);

#define min_offPhase 100 // zum entprellen
  if (offPhaseDauer > min_offPhase)
  {

    if (val == SEND_ON && last_val == !SEND_ON) // == Wechsel von LED AUS auf AN
    {

      if (last_millis != -1)
      {
        char msg[MSG_BUF_LEN];
        long diff_millis = now - last_millis;
        pinMode(LED_BUILTIN, true);
        float watts = calculateWatts(val_ROTATION_WORTH_Wms, diff_millis);
        snprintf(msg, MSG_BUF_LEN, "{\"millis\": %ld, \"W\": %f, \"OFF\":%ld, \"ON\":%ld, \"ERR\":%d, \"RC\":%d }", diff_millis, watts, offPhaseDauer, onPhaseDauer, wlan_error_counter, httpResponseCode_lasterror);
        // Serial.println(msg);

        if (WiFi.status() == WL_CONNECTED)
        {
          HTTPClient http;
          http.begin(espClient, "http://192.168.2.211:5000/json2");
          http.addHeader("Content-Type", "application/json");
          http.addHeader("Accept", "application/json");
          int httpResponseCode = http.POST(msg);
          if (httpResponseCode != 200)
          {
            wlan_error_counter++;
            httpResponseCode_lasterror = httpResponseCode;
            //          String response = http.getString();
            //          Serial.println(httpResponseCode);
            //          Serial.println(response);
            //         }else {
            // Serial.print("Error on sending PUT Request: ");
            // Serial.println(httpResponseCode);
          }
          http.end();
        }
        pinMode(LED_BUILTIN, false);
      }
      last_val = val;
      last_millis = now;
    }
    else if (val == !SEND_ON)
    {
      last_val = val;
    }
  }

// ###################################################
// WLAN LED Ticks
#define tickEvery 5000 // in ms
#define tickDauer 100

  if (wlanTick > tickEvery)
  {
    if (!led_buildin_onoff)
    {
      led_buildin_onoff = ON;
      if (WiFi.status() == WL_CONNECTED)
      {
        pinMode(LED_BUILTIN, ON);
      }
    }

    if (wlanTick > (tickEvery + tickDauer))
    {
      led_buildin_onoff = OFF;
      wlanTick = 0;
      pinMode(LED_BUILTIN, OFF);
    }
  }

  // ###################################################
  // Dauer ON OFF Phasen messen
  switch (val)
  {
    case LOW:
      if (onPhaseFlag == false)
      {
        onPhaseUm = now;
        onPhaseFlag = true;
        offPhaseFlag = false;
      }
      onPhaseDauer = now - onPhaseUm;
      break;

    case HIGH:
      if (offPhaseFlag == false)
      {
        offPhaseStartUm = now;
        onPhaseFlag = false;
        offPhaseFlag = true;
      }
      offPhaseDauer = now - offPhaseStartUm;
      break;

    default:
      break;
  }

  delay(delayMS);
  wlanTick += delayMS;

  // Check for over the air update request and (if present) flash it
  ArduinoOTA.handle();
}