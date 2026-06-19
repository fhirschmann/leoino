#ifndef RMD_H_
#define RMD_H_

#include <WiFiClient.h>

#include "../FTPCommand.h"
#include "../FTPResponseCodes.h"

class RMD : public FTPCommand {
public:
  explicit RMD(WiFiClient *const Client, FTPFilesystem *const Filesystem) : FTPCommand("RMD", 1, Client, Filesystem) {
  }

  void run(FTPPath &WorkDirectory, const std::vector<String> &Line) override {
    String filepath = WorkDirectory.getFilePath(Line[1]);
    if (!_Filesystem->exists(filepath)) {
      SendResponse(FtpCodes::FILE_NOT_FOUND, "Folder " + filepath + " not found");
      return;
    }
    if (_Filesystem->rmdir(filepath)) {
      SendResponse(FtpCodes::FILE_ACTION_OK, " Deleted \"" + filepath + "\"");
    } else {
      SendResponse(FtpCodes::FILE_ACTION_ABORTED, "Can't delete \"" + filepath + "\"");
    }
  }
};

#endif
