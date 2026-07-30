# FRANK Core 2 — dual-RP2350 test firmware

Bring-up and link-characterisation firmware for the FRANK Core 2 board:
an RP2350B master (U3, QFN-80) and an RP2350 slave (U6, QFN-60) joined by
two 8-bit source-synchronous parallel buses.

The master brings up HDMI, prints a full diagnostic report on screen,
runs its own flash and PSRAM tests, then interrogates the slave over the
link and measures how fast the two halves can actually talk.

```
firmware/
├── boards/          SDK board headers (package + default pin selection)
├── common/          shared by both MCUs: board map, link, protocol, memory tests
├── drivers/         reused verbatim from frank-msx (HDMI, SD, audio, PSRAM, USB HID)
├── master/          RP2350B firmware + build/flash scripts
├── slave/           RP2350A firmware + build/flash scripts
├── probe/           minimal firmware for bisecting a silent bring-up
├── build_all.sh     build both halves with matching options
├── swd_flash.sh     flash over SWD with a Debug Probe (preferred)
└── flash_all.sh     guided two-step USB BOOTSEL flash
```

## Quick start

```bash
cd frank_core2/firmware
./build_all.sh          # both halves, 252 MHz, USB CDC console

./swd_flash.sh master   # with a Debug Probe on J1 (preferred)
./swd_flash.sh slave    #                  ... or J3

./flash_all.sh          # or USB BOOTSEL: prompts through slave then master
```

**Prefer SWD if you have a Debug Probe.** USB-BOOTSEL flashing needs a
button press whenever the firmware wedges — exactly when you are
iterating fastest — and `picotool reboot -u` will not recover a target
that has faulted into lockup. SWD does not care what the target is
doing, and it gives you `pc` when something stops.

Then reset the slave (S4) first, then the master (S2). The master reports
the link as down if the slave is not already serving when the sweep runs.

## Build options

Both scripts read the same environment variables, and `build_all.sh`
passes them to both halves so the two firmwares stay matched:

| Variable | Default | Meaning |
|---|---|---|
| `USB_HID` | `0` | `0` = USB CDC serial console. `1` = USB HID host keyboard on the master, console moves to UART |
| `CPU_SPEED` | `252` | System clock in MHz. Sets the link's ceiling: one byte per 4 clocks |
| `PSRAM_SPEED` | `133` | PSRAM clock ceiling in MHz |
| `FLASH_SPEED` | `66` | Flash clock ceiling in MHz |
| `CLEAN` | `0` | `1` wipes the build directory first |

```bash
USB_HID=1 ./build_all.sh              # HID keyboard, UART console
CPU_SPEED=300 CLEAN=1 ./build_all.sh  # push the link to 75 MB/s per direction
```

**Build both halves at the same `CPU_SPEED`.** The receiving PIO state
machine has to complete its three-instruction loop inside the
transmitter's byte period, and each side derives that from its own
system clock. Mismatched clocks give you a link that works in one
direction and drops bytes in the other. `build_all.sh` exists to make
the matched build the path of least resistance.

## Consoles

| `USB_HID` | Master console | Slave console |
|---|---|---|
| `0` (default) | USB CDC on J8 | USB CDC on J9 |
| `1` | UART0 on J2 (GPIO0 TX / GPIO1 RX, 115200) | UART1 on J4 (GPIO24 TX / GPIO25 RX, 115200) |

The master mirrors everything it draws on screen to its console, so a
serial capture is a complete record of the run. Pressing any key on
either the USB HID keyboard or the serial console re-runs the whole
diagnostic.

## The link

Two independent buses, each 8 data lines plus a clock and a VALID
strobe, plus three single-wire control signals. Every pin below comes
from the KiCad netlist.

| Signal | Master | Slave | Notes |
|---|---|---|---|
| Bus A data D0..D7 | GPIO20..27 | GPIO1..8 | master → slave |
| Bus A clock | GPIO28 | GPIO9 | 33R series (R1) |
| Bus A valid | GPIO29 | GPIO10 | |
| Bus B data D0..D7 | GPIO30..37 | GPIO11..18 | slave → master |
| Bus B clock | GPIO38 | GPIO19 | 33R series (R2) |
| Bus B valid | GPIO39 | GPIO20 | |
| FS (frame sync) | GPIO40 out | GPIO21 in | reserved for future phase signalling |
| DB_MS (doorbell) | GPIO41 out | GPIO22 in | "master ready" |
| DB_SM (doorbell) | GPIO42 in | GPIO23 out | "slave ready / done" |

Both buses share one relative layout — clock at `data_base + 8`, valid at
`data_base + 9` — which is what lets a single pair of PIO programs serve
either direction on either chip.

### Wire protocol

Transmit is two PIO instructions, four system clocks per byte:

```
cycle:   0     1     2     3     0     1
DATA:  <--- byte N --------><--- byte N+1 ...
CLK:   ____________/‾‾‾‾‾‾‾‾‾‾‾‾\________/‾‾
                   ^ receiver samples here
```

