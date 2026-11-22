#ifndef MSLD_H_
#define MSLD_H_
// Copyright (c) 2020 Arkady Korotkevich
// Based on LIST.h by Peter Buchegger
#include <WiFiClient.h>

#include "../FTPCommand.h"
#include "../FTPResponseCodes.h"
#include "../common.h"
#include <time.h>

class MLSD : public FTPCommand {
public:
  explicit MLSD(WiFiClient *const Client, FTPFilesystem *const Filesystem, IPAddress *DataAddress, int *DataPort, WiFiServer **PassiveServer = 0, bool *PassiveMode = 0) : FTPCommand("MLSD", 1, Client, Filesystem, DataAddress, DataPort, PassiveServer, PassiveMode) {
  }

  void run(FTPPath &WorkDirectory, const std::vector<String> &Line) override {
    FTPPath listPath = WorkDirectory;
    // 1. Check if we have arguments
    if (Line.size() > 1) {
      String args = Line[1];
      args.trim(); // Modifies 'args' in place

      if (!args.isEmpty()) {
        String path = ExtractPathFromOptions(args);
        if (!path.isEmpty()) {
          listPath.changePath(path);
        }
      }
    }

    if (!ConnectDataConnection()) {
      return;
    }

    File root = _Filesystem->open(listPath.getPath(), "r");

    if (!root || !root.isDirectory()) {
      root.close();
      CloseDataConnection();
      SendResponse(FtpCodes::FILE_NOT_FOUND, "Can't open directory " + listPath.getClearPath());
      return;
    }

    int  cnt = 0;
    File f   = root.openNextFile();
    while (f) {
      const bool isDir = f.isDirectory();
      char       modifyBuf[32];
      time_t     ft    = f.getLastWrite();
      struct tm  tmBuf = {};
      if (gmtime_r(&ft, &tmBuf) != nullptr) {
        snprintf(modifyBuf, sizeof(modifyBuf), "modify=%04d%02d%02d%02d%02d%02d;", tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday, tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
      } else {
        snprintf(modifyBuf, sizeof(modifyBuf), "modify=19700101000000;");
      }

      String filename = f.name();
      filename.remove(0, filename.lastIndexOf('/') + 1);
      filename = listPath.reparse(filename);

      String line;
      line.reserve(96 + filename.length());
      line += isDir ? "type=dir;sizd=" : "type=file;size=";
      line += String(f.size());
      line += ';';
      line += modifyBuf;
      line += "UNIX.mode=0700; ";
      line += filename;
      line += "\r\n";
      data_send(reinterpret_cast<uint8_t *>(const_cast<char *>(line.c_str())), line.length());

      f.close();
      cnt++;
      if ((cnt & 0x0F) == 0) {
        yield();
      }
      f = root.openNextFile();
    }

    root.close();
    CloseDataConnection();
    SendResponse(FtpCodes::TRANSFER_COMPLETE, String(cnt) + " matches total");
  }
};

#endif
