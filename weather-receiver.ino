#include "Constants.h"

#include <Wire.h>
#include <ESP8266WiFi.h>
//#include <RCSwitch.h>
#include <SPI.h>
#include "CustomWiFiClient.h"
#include <Adafruit_BMP280.h>
#include <Blink.h>
#include <PinReader.h>
//#include "DataColector.h"
#include <simpleDSTadjust.h>
#include <JsonListener.h>
#include "OpenWeatherMapCurrent.h"
#include "OpenWeatherMapForecast.h"
#include <ArduinoJson.h>
#include <SSD1306Wire.h>
#include <OLEDDisplayUi.h>
#include "WeatherStationFonts.h"
#include "WeatherStationImages.h"

#define USE_ARDUINO_OTA true

#ifdef USE_ARDUINO_OTA
#include <ArduinoOTA.h>
#endif

#include <MQTT.h>

#define HOSTNAME "nodemcu-weather-station"

#define TOPIC_TEMP "influx/weather/temp"
#define TOPIC_HUM "influx/weather/hum"
#define TOPIC_LIGHT "influx/weather/light"
#define TOPIC_PRESS "influx/weather/press"
#define TOPIC_VCC "influx/energy/vcc"
#define TOPIC_DOOR "influx/home/door"
#define TOPIC_BELL "influx/home/bell"

#define RX_PIN D6

Blink blinker(D0);

const int UPDATE_INTERVAL_SECS = 60 * 60; // Update time and weather (every 1 hour)

const long READING_INTERVAL = 30 * 60 * 1000; // sensor sending interval (30 minutes)

// Display Settings
const int I2C_DISPLAY_ADDRESS = 0x3c;

// Initialize the oled display for address 0x3c
SSD1306Wire     display(I2C_DISPLAY_ADDRESS, D2, D1);
OLEDDisplayUi   ui( &display );

// TimeClient settings
#define NTP_SERVERS "us.pool.ntp.org", "pool.ntp.org", "time.nist.gov"
#define UTC_OFFSET +1

struct dstRule StartRule = {"CEST", Last, Sun, Mar, 2, 3600}; // Central European Summer Time = UTC/GMT +2 hours
struct dstRule EndRule = {"CET", Last, Sun, Oct, 2, 0};       // Central European Time = UTC/GMT +1 hour

// Setup simpleDSTadjust Library rules
simpleDSTadjust dstAdjusted(StartRule, EndRule);

PinReader buttonReader(D3, 50);  // switch to toggle display frame changing
bool frameAutoTransition = true; // frame transition state

Adafruit_BMP280 bme; // I2C

String OPEN_WEATHER_MAP_LANGUAGE = "en"; // de | cz
const uint8_t MAX_FORECASTS = 4;

const boolean IS_METRIC = true;

// Adjust according to your language
//const String WDAY_NAMES[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const String WDAY_NAMES[] = {"NED", "PON", "UTE", "STR", "CTV", "PAT", "SOB"};
//const String MONTH_NAMES[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
const String MONTH_NAMES[] = {"LED", "UNO", "BRE", "DUB", "KVE", "CER", "CRC", "SRP", "ZAR", "RIJ", "LIS", "PRO"};

// due to wierd indoor temparature measurement 
const float inTempCorrection = -2.4;

OpenWeatherMapCurrentData currentWeather;
OpenWeatherMapCurrent currentWeatherClient;

OpenWeatherMapForecastData forecasts[MAX_FORECASTS];
OpenWeatherMapForecast forecastClient;

Ticker tickerUpdateData;

bool readyForUpdate = false;
unsigned long dataReceived = 0;  // last time data was received
unsigned long dataReceivedUiTime = 200; // how long should be data receiving indicated (ms)

