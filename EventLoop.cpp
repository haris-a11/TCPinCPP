#include "EventLoop.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>


namespace
{
  bool setNonBlocking(int fd)
  {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
      return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
  }
}

#define BUFFER_SIZE 1024
#define MAX_EVENTS 32
#define MAX_OUTBUF (64 * 1024) // stop reading from a client once this much is unsent

EventLoop::EventLoop(int port) : port_(port)
{
  setupListenSocket();

  epoll_fd_ = epoll_create1(0);
  if (epoll_fd_ == -1)
  {
    perror("epoll_create1");
    close(listen_fd_);
    listen_fd_ = -1;
    return;
  }

  addFd(listen_fd_, EPOLLIN | EPOLLET);
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
  listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
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

  if (listen(listen_fd_, SOMAXCONN) == -1)
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

void EventLoop::modFd(int fd, uint32_t events)
{
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

void EventLoop::removeFd(int fd)
{
  epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  close(fd);
  conns_.erase(fd);
}

void EventLoop::handleAccept()
{
  // edge-triggered: accept everything queued until EAGAIN
  while (true)
  {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(listen_fd_, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd == -1)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      if (errno == EINTR || errno == ECONNABORTED)
        continue;
      perror("accept");
      break;
    }

    // client fds do not inherit O_NONBLOCK from the listen socket
    if (!setNonBlocking(client_fd))
    {
      perror("fcntl");
      close(client_fd);
      continue;
    }

    conns_[client_fd] = Connection{};
    conns_[client_fd].events = EPOLLIN | EPOLLET;
    addFd(client_fd, EPOLLIN | EPOLLET);
  }
}

void EventLoop::handleClientEvent(int fd, uint32_t events)
{
  if (events & (EPOLLERR | EPOLLHUP))
  {
    removeFd(fd);
    return;
  }

  bool stopped_early = false;

  if (events & EPOLLIN)
  {
    // edge-triggered: drain the socket until EAGAIN
    char buffer[BUFFER_SIZE];
    while (true)
    {
      // backpressure: leave the rest in the kernel until the client reads its echo
      if (conns_[fd].outbuf.size() >= MAX_OUTBUF)
      {
        stopped_early = true;
        break;
      }

      ssize_t bytes_read = read(fd, buffer, sizeof(buffer));

      if (bytes_read > 0)
      {
        conns_[fd].outbuf.append(buffer, bytes_read);
        continue;
      }

      if (bytes_read == 0)
      {
        printf("Client disconnected\n");
        removeFd(fd);
        return;
      }

      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      if (errno == EINTR)
        continue;

      perror("read");
      removeFd(fd);
      return;
    }
  }

  // runs for EPOLLIN (new data to echo) and EPOLLOUT (socket writable again)
  if (!flush(fd))
    return;

  updateInterest(fd, stopped_early);
}

// Write as much of outbuf as the socket accepts. Returns false if fd was closed.
bool EventLoop::flush(int fd)
{
  std::string &out = conns_[fd].outbuf;

  while (!out.empty())
  {
    // MSG_NOSIGNAL: a closed peer gives EPIPE instead of killing us with SIGPIPE
    ssize_t bytes_written = send(fd, out.data(), out.size(), MSG_NOSIGNAL);
    if (bytes_written == -1)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      if (errno == EINTR)
        continue;
      perror("send");
      removeFd(fd);
      return false;
    }
    // partial write: the kernel took only what fit in the send buffer
    out.erase(0, bytes_written);
  }

  return true;
}

// Recompute what this client should be watched for; touch epoll only if it changed.
// rearm: we stopped reading before EAGAIN, so ET won't fire again for the data
// already queued. EPOLL_CTL_MOD re-checks readiness, so force it.
void EventLoop::updateInterest(int fd, bool rearm)
{
  Connection &conn = conns_[fd];
  uint32_t want = EPOLLET;

  // EPOLLOUT only while there is unsent data, or epoll wakes us for nothing
  if (!conn.outbuf.empty())
    want |= EPOLLOUT;
  // EPOLLIN only while the buffer has room (re-enabling it re-arms ET for queued data)
  if (conn.outbuf.size() < MAX_OUTBUF)
    want |= EPOLLIN;

  if (want != conn.events || rearm)
  {
    modFd(fd, want);
    conn.events = want;
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
      if (errno == EINTR)
        continue;
      perror("epoll_wait");
      break;
    }

    for (int i = 0; i < ready; ++i)
    {
      int fd = events[i].data.fd;

      if (fd == listen_fd_)
        handleAccept();
      else
        handleClientEvent(fd, events[i].events);
    }
  }
}
