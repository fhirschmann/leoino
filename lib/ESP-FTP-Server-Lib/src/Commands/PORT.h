#ifndef PORT_H_
#define PORT_H_

#include <WiFiClient.h>

#include "../FTPCommand.h"
#include "../FTPResponseCodes.h"
#include "../common.h"

class PORT : public FTPCommand {
public:
  explicit PORT(WiFiClient *const Client, IPAddress *DataAddress, int *DataPort, WiFiServer **PassiveServer = 0, bool *PassiveMode = 0) : FTPCommand("PORT", 1, Client, 0, DataAddress, DataPort, PassiveServer, PassiveMode) {
  }

  void run(FTPPath &WorkDirectory, const std::vector<String> &Line) override {
    if (_PassiveServer != 0 && *_PassiveServer != 0) {
      (*_PassiveServer)->stop();
      delete *_PassiveServer;
      *_PassiveServer = 0;
    }
    if (_PassiveMode != 0) {
      *_PassiveMode = false;
    }
    std::vector<String> connection_details = Split<std::vector<String>>(Line[1], ',');
    for (int i = 0; i < 4; i++) {
      (*_DataAddress)[i] = connection_details[i].toInt();
    }
    *_DataPort = connection_details[4].toInt() * 256 + connection_details[5].toInt();
    SendResponse(FtpCodes::COMMAND_OK, "PORT command successful");
  }
};

#endif
