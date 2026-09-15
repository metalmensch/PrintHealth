// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jason Dempsey
#pragma once
#include <stdbool.h>
#include <time.h>
#include "ipp_client.h"

/* Snapshot of everything the status display shows. Updated by the status
 * task; read under status_lock()/status_unlock(). */
typedef struct {
    bool          wifi_up;      /* station connected to home Wi-Fi */
    int           rssi;         /* home Wi-Fi RSSI in dBm */
    unsigned      uptime_s;     /* seconds since boot */
    time_t        now;          /* wall clock, 0 until NTP sync */
    int           ap_clients;   /* clients on the SoftAP (the printer) */
    char          bridge_ip[16];/* this bridge's home-LAN IP */
    printer_info_t printer;     /* last IPP query result */
} sys_status_t;

extern sys_status_t g_status;

void status_lock(void);
void status_unlock(void);
