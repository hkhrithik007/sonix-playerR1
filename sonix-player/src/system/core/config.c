#include "config.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// A player's settings are a handful of short strings, so a fixed table keeps
// the whole thing allocation-free, which matters more on a device with ten
// megabytes free than an unbounded number of entries would.
//
// A full table is not an error anybody sees: config_set() drops every further
// key with only a line in the log, and the symptom is a setting that will not
// stick -- the switch moves, the value is never stored, and the getter keeps
// returning its default. So the size is counted rather than guessed: 126 keys
// written as literals in the source, plus band0..9 for the equaliser and v0..9
// for MSEB, is about 146 fixed entries. The only thing that would grow on top
// of that is the EPUB reader, which stores one entry per book opened and has a
// file of its own (see below), so 384 leaves room to spare.
#define CONFIG_MAX_ENTRIES 384

// The reader's own file. Its entries are one per book ever opened, so it grows
// with the shelf rather than with the player, which is the whole reason it is
// not in the table above. Five hundred books is a shelf nobody has on a
// forty-gigabyte card, and the table costs 192 bytes an entry.
#define EBOOK_MAX_ENTRIES 512

#define CONFIG_SECTION_LEN 32
#define CONFIG_KEY_LEN 32
#define CONFIG_VALUE_LEN 128

typedef struct {
	char section[CONFIG_SECTION_LEN];
	char key[CONFIG_KEY_LEN];
	char value[CONFIG_VALUE_LEN];
} config_entry_t;

// Two files, one piece of code. The settings and the reader's per-book state
// have nothing to do with each other and no reason to share a table, but they
// have exactly the same shape on disk -- so the store is a struct and the
// functions everything else calls are the same functions pointed at the main
// one.
struct config_store {
	config_entry_t *entries;
	int capacity;
	int count;
	char path[512];
	bool dirty;
	uint64_t revision;
	const char *name; // what the log calls it
};

static config_entry_t main_entries[CONFIG_MAX_ENTRIES];
static config_entry_t ebook_entries[EBOOK_MAX_ENTRIES];

static config_store_t g_main = {.entries = main_entries, .capacity = CONFIG_MAX_ENTRIES, .name = "config"};
static config_store_t g_ebook = {.entries = ebook_entries, .capacity = EBOOK_MAX_ENTRIES, .name = "ebook config"};

// RAM access is short; slow saves hold only io_lock, never table_lock.
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;
static bool set_locked(config_store_t *, const char *, const char *, const char *);

config_store_t *config_main_store(void) { return &g_main; }
config_store_t *config_ebook_store(void) { return &g_ebook; }

const char *config_store_path_copy(const config_store_t *store, char *out, size_t size) {
	if (!store || !out || !size)
		return NULL;
	pthread_mutex_lock(&table_lock);
	bool present = store->path[0];
	snprintf(out, size, "%s", store->path);
	pthread_mutex_unlock(&table_lock);
	return present ? out : NULL;
}
const char *config_path_copy(char *out, size_t size) { return config_store_path_copy(&g_main, out, size); }

// Trims leading and trailing whitespace in place, returning the new start.
static char *trim(char *s) {
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
		s++;
	}

	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
		end--;
	}
	*end = '\0';

	return s;
}

static config_entry_t *find(config_store_t *store, const char *section, const char *key) {
	for (int i = 0; i < store->count; i++) {
		if (strcmp(store->entries[i].section, section) == 0 && strcmp(store->entries[i].key, key) == 0) {
			return &store->entries[i];
		}
	}
	return NULL;
}

static bool init_locked(config_store_t *store, const char *path) {
	store->revision++;
	store->count = 0;
	store->dirty = false;
	store->path[0] = '\0';

	if (!path || !path[0]) {
		return false;
	}
	snprintf(store->path, sizeof(store->path), "%s", path);

	FILE *f = fopen(store->path, "r");
	if (!f) {
		printf("%s: %s not there yet, starting from defaults\n", store->name, store->path);
		return false;
	}

	char line[256];
	char section[CONFIG_SECTION_LEN] = "";

	while (fgets(line, sizeof(line), f)) {
		char *text = trim(line);

		if (text[0] == '\0' || text[0] == '#' || text[0] == ';') {
			continue; // blank or comment
		}

		if (text[0] == '[') {
			char *close = strchr(text, ']');
			if (close) {
				*close = '\0';
				snprintf(section, sizeof(section), "%s", trim(text + 1));
			}
			continue;
		}

		char *equals = strchr(text, '=');
		if (!equals) {
			continue; // not a key/value line; ignore rather than fail the load
		}

		*equals = '\0';
		set_locked(store, section, trim(text), trim(equals + 1));
	}

	fclose(f);
	store->dirty = false; // what was just read is by definition already on disk

	printf("%s: loaded %d settings from %s\n", store->name, store->count, store->path);
	return true;
}

