/*
 * Copyright (c) 2018, Jonathan Calmels <jbjcalmels@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <unistd.h>

#define VERSION "1.0.0"

#ifndef TMP_DIR
# define TMP_DIR "/dev/shm"
#endif

#ifndef ENV_KEY
# define ENV_KEY "DONKEY_FILE"
#endif

#define ignerr(func) __extension__ ({ int errsv = errno; func; errno = errsv; })

static inline void
xclose(int fd)
{
        if (close(fd) < 0)
                warn("failed to close fd %d", fd);
}

static int
listen_unix(void)
{
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        socklen_t addrlen = sizeof(addr);
        struct ucred cred;
        socklen_t credlen = sizeof(cred);
        int srv, conn;

        if ((srv = socket(PF_UNIX, SOCK_STREAM, 0)) < 0)
                return (-1);
        if (bind(srv, (const struct sockaddr *)&addr, sizeof(sa_family_t)) < 0)
                goto out_socket;
        if (getsockname(srv, (struct sockaddr *)&addr, &addrlen) < 0)
                goto out_socket;
        if (addrlen - sizeof(sa_family_t) <= 1) {
                errno = EADDRNOTAVAIL;
                goto out_socket;
        }
        if (listen(srv, 1) < 0)
                goto out_socket;

        if (write(STDOUT_FILENO, addr.sun_path + 1, strlen(addr.sun_path + 1)) < 0)
                goto out_socket;
        close(STDOUT_FILENO);

        if ((conn = accept4(srv, NULL, NULL, SOCK_NONBLOCK)) < 0)
                goto out_socket;
        if (getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred, &credlen) < 0)
                goto out_accept;
        if (cred.uid != 0 || cred.gid != 0) {
                errno = EPERM;
                goto out_accept;
        }
        xclose(srv);
        return (conn);

 out_accept:
        ignerr(close(conn));
 out_socket:
        ignerr(close(srv));
        return (-1);
}

static int
connect_unix(const char *path)
{
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        socklen_t addrlen;
        int fd;

        if ((fd = socket(PF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0)) < 0)
                return (-1);

        addr.sun_path[0] = '@';
        strncpy(addr.sun_path + 1, path, sizeof(addr.sun_path) - 2);
        addrlen = (socklen_t)SUN_LEN(&addr);
        addr.sun_path[0] = '\0';

        if (connect(fd, (const struct sockaddr *)&addr, addrlen) < 0)
                goto out_socket;
        return (fd);

 out_socket:
        ignerr(close(fd));
        return (-1);
}

static int
open_tmpfile(const char *tmp_dir, const char *env_key)
{
        char path[PATH_MAX];
        int fd;

        if ((fd = open(tmp_dir, O_RDWR|O_TMPFILE|O_EXCL|O_NONBLOCK, 0600)) < 0)
                return (-1);
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        if (setenv(env_key, path, 1) < 0)
                goto out_open;
        return (fd);

 out_open:
        ignerr(close(fd));
        return (-1);
}

static int
open_memfile(const char *file, void **addr, size_t *size)
{
        int fd;
        struct stat s;
        void *ptr;
        size_t len;

        if ((fd = open(file, O_RDONLY)) < 0)
                return (-1);
        if (fstat(fd, &s) < 0)
                goto out_open;
        len = (size_t)s.st_size;
        if ((ptr = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED)
                goto out_open;
        if (mlock(ptr, len) < 0)
                goto out_mmap;
        *addr = ptr;
        *size = len;
        xclose(fd);
        return (0);

 out_mmap:
        ignerr(munmap(ptr, len));
 out_open:
        ignerr(close(fd));
        return (-1);
}

static int
copy_data(int fd, void *addr, size_t size)
{
        int pfd[2];
        struct iovec iov = {addr, size};

        if (pipe(pfd) < 0)
                return (-1);
        for (ssize_t n, m, tx = 0; (size_t)tx < size; tx += m) {
                if ((n = vmsplice(pfd[1], &iov, 1, SPLICE_F_GIFT|SPLICE_F_NONBLOCK)) < 0) {
                        if (errno != EAGAIN)
                                goto out_pipe;
                        n = 0;
                }
                if ((m = splice(pfd[0], NULL, fd, NULL, size - (size_t)tx, SPLICE_F_MOVE|SPLICE_F_NONBLOCK)) < 0) {
                        if (errno != EAGAIN)
                                goto out_pipe;
                        m = 0;
                }
                iov.iov_base = (char *)iov.iov_base + n;
                iov.iov_len -= (size_t)n;
        }
        xclose(pfd[0]);
        xclose(pfd[1]);
        return (0);

 out_pipe:
        ignerr(close(pfd[0]));
        ignerr(close(pfd[1]));
        return (-1);
}

static int
copy_file(int fd_out, int fd_in)
{
        int pfd[2];
        bool eof = false;
        struct pollfd fds = {.fd = fd_in, .events = POLLIN|POLLRDHUP};

        if (pipe(pfd) < 0)
                return (-1);
        for (ssize_t n, m, rx = 0, tx = 0; !eof || tx < rx; rx += n, tx += m) {
                if (poll(&fds, 1, -1) < 0) /* XXX O_NONBLOCK is ignored for unix sockets, call poll to force block */
                        goto out_pipe;
                if ((n = splice(fd_in, NULL, pfd[1], NULL, PIPE_BUF, SPLICE_F_MOVE|SPLICE_F_NONBLOCK)) < 0) {
                        if (errno != EAGAIN)
                                goto out_pipe;
                        n = 0;
                } else if (n == 0) {
                        eof = true;
                }
                if ((m = splice(pfd[0], NULL, fd_out, NULL, PIPE_BUF, SPLICE_F_MOVE|SPLICE_F_NONBLOCK)) < 0) {
                        if (errno != EAGAIN)
                                goto out_pipe;
                        m = 0;
                }
        }
        xclose(pfd[0]);
        xclose(pfd[1]);
        return (0);

 out_pipe:
        ignerr(close(pfd[0]));
        ignerr(close(pfd[1]));
        return (-1);
}

