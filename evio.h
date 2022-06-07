// Copyright 2022 Joshua J Baker. All rights reserved.
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file.
// Documentation at https://github.com/tidwall/evio.c

#ifndef EVIO_H
#define EVIO_H

#include <sys/socket.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

struct evio_conn;

void evio_conn_close(struct evio_conn *conn);
void *evio_conn_udata(struct evio_conn *conn);
void evio_conn_set_udata(struct evio_conn *conn, void *udata);
void evio_conn_write(struct evio_conn *conn, const void *data, ssize_t len);
const char *evio_conn_addr(struct evio_conn *conn);

struct evio_events {
    int64_t (*tick)(void *udata);
    bool (*sync)(void *udata);
    void (*data)(struct evio_conn *conn, 
                 const void *data, size_t len, void *udata);
    void (*opened)(struct evio_conn *conn, void *udata);
    void (*closed)(struct evio_conn *conn, void *udata);
    void (*serving)(const char **addrs, int naddrs, void *udata);
    void (*error)(const char *message, bool fatal, void *udata);
};

int64_t evio_now();

void evio_main(const char *addrs[], int naddrs, struct evio_events events, 
               void *udata);
void evio_main_mt(const char *addrs[], int naddrs, struct evio_events events, 
                  void *udata, int nthreads);
void evio_set_allocator(void *(malloc)(size_t), void (*free)(void*));

#endif

