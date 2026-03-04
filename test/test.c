#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>

int  notif_count = 0;
char last_summary[64] = {0};
char last_body[64]    = {0};

// Must be defined before #include "../main.c" so callers inside main.c see the declaration when the file is compiled as a unit
void send_notification(const char *summary, const char *body, const char *icon)
{
    notif_count++;
    strncpy(last_summary, summary, sizeof(last_summary) - 1);
    strncpy(last_body,    body,    sizeof(last_body)    - 1);
}

#include "../main.c"

void reset_notifications(void)
{
    notif_count = 0;
    last_summary[0] = '\0';
    last_body[0] = '\0';
}

int tests_run = 0;
int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __func__, __LINE__, msg); \
        tests_failed++; \
    } else { \
        printf("PASS  %s\n", msg); \
    } \
} while (0)

void write_uevent(int fd, const char *action, const char *subsystem, const char *devpath)
{
    char buf[512];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "ACTION=%s", action) + 1;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "SUBSYSTEM=%s", subsystem) + 1;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "DEVPATH=%s", devpath) + 1;
    send(fd, buf, pos, 0);
}

void fake_write(const char *path, const char *val)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(val, f); fclose(f); }
}

void reset_devices(void)
{
    battery_path[0] = '\0';
    ac_path[0] = '\0';
    ac_count = 0;
}

void test_parse_uevent(void)
{
    char buf[256];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "ACTION=change") + 1;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "SUBSYSTEM=power_supply") + 1;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "DEVPATH=/devices/BAT0")  + 1;

    uevent_t ev;
    parse_uevent(buf, pos, &ev);

    ASSERT(ev.action && strcmp(ev.action, "change") == 0, "parse_uevent: ACTION");
    ASSERT(ev.subsystem && strcmp(ev.subsystem, "power_supply") == 0, "parse_uevent: SUBSYSTEM");
    ASSERT(ev.devpath && strcmp(ev.devpath,   "/devices/BAT0") == 0, "parse_uevent: DEVPATH");
}

void test_is_power_supply_event(void)
{
    uevent_t good = { "change", "power_supply", "/devices/BAT0" };
    uevent_t wrong_subsystem = { "change", "input", "/devices/input0" };
    uevent_t wrong_action = { "add", "power_supply", "/devices/BAT0" };
    uevent_t null_fields = { NULL, NULL, NULL };

    ASSERT(is_power_supply_event(&good) == 1, "is_power_supply_event: valid event");
    ASSERT(is_power_supply_event(&wrong_subsystem) == 0, "is_power_supply_event: wrong subsystem");
    ASSERT(is_power_supply_event(&wrong_action) == 0, "is_power_supply_event: wrong action");
    ASSERT(is_power_supply_event(&null_fields) == 0, "is_power_supply_event: null fields");
}

void test_state_changed(void)
{
    power_state_t a = { .capacity = 80, .online = 0 };
    power_state_t b = { .capacity = 80, .online = 0 };

    ASSERT(state_changed(&a, &b) == CHANGE_NONE, "state_changed: identical -> CHANGE_NONE");

    b.online = 1;
    ASSERT(state_changed(&a, &b) == CHANGE_ONLINE, "state_changed: online diff -> CHANGE_ONLINE");

    b.online = 0;
    b.capacity = 95;
    ASSERT(state_changed(&a, &b) == CHANGE_NONE, "state_changed: capacity only -> CHANGE_NONE");
}

void test_detect_power_devices(void)
{
    reset_devices();
    detect_power_devices();

    ASSERT(strstr(battery_path, "BAT0") != NULL, "detect_power_devices: battery_path -> BAT0");
    ASSERT(strstr(ac_path, "AC1") != NULL, "detect_power_devices: ac_path -> online AC1");
}

void test_update_ac_path_prefers_online(void)
{
    reset_devices();
    detect_power_devices();

    fake_write("/tmp/test_ps/AC1/online", "0");
    fake_write("/tmp/test_ps/AC0/online", "1");
    update_ac_path();
    ASSERT(strstr(ac_path, "AC0") != NULL, "update_ac_path: switches to newly online AC0");

    fake_write("/tmp/test_ps/AC0/online", "0");
    update_ac_path();
    ASSERT(strstr(ac_path, "AC0") != NULL, "update_ac_path: fallback to Mains when all offline");

    fake_write("/tmp/test_ps/AC1/online", "1");
}

void test_read_state(void)
{
    reset_devices();
    detect_power_devices();

    fake_write("/tmp/test_ps/BAT0/capacity", "72");
    fake_write("/tmp/test_ps/AC1/online", "1");

    power_state_t s;
    read_state(&s);

    ASSERT(s.capacity == 72, "read_state: capacity");
    ASSERT(s.online == 1, "read_state: online");
}

