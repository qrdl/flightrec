/**************************************************************************
 *
 *  File:       examine.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      'Examine' component entry point
 *
 *  Notes:
 *
 **************************************************************************
 *
 *  Copyright (C) 2017-2020 Ilya Caramishev (ilya@qrdl.com)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as
 *  published by the Free Software Foundation, either version 3 of the
 *  License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 **************************************************************************/
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>     // for accept()
#include <errno.h>
#include <unistd.h>

#include <dab.h>
#include <eel.h>

#include "examine.h"
#include "jsonapi.h"
#include "requests.h"

struct command {
    const char  *name;
    int         code;
};

const struct command *identify(register const char *str, register size_t len);

FILE *logfd;
int listener = 0;

static void print_usage(char *name);
static int process_request(const JSON_OBJ *request, int fd);


/**************************************************************************
 *
 *  Function:   main
 *
 *  Params:     standard
 *
 *  Return:     EXIT_SUCCESS / EXIT_FAILURE
 *
 *  Descr:      Entry point and main loop
 *
 *  Note:
 *
 **************************************************************************/
int main(int argc, char *argv[]) {
    int c;
    char *port = NULL;
    logfd = stderr;

    while ((c = getopt(argc, argv, "p:")) != -1) {
        switch (c) {
            case 'p':
                port = optarg;
                break;
            default:
                if ('p' == optopt) {
                    printf("Option -%c requires an argument\n", optopt);
                } else {
                    printf("Unknown option %c\n", optopt);
                }
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    if (port) {
        if (SUCCESS != init_comms(port)) {
            ERR("Cannot init comms");
            return EXIT_FAILURE;
        }
    }

    int read_fd, write_fd, client;
    for (;;) {
        if (port) {
            client = accept(listener, NULL, NULL);
            if (!client) {
                ERR("Error accepting connection - %s", strerror(errno));
                return FAILURE;
            }
            INFO("Got connection");
            read_fd = write_fd = client;    // use socket for comms
        } else {
            read_fd = 0;    // read from stdin
            write_fd = 1;   // write to stdout
        }
        char *message;

        /* main loop */
        while (SUCCESS == read_message(read_fd, &message)) {
            INFO("Got '%s'", message);
            JSON_OBJ *request = JSON_PARSE(message);
            free(message);
            if (!request) {
                ERR("Cannot parse incoming request");
                break;  // close the connection
            }
            int ret = process_request(request, write_fd);
            JSON_RELEASE(request);
            if (SUCCESS != ret) {
                break;  // close the connection
            }
        }
        release_cursors();      // close and free all possibly allocated cursors
        DAB_CLOSE(DAB_FLAG_NONE);

        /* in case of remote socket conection loop to wait for new connection, otherwise exit */
        if (client) {
            close(client);
        } else {
            break;
        }
    }

    return EXIT_SUCCESS;
}


/**************************************************************************
 *
 *  Function:   process_request
 *
 *  Params:     standard
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Process incoming request and respond
 *
 *  Note:
 *
 **************************************************************************/
int process_request(const JSON_OBJ *request, int fd) {
    const char *cmd_code = JSON_GET_STRING_FIELD(request, "command");
    if (JSON_OK == json_err) {
        const struct command *cmd = identify(cmd_code, strlen(cmd_code));
        if (!cmd) {
            ERR("Unsupported command '%s'", cmd_code);
            return FAILURE;
        }

        DBG("Processing '%s' command", cmd_code);
        switch (cmd->code) {
            case cmd_init:
                process_init(request, fd);
                break;
            case cmd_launch:
                process_launch(request, fd);
                break;
            case cmd_threads:
                process_threads(request, fd);
                break;
            case cmd_stack:
                process_stack(request, fd);
                break;
            case cmd_scopes:
                process_scopes(request, fd);
                break;
            case cmd_next:
                process_next(request, fd);
                break;
            case cmd_stepin:
                process_stepin(request, fd);
                break;
            case cmd_stepout:
                process_stepout(request, fd);
                break;
            case cmd_stepback:
                process_stepback(request, fd);
                break;
            case cmd_breakpoints:
                process_breakpoints(request, fd);
                break;
            case cmd_continue:
                process_continue(request, fd);
                break;
            case cmd_revcontinue:
                process_revcontinue(request, fd);
                break;
            case cmd_variables:
                process_variables(request, fd);
                break;
            case cmd_evaluate:
                process_evaluate(request, fd);
                break;
            case cmd_config_done:
                process_config_done(request, fd);
                break;
            case cmd_exception_br:
                just_ack(request, fd);
                break;
            case cmd_disconnect:
                just_ack(request, fd);
                return FAILURE;         // to cause caller to reset everything
            default:    // should never happen as all supported commands must have cases,
                        // and unsupported ones should fail at identify()
                return FAILURE;
        }
    } else {
        ERR("Missing or malformed 'command' in request");
        return FAILURE;
    }
    return SUCCESS;
}


void print_usage(char *name) {
    printf("Usage: %s [-p <port>]\n", name);
    printf("\t-p        - port to listen for connections from IDE (by defailt stin/stdout used for comms)\n");
};

