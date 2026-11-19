#ifndef SONIX_OPENSSL_RUNTIME_H
#define SONIX_OPENSSL_RUNTIME_H
#include <stdbool.h>
// Process-owned matched handles. Both consumers use the same selection;
// callers never close them. SONIX_OPENSSL_DIR is an isolated-test override.
bool openssl_runtime_get(void **ssl, void **crypto);
#endif
