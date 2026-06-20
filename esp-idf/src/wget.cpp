/**
 * wget — CLI download command.
 *
 *   wget <url>            fetch an http(s) URL, save to the URL's basename in cwd
 *   wget -O <file> <url>  fetch to a specific file (resolved against cwd)
 *
 * Uses ESP-IDF's esp_http_client with TLS via the IDF cert bundle (the platform
 * has no shared HTTP-client wrapper; acme/duckdns/ota/viewer use it directly the
 * same way). Follows redirects.
 *
 * A TLS handshake needs more stack than the 6 KB CLI task has, so the transfer
 * runs on a dedicated PSRAM-stack worker; the command blocks until it completes
 * (the esp_http_client timeout bounds the wait) and reports the result.
 */
#include "spangap.h"   /* cli.h, log.h, spawnTask */
#include "mem.h"       /* STACK_PSRAM */
#include "fs.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string>
#include <vector>
#include <cstring>

namespace {

struct WgetJob {
    std::string       url;
    std::string       outPath;
    int               file   = -1;
    size_t            bytes  = 0;
    int               status = 0;
    bool              ok     = false;
    std::string       err;
    SemaphoreHandle_t done   = nullptr;
};

bool is2xx(esp_http_client_handle_t c) {
    int s = esp_http_client_get_status_code(c);
    return s >= 200 && s < 300;
}

/* Stream the final (2xx) response body to disk — skip redirect-hop bodies. */
esp_err_t onEvent(esp_http_client_event_t* e) {
    WgetJob* j = (WgetJob*)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && j->file >= 0 && is2xx(e->client)) {
        size_t w = fs_write(e->data, 1, e->data_len, j->file);
        j->bytes += w;
        if (w != (size_t)e->data_len && j->err.empty()) j->err = "write failed (disk full?)";
    }
    return ESP_OK;
}

void wgetWorker(void* arg) {
    WgetJob* j = (WgetJob*)arg;

    j->file = fs_open(j->outPath.c_str(), "wb");
    if (j->file < 0) { j->err = "cannot create " + j->outPath; xSemaphoreGive(j->done); vTaskDelete(nullptr); return; }

    esp_http_client_config_t cfg = {};
    cfg.url                   = j->url.c_str();
    cfg.event_handler         = onEvent;
    cfg.user_data             = j;
    cfg.timeout_ms            = 30000;
    cfg.crt_bundle_attach     = esp_crt_bundle_attach;
    cfg.user_agent            = "spangap-wget/0.1";
    cfg.max_redirection_count = 10;

    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) { j->err = "client init failed"; fs_close(j->file); j->file = -1; xSemaphoreGive(j->done); vTaskDelete(nullptr); return; }

    esp_err_t err = esp_http_client_perform(cl);
    j->status = esp_http_client_get_status_code(cl);
    esp_http_client_cleanup(cl);
    fs_close(j->file); j->file = -1;

    if (err != ESP_OK)                          j->err = esp_err_to_name(err);
    else if (j->status < 200 || j->status >= 300) j->err = "HTTP " + std::to_string(j->status);
    else if (j->err.empty())                    j->ok = true;

    if (!j->ok) fs_remove(j->outPath.c_str());  /* don't leave a partial/empty file */

    xSemaphoreGive(j->done);
    vTaskDelete(nullptr);
}

/* basename after the last '/', minus any ?query / #fragment. */
std::string urlBasename(const std::string& url) {
    size_t scheme = url.find("://");
    size_t start  = scheme == std::string::npos ? 0 : scheme + 3;
    size_t cut    = url.find_first_of("?#", start);
    std::string path = url.substr(start, cut == std::string::npos ? std::string::npos : cut - start);
    size_t slash = path.find_last_of('/');
    std::string base = slash == std::string::npos ? "" : path.substr(slash + 1);
    return base.empty() ? "index.html" : base;
}

void cmdWget(const char* args) {
    if (!args || !*args || cliWantsHelp(args)) {
        cliPrintf("%-*s download a URL (saves to its basename in the current dir)\n",
                  CLI_HELP_COL, "wget <url>");
        cliPrintf("%-*s download to a specific file\n", CLI_HELP_COL, "wget -O <file> <url>");
        return;
    }

    /* tokenize on whitespace: -O <file> anywhere, first bare token is the URL */
    std::vector<std::string> toks;
    { std::string cur; for (const char* p = args; ; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\0') { if (!cur.empty()) { toks.push_back(cur); cur.clear(); } if (!*p) break; }
        else cur.push_back(*p);
    } }
    std::string url, ofile;
    for (size_t i = 0; i < toks.size(); i++) {
        if (toks[i] == "-O" && i + 1 < toks.size()) ofile = toks[++i];
        else if (url.empty())                       url = toks[i];
    }
    if (url.empty()) { cliPrintf("wget: no URL\n"); return; }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        cliPrintf("wget: only http(s) URLs\n"); return;
    }

    char outPath[256];
    std::string rel = ofile.empty() ? urlBasename(url) : ofile;
    if (!cliResolveFsPath(rel.c_str(), outPath, sizeof outPath)) {
        cliPrintf("wget: bad output path\n"); return;
    }

    WgetJob* j = new WgetJob();
    j->url = url;
    j->outPath = outPath;
    j->done = xSemaphoreCreateBinary();
    if (!j->done) { cliPrintf("wget: out of memory\n"); delete j; return; }

    cliPrintf("fetching %s ...\n", url.c_str());
    TaskHandle_t t = spawnTask(wgetWorker, "wget", 24576, j, 1, 1, STACK_PSRAM);
    if (!t) { cliPrintf("wget: cannot start worker\n"); vSemaphoreDelete(j->done); delete j; return; }

    /* The worker always completes (esp_http_client timeout bounds it), so a plain
     * wait can't hang — and never freeing j while the worker runs avoids a UAF. */
    xSemaphoreTake(j->done, portMAX_DELAY);
    if (j->ok) cliPrintf("saved %u bytes to %s\n", (unsigned)j->bytes, j->outPath.c_str());
    else       cliPrintf("wget failed: %s\n", j->err.c_str());

    vSemaphoreDelete(j->done);
    delete j;
}

}  // namespace

void wgetRegister() { cliRegisterCmd("wget", cmdWget); }
