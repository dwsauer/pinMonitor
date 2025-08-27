// File: components/wifi_provisioning/src/web_server.c
/**
 * @file   web_server.c
 * @brief  Tiny provisioning web server for pinMonitor (Wi-Fi + MQTT).
 *
 * @details
 *  - GET "/" renders a form:
 *      * Shows scanned SSIDs (dropdown)
 *      * Accepts Wi-Fi password
 *      * Accepts MQTT URI / username / password
 *  - POST "/submit" saves:
 *      * Wi-Fi → NVS "wifi_store": keys "ssid", "password"
 *      * MQTT → NVS "mqtt_store": keys "uri", "user", "pass"
 *    then reboots the device.
 *
 * Implementation notes
 *  - Uses blocking Wi-Fi scan inside the GET handler (simple + fine for provisioning).
 *  - Avoids large format buffers for `<option>` rows by streaming HTML in small chunks,
 *    eliminating -Wformat-truncation warnings/errors.
 *  - POST body parser handles application/x-www-form-urlencoded, including '+' and %xx decoding.
 *  - Does NOT log secrets (Wi-Fi/MQTT passwords).
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"         // esp_restart
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "web_server.h"

static const char *TAG = "web_server";

/* Server handle for lifecycle management */
static httpd_handle_t s_server = NULL;

/* ============================================================
 *                      Small helper utilities
 * ============================================================*/

/* Decode one hex digit to nibble; returns -1 on invalid char */
static int hex2nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/**
 * @brief URL-decode an x-www-form-urlencoded string in place.
 *        Converts '+' to space and %xx hex escapes.
 */
static void url_decode_inplace(char *s) {
    char *w = s;
    for (; *s; ++s) {
        if (*s == '+') { *w++ = ' '; }
        else if (*s == '%' && s[1] && s[2]) {
            int hi = hex2nibble(s[1]), lo = hex2nibble(s[2]);
            if (hi >= 0 && lo >= 0) { *w++ = (char)((hi<<4) | lo); s += 2; }
        } else { *w++ = *s; }
    }
    *w = '\0';
}

/**
 * @brief Extract key=value from an x-www-form-urlencoded buffer.
 *        Writes a NUL-terminated decoded value to @p out (may be empty string).
 * @return true if key found, false otherwise.
 */
static bool form_get(const char *body, const char *key, char *out, size_t out_len) {
    if (!body || !key || !out || out_len == 0) return false;
    out[0] = '\0';
    const size_t klen = strlen(key);
    const char *p = body;
    while ((p = strstr(p, key))) {
        const bool at_field_start = (p == body) || (p[-1] == '&');
        if (at_field_start && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (p[i] && p[i] != '&' && i + 1 < out_len) { out[i] = p[i]; i++; }
            out[i] = '\0';
            url_decode_inplace(out);
            return true;
        }
        p += klen;
    }
    return false;
}

/**
 * @brief Minimal HTML escaper for SSID display/values.
 *        Escapes &, <, >, " to entities. Safe for attribute and text contexts.
 *
 * @param dst    Destination buffer (NUL-terminated on return)
 * @param dst_sz Size of destination buffer
 * @param src    Input string (may be non-terminated 802.11 SSID bytes in practice;
 *               IDF exposes them as a C string so this is fine)
 */
static void html_escape(char *dst, size_t dst_sz, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 6 < dst_sz; i++) {
        char c = src[i];
        if (c == '&') { j += snprintf(dst + j, dst_sz - j, "&amp;"); }
        else if (c == '<') { j += snprintf(dst + j, dst_sz - j, "&lt;"); }
        else if (c == '>') { j += snprintf(dst + j, dst_sz - j, "&gt;"); }
        else if (c == '"') { j += snprintf(dst + j, dst_sz - j, "&quot;"); }
        else { dst[j++] = c; }
    }
    dst[j] = '\0';
}

/* NVS set helper: store empty string if val is NULL/empty */
static esp_err_t nvs_set_str_checked(nvs_handle_t h, const char *key, const char *val) {
    return nvs_set_str(h, key, (val && val[0]) ? val : "");
}

/* ============================================================
 *                          HTTP handlers
 * ============================================================*/

