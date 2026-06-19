#ifndef LIST_H_
#define LIST_H_

#include <WiFiClient.h>

#include "../FTPCommand.h"
#include "../FTPResponseCodes.h"
#include "../common.h"

class LIST : public FTPCommand {
public:
  explicit LIST(WiFiClient *const Client, FTPFilesystem *const Filesystem, IPAddress *DataAddress, int *DataPort, WiFiServer **PassiveServer = 0, bool *PassiveMode = 0) : FTPCommand("LIST", 1, Client, Filesystem, DataAddress, DataPort, PassiveServer, PassiveMode) {
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
    File dir = _Filesystem->open(listPath.getPath()); //
    if (!dir || !dir.isDirectory()) {
      CloseDataConnection();
      SendResponse(FtpCodes::FILE_NOT_FOUND, "Can't open directory " + listPath.getClearPath());
      return;
    }
    int cnt = 2;
    data_println("drwxr-xr-x 1 owner group 0 Jan 01  1970 .");
    data_println("drwxr-xr-x 1 owner group 0 Jan 01  1970 ..");
    File f = dir.openNextFile();
    while (f) {
      String filename = f.name();
      filename.remove(0, filename.lastIndexOf('/') + 1);
      if (f.isDirectory()) {
        data_print("drwxr-xr-x");
      } else {
        data_print("-rw-r--r--");
      }
      String filesize = String(f.size());
      data_print(" 1 owner group ");
      int fill_cnt = 13 - filesize.length();
      for (int i = 0; i < fill_cnt; i++) {
        data_print(" ");
      }
      filename = listPath.reparse(filename);
      data_println(filesize + " Jan 01  1970 " + filename);
      cnt++;
      f.close();
      f = dir.openNextFile();
    }
    CloseDataConnection();
    SendResponse(FtpCodes::TRANSFER_COMPLETE, String(cnt) + " matches total");
  }
};

#endif
