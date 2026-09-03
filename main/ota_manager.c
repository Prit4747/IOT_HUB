/*
 * ota_manager.c -- Full .bin OTA and delta .patch OTA over HTTPS/HTTP.
 *
 * Full OTA  : esp_https_ota
 * Delta OTA : espressif/esp_delta_ota (detools sequential + heatshrink patch)
 *
 * Patch file layout (matches tools/make_patch.py):
 *   [64 B header: magic + partition SHA256 + reserved] + [detools patch body]
 * The 64-byte header is verified then stripped; only the body goes to
 * esp_delta_ota_feed_patch().
 *
 * Merged from the two originals (USB Modem's plain-C version and
 * Quectel_EC200U's C++ port), keeping both real improvements the C++
 * version added on top of the base C project:
 *   - 3x connect retry (both full and delta OTA) -- cellular DNS/TLS
 *     handshakes are flaky enough that one failed attempt shouldn't abort
 *     the whole OTA.
 *   - Progress reported in 5% buckets instead of every 1% change -- avoids
 *     ~100 event pushes per transfer competing with a modem UART/USB task
 *     for CPU during the exact window OTA can least afford it.
 *   - An optional pre-reboot hook (ota_manager_set_pre_reboot_hook) so a
 *     modem backend can leave the link in a clean state before esp_restart().
 * generic/plain C here (this project's main.c is C, unlike the Quectel
 * project's C++), so none of the C++-only workarounds in that version
 * (declaring locals before a `goto`, `{}`-init instead of designated
 * initializers) are needed -- this uses the simpler original C style.
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "esp_delta_ota.h"

#include "ota_manager.h"

static const char *TAG = "OTA";

#define OTA_HTTP_RX_BUF      4096
#define OTA_HTTP_TX_BUF      4096
#define PATCH_HEADER_SIZE    64
#define PATCH_MAGIC          0xfccdde10
#define PATCH_DIGEST_SIZE    32
#define IMG_HEADER_LEN       sizeof(esp_image_header_t)
#define OTA_CONNECT_RETRIES  3

/* Reporting every 1% change pushes ~100 event sends over the course of a
 * single OTA transfer. Bucketing to 5% steps cuts that by ~5x while still
 * giving smooth-looking progress. 100% is always reported so the "Done"
 * transition is never skipped by the bucketing. */
#define OTA_PROGRESS_STEP_PCT 5

static volatile ota_state_t s_state = OTA_STATE_IDLE;
static char s_url[512];
static ota_event_cb_t s_event_cb = NULL;
static ota_pre_reboot_hook_t s_pre_reboot_hook = NULL;

/* ------------------------------------------------------------------ */

static void ota_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", buf);

    if (s_event_cb) {
        char event[300];
        snprintf(event, sizeof(event), "{\"type\":\"log\",\"msg\":\"OTA: %s\"}", buf);
        s_event_cb(event);
    }
}

static void ota_progress(int pct, const char *label)
{
    if (!s_event_cb) {
        return;
    }
    char event[128];
    snprintf(event, sizeof(event),
             "{\"type\":\"progress\",\"pct\":%d,\"label\":\"%s\"}", pct, label);
    s_event_cb(event);
}

static bool progress_step_changed(int pct, int *last_reported)
{
    int bucket = (pct >= 100) ? 100 : (pct / OTA_PROGRESS_STEP_PCT) * OTA_PROGRESS_STEP_PCT;
    if (bucket == *last_reported) {
        return false;
    }
    *last_reported = bucket;
    return true;
}

static bool url_is_delta_patch(const char *url)
{
    if (!url) {
        return false;
    }
    const char *dot = strrchr(url, '.');
    if (!dot) {
        return false;
    }
    /* Ignore query string: foo.patch?token=abc */
    char ext[16];
    size_t n = 0;
    while (dot[n + 1] && dot[n + 1] != '?' && dot[n + 1] != '#' && n + 1 < sizeof(ext)) {
        ext[n] = (char)tolower((unsigned char)dot[n + 1]);
        n++;
    }
    ext[n] = '\0';
    return (strcmp(ext, "patch") == 0);
}

/* ------------------------------------------------------------------ */
/*  Full binary OTA                                                     */
/* ------------------------------------------------------------------ */