void test_uevent_plug_in(void)
{
    reset_devices();
    detect_power_devices();

    fake_write("/tmp/test_ps/AC1/online", "0");
    power_state_t prev;
    read_state(&prev);

    fake_write("/tmp/test_ps/AC1/online", "1");

    int sv[2];
    socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
    write_uevent(sv[1], "change", "power_supply", "/devices/AC1");

    reset_notifications();
    handle_netlink_event(sv[0], &prev);

    ASSERT(notif_count == 1, "uevent plug-in: one notification");
    ASSERT(strcmp(last_summary, "Power Adapter") == 0, "uevent plug-in: correct summary");
    ASSERT(strcmp(last_body, "Charging") == 0, "uevent plug-in: correct body");

    close(sv[0]); close(sv[1]);
}

void test_uevent_unplug(void)
{
    reset_devices();
    detect_power_devices();

    fake_write("/tmp/test_ps/AC1/online", "1");
    power_state_t prev;
    read_state(&prev);

    fake_write("/tmp/test_ps/AC1/online", "0");

    int sv[2];
    socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
    write_uevent(sv[1], "change", "power_supply", "/devices/AC1");

    reset_notifications();
    handle_netlink_event(sv[0], &prev);

    ASSERT(notif_count == 1,                              "uevent unplug: one notification");
    ASSERT(strcmp(last_body, "Disconnected") == 0,        "uevent unplug: correct body");

    close(sv[0]); close(sv[1]);
    fake_write("/tmp/test_ps/AC1/online", "1");
}

void test_uevent_irrelevant(void)
{
    reset_devices();
    detect_power_devices();

    power_state_t prev;
    read_state(&prev);

    int sv[2];
    socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
    write_uevent(sv[1], "add", "input", "/devices/input0");

    reset_notifications();
    handle_netlink_event(sv[0], &prev);

    ASSERT(notif_count == 0, "uevent irrelevant: no notification sent");

    close(sv[0]); close(sv[1]);
}

int make_fired_timerfd(void)
{
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);

    struct itimerspec ts = {
        .it_value    = { .tv_sec = 0, .tv_nsec = 1 },
        .it_interval = { .tv_sec = 0, .tv_nsec = 0 },
    };

    timerfd_settime(tfd, 0, &ts, NULL);
    struct timespec sleep_ts = { .tv_sec = 0, .tv_nsec = 5000000 }; /* 5ms */
    nanosleep(&sleep_ts, NULL);
    return tfd;
}

void test_timer_battery_full(void)
{
    reset_devices();
    detect_power_devices();
    fake_write("/tmp/test_ps/BAT0/capacity", "100");

    power_state_t prev = { .capacity = 85, .online = 1 };
    int tfd = make_fired_timerfd();

    reset_notifications();
    handle_timer_event(tfd, &prev);

    ASSERT(notif_count == 1, "timer: battery full notification sent");
    ASSERT(strcmp(last_summary, "Battery full") == 0, "timer: correct summary");

    close(tfd);
}

void test_timer_no_duplicate_full(void)
{
    reset_devices();
    detect_power_devices();
    fake_write("/tmp/test_ps/BAT0/capacity", "100");

    power_state_t prev = { .capacity = 100, .online = 1 };

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec ts = {
        .it_value    = { .tv_sec = 0, .tv_nsec = 1 },
        .it_interval = { .tv_sec = 0, .tv_nsec = 1 },
    };

    timerfd_settime(tfd, 0, &ts, NULL);
    struct timespec sleep_ts = { .tv_sec = 0, .tv_nsec = 5000000 };
    nanosleep(&sleep_ts, NULL);

    reset_notifications();
    handle_timer_event(tfd, &prev);

    ASSERT(notif_count == 0, "timer: no duplicate notification when already at 100%");

    close(tfd);
}

int main(void)
{
    printf("\n=== Layer 1: pure logic ===\n");
    test_parse_uevent();
    test_is_power_supply_event();
    test_state_changed();

    printf("\n=== Layer 2: fake sysfs ===\n");
    test_detect_power_devices();
    test_update_ac_path_prefers_online();
    test_read_state();

    printf("\n=== Layer 3: uevent injection ===\n");
    test_uevent_plug_in();
    test_uevent_unplug();
    test_uevent_irrelevant();

    printf("\n=== Layer 3: timer / battery full ===\n");
    test_timer_battery_full();
    test_timer_no_duplicate_full();

    printf("\n%d/%d tests passed.\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
