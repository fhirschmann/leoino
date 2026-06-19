#include "FTPPath.h"
#include "common.h"

FTPPath::FTPPath() {
}

FTPPath::FTPPath(String path) {
  changePath(path);
}

FTPPath::~FTPPath() {
}

void FTPPath::changePath(String path) {
  path                = sanitize(path);
  std::list<String> p = splitPath(path);
  if (!path.isEmpty() && path[0] == '/') {
    _Path.assign(p.begin(), p.end());
  } else {
    std::copy(p.begin(), p.end(), std::back_inserter(_Path));
  }
}

void FTPPath::goPathUp() {
  if (_Path.size() != 0) // Added Akoro 2021-02-27
    _Path.pop_back();
}

String FTPPath::getPath() const {
  return createPath(_Path);
}

String FTPPath::getClearPath() const {
  return reparse(createPath(_Path));
}

String FTPPath::getFilePath(String filename) const {
  String sane_filename = sanitize(filename);
  if (*sane_filename.begin() == '/') {
    return sane_filename;
  }
  if (_Path.size() == 0) {
    return "/" + sane_filename;
  }
  return getPath() + "/" + sane_filename;
}

std::list<String> FTPPath::splitPath(String path) {
  std::list<String> p = Split<std::list<String>>(path, '/');
  p.erase(std::remove_if(p.begin(), p.end(),
                         [](const String &s) {
                           if (s.isEmpty()) {
                             return true;
                           }
                           return false;
                         }),
          p.end());
  return p;
}

String FTPPath::createPath(const std::list<String> &path) {
  if (path.size() == 0) {
    return "/";
  }
  String new_path;
  for (const String &p : path) {
    new_path += "/";
    new_path += p;
  }
  return new_path;
}

String FTPPath::sanitize(String input) const {
#ifndef ENABLE_FTP_SANITIZATION
  return input;
#else
  String output = "";
  // Pre-allocate memory to prevent heap fragmentation
  output.reserve(input.length());

  for (size_t i = 0; i < input.length(); i++) {
    unsigned char c = input[i];
    // Check for illegal chars or percent sign
    if (getInvalidChars().indexOf(c) != -1) {
      output += '%';
      output += to_hex(c >> 4);
      output += to_hex(c & 0x0F);
    } else {
      output += (char)c;
    }
  }
  return output;
#endif
}

String FTPPath::reparse(String input) const {
#ifndef ENABLE_FTP_SANITIZATION
  return input;
#else
  String output = "";
  output.reserve(input.length());

  for (size_t i = 0; i < input.length(); i++) {
    if (input[i] == '%' && i + 2 < input.length()) {
      int high = from_hex(input[i + 1]);
      int low  = from_hex(input[i + 2]);

      if (high != -1 && low != -1) {
        char c = (char)((high << 4) | low);
        if (getInvalidChars().indexOf(c) != -1) {
          output += c;
          i += 2; // Skip the two hex characters
          continue;
        }
      }
    }
    output += input[i];
  }
  return output;
#endif
}
