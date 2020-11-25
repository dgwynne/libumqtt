/*
 * MIT License
 *
 * Copyright (c) 2019 Jianhui Zhao <zhaojh329@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>

#include "log.h"
#include "utils.h"

int tcp_connect(const char *host, const char *port, int flags, bool *inprogress, int *eai)
{
    int ret;
    int sock = -1;
    int addr_len;
    struct sockaddr *addr = NULL;
    struct addrinfo *result, *rp;
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_ADDRCONFIG
    };

    if (inprogress)
        *inprogress = false;

    ret = getaddrinfo(host, port, &hints, &result);
    if (ret) {
        if (ret == EAI_SYSTEM)
            return -1;
        *eai =  ret;
        return 0;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        if (rp->ai_family == AF_INET) {
            addr = rp->ai_addr;
            addr_len = rp->ai_addrlen;
            break;
        }
    }

    if (!addr)
        goto free_addrinfo;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    int old_flags = fcntl(sock, F_GETFL);
    fcntl(sock, F_SETFL, old_flags | flags);
    if (sock < 0)
        goto free_addrinfo;

    if (connect(sock, addr, addr_len) < 0) {
        if (errno != EINPROGRESS) {
            close(sock);
            sock = -1;
        } else if (inprogress) {
            *inprogress = true;
        }
    }

free_addrinfo:
    freeaddrinfo(result);
    return sock;
}

