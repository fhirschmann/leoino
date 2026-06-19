#ifndef MKD_H_
#define MKD_H_

#include <WiFiClient.h>

#include "../FTPCommand.h"
#include "../FTPResponseCodes.h"

class MKD : public FTPCommand {
public:
  explicit MKD(WiFiClient *const Client, FTPFilesystem *const Filesystem) : FTPCommand("MKD", 1, Client, Filesystem) {
  }

  void run(FTPPath &WorkDirectory, const std::vector<String> &Line) override {
    String filepath = WorkDirectory.getFilePath(Line[1]);
    if (_Filesystem->exists(filepath)) {
      SendResponse(FtpCodes::FILE_ACTION_NOT_TAKEN, "Can't create \"" + filepath + "\", Directory exists");
      return;
    }
    if (_Filesystem->mkdir(filepath)) {
      SendResponse(FtpCodes::PATHNAME_CREATED, "\"" + filepath + "\" created");
    } else {
      SendResponse(FtpCodes::FILE_ACTION_NOT_TAKEN, "Can't create \"" + filepath + "\"");
    }
  }
};

#endif
