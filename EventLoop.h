#pragma once

#include "Connection.h"

#include <cstdint>
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
  void updateInterest(int fd, Connection &conn, bool rearm);
  void setupSignals();
  void handleSignal();
  void beginShutdown();
  void closeIdleConnections();

  int port_;
  int listen_fd_ = -1;
  int epoll_fd_ = -1;
  int signal_fd_ = -1; // SIGINT/SIGTERM delivered as readable data
  int spare_fd_ = -1;  // reserved fd, released to shed clients when we hit EMFILE
  std::unordered_map<int, Connection> conns_;

  bool shutting_down_ = false;
  Connection::Clock::time_point shutdown_deadline_;
  Connection::Clock::time_point last_sweep_;
};
