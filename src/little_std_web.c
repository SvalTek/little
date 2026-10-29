#define _GNU_SOURCE
#include "little_std.h"
#include "little_async.h"
#include "little_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#endif

typedef struct {
    int status;
    char* status_text;
    lt_Value headers;
    char* body;
    char* url;
} WebResponseData;

typedef struct {
    char* url;
    char* method;
    char* body;
    lt_Value headers;
} WebRequestData;

/* At most this many requests can be active on one listener. */
#define WEB_SERVER_CLIENT_LIMIT 8

typedef struct {
#ifdef _WIN32
    SOCKET socket;
#else
    int socket;
#endif
    char input[65536];
    size_t input_len;
    char* output;
    size_t output_len;
    size_t output_sent;
    lt_Value pending;
    uint8_t head_request;
} WebClientData;

typedef struct {
#ifdef _WIN32
    SOCKET socket;
#else
    int socket;
#endif
    uint16_t port;
    uint8_t closed;
    uint32_t hook;
    WebClientData* clients[WEB_SERVER_CLIENT_LIMIT];
    uint8_t dispatching;
} WebServerData;

typedef struct {
    WebServerData* server;
    lt_Value instance;
    lt_Value handler;
} WebServerHook;

#define web_api (ltstd_native_api())

/* Classes belong to their VM. Keep their registry reachable independently of
   the public web table, which scripts may replace. */
static lt_Value web_class(lt_VM* vm, const char* name)
{
    lt_Value registry = web_api->table_get(vm, vm->global, web_api->make_string(vm, "__little_web_classes"));
    lt_Value klass = web_api->table_get(vm, registry, web_api->make_string(vm, name));
    if (!LT_IS_CLASS(klass)) web_api->runtime_error(vm, "Web class registry is unavailable!");
    return klass;
}

static void web_destroy_response(void* data)
{
    WebResponseData* rd = (WebResponseData*)data;
    if (!rd) return;
    free(rd->status_text);
    free(rd->body);
    free(rd->url);
    free(rd);
}

static void web_destroy_request(void* data)
{
    WebRequestData* rq = (WebRequestData*)data;
    if (!rq) return;
    free(rq->url);
    free(rq->method);
    free(rq->body);
    free(rq);
}

static void web_destroy_server(void* data)
{
    WebServerData* sd = (WebServerData*)data;
    if (!sd) return;
    if (!sd->closed)
    {
#ifdef _WIN32
        closesocket(sd->socket);
#else
        close(sd->socket);
#endif
        sd->closed = 1;
    }
    free(sd);
}

static char* web_strdup(const char* s);

static lt_Value web_instance_class(lt_VM* vm, lt_Value instance, lt_NativeDataDestroyFn destroy)
{
    if (LT_IS_INSTANCE(instance)) {
        for (lt_Object* klass = LT_GET_OBJECT(instance)->instance.klass; klass; klass = klass->class_def.superclass)
            if (klass->class_def.native_data_destroy == destroy) return LT_VALUE_OBJECT(klass);
    }
    web_api->runtime_error(vm, "Invalid web instance!");
    return LT_VALUE_NULL;
}

static void web_keep_headers(lt_VM* vm, lt_Value instance, lt_Value headers)
{
    /* The instance's private-field table is traced by GC. A null key cannot
       collide with its class-keyed private-field entries. */
    lt_Buffer* bucket = &LT_GET_OBJECT(instance)->instance.private_fields.buckets[0];
    if (!bucket->element_size) *bucket = lt_buffer_new(sizeof(lt_TablePair));
    for (uint32_t i = 0; i < bucket->length; ++i) {
        lt_TablePair* pair = lt_buffer_at(bucket, i);
        if (LT_IS_NULL(pair->key)) { pair->value = headers; return; }
    }
    lt_TablePair reference = { LT_VALUE_NULL, headers };
    lt_buffer_push(vm, bucket, &reference);
}

static const char* web_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default: return "";
    }
}

static uint8_t response_constructor(lt_VM* vm, uint8_t argc)
{
    if (argc < 1 || argc > 3) { web_api->runtime_error(vm, "Response expects optional body and options!"); return 0; }
    lt_Value options = argc == 3 ? web_api->pop(vm) : LT_VALUE_NULL;
    lt_Value body = argc >= 2 ? web_api->pop(vm) : LT_VALUE_NULL;
    lt_Value instance = web_api->pop(vm);
    web_api->root(vm, instance);
    web_api->root(vm, options);
    web_api->root(vm, body);
    if (!LT_IS_NULL(body) && !LT_IS_STRING(body)) { web_api->runtime_error(vm, "Response body must be a string!"); return 0; }
    if (!LT_IS_NULL(options) && !LT_IS_TABLE(options)) { web_api->runtime_error(vm, "Response options must be a table!"); return 0; }
    int status = 200;
    lt_Value headers = web_api->make_table(vm);
    const char* status_text = 0;
    if (LT_IS_TABLE(options)) {
        lt_Value value = web_api->table_get(vm, options, web_api->make_string(vm, "status"));
        if (!LT_IS_NULL(value)) {
            if (!LT_IS_NUMBER(value)) { web_api->runtime_error(vm, "Response status must be an integer from 100 to 599!"); return 0; }
            double n = web_api->get_number(value);
            if (!(n >= 100 && n <= 599) || n != (double)(int)n) { web_api->runtime_error(vm, "Response status must be an integer from 100 to 599!"); return 0; }
            status = (int)n;
        }
        value = web_api->table_get(vm, options, web_api->make_string(vm, "statusText"));
        if (!LT_IS_NULL(value)) {
            if (!LT_IS_STRING(value)) { web_api->runtime_error(vm, "Response statusText must be a string!"); return 0; }
            status_text = web_api->get_string(vm, value);
            if (strpbrk(status_text, "\r\n")) { web_api->runtime_error(vm, "Invalid response statusText!"); return 0; }
        }
        value = web_api->table_get(vm, options, web_api->make_string(vm, "headers"));
        if (!LT_IS_NULL(value)) {
            if (!LT_IS_TABLE(value)) { web_api->runtime_error(vm, "Response headers must be a table!"); return 0; }
            headers = value;
        }
    }
    WebResponseData* rd = calloc(1, sizeof(*rd));
    if (!rd) { web_api->runtime_error(vm, "Out of memory creating web response!"); return 0; }
    rd->status = status;
    rd->status_text = web_strdup(status_text ? status_text : web_reason(status));
    rd->body = web_strdup(LT_IS_STRING(body) ? web_api->get_string(vm, body) : "");
    rd->url = web_strdup("");
    rd->headers = headers;
    if (!rd->status_text || !rd->body || !rd->url) {
        web_destroy_response(rd); web_api->runtime_error(vm, "Out of memory creating web response!"); return 0;
    }
    web_keep_headers(vm, instance, headers);
    web_api->instance_set_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response), rd);
    return 0;
}

static uint8_t response_status(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = web_api->pop(vm);
    WebResponseData* rd = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    web_api->push(vm, web_api->make_number(rd ? (double)rd->status : 0.0));
    return 1;
}

static uint8_t response_status_text(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = web_api->pop(vm);
    WebResponseData* rd = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    web_api->push(vm, web_api->make_string(vm, rd && rd->status_text ? rd->status_text : ""));
    return 1;
}

