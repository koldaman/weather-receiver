/**************************************************************
  Zabaleni funkcionality pro WiFiClient (odesilani dat pres HTTPS)
 **************************************************************/
#include "Arduino.h"

#include <WiFiClient.h>
#include "CustomWiFiClient.h"
#include "Constants.h"

CustomWiFiClient::CustomWiFiClient() {
   WiFiClient _client;
   _host = "iot.e23.cz";
   _httpPort = 80;
   _auth = Constants::AUTH();
}

void CustomWiFiClient::sendData(byte packetId, float outTemp, float inTemp, float hum, float pressure, float vcc) {

   if (isnan(outTemp) || isnan(hum)) {
     Serial.println(F("Failed to read from DHT sensor!"));
     if (_callback) {
        _callback(parseHttpResult(F("FAIL")));
     }
     return;
   }

   // volame zabezpecene - WiFiClientSecure, pro obycejne HTTP by stacil WiFiClient
   if (!_client.connect(_host, _httpPort)) {
     Serial.println(F("connection failed"));
     if (_callback) {
        _callback(parseHttpResult(F("FAIL")));
     }
     return;
   }

   // knstrukce url
   // priklad: http://iot.e23.cz/send.php?teplota=23.2'&'vlhkost=51.8
   String url = "/send.php";
   url += "?packet=";
   url += packetId;
   url += "&teplotaOut=";
   url += outTemp;
   url += "&teplotaIn=";
   url += inTemp;
   url += "&vlhkost=";
   url += hum;
   url += "&tlak=";
   url += pressure;
   url += "&vcc=";
   url += vcc;

   Serial.print(F("Requesting URL: "));
   Serial.println(url);

   // posli request
   _client.print(String("GET ") + url + " HTTP/1.1\r\n" +
                "Host: " + _host + "\r\n" +
                "Authorization: Basic " + _auth + "\r\n" +
                "Connection: close\r\n\r\n");

    unsigned long timeout = millis();
    while (_client.available() == 0) {
      if (millis() - timeout > 15000) {
        Serial.println(F(">>> Client Timeout !"));
        _client.stop();
        return;
      }
    }

    // precteni vystupu volani skriptu
    String firstLine;
    if (_client.available()){
      firstLine = _client.readStringUntil('\r');
    }

    Serial.println(F("closing connection"));

    if (_callback) {
      _callback(parseHttpResult(firstLine));
    }
}

int CustomWiFiClient::parseHttpResult(String httpResultString) {
   // HTTP/1.1 200 - OK
   if (httpResultString.length() > 12) {
      String numericHttpResult = httpResultString.substring(9,12);
      int httpResult = atoi(numericHttpResult.c_str());
      return httpResult == 0 ? -1 : httpResult;
   }
   return -1; // FAILURE
}

void CustomWiFiClient::sentCallback(void (*callback)(int)) {
   _callback = callback;
}
