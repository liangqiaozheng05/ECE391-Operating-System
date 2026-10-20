#include "syscall.h"
#include "string.h"
#include "shell.h"

void main(int argc, char *argv[])
{
    char pathbuf[128];
    char *path;
    if (argc < 2) {
        dprintf(CONSOLEOUT, "touch: missing operand\n");
        _exit();
    }

    for (int i = 1; i < argc; i++) {
        if (strchr(argv[i], '/') == NULL) {
            snprintf(pathbuf, sizeof(pathbuf), "c/%s", argv[i]);
            path = pathbuf;
        } else {
            path = argv[i];
        }

        int result = _fscreate(path);
        if (result < 0) {
            dprintf(CONSOLEOUT, "touch: cannot create %s, err code %d\n", path, result);
        }
    }
    _exit();
}
