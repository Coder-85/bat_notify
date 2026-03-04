CC = gcc
CFLAGS = -Wall -Wextra $(shell pkg-config --cflags libnotify)
LIBS = $(shell pkg-config --libs libnotify)
PREFIX = /usr/local
SYSTEMD_DIR = /usr/lib/systemd/user

.PHONY: all test install uninstall clean

all: bat_notify

bat_notify: main.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

test: test/test_bin
	chmod +x test/setup_fake_sysfs.sh
	test/setup_fake_sysfs.sh setup
	test/test_bin; ret=$$?; test/setup_fake_sysfs.sh teardown; exit $$ret

test/test_bin: test/test.c main.c
	$(CC) $(CFLAGS) -DUNIT_TEST -DSYSFS_PATH=\"/tmp/test_ps\" -o $@ test/test.c $(LIBS)

install: bat_notify
	install -Dm755 bat_notify $(DESTDIR)$(PREFIX)/bin/bat_notify
	install -Dm644 bat_notify.service $(DESTDIR)$(SYSTEMD_DIR)/bat_notify.service
	@echo "Run 'systemctl --user daemon-reload' then 'systemctl --user enable --now bat_notify' to start the service."

uninstall:
	systemctl --user disable --now bat_notify || true
	rm -f $(DESTDIR)$(PREFIX)/bin/bat_notify
	rm -f $(DESTDIR)$(SYSTEMD_DIR)/bat_notify.service
	systemctl --user daemon-reload || true

clean:
	rm -f bat_notify test/test_bin
