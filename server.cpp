#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <cstring>
#include <cstdio>

#define PORT 8080
#define BUFFER_SIZE 1024

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

    // 4. accept clients
    while (true)
    {
        int client_fd = accept(socket_fd, nullptr, nullptr);
        printf("Client connected\n");
        if (client_fd == -1)
        {
            perror("accept");
            continue;
        }

        // 5. read() then write()
        char buffer[BUFFER_SIZE];
        // ssize_t is an integer type specifically intended for representing sizes/counts returned by system calls,
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
        // Give me whatever bytes are currently available, up to 1024 bytes.
        
        if (bytes_read == -1)
        {
            perror("read");
            close(client_fd);
            continue;
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

        printf("Client disconnected\n");
        // 6. close client
        close(client_fd);
    }

    // Never reached in this version
    close(socket_fd);

    return 0;
}