void drawDateTime(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawCurrentWeather(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
//void drawGraph(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawForecast(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawHeaderOverlay(OLEDDisplay *display, OLEDDisplayUiState* state);

//FrameCallback frames[] = { drawDateTime, drawCurrentWeather, drawForecast, drawGraph };
FrameCallback frames[] = { drawDateTime, drawCurrentWeather, drawForecast };

int numberOfFrames = 3;

OverlayCallback overlays[] = { drawHeaderOverlay };
int numberOfOverlays = 1;

//DataColector pressureDataColector;
//DataColector tempDataColector;

//RCSwitch mySwitch = RCSwitch();

WiFiClient espClient;
MQTTClient mqttClient;
const char * mqttServer = "10.10.10.20";
const int mqttPort = 1883;
unsigned long mqttLastReconnectAttempt = 0;

struct MyData {
  const char * meterId;
  byte packetId;
  float outTemp;
  float inTemp;
  float hum;
  float pressure;
  float vcc;
  String timestamp;
  unsigned long lastUpdate;
  unsigned long lastSent;         // last time data was sent to cloud
  unsigned long sentCount = 0;    // counting data sent
  bool sent;
};

MyData lastData;

unsigned long lastRefreshUi = 0;
const unsigned long sendingInterval = 1000 * 60 * 15;  // send to cloud every 15 minutes 

unsigned long doorOpened = 0;
unsigned long doorClosed = 0;
const unsigned long doorClosedHideTime = 1000 * 10;  // hide notification after 10 secs
unsigned long doorBellRing = 0;
const unsigned long doorBellRingHideTime = 1000 * 10;  // hide notification after 10 secs

bool wifiConnect() {
  int timeout = 100;
  Serial.print(F("Connecting to "));
  Serial.println(Constants::SSID());
  drawProgress(&display, 0, F("Connecting to WiFi..."));
  
  WiFi.hostname(HOSTNAME);

  WiFi.begin(Constants::SSID(), Constants::WIFI_PW());

  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.print(".");
    drawProgress(&display, 100-timeout, F("Connecting to WiFi..."));
    timeout--;
    if (timeout <= 0) {
      Serial.println("WiFi connect timeout!");
      drawProgress(&display, 100, F("WiFi connect timeout"));
      return false;
    }
  }
  Serial.println();
  Serial.println(F("WiFi connected!"));
  Serial.print(F("IP Address: "));
  Serial.println(WiFi.localIP().toString().c_str());
  drawProgress(&display, 100, F("WiFi connected"));
  return true;
}

void mqttReconnect() {
  Serial.print("MQTT connecting...");
  while (!mqttClient.connect(HOSTNAME)) {
    Serial.print(".");
    delay(100);
  }

  Serial.println();
  Serial.println("MQTT subscribing topics...");
  
  mqttClient.subscribe(TOPIC_TEMP);
  mqttClient.subscribe(TOPIC_HUM);
  mqttClient.subscribe(TOPIC_PRESS);
  mqttClient.subscribe(TOPIC_LIGHT);
  mqttClient.subscribe(TOPIC_VCC);
  mqttClient.subscribe(TOPIC_DOOR);
  mqttClient.subscribe(TOPIC_BELL);

  Serial.println("MQTT connected!");
}

void mqttCallback(String &topic, String &payload) {
  String msg = payload;
  Serial.print(F("Message arrived ["));
  Serial.print(topic);
  Serial.print(F("]: "));
  Serial.println(msg);

  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, msg);
//  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& root = doc.as<JsonObject>();
//  JsonObject& root = jsonBuffer.parseObject(msg);

  // Test if parsing succeeds.
  //if (!root.success()) {
  if (error) {
    Serial.println(F("parseObject() failed"));
    return;
  }

  String topicJson = root["topic"];
  String meterJson = root["meter"];
  String devJson = root["dev"];
  double valueJson = root["value"];

  Serial.print(F("Topic: "));
  Serial.print(topicJson);
  Serial.print(F(", Meter: "));
  Serial.print(meterJson);
  Serial.print(F(", Device: "));
  Serial.print(devJson);
  Serial.print(F(", Value: "));
  Serial.println(valueJson);

  if (meterJson == "home" && topicJson == "door") {
    if (valueJson > 0.0) {
      doorOpened = millis();
      doorClosed = 0;
    } else {
      doorOpened = 0;
      doorClosed = millis();
    }
  }

  if (meterJson == "home" && topicJson == "bell") {
    if (valueJson > 0.0) {
      doorBellRing = millis();
    }
  }
  
  unsigned long originalLastUpdate = lastData.lastUpdate; 
  if (topicJson == "weather" && devJson == "wemos-weather") {
    if (meterJson == "temp") {
      lastData.outTemp = valueJson;
      lastData.lastUpdate = millis();
    }
    if (meterJson == "hum") {
      lastData.hum = valueJson;
      lastData.lastUpdate = millis();
    }
    if (meterJson == "press") {
      lastData.pressure = valueJson;
      lastData.lastUpdate = millis();
    }
  } else if (topicJson == "energy" && devJson == "wemos-weather") {
    if (meterJson == "vcc") {
      lastData.vcc = valueJson;
      lastData.lastUpdate = millis();
    }
  }

  if (originalLastUpdate == lastData.lastUpdate) {
    // no appropriate data received
    Serial.println(F("No data to collect"));
    return;
  }

  setDataReceived(&display, millis());

  getInsideTempData(lastData);
  getTimestamp(lastData);

  Serial.print(F("Current data: outT="));
  Serial.print(lastData.outTemp);
  Serial.print(F(", inT="));
  Serial.print(lastData.inTemp);
  Serial.print(F(", outHum="));
  Serial.print(lastData.hum);
  Serial.print(F(", press="));
  Serial.print(lastData.pressure);
  Serial.print(F(", vcc="));
  Serial.print(lastData.vcc);
  Serial.print(F(", ts="));
  Serial.println(lastData.timestamp.c_str());
/*
  if (lastData.sent && ((lastData.sentCount+1) % 2 == 0)) { // log every other data to extent history to 30hours back (for graph)
    tempDataColector.add((int)(lastData.outTemp * 100));
    pressureDataColector.add((int)(lastData.pressure * 100));
    Serial.println(F("Data logged to internal history."));
  } else {
    Serial.println(F("Data not logged to internal history."));
  }
*/
  lastData.sent = false;
}

