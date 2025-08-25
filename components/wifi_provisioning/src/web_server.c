#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "web_server.h"

static const char *TAG = "web_server";

static httpd_handle_t s_server = NULL;
static void (*s_save_fn)(const char *, const char *) = NULL;

/* Small HTML escape (only quotes & < > for now) */
static void html_escape(char *dst, size_t dst_sz, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 6 < dst_sz; i++) {
        char c = src[i];
        if (c == '<') { j += snprintf(dst + j, dst_sz - j, "&lt;"); }
        else if (c == '>') { j += snprintf(dst + j, dst_sz - j, "&gt;"); }
        else if (c == '"') { j += snprintf(dst + j, dst_sz - j, "&quot;"); }
        else if (c == '&') { j += snprintf(dst + j, dst_sz - j, "&amp;"); }
        else { dst[j++] = c; }
    }
    dst[j] = '\0';
}

/* Serve provisioning page with scanned SSIDs */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Heap before scan: %u", esp_get_free_heap_size());
    // Do a blocking scan
    wifi_scan_config_t scan_cfg = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = false
    };
    esp_wifi_scan_start(&scan_cfg, true);

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "Scan found %u APs", ap_count);

    if (ap_count == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req, "<html><body><h3>No Wi-Fi networks found. Please try again.</h3></body></html>");
        return ESP_OK;
    }

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        ESP_LOGE(TAG, "Heap after scan: %u", esp_get_free_heap_size());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem");
        return ESP_FAIL;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    // Build HTML
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
        "<title>ESP32 Wi-Fi Setup</title></head><body>"
        "<h2>Wi-Fi Provisioning</h2>"
        "<form action=\"/submit\" method=\"post\">"
        "SSID:<br><select name=\"ssid\">");

    for (int i = 0; i < ap_count; i++) {
        char ssid_safe[64];
        html_escape(ssid_safe, sizeof(ssid_safe), (char *)ap_records[i].ssid);

        char option[128];
        snprintf(option, sizeof(option),
                 "<option value=\"%.32s\">%.32s (RSSI %d)</option>",
                 ssid_safe, ssid_safe, ap_records[i].rssi);
        httpd_resp_sendstr_chunk(req, option);
    }

    httpd_resp_sendstr_chunk(req,
        "</select><br><br>"
        "Password:<br><input type=\"password\" name=\"pass\"><br><br>"
        "<input type=\"submit\" value=\"Save\">"
        "</form></body></html>");

    httpd_resp_sendstr_chunk(req, NULL); // end of chunked response
    free(ap_records);
    ESP_LOGI(TAG, "Heap after response: %u", esp_get_free_heap_size());
    return ESP_OK;
}

/* Handle form submission (same as before) */
static esp_err_t submit_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Heap before POST alloc: %u", esp_get_free_heap_size());
    char *buf = calloc(1, total_len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "Heap after POST alloc fail: %u", esp_get_free_heap_size());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem");
        return ESP_ERR_NO_MEM;
    }

    int recv_len = httpd_req_recv(req, buf, total_len);
    if (recv_len <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv error");
        return ESP_FAIL;
    }
    buf[recv_len] = '\0';

    ESP_LOGI(TAG, "Form body: %s", buf);

    char ssid[32] = {0}, pass[64] = {0};
    char *ssid_ptr = strstr(buf, "ssid=");
    char *pass_ptr = strstr(buf, "pass=");

    if (ssid_ptr) {
        ssid_ptr += 5;
        char *end = strchr(ssid_ptr, '&');
        size_t len = end ? (size_t)(end - ssid_ptr) : strlen(ssid_ptr);
        if (len >= sizeof(ssid)) len = sizeof(ssid) - 1;
        strncpy(ssid, ssid_ptr, len);
        ssid[len] = '\0';
    }
    if (pass_ptr) {
        pass_ptr += 5;
        size_t len = strlen(pass_ptr);
        if (len >= sizeof(pass)) len = sizeof(pass) - 1;
        strncpy(pass, pass_ptr, len);
        pass[len] = '\0';
    }

    // Replace '+' with spaces
    for (char *p = ssid; *p; ++p) if (*p == '+') *p = ' ';
    for (char *p = pass; *p; ++p) if (*p == '+') *p = ' ';

    ESP_LOGI(TAG, "User selected SSID: '%s'", ssid);
    ESP_LOGI(TAG, "User entered password: '%s'", pass);

    if (s_save_fn && ssid[0]) {
        s_save_fn(ssid, pass);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<html><body><h3>Credentials saved. ESP32 will reconnect.</h3></body></html>");

    free(buf);
    ESP_LOGI(TAG, "Heap after POST response: %u", esp_get_free_heap_size());
    return ESP_OK;
}

/* Start server */
esp_err_t web_server_start(void (*save_fn)(const char *, const char *))
{
    if (s_server) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_OK;
    }

    s_save_fn = save_fn;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.server_port = 80;

    ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error starting server: %s", esp_err_to_name(ret));
        s_server = NULL;
        return ret;
    }

    httpd_uri_t root_uri = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = root_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &root_uri);

    httpd_uri_t submit_uri = {
        .uri      = "/submit",
        .method   = HTTP_POST,
        .handler  = submit_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &submit_uri);

    return ESP_OK;
}

/* Stop server */
void web_server_stop(void)
{
    if (s_server) {
        ESP_LOGI(TAG, "Stopping web server");
        httpd_stop(s_server);
        s_server = NULL;
    }
}
