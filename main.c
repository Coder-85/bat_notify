#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <linux/netlink.h>
#include <libnotify/notify.h>

#define MAX_AC_DEVICES        16
#define CHANGE_NONE           0
#define CHANGE_ONLINE         1
#define BATTERY_POLL_INTERVAL 60  // seconds
#ifndef SYSFS_PATH // Guard for test
#define SYSFS_PATH "/sys/class/power_supply"
#endif
#define UEVENT_BUFFER_SIZE 4096

typedef struct {
    int capacity;
    int online;
} power_state_t;

typedef struct {
    const char *action;
    const char *subsystem;
    const char *devpath;
} uevent_t;

char battery_path[256] = {0};
char ac_path[256] = {0};

char ac_paths[MAX_AC_DEVICES][256];
char ac_types[MAX_AC_DEVICES][64];
int ac_count = 0;

int read_int(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int value = -1;
    fscanf(f, "%d", &value);
    fclose(f);
    return value;
}

void read_string(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        buf[0] = '\0';
        return;
    }

    if (fgets(buf, size, f))
        buf[strcspn(buf, "\n")] = 0;
    else
        buf[0] = '\0';

    fclose(f);
}

void update_ac_path(void)
{
    int mains_idx = -1;
    ac_path[0] = '\0';

    for (int i = 0; i < ac_count; i++)
    {
        char online_path[512];
        snprintf(online_path, sizeof(online_path), "%s/online", ac_paths[i]);
        int online = read_int(online_path);
        if (online == 1)
        {
            strncpy(ac_path, ac_paths[i], sizeof(ac_path) - 1);
            return;
        }
        if (mains_idx == -1 && strcmp(ac_types[i], "Mains") == 0)
            mains_idx = i;
    }

    if (ac_count > 0)
    {
        int fallback = (mains_idx != -1) ? mains_idx : 0;
        strncpy(ac_path, ac_paths[fallback], sizeof(ac_path) - 1);
    }
}

void detect_power_devices(void)
{
    DIR *dir = opendir(SYSFS_PATH);

    if (!dir)
    {
        perror("opendir");
        exit(EXIT_FAILURE);
    }

    struct dirent *entry;

    while ((entry = readdir(dir)))
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char base_path[512];
        snprintf(base_path, sizeof(base_path), "%s/%s", SYSFS_PATH, entry->d_name);

        char type_path[512];
        snprintf(type_path, sizeof(type_path), "%s/type", base_path);

        if (access(type_path, R_OK) != 0)
            continue;

        char type[64] = {0};
        read_string(type_path, type, sizeof(type));

        if (strcmp(type, "Battery") == 0 && battery_path[0] == 0)
            strncpy(battery_path, base_path, sizeof(battery_path) - 1);

        if ((strcmp(type, "Mains") == 0 || strcmp(type, "USB") == 0) && ac_count < MAX_AC_DEVICES)
        {
            strncpy(ac_paths[ac_count], base_path, sizeof(ac_paths[ac_count]) - 1);
            strncpy(ac_types[ac_count], type, sizeof(ac_types[ac_count]) - 1);
            ac_count++;
        }
    }

    closedir(dir);

    if (!battery_path[0])
    {
        fprintf(stderr, "No battery found\n");
        exit(EXIT_FAILURE);
    }

    update_ac_path();

    printf("Battery: %s\n", battery_path);
    if (ac_path[0])
        printf("AC: %s\n", ac_path);
}

void read_state(power_state_t *state)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/capacity", battery_path);
    state->capacity = read_int(path);

    update_ac_path();

    if (ac_path[0])
    {
        snprintf(path, sizeof(path), "%s/online", ac_path);
        state->online = read_int(path);
    }
    else
    {
        state->online = -1;
    }
}

int state_changed(const power_state_t *a, const power_state_t *b)
{
    if (a->online != b->online)
        return CHANGE_ONLINE;

    return CHANGE_NONE;
}

void parse_uevent(char *buf, int len, uevent_t *ev)
{
    memset(ev, 0, sizeof(*ev));

    for (int i = 0; i < len; )
    {
        char *s = buf + i;
        if (strncmp(s, "ACTION=", 7) == 0)
            ev->action = s + 7;
        else if (strncmp(s, "SUBSYSTEM=", 10) == 0)
            ev->subsystem = s + 10;
        else if (strncmp(s, "DEVPATH=", 8) == 0)
            ev->devpath = s + 8;

        i += strlen(s) + 1;
    }
}