void getInsideTempData(MyData& data) {
//  Serial.print(bme.readTemperature());
//  Serial.print(bme.readAltitude(1020.4)); // this should be adjusted to your local forcase
  float temp = bme.readTemperature() + inTempCorrection;
  float pressure = bme.readPressure();
  pressure /= 100; // convert to hPa
  data.inTemp = temp;
//  data.pressure = relPressure(data.outTemp, pressure);
  data.pressure = pressure;
}

// prepocet na relativni tlak dle umisteni
float relPressure(float temp, float absPressure) {
  int nadmorskaVyska = 370;
  // http://forum.amaterskameteorologie.cz/viewtopic.php?f=9&t=65
  // ((986*9.80665*360)/(287*(273+10+(360/400))))+986
  return ((absPressure*9.80665*nadmorskaVyska)/(287*(273+temp+(nadmorskaVyska/400))))+absPressure;
}

void getTimestamp(MyData& data) {
  char time_str[11];
  time_t now = dstAdjusted.time(nullptr);
  struct tm * timeinfo = localtime (&now);
  sprintf(time_str, "%02d:%02d:%02d\n",timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
  data.timestamp = time_str;
}

void send(MyData& data) {
  getInsideTempData(lastData);
  getTimestamp(lastData);
  
  // MQTT
  if (mqttClient.connected()) {
    mqttPublish(TOPIC_TEMP, createJson("weather", "temp", data.inTemp));
    mqttPublish(TOPIC_PRESS, createJson("weather", "press", data.pressure));
  }
  // HTTP client
  CustomWiFiClient client;
  client.sentCallback(handleDataSent);
  client.sendData(data.packetId, data.outTemp, data.inTemp, data.hum, data.pressure, data.vcc);
}

void handleDataSent(int httpResult) {
   Serial.print(F("HttpResult: "));
   Serial.print(httpResult);
   if (httpResult >= 200 && httpResult < 300) {
      Serial.println(F(" - OK"));
      blinker.init({10}, 1);
      lastData.sent = true;
      lastData.lastSent = millis();
      lastData.sentCount++;
   } else {
      drawProgress(&display, 100, F("Connection to server FAILED"));
      Serial.println(F(" - connecting to server FAILED"));
      blinker.init({10,50,10,50,10}, 1);
   }
   blinker.start();
}

void handleButtonSwitch(int oldValue, int newValue) {
  if (newValue == LOW) {
    if (frameAutoTransition) {
      ui.disableAutoTransition();
      Serial.println(F("Disabling autotransition"));
    } else {
      ui.enableAutoTransition();
      Serial.println(F("Enabling autotransition"));
    }
    frameAutoTransition = !frameAutoTransition;
    display.display();
  }
}

void setupRadio() {
//  mySwitch.enableReceive(RX_PIN);
}

void setupDisplay() {
  display.init();
  display.clear();
  display.flipScreenVertically();  // Comment out to flip display 180deg
  display.display();

  //display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setContrast(255);
}

void setupUi() {
  ui.setTargetFPS(30);

  //Hack until disableIndicator works:
  //Set an empty symbol
  ui.setActiveSymbol(emptySymbol);
  ui.setInactiveSymbol(emptySymbol);

  ui.disableIndicator();

  // You can change the transition that is used
  // SLIDE_LEFT, SLIDE_RIGHT, SLIDE_TOP, SLIDE_DOWN
  ui.setFrameAnimation(SLIDE_LEFT);

  ui.setFrames(frames, numberOfFrames);

  ui.setOverlays(overlays, numberOfOverlays);

  // Inital UI takes care of initalising the display too.
  ui.init();

  if (!frameAutoTransition) {
    ui.disableAutoTransition();
  }
//  ui.switchToFrame(0);
}

void checkWifiConnected() {
  // restart if wifi not connected for some time
  for (int i = 0; i < 50; i++) {
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiConnect()) {
        return;
      }
    } else {
      return;
    }
  }

  ESP.reset(); // restart
  delay(1000);
}

