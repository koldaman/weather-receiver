#include "Constants.h"

#include <Wire.h>
#include <ESP8266WiFi.h>
#include <RCSwitch.h>
#include <SPI.h>
#include "CustomWiFiClient.h"
#include <Adafruit_BMP280.h>
#include <Blink.h>
#include <PinReader.h>
#include "DataColector.h"
#include <simpleDSTadjust.h>
#include <JsonListener.h>
#include "WundergroundClient.h"

#include <SSD1306Wire.h>
#include <OLEDDisplayUi.h>
#include "WeatherStationFonts.h"
#include "WeatherStationImages.h"

#include <PubSubClient.h>

#define RX_PIN D6

#define SENSORDATA_JSON_SIZE (JSON_OBJECT_SIZE(4))

const uint8_t BUFFER_SIZE = 32;
uint8_t buffer[BUFFER_SIZE];

#define MAX_DATA_SIZE 32

Blink blinker(D0);

const int UPDATE_INTERVAL_SECS = 60 * 60; // Update time and weather (every 1 hour)

const long READING_INTERVAL = 15 * 60 * 1000; // sensor sending interval (15 minutes)

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

// Wunderground Settings
const boolean IS_METRIC = true;
const String WUNDERGRROUND_LANGUAGE = "EN"; // CZ
const String WUNDERGROUND_ZMW_CODE = "00000.2.11538";

// Initialize Wunderground client with METRIC setting
WundergroundClient wunderground(IS_METRIC);

Ticker tickerUpdateData;

bool readyForUpdate = false;
unsigned long dataReceived = 0;  // last time data was received
unsigned long dataReceivedUiTime = 100; // how long should be data receiving indicated (ms)

