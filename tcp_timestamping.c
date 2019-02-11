#include <stdio.h>
#include <error.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <linux/net_tstamp.h>
#include <linux/errqueue.h>


#define ENFORCE(condition, report) \
    do {                                        \
        if(!(condition)) {                      \
            report;                             \
            exit(1);                            \
        }                                       \
    } while(0)                                  \


#define ENFORCE_ERRNO(condition, message)       \
    ENFORCE((condition >= 0), perror(message))  \

#define ENFORCE_CUSTOM(condition, ...)                            \
    ENFORCE((condition), fprintf(stderr, __VA_ARGS__))            \

#define ENABLE 1
#define DISABLE 0

#define PORT 10000

#define MESSAGE_MAX_SIZE 65507
#define MAX_EVENTS 32

#define AVG_WINDOW 60

static const char* stages[] = {
    "SCM_TSTAMP_SND", "SCM_TSTAMP_SCHED", "SCM_TSTAMP_ACK"
};

inline long delta_ns(const struct timespec* lhs, const struct timespec* rhs) {
    return 1000000000*(lhs->tv_sec - rhs->tv_sec) + (lhs->tv_nsec - rhs->tv_nsec);
}

int main() {
    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    ENFORCE_ERRNO(tcp, "socket: tcp");
    uint32_t flags;

    flags = ENABLE;
    ENFORCE_ERRNO(setsockopt(tcp, SOL_SOCKET, SO_RXQ_OVFL, &flags, sizeof(flags)),
                  "setsockopt: SO_RXQ_OVFL");

    flags = ENABLE;
    ENFORCE_ERRNO(setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags)),
                  "setsockopt: TCP_NODELAY");

    struct sockaddr_in remote;
    remote.sin_family = AF_INET;
    remote.sin_port = htons(PORT);
    remote.sin_addr.s_addr = INADDR_ANY;
    ENFORCE_ERRNO(connect(tcp, (struct sockaddr*)&remote, sizeof(remote)), "connect: tcp");

    flags = ENABLE;
    ENFORCE_ERRNO(setsockopt(tcp, SOL_SOCKET, SO_TIMESTAMPNS, &flags, sizeof(flags)),
                  "setsockopt: SO_TIMESTAMPNS");

    flags = DISABLE
            /* Not supported by RPi
            | SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_TX_HARDWARE
            */
            | SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_TX_SOFTWARE
            | SOF_TIMESTAMPING_TX_SCHED
            | SOF_TIMESTAMPING_TX_ACK
            ;

    flags = flags
            | SOF_TIMESTAMPING_SOFTWARE
            | SOF_TIMESTAMPING_RAW_HARDWARE
            ;

    /* sender */
    flags = flags
            | SOF_TIMESTAMPING_OPT_ID /* Note: must be set on connected socket */
            /* | SOF_TIMESTAMPING_OPT_CMSG */
            | SOF_TIMESTAMPING_OPT_TSONLY
            ;
    ENFORCE_ERRNO(setsockopt(tcp, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)),
                  "setsockopt: SO_TIMESTAMPING");

    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    ENFORCE_ERRNO(timer, "timerfd_create");

    struct itimerspec interval;
    interval.it_value.tv_sec = 1;
    interval.it_value.tv_nsec = 0;
    interval.it_interval.tv_sec = 0;
    interval.it_interval.tv_nsec = 0;

    int epoll = epoll_create1(EPOLL_CLOEXEC);
    ENFORCE_ERRNO(epoll, "epoll: create");

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLERR;
    ev.data.fd = tcp;
    ENFORCE_ERRNO(epoll_ctl(epoll, EPOLL_CTL_ADD, tcp, &ev), "epoll_ctrl: tcp");

    ev.events = EPOLLIN;
    ev.data.fd = timer;
    ENFORCE_ERRNO(epoll_ctl(epoll, EPOLL_CTL_ADD, timer, &ev), "epoll_ctrl: timer");

    long counter = 0;
    uint32_t total_sent = 0;
    long long call[AVG_WINDOW];
    long long timestampns[sizeof(stages)/sizeof(stages[0])][AVG_WINDOW];
    long long timestamping[sizeof(stages)/sizeof(stages[0])][AVG_WINDOW];
    struct epoll_event events[MAX_EVENTS];
    printf("Sending data...\n");

    while(1) {
        const char hello[] = "hello\n";
        struct timespec now;
        ENFORCE_ERRNO(clock_gettime(CLOCK_REALTIME, &now), "clock_gettime: now");
        size_t sent = send(tcp, hello, strlen(hello), MSG_DONTWAIT);
        total_sent += sent;
        struct timespec done;
        ENFORCE_ERRNO(clock_gettime(CLOCK_REALTIME, &done), "clock_gettime: done");
        call[counter % AVG_WINDOW] = delta_ns(&done, &now);
        /* printf("NOW                : %ld.%09ld sent: %ld ns counter: %u\n", */
        /*        now.tv_sec, now.tv_nsec, delta_ns(&done, &now), counter); */

        ENFORCE_CUSTOM(sent == strlen(hello),
                       "Unable send %zd bytes\n", strlen(hello));

        ENFORCE_ERRNO(timerfd_settime(timer, 0, &interval, NULL), "timerfd_settime");

        int wait = 1;
        while(wait) {
            int fds = epoll_wait(epoll, events, MAX_EVENTS, -1);
            ENFORCE_ERRNO(fds, "epoll_wait");
            for(int i = 0; i < fds; ++i) {
                if(events[i].data.fd == tcp) {
                    if((events[i].events & EPOLLERR) != 0) {
                        char control[65535];
                        struct msghdr aux;
                        aux.msg_name = NULL;
                        aux.msg_namelen = 0;
                        aux.msg_iov = NULL;
                        aux.msg_iovlen = 0;
                        aux.msg_control = control;
                        aux.msg_controllen = sizeof(control);

                        ENFORCE_ERRNO(recvmsg(tcp, &aux, MSG_ERRQUEUE), "recvmsg: tcp");

                        size_t stage = 0;
                        long timestampns_delta = 0;
                        long timestamping_delta = 0;

                        for(struct cmsghdr* cmsg = CMSG_FIRSTHDR(&aux);
                            cmsg != NULL;
                            cmsg = CMSG_NXTHDR(&aux, cmsg)) {
                            /* printf("cmsg: level %d type %d\n", */
                            /*        cmsg->cmsg_level, */
                            /*        cmsg->cmsg_type); */
                            struct scm_timestamping* data;
                            struct timespec* pts;
                            struct sock_extended_err* err;
                            switch(cmsg->cmsg_type) {
                                case SCM_TIMESTAMPNS:
                                    pts = (struct timespec*)CMSG_DATA(cmsg);
                                    timestampns_delta = delta_ns(pts, &now);
                                    /* printf("SCM_TIMESTAMS      : %ld.%09ld latency: %ld ns\n", */
                                    /*        pts->tv_sec, pts->tv_nsec, delta_ns(pts, &now)); */
                                    //timestampns[counter % AVG_WINDOW] = delta_ns(&now, pts);
                                    break;
                                case SCM_TIMESTAMPING:
                                    data = (struct scm_timestamping*)CMSG_DATA(cmsg);
                                    timestamping_delta = delta_ns(&data->ts[0], &now);
                                    /* for(size_t idx = 0; idx < 2; ++idx) { */
                                    /*     if(data->ts[idx].tv_sec == 0 && */
                                    /*        data->ts[idx].tv_nsec == 0) { */
                                    /*         continue; */
                                    /*     } */
                                    /*     //timestamping[counter % AVG_WINDOW] = delta_ns(&now, &data->ts[0]); */
                                    /*     printf("SCM_TIMESTAMPING[%zd]: %ld.%09ld latency: %ld ns\n", */
                                    /*            idx, data->ts[idx].tv_sec, data->ts[idx].tv_nsec, */
                                    /*            delta_ns(&data->ts[idx], &now)); */
                                    /*     } */
                                    break;
                                case IP_RECVERR:
                                    ENFORCE_CUSTOM(cmsg->cmsg_level == SOL_IP,
                                                   "Unexpected level: %d wants: %d\n", cmsg->cmsg_level, SOL_IP);
                                    err = (struct sock_extended_err*)CMSG_DATA(cmsg);
                                    ENFORCE_CUSTOM(err->ee_errno == ENOMSG,
                                                   "Unexpected errno: %d wants: %d\n", err->ee_errno, ENOMSG);
                                    ENFORCE_CUSTOM(err->ee_origin == SO_EE_ORIGIN_TIMESTAMPING,
                                                   "Unexpected origin: %d wants: %d\n", err->ee_origin, SO_EE_ORIGIN_TIMESTAMPING);
                                    uint32_t message_id = err->ee_data;
                                    ENFORCE_CUSTOM(total_sent - 1 == message_id,
                                                   "Unexpected message_id: %u wants: %u\n", message_id, total_sent);
                                    stage = err->ee_info;
                                    /* printf("Stage: %s\n", stages[stage]); */
                                    break;
                                default:
                                    ENFORCE_CUSTOM(0, "unexpected control message type: %d\n", cmsg->cmsg_type);
                            }
                        }
                        timestampns[stage][counter % AVG_WINDOW] = timestampns_delta;
                        timestamping[stage][counter % AVG_WINDOW] = timestamping_delta;
                    }
                }
                if(events[i].data.fd == timer) {
                    if((events[i].events & EPOLLIN) != 0) {
                        uint64_t exp = 0;
                        ENFORCE_CUSTOM(read(timer, &exp, sizeof(exp)) == sizeof(exp) && exp == 1, "read: timer");
                        wait = 0;
                    }
                }
            }
        }
        ++counter;
        if((counter % AVG_WINDOW) == 0) {
            for(size_t s = 0; s < sizeof(stages)/sizeof(stages[0]); ++s) {
                long long avg_timestampns = 0;
                long long avg_timestamping = 0;
                for(size_t i = 0; i < AVG_WINDOW; ++i) {
                    avg_timestampns += timestampns[s][i];
                    avg_timestamping += timestamping[s][i];
                }
                printf("Avg %s over %d - timestampns: %llu ns, timestamping: %llu ns\n",
                       stages[s], AVG_WINDOW, avg_timestampns / AVG_WINDOW, avg_timestamping / AVG_WINDOW);
            }
            long long avg_done = 0;
            for(size_t i = 0; i < AVG_WINDOW; ++i) {
                avg_done += call[i];
            }
            printf("Avg sent() over %d - %llu ns\n", AVG_WINDOW, avg_done / AVG_WINDOW);
        }
    }
    ENFORCE_ERRNO(close(epoll), "close: epoll");
    ENFORCE_ERRNO(close(tcp), "close: tcp");
    return 0;
}
