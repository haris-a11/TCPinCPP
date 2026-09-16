#include <poll.h>
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


    // struct pollfd
    // {
    //   int fd;        // which file descriptor to watch
    //   short events;  // what you're asking about (input)
    //   short revents; // what actually happened (output, filled by poll())
    // };

    // one entry per fd we're watching -- listening socket is index 0,
    // clients are appended after it
    std::vector<pollfd> poll_fds;

    pollfd listen_pfd{};
    listen_pfd.fd = socket_fd;
    listen_pfd.events = POLLIN;   // we only care about "ready to read"
    poll_fds.push_back(listen_pfd);

    while (true)
    {
        // timeout in milliseconds -- 5000 == 5 seconds, same as before.
        // use -1 for "block forever", 0 for "return immediately"

        // .data() returns a pointer to the underlying array of the vector. 
        int ready = poll(poll_fds.data(), poll_fds.size(), -1);

        if (ready < 0)
        {
            perror("poll");
            break;
        }
        else if (ready == 0)
        {
            std::cout << "No activity for 5s, looping again...\n";
            continue;
        }


        // checks if the POLLIN bit is turned on    
        if (poll_fds[0].revents & POLLIN)
        {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            int client_fd = accept(
                socket_fd,
                (struct sockaddr *)&client_addr,
                &client_len);

            pollfd client_pfd{};
            client_pfd.fd = client_fd;
            client_pfd.events = POLLIN;
            poll_fds.push_back(client_pfd);
        }

        // walk the client entries (skip index 0, that's the listener)
        for (size_t i = 1; i < poll_fds.size();)
        {
            if (!(poll_fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
            {
                ++i;
                continue;
            }

            int c = poll_fds[i].fd;

            char buffer[BUFFER_SIZE];
            ssize_t bytes_read = read(c, buffer, sizeof(buffer));

            if (bytes_read == -1)
            {
                perror("read");
                close(c);
                poll_fds.erase(poll_fds.begin() + i);
                continue;
            }

            if (bytes_read == 0)
            {
                printf("Client disconnected\n");
                close(c);
                poll_fds.erase(poll_fds.begin() + i);
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

            ++i;
        }
    }

    for (auto &p : poll_fds) close(p.fd);

    return 0;
}