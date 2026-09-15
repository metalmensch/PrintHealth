/*
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
    buf[p++] = IPP_TAG_END;
    return p;
}

/* Walk the IPP response body and pull out the attributes we asked for. */
static void parse_response(const uint8_t *b, int len, printer_info_t *out)
{
    int i = 8;                 /* skip version(2) status(2) request-id(4) */
    char cur[48] = "";
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
            /* keep only the first reason listed */
            if (out->state_reasons[0] == '\0') {
                int c = vl < (int)sizeof(out->state_reasons) - 1 ? vl : (int)sizeof(out->state_reasons) - 1;
                memcpy(out->state_reasons, val, c); out->state_reasons[c] = '\0';
            }
        }
    }
}

bool ipp_get_printer_info(const char *host, printer_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = 3;

    char url[64], uri[64];
    snprintf(url, sizeof(url), "http://%s:631/ipp/print", host);
    snprintf(uri, sizeof(uri), "ipp://%s/ipp/print", host);

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
    esp_err_t err = esp_http_client_open(cli, req_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        goto done;
    }
    if (esp_http_client_write(cli, (const char *)req, req_len) != (int)req_len) {
        ESP_LOGW(TAG, "write failed");
        goto done;
    }
    int clen = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP status %d", status);
        goto done;
    }
    (void)clen;
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
    }
done:
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return ok;
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
