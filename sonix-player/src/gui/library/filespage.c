#include "filespage.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/imageview.h"
#include "src/gui/library/textview.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_mini.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/touch.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/fileops.h"
#include "src/system/device/system.h"
#include "src/system/library/library.h"
#include "src/system/playback/playlist.h"

lv_obj_t *filespage_screen;

// The tap a gesture ends with is not a tap. LVGL sends CLICKED to whatever the
// finger was on when it lifted, and for a swipe-back that is the row the swipe
// began on, so a tap handler has to ignore a click that closed a drag. Same
// guard the browser, the media lists and the grid pages use.
static bool gesture_not_tap(void) {
	return player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active();
}

// How deep the page will follow a path, and how many entries it will list from
// one folder. Both are limits against a pathological card rather than against
// any real one: a folder with more than this in it is already unusable as a
// list. What the cap leaves out goes in the log, because a file manager that
// drops entries without saying so is worse than one with a limit.
#define MAX_ENTRIES 2000
#define PATH_MAX_LEN FILEOP_ROOT_MAX

// The listing is built a screenful at a time, for the reason the chapter list
// gives in ebookreader.c: every row is a card with a glyph and two labels, and
// LVGL measures all of them before it can draw the first. What the reader is
// waiting for is the first six.
//
// It matters more here than there, because a row also costs a stat() on the
// card to find out how big the file is -- and a stat on a slow card, two
// thousand times, is a folder that takes seconds to open. Built this way, it is
// a stat for each row that is actually on screen.
#define ROWS_FIRST 14
#define ROWS_MORE 14

#define ROW_HEIGHT SX_ROW_H

static void entry_art(lv_obj_t *parent, const lv_image_dsc_t *source, const char *name) {
	lv_obj_t *plate = sx_box(parent);
	lv_obj_set_size(plate, SX_ROW_THUMB, SX_ROW_THUMB);
	lv_obj_set_style_radius(plate, SX_ROW_THUMB_R, 0);
	lv_obj_set_style_bg_color(plate, sx_placeholder_name(name), 0);
	lv_obj_set_style_bg_opa(plate, LV_OPA_COVER, 0);
	lv_obj_t *glyph = lv_image_create(plate);
	lv_image_set_src(glyph, source);
	if (source->header.w)
		lv_image_set_scale(glyph, 28 * LV_SCALE_NONE / source->header.w);
	sx_set_recolor(glyph, lv_color_white());
	lv_obj_center(glyph);
}

typedef struct {
	char *name;	 // in the pool
	bool dir;
} entry_t;

static char root_path[PATH_MAX_LEN];  // the card, which is as far up as this goes
static char path[PATH_MAX_LEN];		  // the folder on screen

static entry_t *entries;
static int entry_count;
static int entries_built;  // how many rows are on the list
static int skipped;		   // entries the cap left out

// Set while rebuild() is pulling the list apart and putting it back.
//
// Emptying a scrolling list changes how much there is to scroll, and LVGL says
// so with an LV_EVENT_SCROLL -- delivered from inside lv_obj_clean(), while the
// entries still belong to the folder being left. Taken as "scrolled near the
// end, build some more", that would append rows of the outgoing folder to the
// list of the incoming one.
static bool rebuilding;

static lv_obj_t *file_list;
static lv_obj_t *empty_label;
static lv_obj_t *title_label;


// The name dialog, in the shape the playlist page uses: a full-screen layer
// with the field at the top and the shared keyboard at the bottom.
static lv_obj_t *name_layer;
static lv_obj_t *name_heading;
static lv_obj_t *name_field;
static keyboard_t *name_keyboard;

// What the dialog is naming: a folder being created, or the entry below.
static bool naming_new_folder;
static storage_card_token_t list_card, action_card;
static unsigned list_database_epoch, action_database_epoch;
static char action_path[PATH_MAX_LEN], action_root[PATH_MAX_LEN];
static char acting_on[NAME_MAX + 1];

// ---------------------------------------------------------------------------
// Moving and copying
//
// Both need a second thing the rest of the page does not: somewhere to put it.
// That is the picker layer -- the same walk as the page itself, folders only,
// with the button that says "here" at the bottom.
//
// And both can take a while. A copy is bounded by how big the file is, which on
// a music card is half a gigabyte often enough, and a move across the card is a
// rename. All mutations happen on one worker with the modal up. The page owns
// its completion even when gui_post() cannot accept the wake-up.
// ---------------------------------------------------------------------------

typedef enum {
	OP_NONE = 0,
	OP_MOVE,
	OP_COPY,
	OP_DELETE,
	OP_RENAME,
	OP_MKDIR,
} op_t;

static op_t pending_op;
static char op_source[PATH_MAX_LEN + NAME_MAX + 2];	 // what is being moved or copied
static char op_target[PATH_MAX_LEN + NAME_MAX + 2];	 // where it is going, name and all
static bool op_ok;
static int op_error;
static fileop_request_t op_request;
static unsigned op_database_epoch;
static bool op_running; // GUI owner; released only after terminal consumption.
static atomic_bool op_ready, op_closing;

// How far along it is, in bytes rather than in files: a folder holding one
// album and a hundred cover thumbnails would otherwise stand at 1% and then
// jump to the end.
//
// The total is counted by the worker and not by op_start(), because a move
// within the card is a rename and has no length worth counting. Only copies
// need a byte total. A total of zero means "no bar", which is right for both the
// rename and the moment before the walk has finished.
static pthread_mutex_t op_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t op_total_bytes;
static uint64_t op_done_bytes;
static lv_timer_t *op_progress_timer;