static esp_err_t run_full_ota(const char *url)
{
    ota_log("Full OTA starting: %s", url);

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 30000,
        .keep_alive_enable = true,
        .buffer_size       = OTA_HTTP_RX_BUF,
        .buffer_size_tx    = OTA_HTTP_TX_BUF,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = ESP_FAIL;

    /* DNS/connect over cellular is flaky enough (a dropped DNS round-trip,
     * transient PPP hiccups) that a single failed attempt here shouldn't
     * abort the whole OTA. esp_https_ota_perform() failures further down
     * are NOT retried -- a corrupt/aborted image download is a different
     * failure class from "couldn't even open the connection". */
    for (int attempt = 1; attempt <= OTA_CONNECT_RETRIES; attempt++) {
        err = esp_https_ota_begin(&ota_cfg, &handle);
        if (err == ESP_OK) {
            break;
        }
        ota_log("esp_https_ota_begin failed (attempt %d/%d): %s",
                attempt, OTA_CONNECT_RETRIES, esp_err_to_name(err));
        if (attempt < OTA_CONNECT_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    if (err != ESP_OK) {
        return err;
    }

    esp_app_desc_t app_desc;
    if (esp_https_ota_get_img_desc(handle, &app_desc) == ESP_OK) {
        ota_log("New firmware: %s %s", app_desc.project_name, app_desc.version);
    }

    int last_pct = -1;
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int image_len = esp_https_ota_get_image_len_read(handle);
        int total     = esp_https_ota_get_image_size(handle);
        if (total > 0) {
            int pct = (image_len * 100) / total;
            if (progress_step_changed(pct, &last_pct)) {
                char lbl[32];
                snprintf(lbl, sizeof(lbl), "%d / %d KB", image_len / 1024, total / 1024);
                ota_progress(last_pct, lbl);
            }
        }
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        ota_log("Incomplete data received");
        esp_https_ota_abort(handle);
        return ESP_FAIL;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ota_log("esp_https_ota_finish: %s", esp_err_to_name(err));
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ota_log("Image validation failed -- wrong chip or binary?");
        }
        return err;
    }

    ota_progress(100, "Done");
    ota_log("Full OTA complete -- rebooting");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Delta OTA                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    esp_ota_handle_t       ota;
    const esp_partition_t *src_part;
    uint8_t                img_hdr[IMG_HEADER_LEN];
    int                    img_hdr_len;
    bool                   chip_verified;
} delta_ctx_t;

static esp_err_t delta_write_cb(const uint8_t *buf, size_t len, void *user_data)
{
    delta_ctx_t *ctx = (delta_ctx_t *)user_data;
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int index = 0;

    if (!ctx->chip_verified) {
        int need = IMG_HEADER_LEN - ctx->img_hdr_len;
        int take = (int)len < need ? (int)len : need;

        memcpy(ctx->img_hdr + ctx->img_hdr_len, buf, (size_t)take);
        ctx->img_hdr_len += take;

        if (ctx->img_hdr_len < IMG_HEADER_LEN) {
            return ESP_OK;
        }

        esp_image_header_t *ih = (esp_image_header_t *)ctx->img_hdr;
        if (ih->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
            ota_log("Patch targets wrong chip (id=%d, expected=%d)",
                    ih->chip_id, CONFIG_IDF_FIRMWARE_CHIP_ID);
            return ESP_ERR_INVALID_VERSION;
        }
        ctx->chip_verified = true;

        esp_err_t err = esp_ota_write(ctx->ota, ctx->img_hdr, IMG_HEADER_LEN);
        if (err != ESP_OK) {
            return err;
        }
        index = take;
    }

    if (index < (int)len) {
        return esp_ota_write(ctx->ota, buf + index, len - (size_t)index);
    }
    return ESP_OK;
}

static esp_err_t delta_read_cb(uint8_t *buf, size_t size, int offset, void *user_data)
{
    delta_ctx_t *ctx = (delta_ctx_t *)user_data;
    return esp_partition_read(ctx->src_part, (size_t)offset, buf, size);
}

