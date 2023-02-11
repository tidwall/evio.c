// Copyright 2022 Joshua J Baker. All rights reserved.
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file.
// Documentation at https://github.com/tidwall/evio.c

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>
#include <arpa/inet.h>
#include <pthread.h> 
#include "evio.h"
#include "buf.h"
#include "hashmap.h"

static void *(*_malloc)(size_t) = NULL;
static void (*_free)(void *) = NULL;

#define emalloc (_malloc?_malloc:malloc)
#define efree (_free?_free:free)

// evio_set_allocator allows for configuring a custom allocator for
// all evio library operations. This function, if needed, should be called
// only once at startup and a prior to calling any library functions
void evio_set_allocator(void *(malloc)(size_t), void (*free)(void*)) {
    _malloc = malloc;
    _free = free;
    buf_set_allocator(malloc, free);
    hashmap_set_allocator(malloc, free);
}

#define panic(format, ...) { \
    fprintf(stderr, "panic: " format, ##__VA_ARGS__); \
    fprintf(stderr, " (%s:%d)\n", __FILE__, __LINE__); \
    exit(1); \
}

#define eprintf(fatal, format, ...) {                                         \
    snprintf(evio->errmsg, sizeof(evio->errmsg), format, ##__VA_ARGS__);      \
    if (evio->events.error) {                                                 \
        evio->events.error(evio->errmsg, fatal, evio->udata);                 \
    } else if (fatal) {                                                       \
        panic(format, ##__VA_ARGS__);                                         \
    }                                                                         \
    if (fatal) exit(1);                                                       \
}

struct addr {
    bool unsock;
    char *host;
    int port;
    int nfds;
    int *fds;
    char **addrs;
};

struct evio_conn {
    int qfd;
    int fd;
    bool closed;
    bool woke;
    bool faulty;
    struct buf wbuf;
    size_t wbuf_idx;
    void *udata;
    struct evio *evio;
    char *addr;
    struct evio_conn *next_faulty;
};

struct evio {
    struct evio_events events;
    char errmsg[256];
    struct hashmap *conns;
    struct evio_conn *faulty; 
    void *udata;
};

static bool wake(struct evio_conn *conn);

static void set_fault(struct evio_conn *conn) {
    conn->faulty = true;
    conn->next_faulty = conn->evio->faulty;
    conn->evio->faulty = conn;
}

void evio_conn_write(struct evio_conn *conn, const void *data, ssize_t len) {
    if (conn->faulty || conn->closed) {
        return;
    }
    if (!buf_append(&conn->wbuf, data, len) || !wake(conn)) {
        set_fault(conn);
    }
}

void evio_conn_close(struct evio_conn *conn) {
    if (conn->faulty || conn->closed) {
        return;
    }
    conn->closed = true;
    if (!wake(conn)) {
        set_fault(conn);
    }
}

void *evio_conn_udata(struct evio_conn *conn) {
    return conn->udata;
}

void evio_conn_set_udata(struct evio_conn *conn, void *udata) {
    conn->udata = udata;
}

#define EDELAYNS 1000000000

#if defined(__FreeBSD__) || defined(__APPLE__)

#include <sys/event.h>

static int net_queue() {
    return kqueue();
}

static int net_addrd(int qfd, int sfd) {
    struct kevent ev = {.filter = EVFILT_READ,.flags = EV_ADD,.ident = sfd};
    return kevent(qfd, &ev, 1, NULL, 0, NULL);
}

static int net_addwr(int qfd, int sfd) {
    struct kevent ev = {.filter = EVFILT_WRITE,.flags = EV_ADD,.ident = sfd};
    return kevent(qfd, &ev, 1, NULL, 0, NULL);
}

static int net_delwr(int qfd, int sfd) {
    struct kevent ev = {.filter = EVFILT_WRITE,.flags = EV_DELETE,.ident = sfd};
    return kevent(qfd, &ev, 1, NULL, 0, NULL);
}

static int net_events(int qfd, int *fds, int nfds, int64_t timeout_ns) {
    struct kevent evs[nfds]; // VLA
    int n;
    if (timeout_ns < 0) {
        n = kevent(qfd, NULL, 0, evs, nfds, NULL);
    } else {
        if (timeout_ns > EDELAYNS) {
            timeout_ns = EDELAYNS;
        }
        struct timespec timeout = { .tv_nsec = timeout_ns };
        n = kevent(qfd, NULL, 0, evs, nfds, &timeout);
    }
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            fds[i] = evs[i].ident;
        }
    }
    return n;
}

