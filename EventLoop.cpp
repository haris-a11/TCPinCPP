#include "EventLoop.h"

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <csignal>
#include <vector>


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

#define MAX_EVENTS 32
#define SWEEP_INTERVAL_MS 1000 // epoll_wait timeout = how often idle clients are checked
#ifndef IDLE_TIMEOUT_S
#define IDLE_TIMEOUT_S 30      // close clients with no traffic for this long
#endif
#define SHUTDOWN_GRACE_S 5     // time clients get to receive queued replies on shutdown

using std::chrono::milliseconds;
using std::chrono::seconds;

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
  setupSignals();

  spare_fd_ = open("/dev/null", O_RDONLY | O_CLOEXEC);
}

EventLoop::~EventLoop()
{
  conns_.clear(); // ~Connection closes every client fd
  if (spare_fd_ != -1)
    close(spare_fd_);
  if (signal_fd_ != -1)
    close(signal_fd_);
  if (epoll_fd_ != -1)
    close(epoll_fd_);
  if (listen_fd_ != -1)
    close(listen_fd_);
}

// Turn SIGINT/SIGTERM into an fd epoll can watch, instead of an async handler
// that could interrupt us in the middle of anything.
void EventLoop::setupSignals()
{
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);

  // blocked signals are not delivered to a handler; they queue up for signalfd
  if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1)
  {
    perror("sigprocmask");
    return;
  }

  signal_fd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (signal_fd_ == -1)
  {
    perror("signalfd");
    return;
  }

  // level-triggered on purpose: we read one signal per wakeup and epoll
  // reports it again if another is still pending
  addFd(signal_fd_, EPOLLIN);
}

void EventLoop::handleSignal()
{
  signalfd_siginfo info;
  if (read(signal_fd_, &info, sizeof(info)) != sizeof(info))
    return;

  printf("Received %s\n", strsignal(info.ssi_signo));

  if (shutting_down_)
  {
    // second Ctrl+C: stop waiting for slow clients
    printf("Forcing shutdown\n");
    shutdown_deadline_ = Connection::Clock::now();
    return;
  }

  beginShutdown();
}

void EventLoop::beginShutdown()
{
  shutting_down_ = true;
  shutdown_deadline_ = Connection::Clock::now() + seconds(SHUTDOWN_GRACE_S);

  // stop accepting: new clients get "connection refused" instead of hanging
  epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, listen_fd_, nullptr);
  close(listen_fd_);
  listen_fd_ = -1;

  // copy the fds first: removeFd erases from conns_ while we walk it
  std::vector<int> fds;
  for (auto &entry : conns_)
    fds.push_back(entry.first);

  for (int fd : fds)
  {
    Connection &conn = conns_.at(fd);
    conn.beginClose("ERR server shutting down");
    if (!conn.flush() || conn.done())
    {
      removeFd(fd);
      continue;
    }
    updateInterest(fd, conn, false); // drops EPOLLIN, keeps EPOLLOUT until flushed
  }

  printf("Shutting down, waiting for %zu client(s)\n", conns_.size());
}

// Covers both idle clients and dead ones (peer vanished without FIN/RST):
// either way no bytes have moved for IDLE_TIMEOUT_S.
void EventLoop::closeIdleConnections()
{
  Connection::Clock::time_point now = Connection::Clock::now();
  if (now - last_sweep_ < milliseconds(SWEEP_INTERVAL_MS))
    return;
  last_sweep_ = now;

  for (auto it = conns_.begin(); it != conns_.end();)
  {
    if (now - it->second.lastActive() > seconds(IDLE_TIMEOUT_S))
    {
      printf("Closing idle connection (fd %d)\n", it->first);
      epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, it->first, nullptr);
      it = conns_.erase(it); // erase returns the next valid iterator
    }
    else
      ++it;
  }
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
  conns_.erase(fd); // ~Connection closes the fd
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
      if ((errno == EMFILE || errno == ENFILE) && spare_fd_ != -1)
      {
        // out of fds: under ET the queued client would never fire again, so
        // free the spare fd, accept the client and drop it, then re-reserve
        close(spare_fd_);
        int shed_fd = accept(listen_fd_, nullptr, nullptr);
        if (shed_fd != -1)
          close(shed_fd); // before re-reserving, or the open() has no fd to use
        spare_fd_ = open("/dev/null", O_RDONLY | O_CLOEXEC);
        // EMFILE is reported even when the queue is empty, so stop once
        // there was nothing left to shed or this loop never ends
        if (shed_fd == -1)
          break;
        fprintf(stderr, "accept: out of file descriptors, dropped a client\n");
        continue;
      }
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

    // try_emplace builds the Connection in place (it can't be copied)
    Connection &conn = conns_.try_emplace(client_fd, client_fd).first->second;
    conn.registered_events = EPOLLIN | EPOLLET;
    addFd(client_fd, conn.registered_events);
  }
}

void EventLoop::handleClientEvent(int fd, uint32_t events)
{
  auto it = conns_.find(fd);
  if (it == conns_.end())
    return;
  Connection &conn = it->second;

  if (events & (EPOLLERR | EPOLLHUP))
  {
    removeFd(fd);
    return;
  }

  bool stopped_early = false;

  if (events & EPOLLIN)
  {
    Connection::ReadStatus status = conn.onReadable();
    if (status == Connection::ReadStatus::Closed)
    {
      removeFd(fd);
      return;
    }
    stopped_early = (status == Connection::ReadStatus::Paused);
  }

  // runs for EPOLLIN (new replies queued) and EPOLLOUT (socket writable again)
  if (!conn.flush() || conn.done())
  {
    removeFd(fd);
    return;
  }

  updateInterest(fd, conn, stopped_early);
}

// Recompute what this client should be watched for; touch epoll only if it changed.
// rearm: we stopped reading before EAGAIN, so ET won't fire again for the data
// already queued. EPOLL_CTL_MOD re-checks readiness, so force it.
void EventLoop::updateInterest(int fd, Connection &conn, bool rearm)
{
  uint32_t want = EPOLLET;

  // EPOLLOUT only while there is unsent data, or epoll wakes us for nothing
  if (conn.wantsWrite())
    want |= EPOLLOUT;
  // EPOLLIN only while open and the output buffer has room
  if (conn.wantsRead())
    want |= EPOLLIN;

  if (want != conn.registered_events || rearm)
  {
    modFd(fd, want);
    conn.registered_events = want;
  }
}

void EventLoop::run()
{
  if (listen_fd_ == -1 || epoll_fd_ == -1 || signal_fd_ == -1)
    return;

  epoll_event events[MAX_EVENTS];
  last_sweep_ = Connection::Clock::now();

  // normal mode: forever. shutdown mode: until every client is gone or the grace period ends
  while (!shutting_down_ ||
         (!conns_.empty() && Connection::Clock::now() < shutdown_deadline_))
  {
    // a timeout instead of -1 so idle connections are checked even when nothing happens
    int ready = epoll_wait(epoll_fd_, events, MAX_EVENTS, SWEEP_INTERVAL_MS);

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
      else if (fd == signal_fd_)
        handleSignal();
      else
        handleClientEvent(fd, events[i].events);
    }

    closeIdleConnections();
  }

  if (!conns_.empty())
    printf("Grace period over, dropping %zu client(s)\n", conns_.size());
  conns_.clear();
  printf("Server stopped\n");
}