/* Parse ".../vA_to_vB.patch" filenames used by our release assets. */
static bool parse_delta_patch_versions(const char *url,
                                       char *from, size_t from_sz,
                                       char *to, size_t to_sz)
{
    if (!url || !from || !to || from_sz < 2 || to_sz < 2) {
        return false;
    }
    const char *slash = strrchr(url, '/');
    const char *name = slash ? slash + 1 : url;
    const char *sep = strstr(name, "_to_");
    if (!sep) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (!dot || strcmp(dot, ".patch") != 0) {
        return false;
    }
    size_t from_len = (size_t)(sep - name);
    size_t to_len = (size_t)(dot - (sep + 4));
    if (from_len == 0 || to_len == 0 || from_len >= from_sz || to_len >= to_sz) {
        return false;
    }
    memcpy(from, name, from_len);
    from[from_len] = '\0';
    memcpy(to, sep + 4, to_len);
    to[to_len] = '\0';
    return true;
}

static bool verify_patch_header(const uint8_t *hdr, const char *url)
{
    uint32_t magic = *(const uint32_t *)hdr;
    if (magic != PATCH_MAGIC) {
        ota_log("Invalid patch magic 0x%08" PRIx32 " (need 0x%08x)", magic, PATCH_MAGIC);
        return false;
    }

    uint8_t part_sha[PATCH_DIGEST_SIZE] = {0};
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_partition_get_sha256(running, part_sha) != ESP_OK) {
        ota_log("Could not hash running partition");
        return false;
    }

    const uint8_t *patch_sha = hdr + 4;
    if (memcmp(part_sha, patch_sha, PATCH_DIGEST_SIZE) != 0) {
        const esp_app_desc_t *app = esp_app_get_description();
        const char *ver = (app && app->version[0]) ? app->version : "?";

        ota_log("REASON: delta base/version mismatch");
        ota_log("Device running: %s", ver);
        ota_log("Patch SHA does not match running partition SHA (wrong base image)");
        ota_log("Device SHA: %02x%02x%02x%02x...  Patch expects: %02x%02x%02x%02x...",
                part_sha[0], part_sha[1], part_sha[2], part_sha[3],
                patch_sha[0], patch_sha[1], patch_sha[2], patch_sha[3]);

        char from[64] = {0};
        char to[64] = {0};
        if (parse_delta_patch_versions(url, from, sizeof(from), to, sizeof(to))) {
            ota_log("Patch filename: upgrade %s -> %s", from, to);
            if (strcmp(from, ver) != 0) {
                ota_log("Wrong base version: patch requires %s, device has %s", from, ver);
            }
            if (strcmp(to, ver) == 0) {
                ota_log("Device is already at target %s -- this patch cannot be re-applied", to);
            } else {
                ota_log("Use patch built from exact .bin of %s, or use full .bin OTA", ver);
            }
        } else {
            ota_log("Delta must be built from the exact .bin currently on device (%s)", ver);
            ota_log("Rebuild: tools/make_patch.py --base <running.bin> --new <new.bin>");
        }
        return false;
    }

    ota_log("Patch header OK for running firmware (%s)", running->label);
    return true;
}

static esp_err_t http_open_following_redirects(esp_http_client_handle_t client,
                                               int *content_len_out)
{
    for (int hop = 0; hop < 5; hop++) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ota_log("http_client_open: %s", esp_err_to_name(err));
            return err;
        }

        int clen   = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 200) {
            *content_len_out = clen;
            return ESP_OK;
        }

        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            ota_log("HTTP %d -- following redirect", status);
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }

        ota_log("HTTP %d -- server refused the request", status);
        esp_http_client_close(client);
        return ESP_FAIL;
    }

    ota_log("Too many redirects");
    return ESP_FAIL;
}

static esp_err_t http_read_exact(esp_http_client_handle_t client,
                                 uint8_t *buf, int want)
{
    int got = 0;
    while (got < want) {
        int rd = esp_http_client_read(client, (char *)(buf + got), want - got);
        if (rd < 0) {
            ota_log("HTTP read error");
            return ESP_FAIL;
        }
        if (rd == 0) {
            ota_log("Connection closed early (%d/%d bytes)", got, want);
            return ESP_FAIL;
        }
        got += rd;
    }
    return ESP_OK;
}

