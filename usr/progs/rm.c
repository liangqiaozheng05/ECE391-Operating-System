#include "syscall.h"
#include "string.h"
#include "shell.h"

void main(int argc, char *argv[])
{
    if (argc < 2) {
        dprintf(CONSOLEOUT, "rm: missing operand\n");
        _exit();
    }

    for (int i = 1; i < argc; i++) {
        // Add c/ if missing
        char path[64];
        if (strchr(argv[i], '/') == NULL) {
             snprintf(path, sizeof(path), "c/%s", argv[i]);
        } else {
             strncpy(path, argv[i], sizeof(path));
        }

        int result = _fsdelete(path);
        if (result < 0) {
            dprintf(CONSOLEOUT, "rm: cannot remove %s, err code %d\n", path, result);
        }
    }

    _exit();
}