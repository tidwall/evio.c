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

void serving(const char **addrs, int naddrs, void *udata) {
    for (int i = 0; i < naddrs; i++) {
        printf("Serving at %s\n", addrs[i]);
    }
}

void error(const char *msg, bool fatal, void *udata) {
    fprintf(stderr, "%s\n", msg);
}

int64_t tick(void *udata) {
    printf("Tick\n");
    // next tick in 1 second. This can be any duration in nanoseconds.
    return 1e9; 
}

void opened(struct evio_conn *conn, void *udata) {
    printf("Connection opened: %s\n", evio_conn_addr(conn));
}

void closed(struct evio_conn *conn, void *udata) {
    printf("Connection closed: %s\n", evio_conn_addr(conn));
}

void data(struct evio_conn *conn, const void *data, size_t len, void *udata) {
    // echo back to the connection
    evio_conn_write(conn, data, len);
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

    // Any number of addresses can be bound to the appliation.
    // Here we are listening on tcp port 9999 (ipv4 and ipv6), and at a local
    // unix socket file named "socket".
    // For ipv4 only use an ip address like tcp://127.0.0.1:9999
    // For ipv6 use something like tcp://[::1]:9999
    const char *addrs[] = { 
        "tcp://localhost:9999",
        "unix://socket",
    };
    // Run the application. This is a forever operation. 
    evio_main(addrs, 2, evs, NULL);
}
```

Then build and run the server

```
$ cc *.c && ./a.out
```

And connect

```
$ nc localhost 9999
```


## Multithreading

Use the `evio_main_mt` function to start the server using multiple threads.

```c
// Use five threads
evio_main_mt(addrs, 2, evs, NULL, 5);

// Use ten threads
evio_main_mt(addrs, 2, evs, NULL, 10);

// Use the number of threads equal to the number of cores on the machine.
evio_main_mt(addrs, 2, evs, NULL, 0);
```
