# clip-sync

Bidirectional Wayland-Android clipboard synchronization daemon.

`clip-sync` synchronizes clipboard content seamlessly between a Wayland desktop (using `wl-clipboard`) and an Android device via wireless ADB / `scrcpy-server`.

Features:
- Bidirectional real-time clipboard sync
- Automatic reconnection with exponential backoff
- Lightweight C99 daemon with minimal CPU and memory footprint
- Direct socket communication with `scrcpy-server`

## Requirements

- `wl-clipboard` (`wl-copy`, `wl-paste`)
- `android-tools` (`adb`)
- `scrcpy` (for `scrcpy-server.jar`)
- C99 compiler (`cc`)
- GNU Make

## Installation

```sh
make
sudo make install
```

## Configuration

Settings (device IP, ports, timeouts) are configured in `config.h`:

```sh
cp config.def.h config.h
# edit config.h (set your device IP and port)
make
```

## License

MIT License.
