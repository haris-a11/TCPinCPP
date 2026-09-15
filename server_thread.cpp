#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>

#include <cstring>
#include <cstdio>
#include <thread>

#define PORT 8080
#define BUFFER_SIZE 1024

void handle_client(int client_fd){
  char buffer[BUFFER_SIZE];
  ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
 
  if (bytes_read == -1)
  {
    perror("read");
    close(client_fd);
    return;
  }

  while (bytes_read > 0)
  {

    ssize_t total_written = 0;
    while (total_written < bytes_read)
    {
      ssize_t bytes_written = write(client_fd, buffer + total_written, bytes_read - total_written);
      if (bytes_written == -1)
      {
        perror("write");
        break;
      }
      total_written += bytes_written;
    }
    bytes_read = read(client_fd, buffer, sizeof(buffer));
  }

  printf("Client disconnected\n");  
  close(client_fd);
}

int main()
{
  // 1. socket()
  // AF_INET = IPv4, SOCK_STREAM = TCP, 0 = default protocol
  int socket_fd = socket(AF_INET, SOCK_STREAM, 0);

  if (socket_fd == -1)
  {
    perror("socket");
    return 1;
  }
  int opt = 1;

  // sockets have multiple options, setsockopt() allows to set any option each call is for a particular option. Here we set the SO_REUSEADDR option to allow the socket to be bound to an address that is already in use.
  // If server is closed with ctrl+c and restarted quickly, the socket may still be in TIME_WAIT state and the bind() call will fail with "Address already in use" error. Setting SO_REUSEADDR allows the server to bind to the same address and port even if they are still in use by a previous instance of the server.
  setsockopt(
      socket_fd,
      SOL_SOCKET,
      SO_REUSEADDR,
      &opt,
      sizeof(opt));

  // 2. bind()
  // struct sockaddr_in
  // {
  //     sin_family; // IPv4
  //     sin_port;   // port
  //     sin_addr;   // IP address
  // };
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;

  // htons = multibytes numbers can be stored in different ways depending on the architecture (big-endian vs little-endian). htons converts a short integer from host byte order to network byte order.
  addr.sin_port = htons(PORT);

  if (bind(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
  {
    perror("bind");
    close(socket_fd);
    return 1;
  }

  // 3. listen()
  if (listen(socket_fd, 1) == -1)
  {
    perror("listen");
    close(socket_fd);
    return 1;
  }

  printf("Server listening on port %d\n", PORT);
  while (true)
  {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    // client already send options providing addresses to store them and then print that information
    int client_fd = accept(
        socket_fd,
        (struct sockaddr *)&client_addr,
        &client_len);
    if (client_fd == -1)
    {
      perror("accept");
      continue;
    }
    printf("Client connected from %s:%d\n",
           inet_ntoa(client_addr.sin_addr),
           ntohs(client_addr.sin_port));

    std::thread(handle_client, client_fd).detach();
  }

  close(socket_fd);

  return 0;
}