static uint8_t response_ok(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = web_api->pop(vm);
    WebResponseData* rd = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    int ok = rd && rd->status >= 200 && rd->status < 300;
    web_api->push(vm, ok ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t response_url(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = web_api->pop(vm);
    WebResponseData* rd = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    web_api->push(vm, web_api->make_string(vm, rd && rd->url ? rd->url : ""));
    return 1;
}

static uint8_t response_headers(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = web_api->pop(vm);
    WebResponseData* rd = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    web_api->push(vm, rd && LT_IS_OBJECT(rd->headers) ? rd->headers : web_api->make_table(vm));
    return 1;
}

static uint8_t response_body(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = web_api->pop(vm);
    WebResponseData* rd = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    web_api->push(vm, web_api->make_string(vm, rd && rd->body ? rd->body : ""));
    return 1;
}

static uint8_t response_text(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) web_api->runtime_error(vm, "Response.text expects no arguments!");
    lt_Value instance = web_api->pop(vm);
    WebResponseData* rd = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    lt_Value promise = ltasync_make_promise(vm);
    ltasync_settle_promise(vm, promise, LT_PROMISE_FULFILLED, web_api->make_string(vm, rd && rd->body ? rd->body : ""));
    web_api->push(vm, promise);
    return 1;
}

static uint8_t response_json(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) web_api->runtime_error(vm, "Response.json expects no arguments!");
    lt_Value instance = web_api->pop(vm);
    WebResponseData* rd = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    lt_json_init(ltstd_native_api());
    lt_Value value = LT_VALUE_NULL;
    uint8_t ok = lt_json_parse(vm, rd && rd->body ? rd->body : "", &value);
    if (ok) web_api->root(vm, value);
    lt_Value promise = ltasync_make_promise(vm);
    web_api->root(vm, promise);
    if (ok) ltasync_settle_promise(vm, promise, LT_PROMISE_FULFILLED, value);
    else ltasync_settle_promise(vm, promise, LT_PROMISE_REJECTED, web_api->make_string(vm, "Invalid JSON response body!"));
    web_api->push(vm, promise);
    return 1;
}

static uint8_t request_url(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = web_api->pop(vm);
    WebRequestData* rq = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_request));
    web_api->push(vm, web_api->make_string(vm, rq && rq->url ? rq->url : ""));
    return 1;
}

static uint8_t request_method(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = web_api->pop(vm);
    WebRequestData* rq = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_request));
    web_api->push(vm, web_api->make_string(vm, rq && rq->method ? rq->method : "GET"));
    return 1;
}

static uint8_t request_body(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = web_api->pop(vm);
    WebRequestData* rq = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_request));
    web_api->push(vm, web_api->make_string(vm, rq && rq->body ? rq->body : ""));
    return 1;
}

static uint8_t request_headers(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = web_api->pop(vm);
    WebRequestData* rq = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_request));
    web_api->push(vm, rq && LT_IS_OBJECT(rq->headers) ? rq->headers : web_api->make_table(vm));
    return 1;
}

/* ---------- helpers ---------- */

static char* web_strdup(const char* s)
{
    if (!s) return 0;
    size_t n = strlen(s);
    char* c = malloc(n + 1);
    if (c) memcpy(c, s, n + 1);
    return c;
}

static int parse_url(const char* url, char* host, size_t hostsz, int* port, int* secure, char* path, size_t pathsz)
{
    const char* p;
    if (strncmp(url, "http://", 7) == 0) { *secure = 0; p = url + 7; }
    else if (strncmp(url, "https://", 8) == 0) { *secure = 1; p = url + 8; }
    else return 0;
    const char* authority_end = p + strcspn(p, "/?#");
    const char* host_start = p;
    const char* host_end = authority_end;
    const char* port_start = 0;
    if (p < authority_end && *p == '[') {
        host_start = p + 1;
        host_end = memchr(host_start, ']', (size_t)(authority_end - host_start));
        if (!host_end) return 0;
        if (host_end + 1 < authority_end) {
            if (host_end[1] != ':') return 0;
            port_start = host_end + 2;
        }
    } else {
        const char* colon = memchr(p, ':', (size_t)(authority_end - p));
        if (colon) { host_end = colon; port_start = colon + 1; }
    }
    size_t host_len = (size_t)(host_end - host_start);
    if (!host_len || host_len >= hostsz || memchr(host_start, '@', host_len)) return 0;
    memcpy(host, host_start, host_len); host[host_len] = 0;
    *port = *secure ? 443 : 80;
    if (port_start) {
        int n = 0;
        if (port_start == authority_end) return 0;
        for (const char* q = port_start; q < authority_end; ++q) {
            if (!isdigit((unsigned char)*q) || n > 6553) return 0;
            n = n * 10 + (*q - '0');
        }
        if (n < 1 || n > 65535) return 0;
        *port = n;
    }
    const char* path_end = authority_end + strcspn(authority_end, "#");
    size_t path_len = (size_t)(path_end - authority_end);
    int needs_slash = !path_len || *authority_end == '?';
    if (path_len + (size_t)needs_slash >= pathsz) return 0;
    if (needs_slash) path[0] = '/';
    memcpy(path + needs_slash, authority_end, path_len);
    path[path_len + needs_slash] = 0;
    return 1;
}

/* Append only complete fields; snprintf's return value can exceed the buffer. */
static int web_append(char* buffer, size_t capacity, size_t* length, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + *length, capacity - *length, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= capacity - *length) return 0;
    *length += (size_t)written;
    return 1;
}

static int web_header_is(const char* name, const char* expected)
{
    while (*name && *expected && tolower((unsigned char)*name) == tolower((unsigned char)*expected)) {
        ++name;
        ++expected;
    }
    return !*name && !*expected;
}

static int web_header_valid(const char* name, const char* value)
{
    if (!*name) return 0;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p)
        if (*p <= 32 || *p >= 127 || strchr("()<>@,;:\\\"/[]?={}", *p)) return 0;
    for (const unsigned char* p = (const unsigned char*)value; *p; ++p)
        if ((*p < 32 && *p != '\t') || *p == 127) return 0;
    return 1;
}

#ifdef _WIN32
typedef SOCKET WebSocket;
#define WEB_BAD_SOCKET INVALID_SOCKET
#else
typedef int WebSocket;
#define WEB_BAD_SOCKET (-1)
#endif

static void web_close_socket(WebSocket socket_fd)
{
    if (socket_fd == WEB_BAD_SOCKET) return;
#ifdef _WIN32
    closesocket(socket_fd);
#else
    close(socket_fd);
#endif
}

static int web_nonblocking(WebSocket socket_fd)
{
#ifdef _WIN32
    u_long value = 1;
    return ioctlsocket(socket_fd, FIONBIO, &value) == 0;
#else
    int flags = fcntl(socket_fd, F_GETFL, 0);
    return flags >= 0 && fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static void web_no_sigpipe(WebSocket socket_fd)
{
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&enabled, sizeof(enabled));
#else
    (void)socket_fd;
#endif
}

