/*
 * FRANK Core 2 — dual-RP2350 test firmware
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * main.c — master (RP2350B, U3) bring-up and diagnostics.
 *
 * Boot order matters and is not arbitrary:
 *
 *   1. clocks / vreg
 *   2. stdio                     — so a failure after this point is visible
 *   3. heartbeat                 — proves the core is alive before anything
 *                                  slow runs
 *   4. flash identify            — MUST be before step 6: it drops the QMI
 *                                  out of XIP, which would hang core 1 if
 *                                  core 1 were already fetching from flash
 *   5. PSRAM probe               — same reasoning, plus it wants a quiet bus
 *   6. graphics_init             — launches core 1 for HSTX scanout
 *   7. console + report skeleton — first thing the user sees
 *   8. memory throughput, SD, USB, audio
 *   9. link tests against the slave
 */

#include "console.h"
#include "diag_link.h"
#include "report.h"

#include "frank_core2_board.h"
#include "heartbeat.h"
#include "mem_test.h"

#include "HDMI.h"
#include "ff.h"
#include "usbhid_wrapper.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "audio_i2s.pio.h"
#include "hardware/structs/sysinfo.h"
#include "hardware/vreg.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "1.0"
#endif

#define AUDIO_SAMPLE_RATE 44100u

static link_node_info_t   g_master_info;
static link_mem_result_t  g_master_mem;
static diag_link_result_t g_link_result;

static PIO                g_audio_pio;
static uint               g_audio_sm;
static bool               g_audio_up;
static bool               g_audio_ok;
static FATFS              g_fatfs;

/* Progress marker.
 *
 * This is a diagnostic firmware, so "it stopped and told you nothing" is
 * the one failure mode it must not have. Every stage announces itself on
 * the serial console before it runs, so a stall is located by reading
 * the last line rather than by bisecting the binary. That is exactly how
 * the I2S interrupt collision was found. */
static void stage(const char *what) {
    printf("[boot] %s\n", what);
    stdio_flush();
}

/* ------------------------------------------------------------------ */
/* Identity                                                            */
/* ------------------------------------------------------------------ */

