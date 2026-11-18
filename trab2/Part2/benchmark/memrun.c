/*
 * memrun - run a command and report its wall-clock time and peak memory.
 *
 *   memrun result.txt command [args...]
 *
 * Writes "<seconds> <peak RSS in KB>" to result.txt and exits with the command's
 * exit code. It exists because a process forked from Python inherits Python's
 * memory high-water mark, so measuring from Python directly reports Python's size;
 * memrun is small, so what it reports is the command's own peak.
 *
 * Build: cc -O2 -o memrun memrun.c
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: memrun result.txt command [args...]\n");
        return 2;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[2], argv + 2);
        perror(argv[2]);
        _exit(127);
    }
    int status;
    struct rusage ru;
    if (pid < 0 || wait4(pid, &status, 0, &ru) < 0) {
        perror("memrun");
        return 2;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    FILE *f = fopen(argv[1], "w");
    if (f) {
        fprintf(f, "%.6f %ld\n", (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9, ru.ru_maxrss);
        fclose(f);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
