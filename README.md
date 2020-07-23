# evio.c

A framework for building event based networking applications. 

This is a C version of the [original evio](https://github.com/tidwall/evio).

## Install

Clone this respository and then use [pkg.sh](https://github.com/tidwall/pkg.sh)
to import dependencies.

```
$ git clone https://github.com/tidwall/evio.c
$ cd evio.c/
$ pkg.sh import
```

## Example

Here's a simple echo server. Save the following file to `echo.c` 

```c
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "evio.h"

void serving(int64_t nano, const char **addrs, int naddrs, void *udata) {
    for (int i = 0; i < naddrs; i++) {
        printf("Serving at %s\n", addrs[i]);
    }
}

void error(int64_t nano, const char *msg, bool fatal, void *udata) {
    fprintf(stderr, "%s\n", msg);
}

int64_t tick(int64_t nano, void *udata) {
    // Ticker fires every 5 seconds
    printf("Tick\n");
    return 5e9; // nanoseconds
}

void opened(int64_t nano, struct evio_conn *conn, void *udata) {
    printf("Connection opened: %s\n", evio_conn_addr(conn));
}

void closed(int64_t nano, struct evio_conn *conn, void *udata) {
    printf("Connection closed: %s\n", evio_conn_addr(conn));
}

void data(int64_t nano, struct evio_conn *conn, const void *data, size_t len, void *udata) {
    // Echo data
    evio_conn_write(conn, "+OK\r\n", -1);
    // evio_conn_write(conn, data, len);
}

int main() {
    struct evio_events evs = {
        .serving = serving,
        .error = error,
        .tick = tick,
        .opened = opened,
        .closed = closed,
        .data = data,
    };
    const char *addrs[] = { 
        "tcp://127.0.0.1:6379",
        "unix://socket",
    };
    evio_main(addrs, sizeof(addrs)/sizeof(char*), evs, NULL);
}
```

Then build and run the server

```
$ cc *.c && ./a.out
```
