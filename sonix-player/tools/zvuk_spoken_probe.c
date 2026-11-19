// Read-only spoken-content contract probe. No audio media are requested.
// Stream URLs are reduced to presence flags and never included in the report.
// Uses the normal session loader and HTTP transport; never calls sign-in,
// cache/download functions or config_save. Run manually only. Actual API modes
// may invalidate their process-local session on refusal; disk removal is denied.
// Queries are evidenced in public clients, not assumed production support:
// oshokin/zvuk-grabber c9079597 internal/client/zvuk/graphql.go
// trudenboy/zvuk-music ed491f61 graphql/queries/getStream.graphql
#define _GNU_SOURCE 1
#include <errno.h>
#include <stdio.h>
static int probe_remove(const char *path) {
	(void)path;
	errno = EROFS;
	return -1;
}
#define remove probe_remove
#include "src/system/streaming/zvuk.c"
#undef remove

// No GUI/provider or token-import subsystem is started by this small process.
void zvuksync_register_provider(void) {}
void cardimport_register(const char *service, const char *file, cardimport_validate_fn validate) {
	(void)service;
	(void)file;
	(void)validate;
}
const char *tr(const char *key) { return key; }
void streamerr_hint(stream_err_t cls) { (void)cls; }
bool streamerr_online(void) { return true; }
void thread_be_background(const char *name) { (void)name; }

static FILE *report;
static void reportf(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vfprintf(report, fmt, ap);
	va_end(ap);
	fflush(report);
}

static bool probe_variables(const char *op, const char *query, const char *variables, char **body, json_doc_t *doc, int *data) {
	char url[300], payload[2048];
	snprintf(url, sizeof(url), "%s/api/v1/graphql", base_url);
	int n = snprintf(payload, sizeof(payload), "{\"operationName\":\"%s\",\"query\":\"%s\",\"variables\":%s}", op, query, variables);
	if (n < 0 || (size_t)n >= sizeof(payload))
		return false;
	// request(), unlike gql(), does not erase a refused session. Failure stops
	// this probe immediately; there are no retries through alternative routes.
	answer_t answer = request(op, url, payload, NULL, token, body, doc);
	reportf("probe operation=%s http=%d result=%d\n", op, last_status, (int)answer);
	if (answer != ANSWER_OK)
		return false;
	int root = json_root(doc);
	int errors = json_get(doc, root, "errors");
	*data = json_get(doc, root, "data");
	if (json_len(doc, errors) > 0 || *data < 0 || doc->toks[*data].type != JSON_OBJECT) {
		reportf("probe graphql_errors=%d\n", json_len(doc, errors));
		done(*body, doc);
		*body = NULL;
		return false;
	}
	return true;
}

static bool probe(const char *op, const char *query, const char *ids, char **body, json_doc_t *doc, int *data) {
	char variables[128];
	if (snprintf(variables, sizeof(variables), "{\"ids\":%s}", ids) >= (int)sizeof(variables))
		return false;
	return probe_variables(op, query, variables, body, doc, data);
}

static int probe_search(bool book) {
	const char *query = book ? "query searchBooks($query: String, $limit: Int) { search(query: $query) { books(limit: $limit) { items { __typename id title authorNames } } } }" : "query searchPodcasts($query: String, $limit: Int) { search(query: $query) { podcasts(limit: $limit) { items { __typename id title image { src } authors { id name } } } } }";
	char escaped[256], variables[320], *body = NULL;
	json_escape(book ? "Думай медленно" : "Крупным планом", escaped, sizeof(escaped));
	snprintf(variables, sizeof(variables), "{\"query\":\"%s\",\"limit\":5}", escaped);
	json_doc_t doc;
	int data;
	if (!probe_variables(book ? "searchBooks" : "searchPodcasts", query, variables, &body, &doc, &data))
		return 7;
	int array = json_path(&doc, data, book ? "search.books.items" : "search.podcasts.items");
	reportf("probe search_kind=%s count=%d\n", book ? "book" : "podcast", json_len(&doc, array));
	for (int i = 0; i < json_len(&doc, array); i++) {
		int item = json_at(&doc, array, i);
		char id[ZVUK_ID_MAX], title[256], type[64], image[2] = "";
		id_of(&doc, item, "id", id, sizeof(id));
		json_obj_str(&doc, item, "title", title, sizeof(title));
		json_obj_str(&doc, item, "__typename", type, sizeof(type));
		json_obj_str(&doc, json_get(&doc, item, "image"), "src", image, sizeof(image));
		int authors = json_get(&doc, item, book ? "authorNames" : "authors");
		reportf("probe item=%d id=%s type=%s title=%s authors_type=%d authors_count=%d has_image=%s\n", i, id, type, title, authors >= 0 ? (int)doc.toks[authors].type : -1, json_len(&doc, authors), image[0] ? "yes" : "no");
	}
	done(body, &doc);
	return 0;
}

