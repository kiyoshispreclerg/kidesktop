#define _GNU_SOURCE
#include "xis_rootexec.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Fixed locations only: a $PATH entry of the user's own must not be able to
 * stand in for pkexec. */
static const char *const PKEXEC_PATHS[] = { "/usr/bin/pkexec", "/bin/pkexec", "/usr/local/bin/pkexec", NULL };

static const char *find_pkexec(void)
{
    for (int i = 0; PKEXEC_PATHS[i]; i++)
        if (access(PKEXEC_PATHS[i], X_OK) == 0)
            return PKEXEC_PATHS[i];
    return NULL;
}

int xis_rootexec_available(void)
{
    return find_pkexec() != NULL;
}

/* av[0] is the pkexec path. */
static int run(char *const av[], const char *in, size_t in_len)
{
    int p[2];
    if (pipe(p) != 0)
        return -1;
    /* A dead child must not kill the caller on write(). */
    struct sigaction ign = { .sa_handler = SIG_IGN }, old;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, &old);

    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        sigaction(SIGPIPE, &old, NULL);
        return -1;
    }
    if (pid == 0) {
        sigaction(SIGPIPE, &old, NULL);
        dup2(p[0], 0);
        close(p[0]);
        close(p[1]);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) {
            dup2(nul, 1);
            if (nul > 2)
                close(nul);
        }
        execv(av[0], av);
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
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int run_with(const char *const head[], size_t nhead, char *const tail[], const char *in, size_t in_len)
{
    const char *pk = find_pkexec();
    if (!pk)
        return XIS_ROOTEXEC_NOT_RUN;
    size_t nt = 0;
    while (tail && tail[nt])
        nt++;
    char **av = calloc(nhead + nt + 2, sizeof *av);
    if (!av)
        return -1;
    size_t n = 0;
    av[n++] = (char *)pk;
    for (size_t i = 0; i < nhead; i++)
        av[n++] = (char *)head[i];
    for (size_t i = 0; i < nt; i++)
        av[n++] = tail[i];
    int rc = run(av, in, in_len);
    free(av);
    return rc;
}

int xis_rootexec(char *const argv[], const char *in, size_t in_len)
{
    return run_with(NULL, 0, argv, in, in_len);
}

int xis_rootexec_script(const char *script, char *const args[], const char *in)
{
    const char *head[] = { "/bin/sh", "-c", script, "sh" };
    return run_with(head, 4, args, in, in ? strlen(in) : 0);
}
