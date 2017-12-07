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

#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

struct xpipe
{
    char  *buf;
    size_t bufsize;
    char **argv;
    time_t timeout;
};

static void    usage(void);
static int     init(struct xpipe *xpipe, int argc, char **argv);
static void    clean(const struct xpipe *xpipe);
static int     run(const struct xpipe *xpipe);
static int     parse_size(const char *str, size_t *value);
static int     parse_duration(const char *str, time_t *value);
static int     parse_uint(const char *str, uintmax_t *value, uintmax_t limit);
static ssize_t find_last(const char *data, size_t size, char ch);
static ssize_t pipe_lines(char **argv, const char *data, size_t size);
static int     pipe_data(char **argv, const char *data, size_t size);
static pid_t   open_pipe(char **argv, int *fd);
static int     write_all(int fd, const char *data, size_t size);
static ssize_t try_read(int fd, char *buf, size_t size, time_t timeout);
static int     wait_input(int fd, time_t timeout);

enum
{
    exit_panic = 255,
};

int main(int argc, char **argv)
{
    struct xpipe xpipe;

    if (init(&xpipe, argc, argv) == -1) {
        return 1;
    }

    int rc = run(&xpipe);
    clean(&xpipe);

    return rc == -1;
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

// init initializes xpipe using predefined defaults and command-line arguments.
//
// Returns 0 on success or -1 on error.
int init(struct xpipe *xpipe, int argc, char **argv)
{
    *xpipe = (struct xpipe) {
        .buf     = NULL,
        .bufsize = 8192,
        .argv    = NULL,
        .timeout = (time_t) -1,
    };

    for (int ch; (ch = getopt(argc, argv, "b:t:h")) != -1; ) {
        switch (ch) {
          case 'b':
            if (parse_size(optarg, &xpipe->bufsize) == -1) {
                fputs("xpipe: invalid buffer size\n", stderr);
                return -1;
            }
            break;

          case 't':
            if (parse_duration(optarg, &xpipe->timeout) == -1) {
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
    xpipe->argv = argv + optind;

    xpipe->buf = malloc(xpipe->bufsize);
    if (xpipe->buf == NULL) {
        perror("xpipe: buffer allocation failed");
        return -1;
    }

    return 0;
}

// clean frees resources associated to xpipe.
void clean(const struct xpipe *xpipe)
{
    free(xpipe->buf);
}

// run executes the main functionality: Reads stdin by chunk and sends lines in
// each chunk to a command via pipe.
int run(const struct xpipe *xpipe)
{
    size_t avail = 0;

    for (;;) {
        ssize_t nb_read = try_read(
            STDIN_FILENO, xpipe->buf + avail, xpipe->bufsize - avail,
            xpipe->timeout);
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
        avail += (size_t) nb_read;

        if (avail == xpipe->bufsize || nb_read == 0) {
            ssize_t used = pipe_lines(xpipe->argv, xpipe->buf, avail);
            if (used == -1) {
                perror("xpipe: failed to write to pipe");
                return -1;
            }
            avail -= (size_t) used;
            memmove(xpipe->buf, xpipe->buf + used, avail);
        }

        if (avail == xpipe->bufsize) {
            fputs("xpipe: buffer full\n", stderr);
            return -1;
        }
    }

    if (avail > 0 && pipe_data(xpipe->argv, xpipe->buf, avail) == -1) {
        perror("xpipe: failed to write to pipe");
        return -1;
    }

    return 0;
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
ssize_t find_last(const char *data, size_t size, char ch)
{
    ssize_t pos = (ssize_t) size - 1;

    for (; pos >= 0; pos--) {
        if (data[pos] == ch) {
            break;
        }
    }
    return pos;
}

// pipe_lines pipes lines to a command.
//
// The function succeeds and does nothing if the data does not contain any
// newline character.
//
// Returns the number of bytes piped on success or -1 on error.
ssize_t pipe_lines(char **argv, const char *data, size_t size)
{
    ssize_t end_pos = find_last(data, size, '\n');
    if (end_pos == -1) {
        return 0;
    }
    size_t use = (size_t) end_pos + 1; // Include newline.
    if (pipe_data(argv, data, use) == -1) {
        return -1;
    }
    return (ssize_t) use;
}

// pipe_data executes a command, writes data to its stdin and waits for exit.
//
// Returns 0 on success or -1 on error.
int pipe_data(char **argv, const char *data, size_t size)
{
    int pipe_wr;

    pid_t pid = open_pipe(argv, &pipe_wr);
    if (pid == -1) {
        return -1;
    }

    if (write_all(pipe_wr, data, size) == -1) {
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
int write_all(int fd, const char *data, size_t size)
{
    while (size > 0) {
        ssize_t written = write(fd, data, size);
        if (written == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        data += written;
        size -= (size_t) written;
    }
    return 0;
}

// try_read attempts to read data from a blocking descriptor.
//
// Timeout is not imposed (thus indefinitely blocks until data is read) if
// timeout is set to (time_t) -1.
//
// Returns the number on bytes read on success, 0 on EOF, or -1 on timeout or
// error. errno is set to EWOULDBLOCK in case of timeout.
ssize_t try_read(int fd, char *buf, size_t size, time_t timeout)
{
    if (timeout != (time_t) -1) {
        int ready = wait_input(fd, timeout);
        if (ready == -1) {
            return -1;
        }
        if (ready == 0) {
            errno = EWOULDBLOCK;
            return -1;
        }
    }
    return read(fd, buf, size);
}

// wait_input waits for any data available for read from given descriptor or
// timeout.
//
// Returns 1 on receiving data, 0 on timeout, or -1 on any error.
int wait_input(int fd, time_t timeout)
{
    struct timeval timeout_tv = {
        .tv_sec  = timeout,
        .tv_usec = 0,
    };
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    return select(1, &fds, NULL, NULL, &timeout_tv);
}