#elif defined(__linux__)

#include <sys/epoll.h>

static int net_queue() {
    return epoll_create1(0);
}

static int net_addrd(int qfd, int sfd) {
    struct epoll_event ev = { 0 };
    ev.events = EPOLLIN;
    ev.data.fd = sfd;
    return epoll_ctl(qfd, EPOLL_CTL_ADD, sfd, &ev);
}

static int net_addwr(int qfd, int sfd) {
    struct epoll_event ev = { 0 };
    ev.events = EPOLLIN|EPOLLOUT;
    ev.data.fd = sfd;
    return epoll_ctl(qfd, EPOLL_CTL_MOD, sfd, &ev);
}

static int net_delwr(int qfd, int sfd) {
    struct epoll_event ev = { 0 };
    ev.events = EPOLLIN;
    ev.data.fd = sfd;
    return epoll_ctl(qfd, EPOLL_CTL_MOD, sfd, &ev);
}

static int net_events(int qfd, int *fds, int nfds, int64_t timeout_ns) {
    struct epoll_event evs[nfds]; // VLA
    int n;
    if (timeout_ns < 0) {
        n = epoll_wait(qfd, evs, nfds, -1);
    } else {
        if (timeout_ns > EDELAYNS) {
            timeout_ns = EDELAYNS;
        }
        n = epoll_wait(qfd, evs, nfds, (int)(timeout_ns/1000000));
    }
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            fds[i] = evs[i].data.fd;
        }
    }
    return n;
}
#else 

#error unsupported platform

#endif

int setkeepalive(int fd) {
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &(int){1}, sizeof(int))) {
        return -1;
    }
#if defined(__linux__)
    // tcp_keepalive_time
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &(int){600}, sizeof(int))) {
        return -1;
    }
    // tcp_keepalive_intvl
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &(int){60}, sizeof(int))) {
        return -1;
    }
    // tcp_keepalive_probes
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &(int){6}, sizeof(int))) {
        return -1;
    }
#endif
    return 0;
}

int settcpnodelay(int fd) {
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
}


static int setnonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void ipstr(const struct sockaddr *sa, char *s, size_t len) {
    switch(sa->sa_family) {
    case AF_INET:
        strcpy(s, "tcp://");
        inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr), 
                  s+6, len-6);
        break;
    case AF_INET6:
        strcpy(s, "tcp://[");
        inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr), 
                  s+7, len-7);
        strcat(s, "]");
        break;
    default:
        strncpy(s, "Unknown AF", len);
        return;
    }
}

const char *evio_conn_addr(struct evio_conn *conn) {
    return conn->addr;
}

static void net_accept(struct evio *evio, int qfd, int sfd, 
                       struct addr *a)
{
    struct evio_conn *conn = NULL;
    int cfd = -1;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    cfd = accept(sfd, (struct sockaddr *)&addr, &addrlen);
    if (cfd < 0) goto fail;
    if (setnonblock(cfd) == -1) goto fail;
    if (!a->unsock && setkeepalive(cfd) == -1) goto fail;
    // if (!a->unsock && settcpnodelay(cfd) == -1) goto fail;
    if (net_addrd(qfd, cfd) == -1) goto fail;
    conn = emalloc(sizeof(struct evio_conn));
    if (!conn) goto fail;
    memset(conn, 0, sizeof(struct evio_conn));
    char saddr[256];
    if (a->unsock) {
        snprintf(saddr, sizeof(saddr), "unix://%s", a->host);
    } else {
        // copy the prefix 
        ipstr((struct sockaddr *)&addr, saddr, sizeof(saddr)-1);
        // copy the port
        snprintf(saddr+strlen(saddr), sizeof(saddr), ":%d", 
                ((struct sockaddr_in *)&addr)->sin_port);
    }
    size_t saddrlen = strlen(saddr);
    conn->addr = emalloc(saddrlen+1);
    if (!conn->addr) goto fail;
    memcpy(conn->addr, saddr, saddrlen+1);
    conn->fd = cfd;
    conn->qfd = qfd;
    conn->evio = evio;
    if (hashmap_set(evio->conns, &conn)) {
        panic("duplicate fd");
    } else if (hashmap_oom(evio->conns)) {
        goto fail;
    }
    if (evio->events.opened) {
        evio->events.opened(conn, evio->udata);
    }
    return;
fail:
    if (cfd != -1) close(cfd);
    if (conn) {
        if (conn->addr) efree(conn->addr);
        efree(conn);
    }
}

