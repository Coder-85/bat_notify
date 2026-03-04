# bat_notify

A lightweight battery notification daemon for Linux. It sends desktop notifications through when:

- The power adapter is **connected** or **disconnected**
- The battery reaches **100% charge**

---

## Dependencies
```
libnotify pkg-config
```
A running notification daemon is also required for displaying notifications.

---

## Building

```sh
make
```

The binary is output as `./bat_notify`.

### Running tests

```sh
make test
```

---

## Installing

`make install` copies the binary to `/usr/local/bin` and the systemd user unit to `/usr/lib/systemd/user`. Run it as root:

```sh
sudo make install
```

To install to a different prefix:

```sh
sudo make install PREFIX=/usr
```

`DESTDIR` is supported for building packages:

```sh
make install DESTDIR=/tmp/pkg
```

After installing, each user enables the service once:

```sh
systemctl --user daemon-reload
systemctl --user enable --now bat_notify
```

## Uninstalling

```sh
sudo make uninstall
```

This stops and disables the service, removes the binary and unit file, and reloads systemd.
