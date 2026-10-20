#include "syscall.h"
#include "string.h"
#include "shell.h"

#define CAT_FD 3
#define CAT_BUFSZ 1024

void main(int argc, char* argv[]) {
    char buffer[CAT_BUFSZ];
    int n;

    if (argc == 1) {
        // Default STDIN and STDOUT
        while (1) {
            n = _read(STDIN, buffer, sizeof(buffer));
            if (n <= 0) {
                break;
            }
            _write(STDOUT, buffer, n);
        }
        return;
    }

    // File name provided
    for (int i = 1; i < argc; ++i) {
        // prepend c/ if missing
        char path[128];
        if (strchr(argv[i], '/') == NULL) {
             snprintf(path, sizeof(path), "c/%s", argv[i]);
        } else {
             strncpy(path, argv[i], sizeof(path));
        }

        int ret = _open(CAT_FD, path);
        if (ret < 0) {
            dprintf(CONSOLEOUT, "cat: cannot open %s, err code %d\n", path, ret);
            continue;
        }

        // Read from the file and write to STDOUT until EOF
        while (1) {
            n = _read(CAT_FD, buffer, sizeof(buffer));
            if (n <= 0) {
                if (n < 0) {
                    dprintf(CONSOLEOUT, "cat: error reading %s", path);
                }
                // EOF if n == 0
                break;
            }
            int written = _write(STDOUT, buffer, n);
            if (written < 0) {
                dprintf(CONSOLEOUT, "cat: error writing to STDOUT\n");
                break;
            }
        }

        _close(CAT_FD);
    }
}
