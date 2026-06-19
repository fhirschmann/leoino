#ifndef PASV_H_
#define PASV_H_

#include <WiFiClient.h>
#include <WiFiServer.h>
#if defined(ESP32)
#include <WiFi.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#endif

#include "../FTPCommand.h"
#include "../FTPResponseCodes.h"
#include "../common.h"

class PASV : public FTPCommand {
public:
  explicit PASV(WiFiClient *const Client, IPAddress *DataAddress, int *DataPort, WiFiServer **PassiveServer = 0, bool *PassiveMode = 0) : FTPCommand("PASV", 0, Client, 0, DataAddress, DataPort, PassiveServer, PassiveMode) {
  }

  void run(FTPPath &WorkDirectory, const std::vector<String> &Line) override {
    if (_PassiveServer != 0 && *_PassiveServer != 0) {
      (*_PassiveServer)->stop();
      delete *_PassiveServer;
      *_PassiveServer = 0;
    }
    IPAddress localIP = WiFi.localIP();
    if (localIP == IPAddress(0, 0, 0, 0)) {
      if (_PassiveMode != 0) {
        *_PassiveMode = false;
      }
      SendResponse(FtpCodes::NO_DATA_CONNECTION, "No local IP address for passive mode");
      return;
    }
    int port   = 20000 + random(0, 1000);
    *_DataPort = port;
    if (_PassiveServer != 0) {
      *_PassiveServer = new WiFiServer(port);
      (*_PassiveServer)->begin();
    }
    if (_PassiveMode != 0) {
      *_PassiveMode = true;
    }
    int    p1       = port / 256;
    int    p2       = port % 256;
    String response = "Entering Passive Mode (" + String(localIP[0]) + "," + String(localIP[1]) + "," + String(localIP[2]) + "," + String(localIP[3]) + "," + String(p1) + "," + String(p2) + ")";
    SendResponse(FtpCodes::ENTERING_PASV_MODE, response);
  }
};

#endif
