#define _GNU_SOURCE
#include "xis_rootexec.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int xis_rootexec(char *const argv[], const char *in, size_t in_len)
{
    size_t n = 0;
    while (argv[n])
        n++;
    char **av = calloc(n + 2, sizeof *av);
    if (!av)
        return -1;
    av[0] = "pkexec";
    for (size_t i = 0; i < n; i++)
        av[i + 1] = argv[i];

    int p[2] = { -1, -1 };
    if (pipe(p) != 0) {
        free(av);
        return -1;
    }
    /* A dead child must not kill the caller on write(). */
    struct sigaction ign = { .sa_handler = SIG_IGN }, old;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, &old);

    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        sigaction(SIGPIPE, &old, NULL);
        free(av);
        return -1;
    }
    if (pid == 0) {
        dup2(p[0], 0);
        close(p[0]);
        close(p[1]);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) {
            dup2(nul, 1);
            dup2(nul, 2);
            if (nul > 2)
                close(nul);
        }
        execvp("pkexec", av);
        _exit(127);
    }
    close(p[0]);
    size_t off = 0;
    while (in && off < in_len) {
        ssize_t w = write(p[1], in + off, in_len - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        off += (size_t)w;
    }
    close(p[1]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    sigaction(SIGPIPE, &old, NULL);
    free(av);
    if (!WIFEXITED(st))
        return -1;
    int rc = WEXITSTATUS(st);
    return rc == 127 ? -1 : rc;
}
