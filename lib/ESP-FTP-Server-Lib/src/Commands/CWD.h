#ifndef CWD_H_
#define CWD_H_

#include <WiFiClient.h>

#include "../FTPCommand.h"
#include "../FTPResponseCodes.h"

class CWD : public FTPCommand {
public:
  explicit CWD(WiFiClient *const Client, FTPFilesystem *const Filesystem) : FTPCommand("CWD", 1, Client, Filesystem) {
  }

  void run(FTPPath &WorkDirectory, const std::vector<String> &Line) override {
    FTPPath path = WorkDirectory;
    if (Line[1] == "..") {
      path.goPathUp();
    } else {
      path.changePath(Line[1]);
    }
    File dir = _Filesystem->open(path.getPath());
    if (dir.isDirectory()) {
      WorkDirectory = path;
      SendResponse(FtpCodes::COMMAND_OK, "Ok. Current directory is " + WorkDirectory.getClearPath());
    } else {
      SendResponse(FtpCodes::FILE_ACTION_NOT_TAKEN, "Directory does not exist");
    }
  }
};

#endif