static struct addr *addr_listen(struct evio *evio, const char *str) {
    bool unsock = false;
    bool tcp = false;
    const char *host = str;
    if (strstr(str, "tcp://") == str) {
        host = str + 6;
        tcp = true;
    } else if (strstr(str, "unix://") == str) {
        host = str + 7;
        unsock = true;
    } else if (strstr(str, "://")) {
        eprintf(true, "Invalid address: %s", str);
    }
    char *colon = NULL;
    for (int i = strlen(host)-1; i >= 0; i--) {
        if (host[i] == ':') {
            colon = (char*)host+i;
            break;
        }
    }
    if ((!unsock && !tcp) && colon) {
        tcp = true;
    } else if ((!unsock && !tcp) && !colon) {
        unsock = true;
    }
    if ((unsock && colon) || (!unsock && !colon)) {
        eprintf(true, "Invalid address: %s", str);
    }
    int port = 0;
    int hlen = strlen(host);
    if (colon) {
        char *end = NULL;
        long x = strtol(colon+1, &end, 10);
        if (!end || *end || x > 0xFFFF || x < 0) {
            eprintf(true, "Invalid address: %s", str);
        }
        port = x;
        hlen = colon-host;
        if (host[0] == '[' && host[hlen-1] == ']') {
            host++;
            hlen-=2;
        }
    }
    // Address string looks valid so let's try to bind.
    struct addr *addr = emalloc(sizeof(struct addr));
    if (!addr) {
        eprintf(true, "%s", strerror(ENOMEM));
    }
    memset(addr, 0, sizeof(struct addr));
    addr->nfds = 0;
    addr->fds = NULL;
    addr->unsock = unsock;
    addr->port = port;
    addr->host = emalloc(hlen+1);
    if (!addr->host) {
        eprintf(true, "%s", strerror(ENOMEM));
    }
    memcpy(addr->host, host, hlen);
    addr->host[hlen] = 0;
    if (unsock) {
        struct sockaddr_un unaddr;
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd == -1) {
            eprintf(true, "socket: %s: %s", strerror(errno), str); 
        }
        memset(&unaddr, 0, sizeof(struct sockaddr_un));
        unaddr.sun_family = AF_UNIX;
        strncpy(unaddr.sun_path, addr->host, sizeof(unaddr.sun_path) - 1);
        if (setnonblock(fd) == -1) {
            eprintf(true, "setnonblock: %s: %s", strerror(errno), str); 
        }
        unlink(addr->host);
        if (bind(fd, (struct sockaddr *) &unaddr, 
                 sizeof(struct sockaddr_un)) == -1)
        {
            eprintf(true, "bind: %s: %s", strerror(errno), str); 
        }
        if (listen(fd, SOMAXCONN) == -1) {
            eprintf(true, "listen: %s: %s", strerror(errno), str); 
        }
        addr->fds = emalloc(1*sizeof(int));
        if (!addr->fds) {
            eprintf(true, "%s", strerror(ENOMEM));
        }
        addr->fds[0] = fd;
        addr->nfds = 1;
        addr->addrs = emalloc(1*sizeof(char*));
        if (!addr->addrs) {
            eprintf(true, "%s", strerror(ENOMEM));
        }
        size_t nbytes = 7+strlen(addr->host)+1;
        addr->addrs[0] = emalloc(nbytes);
        if (!addr->addrs[0]) {
            eprintf(true, "%s", strerror(ENOMEM));
        }
        snprintf(addr->addrs[0], nbytes, "unix://%s", addr->host);
    } else {
        struct addrinfo hints = {}, *addrs;
        char port_str[16] = {};

        hints.ai_family = AF_UNSPEC; 
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        snprintf(port_str, sizeof(port_str), "%d", port);
        int err = getaddrinfo(addr->host, port_str, &hints, &addrs);
        if (err != 0) {
            eprintf(true, "getaddrinfo: %s: %s", gai_strerror(err), str); 
        }
        int n = 0;
        struct addrinfo *addrinfo = addrs;
        while (addrinfo) {
            n++;
            addrinfo = addrinfo->ai_next;
        }
        addr->fds = emalloc(n*sizeof(int));
        if (!addr->fds) {
            eprintf(true, "%s", strerror(ENOMEM));
        }
        addr->addrs = emalloc(n*sizeof(char*));
        if (!addr->addrs) {
            eprintf(true, "%s", strerror(ENOMEM));
        }
        addrinfo = addrs;
        char errmsg[256] = "";
        #define emsg_continue(msg) { \
            if (fd != -1) close(fd); \
            snprintf(errmsg, sizeof(errmsg), \
                     "%s: %s: %s", msg, strerror(errno), str); \
            continue; \
        } 
        char saddr[256];
        for ( ; addrinfo ; addrinfo = addrinfo->ai_next) {
            int fd = socket(addrinfo->ai_family, addrinfo->ai_socktype, 
                            addrinfo->ai_protocol);
            if (fd == -1) {
                emsg_continue("socket");
            }
            if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, 
                           sizeof(int)) == -1)
            {
                emsg_continue("setsockopt(SO_REUSEADDR)");
            }
            if (setnonblock(fd) == -1) {
                emsg_continue("setnonblock");
            }
            if (bind(fd, addrinfo->ai_addr, addrinfo->ai_addrlen) == -1) {
                emsg_continue("bind");
            }
            if (listen(fd, SOMAXCONN) == -1) {
                emsg_continue("listen");
            }
            err = 0;
            addr->fds[addr->nfds] = fd;
            ipstr(addrinfo->ai_addr, saddr, sizeof(saddr)-1);
            snprintf(saddr+strlen(saddr), sizeof(saddr), ":%d", port);

            addr->addrs[addr->nfds] = emalloc(strlen(saddr)+1);
            if (!addr->addrs[addr->nfds]) {
                eprintf(true, "%s", strerror(ENOMEM));
            }
            strcpy(addr->addrs[addr->nfds], saddr);
            addr->nfds++;
        }
        if (err) {
            eprintf(true, "%s", errmsg);
        }
        if (addr->nfds == 0) {
            if (strlen(errmsg)) {
                eprintf(true, "%s", errmsg);
            } else {
                eprintf(true, "Address fail: %s", str);
            }
        }
        freeaddrinfo(addrs);
    }
    return addr;
}

