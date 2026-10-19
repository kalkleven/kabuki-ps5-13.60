/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * hello-tile: the smallest Kabuki provider. One `list` tile with a counter row and a reset row.
 * Cross on the counter adds one; Cross on Reset sets it back to 0.
 *
 * Kabuki draws the card (scrolling, focus, sounds); this program only sends the rows and hears
 * what was chosen. If Kabuki isn't running, or restarts, it connects again.
 *
 * Stop it by creating /data/hello-tile.stop (delete the file before running it again).
 */
#include "kabuki.h"
#include <unistd.h>

static void rows(kabuki_client* k, int count) {
    char data[256];
    snprintf(data, sizeof data,
             "{\"top\":{\"title\":\"Hello\",\"covers\":false,\"rows\":["
             "{\"key\":\"n\",\"title\":\"Count: %d\",\"subtitle\":\"Press Cross to add one\"},"
             "{\"key\":\"reset\",\"title\":\"Reset\"}]}}", count);
    kabuki_content(k, "HELLOW", data);
}

int main(void) {
    static const kabuki_tile tile = {"HELLOW", "Hello", "icon:iconid_list", "list", 500};
    int count = 0;
    while (access("/data/hello-tile.stop", F_OK)) {
        kabuki_client k;
        if (kabuki_connect(&k) || kabuki_hello(&k, "hello-tile", &tile, 1, NULL, 3000)) {
            kabuki_close(&k);
            sleep(2); /* Kabuki isn't there (yet): try again */
            continue;
        }
        rows(&k, count);
        kabuki_status(&k, "Counting");
        kabuki_event ev;
        int got;
        while ((got = kabuki_poll(&k, &ev, 1000)) >= 0) {
            if (got == 1 && ev.kind == KABUKI_ACTION && !strcmp(ev.action, "select")) {
                count = !strcmp(ev.key, "reset") ? 0 : count + 1;
                rows(&k, count);
            } else if (got == 1 && ev.kind == KABUKI_ERROR) {
                fprintf(stderr, "kabuki: %s: %s\n", ev.code, ev.message);
            }
            if (!access("/data/hello-tile.stop", F_OK)) break;
        }
        kabuki_close(&k);
    }
    return 0;
}