static lv_obj_t *pick_layer;
static lv_obj_t *pick_list;
static lv_obj_t *pick_title;
static lv_obj_t *pick_here_btn;
static lv_obj_t *pick_here_label;
static char pick_path[PATH_MAX_LEN];

static void rebuild(void);

// ---------------------------------------------------------------------------
// what a file is
// ---------------------------------------------------------------------------

// The glyph for a name, by its extension. The same families the transfer page
// draws in a browser, so the card looks the same from both ends.
static const char *ext_of(const char *name) {
	const char *dot = strrchr(name, '.');
	return dot ? dot + 1 : "";
}

static bool ext_in(const char *ext, const char *const *list) {
	for (int i = 0; list[i]; i++) {
		if (strcasecmp(ext, list[i]) == 0) {
			return true;
		}
	}
	return false;
}

static const char *const EXT_AUDIO[] = {"iso", "dff", "dsf", "dts", "ape", "flac", "aif", "aiff", "wav",
										"m4a", "aac", "mp2", "mp3", "ogg", "oga", "wma", "opus", "dsd",
										"wv",  "mpc", "tak", "tta", NULL};
static const char *const EXT_AUDIOBOOK[] = {"m4b", "aa", "aax", NULL};
static const char *const EXT_PLAYLIST[] = {"m3u", "m3u8", "cue", NULL};
static const char *const EXT_IMAGE[] = {"bmp", "png", "jpg", "jpeg", "gif", "webp", NULL};
static const char *const EXT_TEXT[] = {"lrc", "txt", "t", "ini", "nfo", NULL};
static const char *const EXT_FIRMWARE[] = {"upt", NULL};
static const char *const EXT_GAME[] = {"gb", "gbc", "sav", NULL};
static const char *const EXT_BOOK[] = {"epub", NULL};

static const lv_image_dsc_t *glyph_for(const entry_t *entry) {
	if (entry->dir) {
		return &icon_folder;
	}
	const char *ext = ext_of(entry->name);
	if (ext_in(ext, EXT_AUDIO)) {
		return &icon_files_music;
	}
	if (ext_in(ext, EXT_AUDIOBOOK)) {
		return &icon_files_audiobook;
	}
	if (ext_in(ext, EXT_PLAYLIST)) {
		return &icon_files_playlist;
	}
	if (ext_in(ext, EXT_IMAGE)) {
		return &icon_files_image;
	}
	if (ext_in(ext, EXT_TEXT)) {
		return &icon_files_text;
	}
	if (ext_in(ext, EXT_FIRMWARE)) {
		return &icon_files_update;
	}
	if (ext_in(ext, EXT_GAME)) {
		return &icon_files_game;
	}
	if (ext_in(ext, EXT_BOOK)) {
		return &icon_files_book;
	}
	return &icon_file;
}

// A name with any of the extensions above is a file and there is nothing to ask
// the card about. Only names that could go either way are worth a stat -- which
// on exFAT, where readdir fills in no type at all, is what keeps opening a
// folder of eight hundred tracks from being eight hundred stats.
static bool ext_is_known(const char *name) {
	const char *ext = ext_of(name);
	return ext[0] && (ext_in(ext, EXT_AUDIO) || ext_in(ext, EXT_AUDIOBOOK) || ext_in(ext, EXT_PLAYLIST) ||
					  ext_in(ext, EXT_IMAGE) || ext_in(ext, EXT_TEXT) || ext_in(ext, EXT_FIRMWARE) ||
					  ext_in(ext, EXT_GAME) || ext_in(ext, EXT_BOOK));
}

// The player's own way of putting a size on screen, the same one the transfer
// page uses.
static void human_size(long long bytes, char *out, size_t size) {
	if (bytes >= 1073741824LL) {
		snprintf(out, size, "%.2f GB", (double)bytes / 1073741824.0);
	} else if (bytes >= 1048576LL) {
		snprintf(out, size, "%.1f MB", (double)bytes / 1048576.0);
	} else if (bytes >= 1024LL) {
		snprintf(out, size, "%lld KB", bytes / 1024);
	} else {
		snprintf(out, size, "%lld B", bytes);
	}
}

