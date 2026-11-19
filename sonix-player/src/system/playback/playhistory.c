#include "playhistory.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/audio/audio.h"
#include "src/system/core/utils.h"
#include "src/system/device/clock.h"
#include "src/system/library/library.h"

// One bounded queue serves both immediate history reads and background saves.
// The writer starts only when needed and sleeps without a periodic timer.
#define DEFERRED_MAX 64
#define FLUSH_BATCH 4
#define WRITE_RETRY_MS 30000
typedef struct history_pending {
	struct history_pending *next;
	long long token, id, when;
	unsigned epoch;
	bool dirty, exposed;
	char path[512], title[256], artist[256];
} history_pending_t;

static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	bool active, waiting;
	unsigned epoch;
	history_pending_t *event;
	char path[512], title[256], artist[256];
} live;
static history_pending_t *first, *last;
static unsigned queued, revision, epoch;
static long long token = -1;
static long long retry_at;
static bool write_failed, dropped;
static pthread_t writer;
static pthread_cond_t writer_wakeup = PTHREAD_COND_INITIALIZER;
static pthread_cond_t writer_idle = PTHREAD_COND_INITIALIZER;
static bool writer_started, writer_requested, writer_busy, writer_stopping;
static unsigned writer_saved;
// Keep published temporary IDs usable after the deferred nodes are freed.
static struct {
	long long token, id;
} aliases[DEFERRED_MAX];
static unsigned alias_next;

static long long monotonic_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void unlink_locked(history_pending_t **link) {
	history_pending_t *entry = *link;
	*link = entry->next;
	if (last == entry) {
		last = first;
		while (last && last->next)
			last = last->next;
	}
	if (live.event == entry)
		live.event = NULL;
	free(entry);
	queued--;
}

static void sync_epoch_locked(void) {
	unsigned current = library_database_epoch();
	if (current == epoch)
		return;
	while (first)
		unlink_locked(&first);
	epoch = current;
	retry_at = 0;
	write_failed = dropped = false;
	memset(aliases, 0, sizeof(aliases));
	alias_next = 0;
	if (live.epoch != current)
		live.active = false;
	revision++;
}

// The writer owns one immutable snapshot while SQLite works. Never hold the
// live/history lock across a card write: playback and metadata must stay usable.
// Epoch checks inside the library keep an old save out of a newly opened card.
static unsigned flush_batch_locked(void) {
	sync_epoch_locked();
	if (!epoch || (retry_at && monotonic_ms() < retry_at))
		return 0;
	unsigned budget = FLUSH_BATCH;
	unsigned saved = 0;
	history_pending_t **link = &first;
	while (*link && budget) {
		history_pending_t *entry = *link;
		if (!entry->dirty && entry->id > 0) {
			if (entry != live.event) {
				unlink_locked(link);
				continue;
			}
			link = &entry->next;
			continue;
		}
		history_pending_t writing = *entry;
		pthread_mutex_unlock(&live_lock);
		long long result = writing.id > 0 ? library_history_update_at(writing.epoch, writing.id, writing.path, writing.title, writing.artist) : library_history_record_at(writing.epoch, writing.path, writing.title, writing.artist, writing.when);
		pthread_mutex_lock(&live_lock);
		budget--;
		sync_epoch_locked();
		// The old node may have been evicted or the whole card replaced while
		// the lock was released. Find it again; never retain a borrowed pointer.
		link = &first;
		while (*link && (*link)->token != writing.token)
			link = &(*link)->next;
		if (!*link || epoch != writing.epoch) {
			link = &first;
			continue;
		}
		entry = *link;
		if (result < 0)
			return saved; // A foreign transaction is retried by an existing poll.
		if (!result) {
			if (!write_failed)
				fprintf(stderr, "history: save deferred after a storage failure\n");
			write_failed = true;
			retry_at = monotonic_ms() + WRITE_RETRY_MS;
			return saved;
		}
		if (entry->id <= 0) {
			entry->id = result;
			if (entry->exposed) {
				aliases[alias_next].token = entry->token;
				aliases[alias_next].id = result;
				alias_next = (alias_next + 1) % DEFERRED_MAX;
			}
		}
		// Lazy metadata may have arrived during the write. Keep that newer
		// version dirty, but retain the inserted id so it is updated, not added.
		entry->dirty = strcmp(entry->title, writing.title) || strcmp(entry->artist, writing.artist);
		write_failed = false;
		retry_at = 0;
		revision++;
		saved++;
		if (entry != live.event && !entry->dirty)
			unlink_locked(link);
		else
			link = &entry->next;
	}
	return saved;
}

static bool dirty_locked(void) {
	bool dirty = false;
	history_pending_t **link = &first;
	while (*link) {
		history_pending_t *entry = *link;
		if (!entry->dirty && entry->id > 0 && entry != live.event) {
			unlink_locked(link);
			continue;
		}
		dirty |= entry->dirty || entry->id <= 0;
		link = &entry->next;
	}
	return dirty;
}

