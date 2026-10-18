/* loop.h — single-threaded epoll event loop with µs timers (REQ-009) */
#ifndef FNETD_LOOP_H
#define FNETD_LOOP_H

#include <stdbool.h>
#include <stdint.h>

struct evh;
typedef void (*ev_cb)(struct evh *h, uint32_t events);

/* Embedded in the owner's struct; must stay valid while registered. */
struct evh {
    int   fd;
    ev_cb cb;
    void *arg;
};

struct ftimer;
typedef void (*timer_cb)(struct ftimer *t);

struct ftimer {
    uint64_t       when;      /* now_us() deadline */
    timer_cb       cb;
    void          *arg;
    bool           armed;
    struct ftimer *next;
};

int  loop_init(void);
int  loop_add(struct evh *h, uint32_t events);
int  loop_mod(struct evh *h, uint32_t events);
void loop_del(struct evh *h);

void timer_init(struct ftimer *t, timer_cb cb, void *arg);
void timer_arm(struct ftimer *t, uint64_t delay_us);
void timer_cancel(struct ftimer *t);

/* Wait for events/timers (at most max_wait_us) and dispatch them. */
void loop_once(uint64_t max_wait_us);

#endif
