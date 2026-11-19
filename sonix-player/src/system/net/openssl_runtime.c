#define _GNU_SOURCE
#include "openssl_runtime.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *shared_ssl, *shared_crypto;
static bool attempted;

static bool load_pair(const char *directory, const char *suffix) {
	char ssl_path[1024], crypto_path[1024];
	int a = snprintf(ssl_path, sizeof(ssl_path), "%s%slibssl.so%s", directory, directory[0] ? "/" : "", suffix);
	int b = snprintf(crypto_path, sizeof(crypto_path), "%s%slibcrypto.so%s", directory, directory[0] ? "/" : "", suffix);
	if (a < 0 || a >= (int)sizeof(ssl_path) || b < 0 || b >= (int)sizeof(crypto_path))
		return false;
	void *crypto = dlopen(crypto_path, RTLD_NOW | RTLD_LOCAL);
	if (!crypto)
		return false;
	void *ssl = dlopen(ssl_path, RTLD_NOW | RTLD_LOCAL);
	// Looking through libssl's dependency scope must reach this exact crypto.
	void *version = dlsym(crypto, "OpenSSL_version");
	bool matched = ssl && version && dlsym(ssl, "OpenSSL_version") == version && dlsym(ssl, "TLS_client_method") && dlsym(crypto, "EVP_CIPHER_CTX_new");
	if (!matched) {
		if (ssl)
			dlclose(ssl);
		dlclose(crypto);
		return false;
	}
	shared_ssl = ssl;
	shared_crypto = crypto;
	const char *(*describe)(int);
	memcpy(&describe, &version, sizeof(describe));
	fprintf(stderr, "openssl: matched pair %s (%s)\n", describe(0), ssl_path);
	return true;
}

bool openssl_runtime_get(void **ssl, void **crypto) {
	pthread_mutex_lock(&lock);
	if (!attempted) {
		attempted = true;
		const char *override = getenv("SONIX_OPENSSL_DIR");
		if (override && override[0]) {
			// A requested test pair must fail visibly, never silently use stock.
			load_pair(override, ".3");
		} else if (!load_pair("/usr/lib/sonix/openssl3", ".3") && !load_pair("", ".3")) {
			if (!load_pair("", ".1.1"))
				load_pair("", "");
		}
	}
	if (ssl)
		*ssl = shared_ssl;
	if (crypto)
		*crypto = shared_crypto;
	bool ok = shared_ssl && shared_crypto;
	pthread_mutex_unlock(&lock);
	return ok;
}