static void *history_writer(void *unused) {
	(void)unused;
	thread_be_low_priority("history save");
	pthread_mutex_lock(&live_lock);
	for (;;) {
		while (!writer_requested && !writer_stopping)
			pthread_cond_wait(&writer_wakeup, &live_lock);
		if (writer_stopping)
			break;
		writer_requested = false;
		writer_busy = true;
		writer_saved = flush_batch_locked();
		writer_busy = false;
		pthread_cond_broadcast(&writer_idle);
	}
	pthread_mutex_unlock(&live_lock);
	return NULL;
}

static void request_flush_locked(void) {
	sync_epoch_locked();
	if (writer_stopping || writer_busy || writer_requested || !epoch || !dirty_locked() || (retry_at && monotonic_ms() < retry_at))
		return;
	if (!writer_started) {
		if (pthread_create(&writer, NULL, history_writer, NULL) != 0) {
			write_failed = true;
			retry_at = monotonic_ms() + WRITE_RETRY_MS;
			return; // Never perform the write inline on a failed worker start.
		}
		writer_started = true;
	}
	writer_requested = true;
	pthread_cond_signal(&writer_wakeup);
}

// A consistency barrier for an explicit history read or storage/power exit.
// Ordinary playback only requests a write and never waits for this worker.
void playhistory_flush(void) {
	pthread_mutex_lock(&live_lock);
	for (unsigned n = 0; n <= DEFERRED_MAX / FLUSH_BATCH; n++) {
		request_flush_locked();
		bool requested = writer_busy || writer_requested;
		while (writer_busy || writer_requested)
			pthread_cond_wait(&writer_idle, &live_lock);
		if (!requested || !writer_saved || !dirty_locked())
			break;
	}
	pthread_mutex_unlock(&live_lock);
}

void playhistory_shutdown(void) {
	playhistory_flush();
	pthread_mutex_lock(&live_lock);
	writer_stopping = true;
	bool started = writer_started;
	pthread_cond_signal(&writer_wakeup);
	pthread_mutex_unlock(&live_lock);
	if (started)
		pthread_join(writer, NULL);
}

static void heard_locked(const char *path) {
	sync_epoch_locked();
	if (!live.active || live.waiting || live.event || !epoch || live.epoch != epoch || !path || strcmp(path, live.path))
		return;
	if (queued == DEFERRED_MAX) {
		// Keep the newest events if storage remains unavailable for many starts.
		// The UI reports that some history could not be saved.
		if (first->id <= 0 || first->dirty) {
			dropped = true;
			fprintf(stderr, "history: pending history limit reached\n");
		}
		unlink_locked(&first);
	}
	history_pending_t *entry = calloc(1, sizeof(*entry));
	if (!entry) {
		dropped = true;
		return;
	}
	entry->token = token--;
	entry->epoch = epoch;
	entry->when = clock_is_set() ? (long long)time(NULL) : 0;
	entry->dirty = true;
	snprintf(entry->path, sizeof(entry->path), "%s", live.path);
	snprintf(entry->title, sizeof(entry->title), "%s", live.title);
	snprintf(entry->artist, sizeof(entry->artist), "%s", live.artist);
	if (last)
		last->next = entry;
	else
		first = entry;
	last = live.event = entry;
	queued++;
	revision++;
}

void playhistory_started(const char *path, const char *title, const char *artist, bool kept_out, bool waiting) {
	unsigned started_epoch = library_database_epoch();
	bool counts = path && path[0] && !kept_out;
	char shown[sizeof(live.title)];
	if (!counts || !library_track_title(path, shown, sizeof(shown)))
		snprintf(shown, sizeof(shown), "%s", title ? title : "");
	if (counts && !shown[0]) {
		const char *base = strrchr(path, '/');
		snprintf(shown, sizeof(shown), "%s", base ? base + 1 : path);
	}
	pthread_mutex_lock(&live_lock);
	sync_epoch_locked();
	if (counts && live.active && live.epoch == epoch && !strcmp(path, live.path)) {
		pthread_mutex_unlock(&live_lock);
		playhistory_metadata(path, shown, artist);
		return; // The caller uses left() for a new listen, not a seek/resume.
	}
	live.active = counts && epoch && started_epoch == epoch;
	live.waiting = waiting;
	live.epoch = started_epoch;
	live.event = NULL;
	snprintf(live.path, sizeof(live.path), "%s", path ? path : "");
	snprintf(live.title, sizeof(live.title), "%s", shown);
	snprintf(live.artist, sizeof(live.artist), "%s", artist ? artist : "");
	revision++;
	request_flush_locked();
	pthread_mutex_unlock(&live_lock);
}

void playhistory_resumed(void) {
	pthread_mutex_lock(&live_lock);
	live.waiting = false;
	pthread_mutex_unlock(&live_lock);
}

void playhistory_heard(const char *path) {
	pthread_mutex_lock(&live_lock);
	heard_locked(path);
	request_flush_locked();
	pthread_mutex_unlock(&live_lock);
}