static void collect_identity(void) {
    uint32_t jedec = 0;
    uint8_t  uid[8] = { 0 };

    mem_test_flash_identify(&jedec, uid);

    memcpy(g_master_info.chip_id, uid, 8);
    g_master_info.flash_jedec_id = jedec;
    g_master_info.flash_bytes    = mem_test_flash_capacity(jedec);
    g_master_info.sys_clk_hz     = clock_get_hz(clk_sys);
    g_master_info.fw_version     = 0x0100;

    uint32_t pkg = *((io_ro_32 *)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    g_master_info.package_is_a = (pkg & 1u) ? 1 : 0;
    g_master_info.rp2350_rev   = (uint8_t)((*((io_ro_32 *)(SYSINFO_BASE +
                                    SYSINFO_CHIP_ID_OFFSET)) >> 28) & 0xFu);
}

/* ------------------------------------------------------------------ */
/* Report rendering                                                    */
/* ------------------------------------------------------------------ */

static void render_header(void) {
    console_at(R_TITLE, 0, C_TITLE,
               "FRANK Core 2  -  dual RP2350 bring-up      v" FIRMWARE_VERSION);
    console_rule(R_RULE_TOP, C_DIM);
    console_at(R_COLHDR, R_MASTER_COL, C_ACCENT, "MASTER 2350B");
    console_at(R_COLHDR, R_SLAVE_COL,  C_ACCENT, "SLAVE 2350A");

    console_at(R_CHIP_ID,     0, C_DIM, "Chip ID");
    console_at(R_PACKAGE,     0, C_DIM, "Package/rev");
    console_at(R_SYSCLK,      0, C_DIM, "Sys clock");
    console_at(R_FLASH_ID,    0, C_DIM, "Flash ID");
    console_at(R_FLASH_READ,  0, C_DIM, "Flash read");
    console_at(R_FLASH_CRC,   0, C_DIM, "Flash CRC32");
    console_at(R_PSRAM_SIZE,  0, C_DIM, "PSRAM");
    console_at(R_PSRAM_WRITE, 0, C_DIM, "PSRAM write");
    console_at(R_PSRAM_READ,  0, C_DIM, "PSRAM read");
    console_at(R_PSRAM_ERR,   0, C_DIM, "PSRAM errors");

    console_rule(R_RULE_MID, C_DIM);
    console_flush();
}

/* Fill one identity/memory column. `col` selects master or slave. */
static void render_column(int col, const link_node_info_t *info,
                          const link_mem_result_t *mem, bool valid) {
    char buf[24];

    if (!valid) {
        /* Placeholder only — kept off the serial log, which would
         * otherwise carry ten lines of "-" before every run. */
        for (int row = R_CHIP_ID; row <= R_PSRAM_ERR; row++)
            console_at_quiet(row, col, C_DIM, "-");
        return;
    }

    console_at(R_CHIP_ID, col, C_TEXT, "%02X%02X%02X%02X%02X%02X%02X%02X",
               info->chip_id[0], info->chip_id[1], info->chip_id[2],
               info->chip_id[3], info->chip_id[4], info->chip_id[5],
               info->chip_id[6], info->chip_id[7]);

    console_at(R_PACKAGE, col, C_TEXT, "%s / rev %u",
               info->package_is_a ? "QFN-60 A" : "QFN-80 B",
               (unsigned)info->rp2350_rev);

    console_at(R_SYSCLK, col, C_TEXT, "%u MHz",
               (unsigned)(info->sys_clk_hz / 1000000u));

    if (mem) {
        report_format_size(buf, sizeof(buf), mem->flash_bytes);
        console_at(R_FLASH_ID, col, mem->flash_ok ? C_TEXT : C_FAIL,
                   "%06X %s", (unsigned)info->flash_jedec_id, buf);

        report_format_kbps(buf, sizeof(buf), mem->flash_read_kbps);
        console_at(R_FLASH_READ, col, C_TEXT, "%s", buf);

        console_at(R_FLASH_CRC, col, C_TEXT, "%08X", (unsigned)mem->flash_crc);

        report_format_size(buf, sizeof(buf), mem->psram_bytes);
        console_at(R_PSRAM_SIZE, col, mem->psram_ok ? C_OK : C_FAIL,
                   "%s %s", buf, mem->psram_ok ? "ok" : "FAIL");

        report_format_kbps(buf, sizeof(buf), mem->psram_write_kbps);
        console_at(R_PSRAM_WRITE, col, C_TEXT, "%s", buf);

        report_format_kbps(buf, sizeof(buf), mem->psram_read_kbps);
        console_at(R_PSRAM_READ, col, C_TEXT, "%s", buf);

        console_at(R_PSRAM_ERR, col,
                   mem->psram_bit_errors ? C_FAIL : C_OK,
                   "%u", (unsigned)mem->psram_bit_errors);
    }
}

/* ------------------------------------------------------------------ */
/* Local peripherals                                                   */
/* ------------------------------------------------------------------ */

static void test_sdcard(char *status, size_t status_len) {
    FRESULT fr = f_mount(&g_fatfs, "", 1);
    if (fr != FR_OK) {
        snprintf(status, status_len, "SD:none");
        console_log(C_WARN, "sd: not mounted (FatFS error %d)", (int)fr);
        return;
    }

    DWORD free_clusters = 0;
    FATFS *fs = NULL;
    uint64_t total = 0, freeb = 0;

    if (f_getfree("", &free_clusters, &fs) == FR_OK) {
        total = (uint64_t)(fs->n_fatent - 2) * fs->csize * 512ull;
        freeb = (uint64_t)free_clusters * fs->csize * 512ull;
    }

    char t[16], f[16];
    report_format_size(t, sizeof(t), total);
    report_format_size(f, sizeof(f), freeb);

    snprintf(status, status_len, "SD:%s", t);
    console_log(C_OK, "sd: mounted, %s total, %s free", t, f);
}

/* Bring up the I2S state machine directly rather than through the
 * frank-msx driver's i2s_init().
 *
 * That driver installs an exclusive DMA_IRQ_0 handler for its
 * double-buffered playback path — but the HSTX video driver has already
 * claimed DMA_IRQ_0 (video_output.c). The second irq_set_exclusive_handler()
 * hard-asserts, panic() executes a breakpoint with no debugger attached,
 * and the core escalates straight into lockup: report half-drawn, USB
 * dead, no message. A debug probe reading pc == 0xeffffffe is what
 * finally pinned it down.
 *
 * A tone test needs neither DMA nor an interrupt, so it takes none. The
 * SM is claimed once because the diagnostic is re-runnable from a
 * keypress and pio1 only has four of them. */
static void audio_init_once(void) {
    if (g_audio_up) return;

    g_audio_pio = pio1;
    g_audio_sm  = pio_claim_unused_sm(g_audio_pio, true);

    uint offset = pio_add_program(g_audio_pio, &audio_i2s_program);
    audio_i2s_program_init(g_audio_pio, g_audio_sm, offset,
                           I2S_DATA_PIN, I2S_CLOCK_PIN_BASE);

    /* 32 bits per stereo frame x 2 PIO cycles per bit => sys_clk*4/rate
     * in 8.8 fixed point, matching the driver's own arithmetic. */
    uint32_t divider = clock_get_hz(clk_sys) * 4u / AUDIO_SAMPLE_RATE;
    pio_sm_set_clkdiv_int_frac(g_audio_pio, g_audio_sm,
                               divider >> 8u, divider & 0xffu);
    pio_sm_set_enabled(g_audio_pio, g_audio_sm, true);

    g_audio_up = true;
}

/* Push a short tone through the TDA1387 so the I2S clocks, the DAC and
 * the analogue path all get exercised. Anything audible means SCLK,
 * LRCK and DATA on GPIO 10/11/9 are all moving.
 *
 * Feeds the PIO FIFO directly rather than going through the driver's
 * i2s_dma_write(). That call blocks until its DMA completion IRQ frees
 * a buffer, and on this firmware the HSTX video path already owns the
 * DMA interrupt, so the buffer is never released and the whole
 * diagnostic wedges here — silently, with the report half-drawn.
 *
 * The deeper rule: no single probe may be able to hang the run. A
 * diagnostic that stops at the first unhappy peripheral cannot report
 * on the ones after it, which is precisely when you need it most.
 * Everything here is bounded by a deadline and degrades to a warning. */
static bool test_audio(void) {
    audio_init_once();

    /* Roughly 0.2 s of a square wave a little above 1 kHz. */
    const uint32_t frames = AUDIO_SAMPLE_RATE / 5;   /* ~0.2 s */
    absolute_time_t deadline = make_timeout_time_ms(600);

    for (uint32_t i = 0; i < frames; i++) {
        int16_t s = (int16_t)(((i / 11) & 1) ? 6000 : -6000);
        uint32_t stereo = ((uint32_t)(uint16_t)s << 16) | (uint16_t)s;

        while (pio_sm_is_tx_fifo_full(g_audio_pio, g_audio_sm)) {
            if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
                console_log(C_WARN,
                            "audio: I2S FIFO not draining - no bit clock?");
                return false;
            }
        }
        pio_sm_put(g_audio_pio, g_audio_sm, stereo);
    }

    console_log(C_OK, "audio: TDA1387 tone sent on GPIO %d/%d/%d",
                I2S_DATA_PIN, I2S_CLOCK_PIN_BASE, I2S_CLOCK_PIN_BASE + 1);
    return true;
}

