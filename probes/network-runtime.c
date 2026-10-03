/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded, credential-free HTTPS diagnostics with verification always enabled.
 */
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/x509.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../build/network-probe-ca.h"
#ifdef PS5_PROBE_RESOLVE
#include "../build/network-probe-dns.h"
#endif

int sceNetPoolCreate(const char *, int, int);
int sceNetPoolDestroy(int);
int sceNetResolverCreate(const char *, int, int);
int sceNetResolverDestroy(int);
int sceNetResolverStartNtoa(int, const char *, in_addr_t *, int, int, int);
int *sceNetErrnoLoc(void);

static void dns_probe(void) {
    for (int variant = 0; variant < 3; variant++) {
        struct addrinfo hints = {0}, *result = NULL;
        hints.ai_family = variant == 2 ? AF_INET : AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = variant == 1 ? AI_ADDRCONFIG : 0;
        int error = getaddrinfo("auth.openai.com", "443", &hints, &result);
        printf("getaddrinfo variant=%d result=%d network_errno=0x%x\n",
               variant, error, *sceNetErrnoLoc());
        if (!error) freeaddrinfo(result);
    }
    int pool = sceNetPoolCreate("ps5-ai-cli-dns", 0x4000, 0);
    printf("Native DNS pool=%d network_errno=0x%x\n", pool, *sceNetErrnoLoc());
    if (pool < 0) return;
    int resolver = sceNetResolverCreate("ps5-ai-cli-dns", pool, 0);
    printf("Native DNS resolver=%d network_errno=0x%x\n", resolver, *sceNetErrnoLoc());
    if (resolver >= 0) {
        in_addr_t address = 0;
        int result = sceNetResolverStartNtoa(resolver, "auth.openai.com", &address, 0, 0, 0);
        printf("Native DNS request=%d network_errno=0x%x nonzero_address=%d\n",
               result, *sceNetErrnoLoc(), address != 0);
        sceNetResolverDestroy(resolver);
    }
    sceNetPoolDestroy(pool);
}

static size_t discard(char *data, size_t size, size_t count, void *unused) {
    (void)data;
    (void)unused;
    return size * count;
}

static int request(const char *label, const char *url, int embedded_ca,
                   CURLcode expected) {
    CURL *curl = curl_easy_init();
    if (!curl) return 1;
    char error[CURL_ERROR_SIZE] = {0};
    struct curl_blob roots = {
        .data = (void *)(uintptr_t)network_probe_ca,
        .len = sizeof(network_probe_ca),
        .flags = CURL_BLOB_NOCOPY,
    };
    CURLcode result;
    struct curl_slist *resolved = NULL;
#define SET(option, value) do { \
    result = curl_easy_setopt(curl, option, value); \
    if (result != CURLE_OK) goto done; \
} while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_PROTOCOLS_STR, "https");
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    SET(CURLOPT_TIMEOUT_MS, 12000L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_NOBODY, 1L);
    SET(CURLOPT_WRITEFUNCTION, discard);
    SET(CURLOPT_ERRORBUFFER, error);
#ifdef PS5_PROBE_RESOLVE
    for (size_t i = 0; i < sizeof(network_probe_dns) / sizeof(network_probe_dns[0]); i++) {
        struct curl_slist *next = curl_slist_append(resolved, network_probe_dns[i]);
        if (!next) { result = CURLE_OUT_OF_MEMORY; goto done; }
        resolved = next;
    }
    SET(CURLOPT_RESOLVE, resolved);
#endif
    if (embedded_ca) SET(CURLOPT_CAINFO_BLOB, &roots);
    result = curl_easy_perform(curl);
done:
    ;
    long status = 0, verify = -1, os_error = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &verify);
    curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &os_error);
    printf("%s: curl=%d http=%ld verify=%ld os_errno=%ld error=%s\n",
           label, (int)result, status, verify, os_error, error);
    curl_easy_cleanup(curl);
    curl_slist_free_all(resolved);
    return result != expected;
#undef SET
}

int main(void) {
    if (mkdir("/data/ps5-ai-cli", 0700) && errno != EEXIST) return 1;
    char path[128];
    snprintf(path, sizeof(path), "/data/ps5-ai-cli/os-runtime-%ld.log", (long)getpid());
    int log = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (log < 0) return 1;
    if (dup2(log, 1) < 0 || dup2(log, 2) < 0) { close(log); return 1; }
    if (log > 2) close(log);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    alarm(50);
    puts("PS5 native HTTPS diagnostic (no account credentials)");
#ifdef PS5_PROBE_RESOLVE
    puts("DNS isolation: temporary host-resolved test addresses; original TLS hostnames retained");
#endif
    printf("Unix time: %lld\n", (long long)time(NULL));
    printf("TLS library: %s\n", OpenSSL_version(OPENSSL_VERSION));
    const char *file = X509_get_default_cert_file();
    const char *dir = X509_get_default_cert_dir();
    printf("Default CA file readable: %d; directory accessible: %d\n",
           access(file, R_OK) == 0, access(dir, R_OK | X_OK) == 0);
    dns_probe();
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    /* The default-root result is informational; embedded roots must work. */
    request("default roots", "https://auth.openai.com/", 0, CURLE_OK);
    int failed = request("embedded roots", "https://auth.openai.com/", 1, CURLE_OK);
    failed += request("untrusted certificate", "https://self-signed.badssl.com/", 1,
                      CURLE_PEER_FAILED_VERIFICATION);
    failed += request("wrong hostname", "https://wrong.host.badssl.com/", 1,
                      CURLE_PEER_FAILED_VERIFICATION);
    curl_global_cleanup();
    printf("HTTPS probe complete: %d failed cases\n", failed);
    alarm(0);
    return failed != 0;
}