static void close_remove_conn(struct evio_conn *conn, struct evio *evio) {
    if (evio->events.closed) {
        evio->events.closed(conn, evio->udata);
    }
    buf_clear(&conn->wbuf);
    close(conn->fd);
    hashmap_delete(evio->conns, &conn);
    efree(conn->addr);
    efree(conn);
}

static int conn_compare(const void *a, const void *b, void *udata) {
    struct evio_conn *ca = *(struct evio_conn **)a;
    struct evio_conn *cb = *(struct evio_conn **)b;
    return ca->fd < cb->fd ? -1 : ca->fd > cb->fd ? 1 : 0;
}

static uint64_t conn_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return (*(struct evio_conn **)item)->fd;
}

static bool wake(struct evio_conn *conn) {
    if (!conn->woke) {
        if (net_addwr(conn->qfd, conn->fd) == -1) {
            return false;
        }
        conn->woke = true;
    }
    return true;
}

static bool unwake(struct evio_conn *conn) {
    if (conn->woke) {
        if (net_delwr(conn->qfd, conn->fd) == -1) {
            return false;
        }
        conn->woke = false;
    }
    return true;
}

static bool conn_flush(struct evio *evio, struct evio_conn *conn) {
    if (conn->wbuf.len > 0) {
        for (size_t i = conn->wbuf_idx; i < conn->wbuf.len; ) {
            int n = write(conn->fd, conn->wbuf.data+i, conn->wbuf.len-i);
            if (n == -1) {
                if (errno == EAGAIN) {
                    if (!wake(conn)) {
                        close_remove_conn(conn, evio);    
                    } else {
                        conn->wbuf_idx = i;
                    }
                } else {
                    close_remove_conn(conn, evio);
                }
                return false;
            }
            i += n;
        }
        conn->wbuf.len = 0;
        conn->wbuf_idx = 0;
    }
    if (conn->closed) {
        close_remove_conn(conn, evio);
        return false;
    }
    if (!unwake(conn)) {
        close_remove_conn(conn, evio);
        return false;
    }
    return true;
}