static void render_peripherals(const char *sd_status) {
    const char *usb;
#ifdef USB_HID_ENABLED
    usb = usbhid_wrapper_keyboard_connected() ? "USB:kbd" : "USB:host";
#else
    usb = "USB:cdc";
#endif

    console_clear_row(R_PERIPH);
    console_at(R_PERIPH, 0, C_TEXT, " %s   %s   %s   HDMI:640x480@60",
               sd_status, usb, g_audio_ok ? "I2S:ok" : "I2S:FAIL");
    console_flush();
}

/* ------------------------------------------------------------------ */

static void run_full_diagnostic(void) {
    char sd_status[24] = "SD:?";

    stage("diag: console_clear");
    console_clear();
    stage("diag: render_header");
    render_header();
    stage("diag: header done");

    heartbeat_set(HB_TEST);
    console_log(C_DIM, "master: running flash + PSRAM tests...");

    stage("diag: mem_test_run_all");
    mem_test_run_all(&g_master_mem,
                     g_master_info.flash_bytes,
                     g_master_info.psram_bytes);
    stage("diag: mem_test done");

    render_column(R_MASTER_COL, &g_master_info, &g_master_mem, true);
    render_column(R_SLAVE_COL, NULL, NULL, false);
    console_flush();

    console_log(g_master_mem.psram_ok ? C_OK : C_FAIL,
                "master: flash %s, psram %s",
                g_master_mem.flash_ok ? "ok" : "FAIL",
                g_master_mem.psram_ok ? "ok" : "FAIL");

    stage("diag: sdcard");
    test_sdcard(sd_status, sizeof(sd_status));
    stage("diag: audio");
    g_audio_ok = test_audio();
    stage("diag: peripherals rendered");
    render_peripherals(sd_status);

    stage("diag: link");
    diag_link_run(&g_link_result);
    stage("diag: link done");

    if (g_link_result.contacted) {
        render_column(R_SLAVE_COL, &g_link_result.slave_info,
                      g_link_result.slave_mem_valid ? &g_link_result.slave_mem : NULL,
                      true);
        console_flush();
    }

    console_log(C_DIM, "press any key to re-run");
}

