#pragma once
#include <algorithm>
#include <cctype>
#include <map>
#include <string>

namespace surface::device {
// Incremental, bounded HTTP/1.1 subset. No chunking, pipelining, or keep-alive.
// The adapter feeds at most 512 bytes per loop and enforces a whole-client deadline.
class WriterHttpRequest {
  std::string buffer;
  size_t headerEnd = 0, length = 0;

public:
  static constexpr size_t maxBody = 4608, maxHeaders = 2048;
  int error = 0;
  std::string method, path, body;
  std::map<std::string, std::string> headers;
  bool complete = false;
  void feed(char c) {
    if (complete || error)
      return;
    buffer += c;
    if (!headerEnd) {
      if (buffer.size() > maxHeaders) {
        error = 431;
        return;
      }
      if (buffer.size() < 4 || buffer.compare(buffer.size() - 4, 4, "\r\n\r\n") != 0)
        return;
      headerEnd = buffer.size();
      size_t line = buffer.find("\r\n"), a = buffer.find(' '), b = buffer.find(' ', a + 1);
      if (a == std::string::npos || b == std::string::npos || b >= line ||
          buffer.substr(b + 1, line - b - 1) != "HTTP/1.1") {
        error = 400;
        return;
      }
      method = buffer.substr(0, a);
      path = buffer.substr(a + 1, b - a - 1);
      size_t start = line + 2;
      while (start < headerEnd - 2) {
        line = buffer.find("\r\n", start);
        const auto colon = buffer.find(':', start);
        if (colon == std::string::npos || colon >= line || colon == start) {
          error = 400;
          return;
        }
        auto key = buffer.substr(start, colon - start);
        for (char& k : key) {
          if (!(std::isalnum(static_cast<unsigned char>(k)) || k == '-')) {
            error = 400;
            return;
          }
          k = char(std::tolower(static_cast<unsigned char>(k)));
        }
        auto value = buffer.substr(colon + 1, line - colon - 1);
        const auto first = value.find_first_not_of(" \t");
        value = first == std::string::npos
                    ? ""
                    : value.substr(first, value.find_last_not_of(" \t") - first + 1);
        if (!headers.emplace(key, value).second) {
          error = 400;
          return;
        }
        start = line + 2;
      }
      if (!headers.count("host") || headers.count("transfer-encoding") || headers.count("expect")) {
        error = 400;
        return;
      }
      if (headers.count("content-length")) {
        const auto& size = headers.at("content-length");
        if (size.empty()) {
          error = 400;
          return;
        }
        for (char digit : size) {
          if (digit < '0' || digit > '9') {
            error = 400;
            return;
          }
          length = length * 10 + unsigned(digit - '0');
          if (length > maxBody) {
            error = 413;
            return;
          }
        }
      } else if (method == "POST") {
        error = 411;
        return;
      }
      if (method != "POST" && method != "GET") {
        error = 405;
        return;
      }
      if (method == "GET" && length) {
        error = 400;
        return;
      }
      buffer.clear();
      complete = length == 0;
    } else if (buffer.size() == length) {
      body = std::move(buffer);
      complete = true;
    }
  }
  bool sameOrigin(const std::string& ip) const {
    const auto host = headers.find("host");
    if (host == headers.end() || (host->second != ip && host->second != ip + ":80"))
      return false;
    if (method == "GET")
      return true;
    const auto origin = headers.find("origin"), type = headers.find("content-type");
    return origin != headers.end() &&
           (origin->second == "http://" + ip || origin->second == "http://" + ip + ":80") &&
           type != headers.end() && type->second == "application/json";
  }
};
} // namespace surface::device
