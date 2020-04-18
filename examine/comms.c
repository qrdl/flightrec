/**************************************************************************
 *
 *  File:       comms.c
 *
 *  Project:    Flight recorder (https://github.com/qrdl/flightrec)
 *
 *  Descr:      Communications with debug client
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
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <errno.h>
#include <stdlib.h>

#include "eel.h"

#include "examine.h"

#define PREFIX "Content-Length: "


/**************************************************************************
 *
 *  Function:   init_comms
 *
 *  Params:     port - port to listen
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Initialise TCP/IP comms and start listening for incoming
 *              connections
 *
 *  Note:
 *
 **************************************************************************/
int init_comms(char *port) {
    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(NULL, port, &hints, &result)) {
        ERR("Error getting address info - %s", strerror(errno));
        return FAILURE;
    }

    listener = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (listener < 0) {
        ERR("Error creating the socket - %s", strerror(errno));
        freeaddrinfo(result);
        return FAILURE;
    }

    unsigned int one = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one))) {
        ERR("Error setting socket option - %s", strerror(errno));
        freeaddrinfo(result);
        return FAILURE;
    }

    if (bind(listener, result->ai_addr, (int)result->ai_addrlen)) {
        ERR("Error binding socket - %s", strerror(errno));
        freeaddrinfo(result);
        return FAILURE;
    }
    freeaddrinfo(result);

    if (listen(listener, 1)) {
        ERR("Error listening socket - %s", strerror(errno));
        return FAILURE;
    }

    struct sockaddr_in addr;
    socklen_t len = sizeof(struct sockaddr);
    if (!getsockname(listener, (struct sockaddr *)&addr, &len)) {
        printf("Listening on port %d\n", ntohs(addr.sin_port));
    }

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   read_message
 *
 *  Params:     fd - descriptior to read from
 *              message - where to store receved message (0-terminated)
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Read message from debugging client
 *
 *  Note:
 *
 **************************************************************************/
int read_message(int fd, char **message)
{
    size_t received, got;
    char buf[sizeof(PREFIX)-1];

    /* read prefix */
    for (received = 0; received < sizeof(PREFIX)-1; received += got) {
        got = read(fd, buf + received, sizeof(buf) - received);
        if (!got) {
            ERR("Error reading from socket: %s", strerror(errno));
            return FAILURE;
        }
    }

    /* make sure we have real prefix */
    if (strncmp(PREFIX, buf, sizeof(PREFIX)-1)) {
        ERR("Got invalid message from client");
        return FAILURE;
    }

    size_t message_len = 0;
    /* read message length, byte by byte */
    for (received = 0;;) {
        got = read(fd, buf, 1);
        if (!got) {
            ERR("Error reading from socket: %s", strerror(errno));
            return FAILURE;
        }
        if (buf[0] > '9' || buf[0] < '0') {
            break;  // number has finished
        }
        message_len = message_len * 10 + (buf[0] - '0');
    }
    got = read(fd, buf+1, 3);
    if (!got) {
        ERR("Error reading from socket: %s", strerror(errno));
        return FAILURE;
    }
    if (buf[0] != '\r' || buf[1] != '\n' || buf[2] != '\r' || buf[3] != '\n') {
        ERR("Got invalid message from client");
        return FAILURE;
    }
    // sanity check
    if (message_len > 65536) {
        ERR("Message is too long");
        return FAILURE;
    }
    *message = malloc(message_len + 1); // extra byte for 0-terminator
    if (!*message) {
        ERR("Out of memory");
        return FAILURE;
    }

    for (received = 0; received < message_len; received += got) {
        got = read(fd, *message + received, message_len - received);
        if (!got) {
            ERR("Error reading from socket: %s", strerror(errno));
            return FAILURE;
        }
    }
    (*message)[message_len] = '\0';

    return SUCCESS;
}


/**************************************************************************
 *
 *  Function:   send_message
 *
 *  Params:     fd - descriptior to send to
 *              message - message to send (0-terminated)
 *
 *  Return:     SUCCESS / FAILURE
 *
 *  Descr:      Read message from debugging client
 *
 *  Note:
 *
 **************************************************************************/
int send_message(int fd, const char *message) {
    if (write(fd, PREFIX, sizeof(PREFIX)-1) <= 0) {
        ERR("Cannot write to socket: %s", strerror(errno));
        return FAILURE;
    }
    char buffer[16];
    int len = sprintf(buffer, "%zu\r\n\r\n", strlen(message));
    if (write(fd, buffer, len) <= 0) {
        ERR("Cannot write to socket: %s", strerror(errno));
        return FAILURE;
    }
    if (write(fd, message, strlen(message)) <= 0) {
        ERR("Cannot write to socket: %s", strerror(errno));
        return FAILURE;
    }
    INFO("Sent '%s'", message);
    return SUCCESS;
}