bool config_store_init(config_store_t *store, const char *path) {
	if (!store)
		return false;
	pthread_mutex_lock(&io_lock);
	pthread_mutex_lock(&table_lock);
	bool ok = init_locked(store, path);
	pthread_mutex_unlock(&table_lock);
	pthread_mutex_unlock(&io_lock);
	return ok;
}

bool config_init(const char *path) { return config_store_init(&g_main, path); }

// The result belongs to the caller and cannot change under another writer.
const char *config_store_get_copy(config_store_t *store, const char *section, const char *key, const char *fallback, char *out, size_t size) {
	if (!store || !section || !key || !out || !size)
		return NULL;
	pthread_mutex_lock(&table_lock);
	config_entry_t *entry = find(store, section, key);
	const char *value = entry ? entry->value : fallback;
	if (value)
		snprintf(out, size, "%s", value);
	else
		out[0] = '\0';
	pthread_mutex_unlock(&table_lock);
	return value ? out : NULL;
}

long config_store_get_int(config_store_t *store, const char *section, const char *key, long fallback) {
	char text[CONFIG_VALUE_LEN];
	const char *value = config_store_get_copy(store, section, key, NULL, text, sizeof(text));
	if (!value || !value[0])
		return fallback;
	char *end = NULL;
	long parsed = strtol(value, &end, 10);
	return (end && end != value) ? parsed : fallback;
}

// Called only with table_lock held. False leaves the original entry intact.
static bool set_locked(config_store_t *store, const char *section, const char *key, const char *value) {
	config_entry_t *entry = find(store, section, key);
	if (entry) {
		if (strcmp(entry->value, value) == 0)
			return true;
	} else {
		if (store->count >= store->capacity) {
			fprintf(stderr, "%s: no room for %s/%s\n", store->name, section, key);
			return false;
		}
		entry = &store->entries[store->count++];
		snprintf(entry->section, sizeof(entry->section), "%s", section);
		snprintf(entry->key, sizeof(entry->key), "%s", key);
	}
	snprintf(entry->value, sizeof(entry->value), "%s", value);
	store->dirty = true;
	store->revision++;
	return true;
}

void config_store_set(config_store_t *store, const char *section, const char *key, const char *value) {
	if (!store || !section || !key || !value)
		return;
	pthread_mutex_lock(&table_lock);
	set_locked(store, section, key, value);
	pthread_mutex_unlock(&table_lock);
}

void config_store_set_int(config_store_t *store, const char *section, const char *key, long value) {
	char text[32];
	snprintf(text, sizeof(text), "%ld", value);
	config_store_set(store, section, key, text);
}

const char *config_get_copy(const char *section, const char *key, const char *fallback, char *out, size_t size) { return config_store_get_copy(&g_main, section, key, fallback, out, size); }

long config_get_int(const char *section, const char *key, long fallback) {
	return config_store_get_int(&g_main, section, key, fallback);
}

bool config_get_bool(const char *section, const char *key, bool fallback) {
	char text[CONFIG_VALUE_LEN];
	const char *value = config_get_copy(section, key, NULL, text, sizeof(text));
	if (!value || !value[0]) {
		return fallback;
	}

	return value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
}

void config_set(const char *section, const char *key, const char *value) {
	config_store_set(&g_main, section, key, value);
}

void config_set_int(const char *section, const char *key, long value) {
	config_store_set_int(&g_main, section, key, value);
}

void config_set_bool(const char *section, const char *key, bool value) {
	config_set(section, key, value ? "1" : "0");
}

