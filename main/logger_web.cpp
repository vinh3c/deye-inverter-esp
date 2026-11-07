#include "logger_web.h"
#include "logger_config.h"
#include "logger_form.h"
#include "html_template.h"

#include <cstdio>
#include <cstring>
#include <string_view>
#include <atomic>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <nvs.h>

extern const char logger_html[] asm("_binary_logger_config_html_start");

namespace logger_web {
namespace {
constexpr char TAG[] = "logger_web";
httpd_handle_t server = nullptr;
std::atomic<bool> running{false};
char csrf_token[33]{};

void headers(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
        "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'");
}

esp_err_t reject(httpd_req_t *request, httpd_err_code_t code, const char *message)
{
    headers(request);
    httpd_resp_set_hdr(request, "Connection", "close");
    httpd_resp_send_err(request, code, message);
    // Close after rejecting so unread request bytes cannot become a second request.
    return ESP_FAIL;
}

esp_err_t page(httpd_req_t *request, const char *notice = "", bool failed = false)
{
    logger_config::Settings settings{};
    const auto error = logger_config::get(settings);
    if (error != ESP_OK) {
        settings = {};
        settings.port = 502;
        if (!*notice) {
            notice = error == ESP_ERR_NVS_NOT_FOUND ? "No logger configured. Enter a host and port to get started." :
                                                    "Could not read the saved configuration. Try reloading the page.";
            failed = error != ESP_ERR_NVS_NOT_FOUND;
            if (failed) httpd_resp_set_status(request, HTTPD_500);
        }
    }
    char port[6];
    std::snprintf(port, sizeof(port), "%u", settings.port);
    headers(request);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    // Stream the flash-resident template: copying/replacing the whole HTML can
    // require a contiguous allocation larger than the C3's remaining heap.
    const Replacement replacements[] = {
        {"{{HOST}}", settings.host}, {"{{PORT}}", port},
        {"{{TOKEN}}", csrf_token}, {"{{NOTICE}}", nullptr},
    };
    const auto emit = [&](const char *data, size_t length) {
        return httpd_resp_send_chunk(request, data, length) == ESP_OK;
    };
    const auto emit_notice = [&]() {
        if (!*notice) return true;
        const char *open = failed ? "<p role=\"status\" class=\"notice error\">" :
                                    "<p role=\"status\" class=\"notice\">";
        return emit(open, std::strlen(open)) && emit(notice, std::strlen(notice)) && emit("</p>", 4);
    };
    // Stored host is validated ASCII text and cannot contain HTML.
    if (!stream_template(logger_html, replacements, 4, emit, emit_notice)) return ESP_FAIL;
    return httpd_resp_send_chunk(request, nullptr, 0);
}

esp_err_t get_page(httpd_req_t *request) { return page(request); }

esp_err_t save(httpd_req_t *request)
{
    if (request->content_len == 0 || request->content_len > logger_form::max_body)
        return reject(request, HTTPD_413_CONTENT_TOO_LARGE, "Form too large or empty");
    char content_type[80];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type, sizeof(content_type)) != ESP_OK)
        return reject(request, HTTPD_400_BAD_REQUEST, "Expected a form");
    const char *parameters = std::strchr(content_type, ';');
    const std::string_view type(content_type, parameters ? parameters - content_type : std::strlen(content_type));
    if (type != "application/x-www-form-urlencoded")
        return reject(request, HTTPD_400_BAD_REQUEST, "Unsupported form encoding");
    char body[logger_form::max_body];
    size_t received = 0;
    const auto deadline = esp_timer_get_time() + 5000000;
    while (received < request->content_len) {
        if (esp_timer_get_time() >= deadline)
            return reject(request, HTTPD_408_REQ_TIMEOUT, "Request timed out");
        const int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT)
            return reject(request, HTTPD_408_REQ_TIMEOUT, "Request timed out");
        if (count <= 0) return ESP_FAIL;
        received += count;
    }
    logger_form::Values values{};
    const auto result = logger_form::parse(std::string_view(body, received), csrf_token, values);
    if (result == logger_form::Result::Forbidden)
        return reject(request, HTTPD_403_FORBIDDEN, "Reload the form and try again");
    if (result != logger_form::Result::Ok) {
        httpd_resp_set_status(request, HTTPD_400);
        return page(request, "Invalid input. Host must be 1-63 letters, digits, dots or hyphens; port must be 1-65535.", true);
    }
    const auto error = logger_config::set(values.host, values.port);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Cannot save logger config: %s", esp_err_to_name(error));
        httpd_resp_set_status(request, HTTPD_500);
        return page(request, "Could not save the configuration. Please try again.", true);
    }
    ESP_LOGI(TAG, "Logger configuration committed and read back via web");
    return page(request, "Configuration saved. The new settings take effect on the next polling cycle.");
}
} // namespace

void log_address()
{
    if (!running) return;
    auto *interface = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info{};
    if (interface && esp_netif_get_ip_info(interface, &info) == ESP_OK && info.ip.addr)
        ESP_LOGI(TAG, "Logger form: http://" IPSTR "/logger", IP2STR(&info.ip));
}

void start()
{
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i)
        std::snprintf(csrf_token + i * 2, 3, "%02x", random[i]);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    auto error = httpd_start(&server, &config);
    if (error == ESP_OK) {
        httpd_uri_t root{};
        root.uri = "/"; root.method = HTTP_GET; root.handler = get_page;
        httpd_uri_t form = root;
        form.uri = "/logger";
        httpd_uri_t submit = form;
        submit.method = HTTP_POST; submit.handler = save;
        error = httpd_register_uri_handler(server, &root);
        if (error == ESP_OK) error = httpd_register_uri_handler(server, &form);
        if (error == ESP_OK) error = httpd_register_uri_handler(server, &submit);
        if (error != ESP_OK) {
            httpd_stop(server);
            server = nullptr;
        }
    }
    if (error != ESP_OK) ESP_LOGE(TAG, "Cannot start logger form: %s; console configuration remains available", esp_err_to_name(error));
    else {
        running = true;
        ESP_LOGI(TAG, "Logger form listening on HTTP port 80");
        log_address();
    }
}
} // namespace logger_web
