/*
 * memrun.c - run a command and report its wall time and peak memory.
 *
 *   memrun stats_file command [args...]
 *
 * Writes "<wall seconds> <peak RSS KiB>" to stats_file and exits with the
 * command's exit code. Used by benchmark.py: a child's ru_maxrss also counts
 * the memory of the process it was forked from, so launching tools directly
 * from Python would report Python's ~18 MiB for every tool. Forking from this
 * small program keeps that baseline at ~1 MiB.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    struct timespec t0, t1;
    struct rusage   ru;
    int             status;
    pid_t           pid;
    FILE           *f;

    if (argc < 3) {
        fprintf(stderr, "usage: memrun stats_file command [args...]\n");
        return 2;
    }

    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 2;
    }
    if (pid == 0) {
        execvp(argv[2], argv + 2);
        perror(argv[2]);
        _exit(127);
    }
    if (wait4(pid, &status, 0, &ru) < 0) {
        perror("wait4");
        return 2;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    f = fopen(argv[1], "w");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fprintf(f, "%.6f %ld\n",
            (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9,
            ru.ru_maxrss);
    fclose(f);

    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
