/* loop.c — epoll event loop with µs timers */
#include "loop.h"
#include "util.h"

#include <errno.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

static int g_epfd = -1;
static struct ftimer *g_timers;

int loop_init(void)
{
    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    return g_epfd < 0 ? -1 : 0;
}

int loop_add(struct evh *h, uint32_t events)
{
    struct epoll_event ev = { .events = events, .data.ptr = h };
    return epoll_ctl(g_epfd, EPOLL_CTL_ADD, h->fd, &ev);
}

int loop_mod(struct evh *h, uint32_t events)
{
    struct epoll_event ev = { .events = events, .data.ptr = h };
    return epoll_ctl(g_epfd, EPOLL_CTL_MOD, h->fd, &ev);
}

void loop_del(struct evh *h)
{
    if (h->fd >= 0)
        epoll_ctl(g_epfd, EPOLL_CTL_DEL, h->fd, NULL);
}

void timer_init(struct ftimer *t, timer_cb cb, void *arg)
{
    t->cb = cb;
    t->arg = arg;
    t->armed = false;
    t->next = NULL;
}

static void timer_unlink(struct ftimer *t)
{
    for (struct ftimer **pp = &g_timers; *pp; pp = &(*pp)->next) {
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    }
    t->next = NULL;
    t->armed = false;
}

void timer_arm(struct ftimer *t, uint64_t delay_us)
{
    if (t->armed)
        timer_unlink(t);
    t->when = now_us() + delay_us;
    t->armed = true;
    t->next = g_timers;
    g_timers = t;
}

void timer_cancel(struct ftimer *t)
{
    if (t->armed)
        timer_unlink(t);
}

static void run_timers(void)
{
    uint64_t now = now_us();
    bool again = true;

    /* A callback may arm/cancel any timer, so rescan after each one. */
    while (again) {
        again = false;
        for (struct ftimer *t = g_timers; t; t = t->next) {
            if (t->when <= now) {
                timer_unlink(t);
                t->cb(t);
                again = true;
                break;
            }
        }
    }
}

void loop_once(uint64_t max_wait_us)
{
    struct epoll_event evs[32];
    uint64_t now = now_us(), wait = max_wait_us;
    struct timespec ts;
    int n;

    for (struct ftimer *t = g_timers; t; t = t->next) {
        uint64_t d = t->when > now ? t->when - now : 0;
        if (d < wait)
            wait = d;
    }
    ts.tv_sec = (time_t)(wait / 1000000u);
    ts.tv_nsec = (long)(wait % 1000000u) * 1000;
    n = epoll_pwait2(g_epfd, evs, (int)ARRAY_LEN(evs), &ts, NULL);
    if (n < 0 && errno != EINTR)
        n = 0;
    for (int i = 0; i < n; i++) {
        struct evh *h = evs[i].data.ptr;
        h->cb(h, evs[i].events);
    }
    run_timers();
}
