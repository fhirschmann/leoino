#ifndef FTP_COMMAND_H_
#define FTP_COMMAND_H_

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <vector>

#include "FTPFilesystem.h"
#include "FTPPath.h"
#include "FTPResponseCodes.h"

class FTPCommand {
public:
  FTPCommand(String Name, int ExpectedArgumentCnt, WiFiClient *const Client, FTPFilesystem *const Filesystem = 0, IPAddress *DataAddress = 0, int *DataPort = 0, WiFiServer **PassiveServer = 0, bool *PassiveMode = 0) : _Name(Name), _ExpectedArgumentCnt(ExpectedArgumentCnt), _Filesystem(Filesystem), _DataAddress(DataAddress), _DataPort(DataPort), _PassiveServer(PassiveServer), _PassiveMode(PassiveMode), _Client(Client), _DataConnection(0) {
  }
  virtual ~FTPCommand() {
    if (_DataConnection != 0) {
      _DataConnection->stop();
      delete _DataConnection;
      _DataConnection = 0;
    }
  }

  String getName() const {
    return _Name;
  }

  virtual void run(FTPPath &WorkDirectory, const std::vector<String> &Line) = 0;

  void SendResponse(int Code, const String &Text) {
    // Emit the entire FTP control reply in one write() so stateful router/firewall FTP ALGs
    // (such as Linux nf_conntrack_ftp inspecting "227 Entering Passive Mode (...)\r\n") never
    // see "227" split into a separate 3-byte TCP segment without CRLF.
    String line;
    line.reserve(Text.length() + 8);
    line += String(Code);
    line += ' ';
    line += Text;
    line += "\r\n";
    _Client->write(reinterpret_cast<const uint8_t *>(line.c_str()), line.length());
  }

  bool ConnectDataConnection() {
    if (_DataConnection == 0) {
      _DataConnection = new WiFiClient();
    }
    if (_DataConnection->connected()) {
      _DataConnection->stop();
    }
    if (_PassiveMode != 0 && *_PassiveMode) {
      if (_PassiveServer == 0 || *_PassiveServer == 0) {
        SendResponse(FtpCodes::NO_DATA_CONNECTION, "No passive server");
        return false;
      }
      WiFiServer         *server                 = *_PassiveServer;
      const unsigned long passiveAcceptTimeoutMs = 1500;
      const unsigned long passivePollDelayMs     = 5;
      unsigned long       start                  = millis();
      while (!server->hasClient() && millis() - start < passiveAcceptTimeoutMs) {
        yield();
        delay(passivePollDelayMs);
      }
      if (!server->hasClient()) {
        StopPassiveServer();
        SendResponse(FtpCodes::NO_DATA_CONNECTION, "No data connection");
        return false;
      }
      WiFiClient client = server->accept();
      if (!client) {
        StopPassiveServer();
        SendResponse(FtpCodes::NO_DATA_CONNECTION, "No data connection");
        return false;
      }
      *_DataConnection = client;
      StopPassiveServer();
      *_PassiveMode = false;
      SendResponse(FtpCodes::DATA_CONNECTION_OPEN, "Accepted data connection");
      return true;
    }
    _DataConnection->connect(*_DataAddress, *_DataPort);
    if (!_DataConnection->connected()) {
      _DataConnection->stop();
      SendResponse(FtpCodes::NO_DATA_CONNECTION, "No data connection");
      return false;
    }
    SendResponse(FtpCodes::DATA_CONNECTION_OPEN, "Accepted data connection");
    return true;
  }

  void data_print(String str) {
    if (_DataConnection == 0 || !_DataConnection->connected()) {
      return;
    }
    _DataConnection->print(str);
  }

  void data_println(String str) {
    if (_DataConnection == 0 || !_DataConnection->connected()) {
      return;
    }
    _DataConnection->println(str);
  }

  void data_send(uint8_t *c, size_t l) {
    if (_DataConnection == 0 || !_DataConnection->connected()) {
      return;
    }
    _DataConnection->write(c, l);
  }

  int data_read(uint8_t *c, size_t l) {
    if ((_DataConnection != 0) && (_DataConnection->available() > 0)) {
      return _DataConnection->read(c, l);
    }
    return 0;
  }

  bool isDataConnected() const {
    return (_DataConnection != 0) && (_DataConnection->connected() || _DataConnection->available() > 0);
  }

  bool isControlConnected() const {
    return (_Client != 0) && _Client->connected();
  }

  void CloseDataConnection() {
    if (_DataConnection != 0) {
      _DataConnection->stop();
    }
    if (_PassiveMode != 0 && *_PassiveMode) {
      StopPassiveServer();
      *_PassiveMode = false;
    }
  }

private:
  void StopPassiveServer() {
    if (_PassiveServer != 0 && *_PassiveServer != 0) {
      (*_PassiveServer)->stop();
      delete *_PassiveServer;
      *_PassiveServer = 0;
    }
  }

protected:
  String               _Name;
  int                  _ExpectedArgumentCnt;
  FTPFilesystem *const _Filesystem;
  IPAddress *const     _DataAddress;
  int *const           _DataPort;
  WiFiServer **const   _PassiveServer;
  bool *const          _PassiveMode;

private:
  WiFiClient *const _Client;
  WiFiClient       *_DataConnection;
};

class FTPCommandTransfer : public FTPCommand {
public:
  FTPCommandTransfer(String Name, int ExpectedArgumentCnt, WiFiClient *const Client, FTPFilesystem *const Filesystem = 0, IPAddress *DataAddress = 0, int *DataPort = 0, WiFiServer **PassiveServer = 0, bool *PassiveMode = 0) : FTPCommand(Name, ExpectedArgumentCnt, Client, Filesystem, DataAddress, DataPort, PassiveServer, PassiveMode) {
  }

  virtual void workOnData() = 0;

  bool trasferInProgress() {
    return _file;
  }

  virtual void abort() {
    if (_file) {
      CloseDataConnection();
      SendResponse(FtpCodes::CONNECTION_CLOSED, "Transfer aborted");
      _file.close();
    }
  }

protected:
  File _file;
};

#endif
