
#if defined(ESP32)
#include <WiFi.h>
#include <HTTPClient.h>
#else
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#endif
#include <ArduinoOTA.h>
#include <time.h>

#include "config.h"
#include "ferraris_calc.h"

// Logmeldungen zentral ein-/ausschalten: 1 = Serial-Ausgabe aktiv, 0 = komplett deaktiviert (No-Op).
#define ENABLE_LOGGING 0

#if ENABLE_LOGGING
#define LOG_PRINT(x) Serial.print(x)
#define LOG_PRINTLN(x) Serial.println(x)
#else
#define LOG_PRINT(x)
#define LOG_PRINTLN(x)
#endif

#ifndef FERRARIS_CONFIG
#define FERRARIS_CONFIG

// CONFIGURATION HERE vvvvvv
// #define LED_BUILTIN_NODE 16 // LED for signalling
#ifdef WOKWI_SIM
// Wokwi unterstuetzt ESP8266 nicht als programmierbaren Chip, daher laeuft die
// Simulation auf einem ESP32-DevKit (siehe platformio.ini/diagram.json).
// Pin-Umbelegung gilt nur fuer die Simulation, echte Hardware (esp12e) bleibt unveraendert.
#define FERRARIS_PIN 4
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
#else
#define FERRARIS_PIN 13 // IO pin for
#endif
#define FERRARIS_DIVIDER 150 // Number of rotations for a full kWh

#define ON true
#define OFF false

#define SEND_ON LOW     // LOW == Wechsel auf LED AN
#define MSG_BUF_LEN 160 // JSON preparation (inkl. "zeitpkt")
#define delayMS 25

#define JSON_ENDPOINT "http://192.168.2.211:5000/json2"
#define PENDING_MSG_BUF_SIZE 200 // max. Anzahl gepufferter, noch nicht gesendeter Nachrichten

#ifdef WOKWI_SIM
// Taster in diagram.json: solange gedrueckt (LOW), wird ein fehlerhafter
// HTTP-Response-Code simuliert, ohne echt zu senden. Loslassen -> echter Code.
// Gilt sowohl fuer den Erstversand als auch fuer das Nachsenden aus dem Puffer.
#define WIFI_ERROR_BUTTON_PIN 5
#define WIFI_ERROR_SIMULATED_CODE -1
#endif

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

// Zeit-Synchronisation per NTP (siehe syncTime()/formatEpoch()).
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3" // Europe/Berlin inkl. Sommerzeit
#define NTP_SERVER "pool.ntp.org"
#define TIME_FORMAT "%Y-%m-%d %H:%M:%S"
#ifdef WOKWI_SIM
// Kurzes Intervall in der Simulation, um den Zyklus beobachten zu koennen.
#define TIME_SYNC_INTERVAL_MS 60000
#else
#define TIME_SYNC_INTERVAL_MS (12UL * 60UL * 60UL * 1000UL) // 12 Stunden auf echter Hardware
#endif

// Rohwerte einer Messung statt fertigem JSON-Text - spart deutlich RAM im Puffer
// (z.B. 28-32 Byte statt 160 Byte pro Eintrag).
struct PendingReading
{
  long diffMillis;
  float watts;
  long offPhase;
  long onPhase;
  time_t zeitpkt;
  int errCount;
  int lastResponseCode;
};

// Ringpuffer fuer Messungen, deren Versand fehlgeschlagen ist.
PendingReading pendingReadings[PENDING_MSG_BUF_SIZE];
int pendingMsgHead = 0;  // Index der aeltesten gepufferten Messung
int pendingMsgCount = 0; // Anzahl aktuell gepufferter Messungen

// Baut aus einem Unix-Timestamp den JSON-Zeitstempel; "unknown" falls noch nie synchronisiert.
void formatEpoch(time_t epoch, char *buf, size_t len)
{
  if (epoch < 1000000000) // Sentinel: vor Jahr 2001 == nie erfolgreich per NTP synchronisiert
  {
    strncpy(buf, "unknown", len);
    return;
  }
  struct tm timeinfo;
  localtime_r(&epoch, &timeinfo);
  strftime(buf, len, TIME_FORMAT, &timeinfo);
}

