#include "src/system/core/config.h"
// Read-only metadata contract probe for the two user-supplied Yandex examples.
// Uses the normal config/session reader, HTTP transport and complete parser.
// Does not call api(): its automatic token refresh can write the session.
// No audio/file-info requests, credential output, login/logout or config saves.
#include "src/system/streaming/yandex.c"
#include <fcntl.h>

const char *tr(const char *key) { return key; }

static FILE *report;

static const char *kind_name(yandex_content_t kind) {
	switch (kind) {
	case YANDEX_CONTENT_AUDIOBOOK:
		return "audiobook";
	case YANDEX_CONTENT_PODCAST:
		return "podcast";
	case YANDEX_CONTENT_EPISODE:
		return "podcast-episode";
	case YANDEX_CONTENT_MUSIC:
		return "music";
	default:
		return "unknown";
	}
}

static bool inspect_album(const char *id) {
	char url[400], headers[YANDEX_TOKEN_MAX + 64];
	snprintf(url, sizeof(url), "%s/albums/%s/with-tracks", api_url, id);
	snprintf(headers, sizeof(headers), "Authorization: OAuth %s\r\n", access_token);
	http_req_t request = {.method = "GET", .extra_headers = headers};
	char *body = NULL;
	int status = 0;
	bool fetched = http_request(url, &request, &body, NULL, YANDEX_BODY_LIMIT, 10, &status);
	memset(headers, 0, sizeof(headers));
	if (!fetched || status < 200 || status >= 300) {
		fprintf(report, "metadata http=%d success=no\n", status);
		free(body);
		return false;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		fputs("metadata parse=no\n", report);
		return false;
	}
	yandex_track_t *tracks = calloc(300, sizeof(*tracks));
	yandex_album_meta_t album;
	int count = tracks ? album_complete_from(&doc, json_get(&doc, json_root(&doc), "result"), id, tracks, 300, &album) : -1;
	bool ok = count > 0;
	if (ok) {
		int available = 0;
		for (int i = 0; i < count; i++)
			available += tracks[i].available;
		fprintf(report, "metadata type=%s title=%s count=%d available=%d\n", kind_name(album.content_type), album.title, count, available);
		fprintf(report, "first type=%s title=%s available=%s\n", kind_name(tracks[0].content_type), tracks[0].title, tracks[0].available ? "yes" : "no");
		fprintf(report, "last type=%s title=%s available=%s\n", kind_name(tracks[count - 1].content_type), tracks[count - 1].title, tracks[count - 1].available ? "yes" : "no");
	} else {
		fputs("metadata complete=no\n", report);
	}
	free(tracks);
	json_free(&doc);
	free(body);
	fflush(report);
	return ok;
}

static bool inspect_search(yandex_content_t kind) {
	const char *text = kind == YANDEX_CONTENT_AUDIOBOOK ? "Симона де Бовуар" : "У Холмов Есть Подкаст";
	char query[600], url[1024], headers[YANDEX_TOKEN_MAX + 64];
	http_url_encode(text, query, sizeof(query));
	snprintf(url, sizeof(url), "%s/search?text=%s&type=all&page=0", api_url, query);
	snprintf(headers, sizeof(headers), "Authorization: OAuth %s\r\n", access_token);
	http_req_t request = {.method = "GET", .extra_headers = headers};
	char *body = NULL;
	int status = 0;
	bool fetched = http_request(url, &request, &body, NULL, YANDEX_BODY_LIMIT, 10, &status);
	memset(headers, 0, sizeof(headers));
	if (!fetched || status < 200 || status >= 300) {
		fprintf(report, "search http=%d success=no\n", status);
		free(body);
		return false;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		fprintf(report, "search http=%d parse=no\n", status);
		return false;
	}
	yandex_album_t *shows = calloc(30, sizeof(*shows));
	yandex_track_t *episodes = calloc(30, sizeof(*episodes));
	int ns = 30, ne = 30;
	bool ok = shows && episodes;
	if (ok) {
		spoken_search_from(&doc, kind, shows, &ns, episodes, &ne);
		fprintf(report, "search http=%d kind=%s shows=%d episodes=%d\n", status, kind_name(kind), ns, ne);
		if (ns > 0)
			fprintf(report, "first_show id=%s type=%s title=%s\n", shows[0].id, kind_name(shows[0].content_type), shows[0].title);
		if (ne > 0)
			fprintf(report, "first_episode id=%s type=%s title=%s\n", episodes[0].id, kind_name(episodes[0].content_type), episodes[0].title);
		ok = ns > 0 || ne > 0;
	} else {
		fputs("search allocation_failed\n", report);
	}
	free(shows);
	free(episodes);
	json_free(&doc);
	free(body);
	fflush(report);
	return ok;
}

int main(int argc, char **argv) {
	int report_fd = dup(STDOUT_FILENO);
	int null_fd = open("/dev/null", O_WRONLY);
	if (report_fd < 0 || null_fd < 0)
		return 1;
	report = fdopen(report_fd, "w");
	if (!report || dup2(null_fd, STDOUT_FILENO) < 0 || dup2(null_fd, STDERR_FILENO) < 0)
		return 1;
	close(null_fd);
	const char *config = argc >= 2 ? argv[1] : "/data/device_config.ini";
	const char *mode = argc == 3 ? argv[2] : "albums";
	if (argc == 2 && (strcmp(argv[1], "searchbooks") == 0 || strcmp(argv[1], "searchpodcasts") == 0)) {
		mode = argv[1];
		config = "/data/device_config.ini";
	}
	if (argc > 3 || (strcmp(mode, "albums") != 0 && strcmp(mode, "searchbooks") != 0 && strcmp(mode, "searchpodcasts") != 0)) {
		fputs("metadata unsupported_mode\n", report);
		fclose(report);
		return 1;
	}
	if (!config_init(config)) {
		fputs("metadata config_unavailable\n", report);
		fclose(report);
		return 1;
	}
	const char *base = config_get_copy("yandex", "base_url", YANDEX_API_DEFAULT, (char[CONFIG_VALUE_SIZE]){0}, CONFIG_VALUE_SIZE);
	// Fixed metadata requests only, never another host or an audio endpoint.
	if (strcmp(base, YANDEX_API_DEFAULT) != 0) {
		fputs("metadata nonstandard_endpoint\n", report);
		fclose(report);
		return 1;
	}
	snprintf(api_url, sizeof(api_url), "%s", base);
	load_session();
	if (!access_token[0]) {
		fputs("metadata no_saved_session\n", report);
		fclose(report);
		return 2;
	}
	http_set_thread_timeout_cap(10);
	bool ok;
	if (strcmp(mode, "searchbooks") == 0) {
		ok = inspect_search(YANDEX_CONTENT_AUDIOBOOK);
	} else if (strcmp(mode, "searchpodcasts") == 0) {
		ok = inspect_search(YANDEX_CONTENT_PODCAST);
	} else {
		ok = inspect_album("38681851") && inspect_album("11002256");
	}
	memset(access_token, 0, sizeof(access_token));
	memset(refresh_token, 0, sizeof(refresh_token));
	fclose(report);
	return ok ? 0 : 3;
}
