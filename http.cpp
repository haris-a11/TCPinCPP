#include "http.h"

#include <cctype>

static std::string toLower(std::string s)
{
  for (char &c : s)
    c = std::tolower(static_cast<unsigned char>(c));
  return s;
}

static std::string trim(const std::string &s)
{
  size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos)
    return "";
  size_t e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

// "GET /index.html HTTP/1.1" -> method, path, version
static bool parseRequestLine(const std::string &line, HttpRequest &req)
{
  size_t sp1 = line.find(' ');
  if (sp1 == std::string::npos)
    return false;
  size_t sp2 = line.find(' ', sp1 + 1);
  if (sp2 == std::string::npos)
    return false;

  req.method = line.substr(0, sp1);
  req.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
  req.version = line.substr(sp2 + 1);

  return !req.method.empty() && !req.path.empty() &&
         req.version.rfind("HTTP/", 0) == 0; // version starts with "HTTP/"
}

ParseResult parseRequest(const std::string &buf, size_t start,
                         HttpRequest &req, size_t &consumed)
{
  // the header block ends at the first empty line
  size_t end = buf.find("\r\n\r\n", start);
  if (end == std::string::npos)
    return ParseResult::Incomplete;

  req = HttpRequest{};
  consumed = end + 4 - start;

  size_t pos = start;
  size_t eol = buf.find("\r\n", pos);
  if (!parseRequestLine(buf.substr(pos, eol - pos), req))
    return ParseResult::Bad;
  pos = eol + 2;

  // each remaining line up to `end` is "Name: value"
  while (pos < end)
  {
    eol = buf.find("\r\n", pos);
    std::string line = buf.substr(pos, eol - pos);
    pos = eol + 2;

    size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0)
      return ParseResult::Bad;

    req.headers[toLower(line.substr(0, colon))] = trim(line.substr(colon + 1));
  }
  return ParseResult::Ok;
}

const char *statusText(int status)
{
  switch (status)
  {
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 431:
    return "Request Header Fields Too Large";
  case 500:
    return "Internal Server Error";
  case 503:
    return "Service Unavailable";
  default:
    return "Unknown";
  }
}

std::string serializeResponse(const HttpResponse &res)
{
  std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " +
                    statusText(res.status) + "\r\n";

  for (const auto &[name, value] : res.headers)
    out += name + ": " + value + "\r\n";

  // computed here so it can never disagree with the body
  out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";

  out += "\r\n"; // end of headers
  out += res.body;
  return out;
}