void checkMqttConnected() {
  mqttClient.loop();
  delay(10);  // <- fixes some issues with WiFi stability
  if (!mqttClient.connected()) {
    mqttReconnect();
  }
}

String createJson(String topic, String meter, float value) {
  // {"topic" : "weather", "meter" : "temp", "dev" : "wemos-weather", "value" : 0.01}
  String result = "{\"topic\" : \"";
  result += topic;
  result += "\", \"meter\" : \"";
  result += meter;
  result += "\", \"dev\" : \"";
  result += HOSTNAME;
  result += "\", \"value\" : ";
  result += value;
  result += "}";
  return result;
}

bool mqttPublish(char *topic, String msg) {
  if (mqttClient.connected()) {
    mqttClient.publish(topic, msg.c_str());
    Serial.print(F("MQTT data sent: "));
    Serial.println(msg.c_str());
  }
}

void setupOTA() {
  String hostname(HOSTNAME);
  ArduinoOTA.setHostname((const char *)hostname.c_str());
  ArduinoOTA.begin();
}

void setup() {
  Serial.begin(115200);

  blinker.stop();

 #ifdef USE_ARDUINO_OTA
    setupOTA();
  #endif
  
  setupDisplay();
  setupUi();

//  CustomWiFiManager::start(&blinker);

  wifiConnect();

  mqttClient.begin(mqttServer, mqttPort, espClient);
  mqttClient.onMessage(mqttCallback);
  
//  setupRadio();

  if (!bme.begin()) {  
    Serial.println(F("Could not find a valid BMP280 sensor, check wiring!"));
    ESP.reset();
    delay(1000);
  }

  buttonReader.init();
  buttonReader.setCallback(handleButtonSwitch);
  
  Serial.println(F("Setup done"));

  updateData(&display);

  tickerUpdateData.attach(UPDATE_INTERVAL_SECS, setReadyForUpdate);

  lastRefreshUi = millis();
}

void loop(void) {
  checkWifiConnected();
  checkMqttConnected();

  #ifdef USE_ARDUINO_OTA
    ArduinoOTA.handle();
  #endif
  
  yield();

  buttonReader.monitorChanges();
  
  if (readyForUpdate && ui.getUiState()->frameState == FIXED) {
    updateData(&display);
  }

  if (!lastData.sent && millis() - lastData.lastSent > sendingInterval) {
    send(lastData);
  }

  lastRefreshUi = millis();
  int remainingTimeBudget = ui.update();

  if (remainingTimeBudget > 0) {
    // You can do some work here
    // Don't do stuff if you are below your
    // time budget.

    if (remainingTimeBudget > 10) {
//      if (!mqttClient.loop()) {
//        mqttReconnect();
//      }
    }

//    delay(remainingTimeBudget);
  }

}