int64_t evio_now() {
    struct timespec tm;
    if (clock_gettime(CLOCK_MONOTONIC, &tm) == -1) {
        panic("clock_gettime: %s", strerror(errno));
    }
    return tm.tv_sec * 1000000000 + tm.tv_nsec;
}

static int which_socketfd(int fd, struct addr **addrs, int naddrs) {
    for (int j = 0; j < naddrs; j++) {
        for (int k = 0; k < addrs[j]->nfds; k++) {
            if (fd == addrs[j]->fds[k]) {
                return j;
            }
        }
    }
    return -1;
}

struct evio_conn *get_conn(struct evio *evio, int fd) {
    struct evio_conn key = { .fd = fd };
    struct evio_conn *keyptr = &key;
    void *v = hashmap_get(evio->conns, &keyptr);
    if (!v) {
        return NULL;
    }
    return *(struct evio_conn**)v;
}

struct thread_context {
    pthread_mutex_t *mu;
    bool serving;
    int server_id;
    //
    struct evio_events events;
    void *udata;
    struct addr **paddrs;
    int naddrs;
    
};

static void *thread(void *thdata) {
    struct thread_context *thctx = thdata;
    struct evio _evio;
    struct evio *evio = &_evio;
    memset(evio, 0, sizeof(struct evio));
    evio->events = thctx->events;
    evio->udata = thctx->udata;
    evio->conns = hashmap_new(sizeof(struct conn *), 0, 0, 0, conn_hash, 
                              conn_compare, NULL, NULL);
    if (!evio->conns) {
        eprintf(true, "%s", strerror(ENOMEM));
    }
    int qfd = net_queue();
    if (qfd == -1) {
        eprintf(true, "net_queue: %s", strerror(errno));
    }
    // add all socket fds to queue
    int naddrsfds = 0;
    for (int i = 0; i < thctx->naddrs; i++) {
        for (int j = 0; j < thctx->paddrs[i]->nfds; j++) {
            int sfd = thctx->paddrs[i]->fds[j];
            if (net_addrd(qfd, sfd) == -1) {
                eprintf(true, "net_addrd(socket): %s", strerror(errno));
            }
            naddrsfds++;
        }
    }
    // int server_id;
    int64_t tick_delay = -1;
    pthread_mutex_lock(thctx->mu);
    thctx->server_id++;
    // server_id = thctx->server_id;
    if (!thctx->serving) {
        thctx->serving = true;
        if (evio->events.serving) {
            char **saddrs = emalloc(naddrsfds*sizeof(char *));
            if (!saddrs) {
                eprintf(true, "%s", strerror(ENOMEM));
            }
            int k = 0;
            for (int i = 0; i < thctx->naddrs; i++) {
                for (int j = 0; j < thctx->paddrs[i]->nfds; j++) {
                    saddrs[k++] = thctx->paddrs[i]->addrs[j];
                }
            }
            evio->events.serving((const char**)saddrs, naddrsfds, evio->udata);
        }
        if (evio->events.tick) {
            tick_delay = evio->events.tick(evio->udata);
            if (tick_delay < 0) {
                tick_delay = 0;
            }
        }
    } else {
        evio->events.tick = NULL;
    }
    pthread_mutex_unlock(thctx->mu);

    bool synced = false;
    char buffer[4096];
    int fds[128];

    int64_t start = evio_now();
    for (;;) {
        int64_t delay = synced ? tick_delay : 0;
        int n = net_events(qfd, fds, sizeof(fds)/sizeof(int), delay);
        if (n == -1) {
            panic("net_events: %s", strerror(errno));
        }
        if (evio->events.tick) {
            int64_t now = evio_now();
            int64_t elapsed = now-start;
            if (elapsed > tick_delay) {
                start = now;
                tick_delay = ((int64_t)evio->events.tick(evio->udata));
                tick_delay = tick_delay < 0 ? 0 : tick_delay;
            }
        }
        if (evio->faulty) {
            // close faulty connections
            while (evio->faulty) {
                close_remove_conn(evio->faulty, evio);
                evio->faulty = evio->faulty->next_faulty;
            }
            evio->faulty = false;
            continue;
        }
        if (!synced) {
            // sync before doing anything with connections.
            if (evio->events.sync) {
                synced = evio->events.sync(evio->udata);
                if (!synced) {
                    continue;
                }
            } else {
                synced = true;
            }
        }
        for (int i = 0; i < n; i++) {
            // not a connection, check if it's a server socket.
            int j = which_socketfd(fds[i], thctx->paddrs, thctx->naddrs);
            if (j != -1) {
                // accept the incoming connection.
                net_accept(evio, qfd, fds[i], thctx->paddrs[j]);
                continue;
            }
            struct evio_conn *conn = get_conn(evio, fds[i]);
            if (!conn) {
                continue;
            }
            if (!conn_flush(evio, conn)) {
                continue;
            }
            while (true) {
                int n = read(conn->fd, buffer, sizeof(buffer)-1);
                if (n <= 0) {
                    if (n != -1 || errno != EAGAIN) {
                        close_remove_conn(conn, evio);
                    }
                    break;
                }
                buffer[n] = '\0';
                if (evio->events.data) {
                    conn->woke = true;
                    evio->events.data(conn, buffer, n, evio->udata);
                    conn->woke = false;
                }
            }
        }
        if (evio->events.sync && n > 0) {
            synced = evio->events.sync(evio->udata);
            if (!synced) {
                continue;
            }
        }
        for (int i = 0; i < n; i++) {
            if (which_socketfd(fds[i], thctx->paddrs, thctx->naddrs) != -1) {
                continue;
            }
            struct evio_conn *conn = get_conn(evio, fds[i]);
            if (!conn) {
                continue;
            }
            if (!conn_flush(evio, conn)) {
                continue;
            }
            if (conn->wbuf.cap > 4096) {
                efree(conn->wbuf.data);
                conn->wbuf.data = NULL;
                conn->wbuf.cap = 0;
            }
        }
    }
    return NULL;
}

