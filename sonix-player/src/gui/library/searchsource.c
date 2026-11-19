#include "searchsource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool search_result_add(search_result_t *out, int kind, const char *id, const char *name, const char *detail, const void *data, size_t size) {
	if (out->count >= SEARCH_HITS_MAX || kind < 0 || kind >= SEARCH_KIND_COUNT)
		return false;
	void *copy = NULL;
	if (size) {
		copy = malloc(size);
		if (!copy) {
			out->failed = true;
			return false;
		}
		memcpy(copy, data, size);
	}
	search_hit_t *hit = &out->hits[out->count++];
	hit->kind = kind;
	hit->data = copy;
	snprintf(hit->id, sizeof(hit->id), "%s", id ? id : "");
	snprintf(hit->name, sizeof(hit->name), "%s", name ? name : "");
	snprintf(hit->artist, sizeof(hit->artist), "%s", detail ? detail : "");
	return true;
}

void search_result_free(search_result_t *result) {
	if (!result)
		return;
	for (int i = 0; i < result->count; i++)
		free(result->hits[i].data);
	free(result);
}
