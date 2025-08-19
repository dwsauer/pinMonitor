#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t post_handler(httpd_req_t *req);
esp_err_t get_handler(httpd_req_t *req);
void web_server_start(void (*save_fn)(const char *, const char *)) ;
void web_server_stop(void);