// Joins the folder on screen with one of its entries. False when the result
// would not fit, and then it is a refusal and not a shortened path: everything
// here acts on what this builds, and a truncated path names a different file --
// which for the delete below would be a different file deleted.
static bool child_path(const char *name, char *out, size_t size) {
	int wrote = snprintf(out, size, "%s/%s", path, name);
	if (wrote < 0 || (size_t)wrote >= size) {
		out[0] = '\0';
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// reading a folder
// ---------------------------------------------------------------------------

static void entries_free(void) {
	for (int i = 0; i < entry_count; i++) {
		free(entries[i].name);
	}
	free(entries);
	entries = NULL;
	entry_count = 0;
	entries_built = 0;
	skipped = 0;
}

// Folders first, then by name, the way every list on this device is ordered.
static int entry_cmp(const void *a, const void *b) {
	const entry_t *x = a, *y = b;
	if (x->dir != y->dir) {
		return x->dir ? -1 : 1;
	}
	return strcasecmp(x->name, y->name);
}

// Visibility only: recursive copy/move/delete must still include hidden children.
static bool service_hidden(const char *name) {
	return !config_get_int("files", "show_service", 0) &&
		(name[0] == '.' || playlist_is_junk_name(name));
}

static void read_folder(void) {
	entries_free();
	list_card = (storage_card_token_t){0};
	list_database_epoch = 0;
	storage_card_token_t reading;
	char admitted[PATH_MAX_LEN];
	if (!storage_card_try_admit(&reading, admitted, sizeof(admitted)) || strcmp(admitted, root_path))
		return;

	DIR *dir = opendir(path);
	if (!dir) {
		return;
	}

	entries = calloc(MAX_ENTRIES, sizeof(*entries));
	if (!entries) {
		closedir(dir);
		return;
	}

	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		if (service_hidden(de->d_name)) {
			continue;
		}

		if (entry_count >= MAX_ENTRIES) {
			skipped++;
			continue;
		}

		bool is_dir;
		if (de->d_type == DT_DIR) {
			is_dir = true;
		} else if (de->d_type == DT_REG || de->d_type == DT_LNK) {
			is_dir = false;
		} else if (ext_is_known(de->d_name)) {
			is_dir = false;
		} else {
			char full[PATH_MAX_LEN + NAME_MAX + 2];
			struct stat st;
			is_dir = child_path(de->d_name, full, sizeof(full)) && stat(full, &st) == 0 && S_ISDIR(st.st_mode);
		}

		entries[entry_count].name = strdup(de->d_name);
		if (!entries[entry_count].name) {
			break;
		}
		entries[entry_count].dir = is_dir;
		entry_count++;
	}
	closedir(dir);

	if (skipped > 0) {
		fprintf(stderr, "files: '%s' holds more than %d entries, %d not listed\n", path, MAX_ENTRIES, skipped);
	}

	if (storage_card_try_check(&reading) != 0) {
		entries_free();
		return;
	}
	list_card = reading;
	list_database_epoch = library_database_epoch();
	qsort(entries, (size_t)entry_count, sizeof(*entries), entry_cmp);
}

// Filesystem work is isolated behind the original card token and directory FDs.
// Menu/dialog/picker state is copied once, before the worker can start.
static bool capture_action(void) {
	if (op_running || atomic_load(&op_closing) || storage_card_try_check(&list_card)) {
		gui_notify_popup("card_write_failed");
		return false;
	}
	action_card = list_card;
	action_database_epoch = list_database_epoch;
	snprintf(action_path, sizeof(action_path), "%s", path);
	snprintf(action_root, sizeof(action_root), "%s", root_path);
	return true;
}

static bool action_current(void) {
	if (op_running || atomic_load(&op_closing) || storage_card_try_check(&action_card)) {
		gui_notify_popup("card_write_failed");
		return false;
	}
	return true;
}

static bool action_child(const char *name, char *out, size_t size) {
	int n = snprintf(out, size, "%s/%s", action_path, name);
	return n >= 0 && (size_t)n < size;
}

static const char *op_title(void) {
	switch (pending_op) {
	case OP_MOVE:
		return "moving";
	case OP_COPY:
		return "copying";
	case OP_DELETE:
		return "delete";
	case OP_RENAME:
		return "rename";
	default:
		return "files_new_folder";
	}
}

static const char *op_failure(void) {
	if (op_error == EEXIST)
		return "files_name_taken";
	if (op_error == ESTALE || op_error == EAGAIN || op_error == EBUSY || op_error == ENODEV || op_error == EROFS)
		return "card_write_failed";
	switch (pending_op) {
	case OP_MOVE:
		return "files_could_not_move_it";
	case OP_COPY:
		return "files_could_not_copy_it";
	case OP_DELETE:
		return "files_could_not_delete_it";
	case OP_RENAME:
		return "files_could_not_rename_it";
	default:
		return "files_create_failed";
	}
}

static void op_progress(uint64_t done, uint64_t total, void *unused) {
	(void)unused;
	pthread_mutex_lock(&op_lock);
	op_done_bytes = done;
	op_total_bytes = total;
	pthread_mutex_unlock(&op_lock);
}

static bool op_cancelled(void *unused) {
	(void)unused;
	return atomic_load(&op_closing);
}

// ---------------------------------------------------------------------------
// the thread that does it, and the way back
// ---------------------------------------------------------------------------

// Reads the two counters and moves the bar. It runs on the interface thread and
// the worker never touches LVGL, which is the whole reason for the counters:
// the alternative is a gui_post() per chunk, and a 64 KB chunk of a gigabyte is
// sixteen thousand of them.
#define OP_PROGRESS_MS 200

static void op_progress_cb(lv_timer_t *timer) {
	(void)timer;

	pthread_mutex_lock(&op_lock);
	uint64_t total = op_total_bytes;
	uint64_t done = op_done_bytes;
	pthread_mutex_unlock(&op_lock);

	// Nothing to show until the walk has a total: a rename never gets one, and
	// a bar at nought over a job that has already finished is a lie.
	gui_modal_progress(total > 0 ? (int)((done * 100) / total) : -1);
}

static void op_progress_stop(void) {
	if (op_progress_timer) {
		lv_timer_delete(op_progress_timer);
		op_progress_timer = NULL;
	}
}

static void op_finished(void *user) {
	(void)user;
	op_progress_stop();
	gui_modal_hide();
	if (!op_ok)
		gui_notify_popup(op_failure());
	else if (pending_op == OP_MOVE)
		toast_success("files_moved");
	else if (pending_op == OP_COPY)
		toast_success("files_copied");
	else if (pending_op == OP_DELETE)
		toast_success("files_deleted");
	pending_op = OP_NONE;
	storage_card_files_recheck();
	rebuild();
}

void filespage_service_completion(void) {
	if (atomic_load(&op_closing) || !atomic_exchange(&op_ready, false))
		return;
	op_running = false;
	op_finished(NULL);
}

void filespage_shutdown_completions(void) {
	atomic_store(&op_closing, true);
	atomic_store(&op_ready, false);
}

static void op_completion_wakeup(void *unused) {
	(void)unused;
	filespage_service_completion();
}

static void *op_worker(void *arg) {
	(void)arg;
	thread_be_background("filesop");
	op_error = fileop_run(&op_request, op_progress, op_cancelled, NULL);
	op_ok = op_error == 0;
	// Partial deletion still needs pruning, but a late result cannot touch the
	// database that replaced the card this menu originally described.
	if (op_request.kind == FILEOP_DELETE) {
		// The mutation has released every FD. A worker may now wait for a busy
		// supervisor, then pin this exact card while checking the old DB epoch.
		// A one-shot try_check would silently skip pruning on transient EAGAIN.
		int card_fd = storage_pin_card_if(&op_request.card);
		if (card_fd >= 0) {
			const char *gone[] = {op_request.source};
			library_forget_deleted_at(op_database_epoch, gone, 1);
			close(card_fd);
		}
	}
	if (!atomic_load(&op_closing)) {
		atomic_store(&op_ready, true);
		(void)gui_post(op_completion_wakeup, NULL);
	}
	return NULL;
}

static void op_start(void) {
	if (!action_current()) {
		pending_op = OP_NONE;
		return;
	}
	static const fileop_kind_t kinds[] = {FILEOP_COPY, FILEOP_MOVE, FILEOP_COPY, FILEOP_DELETE, FILEOP_RENAME, FILEOP_MKDIR};
	if (pending_op <= OP_NONE || pending_op > OP_MKDIR)
		return;
	if (strlen(op_source) >= sizeof(op_request.source) || strlen(op_target) >= sizeof(op_request.target)) {
		pending_op = OP_NONE;
		gui_notify_popup("files_too_deep");
		return;
	}
	op_request = (fileop_request_t){.kind = kinds[pending_op], .card = action_card};
	snprintf(op_request.root, sizeof(op_request.root), "%s", action_root);
	memcpy(op_request.source, op_source, strlen(op_source) + 1);
	memcpy(op_request.target, op_target, strlen(op_target) + 1);
	op_database_epoch = action_database_epoch;
	op_error = 0;
	op_running = true;
	pthread_mutex_lock(&op_lock);
	op_total_bytes = 0;
	op_done_bytes = 0;
	pthread_mutex_unlock(&op_lock);

	gui_modal_show(NULL, theme()->accent, op_title(), NULL);

	pthread_t thread;
	if (pthread_create(&thread, NULL, op_worker, NULL) != 0) {
		// A worker changes its scheduler policy and performs blocking I/O.
		// Running it on this caller would leave the GUI in SCHED_IDLE.
		const char *failure = op_failure();
		pending_op = OP_NONE;
		op_running = false;
		op_progress_stop();
		gui_modal_hide();
		gui_notify_popup(failure);
		return;
	}
	pthread_detach(thread);

	op_progress_stop();
	op_progress_timer = lv_timer_create(op_progress_cb, OP_PROGRESS_MS, NULL);
}

static void delete_confirmed(void *user) {
	(void)user;
	if (!acting_on[0] || !action_current())
		return;
	if (!action_child(acting_on, op_source, sizeof(op_source))) {
		gui_notify_popup("files_too_deep");
		return;
	}
	op_target[0] = '\0';
	pending_op = OP_DELETE;
	acting_on[0] = '\0';
	op_start();
}

static void menu_delete_action(void *user) {
	(void)user;
	if (!acting_on[0]) {
		return;
	}
	// The name goes in the question, because on a list of forty rows the one the
	// menu belongs to is not obvious by the time the dialog is up. Formatted
	// here and passed as the title: confirm_show() puts its message through
	// tr(), and a file name is not a translation key.
	char question[NAME_MAX + 64];
	snprintf(question, sizeof(question), tr("files_delete_x_question"), acting_on);
	confirm_show(question, NULL, "delete", delete_confirmed, NULL);
}

static void name_layer_show(const char *heading, const char *initial) {
	lv_label_set_text(name_heading, tr(heading));
	lv_textarea_set_text(name_field, initial ? initial : "");
	keyboard_reset(name_keyboard);
	keyboard_set_field(name_keyboard, name_field);
	lv_obj_set_hidden(name_layer, false);
	sx_mini_update_visibility(); // its keyboard comes or goes with it
	lv_obj_move_foreground(name_layer);
}

static void name_layer_hide(void) {
	lv_obj_set_hidden(name_layer, true);
	sx_mini_update_visibility(); // its keyboard comes or goes with it
	acting_on[0] = '\0';
	naming_new_folder = false;
}

// ---------------------------------------------------------------------------
// choosing where
// ---------------------------------------------------------------------------

static void pick_rebuild(void);

static void pick_row_deleted(lv_event_t *e) {
	if (lv_event_get_target(e) == lv_event_get_current_target(e))
		free(lv_event_get_user_data(e));
}

static void pick_row_cb(lv_event_t *e) {
	if (gesture_not_tap()) {
		return;
	}
	const char *name = lv_event_get_user_data(e);
	char next[PATH_MAX_LEN];
	int wrote = snprintf(next, sizeof(next), "%s/%s", pick_path, name);
	if (wrote < 0 || (size_t)wrote >= sizeof(next)) {
		gui_notify_popup("files_too_deep");
		return;
	}
	snprintf(pick_path, sizeof(pick_path), "%s", next);
	pick_rebuild();
	lv_obj_scroll_to_y(pick_list, 0, LV_ANIM_OFF);
}

// Folders only: nothing else can be a destination, and a list of every track on
// the card would bury the three folders that can.
static void pick_rebuild(void) {
	lv_obj_clean(pick_list);
	if (!action_current())
		return;

	const char *slash = strrchr(pick_path, '/');
	lv_label_set_text(pick_title, strcmp(pick_path, root_path) == 0 ? tr("file_explorer") : (slash ? slash + 1 : pick_path));
	lv_label_set_text(pick_here_label, tr(pending_op == OP_MOVE ? "files_move_here" : "files_copy_here"));

	DIR *dir = opendir(pick_path);
	if (!dir) {
		return;
	}

	// Names first, then rows, so they come out in order without a second walk.
	char (*names)[NAME_MAX + 1] = calloc(MAX_ENTRIES, NAME_MAX + 1);
	int count = 0;
	if (names) {
		struct dirent *de;
		while ((de = readdir(dir)) != NULL && count < MAX_ENTRIES) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || service_hidden(de->d_name)) {
				continue;
			}
			char full[PATH_MAX_LEN + NAME_MAX + 2];
			int wrote = snprintf(full, sizeof(full), "%s/%s", pick_path, de->d_name);
			if (wrote < 0 || (size_t)wrote >= sizeof(full)) {
				continue;
			}
			struct stat st;
			if (de->d_type != DT_DIR && !(de->d_type == DT_UNKNOWN && stat(full, &st) == 0 && S_ISDIR(st.st_mode))) {
				continue;
			}
			snprintf(names[count], NAME_MAX + 1, "%s", de->d_name);
			count++;
		}
	}
	closedir(dir);

	if (!names || storage_card_try_check(&action_card)) {
		free(names);
		return;
	}

	for (int i = 0; i < count - 1; i++) {
		for (int j = i + 1; j < count; j++) {
			if (strcasecmp(names[i], names[j]) > 0) {
				char swap[NAME_MAX + 1];
				snprintf(swap, sizeof(swap), "%s", names[i]);
				snprintf(names[i], NAME_MAX + 1, "%s", names[j]);
				snprintf(names[j], NAME_MAX + 1, "%s", swap);
			}
		}
	}

	for (int i = 0; i < count; i++) {
		lv_obj_t *row = lv_btn_create(pick_list);
		lv_obj_set_size(row, lv_pct(100), ROW_HEIGHT);
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row, 12, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, 16, 0);
		lv_obj_set_style_pad_column(row, 14, 0);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		// The row owns the copy the callback reads: the array above is freed
		// before the first tap can happen.
		char *kept = strdup(names[i]);
		if (!kept) {
			lv_obj_delete(row);
			break;
		}
		lv_obj_add_event_cb(row, pick_row_deleted, LV_EVENT_DELETE, kept);
		lv_obj_add_event_cb(row, pick_row_cb, LV_EVENT_CLICKED, kept);

		entry_art(row, &icon_folder, names[i]);

		lv_obj_t *label = lv_label_create(row);
		lv_label_set_text(label, names[i]);
		lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(label, 1);
		lv_obj_set_height(label, lv_font_get_line_height(&font_ui_26_bold));
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, &font_ui_26_bold, 0);
	}
	free(names);
}