static int web_would_block(void)
{
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static int web_send_all(WebSocket socket_fd, const char* data, size_t length)
{
    while (length) {
        int chunk = length > INT_MAX ? INT_MAX : (int)length;
#ifdef MSG_NOSIGNAL
        int sent = send(socket_fd, data, chunk, MSG_NOSIGNAL);
#else
        int sent = send(socket_fd, data, chunk, 0);
#endif
        if (sent <= 0) return 0;
        data += sent;
        length -= (size_t)sent;
    }
    return 1;
}

#ifdef _WIN32
/* Let Windows build and validate the chain so its root, distrust and usage
   policies apply. OpenSSL still checks the requested DNS name or IP address. */
static PCCERT_CONTEXT web_windows_certificate(X509* certificate)
{
    int length = i2d_X509(certificate, 0);
    if (length <= 0) return 0;
    unsigned char* encoded = malloc((size_t)length);
    if (!encoded) return 0;
    unsigned char* cursor = encoded;
    PCCERT_CONTEXT result = 0;
    if (i2d_X509(certificate, &cursor) == length)
        result = CertCreateCertificateContext(X509_ASN_ENCODING, encoded, (DWORD)length);
    free(encoded);
    return result;
}

static int web_windows_verify(X509_STORE_CTX* verify, void* context)
{
    const char* host = context;
    X509* leaf = X509_STORE_CTX_get0_cert(verify);
    PCCERT_CONTEXT certificate = 0;
    PCCERT_CHAIN_CONTEXT chain = 0;
    HCERTSTORE extra = 0;
    int valid = 0;
    unsigned char address[16];
    int is_ip = inet_pton(AF_INET, host, address) == 1 || inet_pton(AF_INET6, host, address) == 1;
    if (!leaf || (is_ip ? X509_check_ip_asc(leaf, host, 0) : X509_check_host(leaf, host, 0, 0, 0)) != 1)
        goto done;
    certificate = web_windows_certificate(leaf);
    extra = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, 0);
    if (!certificate || !extra) goto done;
    STACK_OF(X509)* peers = X509_STORE_CTX_get0_untrusted(verify);
    for (int i = 0; peers && i < sk_X509_num(peers); ++i) {
        PCCERT_CONTEXT peer = web_windows_certificate(sk_X509_value(peers, i));
        if (!peer) goto done;
        BOOL added = CertAddCertificateContextToStore(extra, peer, CERT_STORE_ADD_USE_EXISTING, 0);
        CertFreeCertificateContext(peer);
        if (!added) goto done;
    }
    LPSTR usage = szOID_PKIX_KP_SERVER_AUTH;
    CERT_CHAIN_PARA parameters;
    memset(&parameters, 0, sizeof(parameters));
    parameters.cbSize = sizeof(parameters);
    parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    parameters.RequestedUsage.Usage.cUsageIdentifier = 1;
    parameters.RequestedUsage.Usage.rgpszUsageIdentifier = &usage;
    if (!CertGetCertificateChain(0, certificate, 0, extra, &parameters, 0, 0, &chain)) goto done;
    CERT_CHAIN_POLICY_PARA policy;
    CERT_CHAIN_POLICY_STATUS status;
    memset(&policy, 0, sizeof(policy));
    memset(&status, 0, sizeof(status));
    policy.cbSize = sizeof(policy);
    status.cbSize = sizeof(status);
    valid = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_BASE, chain, &policy, &status) && status.dwError == 0;
done:
    if (chain) CertFreeCertificateChain(chain);
    if (extra) CertCloseStore(extra, 0);
    if (certificate) CertFreeCertificateContext(certificate);
    if (!valid) X509_STORE_CTX_set_error(verify, X509_V_ERR_CERT_REJECTED);
    return valid;
}
#endif

static lt_Value create_response_instance(lt_VM* vm, int status, const char* status_text, lt_Value headers, const char* body, const char* url)
{
    web_api->root(vm, headers);
    uint16_t nret = web_api->exec(vm, web_class(vm, "Response"), 0);
    lt_Value instance = LT_VALUE_NULL;
    if (nret > 0) { instance = web_api->pop(vm); for (uint16_t i = 1; i < nret; ++i) web_api->pop(vm); }
    if (!LT_IS_INSTANCE(instance)) return LT_VALUE_NULL;
    web_api->root(vm, instance);
    WebResponseData* rd = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    if (!rd) { web_api->runtime_error(vm, "Response constructor did not initialize its data!"); return LT_VALUE_NULL; }
    char* new_status_text = web_strdup(status_text ? status_text : "");
    char* new_body = web_strdup(body ? body : "");
    char* new_url = web_strdup(url ? url : "");
    if (!new_status_text || !new_body || !new_url) {
        free(new_status_text); free(new_body); free(new_url);
        web_api->runtime_error(vm, "Out of memory creating web response!");
        return LT_VALUE_NULL;
    }
    free(rd->status_text); free(rd->body); free(rd->url);
    rd->status = status;
    rd->status_text = new_status_text;
    rd->headers = LT_IS_OBJECT(headers) ? headers : web_api->make_table(vm);
    rd->body = new_body;
    rd->url = new_url;
    web_keep_headers(vm, instance, rd->headers);
    return instance;
}

static lt_Value create_request_instance(lt_VM* vm, const char* url, const char* method, const char* body, lt_Value headers)
{
    web_api->root(vm, headers);
    uint16_t nret = web_api->exec(vm, web_class(vm, "Request"), 0);
    lt_Value instance = LT_VALUE_NULL;
    if (nret > 0) { instance = web_api->pop(vm); for (uint16_t i = 1; i < nret; ++i) web_api->pop(vm); }
    if (!LT_IS_INSTANCE(instance)) return LT_VALUE_NULL;
    web_api->root(vm, instance);
    WebRequestData* rq = calloc(1, sizeof(WebRequestData));
    if (!rq) { web_api->runtime_error(vm, "Out of memory creating web request!"); return LT_VALUE_NULL; }
    rq->url = web_strdup(url ? url : "");
    rq->method = web_strdup(method ? method : "GET");
    rq->body = web_strdup(body ? body : "");
    rq->headers = LT_IS_OBJECT(headers) ? headers : web_api->make_table(vm);
    if (!rq->url || !rq->method || !rq->body) {
        web_destroy_request(rq);
        web_api->runtime_error(vm, "Out of memory creating web request!");
        return LT_VALUE_NULL;
    }
    web_keep_headers(vm, instance, rq->headers);
    web_api->instance_set_native_data(vm, instance, web_class(vm, "Request"), rq);
    return instance;
}

/* Network workers never call into the VM.  The hook and the detached worker
   each own one reference, so teardown can release the VM even during DNS. */
typedef struct {
    atomic_int refs;
    atomic_int done;
    lt_Value promise;
    uint32_t hook;
    char host[256];
    int port;
    int secure;
    char* url;
    char* request;
    size_t request_len;
    char* response;
    size_t response_len;
    const char* error;
    int head;
} WebFetch;

static void web_fetch_release(WebFetch* f)
{
    if (atomic_fetch_sub(&f->refs, 1) != 1) return;
    free(f->url);
    free(f->request);
    free(f->response);
    free(f);
}

static void web_fetch_destroy(lt_VM* vm, void* context)
{
    WebFetch* f = context;
    web_api->unroot_persistent(vm, f->promise);
    web_fetch_release(f);
}

