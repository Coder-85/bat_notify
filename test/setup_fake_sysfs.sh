#!/bin/sh
# Creates or tears down a fake /sys/class/power_supply tree under /tmp/test_ps.
# Usage: setup_fake_sysfs.sh setup | teardown

FAKE_ROOT="/tmp/test_ps"

setup() {
    rm -rf "$FAKE_ROOT"

    # Battery
    mkdir -p "$FAKE_ROOT/BAT0"
    echo "Battery" > "$FAKE_ROOT/BAT0/type"
    echo "85"      > "$FAKE_ROOT/BAT0/capacity"

    # Mains adapter (initially offline)
    mkdir -p "$FAKE_ROOT/AC0"
    echo "Mains" > "$FAKE_ROOT/AC0/type"
    echo "0"     > "$FAKE_ROOT/AC0/online"

    # USB adapter (initially online)
    mkdir -p "$FAKE_ROOT/AC1"
    echo "USB" > "$FAKE_ROOT/AC1/type"
    echo "1"   > "$FAKE_ROOT/AC1/online"
}

teardown() {
    rm -rf "$FAKE_ROOT"
}

case "$1" in
    setup)    setup ;;
    teardown) teardown ;;
    *) echo "Usage: $0 setup|teardown"; exit 1 ;;
esac
