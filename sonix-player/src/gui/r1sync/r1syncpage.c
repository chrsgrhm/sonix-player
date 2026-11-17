#include "r1syncpage.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/gui/wireless/wifitransfer.h"
#include "src/system/audio/audio.h"
#include "src/system/core/lang.h"
#include "src/system/device/led.h"
#include "src/system/device/system.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/library/library.h"
#include "src/system/net/mdns.h"
#include "src/system/net/r1syncapp.h"
#include "src/system/net/wifi.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/radio.h"
#include "src/system/streaming/streamturn.h"

// The r1sync page, opened from More. While it is open r1sync has the card:
// playback is paused, the Wi-Fi transfer server is off (r1sync serves its own
// page on the same port, with a file manager of its own), and when it closes
// the card is looked over again -- music and audiobooks -- for what it added
// and removed.
//
// The page is only the screen: the address to open, the PIN, and a line per
// server, all read from the status file r1sync writes. Everything else is on
// the web page.

lv_obj_t *r1syncpage_screen;

#define POLL_MS 500

// What r1sync answers to on the network: http://r1sync.local
#define R1SYNC_MDNS_NAME "r1sync"

typedef enum {
	ST_IDLE,	// not started: no Wi-Fi, or no r1sync
	ST_RUNNING, // r1sync is up
	ST_CLOSING, // asked to stop; waiting for it
} state_t;

static state_t state;
// Sonix's audiobook index is let go while r1sync runs: r1sync writes positions,
// finished books and bookmarks into it (Audiobookshelf's progress sync).
static bool books_released;
static long closing_since;
static bool leaving;

static lv_obj_t *status_label;
static lv_obj_t *url_label;
static lv_obj_t *addr_label;
static lv_obj_t *pin_label;
static lv_obj_t *abs_label;
static lv_obj_t *nd_label;
static lv_obj_t *notice_label;
static lv_obj_t *wifi_row;
static lv_obj_t *close_row;
static lv_timer_t *poll_timer;

// The card, as the rest of the interface knows it (on the device the mount
// point storage found; in the simulator the folder standing in for it).
static const char *sd_root_path;
static const char *card_root(void) { return sd_root_path; }

static void hide(lv_obj_t *obj) { lv_obj_set_hidden(obj, true); }
static void show(lv_obj_t *obj) { lv_obj_set_hidden(obj, false); }

static void set_text(lv_obj_t *label, const char *text) {
	const char *shown = lv_label_get_text(label);
	if (shown && strcmp(shown, text) == 0) {
		return;
	}
	lv_label_set_text(label, text);
}

// A line that is shown only when it has something to say.
static void set_line(lv_obj_t *label, const char *text) {
	if (text && text[0]) {
		set_text(label, text);
		show(label);
	} else {
		hide(label);
	}
}

static bool wifi_is_connected(void) {
#ifdef HOST_BUILD
	return true; // the simulator: the PC's own network, with no supplicant to ask
#endif
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
}

static void hide_status(void) {
	hide(url_label);
	hide(addr_label);
	hide(pin_label);
	hide(abs_label);
	hide(nd_label);
	hide(notice_label);
}

static void refresh(void) {
	if (state == ST_CLOSING) {
		set_text(status_label, tr("r1sync_closing"));
		hide_status();
		hide(wifi_row);
		hide(close_row);
		return;
	}
	if (state == ST_IDLE) {
		hide_status();
		hide(close_row);
		if (!r1syncapp_available(card_root())) {
			set_text(status_label, tr("r1sync_not_installed"));
			hide(wifi_row);
		} else {
			set_text(status_label, tr("enable_wi_fi_first"));
			show(wifi_row);
		}
		return;
	}

	hide(wifi_row);
	show(close_row);
	// Up once it has written its status. The address is the player's own,
	// what every other network page shows; r1sync's is the fallback.
	r1syncapp_status_t st;
	if (!r1syncapp_status(&st)) {
		set_text(status_label, tr("turning_on"));
		hide_status();
		return;
	}
	wifi_status_t ws;
	wifi_get_status(&ws);
	const char *ip = ws.ip[0] ? ws.ip : st.ip;

	set_text(status_label, tr("r1sync_open_note"));
	set_text(url_label, "r1sync.local");
	show(url_label);
	if (ip[0]) {
		lv_label_set_text_fmt(addr_label, tr("wifitransfer_or"), ip);
		show(addr_label);
	} else {
		hide(addr_label);
	}
	if (st.pin[0]) {
		lv_label_set_text_fmt(pin_label, tr("r1sync_pin"), st.pin);
		show(pin_label);
	} else {
		hide(pin_label);
	}
	set_line(abs_label, st.abs);
	set_line(nd_label, st.nd);
	set_line(notice_label, st.notice);
}

