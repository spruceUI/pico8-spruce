# pcmreopen

A second, separate library preloaded into native PICO-8 on the Miyoo A30. It fixes sound that never comes back after sleep (spruceOS #546).

The A30's codec loses its setup on suspend. Sometimes the kernel suspends the open PCM and sometimes it leaves it running, but either way the stream plays silence until the device is closed and opened again. RetroArch's audio recovers through a reopen; PICO-8's SDL2 never reopens, so it stays silent.

`pcmreopen.c` wraps `ioctl`, `mmap`, `munmap` and `close`. It records the playback PCM's hw/sw params and buffer mapping, and spots a suspend from `CLOCK_BOOTTIME` running ahead of `CLOCK_MONOTONIC`. On the first PCM ioctl after one, it reopens the device on the same fd, replays the params and mapping at the same address, then plays a little silence and lets the stream run dry, so alsa-lib sees a real underrun and SDL's recovery prepares and restarts it. Everything else passes straight through.

It only works on a stream it can reopen: spruce points PICO-8 at a softvol over `plughw:sndcodec,0` rather than the shared dmix, and turns the codec's `DACL/DACR Mixer AIF1DA0L/R Switch` back on at wake, which suspend also clears.

Raw ARM EABI syscalls only, so it builds without a sysroot and needs nothing from libc but `__errno_location` and `memset`:

    clang --target=armv7a-linux-gnueabihf -mfloat-abi=hard -march=armv7-a -marm -fPIC -shared \
        -nostdlib -ffreestanding -fno-stack-protector -fuse-ld=lld -O2 -Wall \
        -Wl,-soname,libpcmreopen.so -o libpcmreopen.so pcmreopen.c
