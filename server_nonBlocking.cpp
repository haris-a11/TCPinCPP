#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>

#define PORT 8080
#define BUFFER_SIZE 1024

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
  std::vector<int> client_fds;

  while (true)
  {
    // a bit mask representing which fd is set
    fd_set read_fds;
    // FD_ZERO initializes the set to be empty
    FD_ZERO(&read_fds);
    // FD_SET adds an fd to the set
    FD_SET(socket_fd, &read_fds);

    int max_fd = socket_fd;
    for (int c : client_fds)
    {
      FD_SET(c, &read_fds);
      if (c > max_fd)
        max_fd = c;
    }
    
    // a struct timeval{sec, microsec} representing the timeout for select.
    timeval timeout{5, 0};
    // select(upperBound of fd, readfds, writefds, exceptfds, timeout)
    // it returns the count of fds that are set
    int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &timeout);
    // select is a system call everything else is userspace

    if (ready < 0)
    {
      perror("select");
      break;
    }
    else if (ready == 0)
    {
      std::cout << "No activity for 5s, looping again...\n";
      continue;
    }

    if (FD_ISSET(socket_fd, &read_fds))
    {
      struct sockaddr_in client_addr;
      socklen_t client_len = sizeof(client_addr);

      int client_fd = accept(
          socket_fd,
          (struct sockaddr *)&client_addr,
          &client_len);
      client_fds.push_back(client_fd);
    }

    for (auto it = client_fds.begin(); it != client_fds.end();)
    {
      int c = *it;

      if (!FD_ISSET(c, &read_fds))
      {
        ++it;
        continue;
      }

  
      char buffer[BUFFER_SIZE];
      ssize_t bytes_read = read(c, buffer, sizeof(buffer));

      if (bytes_read == -1)
      {
        perror("read");
        close(c);
        it = client_fds.erase(it);
        continue;
      }

      if (bytes_read == 0)
      {
        printf("Client disconnected\n");
        close(c);
        it = client_fds.erase(it);
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

      ++it;
    }
  }

  close(socket_fd);

  return 0;
}