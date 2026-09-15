// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Dempsey
#pragma once
#include <stdbool.h>

typedef struct {
    bool ok;                    /* true if the query succeeded */
    char make_and_model[64];    /* e.g. "HP ColorLaserJet M255-M256" */
    int  state;                 /* IPP printer-state: 3 idle, 4 processing, 5 stopped */
    char state_reasons[64];     /* e.g. "toner-low-warning", "none" */
    int  queued_jobs;           /* IPP queued-job-count */
} printer_info_t;

/* Query the printer over IPP (Get-Printer-Attributes). host is the printer's
 * IP on the SoftAP subnet, e.g. "192.168.4.2". Returns true on success and
 * fills *out. Blocks up to a few seconds. */
bool ipp_get_printer_info(const char *host, printer_info_t *out);

/* Human-readable printer-state. */
const char *ipp_state_str(int state);
