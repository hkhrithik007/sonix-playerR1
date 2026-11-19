#ifndef PODCASTIMPORT_H
#define PODCASTIMPORT_H

#include <stdbool.h>
#include <stddef.h>

// Provider episodes use the existing local Podcast shelf, one folder per
// show. IDs disambiguate equal titles across shows and services. Pure naming:
// the downloader owns mkdir, the audio temp file, cancellation and its pin.
bool podcastimport_paths(const char *card_root, const char *service, const char *show_id, const char *show_title, const char *episode_id, const char *episode_title, char *folder, size_t folder_size, char *base, size_t base_size);

// Worker only. Publishes display tags plus provider identity under a short
// storage pin. Never writes Podcast Index feed_id/episode_id or Music's DB.
// published_at is an optional ISO date; it becomes the audio file's mtime.
// Accepts a completed hidden .<stem>.part too: caller publishes its .tags
// first, then renames the audio as the last visible commit.
// False means no new sidecar was published (or its date could not be set).
bool podcastimport_write_tags(const char *path, const char *service, const char *show_id, const char *show_title, const char *episode_id, const char *episode_title, const char *artist, const char *cover_url, const char *published_at);

// Stable local resume identity, independent of filenames and of the numeric
// Podcast Index namespace. False for ordinary local/catalogue episodes.
bool podcastimport_key(const char *path, char *out, size_t size);

#endif
