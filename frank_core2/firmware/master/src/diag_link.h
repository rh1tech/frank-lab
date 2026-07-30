/*
 * FRANK Core 2 — dual-RP2350 test firmware
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * diag_link.h — the master's half of the inter-processor test.
 *
 * Brings the link up, interrogates the slave, sweeps the wire rate and
 * writes the results straight into the console report.
 */
#ifndef DIAG_LINK_H
#define DIAG_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "link_proto.h"

typedef struct {
    bool               contacted;      /* slave answered HELLO           */
    link_node_info_t   slave_info;
    bool               slave_mem_valid;
    link_mem_result_t  slave_mem;

    /* Per-divider sweep results. */
    struct {
        uint16_t clkdiv_q88;           /* divider in 8.8 fixed point     */
        uint32_t m2s_bytes_per_s;
        uint32_t s2m_bytes_per_s;
        uint32_t duplex_bytes_per_s;
        uint32_t errors;               /* from the verified pass         */
        bool     ok;
    } sweep[4];
    int  sweep_count;

    uint32_t rtt_ns;                   /* 128-byte control round trip    */
    uint32_t best_bytes_per_s;         /* fastest error-free divider     */
    bool     all_passed;
} diag_link_result_t;

/* Claim PIO/DMA and configure the master's link pins. Call once. */
void diag_link_init(void);

/* Run the whole sequence: handshake, remote self-test, rate sweep,
 * verified transfers, latency. Renders progress into the console log
 * and the results into the fixed report rows as it goes. */
void diag_link_run(diag_link_result_t *out);

/* Repaint the link table from a previous run (used after a redraw). */
void diag_link_render(const diag_link_result_t *r);

#endif /* DIAG_LINK_H */