// ------------------------ FRAMES --------------------------

void drawProgress(OLEDDisplay *display, int percentage, String label) {
  display->clear();
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_10);
  display->drawString(64, 10, label);
  display->drawProgressBar(2, 28, 124, 10, percentage);
  display->flipScreenVertically();  // Comment out to flip display 180deg
  display->display();
}

void updateData(OLEDDisplay *display) {
  Serial.println(F("Updating internet data (time, weather)..."));
  drawProgress(display, 30, F("Updating time..."));
  configTime(UTC_OFFSET * 3600, 0, NTP_SERVERS);
  drawProgress(display, 60, F("Updating weather..."));
  currentWeatherClient.setMetric(IS_METRIC);
  currentWeatherClient.setLanguage(OPEN_WEATHER_MAP_LANGUAGE);
  currentWeatherClient.updateCurrentById(&currentWeather, Constants::OPEN_WEATHER_MAP_APP_ID(), Constants::OPEN_WEATHER_MAP_LOCATION_ID());
  drawProgress(display, 80, F("Updating forecasts..."));
  forecastClient.setMetric(IS_METRIC);
  forecastClient.setLanguage(OPEN_WEATHER_MAP_LANGUAGE);
  uint8_t allowedHours[] = {12};
  forecastClient.setAllowedHours(allowedHours, sizeof(allowedHours));
  forecastClient.updateForecastsById(forecasts, Constants::OPEN_WEATHER_MAP_APP_ID(), Constants::OPEN_WEATHER_MAP_LOCATION_ID(), MAX_FORECASTS);
  readyForUpdate = false;
  Serial.println(F("Updating internet data - DONE"));
}

void drawDateTime(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  char time_str[11];
  char date_str[11];
  time_t now = dstAdjusted.time(nullptr);
  struct tm * timeinfo = localtime (&now);

  sprintf(time_str, "%02d:%02d:%02d\n",timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
  sprintf(date_str, "%01d.%01d.%4d\n",timeinfo->tm_mday, timeinfo->tm_mon+1, 1900+timeinfo->tm_year);
  
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_10);
  display->drawString(64 + x, 5 + y, date_str);
  display->setFont(ArialMT_Plain_24);
  
  display->drawString(64 + x, 15 + y, time_str);
  display->setTextAlignment(TEXT_ALIGN_LEFT);
}