int is_power_supply_event(const uevent_t *ev)
{
    if (!ev->subsystem || !ev->action)
        return 0;

    if (strcmp(ev->subsystem, "power_supply") != 0)
        return 0;

    if (strcmp(ev->action, "change") != 0)
        return 0;

    return 1;
}

int open_uevent_socket(void)
{
    struct sockaddr_nl addr;

    int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);

    if (sock < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = -1;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(sock);
        exit(EXIT_FAILURE);
    }

    return sock;
}

const char *ac_icon(int online)
{
    return online == 1 ? "ac-adapter" : "battery";
}

#ifndef UNIT_TEST
void send_notification(const char *summary, const char *body, const char *icon)
{
    NotifyNotification *n = notify_notification_new(summary, body, icon);
    notify_notification_show(n, NULL);
    g_object_unref(G_OBJECT(n));
}
#endif

void handle_netlink_event(int nl_sock, power_state_t *previous)
{
    char buffer[UEVENT_BUFFER_SIZE];

    int len = recv(nl_sock, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (len <= 0)
        return;

    uevent_t ev;
    parse_uevent(buffer, len, &ev);

    if (!is_power_supply_event(&ev))
        return;

    power_state_t current;
    read_state(&current);

    int changed = state_changed(previous, &current);
    *previous = current;

    if (changed & CHANGE_ONLINE)
        send_notification("Power Adapter", current.online == 1 ? "Charging" : "Disconnected", ac_icon(current.online));
}

int open_timer_fd(void)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd < 0)
    {
        perror("timerfd_create");
        exit(EXIT_FAILURE);
    }

    struct itimerspec ts = {
        .it_interval = { .tv_sec = BATTERY_POLL_INTERVAL, .tv_nsec = 0 },
        .it_value    = { .tv_sec = BATTERY_POLL_INTERVAL, .tv_nsec = 0 },
    };

    if (timerfd_settime(fd, 0, &ts, NULL) < 0)
    {
        perror("timerfd_settime");
        exit(EXIT_FAILURE);
    }

    return fd;
}

void handle_timer_event(int timer_fd, power_state_t *previous)
{
    uint64_t expirations;
    read(timer_fd, &expirations, sizeof(expirations));

    power_state_t current;
    read_state(&current);

    if (current.capacity == 100 && previous->capacity != 100)
        send_notification("Battery full", "Fully Charged", "battery-full");

    previous->capacity = current.capacity;
}

#ifndef UNIT_TEST
int main(void)
{
    printf("Started\n");
    printf("Querying power devices...\n");
    detect_power_devices();

    notify_init("bat_notify");

    power_state_t previous;
    read_state(&previous);

    printf("\nInitial state: %d%% | AC: %d\n", previous.capacity, previous.online);

    int nl_sock = open_uevent_socket();
    int timer_fd = open_timer_fd();

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sig_fd = signalfd(-1, &mask, 0);
    if (sig_fd < 0)
    {
        perror("signalfd");
        exit(EXIT_FAILURE);
    }

    int ep = epoll_create1(0);
    if (ep < 0)
    {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = nl_sock };

    if (epoll_ctl(ep, EPOLL_CTL_ADD, nl_sock, &ev) < 0)
    {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }

    ev.events = EPOLLIN, ev.data.fd = timer_fd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, timer_fd, &ev) < 0)
    {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }

    ev.events = EPOLLIN, ev.data.fd = sig_fd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, sig_fd, &ev) < 0)
    {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }

    struct epoll_event events[8];

    int running = 1;
    while (running)
    {
        int n = epoll_wait(ep, events, 8, -1);
        if (n < 0)
        {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++)
        {
            if (events[i].data.fd == nl_sock)
                handle_netlink_event(nl_sock, &previous);
            else if (events[i].data.fd == timer_fd)
                handle_timer_event(timer_fd, &previous);
            else if (events[i].data.fd == sig_fd)
                running = 0;
        }
    }

    close(ep);
    close(sig_fd);
    close(timer_fd);
    close(nl_sock);
    notify_uninit();

    printf("Closing\n");

    return 0;
}
#endif