static int probe_actual_api(const char *mode) {
	int count = -1;
	if (strcmp(mode, "bookapi") == 0 || strcmp(mode, "shortbookapi") == 0) {
		zvuk_spoken_part_t *parts = calloc(ZVUK_SPOKEN_MAX_PARTS, sizeof(*parts));
		zvuk_spoken_t book;
		count = parts ? zvuk_book_parts(strcmp(mode, "shortbookapi") == 0 ? "13072134" : "28850372", parts, ZVUK_SPOKEN_MAX_PARTS, &book) : -1;
		reportf("probe api=book http=%d count=%d\n", last_status, count);
		if (count > 0) {
			long long duration = 0;
			for (int i = 0; i < count; i++)
				duration += parts[i].track.duration;
			reportf("probe duration=%ld summed_duration=%lld\n", book.duration, duration);
			reportf("probe title=%s first=%s position=%ld last=%s position=%ld availability=%ld\n", book.title, parts[0].track.id, parts[0].source_position, parts[count - 1].track.id, parts[count - 1].source_position, parts[0].availability);
		}
		free(parts);
	} else if (strcmp(mode, "podcastapi") == 0) {
		char (*ids)[ZVUK_ID_MAX] = NULL;
		zvuk_spoken_t show;
		count = zvuk_podcast_ids("12874277", &ids, &show);
		reportf("probe api=podcast http=%d count=%d\n", last_status, count);
		if (count > 0)
			reportf("probe title=%s first=%s last=%s\n", show.title, ids[0], ids[count - 1]);
		free(ids);
	} else if (strcmp(mode, "episodesapi") == 0) {
		const char *ids[] = {"119933520", "188979372"};
		zvuk_track_t episodes[2];
		count = zvuk_episodes(ids, 2, episodes, 2);
		reportf("probe api=episodes http=%d count=%d\n", last_status, count);
		for (int i = 0; i < count; i++)
			reportf("probe id=%s kind=%d title=%s date=%s duration=%d\n", episodes[i].id, episodes[i].content, episodes[i].title, episodes[i].published_at, episodes[i].duration);
	} else if (strcmp(mode, "searchbookapi") == 0 || strcmp(mode, "searchpodcastapi") == 0) {
		bool books = strcmp(mode, "searchbookapi") == 0;
		zvuk_album_t found[5];
		count = zvuk_search(books ? "Думай медленно" : "Крупным планом", books ? ZVUK_SEARCH_BOOKS : ZVUK_SEARCH_PODCASTS, found, 5);
		reportf("probe api=%s http=%d count=%d\n", mode, last_status, count);
		for (int i = 0; i < count; i++)
			reportf("probe id=%s title=%s author=%s has_image=%s\n", found[i].id, found[i].title, found[i].artist, found[i].cover[0] ? "yes" : "no");
	} else
		return -1;
	memset(token, 0, sizeof(token));
	return count >= 0 ? 0 : 8;
}