/**
 * @brief GET "/" — Render provisioning form after a blocking Wi-Fi scan.
 *
 * Implementation detail to avoid -Wformat-truncation:
 *   We **stream** `<option>` rows in small chunks instead of snprintf into a
 *   large temporary buffer. This handles worst-case HTML expansion (each SSID
 *   byte could become &amp; → 5 chars) without huge stack arrays or warnings.
 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    // 1) Scan synchronously (keeps code simple)
    wifi_scan_config_t scan_cfg = { .ssid = 0, .bssid = 0, .channel = 0, .show_hidden = false };
    (void)esp_wifi_scan_start(&scan_cfg, true);

    uint16_t ap_count = 0;
    (void)esp_wifi_scan_get_ap_num(&ap_count);

    httpd_resp_set_type(req, "text/html; charset=utf-8");

    wifi_ap_record_t *ap_records = NULL;
    if (ap_count) {
        ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (ap_records) (void)esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    }

    // 2) Page header + form start
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>pinMonitor Setup</title>"
        "<style>body{font-family:sans-serif;max-width:700px;margin:2rem auto;padding:0 1rem}"
        "label{display:block;margin:.6rem 0 .25rem}input,select{width:100%;padding:.5rem}"
        "button{margin-top:1rem;padding:.6rem 1rem}</style></head><body>"
        "<h2>pinMonitor Setup</h2>"
        "<form action='/submit' method='post'>"

        "<h3>Wi-Fi</h3>"
        "<label>SSID</label><select name='ssid'>");

    // 3) Options (streamed)
    if (ap_records) {
        for (int i = 0; i < ap_count; i++) {
            // Worst-case escaped length for 32-byte SSID: 32 * 5 (+NUL) = 161 → use 192
            char ssid_safe[192];
            html_escape(ssid_safe, sizeof(ssid_safe), (const char *)ap_records[i].ssid);

            httpd_resp_sendstr_chunk(req, "<option value=\"");
            httpd_resp_sendstr_chunk(req, ssid_safe);
            httpd_resp_sendstr_chunk(req, "\">");
            httpd_resp_sendstr_chunk(req, ssid_safe);

            char rssi_buf[24];
            snprintf(rssi_buf, sizeof(rssi_buf), " (RSSI %d)</option>", ap_records[i].rssi);
            httpd_resp_sendstr_chunk(req, rssi_buf);
        }
    } else {
        httpd_resp_sendstr_chunk(req, "<option value=''>-- No networks found --</option>");
    }

    // 4) Rest of form
    httpd_resp_sendstr_chunk(req,
        "</select>"
        "<label>Password</label><input type='password' name='pass' maxlength='63'>"

        "<h3>MQTT</h3>"
        "<label>Broker URI (e.g., mqtt://10.0.0.2:1883)</label>"
        "<input name='mqtt_uri' maxlength='127' placeholder='mqtt://host:1883' required>"
        "<label>Username</label><input name='mqtt_user' maxlength='63' placeholder='(optional)'>"
        "<label>Password</label><input type='password' name='mqtt_pass' maxlength='63' placeholder='(optional)'>"

        "<button type='submit'>Save & Reboot</button>"
        "</form></body></html>");

    // 5) Finish chunked response
    httpd_resp_sendstr_chunk(req, NULL);

    free(ap_records);
    return ESP_OK;
}

/**
 * @brief POST "/submit" — Parse form, save to NVS, and reboot.
 *
 * Security: does not log plaintext passwords.
 */
static esp_err_t submit_post_handler(httpd_req_t *req)
{
    const int to_read = req->content_len;
    if (to_read <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    // Read the whole x-www-form-urlencoded body
    char *body = calloc(1, to_read + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem");
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    while (total < to_read) {
        int r = httpd_req_recv(req, body + total, to_read - total);
        if (r <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv error"); return ESP_FAIL; }
        total += r;
    }
    body[total] = '\0';

    // Extract fields (URL-decodes into fixed buffers)
    char ssid[32]={0}, pass[64]={0}, uri[128]={0}, user[64]={0}, mpass[64]={0};
    (void)form_get(body, "ssid",      ssid, sizeof(ssid));
    (void)form_get(body, "pass",      pass, sizeof(pass));
    (void)form_get(body, "mqtt_uri",  uri,  sizeof(uri));
    (void)form_get(body, "mqtt_user", user, sizeof(user));
    (void)form_get(body, "mqtt_pass", mpass,sizeof(mpass));

    // Log non-sensitive summary (do NOT log passwords)
    ESP_LOGI(TAG, "Provision request: SSID='%s', MQTT URI='%s', MQTT user='%s'",
             ssid, uri, user[0] ? user : "(none)");

    // Persist Wi-Fi creds
    nvs_handle_t h;
    esp_err_t err = nvs_open("wifi_store", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_str_checked(h, "ssid",     ssid);
        nvs_set_str_checked(h, "password", pass);   // unified key name = "password"
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGE(TAG, "nvs_open(wifi_store) failed: %s", esp_err_to_name(err));
    }

    // Persist MQTT settings
    err = nvs_open("mqtt_store", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_str_checked(h, "uri",  uri);
        nvs_set_str_checked(h, "user", user);
        nvs_set_str_checked(h, "pass", mpass);
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGE(TAG, "nvs_open(mqtt_store) failed: %s", esp_err_to_name(err));
    }

    // Respond and reboot
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<!doctype html><html><body><h3>Saved. Rebooting…</h3></body></html>");

    free(body);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ============================================================
 *                      Server lifecycle API
 * ============================================================*/

esp_err_t web_server_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true; // reclaim handlers under memory pressure
    config.server_port = 80;

    ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        s_server = NULL;
        return ret;
    }

    const httpd_uri_t root   = { .uri="/",       .method=HTTP_GET,  .handler=root_get_handler,   .user_ctx=NULL };
    const httpd_uri_t submit = { .uri="/submit", .method=HTTP_POST, .handler=submit_post_handler, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &submit);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Web server stopped");
    }
}
/* ============================================================
 *                           End
 * ============================================================*/