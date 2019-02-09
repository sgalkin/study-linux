#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include <sys/timerfd.h>
#include <sys/epoll.h>

static const int CLOCK_ID = CLOCK_MONOTONIC;
static const size_t MAX_EVENTS = 16;
static const size_t WINDOW = 300;

int main() {
    int timer = timerfd_create(CLOCK_ID, TFD_NONBLOCK | TFD_CLOEXEC);
    if(timer < 0) {
        perror("timerfd_create");
        exit(1);
    }

    struct timespec now;
    if(clock_gettime(CLOCK_ID, &now) < 0) {
        perror("clock_gettime");
        exit(2);
    }

    struct itimerspec interval;
    interval.it_value.tv_sec = now.tv_sec + 2;
    interval.it_value.tv_nsec = 0;
    interval.it_interval.tv_sec = 1;
    interval.it_interval.tv_nsec = 0;
    if(timerfd_settime(timer, TFD_TIMER_ABSTIME, &interval, NULL) < 0) {
        perror("timerfd_settime");
        exit(2);
    }
    printf("timer started\n");

    int epoll = epoll_create1(EPOLL_CLOEXEC);
    if(epoll < 0) {
        perror("epoll_create1");
        exit(1);
    }
    struct epoll_event ev;
    ev.events = EPOLLET | EPOLLIN | EPOLLOUT | EPOLLONESHOT | EPOLLWAKEUP;
    ev.data.fd = timer;
    if(epoll_ctl(epoll, EPOLL_CTL_ADD, timer, &ev) < 0) {
        perror("epoll_ctrl: timer");
        exit(4);
    }

    struct epoll_event events[MAX_EVENTS];
    struct timespec expected = interval.it_value;
    struct timespec prev;
    prev.tv_sec = prev.tv_nsec = 0;

    long long offset_w[WINDOW];
    long long length_w[WINDOW];
    size_t counter = 0;
    do {
        int fds = epoll_wait(epoll, events, MAX_EVENTS, -1);
        if(fds < 0) {
            perror("epoll_wait");
            exit(4);
        }
        for(int i = 0; i < fds; ++i) {
            if(events[i].data.fd == timer &&
               (events[i].events & EPOLLIN) != 0) {
                uint64_t exp = 0;
                size_t s = read(timer, &exp, sizeof(exp));
                if(s != sizeof(exp)) {
                    perror("read");
                    exit(3);
                }
                if(exp != 1) {
                    printf("too many expirations: %lu\n", exp);
                    exit(5);
                }
                struct timespec now;
                if(clock_gettime(CLOCK_ID, &now) < 0) {
                    perror("clock_gettime: loop");
                    exit(2);
                }
                long long delta;
                delta = 1000000000*(now.tv_sec - expected.tv_sec);
                delta += (now.tv_nsec - expected.tv_nsec);

                long long length = 0;
                if(prev.tv_sec == 0 && prev.tv_nsec == 0) {
                    length = 1000000000*interval.it_interval.tv_sec + interval.it_interval.tv_nsec;
                } else {
                    length = 1000000000*(now.tv_sec - prev.tv_sec);
                    length += (now.tv_nsec - prev.tv_nsec);
                }
                /* printf("%ld.%09ld: Mismatch: %lld ns; interval: %lld ns\n", */
                /*        now.tv_sec, now.tv_nsec, delta, length); */
                prev = now;

                struct itimerspec current;
                if(timerfd_gettime(timer, &current) < 0) {
                    perror("timerfs_gettime");
                    exit(5);
                }
                expected.tv_sec += 1 + current.it_value.tv_sec;

                if(epoll_ctl(epoll, EPOLL_CTL_MOD, timer, &ev) < 0) {
                    perror("epoll_ctrl: timer");
                    exit(4);
                }

                length_w[counter % WINDOW] = length;
                offset_w[counter % WINDOW] = delta;
                ++counter;
                if(counter % WINDOW == 0) {
                    long long avg_offset = 0;
                    long long avg_period = 0;
                    for(size_t i = 0; i < WINDOW; ++i) {
                        avg_period += length_w[i];
                        avg_offset += offset_w[i];
                    }
                    printf("Avg over %zd intervals: period %lld ns, offset %lld ns\n",
                           WINDOW, avg_period / WINDOW, avg_offset / WINDOW);
                }
            }
        }
    } while(1);

    if(close(epoll) < 0) {
        perror("close: epoll");
        exit(1);
    }
    if(close(timer) < 0) {
        perror("close: timer");
        exit(1);
    }
    return 0;
}