Data changes on the falling edge and is sampled on the rising edge, so
the receiver gets two full clocks of setup and two of hold. Receive is a
three-instruction loop against that four-cycle period, leaving one cycle
of slack and re-synchronising to the incoming clock on every byte.

At 252 MHz that is **63.0 MB/s per direction, 126 MB/s aggregate** with
both buses running at once.

Four cycles per byte is the floor for this receiver, not an arbitrary
choice: the loop needs three cycles, and three-against-three would leave
no margin at all. Going faster would need a fundamentally different
receiver — two state machines ping-ponging on alternate edges, for
instance — which is a bigger change than a bring-up firmware warrants.

Frame boundaries come from the arm-before-send ordering rather than from
the wire: the receiver restarts its state machine (resetting the input
shift counter) before the sender starts, which is what keeps 32-bit
autopush words aligned with the sender's autopull words. Byte boundaries
are inherent — one clock edge is exactly one byte.

### Handshake

`DB_MS` means "master is ready for the next step", `DB_SM` means "slave
is ready or done". Every phase raises both, transfers, then drops both.
Nothing depends on the two chips agreeing about absolute time, so the
slave can boot seconds after the master and still join cleanly.

## What the master reports

```
FRANK Core 2  -  dual RP2350 bring-up      v1.0
-----------------------------------------------------
              MASTER 2350B      SLAVE 2350A
Chip ID       E661...           E661...
Package/rev   QFN-80 B / rev 2  QFN-60 A / rev 2
Sys clock     252 MHz           252 MHz
Flash ID      EF4018 16 MB      EF4018 16 MB
Flash read    59.8 MB/s         59.6 MB/s
Flash CRC32   1A2B3C4D          9F8E7D6C
PSRAM         8 MB ok           8 MB ok
PSRAM write   41.2 MB/s         41.0 MB/s
PSRAM read    54.7 MB/s         54.5 MB/s
PSRAM errors  0                 0
-----------------------------------------------------
 SD:29.7 GB   USB:cdc   I2S:ok   HDMI:640x480@60
-----------------------------------------------------
 rate     M->S        S->M        duplex      err
 1.00x    63.0 MB/s   63.0 MB/s   125.9 MB/s  0
 1.25x    50.4 MB/s   50.4 MB/s   100.7 MB/s  0
 1.50x    42.0 MB/s   42.0 MB/s   83.9 MB/s   0
 2.00x    31.5 MB/s   31.5 MB/s   62.9 MB/s   0
 round-trip 12.40 us    peak duplex 125.9 MB/s
 LINK OK - error free to 125.9 MB/s aggregate
```

### Throughput and integrity are measured separately

- **Throughput** is one uninterrupted ring DMA — no per-block handshake,
  no CPU verification. The number is the wire and nothing else.
- **Integrity** is block-at-a-time with a handshake between each, every
  byte compared against the expected LFSR pattern.

Mixing them would either understate the speed (verification in the hot
path) or overstate the confidence (unverified bytes). The `err` column
comes from the integrity pass; a row is only marked OK when it is zero.

Each side generates the same pattern from the same seed, so neither has
to send reference data across the link it is trying to test.

## Peripheral tests

Run on **both** MCUs — the slave ships its results back over the link
and the master renders them beside its own.

- **Flash** — JEDEC ID and capacity, sequential XIP read throughput, and
  a CRC-32 over the first 64 KiB computed twice. A CRC mismatch between
  the two passes means marginal QSPI timing, which would make every
  other figure on the screen suspect.
- **PSRAM** — presence and size by address-aliasing probe, then a full
  write-and-verify sweep of all 8 MB with timing for both passes.

The probes read through the *uncached* XIP alias (`0x15000000`); through
the cached window a write followed by a read of the same address is
answered from the 8 KiB XIP cache, so a missing chip would look present
and an aliasing address would look like it isn't. Throughput uses the
cached window, because that is how real code reaches these devices.

## Heartbeats

Both LEDs are driven from a timer IRQ, not the main loop, so they keep
beating while the foreground is parked in a link handshake or a
multi-second PSRAM sweep. If an LED stops, that core is wedged.

**Master** — WS2812B (LD1, GPIO46), colour carries the state:

| Colour | Meaning |
|---|---|
| Blue breathe | Booting, peripherals not probed yet |
| Amber breathe | Self-test or link test running |
| Violet breathe | Idle, waiting for the peer |
| Green breathe | Everything passed |
| Red blink | Something failed — check the console |

**Slave** — blue LED (LD2, GPIO26), rate carries the state: 1 s slow beat
when waiting, 200 ms when serving, 80 ms frantic blink on self-test
failure.

## Hardware notes

### The master cannot reset the slave

The schematic labels master GPIO43 as `RUNA/SR` — slave reset — but the
net only reaches a 10K pull-up to +3V3 (R3). It does **not** connect to
the slave's RUN pin (U6.26), which sees only the S4 reset button.

So the two MCUs must be reset and flashed independently, which is why
`flash_all.sh` is a guided prompt rather than a single command. The
firmware never assumes it can restart the peer; it detects an absent
slave by doorbell timeout and reports the link as down.

If a future board revision wires GPIO43 to U6.26, the firmware can drive
a slave reset before the handshake and `flash_all.sh` can be automated.

