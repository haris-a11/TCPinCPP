#include "EventLoop.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

#define BUFFER_SIZE 1024
#define MAX_EVENTS 32

EventLoop::EventLoop(int port) : port_(port)
{
  setupListenSocket();

  epoll_fd_ = epoll_create1(0);
  if (epoll_fd_ == -1)
  {
    perror("epoll_create1");
    close(listen_fd_);
    return;
  }

  addFd(listen_fd_, EPOLLIN);
}

EventLoop::~EventLoop()
{
  if (epoll_fd_ != -1)
    close(epoll_fd_);
  if (listen_fd_ != -1)
    close(listen_fd_);
}

void EventLoop::setupListenSocket()
{
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ == -1)
  {
    perror("socket");
    return;
  }

  int opt = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (bind(listen_fd_, (struct sockaddr *)&addr, sizeof(addr)) == -1)
  {
    perror("bind");
    close(listen_fd_);
    listen_fd_ = -1;
    return;
  }

  if (listen(listen_fd_, 1) == -1)
  {
    perror("listen");
    close(listen_fd_);
    listen_fd_ = -1;
    return;
  }
}

void EventLoop::addFd(int fd, uint32_t events)
{
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
}

void EventLoop::removeFd(int fd)
{
  epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  close(fd);
}

void EventLoop::handleAccept()
{
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);

  int client_fd = accept(listen_fd_, (struct sockaddr *)&client_addr, &client_len);
  if (client_fd == -1)
  {
    perror("accept");
    return;
  }

  addFd(client_fd, EPOLLIN);
}

void EventLoop::handleClientEvent(int fd)
{
  char buffer[BUFFER_SIZE];
  ssize_t bytes_read = read(fd, buffer, sizeof(buffer));

  if (bytes_read == -1)
  {
    perror("read");
    removeFd(fd);
    return;
  }

  if (bytes_read == 0)
  {
    printf("Client disconnected\n");
    removeFd(fd);
    return;
  }

  while (bytes_read > 0)
  {
    ssize_t bytes_written = write(fd, buffer, bytes_read);
    if (bytes_written == -1)
    {
      perror("write");
      break;
    }
    bytes_read -= bytes_written;
  }
}

void EventLoop::run()
{
  if (listen_fd_ == -1 || epoll_fd_ == -1)
    return;

  epoll_event events[MAX_EVENTS];

  while (true)
  {
    int ready = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);

    if (ready == -1)
    {
      perror("epoll_wait");
      break;
    }

    for (int i = 0; i < ready; ++i)
    {
      int fd = events[i].data.fd;

      if (fd == listen_fd_)
        handleAccept();
      else
        handleClientEvent(fd);
    }
  }
}