static void pick_hide(void) {
	lv_obj_set_hidden(pick_layer, true);
	sx_mini_input_overlay(filespage_screen, false);
	pending_op = OP_NONE;
	acting_on[0] = '\0';
}

static void pick_cancel_cb(lv_event_t *e) {
	(void)e;
	pick_hide();
}

// True when `inner` is `outer` or lies under it. What it is for: a folder
// cannot be moved or copied into itself or into one of its own children --
// a move would make it unreachable, a copy would not end.
static bool path_is_within(const char *inner, const char *outer) {
	size_t n = strlen(outer);
	if (strncmp(inner, outer, n) != 0) {
		return false;
	}
	return inner[n] == '\0' || inner[n] == '/';
}

static void pick_here_cb(lv_event_t *e) {
	(void)e;
	if (pending_op == OP_NONE || !acting_on[0] || !action_current()) {
		return;
	}

	char source[PATH_MAX_LEN + NAME_MAX + 2];
	char target[PATH_MAX_LEN + NAME_MAX + 2];
	int wrote = snprintf(target, sizeof(target), "%s/%s", pick_path, acting_on);
	if (!action_child(acting_on, source, sizeof(source)) || wrote < 0 || (size_t)wrote >= sizeof(target)) {
		gui_notify_popup("files_too_deep");
		return;
	}

	if (strcmp(pick_path, action_path) == 0) {
		gui_notify_popup("files_it_is_already_there");
		return;
	}
	if (path_is_within(pick_path, source)) {
		gui_notify_popup("files_move_into_itself");
		return;
	}
	snprintf(op_source, sizeof(op_source), "%s", source);
	snprintf(op_target, sizeof(op_target), "%s", target);

	lv_obj_set_hidden(pick_layer, true);
	sx_mini_input_overlay(filespage_screen, false);
	acting_on[0] = '\0';
	op_start();
}