void playhistory_poll(void) {
	pthread_mutex_lock(&live_lock);
	request_flush_locked();
	pthread_mutex_unlock(&live_lock);
}

void playhistory_metadata(const char *path, const char *title, const char *artist) {
	pthread_mutex_lock(&live_lock);
	sync_epoch_locked();
	if (live.active && path && !strcmp(path, live.path)) {
		bool changed = false;
		if (title && title[0] && strcmp(title, live.title)) {
			snprintf(live.title, sizeof(live.title), "%s", title);
			changed = true;
		}
		if (artist && artist[0] && strcmp(artist, live.artist)) {
			snprintf(live.artist, sizeof(live.artist), "%s", artist);
			changed = true;
		}
		if (changed) {
			if (live.event) {
				snprintf(live.event->title, sizeof(live.event->title), "%s", live.title);
				snprintf(live.event->artist, sizeof(live.event->artist), "%s", live.artist);
				live.event->dirty = true;
			}
			revision++;
		}
	}
	pthread_mutex_unlock(&live_lock);
}

void playhistory_left(void) {
	pthread_mutex_lock(&live_lock);
	// Catch a short successful listen between two ordinary GUI polls. The audio
	// engine never calls history while holding audio_mutex; getters release it
	// before any SQLite operation. A serial brackets the file identity.
	if (live.active && !live.waiting && !live.event) {
		unsigned before, after;
		char path[512];
		bool ready = audio_get_listening_progress(NULL, NULL, &before);
		audio_get_current_file(path, sizeof(path));
		ready = audio_get_listening_progress(NULL, NULL, &after) && ready;
		if (ready && before == after)
			heard_locked(path);
	}
	live.active = false;
	live.event = NULL;
	revision++;
	request_flush_locked();
	pthread_mutex_unlock(&live_lock);
}

long long playhistory_current_id(void) {
	pthread_mutex_lock(&live_lock);
	sync_epoch_locked();
	long long id = live.active && live.event ? (live.event->id > 0 ? live.event->id : live.event->token) : 0;
	if (id < 0)
		live.event->exposed = true;
	pthread_mutex_unlock(&live_lock);
	return id;
}

unsigned playhistory_revision(void) {
	pthread_mutex_lock(&live_lock);
	sync_epoch_locked();
	unsigned value = revision;
	pthread_mutex_unlock(&live_lock);
	return value;
}

bool playhistory_unsaved(void) {
	pthread_mutex_lock(&live_lock);
	sync_epoch_locked();
	bool warning = write_failed || dropped;
	pthread_mutex_unlock(&live_lock);
	return warning;
}

int playhistory_items(library_history_item_t **out) {
	if (!out)
		return 0;
	*out = NULL;
	// Preserve a coherent DB + pending view even if an INSERT just finished
	// and its temporary id has not yet been acknowledged under live_lock.
	playhistory_flush();
	pthread_mutex_lock(&live_lock);
	while (writer_busy || writer_requested)
		pthread_cond_wait(&writer_idle, &live_lock);
	sync_epoch_locked();
	int count = library_history_items(out);
	unsigned pending = 0;
	for (history_pending_t *entry = first; entry; entry = entry->next)
		pending += entry->id <= 0;
	if (pending) {
		library_history_item_t *grown = realloc(*out, ((size_t)count + pending) * sizeof(**out));
		if (grown) {
			*out = grown;
			for (history_pending_t *entry = first; entry; entry = entry->next)
				if (entry->id <= 0) {
					entry->exposed = true;
					grown[count++] = (library_history_item_t){.id = entry->token, .played_at = entry->when};
				}
		}
	}
	if (count > LIBRARY_HISTORY_MAX) {
		memmove(*out, *out + count - LIBRARY_HISTORY_MAX, LIBRARY_HISTORY_MAX * sizeof(**out));
		count = LIBRARY_HISTORY_MAX;
	}
	if (epoch != library_database_epoch()) {
		free(*out);
		*out = NULL;
		count = 0;
		sync_epoch_locked();
	}
	pthread_mutex_unlock(&live_lock);
	return count;
}

bool playhistory_entry(long long id, char *path, size_t path_size, char *name, size_t name_size, char *artist, size_t artist_size) {
	pthread_mutex_lock(&live_lock);
	sync_epoch_locked();
	bool found = false;
	for (history_pending_t *entry = first; entry; entry = entry->next) {
		if (entry->id != id && entry->token != id)
			continue;
		if (path && path_size)
			snprintf(path, path_size, "%s", entry->path);
		if (name && name_size)
			snprintf(name, name_size, "%s", entry->title);
		if (artist && artist_size)
			snprintf(artist, artist_size, "%s", entry->artist);
		found = true;
		break;
	}
	if (!found && id < 0) {
		for (unsigned i = 0; i < DEFERRED_MAX; i++)
			if (aliases[i].token == id) {
				id = aliases[i].id;
				break;
			}
	}
	if (!found && id > 0)
		found = library_history_entry_at(epoch, id, path, path_size, name, name_size, artist, artist_size);
	pthread_mutex_unlock(&live_lock);
	return found;
}
