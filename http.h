#pragma once

#include <string>
#include <unordered_map>

struct HttpRequest
{
  std::string method;  // "GET"
  std::string path;    // "/index.html"
  std::string version; // "HTTP/1.1"
  std::unordered_map<std::string, std::string> headers; 
};

enum class ParseResult
{
  Incomplete, // need more bytes, try again after the next read()
  Ok,         // one full request parsed
  Bad,        // malformed, answer 400 and close
};

// Try to parse one request from buf starting at `start`.
// On Ok, `consumed` is how many bytes that request used.
ParseResult parseRequest(const std::string &buf, size_t start,
                         HttpRequest &req, size_t &consumed);
