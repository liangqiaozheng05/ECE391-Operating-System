#include "syscall.h"
#include "string.h"
#include "shell.h"
#include "uio.h"

#define SHELL_BUFSZ 1024
#define MAX_ARGS 8
#define EXEC_FD 3

#define REDIR_NONE 0
#define REDIR_IN 1
#define REDIR_OUT 2

// Global states
static int g_has_pipe = 0;

static char *g_in_left  = NULL;
static char *g_out_left = NULL;

static int   g_argc_right = 0;
static char *g_argv_right[MAX_ARGS + 1];
static char *g_in_right  = NULL;
static char *g_out_right = NULL;

// helper function for parser
char* find_terminator(char* buf) {
	char* p = buf;
	while(*p) {
		switch(*p) {
			case ' ':
			case '\0':
			case FIN:
			case FOUT:
			case PIPE:
				return p;
			default:
				p++;
				break;
		}
	}
	return p;
}

int parse(char* buf, char** argv) {
	// FIXME
	// feel free to change this function however you see fit

	int argc_left = 0;
	g_has_pipe = 0;
	g_argc_right = 0;
	g_in_left = g_out_left = g_in_right = g_out_right = NULL;

	char **cur_argv_base = argv;
	int *cur_argc = &argc_left;
	char **cur_in = &g_in_left;
	char **cur_out = &g_in_right;
	int redir_state = REDIR_NONE;

	char *head = buf;

	for(;;) { // find each argv
		while(*head == ' ') head++;
		if (*head == '\0') break;

		if (*head == PIPE){
			// Switch to right
			g_has_pipe = 1;
			cur_argv_base = g_argv_right;
			cur_argc = &g_argc_right;
			cur_in = &g_in_right;
			cur_out = &g_out_right;
			redir_state = REDIR_NONE;
			head++;
			continue;
		} else if (*head == FIN) {
			redir_state = REDIR_IN;
			head++;
			continue;
		} else if (*head == FOUT) {
			redir_state = REDIR_OUT;
			head++;
			continue;
		}

        char *end = find_terminator(head);
        char temp = *end;
        *end = '\0'; // Truncate
        char *word = head;

        // Redirected filename
        if (redir_state == REDIR_IN) {
            *cur_in = word;
            redir_state = REDIR_NONE;
        } else if (redir_state == REDIR_OUT) {
            *cur_out = word;
            redir_state = REDIR_NONE;
        } else {
            // Normal arg
            if (*cur_argc < MAX_ARGS) {
                cur_argv_base[*cur_argc] = word;
                (*cur_argc)++;
            }
        }

		int done = 0;

		switch(temp) {
			case ' ':
				head = end + 1;
				break;

			case '\0':
				head = end;
				done = 1; // Jump to end
				break;

			case FOUT:
				redir_state = REDIR_OUT;
				head = end + 1;
				break;
					
			case FIN:
				redir_state = REDIR_IN;
				head = end + 1;
                break;

			case PIPE:
				g_has_pipe = 1;
                cur_argv_base = g_argv_right;
                cur_argc = &g_argc_right;
                cur_in = &g_in_right;
                cur_out = &g_out_right;
                redir_state = REDIR_NONE;
                head = end + 1;
                break;

			default:
				head = end + 1;
				break;
		}

		if (done) { break; }
	}

	if (argc_left <= MAX_ARGS)
        argv[argc_left] = NULL;
    if (g_argc_right <= MAX_ARGS)
        g_argv_right[g_argc_right] = NULL;

    return argc_left;
}


// Helpers

static void apply_redirections(const char *infile, const char *outfile, int allow_in, int allow_out) {
    if (allow_in && infile != NULL) {
        _close(STDIN);
        if (_open(STDIN, infile) < 0) {
            dprintf(CONSOLEOUT, "Error: cannot open %s for input\n", infile);
            _exit();
        }
    }

    if (allow_out && outfile != NULL) {
        _close(STDOUT);

        int r = _open(STDOUT, outfile);
        if (r < 0) {
            // Create if not exist
            if (_fscreate((char *)outfile) < 0) {
                dprintf(CONSOLEOUT, "Error: cannot create %s\n", outfile);
                _exit();
            }
            r = _open(STDOUT, outfile);
            if (r < 0) {
                dprintf(CONSOLEOUT, "Error: cannot open %s for output\n", outfile);
                _exit();
            }
        }

        unsigned long long zero = 0;
        _fcntl(STDOUT, FCNTL_SETPOS, &zero);
        _fcntl(STDOUT, FCNTL_SETEND, &zero);
    }
}

