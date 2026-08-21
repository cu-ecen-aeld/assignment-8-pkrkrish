#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

#define PORT "9000"
#define RECV_BUF_SIZE 1024

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#if USE_AESD_CHAR_DEVICE
#define DATA_FILE "/dev/aesdchar"
#else
#define DATA_FILE "/var/tmp/aesdsocketdata"
#endif

int server_fd = -1;
volatile sig_atomic_t caught_signal = 0;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        caught_signal = 1;
        if (server_fd != -1) {
            shutdown(server_fd, SHUT_RDWR);
        }
    }
}

void daemonize() {
    pid_t pid = fork();
    if (pid < 0) exit(-1);
    if (pid > 0) exit(0);

    if (setsid() < 0) exit(-1); 
    if (chdir("/") < 0) exit(-1);

    int dev_null = open("/dev/null", O_RDWR);
    if (dev_null != -1) {
        dup2(dev_null, STDIN_FILENO);
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);
        close(dev_null);
    }
}

int main(int argc, char *argv[]) {
    openlog("aesdsocket", LOG_PID, LOG_USER);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct addrinfo hints, *servinfo;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, PORT, &hints, &servinfo) != 0) return -1;

    server_fd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
    if (server_fd == -1) {
        freeaddrinfo(servinfo);
        return -1;
    }

    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    if (bind(server_fd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
        close(server_fd);
        freeaddrinfo(servinfo);
        return -1;
    }

    freeaddrinfo(servinfo);

    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemonize();
    }

    if (listen(server_fd, 10) == -1) {
        close(server_fd);
        return -1;
    }

    while (!caught_signal) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd == -1) {
            if (caught_signal) break;
            continue;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        syslog(LOG_INFO, "Accepted connection from %s", ip_str);

        // Lazily open the character device / data file only upon client connection
        int data_fd = open(DATA_FILE, O_RDWR);
        if (data_fd == -1) {
            syslog(LOG_ERR, "Failed to open %s: %s", DATA_FILE, strerror(errno));
            close(client_fd);
            continue;
        }

        char *buf = malloc(RECV_BUF_SIZE);
        if (!buf) {
            close(data_fd);
            close(client_fd);
            continue;
        }

        ssize_t bytes_recv;
        size_t current_packet_size = 0;

        while ((bytes_recv = recv(client_fd, buf + current_packet_size, RECV_BUF_SIZE - 1, 0)) > 0) {
            current_packet_size += bytes_recv;
            if (buf[current_packet_size - 1] == '\n') {
                write(data_fd, buf, current_packet_size);
                break; 
            }
            char *new_buf = realloc(buf, current_packet_size + RECV_BUF_SIZE);
            if (!new_buf) {
                free(buf);
                break;
            }
            buf = new_buf;
        }

        // Read all available history from the character driver and send back to client
        lseek(data_fd, 0, SEEK_SET);
        char read_buf[RECV_BUF_SIZE];
        ssize_t bytes_read;
        while ((bytes_read = read(data_fd, read_buf, sizeof(read_buf))) > 0) {
            send(client_fd, read_buf, bytes_read, 0);
        }

        close(data_fd);
        free(buf);
        close(client_fd);
        syslog(LOG_INFO, "Closed connection from %s", ip_str);
    }

    if (server_fd != -1) close(server_fd);

#if !USE_AESD_CHAR_DEVICE
    unlink(DATA_FILE);
#endif

    syslog(LOG_INFO, "Cleanup complete");
    closelog();

    return 0;
}