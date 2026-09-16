/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Jason Dempsey
 *
 * Minimal IPP (Internet Printing Protocol) client: a single
 * Get-Printer-Attributes request, just enough to read the model name,
 * printer state, state reasons, and queued job count for the status
 * display. Validated against the HP Color LaserJet Pro M255dw, which
 * serves IPP over HTTP/1.1 at POST /ipp/print (application/ipp).
 */
#include "ipp_client.h"
#include <string.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_http_client.h"

static const char *TAG = "ipp";

/* IPP value/delimiter tags we care about. */
#define IPP_TAG_INTEGER  0x21
#define IPP_TAG_ENUM     0x23
#define IPP_TAG_END      0x03
#define IPP_TAG_OPATTRS  0x01
#define IPP_TAG_CHARSET  0x47
#define IPP_TAG_LANG     0x48
#define IPP_TAG_URI      0x45
#define IPP_TAG_KEYWORD  0x44

/* Append one IPP attribute (tag, name, string value) to buf. */
static size_t put_attr(uint8_t *buf, size_t pos, uint8_t tag,
                       const char *name, const char *val)
{
    size_t nl = strlen(name), vl = strlen(val);
    buf[pos++] = tag;
    buf[pos++] = (nl >> 8) & 0xff; buf[pos++] = nl & 0xff;
    memcpy(buf + pos, name, nl); pos += nl;
    buf[pos++] = (vl >> 8) & 0xff; buf[pos++] = vl & 0xff;
    memcpy(buf + pos, val, vl); pos += vl;
    return pos;
}

static size_t build_request(uint8_t *buf, const char *uri)
{
    size_t p = 0;
    buf[p++] = 0x02; buf[p++] = 0x00;               /* version 2.0 */
    buf[p++] = 0x00; buf[p++] = 0x0b;               /* Get-Printer-Attributes */
    buf[p++] = 0x00; buf[p++] = 0x00;
    buf[p++] = 0x00; buf[p++] = 0x01;               /* request-id 1 */
    buf[p++] = IPP_TAG_OPATTRS;
    p = put_attr(buf, p, IPP_TAG_CHARSET, "attributes-charset", "utf-8");
    p = put_attr(buf, p, IPP_TAG_LANG,    "attributes-natural-language", "en-us");
    p = put_attr(buf, p, IPP_TAG_URI,     "printer-uri", uri);
    p = put_attr(buf, p, IPP_TAG_KEYWORD, "requested-attributes", "printer-make-and-model");
    p = put_attr(buf, p, IPP_TAG_KEYWORD, "", "printer-state");
    p = put_attr(buf, p, IPP_TAG_KEYWORD, "", "printer-state-reasons");
    p = put_attr(buf, p, IPP_TAG_KEYWORD, "", "queued-job-count");
    p = put_attr(buf, p, IPP_TAG_KEYWORD, "", "marker-levels");
    p = put_attr(buf, p, IPP_TAG_KEYWORD, "", "marker-colors");
    p = put_attr(buf, p, IPP_TAG_KEYWORD, "", "marker-names");
    buf[p++] = IPP_TAG_END;
    return p;
}

/* Read a big-endian signed 32-bit IPP integer value. */
static int32_t be_int(const uint8_t *v, int vl)
{
    int32_t x = 0;
    for (int k = 0; k < vl; k++) x = (x << 8) | v[k];
    return x;
}

/* "#RRGGBB" -> 0xRRGGBB; a few color names as fallback; else mid gray. */
static uint32_t parse_marker_color(const uint8_t *v, int vl)
{
    if (vl >= 7 && v[0] == '#') {
        uint32_t rgb = 0;
        for (int k = 1; k <= 6; k++) {
            char c = v[k]; int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0x808080;
            rgb = (rgb << 4) | d;
        }
        return rgb;
    }
    return 0x808080;
}

/* First letter of a marker name / color: C, M, Y, K, else '?'. */
static char marker_letter_from(const char *name, uint32_t rgb)
{
    if (strcasestr(name, "black"))   return 'K';
    if (strcasestr(name, "cyan"))    return 'C';
    if (strcasestr(name, "magenta")) return 'M';
    if (strcasestr(name, "yellow"))  return 'Y';
    switch (rgb) {                 /* fall back to the color */
    case 0x00FFFF: return 'C';
    case 0xFF00FF: return 'M';
    case 0xFFFF00: return 'Y';
    case 0x000000: return 'K';
    default:       return '?';
    }
}

