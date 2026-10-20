#include "syscall.h"
#include "string.h"
#include "shell.h"

#define XARGS_BUFSZ 1024
#define XARGS_MAX_ITEMS 32
#define XARGS_EXECFD 3
#define XARGS_MAX_ARGS (XARGS_MAX_ITEMS + 2) // A little larger than MAX_ITEMS to fit in extra args

static int is_space(char c)
{
    return (c == ' ' || c == '\n' || c == '\t' || c == '\r');
}

void main(int argc, char *argv[])
{
    if (argc < 2) {
        dprintf(CONSOLEOUT, "xargs: missing command\n");
        _exit();
    }

    char input[XARGS_BUFSZ];
    int pos = 0;

    while (pos < XARGS_BUFSZ - 1) {
        long n = _read(STDIN, input + pos, XARGS_BUFSZ - pos - 1);
        if (n < 0) {
            dprintf(CONSOLEOUT, "xargs: read error\n");
            _exit();
        }
        if (n == 0)
            break; // EOF
        pos += (int)n;
    }
    input[pos] = '\0';

    // Split STDIN
    char *items[XARGS_MAX_ITEMS];
    int itemc = 0;
    char *p = input;

    while (*p != '\0' && itemc < XARGS_MAX_ITEMS) {
        // Skip space
        while (*p != '\0' && is_space(*p))
            p++;
        if (*p == '\0')
            break;

        char *start = p;
        while (*p != '\0' && !is_space(*p))
            p++;
        if (*p != '\0')
            *p++ = '\0';

        items[itemc++] = start;
    }

    // Construct cmd path
    const char *cmd_name = argv[1];
    const char *cmd_path;
    char pathbuf[128];

    if (strchr(cmd_name, '/') == NULL) {
        // Add "c/" if no "c/"
        snprintf(pathbuf, sizeof(pathbuf), "c/%s", cmd_name);
        cmd_path = pathbuf;
    } else {
        cmd_path = cmd_name;
    }

    // Construct argv
    char *exec_argv[XARGS_MAX_ARGS + 1];
    int eargc = 0;

    exec_argv[eargc++] = (char *)cmd_path;

    for (int i = 2; i < argc && eargc < XARGS_MAX_ARGS; i++) {
        exec_argv[eargc++] = argv[i];
    }

    // augment items from STDIN
    for (int i = 0; i < itemc && eargc < XARGS_MAX_ARGS; i++) {
        exec_argv[eargc++] = items[i];
    }

    exec_argv[eargc] = NULL;

    // Open the program to run
    _close(XARGS_EXECFD);
    int result = _open(XARGS_EXECFD, cmd_path);
    if (result < 0) {
        dprintf(CONSOLEOUT, "xargs: cannot open %s (err %d)\n", cmd_path, result);
        _exit();
    }

    // Run fork and exec
    int pid = _fork();
    if (pid < 0) {
        dprintf(CONSOLEOUT, "xargs: fork failed\n");
        _exit();
    }

    if (pid == 0) {
        // Child process
        int er = _exec(XARGS_EXECFD, eargc, exec_argv);
        dprintf(CONSOLEOUT, "xargs: exec %s failed (%d)\n", cmd_path, er);
        _exit();
    } else {
        // Parent waiting child
        _wait(pid);
        _exit();
    }
}
