#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syslog.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024
#define TIMER_INTERVAL_SEC 10

// Global variables for signal handling
static volatile bool signal_received = false;
static int server_fd = -1;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread data structure for client connections
typedef struct thread_node
{
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    bool thread_complete;
    struct thread_node *next;
} thread_node_t;

// Timer thread data
typedef struct
{
    pthread_t thread_id;
    bool active;
} timer_data_t;

// Head of the linked list
static thread_node_t *thread_list_head = NULL;
static pthread_mutex_t list_mutex = PTHREAD_MUTEX_INITIALIZER;

void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM)
    {
        syslog(LOG_INFO, "Caught signal, exiting");
        signal_received = true;
    }
}

int setup_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;

    if (sigaction(SIGINT, &sa, NULL) == -1)
    {
        syslog(LOG_ERR, "Failed to setup SIGINT handler: %s", strerror(errno));
        return -1;
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1)
    {
        syslog(LOG_ERR, "Failed to setup SIGTERM handler: %s", strerror(errno));
        return -1;
    }

    return 0;
}

void cleanup_threads(void)
{
    pthread_mutex_lock(&list_mutex);

    thread_node_t *current = thread_list_head;
    thread_node_t *next;

    while (current != NULL)
    {
        next = current->next;

        // Joint the thread if not already joined
        if (!current->thread_complete)
        {
            pthread_join(current->thread_id, NULL);
        }

        if (current->client_fd != 1)
        {
            close(current->client_fd);
        }

        free(current);
        current = next;
    }

    thread_list_head = NULL;
    pthread_mutex_unlock(&list_mutex);
}

void cleanup(void)
{
    cleanup_threads();

    if (server_fd != -1)
    {
        close(server_fd);
        server_fd = -1;
    }

    if (unlink(DATA_FILE) == -1 && errno != ENOENT)
    {
        syslog(LOG_ERR, "Failed to delete data file: %s", strerror(errno));
    }

    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&list_mutex);
    closelog();
}

