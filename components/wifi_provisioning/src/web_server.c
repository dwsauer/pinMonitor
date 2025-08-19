#include <string.h>
#include "web_server.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "web_server";
static httpd_handle_t server = NULL;
static void (*save_cb)(const char *, const char *);

static esp_err_t post_handler(httpd_req_t *req) {
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret <= 0) return ESP_FAIL;
    char ssid[32] = {0}, pass[64] = {0};
    sscanf(buf, "ssid=%31[^&]&pass=%63s", ssid, pass);
    ESP_LOGI(TAG, "Received SSID: %s, PASS: %s", ssid, pass);
    if(strlen(ssid) == 0 || strlen(pass) == 0) {
        httpd_resp_send(req, "Invalid input", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    if (save_cb) save_cb(ssid, pass);

    httpd_resp_send(req, "Credentials saved. Rebooting...", HTTPD_RESP_USE_STRLEN);
    esp_restart();
    return ESP_OK;
}

static esp_err_t get_handler(httpd_req_t *req) {
    const char *html =
        "<form method=\"POST\">"
        "SSID: <input name=\"ssid\"><br>"
        "PASS: <input name=\"pass\" type=\"password\"><br>"
        "<input type=\"submit\">"
        "</form>";
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void web_server_start(void (*save_fn)(const char *, const char *)) {
    save_cb = save_fn;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_start(&server, &config);

    httpd_uri_t get_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = get_handler
    };
    httpd_register_uri_handler(server, &get_uri);

    httpd_uri_t post_uri = {
        .uri = "/",
        .method = HTTP_POST,
        .handler = post_handler
    };
    httpd_register_uri_handler(server, &post_uri);
}

void web_server_stop(void) {
    if (server) httpd_stop(server);
}