/* Walk the IPP response body and pull out the attributes we asked for. */
static void parse_response(const uint8_t *b, int len, printer_info_t *out)
{
    int i = 8;                 /* skip version(2) status(2) request-id(4) */
    char cur[48] = "";
    /* marker-* are parallel 1setOf lists; collect by position. */
    int nlev = 0, ncol = 0, nname = 0;
    uint32_t colors[IPP_MAX_MARKERS] = {0};
    char letters[IPP_MAX_MARKERS] = {0};
    while (i < len) {
        uint8_t tag = b[i++];
        if (tag == IPP_TAG_END) break;
        if (tag <= 0x05) continue;              /* group/delimiter tag */
        if (i + 2 > len) break;
        int nl = (b[i] << 8) | b[i+1]; i += 2;
        if (i + nl + 2 > len) break;
        if (nl > 0) {
            int c = nl < (int)sizeof(cur) - 1 ? nl : (int)sizeof(cur) - 1;
            memcpy(cur, b + i, c); cur[c] = '\0';
        }
        i += nl;
        int vl = (b[i] << 8) | b[i+1]; i += 2;
        if (i + vl > len) break;
        const uint8_t *val = b + i; i += vl;

        if (!strcmp(cur, "printer-make-and-model")) {
            int c = vl < (int)sizeof(out->make_and_model) - 1 ? vl : (int)sizeof(out->make_and_model) - 1;
            memcpy(out->make_and_model, val, c); out->make_and_model[c] = '\0';
        } else if (!strcmp(cur, "printer-state") && (tag == IPP_TAG_ENUM || tag == IPP_TAG_INTEGER)) {
            out->state = 0; for (int k = 0; k < vl; k++) out->state = (out->state << 8) | val[k];
        } else if (!strcmp(cur, "queued-job-count") && tag == IPP_TAG_INTEGER) {
            out->queued_jobs = 0; for (int k = 0; k < vl; k++) out->queued_jobs = (out->queued_jobs << 8) | val[k];
        } else if (!strcmp(cur, "printer-state-reasons")) {
            /* printer-state-reasons is 1setOf: append each value, comma-joined,
             * so per-color toner reasons all show up. */
            size_t used = strlen(out->state_reasons);
            if (used < sizeof(out->state_reasons) - 2) {
                if (used > 0) { out->state_reasons[used++] = ','; out->state_reasons[used] = '\0'; }
                int room = (int)sizeof(out->state_reasons) - 1 - (int)used;
                int c = vl < room ? vl : room;
                memcpy(out->state_reasons + used, val, c);
                out->state_reasons[used + c] = '\0';
            }
        } else if (!strcmp(cur, "marker-levels") && tag == IPP_TAG_INTEGER) {
            if (nlev < IPP_MAX_MARKERS) out->marker_level[nlev++] = be_int(val, vl);
        } else if (!strcmp(cur, "marker-colors")) {
            if (ncol < IPP_MAX_MARKERS) colors[ncol++] = parse_marker_color(val, vl);
        } else if (!strcmp(cur, "marker-names")) {
            if (nname < IPP_MAX_MARKERS) {
                char nm[48];
                int c = vl < (int)sizeof(nm) - 1 ? vl : (int)sizeof(nm) - 1;
                memcpy(nm, val, c); nm[c] = '\0';
                letters[nname++] = marker_letter_from(nm, 0);
            }
        }
    }

    /* Combine the parallel marker lists into out. Levels drive the count. */
    out->marker_count = nlev;
    for (int k = 0; k < nlev && k < IPP_MAX_MARKERS; k++) {
        out->marker_rgb[k] = (k < ncol) ? colors[k] : 0x808080;
        char L = (k < nname) ? letters[k] : '?';
        if (L == '?') L = marker_letter_from("", out->marker_rgb[k]);
        out->marker_letter[k] = L;
    }
}

/* IPP resource paths to try, most common first. Different printers expose
 * Get-Printer-Attributes at different paths; AirPrint's is /ipp/print. */
static const char *k_paths[] = { "/ipp/print", "/ipp/printer", "/ipp", "/" };
static char s_last_path[24] = "/ipp/print";   /* last path that worked */

const char *ipp_endpoint_path(void) { return s_last_path; }

/* One IPP Get-Printer-Attributes attempt at a specific resource path. */
static bool query_once(const char *host, const char *path, printer_info_t *out)
{
    char url[80], uri[80];
    snprintf(url, sizeof(url), "http://%s:631%s", host, path);
    snprintf(uri, sizeof(uri), "ipp://%s%s", host, path);

    uint8_t req[256];
    size_t req_len = build_request(req, uri);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;
    esp_http_client_set_header(cli, "Content-Type", "application/ipp");

    bool ok = false;
    if (esp_http_client_open(cli, req_len) != ESP_OK) goto done;
    if (esp_http_client_write(cli, (const char *)req, req_len) != (int)req_len) goto done;
    esp_http_client_fetch_headers(cli);
    if (esp_http_client_get_status_code(cli) != 200) goto done;

    uint8_t resp[512];
    int total = 0, r;
    while ((r = esp_http_client_read(cli, (char *)resp + total, sizeof(resp) - total)) > 0) {
        total += r;
        if (total >= (int)sizeof(resp)) break;
    }
    if (total > 8) {
        parse_response(resp, total, out);
        out->ok = true;
        ok = true;
        for (int k = 0; k < out->marker_count; k++) {
            ESP_LOGD(TAG, "marker %c: %d%% (#%06lX)", out->marker_letter[k],
                     out->marker_level[k], (unsigned long)out->marker_rgb[k]);
        }
    }
done:
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return ok;
}

bool ipp_get_printer_info(const char *host, printer_info_t *out)
{
    /* Always try paths in preferred order (AirPrint's /ipp/print first) so a
     * transient boot-time failure of the preferred path doesn't leave us
     * stuck on a fallback -- the next cycle reconverges to the standard one. */
    for (size_t i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]); i++) {
        memset(out, 0, sizeof(*out));
        out->state = 3;
        if (query_once(host, k_paths[i], out)) {
            if (strcmp(s_last_path, k_paths[i])) {
                strncpy(s_last_path, k_paths[i], sizeof(s_last_path) - 1);
                s_last_path[sizeof(s_last_path) - 1] = '\0';
                ESP_LOGI(TAG, "IPP endpoint: %s", s_last_path);
            }
            return true;
        }
    }
    memset(out, 0, sizeof(*out));
    out->state = 3;
    return false;
}

const char *ipp_state_str(int state)
{
    switch (state) {
    case 3: return "idle";
    case 4: return "printing";
    case 5: return "stopped";
    default: return "unknown";
    }
}
