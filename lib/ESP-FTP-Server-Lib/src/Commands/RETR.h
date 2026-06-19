#ifndef RETR_H_
#define RETR_H_

#include <WiFiClient.h>

#include "../FTPCommand.h"
#include "../FTPResponseCodes.h"
#include "../common.h"

#define FTP_BUF_SIZE 4096

class RETR : public FTPCommandTransfer {
public:
  explicit RETR(WiFiClient *const Client, FTPFilesystem *const Filesystem, IPAddress *DataAddress, int *DataPort, WiFiServer **PassiveServer = 0, bool *PassiveMode = 0) : FTPCommandTransfer("RETR", 1, Client, Filesystem, DataAddress, DataPort, PassiveServer, PassiveMode) {
  }

  void run(FTPPath &WorkDirectory, const std::vector<String> &Line) override {
    if (trasferInProgress()) {
      return;
    }
    if (!ConnectDataConnection()) {
      return;
    }
    String path = WorkDirectory.getFilePath(Line[1]);
    _file       = _Filesystem->open(path);
    if (!_file || _file.isDirectory()) {
      CloseDataConnection();
      SendResponse(FtpCodes::FILE_NOT_FOUND, "Can't open " + path);
      return;
    }
    workOnData();
  }

  void workOnData() override {
    uint8_t buffer[FTP_BUF_SIZE];
    size_t  nb = _file.read(buffer, FTP_BUF_SIZE);
    if (nb > 0) {
      data_send(buffer, nb);
      return;
    }
    CloseDataConnection();
    SendResponse(FtpCodes::TRANSFER_COMPLETE, "File successfully transferred");
    _file.close();
  }

private:
};

#endif
