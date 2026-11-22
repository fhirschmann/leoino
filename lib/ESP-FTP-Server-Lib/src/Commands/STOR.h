#ifndef STOR_H_
#define STOR_H_

#include <WiFiClient.h>
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

#include "../FTPCommand.h"
#include "../common.h"

#define FTP_BUF_SIZE 4096

class STOR : public FTPCommandTransfer {
public:
  explicit STOR(WiFiClient *const Client, FTPFilesystem *const Filesystem, IPAddress *DataAddress, int *DataPort, WiFiServer **PassiveServer = 0, bool *PassiveMode = 0)
      : FTPCommandTransfer("STOR", 1, Client, Filesystem, DataAddress, DataPort, PassiveServer, PassiveMode), _buf(nullptr), _lastDataMs(0) {
  }

  ~STOR() override {
    if (_file) {
      _file.close();
      if (!_ftpFsFilePath.isEmpty()) {
        _Filesystem->remove(_ftpFsFilePath.c_str());
      }
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
    _ftpFsFilePath = WorkDirectory.getFilePath(Line[1]);
    _file          = _Filesystem->open(_ftpFsFilePath, "w");
    if (!_file) {
      SendResponse(FtpCodes::FILE_ACTION_ABORTED_LOCAL_ERROR, "Can't open/create " + _ftpFsFilePath);
      CloseDataConnection();
      return;
    }
    _lastDataMs = millis();
    workOnData();
  }

  void abort() override {
    if (_file) {
      CloseDataConnection();
      _file.close();
      if (!_ftpFsFilePath.isEmpty()) {
        _Filesystem->remove(_ftpFsFilePath.c_str());
        _ftpFsFilePath = "";
      }
      SendResponse(FtpCodes::CONNECTION_CLOSED, "Transfer aborted");
    }
  }

  void workOnData() override {
    if (!_file || !_buf) {
      return;
    }
    if (!isControlConnected()) {
      _file.close();
      if (!_ftpFsFilePath.isEmpty()) {
        _Filesystem->remove(_ftpFsFilePath.c_str());
        _ftpFsFilePath = "";
      }
      CloseDataConnection();
      return;
    }
    int nb = data_read(_buf, FTP_BUF_SIZE);
    if (nb > 0) {
      _lastDataMs   = millis();
      const auto wb = _file.write(_buf, nb);
      if (wb != static_cast<std::remove_cv<decltype(wb)>::type>(nb)) {
        _file.close();
        _Filesystem->remove(_ftpFsFilePath.c_str());
        _ftpFsFilePath = "";
        SendResponse(FtpCodes::EXCEEDED_STORAGE, "Error occured while STORing");
        CloseDataConnection();
      }
      return;
    }
    if (isDataConnected()) {
      if (millis() - _lastDataMs > 15000UL) {
        _file.close();
        _Filesystem->remove(_ftpFsFilePath.c_str());
        _ftpFsFilePath = "";
        SendResponse(FtpCodes::CONNECTION_CLOSED, "Data transfer timed out");
        CloseDataConnection();
      }
      return;
    }

    _file.close();
    _ftpFsFilePath = "";
    CloseDataConnection();
    SendResponse(FtpCodes::TRANSFER_COMPLETE, "File successfully transferred");
  }

private:
  String        _ftpFsFilePath;
  uint8_t      *_buf;
  unsigned long _lastDataMs;
};

#endif
