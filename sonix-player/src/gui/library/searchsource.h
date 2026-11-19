#ifndef SEARCHSOURCE_H
#define SEARCHSOURCE_H

#include "src/gui/shell/main_menu.h"
#include <stddef.h>

// Bounded overview contract. The coordinator never knows native service types.
// A source owns its client, account, cache and navigation. No network on the UI
// thread; fetch runs on a lazy worker and must check current between requests.
#define SEARCH_SOURCE_MAX 6
#define SEARCH_PREVIEW 3
#define SEARCH_HITS_MAX (3 * SEARCH_PREVIEW)
#define SEARCH_KIND_COUNT 4
typedef enum { SEARCH_TRACK, SEARCH_ALBUM, SEARCH_ARTIST, SEARCH_PODCAST } search_kind_t;
typedef struct {
	int kind;
	char id[128], name[256], artist[160];
	void *data; // Owned native item, released with this result snapshot.
} search_hit_t;
typedef struct {
	unsigned serial, epoch;
	int source, count;
	bool failed;
	search_hit_t hits[SEARCH_HITS_MAX];
} search_result_t;
typedef struct {
	home_item_t home;
	const char *name;
	unsigned kinds;
	bool (*ready)(void); // Memory only, never a network probe.
	void (*fetch)(const char *query, search_result_t *out, bool (*current)(unsigned));
	void (*more)(const char *query, int kind);
	void (*open)(const search_result_t *result, int index, const char *query);
	bool (*path)(const search_hit_t *hit, char *out, size_t size);
} search_source_t;

// Immutable source table; the picker and coordinator use the same availability.
int search_source_count(void);
const search_source_t *search_source_at(int index);
const search_source_t *search_source_home(home_item_t home);
bool search_result_add(search_result_t *out, int kind, const char *id, const char *name, const char *detail, const void *data, size_t size);
void search_result_free(search_result_t *result);
#endif
