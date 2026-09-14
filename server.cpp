#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>

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

    // signal(sig,handler) registers what this process shud do when the signal sig arrives. 

    signal(SIGCHLD, SIG_IGN); // auto reaps children no need to waitpid
   
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

        pid_t pid = fork();
        if (pid == -1)
        {
            perror("fork");
            close(client_fd);
            continue;
        }
        else if (pid == 0)
        {


            // If 5 children are holding copies and you Ctrl + C the parent, port 8080 remains occupied by those children
            
            close(socket_fd); 
            // exec() only takes strings, so convert the fd number to a string
            char fd_str[16];
            snprintf(fd_str, sizeof(fd_str), "%d", client_fd);


            // when execl run all variables are wiped out but descriptors arent closed so closed manually above
            execl("./child", "./child", fd_str, (char *)NULL);
            perror("execl"); // only reached if exec fails
            return 1;
        }
        else
        {
            close(client_fd);
        }
    }

    // Never reached in this version
    close(socket_fd);

    return 0;
}