static void pick_show(op_t op) {
	if (!action_current())
		return;
	pending_op = op;
	snprintf(pick_path, sizeof(pick_path), "%s", action_root);
	pick_rebuild();
	lv_obj_scroll_to_y(pick_list, 0, LV_ANIM_OFF);
	lv_obj_set_hidden(pick_layer, false);
	sx_mini_input_overlay(filespage_screen, true);
	lv_obj_move_foreground(pick_layer);
}

static void menu_move_action(void *user) {
	(void)user;
	if (acting_on[0]) {
		pick_show(OP_MOVE);
	}
}

static void menu_copy_action(void *user) {
	(void)user;
	if (acting_on[0]) {
		pick_show(OP_COPY);
	}
}

static void menu_rename_action(void *user) {
	(void)user;
	if (!acting_on[0]) {
		return;
	}
	naming_new_folder = false;
	name_layer_show("rename", acting_on);
}

static void newdir_btn_cb(lv_event_t *e) {
	(void)e;
	if (!capture_action())
		return;
	acting_on[0] = '\0';
	naming_new_folder = true;
	name_layer_show("files_new_folder", NULL);
}

// A name that would step outside the folder on screen, or overwrite the parent,
// is not a name: everything here joins `path` with a slash, and a '/' in the
// middle of that is a path and not a file name.
static bool name_is_usable(const char *name) {
	return name[0] && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && strchr(name, '/') == NULL;
}

