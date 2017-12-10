// Distributed under the MIT License

#define _POSIX_C_SOURCE 200112L

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

struct config
{
    size_t bufsize;
    char **argv;
    time_t timeout;
};

static void    usage(void);
static int     configure(struct config *config, int argc, char **argv);
static int     run(const struct config *config);
static int     do_run(const struct config *config, char *buf);

static ssize_t pipe_lines(char **argv, const char *buf, size_t size);
static int     pipe_data(char **argv, const char *buf, size_t size);
static pid_t   open_pipe(char **argv, int *fd);

static int     write_all(int fd, const char *buf, size_t size);
static ssize_t try_read(int fd, char *buf, size_t size, const struct timeval *deadline);
static int     wait_input(int fd, const struct timeval *deadline);

static int     monoclock(struct timeval *time);
static void    sub(const struct timeval *t1, const struct timeval *t2, struct timeval *diff);
static void    normalize(struct timeval *time);

static int     parse_size(const char *str, size_t *value);
static int     parse_duration(const char *str, time_t *value);
static int     parse_uint(const char *str, uintmax_t *value, uintmax_t limit);
static ssize_t find_last(const char *buf, size_t size, char ch);

enum
{
    exit_panic = 255,
};

int main(int argc, char **argv)
{
    struct config config = {
        .bufsize = 8192,
        .argv    = NULL,
        .timeout = (time_t) -1,
    };

    if (configure(&config, argc, argv) == -1) {
        return 1;
    }
    if (run(&config) == -1) {
        return 1;
    }
    return 0;
}

// usage prints a help message to stderr.
void usage(void)
{
    const char *msg =
        "Usage: xpipe [-h] [-b bufsize] [-t timeout] command ...\n"
        "\n"
        "Options\n"
        "  -b bufsize  set buffer size in bytes\n"
        "  -t timeout  set read timeout in seconds\n"
        "  -h          show this help\n"
        "\n";
    fputs(msg, stderr);
}

// configure sets xpipe parameters to user-supplied values.
//
// Returns 0 on success or -1 on error.
int configure(struct config *config, int argc, char **argv)
{
    for (int ch; (ch = getopt(argc, argv, "b:t:h")) != -1; ) {
        switch (ch) {
          case 'b':
            if (parse_size(optarg, &config->bufsize) == -1) {
                fputs("xpipe: invalid buffer size\n", stderr);
                return -1;
            }
            break;

          case 't':
            if (parse_duration(optarg, &config->timeout) == -1) {
                fputs("xpipe: invalid timeout\n", stderr);
                return -1;
            }
            break;

          case 'h':
            usage();
            exit(0);

          default:
            return -1;
        }
    }
    config->argv = argv + optind;

    return 0;
}

// run executes the main functionality: Reads stdin by chunk and sends lines in
// each chunk to a command via pipe.
//
// Returns 0 on success or -1 on error.
int run(const struct config *config)
{
    char *buf = malloc(config->bufsize);
    if (buf == NULL) {
        perror("xpipe: failed to allocate memory");
        return -1;
    }

    int result = do_run(config, buf);
    free(buf);
    return result;
}

// do_run implements run() using given preallocated buffer.
//
// Returns 0 on success or -1 on error.
int do_run(const struct config *config, char *buf)
{
    size_t avail = 0;

    struct timeval deadline;
    struct timeval *active_deadline = NULL;

    for (;;) {
        ssize_t nb_read = try_read(
            STDIN_FILENO, buf + avail, config->bufsize - avail, active_deadline);
        if (nb_read == 0) {
            break;
        }
        if (nb_read == -1) {
            if (errno != EWOULDBLOCK) {
                perror("xpipe: failed to read stdin");
                return -1;
            }
            nb_read = 0; // Time out.
        }

        if (config->timeout > 0 && avail == 0 && nb_read > 0) {
            if (monoclock(&deadline) == -1) {
                return -1;
            }
            deadline.tv_sec += config->timeout;
            active_deadline = &deadline;
        }

        avail += (size_t) nb_read;

        if (avail == config->bufsize || nb_read == 0) {
            ssize_t used = pipe_lines(config->argv, buf, avail);
            if (used == -1) {
                perror("xpipe: failed to write to pipe");
                return -1;
            }
            avail -= (size_t) used;
            memmove(buf, buf + used, avail);

            active_deadline = NULL;
        }

        if (avail == config->bufsize) {
            fputs("xpipe: buffer full\n", stderr);
            return -1;
        }
    }

    if (avail > 0 && pipe_data(config->argv, buf, avail) == -1) {
        perror("xpipe: failed to write to pipe");
        return -1;
    }

    return 0;
}

// pipe_lines pipes lines to a command.
//
// The function does nothing and succeeds if the data does not contain any
// newline character.
//
// Returns the number of bytes piped on success or -1 on error.
ssize_t pipe_lines(char **argv, const char *buf, size_t size)
{
    ssize_t end_pos = find_last(buf, size, '\n');
    if (end_pos == -1) {
        return 0;
    }
    size_t use = (size_t) end_pos + 1; // Include newline.
    if (pipe_data(argv, buf, use) == -1) {
        return -1;
    }
    return (ssize_t) use;
}

// pipe_data executes a command, writes data to its stdin and waits for exit.
//
// Returns 0 on success or -1 on error.
int pipe_data(char **argv, const char *buf, size_t size)
{
    int pipe_wr;

    pid_t pid = open_pipe(argv, &pipe_wr);
    if (pid == -1) {
        return -1;
    }

    if (write_all(pipe_wr, buf, size) == -1) {
        // XXX: pipe_wr and pid leak if program recovers from this error.
        return -1;
    }
    if (close(pipe_wr) == -1) {
        // XXX: pid leaks if program recovers from this error.
        return -1;
    }

    int status;
    if (waitpid(pid, &status, 0) == -1) {
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        return -1;
    }

    return 0;
}

