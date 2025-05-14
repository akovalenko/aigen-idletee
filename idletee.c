#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>
#include <stdbool.h>

#define BUFFER_SIZE 4096
#define DEFAULT_IDLE_TIMEOUT 5  // seconds
#define DEFAULT_IDLE_TO_ACTIVE_THRESHOLD (2 * 60)  // 2 minutes
#define DEFAULT_ACTIVE_TO_IDLE_THRESHOLD (3 * 60)  // 3 minutes

// Configuration structure
typedef struct {
    int idle_timeout;  // seconds
    int idle_to_active_threshold;  // seconds
    int active_to_idle_threshold;  // seconds
    char *idle_to_active_command;
    char *active_to_idle_command;
    char *eof_command;
} Config;

void print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s [options]\n", program_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -t SECONDS   Set idle timeout (default: %d seconds)\n", DEFAULT_IDLE_TIMEOUT);
    fprintf(stderr, "  -i SECONDS   Set idle to active threshold (default: %d seconds)\n", DEFAULT_IDLE_TO_ACTIVE_THRESHOLD);
    fprintf(stderr, "  -a SECONDS   Set active to idle threshold (default: %d seconds)\n", DEFAULT_ACTIVE_TO_IDLE_THRESHOLD);
    fprintf(stderr, "  -I COMMAND   Command to run on transition from idle to active\n");
    fprintf(stderr, "  -A COMMAND   Command to run on transition from active to idle\n");
    fprintf(stderr, "  -E COMMAND   Command to run on EOF\n");
    fprintf(stderr, "  -h           Show this help message\n");
    exit(EXIT_FAILURE);
}

Config parse_args(int argc, char *argv[]) {
    Config config = {
        .idle_timeout = DEFAULT_IDLE_TIMEOUT,
        .idle_to_active_threshold = DEFAULT_IDLE_TO_ACTIVE_THRESHOLD,
        .active_to_idle_threshold = DEFAULT_ACTIVE_TO_IDLE_THRESHOLD,
        .idle_to_active_command = NULL,
        .active_to_idle_command = NULL,
        .eof_command = NULL
    };

    int opt;
    while ((opt = getopt(argc, argv, "t:i:a:I:A:E:h")) != -1) {
        switch (opt) {
            case 't':
                config.idle_timeout = atoi(optarg);
                if (config.idle_timeout <= 0) {
                    fprintf(stderr, "Idle timeout must be positive\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'i':
                config.idle_to_active_threshold = atoi(optarg);
                if (config.idle_to_active_threshold <= 0) {
                    fprintf(stderr, "Idle to active threshold must be positive\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'a':
                config.active_to_idle_threshold = atoi(optarg);
                if (config.active_to_idle_threshold <= 0) {
                    fprintf(stderr, "Active to idle threshold must be positive\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'I':
                config.idle_to_active_command = optarg;
                break;
            case 'A':
                config.active_to_idle_command = optarg;
                break;
            case 'E':
                config.eof_command = optarg;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                break;
        }
    }

    return config;
}

int main(int argc, char *argv[]) {
    Config config = parse_args(argc, argv);
    
    // Set stdin to non-blocking mode
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        exit(EXIT_FAILURE);
    }
    if (fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        exit(EXIT_FAILURE);
    }
    
    char buffer[BUFFER_SIZE];
    time_t last_data_time = time(NULL);
    time_t state_start_time = time(NULL);
    bool is_idle = true;  // Start in idle state
    bool eof_reached = false;
    
    while (!eof_reached) {
        // Check for data on stdin with a timeout
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        
        struct timeval tv;
        tv.tv_sec = 1;  // Check every second
        tv.tv_usec = 0;
        
        int ret = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &tv);
        if (ret == -1) {
            perror("select");
            exit(EXIT_FAILURE);
        }
        
        time_t current_time = time(NULL);
        
        // Process data if available
        if (ret > 0 && FD_ISSET(STDIN_FILENO, &read_fds)) {
            ssize_t bytes_read = read(STDIN_FILENO, buffer, BUFFER_SIZE);
            
            if (bytes_read > 0) {
                // Write the data to stdout
                ssize_t bytes_written = 0;
                while (bytes_written < bytes_read) {
                    ssize_t ret = write(STDOUT_FILENO, buffer + bytes_written, bytes_read - bytes_written);
                    if (ret == -1) {
                        if (errno == EINTR) {
                            continue;  // Interrupted by signal, try again
                        }
                        perror("write");
                        exit(EXIT_FAILURE);
                    }
                    bytes_written += ret;
                }
                
                // Check for transition from idle to active
                if (is_idle) {
                    time_t idle_duration = current_time - state_start_time;
                    if (idle_duration >= config.idle_to_active_threshold && config.idle_to_active_command) {
                        system(config.idle_to_active_command);
                    }
                    
                    is_idle = false;
                    state_start_time = current_time;
                }
                
                // Update the last data time AFTER state transition check
                last_data_time = current_time;
            } else if (bytes_read == 0) {
                // EOF reached
                eof_reached = true;
                if (config.eof_command) {
                    system(config.eof_command);
                }
            } else {
                // Error occurred
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("read");
                    exit(EXIT_FAILURE);
                }
            }
        }
        
        // Calculate time_since_last_data AFTER potentially updating last_data_time
        time_t time_since_last_data = current_time - last_data_time;
        
        // Check for transition to idle state (when idle timeout is reached)
        if (!is_idle && time_since_last_data >= config.idle_timeout) {
            time_t active_duration = current_time - state_start_time;
            if (active_duration >= config.active_to_idle_threshold && config.active_to_idle_command) {
                system(config.active_to_idle_command);
            }
            
            is_idle = true;
            state_start_time = current_time;
        }
    }
    
    return EXIT_SUCCESS;
}
