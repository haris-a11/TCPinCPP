#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <vector>
#include <unordered_map>

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_EVENTS 32

int main()
{
  int socket_fd = socket(AF_INET, SOCK_STREAM, 0);

  if (socket_fd == -1)
  {
    perror("socket");
    return 1;
  }
  int opt = 1;

  setsockopt(
      socket_fd,
      SOL_SOCKET,
      SO_REUSEADDR,
      &opt,
      sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(PORT);

  if (bind(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
  {
    perror("bind");
    close(socket_fd);
    return 1;
  }

  if (listen(socket_fd, 1) == -1)
  {
    perror("listen");
    close(socket_fd);
    return 1;
  }

  // 1. create the epoll instance -- a kernel-side container that will
  // track every fd we register with it
  int epoll_fd = epoll_create1(0);
  if (epoll_fd == -1)
  {
    perror("epoll_create1");
    close(socket_fd);
    return 1;
  }

  // 2. register the listening socket with it, once
  epoll_event listen_ev{};
  listen_ev.events = EPOLLIN;    // "tell me when readable"
  listen_ev.data.fd = socket_fd; // stash which fd this event is about

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &listen_ev) == -1)
  {
    perror("epoll_ctl: socket_fd");
    close(socket_fd);
    close(epoll_fd);
    return 1;
  }

  // output buffer epoll_wait() fills with only the ready fds
  epoll_event events[MAX_EVENTS];

  while (true)
  {
    // -1 == block forever, same meaning as poll()'s -1
    int ready = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

    if (ready == -1)
    {
      perror("epoll_wait");
      break;
    }

    // only loop over the fds that are ACTUALLY ready -- no

    for (int i = 0; i < ready; ++i)
    {
      int fd = events[i].data.fd;

      // --- new connection ---
      if (fd == socket_fd)
      {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(
            socket_fd,
            (struct sockaddr *)&client_addr,
            &client_len);

        if (client_fd == -1)
        {
          perror("accept");
          continue;
        }

        epoll_event client_ev{};
        client_ev.events = EPOLLIN;
        client_ev.data.fd = client_fd;

        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);
        continue;
      }

      // --- existing client has data (or hung up / errored) ---
      int c = fd;

      char buffer[BUFFER_SIZE];
      ssize_t bytes_read = read(c, buffer, sizeof(buffer));

      if (bytes_read == -1)
      {
        perror("read");
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c, nullptr);
        close(c);
        continue;
      }

      if (bytes_read == 0)
      {
        printf("Client disconnected\n");
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c, nullptr);
        close(c);
        continue;
      }

      // perform partial write
      while (bytes_read > 0)
      {
        ssize_t bytes_written = write(c, buffer, bytes_read);
        if (bytes_written == -1)
        {
          perror("write");
          break;
        }
        bytes_read -= bytes_written;
      }
    }
  }

  close(socket_fd);
  close(epoll_fd);

  return 0;
}