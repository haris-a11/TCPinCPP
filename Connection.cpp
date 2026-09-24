#include "Connection.h"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>

#define BUFFER_SIZE 1024
#define MAX_OUTBUF (64 * 1024) // stop reading once this much is unsent
#define MAX_LINE 4096          // longest request line we accept

Connection::Connection(int fd) : fd_(fd) {}

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

// Framing: a message is everything up to '\n'. TCP is a byte stream, so one
// read() may hold half a message or several; keep the leftover for next time.
void Connection::processMessages()
{
  size_t start = 0;
  while (state_ == State::Open)
  {
    size_t newline = inbuf_.find('\n', start);
    if (newline == std::string::npos)
      break;

    std::string line = inbuf_.substr(start, newline - start);
    start = newline + 1;

    if (!line.empty() && line.back() == '\r') // telnet sends \r\n
      line.pop_back();

    handleMessage(line);
  }

  // one erase for all consumed messages instead of one per message
  inbuf_.erase(0, start);

  // a partial line that never ends would grow inbuf_ forever
  if (inbuf_.size() > MAX_LINE)
  {
    reply("ERR line too long");
    inbuf_.clear();
    state_ = State::Closing;
  }
}

// Protocol: one command per line, one reply line per command.
//   PING        -> PONG
//   ECHO <text> -> <text>
//   QUIT        -> BYE, then close
void Connection::handleMessage(const std::string &line)
{
  if (line.empty())
    return;

  size_t space = line.find(' ');
  std::string cmd = line.substr(0, space);
  std::string arg = (space == std::string::npos) ? "" : line.substr(space + 1);

  if (cmd == "PING")
    reply("PONG");
  else if (cmd == "ECHO")
    reply(arg);
  else if (cmd == "QUIT")
  {
    reply("BYE");
    state_ = State::Closing;
  }
  else
    reply("ERR unknown command");
}

void Connection::reply(const std::string &msg)
{
  outbuf_ += msg;
  outbuf_ += '\n';
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
  }
  return true;
}