// Moves a whole section from one store to the other, leaving nothing behind.
// One use: the reader's settings that a config written by an older build still
// holds in device_config.ini, which would otherwise be lost -- every reading
// position and every choice -- now that the reader has a file of its own.
int config_store_move_section(config_store_t *from, config_store_t *to, const char *section) {
	if (!from || !to || !section || from == to)
		return 0;
	pthread_mutex_lock(&table_lock);
	uint64_t original_revision = from->revision;
	bool complete = true;
	for (int i = 0; i < from->count; i++) {
		config_entry_t *e = &from->entries[i];
		if (!strcmp(e->section, section) && !set_locked(to, section, e->key, e->value))
			complete = false;
	}
	uint64_t destination_revision = to->revision;
	pthread_mutex_unlock(&table_lock);
	if (!complete || !config_store_save(to))
		return 0;
	pthread_mutex_lock(&table_lock);
	if (from->revision != original_revision || to->revision != destination_revision || to->dirty) {
		pthread_mutex_unlock(&table_lock);
		return 0;
	}
	int moved = 0, kept = 0;
	for (int i = 0; i < from->count; i++) {
		if (!strcmp(from->entries[i].section, section)) {
			moved++;
			continue;
		}
		from->entries[kept++] = from->entries[i];
	}
	if (moved) {
		from->count = kept;
		from->dirty = true;
		from->revision++;
	}
	pthread_mutex_unlock(&table_lock);
	return moved;
}

// Atomic replacement plus checked file/directory sync. A failed save remains
// dirty; a concurrent RAM update is never acknowledged by an older snapshot.
bool config_store_save(config_store_t *store) {
	if (!store)
		return false;
	pthread_mutex_lock(&io_lock);
	pthread_mutex_lock(&table_lock);
	if (!store->path[0] || !store->dirty) {
		pthread_mutex_unlock(&table_lock);
		pthread_mutex_unlock(&io_lock);
		return true;
	}
	int count = store->count;
	uint64_t revision = store->revision;
	char path[sizeof(store->path)];
	snprintf(path, sizeof(path), "%s", store->path);
	config_entry_t *entries = malloc((count ? count : 1) * sizeof(*entries));
	if (entries)
		memcpy(entries, store->entries, count * sizeof(*entries));
	pthread_mutex_unlock(&table_lock);
	bool ok = false;
	if (!entries)
		goto done;
	char temp[560], directory[512];
	snprintf(temp, sizeof(temp), "%s.tmp", path);
	snprintf(directory, sizeof(directory), "%s", path);
	char *slash = strrchr(directory, '/');
	if (!slash)
		snprintf(directory, sizeof(directory), ".");
	else if (slash == directory)
		slash[1] = '\0';
	else
		*slash = '\0';
	FILE *f = fopen(temp, "w");
	if (!f)
		goto done;
	fprintf(f, "# sonix_player settings. Rewritten by the player; hand edits are read back.\n");
	for (int i = 0; i < count; i++) {
		bool started = false;
		for (int j = 0; j < i; j++) {
			if (!strcmp(entries[j].section, entries[i].section)) {
				started = true;
				break;
			}
		}
		if (started)
			continue;
		fprintf(f, "\n[%s]\n", entries[i].section);
		for (int j = i; j < count; j++) {
			if (!strcmp(entries[j].section, entries[i].section))
				fprintf(f, "%s = %s\n", entries[j].key, entries[j].value);
		}
	}
	ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
	if (fclose(f) != 0)
		ok = false;
	if (ok)
		ok = rename(temp, path) == 0;
	if (ok) {
		int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		ok = fd >= 0;
		if (fd >= 0) {
			if (fsync(fd) != 0)
				ok = false;
			if (close(fd) != 0)
				ok = false;
		}
	}
	if (!ok)
		unlink(temp);
	if (ok) {
		pthread_mutex_lock(&table_lock);
		if (store->revision == revision)
			store->dirty = false;
		pthread_mutex_unlock(&table_lock);
	}
done:
	free(entries);
	if (!ok)
		fprintf(stderr, "%s: settings save failed; changes remain dirty\n", store->name);
	pthread_mutex_unlock(&io_lock);
	return ok;
}

bool config_save(void) { return config_store_save(&g_main); }
