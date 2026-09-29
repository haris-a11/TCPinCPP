#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>



struct HttpRequest
{
  std::string method;  // "GET"
  std::string path;    // "/index.html"
  std::string version; // "HTTP/1.1"
  std::unordered_map<std::string, std::string> headers; 
};

struct HttpResponse
{
  int status = 200;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;

  void setHeader(const std::string &name, const std::string &value)
  {
    headers.emplace_back(name, value);
  }
};

// 404 -> "Not Found"
const char *statusText(int status);

// Turn a response into the exact bytes to send. Adds Content-Length itself.
std::string serializeResponse(const HttpResponse &res);

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