noreturn static void
donkey_set(const char *file)
{
        struct rlimit limit = {0, 0};
        int fd;
        void *addr;
        size_t size;

        if (setrlimit(RLIMIT_CORE, &limit) < 0)
                err(EXIT_FAILURE, "could not set coredump limits");
        if ((fd = listen_unix()) < 0)
                err(EXIT_FAILURE, "could not establish connection");

        if (!strcmp(file, "-")) {
                if (copy_file(fd, STDIN_FILENO) < 0)
                        goto out_listen;
        } else {
                if (open_memfile(file, &addr, &size) < 0)
                        goto out_listen;
                if (copy_data(fd, addr, size) < 0)
                        goto out_mmap;
                if (munmap(addr, size) < 0)
                        goto out_listen;
        }
        xclose(fd);
        exit(EXIT_SUCCESS);

 out_mmap:
        ignerr(munmap(addr, size));
 out_listen:
        ignerr(close(fd));
        err(EXIT_FAILURE, "could not copy payload data");
}

noreturn static void
donkey_get(const char *id, const char * const cmd[])
{
        struct rlimit limit = {0, 0};
        int fd, tmp;

        if (setrlimit(RLIMIT_CORE, &limit) < 0)
                err(EXIT_FAILURE, "could not set coredump limits");
        if ((fd = connect_unix(id)) < 0)
                err(EXIT_FAILURE, "could not establish connection");

        if (cmd == NULL) {
                if (copy_file(STDOUT_FILENO, fd) < 0)
                        goto out_connect;
                xclose(fd);
                exit(EXIT_SUCCESS);
        } else {
                if ((tmp = open_tmpfile(TMP_DIR, ENV_KEY)) < 0)
                        goto out_connect;
                if (copy_file(tmp, fd) < 0)
                        goto out_open;
                execvp(cmd[0], (char * const *)cmd);
                ignerr(close(tmp));
                ignerr(close(fd));
                err(EXIT_FAILURE, "could not execute command");
        }

 out_open:
        ignerr(close(tmp));
 out_connect:
        ignerr(close(fd));
        err(EXIT_FAILURE, "could not copy payload data");
}

int
main(int argc, const char *argv[])
{
        if (argc == 3) {
                if (!strcmp(argv[1], "set"))
                        donkey_set(argv[2]);
                else if (!strcmp(argv[1], "get"))
                        donkey_get(argv[2], NULL);
        } else if (argc >= 4) {
                if (!strcmp(argv[1], "get"))
                        donkey_get(argv[2], &argv[3]);
        }

        fprintf(stderr, "version: %s\n", VERSION);
        fprintf(stderr, "usage: %s (set <filename> | get <id> [<command>])\n", basename(argv[0]));
        return (1);
}