static void name_accept_cb(lv_event_t *e) {
	(void)e;
	if (!action_current())
		return;
	const char *name = lv_textarea_get_text(name_field);
	if (!name_is_usable(name) || !action_child(name, op_target, sizeof(op_target))) {
		gui_notify_popup("files_name_invalid");
		return;
	}
	pending_op = naming_new_folder ? OP_MKDIR : OP_RENAME;
	op_source[0] = '\0';
	if (!naming_new_folder && !action_child(acting_on, op_source, sizeof(op_source))) {
		gui_notify_popup("files_too_deep");
		return;
	}
	name_layer_hide();
	op_start();
}

static void name_cancel_cb(lv_event_t *e) {
	(void)e;
	name_layer_hide();
}

// ---------------------------------------------------------------------------
// the rows
// ---------------------------------------------------------------------------

static void entry_menu_cb(lv_event_t *e) {
	if (gesture_not_tap()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) {
		return;
	}
	if (!capture_action())
		return;
	if (strlen(entries[index].name) >= sizeof(acting_on)) {
		gui_notify_popup("files_too_deep");
		return;
	}
	snprintf(acting_on, sizeof(acting_on), "%s", entries[index].name);

	static const popover_item_t items[] = {
		{"rename", menu_rename_action, NULL, false},
		{"files_move_to", menu_move_action, NULL, false},
		{"files_copy_to", menu_copy_action, NULL, false},
		{"delete", menu_delete_action, NULL, false},
	};
	popover_show(lv_event_get_target(e), items, 4);
}

static void row_clicked_cb(lv_event_t *e) {
	if (gesture_not_tap()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) {
		return;
	}

	// A text file opens to be read and a picture to be looked at. Any other
	// file has nothing to open into, and the tap does nothing: the menu is what
	// the three dots are for, and a row that opens a menu when pressed anywhere
	// is a row that opens one every time a scroll ends on it.
	if (!entries[index].dir) {
		char file[PATH_MAX_LEN];
		const char *ext = ext_of(entries[index].name);
		if (ext_in(ext, EXT_TEXT) && child_path(entries[index].name, file, sizeof(file)) && !textview_open(file)) {
			gui_notify_popup("files_text_unreadable");
		} else if (ext_in(ext, EXT_IMAGE) && child_path(entries[index].name, file, sizeof(file))) {
			imageview_open(file);
		}
		return;
	}

	char next[PATH_MAX_LEN];
	if (!child_path(entries[index].name, next, sizeof(next))) {
		gui_notify_popup("files_too_deep");
		return;
	}
	snprintf(path, sizeof(path), "%s", next);
	rebuild();
	lv_obj_scroll_to_y(file_list, 0, LV_ANIM_OFF);
}

