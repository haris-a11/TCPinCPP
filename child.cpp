#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <cstring>
#include <cstdlib> 
#include <cstdio>

#define BUFFER_SIZE 1024

int main(int argc, char *argv[])
{
  int client_fd = atoi(argv[1]);
  // 5. read() then write()
  char buffer[BUFFER_SIZE];
  // ssize_t is an integer type specifically intended for representing sizes/counts returned by system calls,
  ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
  // Give me whatever bytes are currently available, up to 1024 bytes.

  if (bytes_read == -1)
  {
    perror("read");
    close(client_fd);
    return 1;
  }

  while (bytes_read > 0)
  {
    // perform partial write
    while (bytes_read > 0)
    {
      ssize_t bytes_written = write(client_fd, buffer, bytes_read);
      if (bytes_written == -1)
      {
        perror("write");
        break;
      }
      bytes_read -= bytes_written;
    }
    bytes_read = read(client_fd, buffer, sizeof(buffer));
  }

  printf("Client from %s:%d disconnected\n",
         inet_ntoa(((struct sockaddr_in *)&client_fd)->sin_addr),
         ntohs(((struct sockaddr_in *)&client_fd)->sin_port));
  // 6. close client
  close(client_fd);
}