static esp_err_t run_delta_ota(const char *url)
{
    ota_log("Delta OTA starting: %s", url);

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running_part = esp_ota_get_running_partition();
    if (!update_part || !running_part) {
        ota_log("No OTA partition available");
        return ESP_FAIL;
    }
    ota_log("Source: %s  Target: %s", running_part->label, update_part->label);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ota_log("esp_ota_begin: %s", esp_err_to_name(err));
        return err;
    }

    delta_ctx_t ctx = {
        .ota      = ota_handle,
        .src_part = running_part,
    };

    esp_delta_ota_cfg_t dcfg = {
        .user_data               = &ctx,
        .read_cb_with_user_data  = delta_read_cb,
        .write_cb_with_user_data = delta_write_cb,
    };
    esp_delta_ota_handle_t delta = esp_delta_ota_init(&dcfg);
    if (!delta) {
        ota_log("esp_delta_ota_init failed");
        esp_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 60000,
        .buffer_size       = OTA_HTTP_RX_BUF,
        .buffer_size_tx    = OTA_HTTP_TX_BUF,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ota_log("http_client_init failed");
        esp_delta_ota_deinit(delta);
        esp_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    int content_len = 0;

    /* Same 3x connect-retry rationale as run_full_ota() -- delta OTA had no
     * cushion at all against a transient connect failure before this,
     * unlike full OTA. esp_http_client_set_redirection() (inside
     * http_open_following_redirects) updates the client's URL in place, so
     * retrying just re-opens against wherever the previous attempt left
     * off. */
    for (int attempt = 1; attempt <= OTA_CONNECT_RETRIES; attempt++) {
        err = http_open_following_redirects(client, &content_len);
        if (err == ESP_OK) {
            break;
        }
        ota_log("connect failed (attempt %d/%d)", attempt, OTA_CONNECT_RETRIES);
        if (attempt < OTA_CONNECT_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    if (err != ESP_OK) {
        goto cleanup;
    }
    ota_log("Download size: %d bytes", content_len);

    uint8_t patch_hdr[PATCH_HEADER_SIZE];
    err = http_read_exact(client, patch_hdr, PATCH_HEADER_SIZE);
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (!verify_patch_header(patch_hdr, url)) {
        err = ESP_ERR_INVALID_VERSION;
        goto cleanup;
    }

    int body_total = (content_len > PATCH_HEADER_SIZE)
                     ? (content_len - PATCH_HEADER_SIZE) : 0;
    int body_read  = 0;
    int last_pct   = -1;

    uint8_t buf[1024];
    while (1) {
        int rd = esp_http_client_read(client, (char *)buf, sizeof(buf));
        if (rd < 0) {
            err = ESP_FAIL;
            ota_log("Read error");
            break;
        }
        if (rd == 0) {
            if (!esp_http_client_is_complete_data_received(client)) {
                err = ESP_FAIL;
                ota_log("Incomplete patch (%d body bytes)", body_read);
            }
            break;
        }

        body_read += rd;
        err = esp_delta_ota_feed_patch(delta, buf, rd);
        if (err != ESP_OK) {
            ota_log("esp_delta_ota_feed_patch: %s", esp_err_to_name(err));
            ota_log("Patch must be detools+heatshrink (see tools/make_patch.py)");
            break;
        }

        if (body_total > 0) {
            int pct = (body_read * 100) / body_total;
            if (progress_step_changed(pct, &last_pct)) {
                char lbl[32];
                snprintf(lbl, sizeof(lbl), "%d / %d KB", body_read / 1024, body_total / 1024);
                ota_progress(last_pct, lbl);
            }
        }
    }

    if (err == ESP_OK) {
        err = esp_delta_ota_finalize(delta);
        if (err != ESP_OK) {
            ota_log("esp_delta_ota_finalize: %s", esp_err_to_name(err));
        }
    }

cleanup:
    esp_http_client_cleanup(client);
    esp_delta_ota_deinit(delta);

    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        return err;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ota_log("esp_ota_end: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ota_log("esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return err;
    }

    ota_progress(100, "Done");
    ota_log("Delta OTA complete -- rebooting");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Task                                                                */
/* ------------------------------------------------------------------ */

/* At least one backend's modem_ppp_stop() (the SimCom/USB one, wrapping
 * usbh_modem_ppp_stop()) is documented -- by that backend's own code -- as
 * able to block forever if the hang-up AT command never gets a reply
 * (ATH sent while still in USB data mode). Calling the pre-reboot hook
 * straight from ota_task() would mean that hang could silently swallow
 * esp_restart() and never actually reboot after a successful OTA. Running
 * it in its own task and only waiting a bounded time for it here means a
 * stuck hook can never block the reboot -- worst case, the hook's cleanup
 * doesn't finish and the reboot proceeds anyway, which is the same
 * outcome esp_restart() would force a moment later regardless. */
#define PRE_REBOOT_HOOK_TIMEOUT_MS 5000

typedef struct {
    ota_pre_reboot_hook_t hook;
    SemaphoreHandle_t      done;
} pre_reboot_ctx_t;

static void pre_reboot_hook_task(void *arg)
{
    pre_reboot_ctx_t *ctx = (pre_reboot_ctx_t *)arg;
    ctx->hook();
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static void run_pre_reboot_hook_bounded(void)
{
    ota_log("Running pre-reboot hook (bounded to %d ms)...", PRE_REBOOT_HOOK_TIMEOUT_MS);

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        ota_log("Pre-reboot hook: no memory for semaphore, skipping");
        return;
    }

    static pre_reboot_ctx_t ctx;
    ctx.hook = s_pre_reboot_hook;
    ctx.done = done;

    if (xTaskCreate(pre_reboot_hook_task, "ota_prereboot", 4096, &ctx, 5, NULL) != pdPASS) {
        ota_log("Pre-reboot hook: task create failed, skipping");
        vSemaphoreDelete(done);
        return;
    }

    if (xSemaphoreTake(done, pdMS_TO_TICKS(PRE_REBOOT_HOOK_TIMEOUT_MS)) == pdTRUE) {
        vSemaphoreDelete(done);
    } else {
        /* Deliberately NOT deleting `done` or otherwise cleaning up here --
         * the still-running hook task might touch it later. Harmless: the
         * whole system reboots within seconds either way (see ota_task()
         * below), which reclaims everything regardless. */
        ota_log("Pre-reboot hook did not finish in time -- rebooting anyway");
    }
}

static void ota_task(void *arg)
{
    (void)arg;
    bool is_delta = url_is_delta_patch(s_url);
    ota_log("Mode: %s", is_delta ? "delta (.patch)" : "full (.bin)");
    esp_err_t err = is_delta ? run_delta_ota(s_url) : run_full_ota(s_url);

    if (err == ESP_OK) {
        s_state = OTA_STATE_DONE;

        /* Best-effort: OTA has already succeeded at this point regardless
         * of whether this hook does anything useful. See ota_manager.h --
         * a modem backend uses this to leave the link in a clean state
         * (e.g. hang up PPP) before the reboot below. */
        if (s_pre_reboot_hook) {
            run_pre_reboot_hook_bounded();
        }

        if (s_event_cb) {
            s_event_cb("{\"type\":\"reboot\",\"secs\":3}");
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else {
        if (err == ESP_ERR_INVALID_VERSION) {
            ota_log("FAILED: ESP_ERR_INVALID_VERSION -- delta patch base/version mismatch (see REASON above)");
        } else {
            ota_log("FAILED: %s", esp_err_to_name(err));
        }
        s_state = OTA_STATE_FAILED;
        if (s_event_cb) {
            s_event_cb("{\"type\":\"ota_failed\"}");
        }
    }

    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void ota_manager_init(void)
{
    s_state = OTA_STATE_IDLE;
}

void ota_manager_set_event_cb(ota_event_cb_t cb)
{
    s_event_cb = cb;
}

void ota_manager_set_pre_reboot_hook(ota_pre_reboot_hook_t hook)
{
    s_pre_reboot_hook = hook;
}

esp_err_t ota_manager_start(const char *url)
{
    if (s_state == OTA_STATE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!url || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    s_state = OTA_STATE_RUNNING;

    /* Delta patching needs more stack than plain HTTPS OTA */
    if (xTaskCreate(ota_task, "ota_task", 12288, NULL, 5, NULL) != pdPASS) {
        s_state = OTA_STATE_IDLE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

ota_state_t ota_manager_get_state(void)
{
    return s_state;
}
