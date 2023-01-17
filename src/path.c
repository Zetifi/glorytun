#include "common.h"
#include "ctl.h"
#include "str.h"

#include <stdio.h>
#include <stdint.h>
#define _BSD_SOURCE
#include <endian.h>
#include <sys/socket.h>
#include <unistd.h>
#include <net/if.h>

#include "../argz/argz.h"

static void gt_path_print_status(struct mud_path *path, int term)
{
    char remote_address[INET6_ADDRSTRLEN];
    gt_toaddr(remote_address, sizeof(remote_address), (struct sockaddr *)&path->remote_address);

    const char *statestr = NULL;

    switch (path->state)
    {
    case MUD_UP:
        statestr = "UP";
        break;
    case MUD_BACKUP:
        statestr = "BACKUP";
        break;
    case MUD_DOWN:
        statestr = "DOWN";
        break;
    default:
        return;
    }

    printf(term ? "path %s\n"
                  "  status:  %s\n"
                  "  interface_name:    %s\n"
                  "  remote_address:    %s port %" PRIu16 "\n"
                  "  mtu:     %zu bytes\n"
                  "  rtt:     %.3f ms\n"
                  "  rttvar:  %.3f ms\n"
                  "  rate:    %s\n"
                  "  preferred: %s\n"
                  "  losslim: %u%%\n"
                  "  rttlim: %" PRIu64 " ms\n"
                  "  beat:    %" PRIu64 " ms\n"
                  "  tx:\n"
                  "    rate:  %" PRIu64 " bytes/sec\n"
                  "    loss:  %" PRIu64 " percent\n"
                  "    total: %" PRIu64 " packets\n"
                  "  rx:\n"
                  "    rate:  %" PRIu64 " bytes/sec\n"
                  "    loss:  %" PRIu64 " percent\n"
                  "    total: %" PRIu64 " packets\n"
                : "path %s %s"
                  " %s -> %s %" PRIu16
                  " %zu %.3f %.3f"
                  " %s %s %u %" PRIu64 ""
                  " %" PRIu64
                  " %" PRIu64 " %" PRIu64 " %" PRIu64
                  " %" PRIu64 " %" PRIu64 " %" PRIu64
                  "\n",
           statestr,
           path->ok ? "OK" : "DEGRADED",
           path->interface_name,
           remote_address[0] ? remote_address : "-",
           gt_get_port((struct sockaddr *)&path->remote_address),
           path->mtu.ok,
           (double)path->rtt.val / 1e3,
           (double)path->rtt.var / 1e3,
           path->conf.fixed_rate ? "fixed" : "auto",
           path->conf.preferred ? "PREFERRED" : "NOT PREFERRED",
           path->conf.loss_limit,
           path->conf.rtt_limit / 1000,
           path->conf.beat / 1000,
           path->tx.rate,
           path->tx.loss,
           path->tx.total,
           path->rx.rate,
           path->rx.loss,
           path->rx.total);
}

static int
gt_path_status(int fd, enum mud_state state, const char *interface_name)
{
    struct ctl_msg req = {
        .type = CTL_PATH_STATUS,
    }, res = {0};

    if (send(fd, &req, sizeof(struct ctl_msg), 0) == -1)
        return -1;

    struct mud_path path[MUD_PATH_MAX];
    int count = 0;

    while (1) {
        if (recv(fd, &res, sizeof(struct ctl_msg), 0) == -1)
            return -1;

        if (res.type != req.type) {
            errno = EBADMSG;
            return -1;
        }

        if (res.ret == EAGAIN) {
            memcpy(&path[count], &res.path_status, sizeof(struct mud_path));
            count++;
        } else if (res.ret) {
            errno = res.ret;
            return -1;
        } else break;
    }

    int term = isatty(1);

    for (int i = 0; i < count; i++)
    {
        if ((state == MUD_EMPTY || path[i].state == state) &&
            (strlen(req.path.interface_name) == 0 || strcmp(req.path.interface_name, &path[i].interface_name[0]) == 0))
        {
            gt_path_print_status(&path[i], term);
        }
    }

    return 0;
}

int
gt_path(int argc, char **argv)
{
    const char *dev = NULL;
    const char *ifname = NULL;
    unsigned int loss_limit = 0;
    unsigned int rtt_limit = 0;

    struct ctl_msg req = {
        .type = CTL_STATE,
        .path = {
            .state = MUD_EMPTY,
        },
    }, res = {0};

    struct argz ratez[] = {
        {"fixed|auto", NULL, NULL, argz_option},
        {"tx", "BYTES/SEC", &req.path.rate_tx, argz_bytes},
        {"rx", "BYTES/SEC", &req.path.rate_rx, argz_bytes},
        {NULL}};

    struct argz pathz[] = {
        {NULL, "IFNAME", &ifname, argz_str},
        {"dev", "NAME", &dev, argz_str}, 
        {"up|backup|down", NULL, NULL, argz_option},
        {"rate", NULL, &ratez, argz_option},
        {"beat", "SECONDS", &req.path.beat, argz_time},
        {"preferred", NULL, NULL, argz_option},
        {"losslimit", "PERCENT", &loss_limit, argz_percent},
        {"rttlimit", "MS", &rtt_limit, argz_ulong},
        {NULL}};

    if (argz(pathz, argc, argv))
        return 1;

    int fd = ctl_connect(dev);

    if (fd < 0) {
        switch (fd) {
        case -1:
            perror("path");
            break;
        case CTL_ERROR_NONE:
            gt_log("no device\n");
            break;
        case CTL_ERROR_MANY:
            gt_log("please choose a device\n");
            break;
        default:
            gt_log("couldn't connect\n");
        }
        return 1;
    }

    int set = argz_is_set(pathz, "rate")
           || argz_is_set(pathz, "beat")
           || argz_is_set(pathz, "losslimit")
           || argz_is_set(pathz, "rttlimit");

    if (set && !ifname) {
        gt_log("please specify an interface\n");
        return 1;
    }

    if (ifname)
    {
        if (strlen(ifname) > IFNAMSIZ)
        {
            gt_log("Interface name longer than maximum length.\n");
            return 1;
        }

        strncpy(&req.path.interface_name[0], ifname, 15);
    }

    if (argz_is_set(pathz, "up")) {
        req.path.state = MUD_UP;
    } else if (argz_is_set(pathz, "backup")) {
        req.path.state = MUD_BACKUP;
    } else if (argz_is_set(pathz, "down")) {
        req.path.state = MUD_DOWN;
    }

    if (argz_is_set(pathz, "preferred"))
    {
        req.path.preferred = 1;
    }

    if (loss_limit)
    {
        req.path.loss_limit = loss_limit * 255 / 100;
    }

    if (rtt_limit)
    {
        req.path.rtt_limit = htobe64(rtt_limit);
    }

    if (argz_is_set(ratez, "fixed")) {
        req.path.fixed_rate = 3;
    } else if (argz_is_set(ratez, "auto")) {
        req.path.fixed_rate = 1;
    }

    int ret;

    if (!ifname || (req.path.state == MUD_EMPTY && !set)) {
        ret = gt_path_status(fd, req.path.state, &req.path.interface_name[0]);
    } else {
        ret = ctl_reply(fd, &res, &req);
    }

    if (ret == -1)
        perror("path");

    ctl_delete(fd);

    return !!ret;
}
