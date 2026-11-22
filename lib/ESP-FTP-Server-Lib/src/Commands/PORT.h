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
    (void) WorkDirectory;
    if (Line.size() < 2) {
      SendResponse(501, "Syntax error in parameters");
      return;
    }
    std::vector<String> connection_details = Split<std::vector<String>>(Line[1], ',');
    if (connection_details.size() != 6) {
      SendResponse(501, "Syntax error in parameters");
      return;
    }
    int parts[6] = {0};
    for (int i = 0; i < 6; i++) {
      String token = connection_details[i];
      token.trim();
      if (token.isEmpty()) {
        SendResponse(501, "Syntax error in parameters");
        return;
      }
      for (size_t j = 0; j < token.length(); ++j) {
        if (token[j] < '0' || token[j] > '9') {
          SendResponse(501, "Syntax error in parameters");
          return;
        }
      }
      long val = token.toInt();
      if (val < 0 || val > 255) {
        SendResponse(501, "Syntax error in parameters");
        return;
      }
      parts[i] = static_cast<int>(val);
    }
    if (_PassiveServer != 0 && *_PassiveServer != 0) {
      (*_PassiveServer)->stop();
      delete *_PassiveServer;
      *_PassiveServer = 0;
    }
    if (_PassiveMode != 0) {
      *_PassiveMode = false;
    }
    for (int i = 0; i < 4; i++) {
      (*_DataAddress)[i] = static_cast<uint8_t>(parts[i]);
    }
    *_DataPort = parts[4] * 256 + parts[5];
    SendResponse(FtpCodes::COMMAND_OK, "PORT command successful");
  }
};

#endif
