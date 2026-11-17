#ifndef R1SYNCAPP_H
#define R1SYNCAPP_H

#include <stdbool.h>

// r1sync (https://github.com/chrsgrhm/r1sync), run from the More page: it
// keeps the card in step with an Audiobookshelf or Navidrome server, from a
// web page on the phone, while the player waits on its own page.
//
// It is a separate, static binary: /usr/bin/r1sync when the image carries it,
// else .r1sync/r1sync on the card (copied to /tmp first, since the card may be
// FAT and mounted noexec). Started as `r1sync -s -m <mount> -p 80`; it writes
// its status for the page to R1SYNC_STATUS_FILE and exits when the web page's
// Close, its idle timer, or r1syncapp_stop() ends it.

#define R1SYNC_STATUS_FILE "/tmp/r1sync-status.json"

typedef struct {
	char version[32];
	char ip[16];
	char pin[8];
	char abs[64];	 // "Audiobookshelf: downloading 45%", "" when it is not set up
	char nd[64];	 // "Navidrome: song 3 of 16", ""
	char notice[64]; // "Closing r1sync in 42 s", ""
} r1syncapp_status_t;

// Whether there is an r1sync to run, for the card mounted at sd_root.
bool r1syncapp_available(const char *sd_root);

// Start it, for the card mounted at sd_root ("/mnt/sd_0"). False when it is
// already running or cannot be started.
bool r1syncapp_start(const char *sd_root);

// Whether it is still running; reaps it once it has exited.
bool r1syncapp_running(void);

// Whether one has been started and not yet seen to exit: a flag, safe from
// any thread (the power code asks it, to keep Wi-Fi up and the device awake).
bool r1syncapp_active(void);

// Ask it to finish (SIGTERM), without waiting: r1syncapp_running() says when it
// has. r1syncapp_kill() for one that has not after R1SYNC_STOP_WAIT_MS.
void r1syncapp_ask_stop(void);
void r1syncapp_kill(void);

// Ask, and wait (SIGKILL after R1SYNC_STOP_WAIT_MS): for leaving in a hurry.
void r1syncapp_stop(void);

// It stops its downloads and its web server, which can take a few seconds
// with a transfer in flight.
#define R1SYNC_STOP_WAIT_MS 10000

// The last status it wrote. False when there is none yet.
bool r1syncapp_status(r1syncapp_status_t *out);

#endif
