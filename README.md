# FRANK Lab

Official page: **[frank.rh1.tech](https://frank.rh1.tech/)** — hub for all FRANK boards and firmware.

Experimental and work-in-progress boards for the [FRANK](https://github.com/rh1tech/frank) hardware emulation platform.

**WARNING: These boards are untested and unreleased. Do NOT order PCBs from this repository.** They may contain errors, incomplete routing, or unverified component footprints. Use at your own risk.

For production-ready boards, see the main [frank](https://github.com/rh1tech/frank) repository.

## Boards in this repository

| Board | Status |
|-------|--------|
| `frank_air` | Work in progress |
| `frank_core2` | Work in progress — pulled back from the released set |
| `frank_next_proto` | Prototype — superseded by FRANK Next, released |

`frank_core2u/firmware/` and `frank_core2_proto/firmware/` hold the dual-core firmware sources
for those two boards. Both boards themselves have been released; only the firmware still
lives here.

## Released from this repository

These boards have graduated to [rh1tech/frank](https://github.com/rh1tech/frank) and are no
longer maintained here:

| Was | Now |
|-----|-----|
| `frank_next` | [`hardware/frank_next`](https://github.com/rh1tech/frank/tree/master/hardware/frank_next) — the flagship |
| `frank_core2_proto` | [`hardware/frank_core2_proto`](https://github.com/rh1tech/frank/tree/master/hardware/frank_core2_proto) — published as a prototype |
| `frank_core2u` | [`hardware/frank_core2u`](https://github.com/rh1tech/frank/tree/master/hardware/frank_core2u) |
| `oldskoolfrank` | [`hardware/oldskool`](https://github.com/rh1tech/frank/tree/master/hardware/oldskool) |
| `dino` | [`hardware/dino_z80`](https://github.com/rh1tech/frank/tree/master/hardware/dino_z80) |
| `xt8086_beta` | [`hardware/xt8086_beta`](https://github.com/rh1tech/frank/tree/master/hardware/xt8086_beta) |

Each carries its KiCad project, gerbers, fabrication docs, 3D-printable case and an assembly
guide in English and Russian. Board pages with full specifications are on
[frank.rh1.tech](https://frank.rh1.tech/hardware).

## Author

Mikhail Matveev — <xtreme@rh1.tech> — [rh1.tech](https://rh1.tech)

## License

© 2026 Mikhail Matveev, <xtreme@rh1.tech>

GPL v3. See [LICENSE](./LICENSE).