/* Return 1 for complete, 0 for incomplete, -1 for malformed. */
static int web_chunks(char* body, size_t length, size_t* consumed, int decode)
{
    size_t pos = 0, out = 0;
    for (;;) {
        size_t start = pos, n = 0;
        while (pos < length && isxdigit((unsigned char)body[pos])) {
            unsigned char ch = (unsigned char)body[pos++];
            unsigned digit = isdigit(ch) ? (unsigned)(ch - '0') : (unsigned)(tolower(ch) - 'a' + 10);
            if (n > (SIZE_MAX - digit) / 16) return -1;
            n = n * 16 + digit;
        }
        if (pos == start) return pos == length ? 0 : -1;
        if (pos == length) return 0;
        if (body[pos] == ';') {
            while (pos < length && body[pos] != '\r') ++pos;
        }
        if (pos + 1 >= length) return 0;
        if (body[pos] != '\r' || body[pos + 1] != '\n') return -1;
        pos += 2;
        if (!n) {
            for (;;) {
                size_t line = pos;
                while (pos + 1 < length && !(body[pos] == '\r' && body[pos + 1] == '\n')) ++pos;
                if (pos + 1 >= length) return 0;
                pos += 2;
                if (pos == line + 2) { *consumed = decode ? out : pos; return 1; }
            }
        }
        if (n > length - pos || length - pos - n < 2) return 0;
        if (body[pos + n] != '\r' || body[pos + n + 1] != '\n') return -1;
        if (decode) memmove(body + out, body + pos, n);
        out += n;
        pos += n + 2;
    }
}

static int web_response_framing(char* buf, size_t len, int head, size_t* header_len, size_t* body_len, int* chunked)
{
    char* end = strstr(buf, "\r\n\r\n");
    if (!end) return 0;
    char* line = strstr(buf, "\r\n");
    int status = 0;
    if (!line || sscanf(buf, "HTTP/%*u.%*u %d", &status) != 1 || status < 100 || status > 999) return -1;
    size_t content = 0;
    int has_length = 0;
    *chunked = 0;
    for (char* p = line + 2; p < end;) {
        char* next = strstr(p, "\r\n");
        if (!next || next > end) return -1;
        char* colon = memchr(p, ':', (size_t)(next - p));
        if (!colon) return -1;
        size_t name_len = (size_t)(colon - p);
        char name[64];
        if (name_len >= sizeof(name)) { p = next + 2; continue; }
        memcpy(name, p, name_len); name[name_len] = 0;
        char* value = colon + 1;
        while (value < next && (*value == ' ' || *value == '\t')) ++value;
        if (web_header_is(name, "Content-Length")) {
            size_t n = 0; char* q = value;
            if (q == next || !isdigit((unsigned char)*q)) return -1;
            while (q < next && isdigit((unsigned char)*q)) {
                unsigned d = (unsigned)(*q++ - '0');
                if (n > (SIZE_MAX - d) / 10) return -1;
                n = n * 10 + d;
            }
            while (q < next && (*q == ' ' || *q == '\t')) ++q;
            if (q != next || (has_length && content != n)) return -1;
            has_length = 1; content = n;
        } else if (web_header_is(name, "Transfer-Encoding")) {
            size_t n = (size_t)(next - value);
            while (n && (value[n-1] == ' ' || value[n-1] == '\t')) --n;
            if (n != 7) return -1;
            char encoding[8]; memcpy(encoding, value, n); encoding[n] = 0;
            if (!web_header_is(encoding, "chunked") || *chunked) return -1;
            *chunked = 1;
        }
        p = next + 2;
    }
    if (*chunked && has_length) return -1;
    *header_len = (size_t)(end + 4 - buf);
    if (head || (status >= 100 && status < 200) || status == 204 || status == 304) {
        /* These responses may carry Transfer-Encoding but no chunk stream. */
        *chunked = 0;
        *body_len = 0;
        return 1;
    }
    size_t available = len - *header_len;
    if (*chunked) return web_chunks(buf + *header_len, available, body_len, 0);
    if (has_length) { *body_len = content; return available >= content ? 1 : 0; }
    *body_len = available;
    return 2; /* close-delimited */
}

static void web_fetch_worker(WebFetch* f)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { f->error = "Socket initialization failed"; goto done; }
#else
    /* OpenSSL's socket BIO cannot use MSG_NOSIGNAL. Keep SIGPIPE local to
       this detached worker, where any pending signal disappears on exit. */
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE);
    if (f->secure && pthread_sigmask(SIG_BLOCK, &signals, 0) != 0) { f->error = "Socket initialization failed"; goto sockets_done; }
