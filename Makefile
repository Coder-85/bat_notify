CC = gcc
CFLAGS = -Wall -Wextra $(shell pkg-config --cflags libnotify)
LIBS = $(shell pkg-config --libs libnotify)

.PHONY: all test clean

all: bat_notify

bat_notify: main.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

test: test/test_bin
	chmod +x test/setup_fake_sysfs.sh
	test/setup_fake_sysfs.sh setup
	test/test_bin; ret=$$?; test/setup_fake_sysfs.sh teardown; exit $$ret

test/test_bin: test/test.c main.c
	$(CC) $(CFLAGS) -DUNIT_TEST -DSYSFS_PATH=\"/tmp/test_ps\" -o $@ test/test.c $(LIBS)

clean:
	rm -f bat_notify test/test_bin