static void exec_with_path(int argc, char **argv) {
    if (argc <= 0)
        _exit();

    char pathbuf[SHELL_BUFSZ];
    const char *user_name = argv[0];
    const char *path;

    if (strchr(user_name, '/') == NULL) {
        // "trek" -> "c/trek"
        snprintf(pathbuf, sizeof(pathbuf), "c/%s", user_name);
        path = pathbuf;
    } else {
        path = user_name;
    }

    _close(EXEC_FD);
    if (_open(EXEC_FD, path) < 0) {
        dprintf(CONSOLEOUT, "Error: Cannot open program %s\n", path);
        _exit();
    }

    char *exec_argv[MAX_ARGS + 1];
    exec_argv[0] = (char *)path;
    for (int i = 1; i < argc && i < MAX_ARGS; i++)
        exec_argv[i] = argv[i];
    exec_argv[argc] = NULL;

    int result = _exec(EXEC_FD, argc, exec_argv);
    dprintf(CONSOLEOUT, "Error: exec %s failed, err code %d\n", path, result);
    _exit();
}

static void run_simple(int argc_left, char **argv_left) {
    if (argc_left == 0)
        return;

    int pid = _fork();
    if (pid < 0) {
        dprintf(CONSOLEOUT, "Error: fork failed\n");
        return;
    }

    if (pid == 0) {
        apply_redirections(g_in_left, g_out_left, 1, 1);
        exec_with_path(argc_left, argv_left);
    } else {
        // Parent thread waits for child
        _wait(pid);
    }
}

static void run_pipeline(int argc_left, char **argv_left) {
    if (argc_left == 0)
        return;

    if (g_argc_right == 0) {
        // Run as simple command
        run_simple(argc_left, argv_left);
        return;
    }

    int wfd = -1, rfd = -1;
    if (_pipe(&wfd, &rfd) < 0) {
        dprintf(CONSOLEOUT, "Error: pipe failed\n");
        return;
    }

    int pid1 = _fork();
    if (pid1 < 0) {
        dprintf(CONSOLEOUT, "Error: fork failed\n");
        _close(wfd);
        _close(rfd);
        return;
    }

    if (pid1 == 0) {
        // child 1
        apply_redirections(g_in_left, NULL, 1, 0);

        _close(STDOUT);
        _uiodup(wfd, STDOUT);
        _close(wfd);
        _close(rfd);

        exec_with_path(argc_left, argv_left);
    }

    int pid2 = _fork();
    if (pid2 < 0) {
        dprintf(CONSOLEOUT, "Error: fork failed\n");
        _close(wfd);
        _close(rfd);
        _wait(pid1);
        return;
    }

    if (pid2 == 0) {
        // child 2
        _close(STDIN);
        _uiodup(rfd, STDIN);
        _close(wfd);
        _close(rfd);

        apply_redirections(NULL, g_out_right, 0, 1);

        exec_with_path(g_argc_right, g_argv_right);
    }

    _close(wfd);
    _close(rfd);
    _wait(pid1);
    _wait(pid2);
}

int main()
{
    char buf[SHELL_BUFSZ];
	int argc;
	char* argv[MAX_ARGS + 1]; 

  	_open(CONSOLEOUT, "dev/uart0");		// console device
	_close(STDIN);              		// close any existing stdin
	_uiodup(CONSOLEOUT, STDIN);      	// stdin from console
	_close(STDOUT);              		// close any existing stdout
	_uiodup(CONSOLEOUT, STDOUT);     	// stdout to console

	printf("Starting 391 Shell\n");

	for (;;)
	{
		printf("LUMON OS> ");
		if (getsn(buf, SHELL_BUFSZ - 1) == NULL) {
            break;
        }

		// Skip leading spaces
        char *p = buf;
        while (*p == ' ') { p++; }

        if (*p == '\0')
            continue;

        if (0 == strcmp(p, "exit")) {
			_exit();
		}

        argc = parse(buf, argv);

        if (!g_has_pipe) {
			run_simple(argc, argv);
		}
        else {
			run_pipeline(argc, argv);
		}
	}
    return 0;
}