#endif
    SSL_CTX* tls_context = 0;
    SSL* tls = 0;
    struct addrinfo hints, *addresses = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port[16];
    snprintf(port, sizeof(port), "%d", f->port);
    if (getaddrinfo(f->host, port, &hints, &addresses) != 0) { f->error = "DNS lookup failed"; goto sockets_done; }
    WebSocket socket_fd = WEB_BAD_SOCKET;
    for (struct addrinfo* a = addresses; a; a = a->ai_next) {
        socket_fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (socket_fd == WEB_BAD_SOCKET) continue;
        web_no_sigpipe(socket_fd);
#ifdef _WIN32
        DWORD timeout = 10000;
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
        struct timeval timeout = { 10, 0 };
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
        if (connect(socket_fd, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        web_close_socket(socket_fd); socket_fd = WEB_BAD_SOCKET;
    }
    freeaddrinfo(addresses);
    if (socket_fd == WEB_BAD_SOCKET) { f->error = "Connection failed"; goto sockets_done; }
    if (f->secure) {
        tls_context = SSL_CTX_new(TLS_client_method());
        if (!tls_context) { f->error = "TLS trust setup failed"; goto close_socket; }
#ifdef _WIN32
        const char* trust_file = getenv("SSL_CERT_FILE");
        if (!trust_file || !*trust_file)
            SSL_CTX_set_cert_verify_callback(tls_context, web_windows_verify, f->host);
        else
#endif
        if (SSL_CTX_set_default_verify_paths(tls_context) != 1) { f->error = "TLS trust setup failed"; goto close_socket; }
        SSL_CTX_set_verify(tls_context, SSL_VERIFY_PEER, 0);
        tls = SSL_new(tls_context);
        if (!tls) { f->error = "TLS initialization failed"; goto close_socket; }
        unsigned char address[16];
        int is_ip = inet_pton(AF_INET, f->host, address) == 1 || inet_pton(AF_INET6, f->host, address) == 1;
        X509_VERIFY_PARAM* verify = SSL_get0_param(tls);
        if ((is_ip ? X509_VERIFY_PARAM_set1_ip_asc(verify, f->host) :
                     (SSL_set_tlsext_host_name(tls, f->host) && X509_VERIFY_PARAM_set1_host(verify, f->host, 0))) != 1 ||
            SSL_set_fd(tls, (int)socket_fd) != 1) {
            f->error = "TLS initialization failed"; goto close_socket;
        }
        ERR_clear_error();
        if (SSL_connect(tls) != 1) {
            f->error = SSL_get_verify_result(tls) != X509_V_OK ? "TLS certificate verification failed" : "TLS handshake failed";
            goto close_socket;
        }
        size_t sent = 0;
        while (sent < f->request_len) {
            size_t remaining = f->request_len - sent;
            ERR_clear_error();
            int count = SSL_write(tls, f->request + sent, remaining > INT_MAX ? INT_MAX : (int)remaining);
            if (count <= 0) { f->error = "TLS request send failed"; goto close_socket; }
            sent += (size_t)count;
        }
    } else if (!web_send_all(socket_fd, f->request, f->request_len)) { f->error = "Request send failed"; goto close_socket; }
    size_t cap = 8192, len = 0;
    char* data = malloc(cap);
    if (!data) { f->error = "Out of memory"; goto close_socket; }
    data[0] = 0;
    int complete = 0;
    for (;;) {
        if (len + 4096 + 1 > cap) {
            if (cap >= 4194304) { f->error = "HTTP response too large"; break; }
            size_t next = cap * 2;
            char* grown = realloc(data, next);
            if (!grown) { f->error = "Out of memory"; break; }
            data = grown; cap = next;
        }
        if (tls) ERR_clear_error();
        int count = tls ? SSL_read(tls, data + len, (int)(cap - len - 1)) : recv(socket_fd, data + len, (int)(cap - len - 1), 0);
        if (tls && count <= 0) {
            int error = SSL_get_error(tls, count);
            if (error == SSL_ERROR_ZERO_RETURN) count = 0;
            else { f->error = "TLS response read failed"; break; }
        }
        if (count < 0) { f->error = "HTTP response read failed"; break; }
        if (count == 0) {
            size_t h = 0, b = 0; int chunked = 0;
            int framing = web_response_framing(data, len, f->head, &h, &b, &chunked);
            if (framing == 1 || framing == 2) complete = 1;
            else f->error = "Invalid or incomplete HTTP response";
            break;
        }
        if (memchr(data + len, 0, (size_t)count)) { f->error = "Binary HTTP response is not supported"; break; }
        len += (size_t)count; data[len] = 0;
        size_t h = 0, b = 0; int chunked = 0;
        int framing = web_response_framing(data, len, f->head, &h, &b, &chunked);
        int status = 0;
        while (h && sscanf(data, "HTTP/%*u.%*u %d", &status) == 1 && status >= 100 && status < 200) {
            if (status == 101) { f->error = "HTTP protocol upgrades are not supported"; break; }
            memmove(data, data + h, len - h);
            len -= h; data[len] = 0;
            h = 0;
            framing = web_response_framing(data, len, f->head, &h, &b, &chunked);
        }
        if (f->error) break;
        if (framing < 0) { f->error = "Invalid HTTP response framing"; break; }
        if (framing == 1) { complete = 1; break; }
    }
    if (complete) { f->response = data; f->response_len = len; }
    else free(data);
close_socket:
    if (tls) SSL_free(tls);
    if (tls_context) SSL_CTX_free(tls_context);
    web_close_socket(socket_fd);
sockets_done:
#ifdef _WIN32
    WSACleanup();
done:
#endif
    atomic_store_explicit(&f->done, 1, memory_order_release);
    web_fetch_release(f);
}

#ifdef _WIN32
static DWORD WINAPI web_fetch_thread(LPVOID data) { web_fetch_worker(data); return 0; }
#else
static void* web_fetch_thread(void* data) { web_fetch_worker(data); return 0; }
#endif

static lt_PollResult web_fetch_poll(lt_VM* vm, void* context)
{
    WebFetch* f = context;
    if (!atomic_load_explicit(&f->done, memory_order_acquire)) return LT_POLL_PENDING;
    if (f->error) {
        ltasync_settle_promise(vm, f->promise, LT_PROMISE_REJECTED, web_api->make_string(vm, f->error));
    } else {
        size_t header_len = 0, body_len = 0; int chunked = 0;
        int framing = web_response_framing(f->response, f->response_len, f->head, &header_len, &body_len, &chunked);
        char* body = f->response + header_len;
        if (chunked && web_chunks(body, f->response_len - header_len, &body_len, 1) != 1) framing = -1;
        if (framing < 1) {
            ltasync_settle_promise(vm, f->promise, LT_PROMISE_REJECTED, web_api->make_string(vm, "Invalid HTTP response framing"));
        } else {
            body[body_len] = 0;
            int status = 0; char status_text[256] = "";
            sscanf(f->response, "HTTP/%*u.%*u %d %255[^\r]", &status, status_text);
            lt_Value headers = web_api->make_table(vm);
            web_api->root(vm, headers);
            char* p = strstr(f->response, "\r\n") + 2;
            char* end = f->response + header_len - 2;
            while (p < end) {
                char* next = strstr(p, "\r\n");
                if (!next || next > end) break;
                char* colon = memchr(p, ':', (size_t)(next - p));
                if (!colon) break;
                *colon = 0; *next = 0;
                char* value = colon + 1;
                while (*value == ' ' || *value == '\t') ++value;
                web_api->table_set(vm, headers, web_api->make_string(vm, p), web_api->make_string(vm, value));
                p = next + 2;
            }
            lt_Value response = create_response_instance(vm, status, status_text, headers, body, f->url);
            ltasync_settle_promise(vm, f->promise, LT_PROMISE_FULFILLED, response);
        }
    }
    web_api->remove_poll_hook(vm, f->hook);
    return LT_POLL_WORK;
}

/* ---------- fetch ---------- */

static uint8_t web_fetch(lt_VM* vm, uint8_t argc)
{
    if (argc < 1 || argc > 2) { web_api->runtime_error(vm, "web.fetch expects url and optional init!"); return 0; }
    lt_Value init = argc == 2 ? web_api->pop(vm) : LT_VALUE_NULL;
    lt_Value url_value = web_api->pop(vm);
    if (!LT_IS_STRING(url_value)) { web_api->runtime_error(vm, "web.fetch url must be string!"); return 0; }
    web_api->root(vm, url_value);
    web_api->root(vm, init);
    const char* url = web_api->get_string(vm, url_value);
    char host[256], path[1024];
    int port, secure;
    if (!parse_url(url, host, sizeof(host), &port, &secure, path, sizeof(path)) || !host[0] || port > 65535 ||
        strpbrk(host, " \r\n\t") || strpbrk(path, " \r\n\t")) {
        lt_Value promise = ltasync_make_promise(vm);
        web_api->root(vm, promise);
        ltasync_settle_promise(vm, promise, LT_PROMISE_REJECTED, web_api->make_string(vm, "Unsupported URL"));
        web_api->push(vm, promise);
        return 1;
    }

    const char* method = "GET";
    const char* body = "";
    lt_Value headers = LT_VALUE_NULL;
    if (LT_IS_TABLE(init)) {
        lt_Value value = web_api->table_get(vm, init, web_api->make_string(vm, "method"));
        if (LT_IS_STRING(value)) method = web_api->get_string(vm, value);
        value = web_api->table_get(vm, init, web_api->make_string(vm, "body"));
        if (LT_IS_STRING(value)) body = web_api->get_string(vm, value);
        headers = web_api->table_get(vm, init, web_api->make_string(vm, "headers"));
    }
    if (!web_header_valid(method, "")) { web_api->runtime_error(vm, "Invalid HTTP method!"); return 0; }
    char header[8192]; size_t header_len = 0;
    int valid;
    if (strchr(host, ':') && port == (secure ? 443 : 80))
        valid = web_append(header, sizeof(header), &header_len, "%s %s HTTP/1.1\r\nHost: [%s]\r\nConnection: close\r\n", method, path, host);
    else if (strchr(host, ':'))
        valid = web_append(header, sizeof(header), &header_len, "%s %s HTTP/1.1\r\nHost: [%s]:%d\r\nConnection: close\r\n", method, path, host, port);
    else if (port == (secure ? 443 : 80))
        valid = web_append(header, sizeof(header), &header_len, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n", method, path, host);
    else
        valid = web_append(header, sizeof(header), &header_len, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n", method, path, host, port);
    size_t body_len = strlen(body);
    if (body_len) valid = valid && web_append(header, sizeof(header), &header_len, "Content-Length: %zu\r\n", body_len);
    uint64_t cursor = 0; lt_Value key, value;
    while (LT_IS_TABLE(headers) && web_api->table_next(vm, headers, &cursor, &key, &value)) {
        if (!LT_IS_STRING(key) || !LT_IS_STRING(value)) { valid = 0; break; }
        const char* name = web_api->get_string(vm, key);
        const char* text = web_api->get_string(vm, value);
        if (!web_header_valid(name, text)) { valid = 0; break; }
        if (web_header_is(name, "Host") || web_header_is(name, "Connection") ||
            web_header_is(name, "Content-Length") || web_header_is(name, "Transfer-Encoding")) continue;
        valid = valid && web_append(header, sizeof(header), &header_len, "%s: %s\r\n", name, text);
    }
    valid = valid && web_append(header, sizeof(header), &header_len, "\r\n");
    if (!valid || body_len > SIZE_MAX - header_len - 1) { web_api->runtime_error(vm, "Invalid or oversized request headers!"); return 0; }

    WebFetch* f = calloc(1, sizeof(*f));
    if (!f) { web_api->runtime_error(vm, "Out of memory creating fetch!"); return 0; }
    f->url = web_strdup(url);
    f->request = malloc(header_len + body_len + 1);
    if (!f->url || !f->request) {
        free(f->url); free(f->request); free(f);
        web_api->runtime_error(vm, "Out of memory creating fetch!");
        return 0;
    }
    memcpy(f->host, host, strlen(host) + 1);
    f->port = port;
    f->secure = secure;
    f->head = web_header_is(method, "HEAD");
    memcpy(f->request, header, header_len);
    memcpy(f->request + header_len, body, body_len);
    f->request_len = header_len + body_len;
    atomic_init(&f->refs, 2);
    atomic_init(&f->done, 0);
    f->promise = ltasync_make_promise(vm);
    web_api->root_persistent(vm, f->promise);
    f->hook = ltasync_add_owned_poll_hook(vm, web_fetch_poll, f, web_fetch_destroy);
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, web_fetch_thread, f, 0, NULL);
    if (thread) CloseHandle(thread);
    else {
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, web_fetch_thread, f) == 0) pthread_detach(thread);
    else {
#endif
        f->error = "Failed to start fetch worker";
        atomic_store_explicit(&f->done, 1, memory_order_release);
        web_fetch_release(f);
    }
    web_api->push(vm, f->promise);
    return 1;
}
/* ---------- serve ---------- */

static void web_server_remove_client(lt_VM* vm, WebServerData* sd, size_t index)
{
    WebClientData* client = sd->clients[index];
    if (!client) return;
    web_close_socket(client->socket);
    free(client->output);
    if (!LT_IS_NULL(client->pending)) web_api->unroot_persistent(vm, client->pending);
    free(client);
    sd->clients[index] = 0;
}

static void web_server_hook_destroy(lt_VM* vm, void* context)
{
    WebServerHook* hook = context;
    WebServerData* sd = hook->server;
    for (size_t i = 0; i < WEB_SERVER_CLIENT_LIMIT; ++i) web_server_remove_client(vm, sd, i);
    web_close_socket(sd->socket);
    sd->socket = WEB_BAD_SOCKET;
    sd->closed = 1;
#ifdef _WIN32
    WSACleanup();
#endif
    web_api->unroot_persistent(vm, hook->handler);
    web_api->unroot_persistent(vm, hook->instance);
    free(hook);
}

/* Determine whether a complete request is available. */
static int web_server_request_length(WebClientData* sd, size_t* total)
{
    char* end = strstr(sd->input, "\r\n\r\n");
    if (!end) return 0;
    char* first = strstr(sd->input, "\r\n");
    if (!first || first > end) return -1;
    size_t length = 0; int has_length = 0;
    for (char* p = first + 2; p < end;) {
        char* next = strstr(p, "\r\n");
        if (!next || next > end) return -1;
        char* colon = memchr(p, ':', (size_t)(next - p));
        if (!colon) return -1;
        size_t n = (size_t)(colon - p);
        char name[64];
        if (n >= sizeof(name)) return -1;
        memcpy(name, p, n); name[n] = 0;
        char* value = colon + 1;
        while (value < next && (*value == ' ' || *value == '\t')) ++value;
        if (web_header_is(name, "Transfer-Encoding")) return -1;
        if (web_header_is(name, "Content-Length")) {
            size_t num = 0; char* q = value;
            if (q == next || !isdigit((unsigned char)*q)) return -1;
            while (q < next && isdigit((unsigned char)*q)) {
                unsigned digit = (unsigned)(*q++ - '0');
                if (num > (SIZE_MAX - digit) / 10) return -1;
                num = num * 10 + digit;
            }
            while (q < next && (*q == ' ' || *q == '\t')) ++q;
            if (q != next || (has_length && length != num)) return -1;
            length = num; has_length = 1;
        }
        p = next + 2;
    }
    size_t header_len = (size_t)(end + 4 - sd->input);
    if (length > sizeof(sd->input) - 1 - header_len) return -1;
    *total = header_len + length;
    return sd->input_len >= *total ? 1 : 0;
}

static int web_server_output(lt_VM* vm, WebClientData* sd, lt_Value result, int failed)
{
    int status = failed ? 500 : 200;
    const char* status_text = failed ? "Internal Server Error" : "OK";
    const char* body = failed ? "Internal Server Error" : "";
    lt_Value headers = LT_VALUE_NULL;
    web_api->root(vm, result);
    if (!failed && LT_IS_STRING(result)) body = web_api->get_string(vm, result);
    else if (!failed && LT_IS_INSTANCE(result)) {
        lt_Object* klass = LT_GET_OBJECT(result)->instance.klass;
        while (klass && klass->class_def.native_data_destroy != web_destroy_response) klass = klass->class_def.superclass;
        if (klass) {
            WebResponseData* rd = web_api->instance_get_native_data(vm, result, LT_VALUE_OBJECT(klass));
            if (rd) { status = rd->status; status_text = rd->status_text; body = rd->body; headers = rd->headers; }
        }
    }
    if (!body) body = "";
    if (!status_text || strpbrk(status_text, "\r\n")) return 0;
    /* A handler result is the final response; informational statuses alone
       cannot terminate an HTTP exchange. */
    if (status < 200) {
        status = 500; status_text = "Internal Server Error";
        body = "Internal Server Error"; headers = LT_VALUE_NULL;
    }
    char header[8192]; size_t len = 0;
    int ok = web_append(header, sizeof(header), &len, "HTTP/1.1 %d %s\r\n", status, status_text);
    uint64_t cursor = 0; lt_Value key, value;
    while (LT_IS_TABLE(headers) && web_api->table_next(vm, headers, &cursor, &key, &value)) {
        if (!LT_IS_STRING(key) || !LT_IS_STRING(value)) return 0;
        const char* name = web_api->get_string(vm, key);
        const char* text = web_api->get_string(vm, value);
        if (!web_header_valid(name, text)) return 0;
        if (web_header_is(name, "Content-Length") || web_header_is(name, "Connection") ||
            web_header_is(name, "Transfer-Encoding")) continue;
        ok = ok && web_append(header, sizeof(header), &len, "%s: %s\r\n", name, text);
    }
    size_t body_len = strlen(body);
    if (status == 204 || status == 304 || (status >= 100 && status < 200)) body_len = 0;
    if (status != 204 && status != 304)
        ok = ok && web_append(header, sizeof(header), &len, "Content-Length: %zu\r\n", body_len);
    ok = ok && web_append(header, sizeof(header), &len, "Connection: close\r\n\r\n");
    if (sd->head_request) body_len = 0;
    if (!ok || body_len > SIZE_MAX - len) return 0;
    sd->output = malloc(len + body_len);
    if (!sd->output) return 0;
    memcpy(sd->output, header, len);
    memcpy(sd->output + len, body, body_len);
    sd->output_len = len + body_len;
    return 1;
}

static int web_server_handle(lt_VM* vm, WebServerHook* hook, WebClientData* sd, size_t total)
{
    sd->input[total] = 0;
    char method[32], path[2048], version[16];
    if (sscanf(sd->input, "%31s %2047s %15s", method, path, version) != 3 ||
        strncmp(version, "HTTP/1.", 7) != 0) return 0;
    sd->head_request = web_header_is(method, "HEAD");
    char* end = strstr(sd->input, "\r\n\r\n");
    char* body = end + 4;
    lt_Value headers = web_api->make_table(vm);
    web_api->root(vm, headers);
    char* p = strstr(sd->input, "\r\n") + 2;
    while (p < end) {
        char* next = strstr(p, "\r\n");
        char* colon = memchr(p, ':', (size_t)(next - p));
        *colon = 0; *next = 0;
        char* value = colon + 1;
        while (*value == ' ' || *value == '\t') ++value;
        if (!web_header_valid(p, value)) return 0;
        web_api->table_set(vm, headers, web_api->make_string(vm, p), web_api->make_string(vm, value));
        p = next + 2;
    }
    lt_Value request = create_request_instance(vm, path, method, body, headers);
    web_api->push(vm, request);
    lt_Value result = LT_VALUE_NULL;
    hook->server->dispatching = 1;
    if (ltasync_is_async_callable(hook->handler)) result = ltasync_call(vm, hook->handler, 1);
    else {
        uint16_t count = web_api->exec(vm, hook->handler, 1);
        if (count) { result = web_api->pop(vm); while (--count) web_api->pop(vm); }
    }
    hook->server->dispatching = 0;
    if (hook->server->closed) { web_api->remove_poll_hook(vm, hook->server->hook); return 1; }
    if (web_api->is_promise(result)) {
        sd->pending = result;
        web_api->root_persistent(vm, result);
        return 1;
    }
    return web_server_output(vm, sd, result, 0);
}

static lt_PollResult web_server_poll_client(lt_VM* vm, WebServerHook* hook, size_t index)
{
    WebServerData* server = hook->server;
    WebClientData* sd = server->clients[index];
    if (!LT_IS_NULL(sd->pending)) {
        lt_PromiseState state = web_api->promise_state(sd->pending);
        if (state == LT_PROMISE_PENDING) return LT_POLL_PENDING;
        lt_Value result = web_api->promise_result(sd->pending);
        web_api->root(vm, result);
        web_api->unroot_persistent(vm, sd->pending);
        sd->pending = LT_VALUE_NULL;
        if (!web_server_output(vm, sd, result, state == LT_PROMISE_REJECTED)) web_server_remove_client(vm, server, index);
        return LT_POLL_WORK;
    }
    if (sd->output) {
        size_t remaining = sd->output_len - sd->output_sent;
        int count = send(sd->socket, sd->output + sd->output_sent, remaining > INT_MAX ? INT_MAX : (int)remaining,
#ifdef MSG_NOSIGNAL
            MSG_NOSIGNAL
#else
            0
#endif
        );
        if (count > 0) {
            sd->output_sent += (size_t)count;
            if (sd->output_sent == sd->output_len) web_server_remove_client(vm, server, index);
            return LT_POLL_WORK;
        }
        if (count < 0 && web_would_block()) return LT_POLL_PENDING;
        web_server_remove_client(vm, server, index);
        return LT_POLL_WORK;
    }
    if (sd->input_len == sizeof(sd->input) - 1) { web_server_remove_client(vm, server, index); return LT_POLL_WORK; }
    int count = recv(sd->socket, sd->input + sd->input_len, (int)(sizeof(sd->input) - 1 - sd->input_len), 0);
    if (count < 0 && web_would_block()) return LT_POLL_PENDING;
    if (count <= 0 || memchr(sd->input + sd->input_len, 0, (size_t)(count > 0 ? count : 0))) {
        web_server_remove_client(vm, server, index); return LT_POLL_WORK;
    }
    sd->input_len += (size_t)count;
    sd->input[sd->input_len] = 0;
    size_t total = 0;
    int ready = web_server_request_length(sd, &total);
    if (ready < 0 || (ready == 1 && !web_server_handle(vm, hook, sd, total))) web_server_remove_client(vm, server, index);
    return LT_POLL_WORK;
}

static lt_PollResult web_server_poll(lt_VM* vm, void* context)
{
    WebServerHook* hook = context;
    WebServerData* sd = hook->server;
    if (sd->closed) return LT_POLL_IDLE;
    if (sd->dispatching) return LT_POLL_PENDING;

    lt_PollResult result = LT_POLL_PENDING;
    /* Leave connections in the listen backlog when every slot is occupied. */
    for (size_t i = 0; i < WEB_SERVER_CLIENT_LIMIT; ++i) {
        if (sd->clients[i]) continue;
        WebSocket socket_fd = accept(sd->socket, 0, 0);
        if (socket_fd == WEB_BAD_SOCKET) break;
        web_no_sigpipe(socket_fd);
        WebClientData* client = calloc(1, sizeof(*client));
        if (!client || !web_nonblocking(socket_fd)) {
            free(client);
            web_close_socket(socket_fd);
        } else {
            client->socket = socket_fd;
            client->pending = LT_VALUE_NULL;
            sd->clients[i] = client;
        }
        result = LT_POLL_WORK;
    }
    for (size_t i = 0; i < WEB_SERVER_CLIENT_LIMIT && !sd->closed; ++i) {
        if (sd->clients[i] && web_server_poll_client(vm, hook, i) == LT_POLL_WORK)
            result = LT_POLL_WORK;
    }
    return result;
}

static uint8_t web_serve(lt_VM* vm, uint8_t argc)
{
    if (argc < 1 || argc > 2) { web_api->runtime_error(vm, "web.serve expects handler and optional options!"); return 0; }
    lt_Value options = argc == 2 ? web_api->pop(vm) : LT_VALUE_NULL;
    lt_Value handler = web_api->pop(vm);
    if (!ltasync_is_callable(handler)) { web_api->runtime_error(vm, "web.serve handler must be callable!"); return 0; }
    web_api->root(vm, handler);
    web_api->root(vm, options);
    int port = 8000; const char* hostname = "0.0.0.0";
    if (LT_IS_TABLE(options)) {
        lt_Value value = web_api->table_get(vm, options, web_api->make_string(vm, "port"));
        if (LT_IS_NUMBER(value)) {
            double n = web_api->get_number(value);
            if (!(n >= 0 && n <= 65535) || (double)(int)n != n) { web_api->runtime_error(vm, "Invalid server port!"); return 0; }
            port = (int)n;
        }
        value = web_api->table_get(vm, options, web_api->make_string(vm, "hostname"));
        if (LT_IS_STRING(value)) hostname = web_api->get_string(vm, value);
    }
    struct sockaddr_in address; memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET; address.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, hostname, &address.sin_addr) != 1) { web_api->runtime_error(vm, "Server hostname must be an IPv4 address!"); return 0; }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { web_api->runtime_error(vm, "Socket initialization failed!"); return 0; }
