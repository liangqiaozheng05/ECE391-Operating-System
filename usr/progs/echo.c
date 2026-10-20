#include "syscall.h"
#include "string.h"
#include "shell.h"

void main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        _write(STDOUT, argv[i], strlen(argv[i]));
        if (i < argc - 1) {
            _write(STDOUT, " ", 1);
        }
    }
    _write(STDOUT, "\n", 1);
}