static int probe_podcast_diagnostic(void) {
	char url[300], payload[2048], *body = NULL;
	snprintf(url, sizeof(url), "%s/api/v1/graphql", base_url);
	snprintf(payload, sizeof(payload), "{\"operationName\":\"getPodcastEpisodes\",\"query\":\"%s\",\"variables\":{\"ids\":[\"12874277\"]}}", Q_PODCAST_IDS);
	json_doc_t doc;
	answer_t answer = request("podcastdiag", url, payload, NULL, token, &body, &doc);
	reportf("probe diagnostic=podcast http=%d result=%d\n", last_status, answer);
	if (answer != ANSWER_OK)
		return 8;
	int errors = json_get(&doc, json_root(&doc), "errors");
	reportf("probe errors_count=%d\n", json_len(&doc, errors));
	for (int i = 0; i < json_len(&doc, errors) && i < 2; i++) {
		char message[256];
		json_obj_str(&doc, json_at(&doc, errors, i), "message", message, sizeof(message));
		if (strstr(message, "://") || (token[0] && strstr(message, token)))
			strcpy(message, "[redacted]");
		for (char *p = message; *p; p++)
			if ((unsigned char)*p < 0x20)
				*p = ' ';
		reportf("probe error=%s\n", message);
	}
	int parent = json_path(&doc, json_root(&doc), "data.getPodcasts.0");
	int array = json_get(&doc, parent, "episodes");
	reportf("probe parent_type=%d episodes_type=%d count=%d\n", parent < 0 ? -1 : (int)doc.toks[parent].type, array < 0 ? -1 : (int)doc.toks[array].type, json_len(&doc, array));
	int invalid = 0;
	for (int i = 0; i < json_len(&doc, array); i++) {
		int child = json_at(&doc, array, i), field = json_get(&doc, child, "id");
		char id[ZVUK_ID_MAX];
		id_of(&doc, child, "id", id, sizeof(id));
		if (!spoken_id_valid(id)) {
			if (invalid < 3)
				reportf("probe invalid_id_index=%d child_type=%d id_type=%d length=%zu\n", i, child < 0 ? -1 : (int)doc.toks[child].type, field < 0 ? -1 : (int)doc.toks[field].type, strlen(id));
			invalid++;
		}
	}
	reportf("probe invalid_ids=%d\n", invalid);
	done(body, &doc);
	return 0;
}

static bool probe_collection(const char *op, const char *query, const char *ids, const char *field, const char *children, char *child_id, size_t id_size) {
	char *body = NULL;
	json_doc_t doc;
	int data;
	if (!probe(op, query, ids, &body, &doc, &data))
		return false;
	int entries = json_get(&doc, data, field);
	int parent = json_at(&doc, entries, 0);
	if (json_len(&doc, entries) != 1 || parent < 0 || doc.toks[parent].type != JSON_OBJECT) {
		reportf("probe parent_count=%d\n", json_len(&doc, entries));
		done(body, &doc);
		return false;
	}
	char title[256], type[64];
	json_obj_str(&doc, parent, "title", title, sizeof(title));
	json_obj_str(&doc, parent, "__typename", type, sizeof(type));
	int list = json_get(&doc, parent, children);
	int count = json_len(&doc, list);
	long min_position = 0, max_position = 0;
	int duplicate_positions = 0, date_up = 0, date_down = 0;
	char previous_date[40] = "";
	reportf("probe type=%s title=%s count=%d duration=%lld\n", type, title, count, json_llong(&doc, json_get(&doc, parent, "fullDuration"), 0));
	for (int i = 0; i < count; i++) {
		int child = json_at(&doc, list, i);
		long position = json_obj_long(&doc, child, "position", -1);
		if (i == 0 || position < min_position)
			min_position = position;
		if (i == 0 || position > max_position)
			max_position = position;
		if (strcmp(children, "chapters") == 0) {
			for (int j = 0; j < i; j++)
				if (position == json_obj_long(&doc, json_at(&doc, list, j), "position", -1))
					duplicate_positions++;
		}
		char date[40] = "";
		json_obj_str(&doc, child, "publicationDate", date, sizeof(date));
		if (date[0] && previous_date[0]) {
			date_up += strcmp(date, previous_date) > 0;
			date_down += strcmp(date, previous_date) < 0;
		}
		snprintf(previous_date, sizeof(previous_date), "%s", date);
		if (i == 0)
			id_of(&doc, child, "id", child_id, id_size);
		if (i == 0 || i == count - 1) {
			char name[256], kind[64], id[ZVUK_ID_MAX];
			id_of(&doc, child, "id", id, sizeof(id));
			json_obj_str(&doc, child, "title", name, sizeof(name));
			json_obj_str(&doc, child, "__typename", kind, sizeof(kind));
			reportf("probe child=%d id=%s type=%s position=%lld duration=%lld title=%s\n", i, id, kind, json_llong(&doc, json_get(&doc, child, "position"), -1), json_llong(&doc, json_get(&doc, child, "duration"), -1), name);
			reportf("probe availability=%ld publication_date=%s\n", json_obj_long(&doc, child, "availability", -1), date);
		}
	}
	if (strcmp(children, "chapters") == 0)
		reportf("probe positions_min=%ld max=%ld duplicates=%d contiguous=%s\n", min_position, max_position, duplicate_positions, min_position >= 0 && max_position - min_position + 1 == count && !duplicate_positions ? "yes" : "no");
	else
		reportf("probe dates_ascending=%d descending=%d\n", date_up, date_down);
	done(body, &doc);
	return count > 0 && child_id[0];
}