// Rendert eine PendingReading in den uebergebenen JSON-Puffer.
void formatReading(char *out, size_t outLen, const PendingReading &r)
{
  char zeitpkt[20];
  formatEpoch(r.zeitpkt, zeitpkt, sizeof(zeitpkt));
  snprintf(out, outLen, "{\"millis\": %ld, \"zeitpkt\": \"%s\", \"W\": %f, \"OFF\":%ld, \"ON\":%ld, \"ERR\":%d, \"RC\":%d, \"PUF\":%d }",
           r.diffMillis, zeitpkt, r.watts, r.offPhase, r.onPhase, r.errCount, r.lastResponseCode, pendingMsgCount);
}

#define HTTP_DISCARD -409 // Sentinel: Konflikt (409), Nachricht nicht sinnvoll wiederholbar

// Fuehrt den eigentlichen HTTP-POST aus, liefert den HTTP-Response-Code, oder
// HTTP_DISCARD, wenn der Server einen Konflikt (409, z.B. Zeitstempel-Kollision)
// meldet - in dem Fall soll die Nachricht verworfen statt weiter versucht werden.
// Solange der Taster gehalten wird, wird stattdessen ein Fehler simuliert
// (gilt fuer Erstversand und Nachsenden aus dem Puffer gleichermassen).
int postMessage(const char *message)
{
#ifdef WOKWI_SIM
  if (digitalRead(WIFI_ERROR_BUTTON_PIN) == LOW)
  {
    return WIFI_ERROR_SIMULATED_CODE;
  }
#endif

  HTTPClient http;
  http.begin(espClient, JSON_ENDPOINT);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  int httpResponseCode = http.POST(message);
  http.end();

  if (httpResponseCode == 409)
  {
    LOG_PRINT("Nachricht verworfen (HTTP 409 Conflict): ");
    LOG_PRINTLN(message);
    return HTTP_DISCARD;
  }

  return httpResponseCode;
}

// Legt eine Messung in den Puffer; ist er voll, wird die aelteste verworfen.
void bufferMessage(const PendingReading &reading)
{
  if (pendingMsgCount >= PENDING_MSG_BUF_SIZE)
  {
    pendingMsgHead = (pendingMsgHead + 1) % PENDING_MSG_BUF_SIZE;
    pendingMsgCount--;
    LOG_PRINTLN("Nachrichtenpuffer voll, aelteste gepufferte Messung wird verworfen");
  }
  int writeIndex = (pendingMsgHead + pendingMsgCount) % PENDING_MSG_BUF_SIZE;
  pendingReadings[writeIndex] = reading;
  pendingMsgCount++;

  LOG_PRINT("Messung gepuffert, aktuell im Puffer: ");
  LOG_PRINTLN(pendingMsgCount);
}

#define MAX_FLUSH_PER_CALL 25 // begrenzt Blockierzeit von loop(), Rest folgt beim naechsten Aufruf

// Versucht, gepufferte Messungen der Reihe nach nachzusenden (FIFO); pro Aufruf
// hoechstens MAX_FLUSH_PER_CALL Stueck, damit loop() bei einem grossen Rueckstau
// nicht zu lange blockiert und echte Impulse verpasst.
void flushPendingMessages()
{
  int sentThisCall = 0;
  while (pendingMsgCount > 0 && sentThisCall < MAX_FLUSH_PER_CALL && WiFi.status() == WL_CONNECTED)
  {
    char msg[MSG_BUF_LEN];
    formatReading(msg, MSG_BUF_LEN, pendingReadings[pendingMsgHead]);
    int httpResponseCode = postMessage(msg);
    if (httpResponseCode == 200)
    {
      LOG_PRINT("Gepufferte Nachricht nachtraeglich gesendet: ");
      LOG_PRINTLN(msg);
      pendingMsgHead = (pendingMsgHead + 1) % PENDING_MSG_BUF_SIZE;
      pendingMsgCount--;
      sentThisCall++;
    }
    else if (httpResponseCode == HTTP_DISCARD)
    {
      pendingMsgHead = (pendingMsgHead + 1) % PENDING_MSG_BUF_SIZE;
      pendingMsgCount--;
      sentThisCall++;
    }
    else
    {
      httpResponseCode_lasterror = httpResponseCode;
      break; // weiterhin Fehler, spaeter erneut versuchen
    }
  }
}