#endif
    WebSocket socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == WEB_BAD_SOCKET) {
#ifdef _WIN32
        WSACleanup();
#endif
        web_api->runtime_error(vm, "Failed to create server socket!"); return 0;
    }
    int reuse = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    if (bind(socket_fd, (struct sockaddr*)&address, sizeof(address)) != 0 ||
        listen(socket_fd, 16) != 0 || !web_nonblocking(socket_fd)) {
        web_close_socket(socket_fd);
#ifdef _WIN32
        WSACleanup();
#endif
        web_api->runtime_error(vm, "Failed to start server socket!"); return 0;
    }
    socklen_t address_len = sizeof(address);
    if (getsockname(socket_fd, (struct sockaddr*)&address, &address_len) != 0) {
        web_close_socket(socket_fd);
#ifdef _WIN32
        WSACleanup();
#endif
        web_api->runtime_error(vm, "Failed to inspect server socket!"); return 0;
    }
    WebServerData* sd = calloc(1, sizeof(*sd));
    WebServerHook* hook = calloc(1, sizeof(*hook));
    if (!sd || !hook) {
        free(sd); free(hook); web_close_socket(socket_fd);
#ifdef _WIN32
        WSACleanup();
#endif
        web_api->runtime_error(vm, "Out of memory starting server!"); return 0;
    }
    sd->socket = socket_fd; sd->port = ntohs(address.sin_port);
    uint16_t count = web_api->exec(vm, web_class(vm, "Server"), 0);
    lt_Value instance = count ? web_api->pop(vm) : LT_VALUE_NULL;
    while (count > 1) { web_api->pop(vm); --count; }
    if (!LT_IS_INSTANCE(instance)) {
        free(sd); free(hook); web_close_socket(socket_fd);
#ifdef _WIN32
        WSACleanup();
#endif
        web_api->runtime_error(vm, "Failed to create server instance!"); return 0;
    }
    web_api->root(vm, instance);
    web_api->instance_set_native_data(vm, instance, web_class(vm, "Server"), sd);
    hook->server = sd; hook->instance = instance; hook->handler = handler;
    web_api->root_persistent(vm, instance);
    web_api->root_persistent(vm, handler);
    sd->hook = ltasync_add_owned_poll_hook(vm, web_server_poll, hook, web_server_hook_destroy);
    web_api->push(vm, instance);
    return 1;
}

