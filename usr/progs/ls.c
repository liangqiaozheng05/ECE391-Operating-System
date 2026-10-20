#include "syscall.h"
#include "string.h"
#include "shell.h"

#define LS_FD 3
#define LS_BUFSZ 512

void main(int argc, char *argv[])
{
    const char *dir_arg;
    char path[32];
    int ret;
    char buf[LS_BUFSZ];

    if (argc < 2) {
        dir_arg = "/";
    } else {
        dir_arg = argv[1];
    }

    if (dir_arg[0] == '/' && dir_arg[1] == '\0') {
        snprintf(path, sizeof(path), "/");
    } else {
        size_t len = strlen(dir_arg);
        if (len > 0 && dir_arg[len - 1] == '/') {
            if (len >= sizeof(path))
                len = sizeof(path) - 1;
            memcpy(path, dir_arg, len);
            path[len] = '\0';
        } else {
            snprintf(path, sizeof(path), "%s/", dir_arg);
        }
    }
    
    if (strcmp(dir_arg, "/") == 0) {
        // Assume default mount point "c/"
        snprintf(path, sizeof(path), "c/");
    }

    ret = _open(LS_FD, path);
    if (ret < 0) {
        dprintf(CONSOLEOUT, "ls: cannot open listing for %s (try c/)\n", path);
        _exit();
    }

    for (;;) {
        long r = _read(LS_FD, buf, sizeof(buf) - 1);
        if (r < 0) {
            dprintf(CONSOLEOUT, "ls: error reading\n");
            break;
        }
        if (r == 0) {
            break;  // End
        }

        buf[r] = '\0';
        dprintf(STDOUT, "%s\n", buf);
    }

    _close(LS_FD);
    _exit();
}