static void build_row(int index) {
	const entry_t *entry = &entries[index];

	lv_obj_t *row = lv_btn_create(file_list);
	lv_obj_set_size(row, lv_pct(100), ROW_HEIGHT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_style_pad_column(row, 14, 0);
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	entry_art(row, glyph_for(entry), entry->name);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, entry->name);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(label, 1);
	// One line, then dots. Without a height the label wraps instead, and a long
	// name then grows past the fixed row it lives in.
	lv_obj_set_height(label, lv_font_get_line_height(&font_ui_26_bold));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_26_bold, 0);

	// The size, asked of the card only for the rows that are really built.
	if (!entry->dir) {
		char full[PATH_MAX_LEN + NAME_MAX + 2];
		struct stat st;
		if (child_path(entry->name, full, sizeof(full)) && stat(full, &st) == 0) {
			char text[24];
			human_size((long long)st.st_size, text, sizeof(text));
			lv_obj_t *size_label = lv_label_create(row);
			lv_label_set_text(size_label, text);
			lv_obj_add_style(size_label, &theme_style_text_dim, 0);
			lv_obj_set_style_text_font(size_label, &font_ui_18, 0);
		}
	}

	lv_obj_t *dots = lv_btn_create(row);
	lv_obj_set_size(dots, 44, 44);
	// A fingertip's worth (touch.h), kept within the row's own padding.
	touch_grow(dots, 44, 44, 14);
	lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(dots, 0, 0);
	lv_obj_set_style_shadow_width(dots, 0, 0);
	lv_obj_set_style_pad_all(dots, 0, 0);
	lv_obj_add_event_cb(dots, entry_menu_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	lv_obj_t *dots_glyph = lv_image_create(dots);
	lv_image_set_src(dots_glyph, &icon_ellipsis_vertical);
	lv_obj_add_style(dots_glyph, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(dots_glyph, LV_OPA_60, 0);
	lv_obj_center(dots_glyph);
}

static void rows_add(int how_many) {
	int end = entries_built + how_many;
	if (end > entry_count) {
		end = entry_count;
	}
	for (; entries_built < end; entries_built++) {
		build_row(entries_built);
	}
}

// Keeps going until there is something below the fold, so a short first batch
// on a tall list still leaves a scroll to ask for the rest with.
static void rows_fill(void) {
	for (int guard = 0; guard < 4 && entries_built < entry_count; guard++) {
		lv_obj_update_layout(file_list);
		if (lv_obj_get_scroll_bottom(file_list) > 0) {
			return;
		}
		rows_add(ROWS_MORE);
	}
}

static void list_scrolled_cb(lv_event_t *e) {
	(void)e;
	if (rebuilding || entries_built >= entry_count) {
		return;
	}
	if (lv_obj_get_scroll_bottom(file_list) < lv_obj_get_height(file_list)) {
		rows_add(ROWS_MORE);
	}
}

// ---------------------------------------------------------------------------

// The heading is the folder's own name, and the card's is the card's label
// rather than the last piece of a mount point nobody chose.
static void title_refresh(void) {
	if (strcmp(path, root_path) == 0) {
		lv_label_set_text(title_label, tr("file_explorer"));
		return;
	}
	const char *slash = strrchr(path, '/');
	lv_label_set_text(title_label, slash ? slash + 1 : path);
}

static void rebuild(void) {
	rebuilding = true;
	lv_obj_clean(file_list);
	empty_label = NULL;

	read_folder();
	title_refresh();
	rebuilding = false;

	if (entry_count == 0) {
		empty_label = lv_label_create(file_list);
		lv_label_set_text(empty_label, tr("files_this_folder_is_empty"));
		lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(empty_label, lv_pct(100));
		lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(empty_label, &font_ui_22, 0);
		lv_obj_set_style_pad_top(empty_label, 80, 0);
		return;
	}

	rows_add(ROWS_FIRST);
	rows_fill();
}

// The chevron goes up a folder while there is one to go up to, and off the page
// from the card's root -- which is the same thing the music browser does, and
// the only way up that does not need a second control.
static bool back_guard(void) {
	if (!name_layer || !lv_obj_is_hidden(name_layer)) {
		name_layer_hide();
		return true;
	}
	if (pick_layer && !lv_obj_is_hidden(pick_layer)) {
		// Inside the picker the chevron walks back up it, and closes it from the
		// card's root -- the same rule as the page underneath.
		if (strcmp(pick_path, root_path) != 0) {
			char *slash = strrchr(pick_path, '/');
			if (slash && slash != pick_path) {
				*slash = '\0';
				pick_rebuild();
				lv_obj_scroll_to_y(pick_list, 0, LV_ANIM_OFF);
				return true;
			}
		}
		pick_hide();
		return true;
	}
	if (strcmp(path, root_path) == 0) {
		return false;
	}

	char *slash = strrchr(path, '/');
	if (!slash || slash == path) {
		return false;
	}
	*slash = '\0';
	rebuild();
	lv_obj_scroll_to_y(file_list, 0, LV_ANIM_OFF);
	return true;
}

static bool back_guard_peek(void) {
	return (name_layer && !lv_obj_is_hidden(name_layer)) ||
		   (pick_layer && !lv_obj_is_hidden(pick_layer)) || strcmp(path, root_path) != 0;
}

static void service_toggle(void *unused) {
	(void)unused;
	config_set_int("files", "show_service", !config_get_int("files", "show_service", 0));
	config_save();
	rebuild();
}
static void view_menu_cb(lv_event_t *e) {
	if (gesture_not_tap()) return;
	(void)e;
	sx_sheet_item_t item = {.label = tr("files_show_service"), .icon = &icon_menu_settings,
		.checked = config_get_int("files", "show_service", 0) != 0, .action = service_toggle};
	sx_sheet_menu(tr("file_explorer"), NULL, &item, 1);
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	// Another page may have used the shared overlay slot while this picker
	// stayed open behind Home. Naming forms are covered by keyboard visibility.
	sx_mini_input_overlay(filespage_screen, pick_layer && !lv_obj_is_hidden(pick_layer));
	rebuild();
}

void filespage_open(void) {
	storage_card_token_t opening;
	char root[PATH_MAX_LEN];
	// Explicit entry is a new browse intent. Existing dialogs/jobs retain the
	// action snapshot they captured before any card replacement.
	if (storage_card_try_admit(&opening, root, sizeof(root)))
		snprintf(root_path, sizeof(root_path), "%s", root);
	snprintf(path, sizeof(path), "%s", root_path);
	switch_screen(filespage_screen);
}

void filespage_init(gui_config_t *cfg) {
	snprintf(root_path, sizeof(root_path), "%s", cfg->sd_root_path ? cfg->sd_root_path : "/");
	snprintf(path, sizeof(path), "%s", root_path);

	lv_obj_add_style(filespage_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(filespage_screen, cfg, "file_explorer");
	settingsrow_title_corner_slots(title_label, cfg, 2);

	// The one thing on this page that is about the folder rather than about an
	// entry in it, so it is a corner button and not a row of the menu.
	lv_obj_t *newdir_btn = lv_btn_create(filespage_screen);
	lv_obj_set_size(newdir_btn, 56, 56);
	lv_obj_align(newdir_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(newdir_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(newdir_btn, 0, 0);
	lv_obj_set_style_shadow_width(newdir_btn, 0, 0);
	lv_obj_set_style_pad_all(newdir_btn, 0, 0);
	lv_obj_add_event_cb(newdir_btn, newdir_btn_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *newdir_glyph = lv_image_create(newdir_btn);
	lv_image_set_src(newdir_glyph, &icon_folder_new);
	lv_obj_add_style(newdir_glyph, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(newdir_glyph, LV_OPA_COVER, 0);
	lv_obj_center(newdir_glyph);

	lv_obj_t *view_btn = lv_btn_create(filespage_screen);
	lv_obj_set_size(view_btn, 56, 56);
	lv_obj_align(view_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - 64, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(view_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(view_btn, 0, 0);
	lv_obj_set_style_shadow_width(view_btn, 0, 0);
	lv_obj_set_style_pad_all(view_btn, 0, 0);
	lv_obj_add_event_cb(view_btn, view_menu_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *view_glyph = lv_image_create(view_btn);
	lv_image_set_src(view_glyph, &icon_ellipsis_vertical);
	lv_obj_add_style(view_glyph, &theme_style_icon, 0);
	lv_obj_center(view_glyph);

	int content_top = settingsrow_content_top(cfg);

	file_list = lv_obj_create(filespage_screen);
	lv_obj_set_size(file_list, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(file_list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(file_list, 0, 0);
	lv_obj_set_style_border_width(file_list, 0, 0);
	lv_obj_set_style_radius(file_list, 0, 0);
	lv_obj_set_style_pad_hor(file_list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(file_list, 0, 0);
	lv_obj_set_style_pad_gap(file_list, 8, 0);
	lv_obj_set_scroll_dir(file_list, LV_DIR_VER);
	lv_obj_set_flex_flow(file_list, LV_FLEX_FLOW_COLUMN);
	// Set here and not in the row builder: an empty folder creates no rows, and
	// the page still has to answer the swipe back.
	lv_obj_set_event_bubble(file_list, true);
	lv_obj_add_event_cb(file_list, list_scrolled_cb, LV_EVENT_SCROLL, NULL);

	// --- the naming dialog, in the shape the playlist page uses
	name_layer = lv_obj_create(filespage_screen);
	lv_obj_set_size(name_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_align(name_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(name_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(name_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(name_layer, 0, 0);
	lv_obj_set_style_radius(name_layer, 0, 0);
	lv_obj_set_style_pad_all(name_layer, 0, 0);
	lv_obj_set_scrollable(name_layer, false);
	lv_obj_set_hidden(name_layer, true);

	name_heading = settingsrow_title(name_layer, cfg, "files_new_folder");

	lv_obj_t *cancel = lv_btn_create(name_layer);
	lv_obj_set_size(cancel, SX_HEADER_BTN, SX_HEADER_H);
	lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, 0, SX_STATUSBAR_H);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, name_cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	lv_obj_center(cancel_icon);

	name_field = settingsrow_name_field(name_layer, cfg, NAME_MAX);
	keyboard_style_caret(name_field);

	name_keyboard = keyboard_create(name_layer, cfg->screen_width, 316, name_field, NULL, "ok", name_accept_cb, NULL);

	// --- the destination picker: the same page, folders only, with the button
	// that says "here" pinned to the bottom.
	pick_layer = lv_obj_create(filespage_screen);
	lv_obj_set_size(pick_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_align(pick_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(pick_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(pick_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(pick_layer, 0, 0);
	lv_obj_set_style_radius(pick_layer, 0, 0);
	lv_obj_set_style_pad_all(pick_layer, 0, 0);
	lv_obj_set_scrollable(pick_layer, false);
	lv_obj_set_hidden(pick_layer, true);

	pick_title = lv_label_create(pick_layer);
	lv_label_set_text(pick_title, tr("file_explorer"));
	lv_obj_add_style(pick_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(pick_title, &font_ui_24, 0);
	lv_obj_align(pick_title, LV_ALIGN_TOP_LEFT, cfg->padding + 56 + 14, cfg->padding + cfg->top_bar_height + 10);

	lv_obj_t *pick_cancel = lv_btn_create(pick_layer);
	lv_obj_set_size(pick_cancel, 56, 56);
	lv_obj_align(pick_cancel, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(pick_cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(pick_cancel, 0, 0);
	lv_obj_set_style_shadow_width(pick_cancel, 0, 0);
	lv_obj_set_style_pad_all(pick_cancel, 0, 0);
	lv_obj_add_event_cb(pick_cancel, pick_cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *pick_cancel_icon = lv_image_create(pick_cancel);
	lv_image_set_src(pick_cancel_icon, &icon_close);
	lv_obj_add_style(pick_cancel_icon, &theme_style_icon, 0);
	lv_obj_center(pick_cancel_icon);

	// The button first, so the list can be sized against what it leaves.
	pick_here_btn = lv_btn_create(pick_layer);
	lv_obj_set_size(pick_here_btn, cfg->screen_width - 2 * cfg->padding, 64);
	lv_obj_align(pick_here_btn, LV_ALIGN_BOTTOM_MID, 0, -cfg->padding);
	lv_obj_set_style_radius(pick_here_btn, 14, 0);
	// The accent and its own ink (white on a pale accent was barely there).
	lv_obj_add_style(pick_here_btn, &sx_style_accent, 0);
	lv_obj_set_style_border_width(pick_here_btn, 0, 0);
	lv_obj_set_style_shadow_width(pick_here_btn, 0, 0);
	lv_obj_add_event_cb(pick_here_btn, pick_here_cb, LV_EVENT_CLICKED, NULL);

	pick_here_label = lv_label_create(pick_here_btn);
	lv_label_set_text(pick_here_label, tr("files_move_here"));
	lv_obj_set_style_text_font(pick_here_label, &font_ui_24, 0);
	lv_obj_center(pick_here_label);

	pick_list = lv_obj_create(pick_layer);
	lv_obj_set_size(pick_list, lv_pct(100), cfg->screen_height - content_top - 64 - 2 * cfg->padding);
	lv_obj_align(pick_list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(pick_list, 0, 0);
	lv_obj_set_style_border_width(pick_list, 0, 0);
	lv_obj_set_style_radius(pick_list, 0, 0);
	lv_obj_set_style_pad_hor(pick_list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(pick_list, 0, 0);
	lv_obj_set_style_pad_gap(pick_list, 8, 0);
	lv_obj_set_scroll_dir(pick_list, LV_DIR_VER);
	lv_obj_set_flex_flow(pick_list, LV_FLEX_FLOW_COLUMN);

	lv_obj_add_event_cb(filespage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(file_list);
	switcher_set_back_guard(filespage_screen, back_guard);
	switcher_set_back_guard_peek(filespage_screen, back_guard_peek);
}
