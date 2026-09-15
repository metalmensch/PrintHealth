// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Dempsey
#pragma once
#include <stdbool.h>

#include <stdint.h>

#define IPP_MAX_MARKERS 6       /* CMYK + a little slack */

typedef struct {
    bool ok;                    /* true if the query succeeded */
    char make_and_model[64];    /* e.g. "HP ColorLaserJet M255-M256" */
    int  state;                 /* IPP printer-state: 3 idle, 4 processing, 5 stopped */
    char state_reasons[64];     /* e.g. "toner-low-warning", "none" */
    int  queued_jobs;           /* IPP queued-job-count */

    /* Supply (toner) markers, from marker-levels/-colors/-names. */
    int      marker_count;
    int      marker_level[IPP_MAX_MARKERS];  /* 0-100, or <0 if unknown */
    uint32_t marker_rgb[IPP_MAX_MARKERS];    /* 0xRRGGBB display color */
    char     marker_letter[IPP_MAX_MARKERS]; /* 'K','C','M','Y', or '?' */
} printer_info_t;

/* Query the printer over IPP (Get-Printer-Attributes). host is the printer's
 * IP on the SoftAP subnet, e.g. "192.168.4.2". Returns true on success and
 * fills *out. Blocks up to a few seconds. */
bool ipp_get_printer_info(const char *host, printer_info_t *out);

/* Human-readable printer-state. */
const char *ipp_state_str(int state);