void drawCurrentWeather(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_CENTER);

  char desc_str[32];
  sprintf(desc_str, "%d.%1d°C  %d%%  %dhP  %d.%1dV\n", 
      (int)lastData.inTemp, abs((int)(lastData.inTemp*10)%10),
      (int)lastData.hum, 
      (int)lastData.pressure, 
      (int)lastData.vcc, abs((int)(lastData.vcc*10)%10) 
  );

  display->drawString(64 + x, 1 + y, desc_str);

  display->setFont(ArialMT_Plain_24);
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  char temp_str[8];
//  sprintf(temp_str, "%d.%1d°C\n", (int)lastData.outTemp, abs((int)(lastData.outTemp*10)%10));
  sprintf(temp_str, "%d.%1d\n", (int)lastData.outTemp, abs((int)(lastData.outTemp*10)%10));
  display->drawString(1 + x, 16 + y, temp_str);

  // data from forecast
  display->setFont(ArialMT_Plain_16);
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  String temp = String(currentWeather.temp, 1) + (IS_METRIC ? "°C" : "°F");
  display->drawString(128 + x, 18 + y, temp);

  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->drawString(110 + x, 36 + y, currentWeather.description);

  display->setFont(Meteocons_Plain_21);
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->drawString(132 + x, 34 + y, currentWeather.iconMeteoCon);  
}
/*
void drawGraph(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setFont(ArialMT_Plain_10);

  // pressure texts
  char press_min_str[7];
  char press_max_str[7];
  sprintf(press_min_str, "%d\n", (int)pressureDataColector.getMin()/100);
  sprintf(press_max_str, "%d\n", (int)pressureDataColector.getMax()/100);

  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(8 + x, 2 + y, press_max_str);
  display->drawString(8 + x, 35 + y, press_min_str);

  // temp texts
  char temp_min_str[12];
  char temp_max_str[12];
  sprintf(temp_min_str, "%d.%1d°C\n", (int)tempDataColector.getMin()/100, abs((int)(tempDataColector.getMin()/10)%10));
  sprintf(temp_max_str, "%d.%1d°C\n", (int)tempDataColector.getMax()/100, abs((int)(tempDataColector.getMax()/10)%10));

  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->drawString(120 + x, 2 + y, temp_max_str);
  display->drawString(120 + x, 35 + y, temp_min_str);

  // osa y
  display->drawLine(3 + x, 3 + y, 3 + x, 50 + y);
  display->drawLine(4 + x, 3 + y, 4 + x, 50 + y);

  // osa x
  display->drawLine(1 + x, 47 + y, 125 + x, 47 + y);
  display->drawLine(1 + x, 48 + y, 125 + x, 48 + y);

  int offset = 5;

  // draw preesure graph (lines)
  int * pressData = pressureDataColector.getData();
  int pressMin = pressureDataColector.getMin() - 100;
  int pressMax = pressureDataColector.getMax() + 100;

  if (pressData[0] == 0) { // no data
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(64 + x, 20 + y, "No data");
    return;
  }

  int lastValue = map(pressData[0], pressMin, pressMax, 46, 5);
  for (int i = 1; i < pressureDataColector.getSize(); i++) {
    if (pressData[i] == 0) {
      break;
    }
    int value = map(pressData[i], pressMin, pressMax, 46, 5);
    display->drawLine(offset + x + (i*2), lastValue, offset + x + ((i+1)*2), value);
    lastValue = value;
  }

  // draw temp graph (dots)
  int * tempData = tempDataColector.getData();
  int tempMin = tempDataColector.getMin() - 100;
  int tempMax = tempDataColector.getMax() + 100;

  for (int i = 1; i < tempDataColector.getSize(); i++) {
    if (tempData[i] == 0) {
      break;
    }
    int value = map(tempData[i], tempMin, tempMax, 46, 5);
    display->setPixel(offset + x + (i*2), value);
  }
  
//  free(data);
}
*/
void drawForecast(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  drawForecastDetails(display, x, y, 0);
  drawForecastDetails(display, x + 44, y, 1);
  drawForecastDetails(display, x + 88, y, 2);
}

void drawForecastDetails(OLEDDisplay *display, int x, int y, int dayIndex) {
  time_t observationTimestamp = forecasts[dayIndex].observationTime;
  struct tm* timeInfo;
  timeInfo = localtime(&observationTimestamp);
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_10);
  display->drawString(x + 20, y, WDAY_NAMES[timeInfo->tm_wday]);

  display->setFont(Meteocons_Plain_21);
  display->drawString(x + 20, y + 12, forecasts[dayIndex].iconMeteoCon);
  String temp = String(forecasts[dayIndex].temp, 0) + (IS_METRIC ? "°C" : "°F");
  display->setFont(ArialMT_Plain_10);
  display->drawString(x + 20, y + 34, temp);
  display->setTextAlignment(TEXT_ALIGN_LEFT);
}

void drawHomeOverlay(OLEDDisplay *display, int16_t x, int16_t y) {
  bool doorO = doorOpened > 0;
  bool doorC = doorClosed > 0 && millis() - doorClosed < doorClosedHideTime;
  bool doorBell = doorBellRing > 0 && millis() - doorBellRing < doorBellRingHideTime;

  if (doorO || doorC || doorBell) {
    // clear screen
    display->setColor(BLACK);
    display->fillRect(0, 0, display->getWidth(), display->getHeight());
  }
  
  if (doorO) {
    overlayText(display, x, doorBell ? y+5 : y+15, "OTEVRENO!");
  } else if (doorC) {
    overlayText(display, x, doorBell ? y+5 : y+15, "ZAVRENO");
  } else {
    doorOpened = 0;
    doorClosed = 0;
  }

  if (doorBell) {
    overlayText(display, x, doorO || doorC ? y+30 : y+15, "ZVONEK!");
  } else {
    doorBellRing = 0;
  }
}

