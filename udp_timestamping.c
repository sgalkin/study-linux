#include <stdio.h>
#include <error.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/socket.h>
#include <sys/types.h>

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

#define DGRAM_MAX_SIZE 65507

#define AVG_WINDOW 600

inline long delta_ns(const struct timespec* lhs, const struct timespec* rhs) {
    return 1000000000*(lhs->tv_sec - rhs->tv_sec) + (lhs->tv_nsec - rhs->tv_nsec);
}

int main() {
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    ENFORCE_ERRNO(udp, "socket: udp");
    int flags;

    flags = ENABLE;
    ENFORCE_ERRNO(setsockopt(udp, SOL_SOCKET, SO_TIMESTAMPNS, &flags, sizeof(flags)),
                  "setsockopt: SO_TIMESTAMPNS");

    flags = DISABLE
            /* Not supported by RPi
            | SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_TX_HARDWARE
            */
            | SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_TX_SOFTWARE
            /*
            | SOF_TIMESTAMPING_TX_SCHED
            */
            /*
            | SOF_TIMESTAMPING_TX_ACK
            */
            ;

    flags = flags
            | SOF_TIMESTAMPING_SOFTWARE
            | SOF_TIMESTAMPING_RAW_HARDWARE
            ;

    /* sender */
    flags = flags
            | SOF_TIMESTAMPING_OPT_ID
            /* | SOF_TIMESTAMPING_OPT_CMSG */
            | SOF_TIMESTAMPING_OPT_TSONLY
            ;

    ENFORCE_ERRNO(setsockopt(udp, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)),
                  "setsockopt: SO_TIMESTAMPING");
    /*
    flags = ENABLE;
    ENFORCE_ERRNO(setsockopt(udp, SOL_SOCKET, SO_RXQ_OVFL, &flags, sizeof(flags)),
                  "setsockopt: SO_RXQ_OVFL");
    */
    struct sockaddr_in local;
    local.sin_family = AF_INET;
    local.sin_port = htons(PORT);
    local.sin_addr.s_addr = INADDR_ANY;
    ENFORCE_ERRNO(bind(udp, (struct sockaddr*)&local, sizeof(local)), "bind: udp");

    struct sockaddr_in remote;

    char buf[DGRAM_MAX_SIZE];
    struct iovec data;
    data.iov_base = buf;
    data.iov_len = sizeof(buf);

    char control[65536];

    long timestampns[AVG_WINDOW];
    long timestamping[AVG_WINDOW];
    long counter = 0;

    printf("Waitng for incoming DGRAMS\n");
    while(1) {
        struct msghdr aux;
        aux.msg_name = &remote;
        aux.msg_namelen = sizeof(remote);
        aux.msg_iov = &data;
        aux.msg_iovlen = 1;
        aux.msg_control = control;
        aux.msg_controllen = sizeof(control);

        ssize_t read = recvmsg(udp, &aux, 0);
        ENFORCE_ERRNO(read, "recvmsg: udp");
        ENFORCE_CUSTOM((aux.msg_flags & MSG_CTRUNC) == 0, "cmsg truncated\n");
        ENFORCE_CUSTOM((aux.msg_flags & MSG_TRUNC) == 0, "dgram truncated\n");

        struct timespec now;
        ENFORCE_ERRNO(clock_gettime(CLOCK_REALTIME, &now), "clock_gettime");
        /* printf("NOW: %ld.%09ld\n", now.tv_sec, now.tv_nsec); */

        /*
        char addr[12];
        printf("read %zu bytes from %s:%d"
               " - msg_namelen: %d, msg_iovlen: %zd, msg_controllen: %zd, flags: %d\n",
               read,
               inet_ntop(remote.sin_family, &remote.sin_addr, addr, sizeof(addr)),
               ntohs(remote.sin_port),
               aux.msg_namelen, aux.msg_iovlen, aux.msg_controllen, aux.msg_flags);
        */

        for(struct cmsghdr* cmsg = CMSG_FIRSTHDR(&aux);
            cmsg != NULL;
            cmsg = CMSG_NXTHDR(&aux, cmsg)) {
            ENFORCE_CUSTOM(cmsg->cmsg_level == SOL_SOCKET,
                           "unexpected control message level: %d\n", cmsg->cmsg_level);
            struct scm_timestamping* data;
            struct timespec* pts;
            switch(cmsg->cmsg_type) {
                case SCM_TIMESTAMPNS:
                    pts = (struct timespec*)CMSG_DATA(cmsg);
                    /* printf("SCM_TIMESTAMS      : %ld.%09ld latency: %ld ns\n", */
                    /*        pts->tv_sec, pts->tv_nsec, delta_ns(&now, pts)); */
                    timestampns[counter % AVG_WINDOW] = delta_ns(&now, pts);
                    break;
                case SCM_TIMESTAMPING:
                    data = (struct scm_timestamping*)CMSG_DATA(cmsg);
                    /* for(size_t idx = 0; idx < 2; ++idx) { */
                    /*     if(data->ts[idx].tv_sec == 0 && */
                    /*        data->ts[idx].tv_nsec == 0) { */
                    /*         continue; */
                    /*     } */
                    timestamping[counter % AVG_WINDOW] = delta_ns(&now, &data->ts[0]);
                        /* printf("SCM_TIMESTAMPING[%zd]: %ld.%09ld latency: %ld ns\n", */
                        /*        idx, data->ts[idx].tv_sec, data->ts[idx].tv_nsec, */
                        /*        delta_ns(&now, &data->ts[idx])); */
                    /* } */
                     break;
                default:
                    ENFORCE_CUSTOM(0, "unexpected control message type: %d\n", cmsg->cmsg_type);
            }
        }
        ++counter;
        if((counter % AVG_WINDOW) == 0) {
            long long avg_timestampns = 0;
            long long avg_timestamping = 0;
            for(size_t i = 0; i < AVG_WINDOW; ++i) {
                avg_timestampns += timestampns[i];
                avg_timestamping += timestamping[i];
            }
            printf("Avg over %d - timestampns: %llu ns, timestamping: %llu ns\n",
                   AVG_WINDOW, avg_timestampns / AVG_WINDOW, avg_timestamping / AVG_WINDOW);
        }
    }

    ENFORCE_ERRNO(close(udp), "close: udp");
    return 0;
}
