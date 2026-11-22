#ifndef RETR_H_
#define RETR_H_

#include <WiFiClient.h>
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

#include "../FTPCommand.h"
#include "../FTPResponseCodes.h"
#include "../common.h"

#define FTP_BUF_SIZE 4096

class RETR : public FTPCommandTransfer {
public:
  explicit RETR(WiFiClient *const Client, FTPFilesystem *const Filesystem, IPAddress *DataAddress, int *DataPort, WiFiServer **PassiveServer = 0, bool *PassiveMode = 0)
      : FTPCommandTransfer("RETR", 1, Client, Filesystem, DataAddress, DataPort, PassiveServer, PassiveMode), _buf(nullptr) {
  }

  ~RETR() override {
    if (_file) {
      _file.close();
    }
    free(_buf);
    _buf = nullptr;
  }

  void run(FTPPath &WorkDirectory, const std::vector<String> &Line) override {
    if (trasferInProgress()) {
      return;
    }
    if (Line.size() < 2 || Line[1].isEmpty()) {
      SendResponse(501, "Syntax error in parameters");
      return;
    }
    if (!_buf) {
#ifdef ESP_PLATFORM
      _buf = static_cast<uint8_t *>(heap_caps_malloc_prefer(FTP_BUF_SIZE, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT));
#else
      _buf = static_cast<uint8_t *>(malloc(FTP_BUF_SIZE));
#endif
      if (!_buf) {
        SendResponse(FtpCodes::FILE_ACTION_ABORTED_LOCAL_ERROR, "Out of memory");
        return;
      }
    }
    if (!ConnectDataConnection()) {
      return;
    }
    String path = WorkDirectory.getFilePath(Line[1]);
    _file       = _Filesystem->open(path);
    if (!_file || _file.isDirectory()) {
      if (_file) {
        _file.close();
      }
      CloseDataConnection();
      SendResponse(FtpCodes::FILE_NOT_FOUND, "Can't open " + path);
      return;
    }
    workOnData();
  }

  void workOnData() override {
    if (!_file || !_buf) {
      return;
    }
    if (!isControlConnected() || !isDataConnected()) {
      CloseDataConnection();
      _file.close();
      return;
    }
    size_t nb = _file.read(_buf, FTP_BUF_SIZE);
    if (nb > 0) {
      data_send(_buf, nb);
      return;
    }
    CloseDataConnection();
    SendResponse(FtpCodes::TRANSFER_COMPLETE, "File successfully transferred");
    _file.close();
  }

private:
  uint8_t *_buf;
};

#endif