void overlayText(OLEDDisplay *display, int16_t x, int16_t y, char * text) {
  display->setColor(WHITE);
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_16);

  display->drawString(x, y, text);
}
/*
void drawPressureIcon(OLEDDisplay *display, int16_t x, int16_t y) {
  display->setColor(WHITE);
  if (pressureDataColector.isAscending()) {
    // stoupajici tlak
    display->drawLine(x,     y + 3,  5 + x, y - 3);
    display->drawLine(x + 1, y + 3,  5 + x, y - 2);
    display->drawLine(5 + x, y - 3, 10 + x, y + 3);
    display->drawLine(5 + x, y - 2,  9 + x, y + 3);
  } else if (pressureDataColector.isDescending()) {
    // klesajici tlak
    display->drawLine(x,     y - 3,  5 + x, y + 3);
    display->drawLine(x + 1, y - 3,  5 + x, y + 2);
    display->drawLine(5 + x, y + 3, 10 + x, y - 3);
    display->drawLine(5 + x, y + 2,  9 + x, y - 3);
  } else if (!pressureDataColector.isAscending() && !pressureDataColector.isDescending()) {
    // nakresli vodorovnou caru - zadne stoupani ani klesani tlaku
    display->drawHorizontalLine(x, y, 10);
    display->drawHorizontalLine(x, 1+y, 10);
  }
}
*/
void drawLastUpdate(OLEDDisplay *display, int16_t x, int16_t y) {
  display->setColor(WHITE);
  long lastUpdateX = map(millis() - lastData.lastUpdate, 0, READING_INTERVAL, 0, 128); // 0 - 15min -> 0 - 128px
  display->fillCircle(lastUpdateX, y, 2);
}

void drawDataReceived(OLEDDisplay *display, int16_t x, int16_t y) {
  if (millis() - dataReceived  < dataReceivedUiTime) {
    display->setColor(WHITE);
    display->setFont(ArialMT_Plain_10);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(x, y, "...");
  }
}

void drawHeaderOverlay(OLEDDisplay *display, OLEDDisplayUiState* state) {
  display->setColor(WHITE);
  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(0, 54, String(state->currentFrame + 1) + "/" + String(numberOfFrames));

  char time_str[11];
  time_t now = dstAdjusted.time(nullptr);
  struct tm * timeinfo = localtime (&now);
  
  sprintf(time_str, "%02d:%02d\n",timeinfo->tm_hour, timeinfo->tm_min);
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->drawString(35, 54, time_str);

  display->setTextAlignment(TEXT_ALIGN_CENTER);

  if (millis() - dataReceived > (dataReceivedUiTime + 500)) {
    char temp_str[8];
    sprintf(temp_str, "%d.%1d°C\n", (int)lastData.outTemp, abs((int)(lastData.outTemp*10)%10));
    display->drawString(96, 54, temp_str);
  }

//  drawPressureIcon(display, 60, 60);
  
  int8_t quality = getWifiQuality();
  for (int8_t i = 0; i < 4; i++) {
    for (int8_t j = 0; j < 2 * (i + 1); j++) {
      if (quality > i * 25 || j == 0) {
        display->setPixel(120 + 2 * i, 63 - j);
      }
    }
  }

  drawLastUpdate(display, 0, 52);

  drawDataReceived(display, 96, 52);

  drawHomeOverlay(display, 64, 0);

  display->drawHorizontalLine(0, 52, 128);
}

// converts the dBm to a range between 0 and 100%
int8_t getWifiQuality() {
  int32_t dbm = WiFi.RSSI();
  if(dbm <= -100) {
      return 0;
  } else if(dbm >= -50) {
      return 100;
  } else {
      return 2 * (dbm + 100);
  }
}

void setReadyForUpdate() {
  Serial.println(F("Setting readyForUpdate to true"));
  readyForUpdate = true;
}

void setDataReceived(OLEDDisplay *display, unsigned long millisValue) {
  dataReceived = millisValue;
//  display->display();
}