// The card as r1sync left it: what the transfer page does after an upload,
// and the audiobooks too, which Sonix otherwise scans only when asked.
static void card_changed(void) {
	storage_card_files_recheck();
	const char *sd = card_root();
	library_card_returned(sd);
	if (sd) {
		char books[600];
		snprintf(books, sizeof(books), "%s/%s", sd, AUDIOBOOKDB_FOLDER);
		audiobookdb_scan_start(books);
	}
}

// Before r1sync starts: the book being listened to keeps its exact place (the
// player's own save waits for its next poll), then the index is closed, so
// nothing here writes it while r1sync does.
static void release_books(void) {
	if (audiobook_current_book(NULL, 0, NULL, 0)) {
		device_state_t ds;
		device_state_get(&ds);
		audiobook_note_position(ds.progress_current_secs, ds.progress_total_secs, true);
	}
	audiobookdb_close();
	books_released = true;
}

// After r1sync: the index again, and a loaded book where the index now says
// it is -- r1sync may have moved it, and the player would otherwise carry on
// from its own copy and save that over it. Before the rescan, which empties
// the table while it refills it.
static void take_back_books(void) {
	if (!books_released) {
		return;
	}
	books_released = false;
	char path[600];
	snprintf(path, sizeof(path), "%s/.local/audiobooks.db", card_root() ? card_root() : "");
	audiobookdb_open(path);

	char book[512], file[512], resume[512];
	double seconds = 0;
	if (audiobook_current_book(book, sizeof(book), file, sizeof(file)) &&
		audiobook_resume_point(book, resume, sizeof(resume), &seconds)) {
		device_state_t ds;
		device_state_get(&ds);
		if (strcmp(resume, file) != 0 || fabs(seconds - ds.progress_current_secs) > 2) {
			printf("r1sync: %s moved to %.0f s in %s\n", book, seconds, resume);
			player_restore_track(resume, seconds);
		}
	}
}

static void finished(void) {
	state = ST_IDLE;
	mdns_stop();
	led_set_wifi_transfer(false);
	wifi_set_status_poll_slow(false);
	take_back_books();
	card_changed();
}

static void pause_playback(void) {
	if (radio_is_active()) {
		if (radio_is_playing()) {
			radio_stop();
			player_refresh_now_playing();
		}
	} else if (audio_get_status() == AUDIO_STATUS_PLAYING) {
		player_key_play_pause();
	}
	// A Tidal, Qobuz or podcast download in flight is dropped: r1sync is about
	// to write the card.
	streamturn_abandon_all();
}

static void start(void) {
	if (!wifi_is_connected() || !r1syncapp_available(card_root())) {
		state = ST_IDLE;
		return;
	}
	// r1sync takes port 80 and the card; the transfer server must not have
	// either.
	wifitransfer_page_stop();
	pause_playback();
	release_books();
	if (!r1syncapp_start(card_root())) {
		take_back_books();
		gui_notify_popup("r1sync_failed");
		state = ST_IDLE;
		return;
	}
	state = ST_RUNNING;
	mdns_start(R1SYNC_MDNS_NAME, 80);
	led_set_wifi_transfer(true);
	wifi_set_status_poll_slow(true);
}