// open_pipe launches a command with stdin bound to a new pipe.
//
// Returns the PID of the command process and assigns the write end of the pipe
// to *fd on success. Returns -1 on error.
pid_t open_pipe(char **argv, int *fd)
{
    int fds[2];
    if (pipe(fds) == -1) {
        return -1;
    }
    int pipe_rd = fds[0];
    int pipe_wr = fds[1];

    pid_t pid = fork();
    if (pid == -1) {
        close(pipe_rd);
        close(pipe_wr);
        return -1;
    }

    if (pid == 0) {
        // Child process.
        if (close(pipe_wr) == -1) {
            exit(exit_panic);
        }
        if (dup2(pipe_rd, STDIN_FILENO) == -1) {
            exit(exit_panic);
        }
        execvp(argv[0], argv);
        exit(exit_panic);
    }

    // Parent process.
    if (close(pipe_rd) == -1) {
        // XXX: pid leaks if program recovers from this error.
        return -1;
    }
    *fd = pipe_wr;

    return pid;
}

// write_all writes data to a file, handling potential partial writes.
//
// Returns 0 on success or -1 on error.
int write_all(int fd, const char *buf, size_t size)
{
    while (size > 0) {
        ssize_t written = write(fd, buf, size);
        if (written == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        buf += written;
        size -= (size_t) written;
    }
    return 0;
}

// try_read attempts to read data from a blocking descriptor.
//
// deadline must be compatible with the timeval obtained via monoclock().
//
// Returns the number of bytes read on success, 0 on EOF, or -1 on timeout or
// error. errno is set to EWOULDBLOCK in case of timeout.
ssize_t try_read(int fd, char *buf, size_t size, const struct timeval *deadline)
{
    int ready = wait_input(fd, deadline);
    if (ready == -1) {
        return -1;
    }
    if (ready == 0) {
        errno = EWOULDBLOCK;
        return -1;
    }
    return read(fd, buf, size);
}

// wait_input waits for any data available for read from given descriptor or
// passing deadline.
//
// deadline must be compatible with the timeval obtained via monoclock().
//
// Returns 1 on receiving data, 0 on timeout, or -1 on any error.
int wait_input(int fd, const struct timeval *deadline)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    if (deadline) {
        struct timeval now;
        if (monoclock(&now) == -1) {
            return -1;
        }
        struct timeval timeout;
        sub(deadline, &now, &timeout);
        return select(fd + 1, &fds, NULL, NULL, &timeout);
    }
    return select(fd + 1, &fds, NULL, NULL, NULL);
}

// monoclock gets the current time point from a monotonic clock.
//
// Returns 0 on success or -1 on error.
int monoclock(struct timeval *time)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        return -1;
    }
    time->tv_sec = ts.tv_sec;
    time->tv_usec = (suseconds_t) (ts.tv_nsec / 1000);
    return 0;
}

// sub calculates the time difference t1 - t2.
void sub(const struct timeval *t1, const struct timeval *t2, struct timeval *diff)
{
    diff->tv_sec = t1->tv_sec - t2->tv_sec;
    diff->tv_usec = t1->tv_usec - t2->tv_usec;
    normalize(diff);
}

// normalize adjusts tv_usec of given time within 0 to 999999 with carries and
// borrows from tv_sec.
void normalize(struct timeval *time)
{
    const suseconds_t second_usec = 1000000;

    while (time->tv_usec >= second_usec) {
        time->tv_sec++;
        time->tv_usec -= second_usec;
    }

    while (time->tv_usec < 0) {
        time->tv_sec--;
        time->tv_usec += second_usec;
    }
}

// parse_size parses and validates a size_t from string and stores the result
// to given pointer (if not NULL).
//
// Returns 0 on success or -1 on error.
int parse_size(const char *str, size_t *value)
{
    uintmax_t uint;
    if (parse_uint(str, &uint, SIZE_MAX) == -1) {
        return -1;
    }
    if (value) {
        *value = (size_t) uint;
    }
    return 0;
}

// parse_duration parses and validates a time_t from string and stores the
// result to given pointer (if not NULL).
//
// Returns 0 on success or -1 on error.
int parse_duration(const char *str, time_t *value)
{
    uintmax_t uint;
    if (parse_uint(str, &uint, 0x7fffffff) == -1) {
        return -1;
    }
    if (value) {
        *value = (time_t) uint;
    }
    return 0;
}

// parse_uint parses unsigned integer from string with limit validation.
//
// Returns 0 on success or -1 on error.
int parse_uint(const char *str, uintmax_t *value, uintmax_t limit)
{
    errno = 0;
    char *end;
    intmax_t result = strtoimax(str, &end, 10);

    if (end == str) {
        return -1;
    }
    if (*end != '\0') {
        return -1;
    }
    if ((result == INTMAX_MAX || result == INTMAX_MIN) && errno == ERANGE) {
        return -1;
    }
    if (result < 0) {
        return -1;
    }
    if ((uintmax_t) result > limit) {
        return -1;
    }
    assert(errno == 0);

    if (value) {
        *value = (uintmax_t) result;
    }
    return 0;
}

// find_last searches data for the last occurrence of ch.
//
// Returns the index of the last occurrence of ch or -1 if ch is not found.
ssize_t find_last(const char *buf, size_t size, char ch)
{
    ssize_t pos = (ssize_t) size - 1;

    for (; pos >= 0; pos--) {
        if (buf[pos] == ch) {
            break;
        }
    }
    return pos;
}
