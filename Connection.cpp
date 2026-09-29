#include "Connection.h"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>

#define BUFFER_SIZE 1024
#define MAX_OUTBUF (64 * 1024) // stop reading once this much is unsent
#define MAX_HEADER 8192        // largest header block we accept


Connection::Connection(int fd) : fd_(fd), last_active_(Clock::now()) {}

Connection::~Connection()
{
  close(fd_);
}

bool Connection::wantsRead() const
{
  return state_ == State::Open && outbuf_.size() < MAX_OUTBUF;
}

bool Connection::wantsWrite() const
{
  return !outbuf_.empty();
}

bool Connection::done() const
{
  return state_ == State::Closing && outbuf_.empty();
}

void Connection::beginClose(const std::string &notice)
{
  if (state_ != State::Open)
    return;
  reply(notice);
  state_ = State::Closing;
}

Connection::ReadStatus Connection::onReadable()
{
  // edge-triggered: drain the socket until EAGAIN
  char buffer[BUFFER_SIZE];
  while (true)
  {
    if (state_ != State::Open)
      return ReadStatus::Drained;

    // backpressure: leave the rest in the kernel until the client reads its replies
    if (outbuf_.size() >= MAX_OUTBUF)
      return ReadStatus::Paused;

    ssize_t bytes_read = read(fd_, buffer, sizeof(buffer));

    if (bytes_read > 0)
    {
      last_active_ = Clock::now();
      inbuf_.append(buffer, bytes_read);
      processMessages();
      continue;
    }

    if (bytes_read == 0)
    {
      // client finished sending; still deliver replies already queued
      printf("Client disconnected\n");
      state_ = State::Closing;
      return ReadStatus::Drained;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return ReadStatus::Drained;
    if (errno == EINTR)
      continue;

    perror("read");
    return ReadStatus::Closed;
  }
}


// Framing: a request is everything up to "\r\n\r\n". TCP is a byte stream,
// so one read() may hold half a request or several; keep the leftover.
void Connection::processMessages()
{
  size_t start = 0;
  while (state_ == State::Open)
  {
    HttpRequest req;
    size_t consumed = 0;
    ParseResult r = parseRequest(inbuf_, start, req, consumed);

    if (r == ParseResult::Incomplete)
      break;

    if (r == ParseResult::Bad)
    {
      reply("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
      state_ = State::Closing;
      break;
    }

    start += consumed;
    handleRequest(req);
  }

  inbuf_.erase(0, start);

  // headers that never end would grow inbuf_ forever
  if (state_ == State::Open && inbuf_.size() > MAX_HEADER)
  {
    reply("HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    inbuf_.clear();
    state_ = State::Closing;
  }
}

void Connection::handleRequest(const HttpRequest &req)
{
  printf("%s %s %s\n", req.method.c_str(), req.path.c_str(), req.version.c_str());
  for (const auto &[name, value] : req.headers)
    printf("  %s: %s\n", name.c_str(), value.c_str());

  // temporary: step 2 replaces this with a proper response builder
  std::string body = "you asked for " + req.path + "\n";
  reply("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\n\r\n" + body);
}

void Connection::reply(const std::string &msg)
{
  outbuf_ += msg;
}

bool Connection::flush()
{
  while (!outbuf_.empty())
  {
    // MSG_NOSIGNAL: a closed peer gives EPIPE instead of killing us with SIGPIPE
    ssize_t bytes_written = send(fd_, outbuf_.data(), outbuf_.size(), MSG_NOSIGNAL);
    if (bytes_written == -1)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      if (errno == EINTR)
        continue;
      perror("send");
      return false;
    }
    // partial write: the kernel took only what fit in the send buffer
    outbuf_.erase(0, bytes_written);
    // a slow reader that is still making progress is not idle
    last_active_ = Clock::now();
  }
  return true;
}