void evio_main_mt(const char *addrs[], int naddrs, struct evio_events events, 
                  void *udata, int nthreads)
{
    signal(SIGPIPE, SIG_IGN);
    // create local evio for the purpose of error logging only.
    struct evio _evio;
    struct evio *evio = &_evio;
    memset(evio, 0, sizeof(struct evio));
    evio->events = events;
    evio->udata = udata;
    struct addr **paddrs = emalloc(naddrs * sizeof(struct addr*));
    if (!paddrs) {
        eprintf(true, "%s", strerror(ENOMEM));
    }
    memset(paddrs, 0, naddrs * sizeof(struct addr*));
    for (int i = 0; i < naddrs; i++) {
        paddrs[i] = addr_listen(evio, addrs[i]);
    }
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    struct thread_context thctx;
    memset(&thctx, 0, sizeof(struct thread_context));
    thctx.mu = &mu;
    thctx.serving = false;
    thctx.events = events;
    thctx.udata = udata;
    thctx.paddrs = paddrs;
    thctx.naddrs = naddrs;
    if (nthreads <= 0) {
        nthreads = sysconf(_SC_NPROCESSORS_ONLN);
    }
    if (nthreads < 1) {
        nthreads = 1;
    }
    for (long i = 1; i < nthreads; i++) {
        pthread_t th;
        int res = pthread_create(&th, NULL, thread, &thctx);
        if (res) {
            eprintf(true, "pthread_create: %s", strerror(errno));
        }
    }
    thread(&thctx);
}

