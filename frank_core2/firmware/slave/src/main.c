/*
 * FRANK Core 2 — dual-RP2350 test firmware
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * main.c — slave (RP2350, U6) link responder and self-test.
 *
 * The slave has no display of its own: its job is to answer the master
 * over the parallel link, and to run the same flash/PSRAM battery the
 * master runs locally so both halves of the board appear side by side
 * on the master's screen.
 *
 * It runs its self-test once at boot, before the master is likely to
 * have finished its own, and then serves requests forever. The results
 * are cached rather than re-measured per request so a LINK_OP_SELFTEST
 * answers immediately instead of blocking the master for a second.
 *
 * The blue LED (LD2, GPIO26) beats from a timer IRQ:
 *   slow  — booted, waiting for the master
 *   fast  — serving a request
 *   rapid — self-test failed
 */

#include "frank_core2_board.h"
#include "heartbeat.h"
#include "link_bus.h"
#include "link_proto.h"
#include "link_session.h"
#include "mem_test.h"

#include "hardware/clocks.h"
#include "hardware/structs/sysinfo.h"
#include "hardware/vreg.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "1.0"
#endif

/* pio0 for the link, matching the master. The slave has no video or
 * audio, so nothing competes for it. */
#define LINK_PIO pio0

static link_t         g_link;
static link_session_t g_session;

static uint8_t g_ctrl_tx[LINK_CTRL_BYTES] __attribute__((aligned(4)));
static uint8_t g_ctrl_rx[LINK_CTRL_BYTES] __attribute__((aligned(4)));
static uint8_t g_bulk_tx[LINK_BULK_BYTES] LINK_BULK_ALIGN;
static uint8_t g_bulk_rx[LINK_BULK_BYTES] LINK_BULK_ALIGN;

static link_node_info_t  g_info;
static link_mem_result_t g_mem;

static void collect_identity(void) {
    uint32_t jedec = 0;
    uint8_t  uid[8] = { 0 };

    mem_test_flash_identify(&jedec, uid);

    memcpy(g_info.chip_id, uid, 8);
    g_info.flash_jedec_id = jedec;
    g_info.flash_bytes    = mem_test_flash_capacity(jedec);
    g_info.sys_clk_hz     = clock_get_hz(clk_sys);
    g_info.fw_version     = 0x0100;

    uint32_t pkg = *((io_ro_32 *)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    g_info.package_is_a = (pkg & 1u) ? 1 : 0;
    g_info.rp2350_rev   = (uint8_t)((*((io_ro_32 *)(SYSINFO_BASE +
                              SYSINFO_CHIP_ID_OFFSET)) >> 28) & 0xFu);
}

int main(void) {
    vreg_set_voltage(CPU_VOLTAGE);
    sleep_ms(10);

    /* Not "required" — fall back rather than panic into a silent hang. */
    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false))
        set_sys_clock_khz(125 * 1000, false);

    stdio_init_all();

    /* USB CDC enumeration takes far longer than a couple of hundred
     * milliseconds; without this wait the whole boot log is lost. */
#if !defined(USB_HID_ENABLED)
    for (int i = 0; i < 60 && !stdio_usb_connected(); i++) sleep_ms(50);
#endif
    sleep_ms(100);

    /* Attach window — same reasoning as the master: the slave's whole
     * self-test log is emitted in the first second, and a console that
     * attaches after that sees an idle board with nothing to say. */
    for (int i = 0; i < 8; i++) {
        printf("FRANK Core 2 slave - waiting for console (%d/8)\n", i + 1);
        sleep_ms(250);
    }

    printf("\nFRANK Core 2 slave firmware v%s\n", FIRMWARE_VERSION);
    printf("sys_clk %u MHz\n", (unsigned)(clock_get_hz(clk_sys) / 1000000u));

    heartbeat_init_gpio(S_LED_PIN);
    heartbeat_set(HB_BOOT);

    collect_identity();
    g_info.psram_bytes = mem_test_psram_probe(S_PSRAM_CS_PIN);

    printf("slave: flash %06X (%u MB), psram %u MB, %u MHz\n",
           (unsigned)g_info.flash_jedec_id,
           (unsigned)(g_info.flash_bytes / (1024u * 1024u)),
           (unsigned)(g_info.psram_bytes / (1024u * 1024u)),
           (unsigned)(g_info.sys_clk_hz / 1000000u));

    heartbeat_set(HB_TEST);
    mem_test_run_all(&g_mem, g_info.flash_bytes, g_info.psram_bytes);

    printf("slave: flash %s (%u KiB/s), psram %s (%u KiB/s w, %u KiB/s r, %u errors)\n",
           g_mem.flash_ok ? "ok" : "FAIL", (unsigned)g_mem.flash_read_kbps,
           g_mem.psram_ok ? "ok" : "FAIL",
           (unsigned)g_mem.psram_write_kbps, (unsigned)g_mem.psram_read_kbps,
           (unsigned)g_mem.psram_bit_errors);

    /* Link up. The slave transmits on bus B and receives on bus A —
     * the mirror image of the master's assignment. */
    link_init(&g_link, LINK_PIO,
              S_LINK_B_DATA_BASE,   /* we transmit on bus B */
              S_LINK_A_DATA_BASE,   /* we receive on bus A  */
              S_LINK_DB_OUT, S_LINK_DB_IN,
              S_LINK_FS, false /* master drives FS */);

    /* Same seed as the master, so each side can verify what it receives
     * against a locally generated copy. */
    link_bulk_pattern(g_bulk_tx, LINK_BULK_BYTES, 0x12345678u);

    g_session.link    = &g_link;
    g_session.ctrl_tx = g_ctrl_tx;
    g_session.ctrl_rx = g_ctrl_rx;
    g_session.bulk_tx = g_bulk_tx;
    g_session.bulk_rx = g_bulk_rx;
    g_session.seq     = 0;

    bool healthy = g_mem.flash_ok && g_mem.psram_ok;
    heartbeat_set(healthy ? HB_WAITING : HB_ERROR);
    printf("slave: link ready, waiting for master\n");

    uint32_t served = 0;
    while (true) {
        /* One-second wait so an idle slave drops back to the waiting
         * beat instead of looking busy forever. */
        uint16_t op = link_s_serve(&g_session, 1000000u, &g_info, &g_mem);

        if (op) {
            served++;
            heartbeat_set(HB_TEST);
            printf("slave: served op 0x%04X (#%u)\n", op, (unsigned)served);
        } else {
            heartbeat_set(healthy ? HB_WAITING : HB_ERROR);
        }
    }
}
