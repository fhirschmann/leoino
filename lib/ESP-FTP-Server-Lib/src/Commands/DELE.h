#ifndef DELE_H_
#define DELE_H_

#include <WiFiClient.h>

#include "../FTPCommand.h"
#include "../FTPResponseCodes.h"

class DELE : public FTPCommand {
public:
  explicit DELE(WiFiClient *const Client, FTPFilesystem *const Filesystem) : FTPCommand("DELE", 1, Client, Filesystem) {
  }

  void run(FTPPath &WorkDirectory, const std::vector<String> &Line) override {
    String filepath = WorkDirectory.getFilePath(Line[1]);
    if (!_Filesystem->exists(filepath)) {
      SendResponse(FtpCodes::FILE_NOT_FOUND, "File " + filepath + " not found");
      return;
    }
    if (_Filesystem->remove(filepath)) {
      SendResponse(FtpCodes::COMMAND_OK, " Deleted \"" + filepath + "\"");
    } else {
      SendResponse(FtpCodes::FILE_ACTION_ABORTED, "Can't delete \"" + filepath + "\"");
    }
  }
};

#endif
