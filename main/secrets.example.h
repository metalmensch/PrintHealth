/*
 * secrets.example.h -- template for Wi-Fi credentials.
 *
 * DO NOT put real credentials in this file. Instead:
 *   cp main/secrets.example.h main/secrets.h
 * then edit main/secrets.h. That file is git-ignored so your passwords
 * never get committed.
 */
#pragma once

/* ---- Your existing home Wi-Fi (the ESP32 joins this as a station) ---- */
/* Prefer your 5 GHz SSID if you have one -- it is less congested and the
 * printer supports 5 GHz. */
#define HOME_WIFI_SSID      "your-home-ssid"
#define HOME_WIFI_PASS      "your-home-password"

/* ---- The new Wi-Fi the ESP32 broadcasts for the printer to join ---- */
/* This is what you will select on the printer's control panel. Password
 * must be 8-63 characters (WPA2). Keep it different from your home Wi-Fi. */
#define PRINTER_AP_SSID     "M255dw-Bridge"
#define PRINTER_AP_PASS     "change-me-8chars-min"