static void leave_async(void *user) {
	(void)user;
	back_btn_cb(NULL);
	leaving = false;
}

static void begin_close(void) {
	if (state != ST_RUNNING) {
		return;
	}
	state = ST_CLOSING;
	closing_since = (long)lv_tick_get();
	r1syncapp_ask_stop();
	refresh();
}

static void confirm_leave_cb(void *user) {
	(void)user;
	leaving = true;
	begin_close();
}

// The chevron while r1sync runs: a download in flight would stop. Ask first.
static bool back_guard(void) {
	if (leaving || state == ST_IDLE) {
		return false;
	}
	if (state == ST_RUNNING) {
		confirm_show("r1sync_leave", "r1sync_leave_note", "r1sync_close", confirm_leave_cb, NULL);
	}
	return true; // closing: wait for it
}

static void close_row_cb(lv_event_t *e) {
	(void)e;
	confirm_show("r1sync_leave", "r1sync_leave_note", "r1sync_close", confirm_leave_cb, NULL);
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;

	if (state == ST_IDLE && wifi_is_connected() && r1syncapp_available(card_root())) {
		start(); // Wi-Fi came up while the page was open
	}
	if (state == ST_CLOSING && r1syncapp_running() &&
		lv_tick_elaps((uint32_t)closing_since) > R1SYNC_STOP_WAIT_MS) {
		r1syncapp_kill();
	}
	// Ended: by this page, or on its own (the web page's Close, its idle
	// timer). Either way the page goes with it.
	if (state != ST_IDLE && !r1syncapp_running()) {
		finished();
		leaving = true;
		lv_async_call(leave_async, NULL);
		return;
	}
	refresh();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	leaving = false;
	if (state == ST_IDLE) {
		start();
	}
	refresh();
	lv_timer_resume(poll_timer);
	lv_timer_ready(poll_timer);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(poll_timer);
	// Whatever way off the page: r1sync does not outlive it.
	if (state != ST_IDLE) {
		r1syncapp_stop();
		finished();
	}
}

static void wifi_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

static void refresh_accent(void) {
	if (url_label) {
		lv_obj_set_style_text_color(url_label, theme()->accent, 0);
	}
}

static lv_obj_t *line_label(lv_obj_t *parent, const lv_font_t *font, bool dim) {
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(label, dim ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_hor(label, 4, 0);
	return label;
}

void r1syncpage_init(gui_config_t *cfg) {
	sd_root_path = cfg->sd_root_path;
	lv_obj_t *container = settingsrow_page(r1syncpage_screen, cfg, "r1sync");

	status_label = line_label(container, &font_ui_22, false);
	lv_obj_set_style_pad_top(status_label, 14, 0);

	url_label = line_label(container, &font_ui_32, false);
	lv_obj_set_style_text_color(url_label, theme()->accent, 0);
	lv_obj_set_style_pad_ver(url_label, 16, 0);

	addr_label = line_label(container, &font_ui_20, true);
	lv_obj_set_style_pad_bottom(addr_label, 10, 0);

	pin_label = line_label(container, &font_ui_26, false);
	lv_obj_set_style_pad_bottom(pin_label, 10, 0);

	abs_label = line_label(container, &font_ui_20, false);
	nd_label = line_label(container, &font_ui_20, false);
	notice_label = line_label(container, &font_ui_20, true);
	lv_obj_set_style_pad_top(notice_label, 8, 0);
	hide_status();

	close_row = settingsrow_add(container, "r1sync_close", NULL, close_row_cb, NULL);
	lv_obj_set_style_margin_top(close_row, 16, 0);
	hide(close_row);

	wifi_row = settingsrow_add(container, "wi_fi_settings", NULL, wifi_row_cb, NULL);
	lv_obj_set_style_margin_top(wifi_row, 16, 0);
	hide(wifi_row);

	poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	lv_obj_add_event_cb(r1syncpage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(r1syncpage_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_set_back_guard(r1syncpage_screen, back_guard);
	theme_register_refresh(refresh_accent);
	refresh();
}
