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

#include <unistd.h>

struct xpipe
{
    size_t buffer_size;
    char **command;
};

static int     configure(struct xpipe *xpipe, int argc, char **argv);
static int     run(struct xpipe *xpipe);
static int     parse_size_t(const char *str, size_t *value, size_t limit);
static ssize_t find_last(const char *data, size_t size, char ch);
static int     pipe_exec(char **argv, const char *data, size_t size);
static pid_t   open_pipe(char **argv, int *fd);
static int     write_all(int fd, const char *data, size_t size);

enum
{
    exit_panic = 255,
};

int main(int argc, char **argv)
{
    struct xpipe xpipe = {
        .buffer_size = 8192,
        .command     = NULL,
    };
    if (configure(&xpipe, argc, argv) == -1) {
        return 1;
    }
    if (run(&xpipe) == -1) {
        return 1;
    }
    return 0;
}

// configure initializes xpipe by parsing command-line arguments. Returns 0 on
// success. Returns -1 on error.
int configure(struct xpipe *xpipe, int argc, char **argv)
{
    for (int ch; (ch = getopt(argc, argv, "b:")) != -1; ) {
        switch (ch) {
          case 'b':
            if (parse_size_t(optarg, &xpipe->buffer_size, SIZE_MAX) == -1) {
                return -1;
            }
            break;

          default:
            return -1;
        }
    }
    xpipe->command = argv + optind;

    return 0;
}

// run executes the xpipe command: Reads stdin by chunk and sends lines in each
// chunk to a command via pipe.
int run(struct xpipe *xpipe)
{
    char *buffer = malloc(xpipe->buffer_size);
    if (buffer == NULL) {
        return -1;
    }

    size_t nused = 0;

    for (;;) {
        ssize_t bytes_read = read(
            STDIN_FILENO, buffer + nused, xpipe->buffer_size - nused);
        if (bytes_read == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }
        nused += (size_t) bytes_read;

        assert(nused <= xpipe->buffer_size);

        if (nused == xpipe->buffer_size) {
            ssize_t newline_pos = find_last(buffer, nused, '\n');
            if (newline_pos == -1) {
                return -1;
            }
            size_t size = (size_t) newline_pos + 1; // Include newline.

            if (pipe_exec(xpipe->command, buffer, size) == -1) {
                return -1;
            }

            memmove(buffer, buffer + size, nused - size);
            nused -= size;
        }
    }

    if (nused > 0 && pipe_exec(xpipe->command, buffer, nused) == -1) {
        return -1;
    }

    return 0;
}

// parse_size_t parses and validates a size_t from string and stores the result
// to given pointer (if not NULL). Returns 0 on success. Retunrs -1 on error.
int parse_size_t(const char *str, size_t *value, size_t limit)
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
    assert(*end == '\0');

    if (value) {
        *value = (size_t) result;
    }

    return 0;
}

// find_last returns the index of the last occurrence of ch in a memory range.
// Returns -1 if ch is not found.
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

// pipe_exec executes a command, writes data to its stdin and waits for exit.
// Returns 0 on success. Returns -1 on error.
int pipe_exec(char **argv, const char *data, size_t size)
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

// open_pipe launches a command with stdin bound to a new pipe. Returns the
// PID of the command process and assigns the write end of the pipe to *fd on
// success. Returns -1 on error.
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

// write_all writes data to a file, handling potential partial writes. Returns
// 0 on success. Returns -1 on error.
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
