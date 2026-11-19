#include "bookresume.h"
#include "icons.h"
#include "main_menu.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/nowplaying/player.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/playback/audiobook.h"
#include "switcher.h"
#include "sx_sheet.h"
#include "toast.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Only the GUI thread reads and updates the home selection. No new timer.
static char checked_file[512];
static unsigned checked_serial;
static char menu_book[512];
static char menu_title[1024];
static uintptr_t menu_generation;
static unsigned menu_card_epoch;

static void selected_book(char *book, size_t size) {
	size_t used = 0;
	if (!size)
		return;
	book[0] = 0;
	for (int i = 0; i < 5; i++) {
		char key[32];
		snprintf(key, sizeof(key), i ? "resume_book_%d" : "resume_book", i);
		const char *part = config_get_copy("home", key, "", (char[CONFIG_VALUE_SIZE]){0}, CONFIG_VALUE_SIZE);
		size_t length = strlen(part);
		if (used + length >= size) {
			book[0] = 0;
			return;
		}
		memcpy(book + used, part, length + 1);
		used += length;
	}
}
static void select_book(const char *book, const char *title) {
	// Config values hold 127 bytes. Split at UTF-8 boundaries, not mid-path.
	for (int i = 0; i < 5; i++) {
		char key[32], part[121];
		snprintf(key, sizeof(key), i ? "resume_book_%d" : "resume_book", i);
		size_t n = strlen(book);
		if (n > 120) {
			n = 120;
			while (n && ((unsigned char)book[n] & 0xc0) == 0x80)
				n--;
		}
		memcpy(part, book, n);
		part[n] = 0;
		config_set("home", key, part);
		book += n;
	}
	char short_title[128];
	size_t n = strlen(title);
	if (n > 127) {
		n = 127;
		while (n && ((unsigned char)title[n] & 0xc0) == 0x80)
			n--;
	}
	memcpy(short_title, title, n);
	short_title[n] = 0;
	config_set("home", "resume_title", short_title);
	config_save();
}

