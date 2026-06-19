#ifndef FTP_PATH_H_
#define FTP_PATH_H_

#include <Arduino.h>
#include <list>

class FTPPath {
public:
  FTPPath();
  explicit FTPPath(String path);
  virtual ~FTPPath();

  void changePath(String path);
  void goPathUp();

  String getPath() const;
  String getClearPath() const;
  String getFilePath(String filename) const;

  static std::list<String> splitPath(String path);
  static String            createPath(const std::list<String> &path);

  String sanitize(String input) const;
  String reparse(String input) const;

private:
#ifdef ENABLE_FTP_SANITIZATION
  static const String &getInvalidChars() {
    static const String invalidChars = ":*?\"<>|%";
    return invalidChars;
  }

  // Helper to convert nibble to hex character
  char to_hex(unsigned char v) const {
    return v < 10 ? '0' + v : 'A' + (v - 10);
  }

  // Helper to convert hex character to nibble
  int from_hex(char c) const {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return -1;
  }
#endif

  std::list<String> _Path;
};

#endif
