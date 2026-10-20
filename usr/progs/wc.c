#include "syscall.h"
#include "string.h"
#include "shell.h"

#define WC_FD 3
#define WC_BUFSZ 512

static int is_space(char c)
{
    return (c == ' ' || c == '\n' || c == '\t' || c == '\r');
}

void main(int argc, char *argv[])
{
    int fd = STDIN;
    int need_close = 0;
    char buf[WC_BUFSZ];
    unsigned long long nlines = 0;
    unsigned long long nwords = 0;
    unsigned long long nbytes = 0;
    int in_word = 0;

    // Read from file if provided, otherwise from STDIN
    if (argc >= 2) {
        fd = WC_FD;
        char path[64];
        if (strchr(argv[1], '/') == NULL) {
             snprintf(path, sizeof(path), "c/%s", argv[1]);
        } else {
             strncpy(path, argv[1], sizeof(path));
        }

        if (_open(fd, path) < 0) {
            dprintf(CONSOLEOUT, "wc: cannot open %s\n", path);
            _exit();
        }
        need_close = 1;
    }

    for (;;) {
        long r = _read(fd, buf, sizeof(buf));
        if (r < 0) {
            dprintf(CONSOLEOUT, "wc: error reading\n");
            break;
        }
        if (r == 0)
            break; // EOF

        nbytes += (unsigned long long)r;

        for (long i = 0; i < r; i++) {
            char c = buf[i];
            if (c == '\n') {
                nlines++;
            }
            
            if (is_space(c)) {
                if (in_word)
                    in_word = 0;
            } else {
                if (!in_word) {
                    in_word = 1;
                    nwords++;
                }
            }
        }
    }

    if (need_close) {
        _close(fd);
    }

    dprintf(STDOUT, "%llu\t%llu\t%llu\n", nlines, nwords, nbytes);

    _exit();
}