void evio_main(const char *addrs[], int naddrs, struct evio_events events, 
               void *udata)
{
    evio_main_mt(addrs, naddrs, events, udata, 1);
}



//==============================================================================
// TESTS
// $ cc -DEVIO_TEST -pthread *.c && ./a.out
//==============================================================================
#ifdef EVIO_TEST

#include <pthread.h>
#include <assert.h>
#include <time.h>
#include <setjmp.h> 

int tseed = 0;     // test random seed
int ttimeout = 30; // timeout of all tests in seconds


void *test_timeout(void *udata) {
    sleep(ttimeout);
    printf("timeout elapsed\n");
    exit(1);
    return NULL;
}

void taddrserving(const char **addrs, int naddrs, void *udata) {
    longjmp(*((jmp_buf*)udata), 1);
}

void taddrerror(const char *msg, bool fatal, void *udata) {
    longjmp(*((jmp_buf*)udata), 2);
}

void test_addr(const char *addr, bool expect_ok) {
    struct evio_events evs = { .serving = taddrserving, .error = taddrerror, };
    jmp_buf buf;
    int ret = setjmp(buf);
    switch (ret) {
    case 1: 
        if (!expect_ok) {
            fprintf(stderr, "expected ok, got bad: %s\n", addr);
            exit(1);
        }
        return;
    case 2: 
        if (expect_ok) {
            fprintf(stderr, "expected bad, got ok: %s\n", addr);
            exit(1);
        }
        return;
    }
    evio_main((const char *[]){ addr }, 1, evs, &buf);
    abort();
}

void test_bad_addrs() {
    test_addr("badaddr626:0", false);
    test_addr("badaddr626:12312312", false);
    test_addr("badaddr626:usodifus", false);
    test_addr("badaddr626:", false);
    test_addr("tcp://badaddr626", false);
    test_addr("http://badaddr626", false);
    test_addr("unix://badaddr626", true);
    test_addr("badaddr626", true);
    remove("badaddr626");
    test_addr("unix://badaddr626:99", false);
    test_addr("[::1]:0", true);
    test_addr("tcp://[::1]:0", true);
    test_addr("tcp://[::1]:-1", false);
    test_addr("tcp://[::1]:-1", false);
    test_addr("localhost:0", true);
    test_addr("tcp://localhost:0", true);
}



struct tctx {
    pthread_mutex_t ready;
    int copened;
    int cclosed;
};

void tserving(const char **addrs, int naddrs, void *udata) {
    struct tctx *ctx = udata;
    pthread_mutex_unlock(&ctx->ready);
}

void terror(const char *msg, bool fatal, void *udata) {
    printf("error: %s\n", msg);
    abort();
}

void topened(struct evio_conn *conn, void *udata) {
    struct tctx *ctx = udata;
    pthread_mutex_lock(&ctx->ready);
    ctx->copened++;
    pthread_mutex_unlock(&ctx->ready);
}

void tclosed(struct evio_conn *conn, void *udata) {
    struct tctx *ctx = udata;
    pthread_mutex_lock(&ctx->ready);
    ctx->cclosed++;
    pthread_mutex_unlock(&ctx->ready);
}

void tdata(struct evio_conn *conn, const void *data, size_t len, 
           void *udata)
{
    evio_conn_write(conn, data, len);
}

void *server_main(void *udata) {
    struct evio_events evs = {
        .serving = tserving,
        .error = terror,
        .opened = topened,
        .closed = tclosed,
        .data = tdata,
    };
    const char *addrs[] = { "unix://tsock", "tcp://127.0.0.1:12345" };
    evio_main(addrs, 2, evs, udata);
    return NULL;
}

struct cctx {
    bool unix1;
    struct tctx *tctx;
};

pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
int trand() {
    pthread_mutex_lock(&mu);
    int v = rand();
    pthread_mutex_unlock(&mu);
    return v;
}

