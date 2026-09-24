#pragma once

#include <cstdint>
#include <string>

// Stores information about its socket, its buffers and where it is in the protocol for a single client connection. 
class Connection
{
public:
  enum class State
  {
    Open,    // reading requests and answering them
    Closing, // no more reading; close once outbuf_ is flushed
  };

  enum class ReadStatus
  {
    Drained, // hit EAGAIN (or stopped reading on purpose)
    Paused,  // outbuf_ full, data may still be waiting in the kernel
    Closed,  // fatal error, close immediately
  };

  explicit Connection(int fd);
  ~Connection(); 

  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;

  ReadStatus onReadable(); 
  bool flush();           

  bool wantsRead() const;
  bool wantsWrite() const;
  bool done() const; 

  uint32_t registered_events = 0; // mask of events currently registered with epoll for this connection

private:
  void processMessages();
  void handleMessage(const std::string &line);
  void reply(const std::string &msg);

  int fd_;
  State state_ = State::Open;
  std::string inbuf_;  // bytes received but not yet a complete message
  std::string outbuf_; // bytes queued but not yet accepted by the kernel
};