int daemonize(void)
{
    pid_t pid = fork();

    if (pid < 0)
    {
        syslog(LOG_ERR, "Fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid > 0)
    {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0)
    {
        syslog(LOG_ERR, "setsid failed: %s", strerror(errno));
        return -1;
    }

    if (chdir("/") < 0)
    {
        syslog(LOG_ERR, "chdir failed: %s", strerror(errno));
        return -1;
    }

    // Close standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // Redirect standard file descriptors to /dev/null
    int dev_null = open("/dev/null", O_RDWR);
    if (dev_null == -1)
    {
        syslog(LOG_ERR, "Failed to open /dev/null: %s", strerror(errno));
        return -1;
    }

    dup2(dev_null, STDIN_FILENO);
    dup2(dev_null, STDOUT_FILENO);
    dup2(dev_null, STDERR_FILENO);

    if (dev_null > STDERR_FILENO)
    {
        close(dev_null);
    }

    return 0;
}

void *handle_connection(void *arg)
{
    thread_node_t *node = (thread_node_t *)arg;
    int client_fd = node->client_fd;
    char buffer[BUFFER_SIZE];
    size_t bytes_received;
    bool packet_complete = false;
    int data_fd = -1;

    data_fd = open(DATA_FILE, O_CREAT | O_RDWR | O_APPEND, 0644);
    if (data_fd == -1)
    {
        syslog(LOG_ERR, "Failed to open data file: %s", strerror(errno));
        node->thread_complete = true;
        return NULL;
    }

    // Receive data from client
    while (!signal_received)
    {
        bytes_received = recv(client_fd, buffer, BUFFER_SIZE, 0);

        if (bytes_received < 0)
        {
            syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            close(data_fd);
            node->thread_complete = true;
            return NULL;
        }

        if (bytes_received == 0)
        {
            // Connection closed by client
            break;
        }

        // Lock mutex before writing to file
        pthread_mutex_lock(&file_mutex);
        ssize_t bytes_written = write(data_fd, buffer, bytes_received);
        pthread_mutex_unlock(&file_mutex);

        if (bytes_written != (ssize_t)bytes_received)
        {
            syslog(LOG_ERR, "Failed to write to data file: %s", strerror(errno));
            close(data_fd);
            node->thread_complete = true;
            return NULL;
        }

        // Check if packet is complete (contains newline)
        for (size_t i = 0; i < bytes_received; i++)
        {
            if (buffer[i] == '\n')
            {
                packet_complete = true;
                break;
            }
        }

        if (packet_complete)
        {
            break;
        }
    }

    fsync(data_fd);
    close(data_fd);

    // Send file contents back to client
    // Lock mutex before reading to ensure consistency
    pthread_mutex_lock(&file_mutex);

    data_fd = open(DATA_FILE, O_RDONLY);
    if (data_fd == -1)
    {
        syslog(LOG_ERR, "Failed to open data file for reading: %s", strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        node->thread_complete = true;
        return NULL;
    }

    // Get file size to read only existing data
    off_t file_size = lseek(data_fd, 0, SEEK_END);
    lseek(data_fd, 0, SEEK_SET);
    off_t bytes_to_read = file_size;

    while (!signal_received && bytes_to_read > 0)
    {
        size_t read_size = (bytes_to_read < BUFFER_SIZE) ? bytes_to_read : BUFFER_SIZE;
        ssize_t bytes_read = read(data_fd, buffer, read_size);

        if (bytes_read < 0)
        {
            syslog(LOG_ERR, "Failed to read from data file: %s", strerror(errno));
            close(data_fd);
            pthread_mutex_unlock(&file_mutex);
            node->thread_complete = true;
            return NULL;
        }

        if (bytes_read == 0)
        {
            // End of file
            break;
        }

        bytes_to_read -=bytes_read;

        ssize_t bytes_sent = send(client_fd, buffer, bytes_read, 0);
        if (bytes_sent != bytes_read)
        {
            syslog(LOG_ERR, "Send failed: %s", strerror(errno));
            close(data_fd);
            pthread_mutex_unlock(&file_mutex);
            node->thread_complete = true;
            return NULL;
        }
    }

    close(data_fd);
    pthread_mutex_unlock(&file_mutex);

    // Log closed connection
    char* client_ip = inet_ntoa(node->client_addr.sin_addr);
    syslog(LOG_INFO, "Closed connection from %s", client_ip);

    node->thread_complete = true;
    return NULL;
}

void *timer_thread(void *arg)
{
    timer_data_t* timer_data = (timer_data_t*)arg;
    char timestamp[200];
    time_t now;
    struct tm* timeinfo;

    while (!signal_received && timer_data->active)
    {
        sleep(TIMER_INTERVAL_SEC);

        if (signal_received || !timer_data->active)
        {
            break;
        }

        // Get current time
        time(&now);
        timeinfo = localtime(&now);

        // Format timestamp according to RFC 2822
        strftime(timestamp, sizeof(timestamp), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", timeinfo);

        // Write timestamp to file with mutex protection
        pthread_mutex_lock(&file_mutex);

        int data_fd = open(DATA_FILE, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (data_fd != -1)
        {
            ssize_t bytes_written = write(data_fd, timestamp, strlen(timestamp));
            if (bytes_written == -1)
            {
                syslog(LOG_ERR, "Timer thread: failed to write timestamp: %s", strerror(errno));
            }
            close(data_fd);
        }
        else
        {
            syslog(LOG_ERR, "Timer thread: Failed to open data file: %s", strerror(errno));
        }

        pthread_mutex_unlock(&file_mutex);
    }

    return NULL;
}

void add_thread_to_list(thread_node_t *node)
{
    pthread_mutex_lock(&list_mutex);

    node->next = thread_list_head;
    thread_list_head = node;

    pthread_mutex_unlock(&list_mutex);
}

void join_completed_threads(void)
{
    pthread_mutex_lock(&list_mutex);

    thread_node_t *current = thread_list_head;
    thread_node_t *prev = NULL;

    while (current != NULL)
    {
        if (current->thread_complete)
        {
            // Joint the completed thread
            pthread_join(current->thread_id, NULL);

            // Close client socket
            if (current->client_fd != -1)
            {
                close(current->client_fd);
                current->client_fd = -1;
            }

            // Remove from list
            if (prev == NULL)
            {
                thread_list_head = current->next;
            }
            else
            {
                prev->next = current->next;
            }

            thread_node_t *to_free = current;
            current = current->next;
            free(to_free);
        }
        else
        {
            prev = current;
            current = current->next;
        }
    }

    pthread_mutex_unlock(&list_mutex);
}

int main(int argc, char* argv[])
{
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int opt = 1;
    bool daemon_mode = false;
    timer_data_t timer_data = {0};

    // Opening syslog
    openlog("aesdsocket", LOG_PID, LOG_USER);

    // Parse command line arguments
    if (argc > 1 && strcmp(argv[1], "-d") == 0)
    {
        daemon_mode = true;
    }

    // Setup signal handlers
    if (setup_signal_handlers() == -1)
    {
        cleanup();
        return -1;
    }

    // Create socket (I use AF_INET. PF_INET is deprecated)
    server_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        syslog(LOG_ERR, "Socket creation failed: %s", strerror(errno));
        cleanup();
        return -1;
    }

    // Set socket options to reuse address
    // This allows the server to bind to the port immediately after restart,
    // even if the port is still in TIME_WAIT state from a previous connection.
    // Without this, you would get "Address already in use" error and have to wait.
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        cleanup();
        return -1;
    }

    // Bind socket
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1)
    {
        syslog(LOG_ERR, "Bind failed: %s", strerror(errno));
        cleanup();
        return -1;
    }

    // Daemonize if requested
    if (daemon_mode)
    {
        if (daemonize() == -1)
        {
            cleanup();
            return -1;
        }
    }

    // Listen for connections
    if (listen(server_fd, 10) == -1)
    {
        syslog(LOG_ERR, "Listen failed: %s", strerror(errno));
        cleanup();
        return -1;
    }

    syslog(LOG_INFO, "Server listening on port %d", PORT);

    // Start timer thread
    timer_data.active = true;
    if (pthread_create(&timer_data.thread_id, NULL, timer_thread, &timer_data) != 0)
    {
        syslog(LOG_ERR, "Failed to create timer thread: %s", strerror(errno));
        cleanup();
        return -1;
    }

    // Main server loop
    while (!signal_received)
    {
        // Join any completed threads
        join_completed_threads();

        // Accept connection
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_len);

        if (client_fd == -1)
        {
            if (errno == EINTR)
            {
                // Interrupted by signal
                continue;
            }
            syslog(LOG_ERR, "Accept failed: %s", strerror(errno));
            continue;
        }

        char* client_ip = inet_ntoa(client_addr.sin_addr);
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        // Create thread node
        thread_node_t* node = malloc(sizeof(thread_node_t));
        if (node == NULL)
        {
            syslog(LOG_ERR, "Failed to allocate thread node");
            close(client_fd);
            continue;
        }

        node->client_fd = client_fd;
        node->client_addr = client_addr;
        node->thread_complete = false;
        node->next = NULL;

        // Create thread for this connection
        if (pthread_create(&node->thread_id, NULL, handle_connection, node) != 0)
        {
            syslog(LOG_ERR, "Failed to create thread: %s", strerror(errno));
            close(client_fd);
            free(node);
            continue;
        }

        add_thread_to_list(node);
    }

    // Signal timer thread to stop
    timer_data.active = false;
    pthread_join(timer_data.thread_id, NULL);

    cleanup();
    return 0;
}