void *client_main(void *udata) {
    struct cctx *ctx = udata;

    // ensure the server started
    // sleep(1);
    pthread_mutex_lock(&ctx->tctx->ready);
    pthread_mutex_unlock(&ctx->tctx->ready);


    int sockfd;
    struct sockaddr servaddr; 
    memset(&servaddr, 0, sizeof(servaddr)); 
    if (!ctx->unix1) {
        // tcp
        assert((sockfd = socket(AF_INET, SOCK_STREAM, 0)) != -1); 
        struct sockaddr_in *addr = (struct sockaddr_in *)&servaddr;
        addr->sin_family = AF_INET; 
        addr->sin_addr.s_addr = inet_addr("127.0.0.1"); 
        addr->sin_port = htons(12345); 
    } else {
        // unix
        assert((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) != -1); 
        struct sockaddr_un *addr = (struct sockaddr_un *)&servaddr;
        addr->sun_family = AF_UNIX;
        strcpy(addr->sun_path, "tsock");
    }
    // connect the client socket to server socket 
    assert(!connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)));

    // send around 100MB of paritally random packet up to 1MB, which
    // will be echoed back.
    int TSIZE = 100 * 1024 * 1024;
    int PSIZE =   1 * 1024 * 1024;
    int sent = 0;
    char *data = malloc(PSIZE);
    assert(data);
    for (int i = 0; i < PSIZE; i++) {
        data[i] = i;
    }
    char packet[4096];
    int writes = 0;
    while (sent < TSIZE) {
        int nbytes = trand() % PSIZE;
        if (writes%10==9) {
            nbytes = 0;
        }
        if (nbytes > 0) {
            data[trand()%nbytes] = trand();
            data[trand()%nbytes] = trand();
            data[trand()%nbytes] = trand();
            data[trand()%nbytes] = trand();
        }
        int written = 0;
        while (written < nbytes) {
            int n = write(sockfd, data+written, nbytes-written);
            assert(n > 0);
            written += n;
        }
        assert(written == nbytes);
        int bleft = nbytes;
        int bread = 0;
        while (bread < nbytes) {
            int n = read(sockfd, packet, sizeof(packet));
            assert(n > 0);
            assert(n <= bleft);
            assert(memcmp(packet, data+bread, n) == 0);
            bread += n;
        }
        assert(bread == nbytes);
        sent += nbytes;
        writes++;
    }
    free(data);

    assert(!close(sockfd));
    return NULL;
}

void test_client_server() {
    struct tctx ctx = {
        .ready = PTHREAD_MUTEX_INITIALIZER,
    };
    pthread_mutex_lock(&ctx.ready);

    pthread_t sth, cth0, cth1, cth2, cth3;
    pthread_create(&sth, NULL, server_main, &ctx);

    

    struct cctx ctx0 = { .unix1 = false, .tctx = &ctx };
    pthread_create(&cth0, NULL, client_main, &ctx0);

    struct cctx ctx1 = { .unix1 = false, .tctx = &ctx };
    pthread_create(&cth1, NULL, client_main, &ctx1);
    
    struct cctx ctx2 = { .unix1 = false, .tctx = &ctx };
    pthread_create(&cth2, NULL, client_main, &ctx2);
    
    struct cctx ctx3 = { .unix1 = false, .tctx = &ctx };
    pthread_create(&cth3, NULL, client_main, &ctx3);
    
    pthread_join(cth0, NULL);
    pthread_join(cth1, NULL);
    pthread_join(cth2, NULL);
    pthread_join(cth3, NULL);

    pthread_mutex_unlock(&ctx.ready);
    assert(ctx.cclosed == 4);
    assert(ctx.copened == 4);
    pthread_mutex_lock(&ctx.ready);

}

int main() {
    printf("Running evio.c tests...\n");
    tseed = getenv("SEED")?atoi(getenv("SEED")):time(NULL);
    ttimeout = getenv("TIMEOUT")?atoi(getenv("TIMEOUT")):ttimeout;
    printf("seed=%d, timeout=%ds\n", tseed, ttimeout);
    pthread_t tth; // timeout thread
    pthread_create(&tth, NULL, test_timeout, NULL);
    test_bad_addrs();
    test_client_server();
    printf("PASSED\n");
}

#endif
