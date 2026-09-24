#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

class EventLoop
{
public:
  explicit EventLoop(int port);
  // destructor
  ~EventLoop();

  // you can copy objects of your own classes
  // to prevent copying, you can delete the copy constructor and copy assignment operator
  EventLoop(const EventLoop &) = delete;
  // In C++, writing c = a; for
  // objects actually calls a function named operator=
  EventLoop & operator=(const EventLoop &) = delete;
  // delete removes the functions
  
  void run();

private:
  void setupListenSocket();
  void addFd(int fd, uint32_t events);
  void modFd(int fd, uint32_t events);
  void removeFd(int fd);
  void handleAccept();
  void handleClientEvent(int fd, uint32_t events);
  bool flush(int fd);
  void updateInterest(int fd, bool rearm);

  // per-client state: bytes waiting to be written back
  struct Connection
  {
    std::string outbuf;
    uint32_t events = 0; // mask of events currently registered with epoll
  };

  int port_;
  int listen_fd_ = -1;
  int epoll_fd_ = -1;
  std::unordered_map<int, Connection> conns_;
};