// Geplanter naechtlicher Neustart (siehe loop()) gegen Heap-Fragmentierung bei
// Dauerbetrieb. Der "letzter Neustart-Tag" muss einen ESP.restart() ueberdauern,
// damit ein schneller Reconnect direkt danach nicht sofort erneut ausloest -
// dafuer wird RTC-Speicher genutzt (bleibt bei Software-Reset erhalten, nur ein
// echter Stromausfall setzt ihn zurueck).
#define RESTART_HOUR 4        // Neustart-Fenster: ab dieser vollen Stunde
#define RESTART_MAX_WATTS 1000 // nur wenn aktuelle Leistung darunter liegt
float lastWatts = 0;

#if defined(ESP32)
RTC_DATA_ATTR int rtcLastRestartDay = -1;

int getLastRestartDay() { return rtcLastRestartDay; }
void setLastRestartDay(int day) { rtcLastRestartDay = day; }
#else
struct RtcRestartData
{
  uint32_t magic;
  int lastRestartDay;
};
#define RTC_RESTART_MAGIC 0x52535431 // "RST1", zur Gueltigkeitspruefung der RTC-Daten
#define RTC_MEM_OFFSET 64            // 4-Byte-Bloecke; niedrige Bloecke sind dem SDK vorbehalten

int getLastRestartDay()
{
  RtcRestartData data;
  ESP.rtcUserMemoryRead(RTC_MEM_OFFSET, (uint32_t *)&data, sizeof(data));
  if (data.magic != RTC_RESTART_MAGIC)
  {
    return -1;
  }
  return data.lastRestartDay;
}

void setLastRestartDay(int day)
{
  RtcRestartData data;
  data.magic = RTC_RESTART_MAGIC;
  data.lastRestartDay = day;
  ESP.rtcUserMemoryWrite(RTC_MEM_OFFSET, (uint32_t *)&data, sizeof(data));
}
#endif

// Signalphasendauer
long onPhaseUm = 0;
long onPhaseDauer = 0;
bool phaseFlag = false;
bool onPhaseFlag = false;
long offPhaseStartUm = 0; // OFF Phase ist LED aus == Markierung Scheibe
long offPhaseDauer = 0;
bool offPhaseFlag = false;

unsigned long lastTimeSync = 0;

// Fragt die aktuelle Zeit per NTP ab (blockiert bis zu 10s).
void syncTime()
{
  configTzTime(TZ_INFO, NTP_SERVER);
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10000))
  {
    char buf[20];
    strftime(buf, sizeof(buf), TIME_FORMAT, &timeinfo);
    LOG_PRINT("Zeit synchronisiert: ");
    LOG_PRINTLN(buf);
  }
  else
  {
    LOG_PRINTLN("NTP-Zeit konnte nicht ermittelt werden");
  }
}

void setup_wifi()
{
  delay(10);

#ifdef WOKWI_SIM
  // Echte Zugangsdaten aus config.h existieren in der Simulation nicht;
  // Wokwi stellt dafuer ein offenes Netz "Wokwi-GUEST" bereit.
  const char *ssid = "Wokwi-GUEST";
  const char *psk = "";
#else
  const char *ssid = SSID;
  const char *psk = PSK;
#endif

  LOG_PRINT("\nConnecting to ");
  LOG_PRINTLN(ssid);

  WiFi.begin(ssid, psk);

#ifdef WOKWI_SIM
  // Timeout in der Simulation: loop() soll auch starten, wenn das
  // simulierte WLAN nicht (rechtzeitig) verbindet - der HTTP-Versand
  // ist unten ohnehin schon durch WiFi.status()==WL_CONNECTED abgesichert.
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20)
  {
    pinMode(LED_BUILTIN, led_buildin_onoff);
    delay(500);
    LOG_PRINT(".");
    led_buildin_onoff = !led_buildin_onoff;
    wifiAttempts++;
  }
#else
  while (WiFi.status() != WL_CONNECTED)
  {
    pinMode(LED_BUILTIN, led_buildin_onoff);
    delay(500);
    LOG_PRINT(".");
    led_buildin_onoff = !led_buildin_onoff;
  }