int main(int argc, char **argv) {
	// Separate report channel: ordinary library diagnostics may include URLs.
	int report_fd = dup(STDOUT_FILENO);
	int null_fd = open("/dev/null", O_WRONLY);
	if (report_fd < 0 || null_fd < 0)
		return 1;
	report = fdopen(report_fd, "w");
	if (!report || dup2(null_fd, STDOUT_FILENO) < 0 || dup2(null_fd, STDERR_FILENO) < 0)
		return 1;
	close(null_fd);
	const char *config = argc >= 2 ? argv[1] : getenv("SONIX_CONFIG");
	if (argc > 3 || !config_init(config && config[0] ? config : "/data/device_config.ini")) {
		reportf("probe config_unavailable\n");
		return 1;
	}
	zvuk_init();
	diag_thread = session_fresh = false;
	if (!zvuk_logged_in()) {
		reportf("probe no_saved_session\n");
		return 2;
	}
	http_set_thread_timeout_cap(10);
	if (argc == 3 && strcmp(argv[2], "podcastdiag") == 0)
		return probe_podcast_diagnostic();
	if (argc == 3 && strstr(argv[2], "api"))
		return probe_actual_api(argv[2]);
	if (argc == 3 && strcmp(argv[2], "searchbooks") == 0)
		return probe_search(true);
	if (argc == 3 && strcmp(argv[2], "searchpodcasts") == 0)
		return probe_search(false);
	char chapter[ZVUK_ID_MAX] = "", episode[ZVUK_ID_MAX] = "";
	if (argc == 3 && strcmp(argv[2], "streams") == 0) {
		strcpy(chapter, "129274034");
		strcpy(episode, "119933520");
		goto streams;
	}
	if (argc == 3 && strcmp(argv[2], "dates") == 0)
		return probe_collection("getPodcastEpisodes", "query getPodcastEpisodes($ids: [ID!]!) { getPodcasts(ids: $ids) { __typename title episodes { id publicationDate } } }", "[\"12874277\"]", "getPodcasts", "episodes", episode, sizeof(episode)) ? 0 : 4;
	if (argc == 3)
		return 1;
	if (!probe_collection("getBookChapters", "query getBookChapters($ids: [ID!]!) { getBooks(ids: $ids) { __typename title fullDuration bookAuthors { rname } performers { rname } chapters { __typename id title availability duration position } } }", "[\"28850372\"]", "getBooks", "chapters", chapter, sizeof(chapter)))
		return 3;
	if (!probe_collection("getPodcastEpisodes", "query getPodcastEpisodes($ids: [ID!]!) { getPodcasts(ids: $ids) { __typename title authors { name } episodes { __typename id title availability duration publicationDate } } }", "[\"12874277\"]", "getPodcasts", "episodes", episode, sizeof(episode)))
		return 4;
streams:;
	char ids[96], *body = NULL;
	snprintf(ids, sizeof(ids), "[\"%s\",\"%s\"]", chapter, episode);
	json_doc_t doc;
	int data;
	if (!probe("spokenTypes", "query spokenTypes($ids: [ID!]!) { mediaContents(ids: $ids) { __typename ... on Chapter { stream { __typename high mid } } ... on Episode { stream { __typename high mid } } } }", ids, &body, &doc, &data))
		return 5;
	int arr = json_get(&doc, data, "mediaContents");
	bool streams_present = json_len(&doc, arr) == 2;
	for (int i = 0; i < json_len(&doc, arr); i++) {
		int entry = json_at(&doc, arr, i);
		char type[64], stream_type[64];
		json_obj_str(&doc, entry, "__typename", type, sizeof(type));
		json_obj_str(&doc, json_get(&doc, entry, "stream"), "__typename", stream_type, sizeof(stream_type));
		streams_present = streams_present && stream_type[0];
		reportf("probe media=%d type=%s stream_type=%s\n", i, type, stream_type);
		char high[2] = "", mid[2] = "";
		int stream = json_get(&doc, entry, "stream");
		json_obj_str(&doc, stream, "high", high, sizeof(high));
		json_obj_str(&doc, stream, "mid", mid, sizeof(mid));
		reportf("probe has_high=%s has_mid=%s\n", high[0] ? "yes" : "no", mid[0] ? "yes" : "no");
	}
	done(body, &doc);
	memset(token, 0, sizeof(token));
	return streams_present ? 0 : 6;
}