void drawDateTime(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawCurrentWeather(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawGraph(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawForecast(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawHeaderOverlay(OLEDDisplay *display, OLEDDisplayUiState* state);

FrameCallback frames[] = { drawDateTime, drawCurrentWeather, drawForecast, drawGraph };
int numberOfFrames = 4;

OverlayCallback overlays[] = { drawHeaderOverlay };
int numberOfOverlays = 1;

DataColector pressureDataColector;
DataColector tempDataColector;

RCSwitch mySwitch = RCSwitch();

WiFiClient espClient;
PubSubClient mqttClient(espClient);
IPAddress mqttServer(10, 10, 10, 20);
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

bool wifiConnect() {
  int timeout = 100;
  Serial.print(F("Connecting to "));
  Serial.println(Constants::SSID());
  drawProgress(&display, 0, F("Connecting to WiFi..."));
  
  WiFi.hostname("ESPWeather");

  WiFi.begin(Constants::SSID(), Constants::WIFI_PW());

  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.print(F("."));
    drawProgress(&display, 100-timeout, F("Connecting to WiFi..."));
    timeout--;
    if (timeout <= 0) {
      Serial.println(F("WiFi connect timeout!"));
      drawProgress(&display, 100, F("WiFi connect timeout"));
      return false;
    }
  }
  Serial.println();
  Serial.println(F("WiFi connected!"));
  Serial.print(F("IP Address: "));
  Serial.println(WiFi.localIP());
  drawProgress(&display, 100, F("WiFi connected"));
  return true;
}

void mqttReconnect() {
  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - mqttLastReconnectAttempt > 5000) {
      mqttLastReconnectAttempt = now;
      // Attempt to reconnect
      //drawProgress(&display, 50, F("Connecting to MQTT..."));
      Serial.print(F("Attempting MQTT connection..."));
      // Attempt to connect
      if (mqttClient.connect("ESP8266Client")) {
        Serial.println("connected");
        mqttLastReconnectAttempt = 0;
      } else {
        Serial.print("failed, rc=");
        Serial.print(mqttClient.state());
        Serial.println(" try again in 5 seconds");
      }
    }
  } else {
    // Client connected
    mqttClient.loop();
  }
}

void getInsideTempData(MyData& data) {
//  Serial.print(bme.readTemperature());
//  Serial.print(bme.readAltitude(1020.4)); // this should be adjusted to your local forcase
  float temp = bme.readTemperature();
  float pressure = bme.readPressure();
  pressure /= 100; // convert to hPa
  data.inTemp = temp;
  data.pressure = relPressure(data.outTemp, pressure);
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
  // MQTT
  if (mqttClient.connected()) {
    String m = "temp,meter=outside,dev=1 value=";
    m += data.outTemp;
    mqttClient.publish("influx/weather/temp", m.c_str());

    m = "temp,meter=inside,dev=2 value=";
    m += data.inTemp;
    mqttClient.publish("influx/weather/temp", m.c_str());
    
    m = "hum,meter=outside,dev=1 value=";
    m += data.hum;
    mqttClient.publish("influx/weather/hum", m.c_str());

    m = "press,meter=outside,dev=2 value=";
    m += data.pressure;
    mqttClient.publish("influx/weather/press", m.c_str());
    
    m = "vcc,meter=outside,dev=1 value=";
    m += data.vcc;
    mqttClient.publish("influx/weather/vcc", m.c_str());
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

unsigned long deserializeRadioData(MyData& data, unsigned long value) {
  Serial.print(F("Received data: "));
  Serial.println(value);

  if (value == 0) {
    Serial.println(F("Unknown data received"));
  } else if (value <= 256) {
	  Serial.print(F("Received packetId data: "));
	  Serial.println(value);
	  data.packetId = value;
  } else { // parsing weather data
	  int rt = value / 1000000;
	  int rh = (value - (rt *1000000)) / 1000;
	  int rVcc = value - (rt *1000000) - (rh * 1000);
	  float tDecoded = (rt - 500) / 10.0;
	  float hDecoded = rh / 10.0;
	  float vccDecoded = rVcc / 100.0;
	
	  data.outTemp = tDecoded;
	  data.hum = hDecoded;
	  data.vcc = vccDecoded;
	
	  data.lastUpdate = millis();
	
	  Serial.print(F("Received weather data: "));
    Serial.print(F(" outT="));
    Serial.print(data.outTemp);
    Serial.print(F(" h="));
    Serial.print(data.hum);
    Serial.print(F(" vcc="));
    Serial.println(data.vcc);
  }
  
  return value;
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
  mySwitch.enableReceive(RX_PIN);
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
  for (int i = 0; i < 50; i++) { // 5 secs timeout checking for connection
    if (WiFi.status() != WL_CONNECTED) {
      delay(100);
      continue;
    }
    return; // connected
  }

  ESP.reset(); // restart
  delay(1000);
}

void checkMqttConnected() {
  if (!mqttClient.connected()) {
    mqttReconnect();
  }
}

void checkDataReceived() {
  if (mySwitch.available()) {
    setDataReceived(&display, millis());
    Serial.println(F("Message received..."));

    unsigned long value = mySwitch.getReceivedValue();
    
    if (value == 0) {
      Serial.print("Unknown encoding");
    } else {
      Serial.print("Received ");
      Serial.print( mySwitch.getReceivedValue() );
      Serial.print(" / ");
      Serial.print( mySwitch.getReceivedBitlength() );
      Serial.print("bit ");
      Serial.print("Protocol: ");
      Serial.println( mySwitch.getReceivedProtocol() );
    }

    unsigned long result = deserializeRadioData(lastData, value); 
    if (result > 256) {
      getInsideTempData(lastData);

      getTimestamp(lastData);
	  
      Serial.print(F("Updated internal data: "));
      Serial.print(F(" inT="));
      Serial.print(lastData.inTemp);
      Serial.print(F(" p="));
      Serial.print(lastData.pressure);
      Serial.print(F(" ts="));
      Serial.println(lastData.timestamp);
      
      if (lastData.sent && ((lastData.sentCount+1) % 2 == 0)) { // log every other data to extent history to 30hours back (for graph)
        tempDataColector.add((int)(lastData.outTemp * 100));
        pressureDataColector.add((int)(lastData.pressure * 100));
        Serial.print(F("Data logged to internal history."));
      } else {
        Serial.print(F("Data not logged to internal history."));
      }

      Serial.print(F("Temperature collection: "));
      tempDataColector.print();
      Serial.print(F("Pressure collection: "));
      pressureDataColector.print();

      lastData.sent = false;
      
    } else if (result == 0) {
      drawProgress(&display, 100, F("Failed receiving message, unknown message format"));
      Serial.println(F("FAIL"));
    }

    mySwitch.resetAvailable();
  }
}

void setup() {
  Serial.begin(115200);

  blinker.stop();

  setupDisplay();
  setupUi();

//  CustomWiFiManager::start(&blinker);

  if (!wifiConnect()) {
    ESP.reset(); // restart
    delay(1000);
  }

  mqttClient.setServer(mqttServer, mqttPort);
  
  setupRadio();

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
  checkDataReceived();
  
  checkWifiConnected();
  checkMqttConnected();

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
      checkDataReceived();
    }

    delay(remainingTimeBudget);
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
  drawProgress(display, 30, F("Updating time..."));
  configTime(UTC_OFFSET * 3600, 0, NTP_SERVERS);
  drawProgress(display, 60, F("Updating conditions..."));
  wunderground.updateConditions(Constants::WUNDERGRROUND_API_KEY(), WUNDERGRROUND_LANGUAGE, WUNDERGROUND_ZMW_CODE);
  drawProgress(display, 80, F("Updating forecasts..."));
  wunderground.updateForecastZMW(Constants::WUNDERGRROUND_API_KEY(), WUNDERGRROUND_LANGUAGE, WUNDERGROUND_ZMW_CODE);
  readyForUpdate = false;
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

  display->drawString(64 + x, 2 + y, desc_str);

  display->setFont(ArialMT_Plain_24);
  char temp_str[8];
  sprintf(temp_str, "%d.%1d°C\n", (int)lastData.outTemp, abs((int)(lastData.outTemp*10)%10));
  display->drawString(64 + x, 16 + y, temp_str);
}

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

void drawForecast(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  drawForecastDetails(display, x, y, 0);
  drawForecastDetails(display, x + 44, y, 2);
  drawForecastDetails(display, x + 88, y, 4);
}

void drawForecastDetails(OLEDDisplay *display, int x, int y, int dayIndex) {
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_10);
  String day = wunderground.getForecastTitle(dayIndex).substring(0, 3);
  day.toUpperCase();
  display->drawString(x + 20, y, day);

  display->setFont(Meteocons_Plain_21);
  display->drawString(x + 20, y + 12, wunderground.getForecastIcon(dayIndex));

  display->setFont(ArialMT_Plain_10);
  display->drawString(x + 20, y + 34, wunderground.getForecastLowTemp(dayIndex) + "|" + wunderground.getForecastHighTemp(dayIndex));
  display->setTextAlignment(TEXT_ALIGN_LEFT);
}

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

  drawPressureIcon(display, 60, 60);
  
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