#endif

  if (WiFi.status() == WL_CONNECTED)
  {
    digitalWrite(LED_BUILTIN, LOW); // Signal WiFi on 1st builtin LED
    LOG_PRINTLN("  WiFi connected");
    LOG_PRINT("IP address: ");
    LOG_PRINTLN(WiFi.localIP());
  }
  else
  {
    LOG_PRINTLN("  WiFi NICHT verbunden - fahre trotzdem fort");
  }
  pinMode(LED_BUILTIN, OFF);
  led_buildin_onoff = OFF; // init for WLAN LED Ticks
  wlanTick = 0;            // init for WLAN LED Ticks
  ArduinoOTA.begin();
}

void setup()
{
  Serial.begin(9600);
  LOG_PRINTLN("Starte...");
  setup_wifi();

  syncTime();
  lastTimeSync = millis();

  // Set IO modes
  pinMode(FERRARIS_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  // pinMode(LED_BUILTIN_NODE, OUTPUT);

#ifdef WOKWI_SIM
  pinMode(WIFI_ERROR_BUTTON_PIN, INPUT_PULLUP);
#endif
}

void loop()
{
  long now = millis();
  int val = digitalRead(FERRARIS_PIN); // first read the data for precision

  // snprintf(debugMsg, MSG_BUF_LEN, "ON:%ld OFF:%ld   %d", onPhaseDauer, offPhaseDauer, val);
  // Serial.println(debugMsg);

  // Periodischer Zeit-Resync (Intervall siehe TIME_SYNC_INTERVAL_MS)
  if (WiFi.status() == WL_CONNECTED && (unsigned long)(millis() - lastTimeSync) >= TIME_SYNC_INTERVAL_MS)
  {
    syncTime();
    lastTimeSync = millis();
  }

  // Geplanter naechtlicher Neustart: ab RESTART_HOUR Uhr, aber nur wenn der
  // Nachrichtenpuffer leer ist und die aktuelle Leistung unter RESTART_MAX_WATTS
  // liegt; hoechstens einmal pro Nacht (siehe getLastRestartDay()/RTC-Speicher).
  {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0) && timeinfo.tm_hour >= RESTART_HOUR && timeinfo.tm_yday != getLastRestartDay())
    {
      if (pendingMsgCount == 0 && lastWatts < RESTART_MAX_WATTS)
      {
        LOG_PRINTLN("Geplanter naechtlicher Neustart (Puffer leer, Leistung < 1000W)...");
        setLastRestartDay(timeinfo.tm_yday);
        delay(100); // Zeit fuer die letzte Log-Ausgabe
        ESP.restart();
      }
    }
  }


#define min_offPhase 100 // zum entprellen
  if (offPhaseDauer > min_offPhase)
  {

    if (val == SEND_ON && last_val == !SEND_ON) // == Wechsel von LED AUS auf AN
    {

      if (last_millis != -1)
      {
        long diff_millis = now - last_millis;
        pinMode(LED_BUILTIN, true);
        float watts = calculateWatts(val_ROTATION_WORTH_Wms, diff_millis);
        lastWatts = watts;

        PendingReading reading;
        reading.diffMillis = diff_millis;
        reading.watts = watts;
        reading.offPhase = offPhaseDauer;
        reading.onPhase = onPhaseDauer;
        reading.zeitpkt = time(nullptr);
        reading.errCount = wlan_error_counter;
        reading.lastResponseCode = httpResponseCode_lasterror;

        bool sent = false;
        bool discarded = false;

        if (WiFi.status() == WL_CONNECTED)
        {
          // Erst gepufferte Nachrichten nachholen, bevor der neue Wert gesendet wird
          flushPendingMessages();

          char msg[MSG_BUF_LEN];
          formatReading(msg, MSG_BUF_LEN, reading);

          int httpResponseCode = postMessage(msg);

          if (httpResponseCode == 200)
          {
            sent = true;
          }
          else if (httpResponseCode == HTTP_DISCARD)
          {
            discarded = true;
          }
          else
          {
            wlan_error_counter++;
            httpResponseCode_lasterror = httpResponseCode;
          }
        }

        if (!sent && !discarded)
        {
          reading.errCount = wlan_error_counter;
          reading.lastResponseCode = httpResponseCode_lasterror;
          bufferMessage(reading);
        }

        // Neu formatieren, damit ERR/RC den aktuellen Stand zeigen
        char logMsg[MSG_BUF_LEN];
        reading.errCount = wlan_error_counter;
        reading.lastResponseCode = httpResponseCode_lasterror;
        formatReading(logMsg, MSG_BUF_LEN, reading);
        LOG_PRINTLN(logMsg);

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