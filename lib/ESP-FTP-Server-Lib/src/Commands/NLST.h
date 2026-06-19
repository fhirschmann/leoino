#ifndef NLST_H_
#define NLST_H_

#include <WiFiClient.h>

#include "../FTPCommand.h"
#include "../FTPResponseCodes.h"
#include "../common.h"

class NLST : public FTPCommand {
public:
  explicit NLST(WiFiClient *const Client, FTPFilesystem *const Filesystem, IPAddress *DataAddress, int *DataPort, WiFiServer **PassiveServer = 0, bool *PassiveMode = 0) : FTPCommand("NLST", 1, Client, Filesystem, DataAddress, DataPort, PassiveServer, PassiveMode) {
  }

  void run(FTPPath &WorkDirectory, const std::vector<String> &Line) override {
    if (!ConnectDataConnection()) {
      return;
    }
    File dir = _Filesystem->open(WorkDirectory.getPath()); //
    if (!dir || !dir.isDirectory()) {
      CloseDataConnection();
      SendResponse(FtpCodes::FILE_NOT_FOUND, "Can't open directory " + WorkDirectory.getClearPath());
      return;
    }
    int cnt = 2;
    data_println(".");
    data_println("..");
    File f = dir.openNextFile();
    while (f) {
      String filename = f.name();
      filename.remove(0, filename.lastIndexOf('/') + 1);
      data_println(WorkDirectory.reparse(filename));
      cnt++;
      f.close();
      f = dir.openNextFile();
    }
    CloseDataConnection();
    SendResponse(FtpCodes::TRANSFER_COMPLETE, String(cnt) + " matches total");
  }
};

#endif