### RP2350B GPIO window

The master is the B package and drives link pins up to GPIO39 plus the
WS2812 on GPIO46. Two things follow, and both are easy to get wrong:

1. The stock `pico2` board definition declares `PICO_RP2350A 1`, which
   caps `NUM_BANK0_GPIOS` at 30. `boards/frank_core2_master.h` sets it
   to 0.
2. An RP2350B PIO instance can only address 32 consecutive GPIOs, either
   0–31 or 16–47, selected by `pio_set_gpio_base()`. The link spans
   GPIO20 to GPIO38, so it needs the upper window. Without it the SDK
   either rejects the configuration or — with parameter assertions off —
   quietly aliases GPIO32–38 onto GPIO0–6, giving a link that looks
   wired but reads garbage.

`link_init()` selects the window and then hard-asserts on the return
value of `pio_sm_init()`, so a misconfiguration fails loudly at boot
instead of producing plausible-looking wrong numbers.

### Resource allocation (master)

| Resource | Owner |
|---|---|
| PIO0 | Link TX + RX state machines |
| PIO1 (one SM) | I2S audio (TDA1387) |
| PIO2 | WS2812B heartbeat |
| DMA 0, 1 | HSTX video scanout |
| DMA_IRQ_0 | HSTX video scanout (exclusive handler) |
| DMA (claimed) | Link TX + RX |
| Core 1 | HSTX scanout, launched by `graphics_init()` |

### Why the audio test does not use the frank-msx I2S driver

`audio.c`'s `i2s_init()` installs an **exclusive** `DMA_IRQ_0` handler
for its double-buffered playback path. The HSTX video driver
(`pico_hdmi/video_output.c`) has already taken `DMA_IRQ_0`. The second
`irq_set_exclusive_handler()` hard-asserts, `panic()` executes a
breakpoint with no debugger attached, and the core escalates straight
into **lockup** — report half-drawn, USB dead, no message, and no
software route back into BOOTSEL.

That combination is specific to this firmware: frank-msx pairs the audio
driver with the *PIO* HDMI path, which uses `DMA_IRQ_1`, so the two
never collide there.

A tone test needs neither DMA nor an interrupt, so `main.c` drives the
I2S state machine directly from `audio_i2s.pio` and pushes samples into
the FIFO against a deadline. If you later want streaming audio here,
move the driver to `DMA_IRQ_1` rather than reintroducing the collision.

### Boot ordering

`mem_test_flash_identify()` takes the QMI out of XIP to issue a raw
`0x9F`. It runs before `graphics_init()` launches core 1, because core 1
fetching from flash during that window would hang. The PSRAM probe runs
in the same quiet period. Everything after that is plain loads and
stores through the XIP windows and is safe with both cores running.

## Debugging

Every stage prints to the console before it runs (`[boot] ...`), so a
stall is located by reading the last line rather than by bisecting the
binary. Both firmwares also repeat a banner for two seconds at startup:
USB CDC enumeration plus getting a terminal open reliably takes longer
than the diagnostic takes to run, and a report you missed is a report
you do not have.

With a probe attached, `pc` tells you the rest:

```bash
openocd -f interface/cmsis-dap.cfg -c "adapter speed 5000" \
        -f target/rp2350.cfg -c "init" -c "halt" -c "reg pc" -c "exit"
arm-none-eabi-addr2line -f -e master/build/frank-core2-master.elf <pc>
```

`pc == 0xeffffffe` means the core is in **lockup** — a fault escalated,
which is what `panic()` looks like from the outside once its breakpoint
executes with no debugger attached. Anything in flash resolves to a real
source line with `addr2line`.

## `probe/` — bring-up bisect target

A ~20-line firmware that does nothing but set the clock, bring up USB
stdio, and print a line per second. It exists to answer one question
when the master firmware comes up silent: *is this the diagnostic's
fault, or the board's?*

```bash
cd probe && cmake -S . -B build -DPICO_PLATFORM=rp2350 && cmake --build build -j8
picotool load -f build/frank-core2-probe.uf2 && picotool reboot -f
```

If the probe prints, the platform is sound and the fault is in the
diagnostic. If the probe is also silent, the problem is the clock, the
flash timing, or the board header — look there before reading a line of
diagnostic code.

## Reused from frank-msx

`drivers/` is copied verbatim from `frank-msx` so the two projects can
stay in sync:

- `pico_hdmi/` + `HDMI_hstx.c` — HSTX HDMI at 640x480, pins 12–19
- `sdcard/` + `fatfs/` — microSD on SPI0
- `audio.c` + `audio_i2s.pio` — I2S to the TDA1387
- `psram_init.c` — QMI setup for the ESP-PSRAM64H
- `usbhid/` — TinyUSB HID host

`common/board_config.h` is a shim that feeds the FRANK Core 2 pin map to
those drivers under the name they expect, so they can be re-synced from
frank-msx without patching.

The master's pinout for HDMI (12–19), microSD (4–7), I2S (9/10/11) and
PSRAM CS (47) matches Murmulator 2.0 exactly, which is why the drivers
drop in unmodified.