static uint8_t server_close(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) { web_api->runtime_error(vm, "Server.close expects no arguments!"); return 0; }
    lt_Value instance = web_api->pop(vm);
    WebServerData* sd = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_server));
    if (sd && !sd->closed) {
        sd->closed = 1;
        web_close_socket(sd->socket); sd->socket = WEB_BAD_SOCKET;
        for (size_t i = 0; i < WEB_SERVER_CLIENT_LIMIT; ++i) {
            if (sd->clients[i]) {
                web_close_socket(sd->clients[i]->socket);
                sd->clients[i]->socket = WEB_BAD_SOCKET;
            }
        }
        if (!sd->dispatching) web_api->remove_poll_hook(vm, sd->hook);
    }
    return 0;
}

static uint8_t server_port(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = web_api->pop(vm);
    WebServerData* sd = web_api->instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_server));
    web_api->push(vm, web_api->make_number(sd ? sd->port : 0));
    return 1;
}
void ltstd_open_web(lt_VM* vm)
{
    lt_Value response_class, request_class, server_class;

    response_class = web_api->class_create(vm, "Response");
    web_api->class_set_native_data_destroy(vm, response_class, web_destroy_response);
    web_api->class_set_constructor(vm, response_class, response_constructor);
    web_api->class_add_getter(vm, response_class, "status", response_status, LT_VIS_PUBLIC, 0);
    web_api->class_add_getter(vm, response_class, "statusText", response_status_text, LT_VIS_PUBLIC, 0);
    web_api->class_add_getter(vm, response_class, "ok", response_ok, LT_VIS_PUBLIC, 0);
    web_api->class_add_getter(vm, response_class, "url", response_url, LT_VIS_PUBLIC, 0);
    web_api->class_add_getter(vm, response_class, "headers", response_headers, LT_VIS_PUBLIC, 0);
    web_api->class_add_getter(vm, response_class, "body", response_body, LT_VIS_PUBLIC, 0);
    web_api->class_add_method(vm, response_class, "text", response_text, LT_VIS_PUBLIC, 0);
    web_api->class_add_method(vm, response_class, "json", response_json, LT_VIS_PUBLIC, 0);

    request_class = web_api->class_create(vm, "Request");
    web_api->class_set_native_data_destroy(vm, request_class, web_destroy_request);
    web_api->class_add_getter(vm, request_class, "url", request_url, LT_VIS_PUBLIC, 0);
    web_api->class_add_getter(vm, request_class, "method", request_method, LT_VIS_PUBLIC, 0);
    web_api->class_add_getter(vm, request_class, "body", request_body, LT_VIS_PUBLIC, 0);
    web_api->class_add_getter(vm, request_class, "headers", request_headers, LT_VIS_PUBLIC, 0);

    server_class = web_api->class_create(vm, "Server");
    web_api->class_set_native_data_destroy(vm, server_class, web_destroy_server);
    web_api->class_add_method(vm, server_class, "close", server_close, LT_VIS_PUBLIC, 0);
    web_api->class_add_getter(vm, server_class, "port", server_port, LT_VIS_PUBLIC, 0);

    lt_Value registry = web_api->make_table(vm);
    web_api->table_set(vm, registry, web_api->make_string(vm, "Response"), response_class);
    web_api->table_set(vm, registry, web_api->make_string(vm, "Request"), request_class);
    web_api->table_set(vm, registry, web_api->make_string(vm, "Server"), server_class);
    web_api->table_set(vm, vm->global, web_api->make_string(vm, "__little_web_classes"), registry);
    lt_Value web = web_api->make_table(vm);
    web_api->table_set(vm, web, web_api->make_string(vm, "fetch"), web_api->make_native(vm, web_fetch));
    web_api->table_set(vm, web, web_api->make_string(vm, "serve"), web_api->make_native(vm, web_serve));
    web_api->table_set(vm, web, web_api->make_string(vm, "Request"), request_class);
    web_api->table_set(vm, web, web_api->make_string(vm, "Response"), response_class);
    web_api->table_set(vm, web, web_api->make_string(vm, "Server"), server_class);

    web_api->table_set(vm, vm->global, web_api->make_string(vm, "web"), web);
}
