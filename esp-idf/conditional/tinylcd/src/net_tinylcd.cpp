/**
 * net_tinylcd.cpp — the network status page on the tiny OLED.
 *
 * The when:-gated tinylcd slice of spangap-net (conditional/tinylcd/): only
 * compiled when spangap/tinylcd is staged. Shows what you need to reach the
 * device — the STA SSID, <hostname>.local (only while the network is up),
 * the STA IP, and the AP's SSID + IP while the AP is up. A double click
 * toggles the master WiFi switch (s.net.wifi.enable). All of it reads live
 * storage keys at draw time; the subscriptions below just turn key changes
 * into redraw requests, which tinylcd drops unless this page is on screen.
 *
 * netTinylcdRegister runs in net's init list — platform band, before the
 * straddle band brings tinylcd's task up. That ordering is fine: page
 * registration is task-free, and tinylcdRun parks the subscription setup
 * until the task starts.
 */
#include "tinylcd.h"

#include <stdio.h>

#include "storage.h"

static tinylcd_page_t s_page = TINYLCD_NO_PAGE;

static bool drawNetPage(tinylcd_page_t, u8g2_t* g, tinylcd_ev_t ev)
{
    bool absorbed = false;
    if (ev == TINYLCD_EV_DOUBLECLICK) {
        /* Toggle the master WiFi switch; net's own subscription on the key
         * runs netUp/netDown, and the lines below catch up through the
         * wifi.* subscriptions as the state settles. The drawing below is
         * the absorbed event's response. */
        storageSet("s.net.wifi.enable",
                   storageGetInt("s.net.wifi.enable", 1) ? 0 : 1);
        absorbed = true;
    } else if (ev != TINYLCD_EV_NONE) {
        return false;   /* other gestures: defaults */
    }

    /* Five fixed lines in the rows-8..55 window, prefixes aligned at column
     * five: SSID / DNS name / STA IP / AP SSID / AP IP. The DNS line only
     * exists while the network is up — a .local name that resolves nowhere
     * is noise.
     *
     * This page is a ladder rather than tinylcd's title-and-body shape, so it
     * places its own baselines — but it starts at the same 15 and steps 10,
     * which puts the first line inside the yellow band of a two-tone panel and
     * the seam at row 16 in the pitch below it. Re-space the ladder and that
     * alignment is what to preserve: no line may straddle row 16. */
    char host[32], line[48];
    u8g2_SetFont(g, u8g2_font_6x10_tf);

    std::string ssid = storageGetStr("wifi.sta.ssid", "");
    snprintf(line, sizeof line, "SSID %s", ssid.empty() ? "-" : ssid.c_str());
    u8g2_DrawStr(g, 0, 15, line);

    bool up = storageGetInt("wifi.sta.up", 0) || storageGetInt("wifi.ap.up", 0);
    storageGetStr("s.net.hostname", host, sizeof host, "");
    if (up && *host) {
        snprintf(line, sizeof line, "DNS  %s.local", host);
        u8g2_DrawStr(g, 0, 25, line);
    }

    std::string ip = storageGetStr("wifi.sta.ip", "");
    snprintf(line, sizeof line, "IP   %s", ip.empty() ? "-" : ip.c_str());
    u8g2_DrawStr(g, 0, 35, line);

    if (storageGetInt("wifi.ap.up", 0)) {
        std::string apssid = storageGetStr("wifi.ap.ssid", "");
        std::string apip   = storageGetStr("wifi.ap.ip", "");
        snprintf(line, sizeof line, "AP   %s", apssid.c_str());
        u8g2_DrawStr(g, 0, 45, line);
        snprintf(line, sizeof line, "     %s", apip.c_str());
        u8g2_DrawStr(g, 0, 55, line);
    }
    return absorbed;
}

void netTinylcdRegister(void)
{
    s_page = tinylcdAddPage("net", drawNetPage);
    tinylcdRun(ON_TINYLCD {
        /* On the tinylcd task (subscriptions dispatch on the subscribing
         * task). wifi.sta.ip is the accurate address signal — there is no
         * dedicated IP-changed net event; wifi.ap covers AP up/down + SSID. */
        storageSubscribeChanges("wifi.sta.ip",    ON_CHANGE { tinylcdDraw(s_page); });
        storageSubscribeChanges("wifi.sta.ssid",  ON_CHANGE { tinylcdDraw(s_page); });
        storageSubscribeChanges("wifi.sta.up",    ON_CHANGE { tinylcdDraw(s_page); });
        storageSubscribeChanges("wifi.ap",        ON_CHANGE { tinylcdDraw(s_page); });
        storageSubscribeChanges("s.net.hostname", ON_CHANGE { tinylcdDraw(s_page); });
    });
}
