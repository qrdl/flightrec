#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#include <jsonapi.h>

#include "tester.h"

#define PREFIX "Content-Length: "

struct var {
    char *name;
    char *value;
    struct var  *next;
};

/* there could be just one process running, so globals are ok */
pid_t child;
int child_in, child_out;
JSON_OBJ *json = NULL;
struct var *vars = NULL;    // list of known variables

static char **chop(char *source);
static struct var *find_var(const char *name);

/**************************************************************************
 *
 *  Function:   start
 *
 *  Params:     cmd_line - command line of program to start
 *              error - where to store error message in case of error
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Start program and attach pipes to its stdin and stdout
 *
 **************************************************************************/
int start(char *cmd_line, char **error) {
    if (child) {
        *error = "another child process is running";
        return FAILURE;
    }

    /* selfpipe is used to notify parent of failure to execute in child */
    int pipe1[2], pipe2[2], pipe3[2], selfpipe[2];

    if (pipe(pipe1) == -1) {
        *error = strerror(errno);
        return FAILURE;
    }
    if (pipe(pipe2) == -1) {
        *error = strerror(errno);
        return FAILURE;
    }
    if (pipe(pipe3) == -1) {
        *error = strerror(errno);
        return FAILURE;
    }
    if (pipe(selfpipe) == -1) {
        *error = strerror(errno);
        return FAILURE;
    }
    fcntl(selfpipe[1], F_SETFD, 1);    // set close-on-exec

    char **argv = chop(cmd_line);
    pid_t p = fork();
    if (p < 0) {
        *error = strerror(errno);
        return FAILURE;
    } else if (p == 0) {
        /*child */
        close(selfpipe[0]);
        close(pipe1[1]);
        fcntl(pipe1[0], F_SETFD, 0);    // clear close-on-exec
        if (pipe1[0] != STDIN_FILENO) {
            dup2(pipe1[0], STDIN_FILENO);
            close(pipe1[0]);
        }
        close(pipe2[0]);
        fcntl(pipe2[1], F_SETFD, 0);    // clear close-on-exec for fd
        if (pipe2[1] != STDOUT_FILENO) {
            dup2(pipe2[1], STDOUT_FILENO);
            close(pipe2[1]);
        }
        close(pipe3[0]);
        fcntl(pipe3[1], F_SETFD, 0);    // clear close-on-exec for fd
        if (pipe3[1] != STDERR_FILENO) {
            dup2(pipe3[1], STDERR_FILENO);
            close(pipe3[1]);
        }
        execvp(argv[0], argv);
        // notify parent that things went bad
        write(selfpipe[1], &errno, sizeof(errno));
        _exit(0);
    }
    /* parent */
    close(pipe1[0]);
    close(pipe2[1]);
    close(pipe3[1]);
    close(selfpipe[1]);
    int err;
    int ret = read(selfpipe[0], &err, sizeof(err));
    free(argv);
    if (ret < 0) {
        *error = strerror(errno);
        return FAILURE;
    } else if (ret > 0) {
        /* got errno from child - exec failed */
        *error = strerror(err);
        return FAILURE;
    }

    child = p;
    child_out = pipe2[0];
    child_in = pipe1[1];

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   stop
 *
 *  Params:     error - where to store error message in case of error
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Stop program which has been started by start()
 *
 **************************************************************************/
int stop(char **error) {
    if (!child) {
        *error = "process not started";
        return FAILURE;
    }
    close(child_out);
    close(child_in);
    kill(child, SIGTERM);
    child = 0;
    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   request
 *
 *  Params:     message - message to send (zero-terminarted string)
 *              error - where to store error message in case of error
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Send request to child process
 *
 **************************************************************************/
int request(const char *message, char **error) {
    if (write(child_in, PREFIX, sizeof(PREFIX)-1) <= 0) {
        *error = strerror(errno);
        return FAILURE;
    }
    char buffer[16];
    int len = sprintf(buffer, "%zu\r\n\r\n", strlen(message));
    if (write(child_in, buffer, len) <= 0) {
        *error = strerror(errno);
        return FAILURE;
    }
    if (write(child_in, message, strlen(message)) <= 0) {
        *error = strerror(errno);
        return FAILURE;
    }
    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   response
 *
 *  Params:     error - where to store error message in case of error
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Read message from child
 *
 **************************************************************************/
int response(char **error) {
    size_t received, got;
    char buf[sizeof(PREFIX)-1];

    if (json) {
        JSON_RELEASE(json);
        json = NULL;
    }

    /* read prefix */
    for (received = 0; received < sizeof(PREFIX)-1; received += got) {
        got = read(child_out, buf + received, sizeof(buf) - received);
        if (!got) {
            *error = strerror(errno);
            return FAILURE;
        }
    }

    /* make sure we have real prefix */
    if (strncmp(PREFIX, buf, sizeof(PREFIX)-1)) {
        *error = "Got invalid message from client";
        return FAILURE;
    }

    size_t message_len = 0;
    /* read message length, byte by byte */
    for (received = 0;;) {
        got = read(child_out, buf, 1);
        if (!got) {
            *error = strerror(errno);
            return FAILURE;
        }
        if (buf[0] > '9' || buf[0] < '0') {
            break;  // number has finished
        }
        message_len = message_len * 10 + (buf[0] - '0');
    }
    got = read(child_out, buf+1, 3);
    if (!got) {
        *error = strerror(errno);
        return FAILURE;
    }
    if (buf[0] != '\r' || buf[1] != '\n' || buf[2] != '\r' || buf[3] != '\n') {
        *error = "Got invalid message from client";
        return FAILURE;
    }
    // sanity check
    if (message_len > 65536) {
        *error = "Message is too long";
        return FAILURE;
    }
    char *message = malloc(message_len + 1); // extra byte for 0-terminator
    if (!message) {
        *error = "Out of memory";
        return FAILURE;
    }

    for (received = 0; received < message_len; received += got) {
        got = read(child_out, message + received, message_len - received);
        if (!got) {
            free(message);
            *error = strerror(errno);
            return FAILURE;
        }
    }
    message[message_len] = '\0';

    /* expect valid JSON in message */
    json = JSON_PARSE(message);
    free(message);
    if (!json) {
        *error = "Not a valid JSON in response";
        return FAILURE;
    }

    return SUCCESS;
}

/**************************************************************************
 *
 *  Function:   set_var
 *
 *  Params:     name - variable name
 *              value - variable value
 *
 *  Return:     N/A
 *
 *  Descr:      Set value for new/existing variable 
 *
 **************************************************************************/
void set_var(const char *name, const char *value) {
    struct var *new_var = find_var(name);
    if (new_var) {
        free(new_var->value);
    } else {
        new_var = malloc(sizeof(*new_var));
        new_var->next = vars;
        vars = new_var;
        new_var->name = strdup(name);
    }
    new_var->value = strdup(value);
}


/**************************************************************************
 *
 *  Function:   get_var
 *
 *  Params:     name - variable name
 *
 *  Return:     var value / NULL if not found
 *
 *  Descr:      Get variable value
 *
 **************************************************************************/
const char *get_var(const char *name) {
    struct var *var = find_var(name);
    if (var) {
        return var->value;
    }
    return NULL;
}


/**************************************************************************
 *
 *  Function:   find_var
 *
 *  Params:     name - variable name
 *
 *  Return:     var descriptor / NULL if not found
 *
 *  Descr:      Get variable descriptor from linked list
 *
 **************************************************************************/
struct var *find_var(const char *name) {
    struct var *cur;
    for (cur = vars; cur; cur = cur->next) {
        if (!strcmp(name, cur->name)) {
            return cur;
        }
    }
    return NULL;
}


/**************************************************************************
 *
 *  Function:   chop
 *
 *  Params:     source - line to chop
 *
 *  Return:     vector of line bits / NULL on error
 *
 *  Descr:      Chop source line into bits and store bits in vector
 *
 **************************************************************************/
char **chop(char *source) {
	size_t item_count = 32;     // ought to be enough
	char **vector = malloc(sizeof(char *) * (item_count + 1));	// one extra for NULL terminator

	vector[0] = source;
	/* split input line when unquoted/unescaped space encountered */
	char *cur = NULL;
	int escape = 0;
	int quote = 0;
    size_t cur_item = 0;
	for (cur = source; *cur; cur++) {
		switch (*cur) {
		case '\\':
			escape = !escape;
			break;
		case '"':
			if (!escape) {
				quote = !quote;
			}
			escape = 0;
			break;
		case ' ':
			if (!quote && !escape) {
				*cur = '\0';	// terminate token
                do {
                    cur++;
                } while (isspace(*cur));
                if (*cur) {
                    vector[++cur_item] = cur;
                }
			} else {
				escape = 0;
			}
			break;
		default:
			escape = 0;
			break;
		}
	}
	vector[cur_item+1] = NULL;

    /* unquote single- and double-quoted items */
    size_t len;
    cur_item = 0;
    for (char *item = vector[0]; item; item = vector[++cur_item]) {
        len = strlen(item);
        if (    ('"'  == item[0] && '"'  == item[len-1]) ||
                ('\'' == item[0] && '\'' == item[len-1])) {
            vector[cur_item]++;
            item[len-1] = '\0';
        }
    }

	return vector;
}

/* TODO pattern matching */
int match(const char *string, const char *pattern) {
    return 1;
}