void bookresume_note(const device_state_t *state) {
	if (state->status != AUDIO_STATUS_PLAYING || state->live || !state->current_file[0])
		return;
	unsigned serial = audiobook_serial();
	if (!strcmp(checked_file, state->current_file) && serial == checked_serial)
		return;
	snprintf(checked_file, sizeof(checked_file), "%s", state->current_file);
	checked_serial = serial;
	char book[512], file[512], title[256];
	if (!audiobook_current_book(book, sizeof(book), file, sizeof(file)) || strcmp(file, state->current_file))
		return;
	char selected[512];
	selected_book(selected, sizeof(selected));
	if (!strcmp(book, selected))
		return;
	if (!audiobookdb_book_info(book, title, sizeof(title), NULL, 0, NULL))
		return;
	select_book(book, title);
	main_menu_refresh();
}
void bookresume_description(char *text, size_t size) {
	char book[512];
	selected_book(book, sizeof(book));
	const char *title = config_get_copy("home", "resume_title", "", (char[CONFIG_VALUE_SIZE]){0}, CONFIG_VALUE_SIZE);
	snprintf(text, size, "%s", book[0] && title[0] && !audiobookdb_is_finished(book) ? title : tr("home_choose_book"));
}
static void play(const char *book) {
	if (!book[0] || audiobookdb_is_finished(book)) {
		switch_screen(audiobooks_screen);
		return;
	}
	if (!gui_card_available()) {
		toast_error("home_card_unavailable");
		return;
	}
	char current[512], file[512];
	if (audiobook_current_book(current, sizeof(current), file, sizeof(file)) && !strcmp(current, book)) {
		device_state_t state;
		device_state_get(&state);
		if (!strcmp(state.current_file, file) && state.status == AUDIO_STATUS_PLAYING)
			return;
		if (!strcmp(state.current_file, file) && state.status == AUDIO_STATUS_PAUSED) {
			player_key_play_pause();
			return;
		}
	}
	// A removed checkpoint must not silently turn Continue into Start over.
	double seconds;
	if (!audiobookdb_get_position(book, file, sizeof(file), &seconds) || access(file, R_OK)) {
		toast_error("home_book_unavailable");
		switch_screen(audiobooks_screen);
		return;
	}
	audiobooks_resume_book(book);
}
void bookresume_play(void) {
	char book[512];
	selected_book(book, sizeof(book));
	play(book);
}
static void continue_action(void *generation) {
	if ((uintptr_t)generation != menu_generation)
		return;
	if (menu_card_epoch != audiobookdb_card_epoch()) {
		toast_error("home_card_unavailable");
		return;
	}
	play(menu_book);
}
static void other_action(void *unused) {
	(void)unused;
	audiobooks_open_continue();
}
static void library_action(void *unused) {
	(void)unused;
	switch_screen(audiobooks_screen);
}
static void move_action(void *unused) {
	(void)unused;
	if (main_menu_contains(HOME_CONTINUE_BOOK))
		main_menu_hide(HOME_CONTINUE_BOOK);
	else if (!main_menu_move(HOME_CONTINUE_BOOK, main_menu_count()))
		toast_error("home_full");
}
static void settings_action(void *unused) {
	(void)unused;
	main_menu_open_settings();
}
// A single snapshot when the menu opens: no scanner, timer or checkpoint write.
static void menu_checkpoint(void) {
	char title[256], file[512] = "", place[512] = "";
	config_get_copy("home", "resume_title", "", title, sizeof(title));
	if (!menu_book[0]) {
		snprintf(menu_title, sizeof(menu_title), "%s", tr("home_choose_book"));
		return;
	}
	const char *problem = NULL;
	double seconds = 0;
	if (!gui_card_available()) {
		problem = tr("home_card_unavailable");
	} else {
		bool saved = audiobookdb_get_position(menu_book, file, sizeof(file), &seconds);
		char current[512], current_file[512];
		unsigned serial = audiobook_serial();
		device_state_t state;
		device_state_get(&state);
		if (audiobook_current_book(current, sizeof(current), current_file, sizeof(current_file)) && !strcmp(current, menu_book) && !strcmp(current_file, state.current_file) && (state.status == AUDIO_STATUS_PLAYING || state.status == AUDIO_STATUS_PAUSED)) {
			char live_place[512];
			audiobook_place_label(state.progress_current_secs, live_place, sizeof(live_place));
			if (serial == audiobook_serial()) {
				snprintf(file, sizeof(file), "%s", current_file);
				snprintf(place, sizeof(place), "%s", live_place);
				seconds = state.progress_current_secs;
				saved = true;
			}
		}
		if (!saved || !file[0] || access(file, R_OK))
			problem = tr("home_book_unavailable");
	}
	if (problem) {
		snprintf(menu_title, sizeof(menu_title), "%s\n%s", title, problem);
		return;
	}
	// For a non-loaded book the saved filename is honest; loading tags or
	// inventing an embedded chapter would add I/O and could describe another
	// file.
	if (!place[0]) {
		const char *name = strrchr(file, '/');
		snprintf(place, sizeof(place), "%s", name ? name + 1 : file);
	}
	if (!isfinite(seconds) || seconds < 0)
		seconds = 0;
	if (seconds > 2147483647.0)
		seconds = 2147483647.0;
	long total = (long)seconds;
	char clock[40];
	if (total >= 3600)
		snprintf(clock, sizeof(clock), "%ld:%02ld:%02ld", total / 3600, total / 60 % 60, total % 60);
	else
		snprintf(clock, sizeof(clock), "%ld:%02ld", total / 60, total % 60);
	bool finished = audiobookdb_is_finished(menu_book);
	snprintf(menu_title, sizeof(menu_title), "%s\n%s · %s%s%s", title, place, clock, finished ? "\n" : "", finished ? tr("audiobook_finished") : "");
}
void bookresume_menu(void) {
	if (++menu_generation == 0)
		menu_generation++;
	menu_card_epoch = audiobookdb_card_epoch();
	selected_book(menu_book, sizeof(menu_book));
	menu_checkpoint();
	sx_sheet_item_t items[5];
	int n = 0;
	if (menu_book[0])
		items[n++] = (sx_sheet_item_t){.label = tr("home_resume_book"), .icon = &icon_menu_audiobook_continue, .action = continue_action, .user = (void *)menu_generation};
	items[n++] = (sx_sheet_item_t){.label = tr("home_other_books"), .icon = &icon_menu_audiobook_library, .action = other_action};
	items[n++] = (sx_sheet_item_t){.label = tr("audiobooks"), .icon = &icon_menu_audiobooks, .action = library_action};
	items[n++] = (sx_sheet_item_t){.label = tr(main_menu_contains(HOME_CONTINUE_BOOK) ? "home_to_more" : "home_to_main"), .icon = &icon_menu_more, .action = move_action};
	items[n++] = (sx_sheet_item_t){.label = tr("home_settings"), .icon = &icon_menu_settings, .action = settings_action};
	sx_sheet_menu(tr("home_resume_book"), menu_title, items, n);
}
