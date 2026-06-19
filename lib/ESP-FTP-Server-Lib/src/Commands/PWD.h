#ifndef PWD_H_
#define PWD_H_

#include <WiFiClient.h>

#include "../FTPCommand.h"

class PWD : public FTPCommand {
public:
  explicit PWD(WiFiClient *const Client) : FTPCommand("PWD", 0, Client) {
  }

  void run(FTPPath &WorkDirectory, const std::vector<String> &Line) override {
    SendResponse(FtpCodes::PATHNAME_CREATED, "\"" + WorkDirectory.getClearPath() + "\" is your current directory");
  }
};

#endif
