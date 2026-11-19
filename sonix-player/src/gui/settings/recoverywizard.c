#include "recoverywizard.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_mini.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/device/recoveryprep.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

static lv_obj_t *screen, *status, *action;
static lv_timer_t *timer;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool busy, done;
static bool uncertain;
static int operation;
static recovery_prep_result_t result;

bool recoverywizard_busy(void) { return busy || uncertain; }
static bool guard(void) { return busy || uncertain; }
static void start(int op);

static void *worker(void *arg) {
	int op = (int)(intptr_t)arg;
	recovery_prep_result_t answer = op == 0 ? recovery_prep_check() : op == 1 ? recovery_prep_backup() : recovery_prep_apply();
	pthread_mutex_lock(&lock);
	result = answer;
	done = true;
	pthread_mutex_unlock(&lock);
	return NULL;
}

static void apply_confirmed(void *unused) {
	(void)unused;
	start(2);
}

static void action_clicked(lv_event_t *e) {
	(void)e;
	if (busy)
		return;
	if (operation == 0 && result == RECOVERY_PREP_READY) {
		start(1);
	} else if (result == RECOVERY_PREP_BACKED_UP) {
		confirm_show("recovery_wizard_title", "recovery_wizard_confirm", "recovery_wizard_apply", apply_confirmed, NULL);
	} else {
		start(0);
	}
}

static void poll(lv_timer_t *t) {
	pthread_mutex_lock(&lock);
	bool complete = done;
	done = false;
	recovery_prep_result_t answer = result;
	pthread_mutex_unlock(&lock);
	if (!complete)
		return;
	busy = false;
	uncertain = answer == RECOVERY_PREP_UNCERTAIN;
	recovery_prep_set_active(uncertain);
	if (uncertain)
		power_set_auto_off(false, 0);
	power_hold_screen_on(uncertain);
	lv_timer_pause(t);
	const char *key = answer == RECOVERY_PREP_READY ? "recovery_wizard_ready" : answer == RECOVERY_PREP_FIXED ? "recovery_wizard_fixed" : answer == RECOVERY_PREP_BACKED_UP ? "recovery_wizard_backup_done" : answer == RECOVERY_PREP_UNCERTAIN ? "recovery_wizard_uncertain" : answer == RECOVERY_PREP_UNSUPPORTED ? "recovery_wizard_unsupported" : "recovery_wizard_failed";
	lv_label_set_text(status, tr(key));
	lv_obj_set_hidden(action, answer == RECOVERY_PREP_FIXED || answer == RECOVERY_PREP_UNCERTAIN || answer == RECOVERY_PREP_UNSUPPORTED);
}

static void start(int op) {
	if (busy || uncertain || !timer)
		return;
	if (op == 2) {
		sync_battery_from_sysfs();
		const char *level = read_battery_percent();
		char *end;
		long percent = level ? strtol(level, &end, 10) : -1;
		if (!level || end == level || *end || percent < 50 || percent > 100 || !usb_vbus_present()) {
			confirm_notice("recovery_wizard_title", "recovery_wizard_power");
			return;
		}
	}
	operation = op;
	busy = true;
	recovery_prep_set_active(true);
	done = false;
	pthread_t thread;
	if (pthread_create(&thread, NULL, worker, (void *)(intptr_t)op) != 0) {
		busy = false;
		recovery_prep_set_active(false);
		lv_label_set_text(status, tr("recovery_wizard_failed"));
		return;
	}
	pthread_detach(thread);
	power_hold_screen_on(true);
	lv_obj_set_hidden(action, true);
	lv_label_set_text(status, tr(op == 0 ? "recovery_wizard_checking" : op == 1 ? "recovery_wizard_saving" : "recovery_wizard_applying"));
	lv_timer_resume(timer);
}

void recoverywizard_open(void *unused) {
	(void)unused;
	switch_screen(screen);
	if (!busy)
		start(0);
}

static void build_page(gui_config_t *cfg) {
	lv_obj_t *content = settingsrow_page(screen, cfg, "recovery_wizard_title");
	lv_obj_t *card = sx_card_create(content);
	sx_card_title(card, tr("recovery_wizard_title"));
	sx_card_subtitle(card, tr("recovery_wizard_intro"), false);
	status = sx_card_subtitle(sx_card_create(content), tr("recovery_wizard_checking"), false);
	action = settingsrow_action(content, "recovery_wizard_continue", action_clicked, NULL);
	timer = lv_timer_create(poll, 100, NULL);
	if (timer)
		lv_timer_pause(timer);
	sx_mini_hide_on_screen(screen);
	switcher_set_back_guard(screen, guard);
	switcher_set_back_guard_peek(screen, guard);
	switcher_attach_back_gesture(screen);
}

void recoverywizard_init(gui_config_t *cfg) {
	(void)cfg;
	screen = lv_obj_create(NULL);
	gui_page_defer(screen, build_page);
}
