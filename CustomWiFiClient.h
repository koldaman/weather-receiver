/**************************************************************
Zabaleni funkcionality pro WiFiClient (odesilani dat pres HTTP)
**************************************************************/

#include <WiFiClient.h>

class CustomWiFiClient {
public:
   CustomWiFiClient();

   void   sendData(byte packetId, float outTemp, float inTemp, float hum, float pressure, float vcc);
   void   sentCallback(void (*callback)(int httpStatus));
private:
   char* _host;
   int   _httpPort;
   char* _auth;
   WiFiClient _client;
   void  (*_callback)(int httpResult);
   int parseHttpResult(String httpResultString);
};
