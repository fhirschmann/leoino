#ifndef COMMON_H_
#define COMMON_H_

#include <Arduino.h>
#include <algorithm>
#include <list>
#include <vector>

template <typename T> T Split(String str, char parser) {
  T   str_array;
  int last_idx = 0;
  int next_idx = str.indexOf(parser, last_idx);
  do {
    str_array.push_back(str.substring(last_idx, next_idx));
    last_idx = next_idx + 1;
    next_idx = str.indexOf(parser, last_idx);
    if (next_idx == -1 && last_idx != 0) {
      str_array.push_back(str.substring(last_idx, str.length()));
    }
  } while (next_idx != -1);
  return str_array;
}

inline String ExtractPathFromOptions(const String &args) {
  if (args.isEmpty()) {
    return String();
  }
  std::vector<String> tokens = Split<std::vector<String>>(args, ' ');
  tokens.erase(std::remove_if(tokens.begin(), tokens.end(),
                              [](const String &s) {
                                return s.isEmpty();
                              }),
               tokens.end());

  for (size_t i = 0; i < tokens.size(); ++i) {
    if (!tokens[i].startsWith("-")) {
      String path = tokens[i];
      for (size_t j = i + 1; j < tokens.size(); ++j) {
        path += " ";
        path += tokens[j];
      }
      return path;
    }
  }
  return String();
}

#endif