/* Boot progress marker.
 *
 * This is a diagnostic firmware, so "it hung and told you nothing" is
 * the one failure mode it must not have. Every stage announces itself
 * on the serial console before it runs, so a hang is located by reading
 * the last line rather than by bisecting the binary. */
int main(void) {
    vreg_set_voltage(CPU_VOLTAGE);
    sleep_ms(10);

    /* Not "required": on failure fall back to a speed the part will
     * certainly reach rather than panicking into a silent hang. The
     * report prints the clock we actually got. */
    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false))
        set_sys_clock_khz(125 * 1000, false);

    stdio_init_all();

    /* Attach window. USB CDC enumeration plus the operator getting a
     * terminal open reliably takes longer than the diagnostic takes to
     * run, and a report you missed is a report you do not have. Repeat
     * the banner for a couple of seconds so a console attached at any
     * point in that window sees the run from the start. */
    for (int i = 0; i < 8; i++) {
        printf("FRANK Core 2 master - waiting for console (%d/8)\n", i + 1);
        sleep_ms(250);
    }

    /* USB CDC enumeration takes well over the 200 ms this used to
     * allow, and everything printed before the host attaches is lost.
     * Wait for the connection, with a ceiling so a UART-only or
     * headless run still boots. */
#if !defined(USB_HID_ENABLED)
    for (int i = 0; i < 60 && !stdio_usb_connected(); i++) sleep_ms(50);
#endif
    sleep_ms(100);

    printf("\n\nFRANK Core 2 master firmware v%s\n", FIRMWARE_VERSION);
    printf("sys_clk %u MHz\n", (unsigned)(clock_get_hz(clk_sys) / 1000000u));

    /* WS2812 heartbeat on pio2 — pio0 belongs to the link, pio1 to I2S. */
    stage("heartbeat (WS2812 on GPIO46, pio2)");
    heartbeat_init_ws2812(pio2, M_LED_WS2812_PIN);
    heartbeat_set(HB_BOOT);

    /* Both of these touch the QMI directly and must run with core 1
     * parked; graphics_init() below is what wakes it. */
    stage("flash identify");
    collect_identity();

    stage("PSRAM probe (CS GPIO47)");
    g_master_info.psram_bytes = mem_test_psram_probe(M_PSRAM_CS_PIN);
    printf("[boot] PSRAM %u MB\n",
           (unsigned)(g_master_info.psram_bytes / (1024u * 1024u)));

    stage("HSTX HDMI + core 1");
    graphics_init(g_out_HDMI);
    console_init();

    stage("USB HID");
    usbhid_wrapper_init();

    stage("link (pio0)");
    diag_link_init();

    stage("running diagnostic");

    run_full_diagnostic();

    /* Idle: keep USB HID serviced and re-run on any key. The heartbeat
     * runs off its own timer, so the LED keeps moving regardless.
     *
     * The periodic "idle" line matters more than it looks: on a console
     * attached after boot it is the only thing that distinguishes a
     * finished run from a wedged one, and it does so without the
     * operator having to see the LED. */
    absolute_time_t next_tick = make_timeout_time_ms(5000);
    uint32_t idle_ticks = 0;

    while (true) {
        usbhid_wrapper_tick();

        int pressed = 0;
        unsigned char key = 0;
        bool rerun = usbhid_wrapper_get_key(&pressed, &key) && pressed;

        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) rerun = true;

        if (rerun) run_full_diagnostic();

        if (absolute_time_diff_us(get_absolute_time(), next_tick) < 0) {
            printf("[idle %u] link %s - press any key to re-run\n",
                   (unsigned)++idle_ticks,
                   g_link_result.contacted
                       ? (g_link_result.all_passed ? "OK" : "DEGRADED")
                       : "DOWN");
            next_tick = make_timeout_time_ms(5000);

            /* Nothing on this board lets the master power-cycle the
             * slave, so a slave that boots late or reboots on its own
             * has to be noticed rather than forced into step. Keep
             * probing while the link is down; the moment it answers,
             * re-run the whole diagnostic unprompted. */
            if (!g_link_result.contacted && diag_link_try_reconnect()) {
                printf("[idle] slave appeared - re-running diagnostic\n");
                run_full_diagnostic();
            }
        }

        sleep_ms(5);
    }
}
