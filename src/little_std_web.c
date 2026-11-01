#define _GNU_SOURCE
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "little_std.h"
#include "little_async.h"
#include "little.h"
#include "little_runtime_internal.h"
#include "little_buffer_internal.h"
#include "little_async_internal.h"
#include "little_std_internal.h"
#include "little_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <curl/curl.h>
#include "llhttp.h"

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
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

#define WEB_STREAM_QUEUE_LIMIT (4u * 1024u * 1024u)

typedef struct WebStreamChunk {
    struct WebStreamChunk* next;
    size_t length;
    char* data;
} WebStreamChunk;

typedef struct {
    atomic_int refs;
    atomic_int done;
    atomic_int stop;
    atomic_int headers_ready;
    lt_VM* vm;
    uint32_t hook;
    lt_Value promise;
    lt_Value pending_next;
    char* url;
    char* method;
    char* body;
    char* proxy;
    struct curl_slist* headers;
    long redirects;
    int decompress;
    int head;
    int binary;
    int status;
    int headers_sent;
    char status_text[256];
    char* header_data;
    size_t header_len;
    size_t header_cap;
    char* error;
    WebStreamChunk* chunk_head;
    WebStreamChunk* chunk_tail;
    size_t queued_bytes;
#if defined(_WIN32)
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE wake;
#else
    pthread_mutex_t lock;
    pthread_cond_t wake;
#endif
} WebStreamState;

/* At most this many requests can be active on one listener. */
#define WEB_SERVER_CLIENT_LIMIT 8

typedef struct {
#ifdef _WIN32
    SOCKET socket;
#else
    int socket;
#endif
    lt_VM* vm;
    llhttp_t parser;
    lt_Value headers;
    char input[65536];
    size_t input_len;
    char method[32];
    size_t method_len;
    char path[2048];
    size_t path_len;
    char header_name[256];
    size_t header_name_len;
    char header_value[8192];
    size_t header_value_len;
    char body[65536];
    size_t body_len;
    uint8_t chunked_request;
    uint8_t message_complete;
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


/* Classes belong to their VM. Keep their registry reachable independently of
   the public web table, which scripts may replace. */
static lt_Value web_class(lt_VM* vm, const char* name)
{
    lt_Value registry = lt_table_get(vm, vm->global, lt_make_string(vm, "__little_web_classes"));
    lt_Value klass = lt_table_get(vm, registry, lt_make_string(vm, name));
    if (!LT_IS_CLASS(klass)) lt_runtime_error(vm, "Web class registry is unavailable!");
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

static void web_stream_lock(WebStreamState* stream)
{
#if defined(_WIN32)
    EnterCriticalSection(&stream->lock);
#else
    pthread_mutex_lock(&stream->lock);
#endif
}

static void web_stream_unlock(WebStreamState* stream)
{
#if defined(_WIN32)
    LeaveCriticalSection(&stream->lock);
#else
    pthread_mutex_unlock(&stream->lock);
#endif
}

static void web_stream_wait(WebStreamState* stream)
{
#if defined(_WIN32)
    SleepConditionVariableCS(&stream->wake, &stream->lock, INFINITE);
#else
    pthread_cond_wait(&stream->wake, &stream->lock);
#endif
}

static void web_stream_wake(WebStreamState* stream)
{
#if defined(_WIN32)
    WakeAllConditionVariable(&stream->wake);
#else
    pthread_cond_broadcast(&stream->wake);
#endif
}

static void web_stream_clear_chunks(WebStreamState* stream)
{
    WebStreamChunk* chunk = stream->chunk_head;
    while (chunk)
    {
        WebStreamChunk* next = chunk->next;
        free(chunk->data);
        free(chunk);
        chunk = next;
    }
    stream->chunk_head = 0;
    stream->chunk_tail = 0;
    stream->queued_bytes = 0;
}

static void web_stream_release(WebStreamState* stream)
{
    if (!stream || atomic_fetch_sub(&stream->refs, 1) != 1) return;
#if defined(_WIN32)
    DeleteCriticalSection(&stream->lock);
#else
    pthread_cond_destroy(&stream->wake);
    pthread_mutex_destroy(&stream->lock);
#endif
    web_stream_clear_chunks(stream);
    if (stream->headers) curl_slist_free_all(stream->headers);
    free(stream->url);
    free(stream->method);
    free(stream->body);
    free(stream->proxy);
    free(stream->header_data);
    free(stream->error);
    free(stream);
}

static void web_stream_destroy_data(void* data)
{
    WebStreamState* stream = data;
    if (!stream) return;
    atomic_store(&stream->stop, 1);
    web_stream_lock(stream);
    web_stream_wake(stream);
    web_stream_unlock(stream);
    web_stream_release(stream);
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

#define web_strdup lt_net_strdup
#define web_close_socket lt_net_close
#define web_nonblocking lt_net_nonblocking
#define web_no_sigpipe lt_net_no_sigpipe

static lt_Value web_instance_class(lt_VM* vm, lt_Value instance, lt_NativeDataDestroyFn destroy)
{
    if (LT_IS_INSTANCE(instance)) {
        for (lt_Object* klass = LT_GET_OBJECT(instance)->instance.klass; klass; klass = klass->class_def.superclass)
            if (klass->class_def.native_data_destroy == destroy) return LT_VALUE_OBJECT(klass);
    }
    lt_runtime_error(vm, "Invalid web instance!");
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
    if (argc < 1 || argc > 3) { lt_runtime_error(vm, "Response expects optional body and options!"); return 0; }
    lt_Value options = argc == 3 ? lt_pop(vm) : LT_VALUE_NULL;
    lt_Value body = argc >= 2 ? lt_pop(vm) : LT_VALUE_NULL;
    lt_Value instance = lt_pop(vm);
    lt_native_root_internal(vm, instance);
    lt_native_root_internal(vm, options);
    lt_native_root_internal(vm, body);
    if (!LT_IS_NULL(body) && !LT_IS_STRING(body)) { lt_runtime_error(vm, "Response body must be a string!"); return 0; }
    if (!LT_IS_NULL(options) && !LT_IS_TABLE(options)) { lt_runtime_error(vm, "Response options must be a table!"); return 0; }
    int status = 200;
    lt_Value headers = lt_make_table(vm);
    const char* status_text = 0;
    if (LT_IS_TABLE(options)) {
        lt_Value value = lt_table_get(vm, options, lt_make_string(vm, "status"));
        if (!LT_IS_NULL(value)) {
            if (!LT_IS_NUMBER(value)) { lt_runtime_error(vm, "Response status must be an integer from 100 to 599!"); return 0; }
            double n = lt_get_number(value);
            if (!(n >= 100 && n <= 599) || n != (double)(int)n) { lt_runtime_error(vm, "Response status must be an integer from 100 to 599!"); return 0; }
            status = (int)n;
        }
        value = lt_table_get(vm, options, lt_make_string(vm, "statusText"));
        if (!LT_IS_NULL(value)) {
            if (!LT_IS_STRING(value)) { lt_runtime_error(vm, "Response statusText must be a string!"); return 0; }
            status_text = lt_get_string(vm, value);
            if (strpbrk(status_text, "\r\n")) { lt_runtime_error(vm, "Invalid response statusText!"); return 0; }
        }
        value = lt_table_get(vm, options, lt_make_string(vm, "headers"));
        if (!LT_IS_NULL(value)) {
            if (!LT_IS_TABLE(value)) { lt_runtime_error(vm, "Response headers must be a table!"); return 0; }
            headers = value;
        }
    }
    WebResponseData* rd = calloc(1, sizeof(*rd));
    if (!rd) { lt_runtime_error(vm, "Out of memory creating web response!"); return 0; }
    rd->status = status;
    rd->status_text = web_strdup(status_text ? status_text : web_reason(status));
    rd->body = web_strdup(LT_IS_STRING(body) ? lt_get_string(vm, body) : "");
    rd->url = web_strdup("");
    rd->headers = headers;
    if (!rd->status_text || !rd->body || !rd->url) {
        web_destroy_response(rd); lt_runtime_error(vm, "Out of memory creating web response!"); return 0;
    }
    web_keep_headers(vm, instance, headers);
    lt_instance_set_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response), rd);
    return 0;
}

static uint8_t response_status(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebResponseData* rd = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    lt_push(vm, lt_make_number(rd ? (double)rd->status : 0.0));
    return 1;
}

static uint8_t response_status_text(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebResponseData* rd = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    lt_push(vm, lt_make_string(vm, rd && rd->status_text ? rd->status_text : ""));
    return 1;
}

static uint8_t response_ok(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebResponseData* rd = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    int ok = rd && rd->status >= 200 && rd->status < 300;
    lt_push(vm, ok ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t response_url(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebResponseData* rd = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    lt_push(vm, lt_make_string(vm, rd && rd->url ? rd->url : ""));
    return 1;
}

static uint8_t response_headers(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebResponseData* rd = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    lt_push(vm, rd && LT_IS_OBJECT(rd->headers) ? rd->headers : lt_make_table(vm));
    return 1;
}

static uint8_t response_body(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebResponseData* rd = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    lt_push(vm, lt_make_string(vm, rd && rd->body ? rd->body : ""));
    return 1;
}

static uint8_t response_text(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Response.text expects no arguments!");
    lt_Value instance = lt_pop(vm);
    WebResponseData* rd = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    lt_Value promise = ltasync_make_promise(vm);
    ltasync_settle_promise(vm, promise, LT_PROMISE_FULFILLED, lt_make_string(vm, rd && rd->body ? rd->body : ""));
    lt_push(vm, promise);
    return 1;
}

static uint8_t response_json(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Response.json expects no arguments!");
    lt_Value instance = lt_pop(vm);
    WebResponseData* rd = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    lt_Value value = LT_VALUE_NULL;
    uint8_t ok = lt_json_parse(vm, rd && rd->body ? rd->body : "", &value);
    if (ok) lt_native_root_internal(vm, value);
    lt_Value promise = ltasync_make_promise(vm);
    lt_native_root_internal(vm, promise);
    if (ok) ltasync_settle_promise(vm, promise, LT_PROMISE_FULFILLED, value);
    else ltasync_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, "Invalid JSON response body!"));
    lt_push(vm, promise);
    return 1;
}

static uint8_t request_url(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebRequestData* rq = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_request));
    lt_push(vm, lt_make_string(vm, rq && rq->url ? rq->url : ""));
    return 1;
}

static uint8_t request_method(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebRequestData* rq = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_request));
    lt_push(vm, lt_make_string(vm, rq && rq->method ? rq->method : "GET"));
    return 1;
}

static uint8_t request_body(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebRequestData* rq = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_request));
    lt_push(vm, lt_make_string(vm, rq && rq->body ? rq->body : ""));
    return 1;
}

static uint8_t request_headers(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebRequestData* rq = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_request));
    lt_push(vm, rq && LT_IS_OBJECT(rq->headers) ? rq->headers : lt_make_table(vm));
    return 1;
}

/* ---------- helpers ---------- */

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

typedef lt_NetSocket WebSocket;
#define WEB_BAD_SOCKET LT_NET_BAD_SOCKET

static int web_would_block(void)
{
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static lt_Value create_response_instance(lt_VM* vm, int status, const char* status_text, lt_Value headers, const char* body, const char* url)
{
    lt_native_root_internal(vm, headers);
    uint16_t nret = lt_exec(vm, web_class(vm, "Response"), 0);
    lt_Value instance = LT_VALUE_NULL;
    if (nret > 0) { instance = lt_pop(vm); for (uint16_t i = 1; i < nret; ++i) lt_pop(vm); }
    if (!LT_IS_INSTANCE(instance)) return LT_VALUE_NULL;
    lt_native_root_internal(vm, instance);
    WebResponseData* rd = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_response));
    if (!rd) { lt_runtime_error(vm, "Response constructor did not initialize its data!"); return LT_VALUE_NULL; }
    char* new_status_text = web_strdup(status_text ? status_text : "");
    char* new_body = web_strdup(body ? body : "");
    char* new_url = web_strdup(url ? url : "");
    if (!new_status_text || !new_body || !new_url) {
        free(new_status_text); free(new_body); free(new_url);
        lt_runtime_error(vm, "Out of memory creating web response!");
        return LT_VALUE_NULL;
    }
    free(rd->status_text); free(rd->body); free(rd->url);
    rd->status = status;
    rd->status_text = new_status_text;
    rd->headers = LT_IS_OBJECT(headers) ? headers : lt_make_table(vm);
    rd->body = new_body;
    rd->url = new_url;
    web_keep_headers(vm, instance, rd->headers);
    return instance;
}

static lt_Value create_request_instance(lt_VM* vm, const char* url, const char* method, const char* body, lt_Value headers)
{
    lt_native_root_internal(vm, headers);
    uint16_t nret = lt_exec(vm, web_class(vm, "Request"), 0);
    lt_Value instance = LT_VALUE_NULL;
    if (nret > 0) { instance = lt_pop(vm); for (uint16_t i = 1; i < nret; ++i) lt_pop(vm); }
    if (!LT_IS_INSTANCE(instance)) return LT_VALUE_NULL;
    lt_native_root_internal(vm, instance);
    WebRequestData* rq = calloc(1, sizeof(WebRequestData));
    if (!rq) { lt_runtime_error(vm, "Out of memory creating web request!"); return LT_VALUE_NULL; }
    rq->url = web_strdup(url ? url : "");
    rq->method = web_strdup(method ? method : "GET");
    rq->body = web_strdup(body ? body : "");
    rq->headers = LT_IS_OBJECT(headers) ? headers : lt_make_table(vm);
    if (!rq->url || !rq->method || !rq->body) {
        web_destroy_request(rq);
        lt_runtime_error(vm, "Out of memory creating web request!");
        return LT_VALUE_NULL;
    }
    web_keep_headers(vm, instance, rq->headers);
    lt_instance_set_native_data(vm, instance, web_class(vm, "Request"), rq);
    return instance;
}

/* Network workers never call into the VM.  The hook and the detached worker
   each own one reference, so teardown can release the VM even during DNS. */
typedef struct {
    atomic_int refs;
    atomic_int done;
    lt_Value promise;
    uint32_t hook;
    char* url;
    char* method;
    char* body;
    char* proxy;
    struct curl_slist* headers;
    long redirects;
    int decompress;
    int head;
    int status;
    char status_text[256];
    char* header_data;
    size_t header_len;
    size_t header_cap;
    char* response_data;
    size_t response_len;
    size_t response_cap;
    int too_large;
    int binary;
    const char* error;
} WebFetch;

static void web_fetch_release(WebFetch* f)
{
    if (atomic_fetch_sub(&f->refs, 1) != 1) return;
    if (f->headers) curl_slist_free_all(f->headers);
    free(f->url);
    free(f->method);
    free(f->body);
    free(f->proxy);
    free(f->header_data);
    free(f->response_data);
    free(f);
}

static void web_fetch_destroy(lt_VM* vm, void* context)
{
    WebFetch* f = context;
    lt_native_unroot_persistent_internal(vm, f->promise);
    web_fetch_release(f);
}

static const char* web_fetch_curl_error(CURLcode code, const WebFetch* f)
{
    if (f->too_large) return "HTTP response too large";
    if (f->binary) return "Binary HTTP response is not supported";
    if (f->error) return f->error;
    switch (code)
    {
    case CURLE_COULDNT_RESOLVE_HOST: return "DNS lookup failed";
    case CURLE_COULDNT_CONNECT: return "Connection failed";
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM: return "TLS certificate verification failed";
    case CURLE_SSL_CONNECT_ERROR: return "TLS handshake failed";
    case CURLE_OPERATION_TIMEDOUT: return "HTTP response read failed";
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_URL_MALFORMAT: return "Unsupported URL";
    case CURLE_TOO_MANY_REDIRECTS: return "Too many redirects";
    case CURLE_WRITE_ERROR: return "HTTP response read failed";
    default: return "HTTP request failed";
    }
}

static size_t web_curl_header(char* data, size_t size, size_t count, void* userdata)
{
    WebFetch* f = userdata;
    size_t length = size * count;
    if (length >= 5 && strncmp(data, "HTTP/", 5) == 0)
    {
        int status = 0;
        char reason[256] = "";
        sscanf(data, "HTTP/%*u.%*u %d %255[^\r\n]", &status, reason);
        if (status >= 100)
        {
            f->status = status;
            strncpy(f->status_text, reason, sizeof(f->status_text) - 1);
            f->status_text[sizeof(f->status_text) - 1] = 0;
        }
        return length;
    }

    size_t line = length;
    while (line && (data[line - 1] == '\r' || data[line - 1] == '\n')) --line;
    if (line == 0) return length;
    if (f->header_len + line + 2 > 65535)
    {
        f->error = "HTTP response headers too large";
        return 0;
    }
    if (f->header_len + line + 2 > f->header_cap)
    {
        size_t capacity = f->header_cap ? f->header_cap * 2 : 1024;
        while (capacity < f->header_len + line + 2) capacity *= 2;
        char* grown = realloc(f->header_data, capacity);
        if (!grown) { f->error = "Out of memory"; return 0; }
        f->header_data = grown;
        f->header_cap = capacity;
    }
    memcpy(f->header_data + f->header_len, data, line);
    f->header_len += line;
    f->header_data[f->header_len++] = '\n';
    return length;
}

static size_t web_curl_body(char* data, size_t size, size_t count, void* userdata)
{
    WebFetch* f = userdata;
    size_t length = size * count;
    if (memchr(data, 0, length))
    {
        f->binary = 1;
        return 0;
    }
    if (f->response_len + length > 4194304)
    {
        f->too_large = 1;
        return 0;
    }
    if (f->response_len + length + 1 > f->response_cap)
    {
        size_t capacity = f->response_cap ? f->response_cap * 2 : 8192;
        while (capacity < f->response_len + length + 1) capacity *= 2;
        char* grown = realloc(f->response_data, capacity);
        if (!grown) { f->error = "Out of memory"; return 0; }
        f->response_data = grown;
        f->response_cap = capacity;
    }
    memcpy(f->response_data + f->response_len, data, length);
    f->response_len += length;
    f->response_data[f->response_len] = 0;
    return length;
}

static void web_fetch_worker(WebFetch* f)
{
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        f->error = "HTTP request failed";
        goto done;
    }

    char error_buffer[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_URL, f->url);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 10L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_curl_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, web_curl_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, f);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, f->headers);
    curl_easy_setopt(curl, CURLOPT_PROXY, f->proxy);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, f->redirects > 0 ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, f->redirects);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    const char* ca_file = getenv("CURL_CA_BUNDLE");
    if (!ca_file || !*ca_file) ca_file = getenv("SSL_CERT_FILE");
    if (ca_file && *ca_file) curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file);
    const char* ca_dir = getenv("SSL_CERT_DIR");
    if (ca_dir && *ca_dir) curl_easy_setopt(curl, CURLOPT_CAPATH, ca_dir);
#ifdef _WIN32
    if (!getenv("SSL_CERT_FILE")) curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif

    if (f->decompress) curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (f->head) curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    else if (f->body && f->body[0])
    {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, f->body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(f->body));
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, f->method);
    }
    else if (!web_header_is(f->method, "GET"))
    {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, f->method);
    }

    CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) f->error = web_fetch_curl_error(code, f);
    else
    {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status > 0) f->status = (int)status;
    }
    curl_easy_cleanup(curl);

done:
    atomic_store_explicit(&f->done, 1, memory_order_release);
    web_fetch_release(f);
}

#ifdef _WIN32
static DWORD WINAPI web_fetch_thread(LPVOID data) { web_fetch_worker(data); return 0; }
#else
static void* web_fetch_thread(void* data) { web_fetch_worker(data); return 0; }
#endif

static lt_Value web_headers_from_data(lt_VM* vm, char* header_data)
{
    lt_Value headers = lt_make_table(vm);
    char* cursor = header_data;
    while (cursor && *cursor)
    {
        char* end = strchr(cursor, '\n');
        if (!end) end = cursor + strlen(cursor);
        char saved = *end;
        *end = 0;
        char* colon = strchr(cursor, ':');
        if (colon)
        {
            *colon = 0;
            char* value = colon + 1;
            while (*value == ' ' || *value == '\t') ++value;
            char* tail = value + strlen(value);
            while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) --tail;
            *tail = 0;
            if (web_header_valid(cursor, value))
                lt_table_set(vm, headers, lt_make_string(vm, cursor), lt_make_string(vm, value));
        }
        *end = saved;
        cursor = saved ? end + 1 : end;
    }
    return headers;
}

static lt_PollResult web_fetch_poll(lt_VM* vm, void* context)
{
    WebFetch* f = context;
    if (!atomic_load_explicit(&f->done, memory_order_acquire)) return LT_POLL_PENDING;
    if (f->error)
    {
        ltasync_settle_promise(vm, f->promise, LT_PROMISE_REJECTED, lt_make_string(vm, f->error));
    }
    else
    {
        lt_Value headers = web_headers_from_data(vm, f->header_data);
        const char* body = f->response_data ? f->response_data : "";
        lt_Value response = create_response_instance(vm, f->status, f->status_text, headers, body, f->url);
        ltasync_settle_promise(vm, f->promise, LT_PROMISE_FULFILLED, response);
    }
    lt_remove_poll_hook(vm, f->hook);
    return LT_POLL_WORK;
}

static uint8_t web_fetch(lt_VM* vm, uint8_t argc)
{
    if (argc < 1 || argc > 2) { lt_runtime_error(vm, "web.fetch expects url and optional init!"); return 0; }
    lt_Value init = argc == 2 ? lt_pop(vm) : LT_VALUE_NULL;
    lt_Value url_value = lt_pop(vm);
    if (!LT_IS_STRING(url_value)) { lt_runtime_error(vm, "web.fetch url must be a string!"); return 0; }
    if (!LT_IS_NULL(init) && !LT_IS_TABLE(init)) { lt_runtime_error(vm, "web.fetch init must be a table!"); return 0; }
    lt_native_root_internal(vm, url_value);
    lt_native_root_internal(vm, init);

    const char* method = "GET";
    const char* body = "";
    lt_Value headers = LT_VALUE_NULL;
    long redirects = 0;
    const char* proxy = "";
    int decompress = 0;
    if (LT_IS_TABLE(init))
    {
        lt_Value value = lt_table_get(vm, init, lt_make_string(vm, "method"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_STRING(value)) { lt_runtime_error(vm, "web.fetch method must be a string!"); return 0; }
            method = lt_get_string(vm, value);
        }
        value = lt_table_get(vm, init, lt_make_string(vm, "body"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_STRING(value)) { lt_runtime_error(vm, "web.fetch body must be a string!"); return 0; }
            body = lt_get_string(vm, value);
        }
        value = lt_table_get(vm, init, lt_make_string(vm, "headers"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_TABLE(value)) { lt_runtime_error(vm, "web.fetch headers must be a table!"); return 0; }
            headers = value;
        }
        value = lt_table_get(vm, init, lt_make_string(vm, "redirects"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_NUMBER(value)) { lt_runtime_error(vm, "web.fetch redirects must be an integer from 0 to 20!"); return 0; }
            double n = lt_get_number(value);
            if (n < 0 || n > 20 || n != (double)(int)n) { lt_runtime_error(vm, "web.fetch redirects must be an integer from 0 to 20!"); return 0; }
            redirects = (long)n;
        }
        value = lt_table_get(vm, init, lt_make_string(vm, "proxy"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_STRING(value)) { lt_runtime_error(vm, "web.fetch proxy must be a string!"); return 0; }
            proxy = lt_get_string(vm, value);
        }
        value = lt_table_get(vm, init, lt_make_string(vm, "decompress"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_TRUE(value) && !LT_IS_FALSE(value)) { lt_runtime_error(vm, "web.fetch decompress must be a boolean!"); return 0; }
            decompress = LT_IS_TRUE(value);
        }
    }

    const char* url = lt_get_string(vm, url_value);
    WebFetch* f = calloc(1, sizeof(*f));
    if (!f) { lt_runtime_error(vm, "Out of memory creating fetch!"); return 0; }
    f->url = web_strdup(url);
    f->method = web_strdup(method);
    f->body = web_strdup(body);
    f->proxy = web_strdup(proxy);
    f->redirects = redirects;
    f->decompress = decompress;
    f->head = web_header_is(method, "HEAD");
    if (!f->url || !f->method || !f->body || !f->proxy)
    {
        web_fetch_release(f);
        lt_runtime_error(vm, "Out of memory creating fetch!");
        return 0;
    }

    if (LT_IS_TABLE(headers))
    {
        uint64_t cursor = 0;
        lt_Value key = LT_VALUE_NULL;
        lt_Value value = LT_VALUE_NULL;
        while (lt_table_next(vm, headers, &cursor, &key, &value))
        {
            if (!LT_IS_STRING(key) || !LT_IS_STRING(value)) { web_fetch_release(f); lt_runtime_error(vm, "web.fetch headers must contain string names and values!"); return 0; }
            const char* name = lt_get_string(vm, key);
            const char* text = lt_get_string(vm, value);
            if (!web_header_valid(name, text)) { web_fetch_release(f); lt_runtime_error(vm, "Invalid web.fetch header!"); return 0; }
            if (web_header_is(name, "Host") || web_header_is(name, "Connection") ||
                web_header_is(name, "Content-Length") || web_header_is(name, "Transfer-Encoding")) continue;
            size_t length = strlen(name) + strlen(text) + 3;
            char* line = malloc(length);
            if (!line) { web_fetch_release(f); lt_runtime_error(vm, "Out of memory creating fetch!"); return 0; }
            snprintf(line, length, "%s: %s", name, text);
            struct curl_slist* grown = curl_slist_append(f->headers, line);
            free(line);
            if (!grown) { web_fetch_release(f); lt_runtime_error(vm, "Out of memory creating fetch!"); return 0; }
            f->headers = grown;
        }
    }

    f->promise = ltasync_make_promise(vm);
    lt_native_root_persistent_internal(vm, f->promise);
    atomic_init(&f->refs, 2);
    atomic_init(&f->done, 0);
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
    lt_push(vm, f->promise);
    return 1;
}

/* ---------- fetchStream ---------- */

static void web_stream_retain(WebStreamState* stream)
{
    atomic_fetch_add(&stream->refs, 1);
}

static void web_stream_set_error_locked(WebStreamState* stream, const char* message)
{
    if (stream->error || !message) return;
    stream->error = web_strdup(message);
}

static void web_stream_set_error(WebStreamState* stream, const char* message)
{
    web_stream_lock(stream);
    web_stream_set_error_locked(stream, message);
    web_stream_unlock(stream);
}

static size_t web_stream_curl_header(char* data, size_t size, size_t count, void* userdata)
{
    WebStreamState* stream = userdata;
    size_t length = size * count;
    if (length >= 5 && strncmp(data, "HTTP/", 5) == 0)
    {
        int status = 0;
        char reason[256] = "";
        sscanf(data, "HTTP/%*u.%*u %d %255[^\r\n]", &status, reason);
        if (status >= 100)
        {
            stream->status = status;
            strncpy(stream->status_text, reason, sizeof(stream->status_text) - 1);
            stream->status_text[sizeof(stream->status_text) - 1] = 0;
            if (status >= 200)
            {
                stream->header_len = 0;
                if (stream->header_data) stream->header_data[0] = 0;
            }
        }
        return length;
    }

    size_t line = length;
    while (line && (data[line - 1] == '\r' || data[line - 1] == '\n')) --line;
    if (line == 0)
    {
        if (stream->status >= 200) atomic_store_explicit(&stream->headers_ready, 1, memory_order_release);
        return length;
    }
    if (stream->header_len + line + 2 > 65535)
    {
        web_stream_set_error(stream, "HTTP response headers too large");
        return 0;
    }
    if (stream->header_len + line + 2 > stream->header_cap)
    {
        size_t capacity = stream->header_cap ? stream->header_cap * 2 : 1024;
        while (capacity < stream->header_len + line + 2) capacity *= 2;
        char* grown = realloc(stream->header_data, capacity);
        if (!grown)
        {
            web_stream_set_error(stream, "Out of memory");
            return 0;
        }
        stream->header_data = grown;
        stream->header_cap = capacity;
    }
    memcpy(stream->header_data + stream->header_len, data, line);
    stream->header_len += line;
    stream->header_data[stream->header_len++] = '\n';
    stream->header_data[stream->header_len] = 0;
    return length;
}

static size_t web_stream_curl_body(char* data, size_t size, size_t count, void* userdata)
{
    WebStreamState* stream = userdata;
    size_t length = size * count;
    if (atomic_load_explicit(&stream->stop, memory_order_acquire)) return 0;
    if (!stream->binary && memchr(data, 0, length))
    {
        web_stream_set_error(stream, "Binary HTTP response is not supported; set binary: true");
        return 0;
    }

    web_stream_lock(stream);
    while (!atomic_load_explicit(&stream->stop, memory_order_acquire) &&
        stream->queued_bytes + length > WEB_STREAM_QUEUE_LIMIT)
        web_stream_wait(stream);
    if (atomic_load_explicit(&stream->stop, memory_order_acquire))
    {
        web_stream_unlock(stream);
        return 0;
    }

    WebStreamChunk* chunk = calloc(1, sizeof(*chunk));
    if (!chunk || !(chunk->data = malloc(length + 1)))
    {
        free(chunk);
        web_stream_set_error_locked(stream, "Out of memory");
        web_stream_unlock(stream);
        return 0;
    }
    if (length) memcpy(chunk->data, data, length);
    chunk->data[length] = 0;
    chunk->length = length;
    if (stream->chunk_tail) stream->chunk_tail->next = chunk;
    else stream->chunk_head = chunk;
    stream->chunk_tail = chunk;
    stream->queued_bytes += length;
    web_stream_unlock(stream);
    return length;
}

static const char* web_stream_curl_error(CURLcode code, const WebStreamState* stream)
{
    if (stream->error) return stream->error;
    switch (code)
    {
    case CURLE_COULDNT_RESOLVE_HOST: return "DNS lookup failed";
    case CURLE_COULDNT_CONNECT: return "Connection failed";
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM: return "TLS certificate verification failed";
    case CURLE_SSL_CONNECT_ERROR: return "TLS handshake failed";
    case CURLE_OPERATION_TIMEDOUT: return "HTTP response read failed";
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_URL_MALFORMAT: return "Unsupported URL";
    case CURLE_TOO_MANY_REDIRECTS: return "Too many redirects";
    default: return "HTTP request failed";
    }
}

static void web_stream_worker(WebStreamState* stream)
{
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        web_stream_set_error(stream, "HTTP request failed");
        goto done;
    }

    char error_buffer[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_URL, stream->url);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 10L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_stream_curl_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, stream);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, web_stream_curl_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, stream);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, stream->headers);
    curl_easy_setopt(curl, CURLOPT_PROXY, stream->proxy);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, stream->redirects > 0 ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, stream->redirects);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    const char* ca_file = getenv("CURL_CA_BUNDLE");
    if (!ca_file || !*ca_file) ca_file = getenv("SSL_CERT_FILE");
    if (ca_file && *ca_file) curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file);
    const char* ca_dir = getenv("SSL_CERT_DIR");
    if (ca_dir && *ca_dir) curl_easy_setopt(curl, CURLOPT_CAPATH, ca_dir);
#ifdef _WIN32
    if (!getenv("SSL_CERT_FILE")) curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
    if (stream->decompress) curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (stream->head) curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    else if (stream->body && stream->body[0])
    {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, stream->body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(stream->body));
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, stream->method);
    }
    else if (!web_header_is(stream->method, "GET"))
    {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, stream->method);
    }

    CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK && !atomic_load_explicit(&stream->stop, memory_order_acquire))
        web_stream_set_error(stream, web_stream_curl_error(code, stream));
    else
    {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status > 0) stream->status = (int)status;
    }
    curl_easy_cleanup(curl);

done:
    atomic_store_explicit(&stream->done, 1, memory_order_release);
    web_stream_release(stream);
}

#ifdef _WIN32
static DWORD WINAPI web_stream_thread(LPVOID data) { web_stream_worker(data); return 0; }
#else
static void* web_stream_thread(void* data) { web_stream_worker(data); return 0; }
#endif

static WebStreamState* web_stream_data(lt_VM* vm, lt_Value instance)
{
    return lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_stream_destroy_data));
}

static lt_Value web_stream_chunk_value(lt_VM* vm, WebStreamState* stream, WebStreamChunk* chunk)
{
    if (!stream->binary) return lt_make_string(vm, chunk->data);
    lt_Value array = lt_make_array(vm);
    lt_push(vm, array);
    for (size_t i = 0; i < chunk->length; ++i)
        lt_array_push(vm, array, lt_make_number((double)(unsigned char)chunk->data[i]));
    lt_pop(vm);
    return array;
}

static uint8_t web_stream_next(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Stream.next expects no arguments!");
    lt_Value instance = lt_pop(vm);
    WebStreamState* stream = web_stream_data(vm, instance);
    if (!stream) lt_runtime_error(vm, "Invalid web stream!");

    lt_Value promise = ltasync_make_promise(vm);
    lt_push(vm, promise);
    if (!LT_IS_NULL(stream->pending_next))
    {
        ltasync_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, "Stream.next is already pending!"));
        return 1;
    }

    web_stream_lock(stream);
    WebStreamChunk* chunk = stream->chunk_head;
    if (chunk)
    {
        stream->chunk_head = chunk->next;
        if (!stream->chunk_head) stream->chunk_tail = 0;
        stream->queued_bytes -= chunk->length;
        web_stream_wake(stream);
    }
    char* error = stream->error ? web_strdup(stream->error) : 0;
    uint8_t done = (uint8_t)atomic_load_explicit(&stream->done, memory_order_acquire);
    uint8_t stopped = (uint8_t)atomic_load_explicit(&stream->stop, memory_order_acquire);
    web_stream_unlock(stream);

    if (chunk)
    {
        lt_Value value = web_stream_chunk_value(vm, stream, chunk);
        ltasync_settle_promise(vm, promise, LT_PROMISE_FULFILLED, value);
        free(chunk->data);
        free(chunk);
    }
    else if (error)
    {
        ltasync_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, error));
        free(error);
    }
    else if (done || stopped)
    {
        ltasync_settle_promise(vm, promise, LT_PROMISE_FULFILLED, LT_VALUE_NULL);
    }
    else
    {
        stream->pending_next = promise;
        lt_native_root_persistent_internal(vm, promise);
    }
    return 1;
}

static uint8_t web_stream_close(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Stream.close expects no arguments!");
    lt_Value instance = lt_pop(vm);
    WebStreamState* stream = web_stream_data(vm, instance);
    if (!stream) return 0;

    atomic_store(&stream->stop, 1);
    web_stream_lock(stream);
    web_stream_clear_chunks(stream);
    web_stream_wake(stream);
    lt_Value pending = stream->pending_next;
    stream->pending_next = LT_VALUE_NULL;
    web_stream_unlock(stream);
    if (!LT_IS_NULL(pending))
    {
        ltasync_settle_promise(vm, pending, LT_PROMISE_FULFILLED, LT_VALUE_NULL);
        lt_native_unroot_persistent_internal(vm, pending);
    }
    if (stream->hook) lt_remove_poll_hook(vm, stream->hook);
    return 0;
}

static uint8_t web_stream_status(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebStreamState* stream = web_stream_data(vm, instance);
    lt_push(vm, lt_make_number(stream ? (double)stream->status : 0));
    return 1;
}

static uint8_t web_stream_status_text(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebStreamState* stream = web_stream_data(vm, instance);
    lt_push(vm, lt_make_string(vm, stream ? stream->status_text : ""));
    return 1;
}

static uint8_t web_stream_ok(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebStreamState* stream = web_stream_data(vm, instance);
    lt_push(vm, stream && stream->status >= 200 && stream->status < 300 ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t web_stream_url(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebStreamState* stream = web_stream_data(vm, instance);
    lt_push(vm, lt_make_string(vm, stream && stream->url ? stream->url : ""));
    return 1;
}

static lt_Value web_kept_headers(lt_VM* vm, lt_Value instance)
{
    lt_Buffer* bucket = &LT_GET_OBJECT(instance)->instance.private_fields.buckets[0];
    for (uint32_t i = 0; i < bucket->length; ++i)
    {
        lt_TablePair* pair = lt_buffer_at(bucket, i);
        if (LT_IS_NULL(pair->key)) return pair->value;
    }
    return lt_make_table(vm);
}

static uint8_t web_stream_headers(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    lt_push(vm, web_kept_headers(vm, instance));
    return 1;
}

static uint8_t web_stream_closed(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebStreamState* stream = web_stream_data(vm, instance);
    uint8_t closed = stream && (atomic_load(&stream->stop) || atomic_load(&stream->done));
    lt_push(vm, closed ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static void web_stream_hook_destroy(lt_VM* vm, void* context)
{
    WebStreamState* stream = context;
    atomic_store(&stream->stop, 1);
    web_stream_lock(stream);
    web_stream_wake(stream);
    web_stream_unlock(stream);
    if (!LT_IS_NULL(stream->promise))
    {
        lt_native_unroot_persistent_internal(vm, stream->promise);
        stream->promise = LT_VALUE_NULL;
    }
    if (!LT_IS_NULL(stream->pending_next))
    {
        lt_native_unroot_persistent_internal(vm, stream->pending_next);
        stream->pending_next = LT_VALUE_NULL;
    }
    web_stream_release(stream);
}

static lt_PollResult web_stream_poll(lt_VM* vm, void* context)
{
    WebStreamState* stream = context;
    uint8_t did_work = 0;
    uint8_t done = (uint8_t)atomic_load_explicit(&stream->done, memory_order_acquire);
    uint8_t stopped = (uint8_t)atomic_load_explicit(&stream->stop, memory_order_acquire);

    if (!stream->headers_sent)
    {
        if (atomic_load_explicit(&stream->headers_ready, memory_order_acquire))
        {
            lt_Value headers = web_headers_from_data(vm, stream->header_data);
            lt_push(vm, headers);
            uint16_t count = lt_exec(vm, web_class(vm, "Stream"), 0);
            lt_Value instance = count ? lt_pop(vm) : LT_VALUE_NULL;
            while (count > 1)
            {
                lt_pop(vm);
                --count;
            }
            if (!LT_IS_INSTANCE(instance))
            {
                lt_pop(vm);
                ltasync_settle_promise(vm, stream->promise, LT_PROMISE_REJECTED, lt_make_string(vm, "Failed to create HTTP stream!"));
                lt_native_unroot_persistent_internal(vm, stream->promise);
                stream->promise = LT_VALUE_NULL;
                if (stream->hook) lt_remove_poll_hook(vm, stream->hook);
                return LT_POLL_WORK;
            }
            lt_push(vm, instance);
            web_stream_retain(stream);
            lt_instance_set_native_data(vm, instance, web_class(vm, "Stream"), stream);
            web_keep_headers(vm, instance, headers);
            ltasync_settle_promise(vm, stream->promise, LT_PROMISE_FULFILLED, instance);
            lt_native_unroot_persistent_internal(vm, stream->promise);
            stream->promise = LT_VALUE_NULL;
            stream->headers_sent = 1;
            lt_pop(vm);
            lt_pop(vm);
            did_work = 1;
        }
        else if (done || stopped)
        {
            const char* message = stream->error ? stream->error : "HTTP stream ended before headers!";
            ltasync_settle_promise(vm, stream->promise, LT_PROMISE_REJECTED, lt_make_string(vm, message));
            lt_native_unroot_persistent_internal(vm, stream->promise);
            stream->promise = LT_VALUE_NULL;
            if (stream->hook) lt_remove_poll_hook(vm, stream->hook);
            return LT_POLL_WORK;
        }
        else return LT_POLL_PENDING;
    }

    lt_Value pending = stream->pending_next;
    if (!LT_IS_NULL(pending))
    {
        web_stream_lock(stream);
        WebStreamChunk* chunk = stream->chunk_head;
        if (chunk)
        {
            stream->chunk_head = chunk->next;
            if (!stream->chunk_head) stream->chunk_tail = 0;
            stream->queued_bytes -= chunk->length;
            web_stream_wake(stream);
        }
        char* error = stream->error ? web_strdup(stream->error) : 0;
        done = (uint8_t)atomic_load_explicit(&stream->done, memory_order_acquire);
        stopped = (uint8_t)atomic_load_explicit(&stream->stop, memory_order_acquire);
        web_stream_unlock(stream);

        stream->pending_next = LT_VALUE_NULL;
        if (chunk)
        {
            lt_Value value = web_stream_chunk_value(vm, stream, chunk);
            ltasync_settle_promise(vm, pending, LT_PROMISE_FULFILLED, value);
            free(chunk->data);
            free(chunk);
            did_work = 1;
        }
        else if (error)
        {
            ltasync_settle_promise(vm, pending, LT_PROMISE_REJECTED, lt_make_string(vm, error));
            free(error);
            did_work = 1;
        }
        else if (done || stopped)
        {
            ltasync_settle_promise(vm, pending, LT_PROMISE_FULFILLED, LT_VALUE_NULL);
            did_work = 1;
        }
        else
        {
            stream->pending_next = pending;
        }
        if (LT_IS_NULL(stream->pending_next)) lt_native_unroot_persistent_internal(vm, pending);
    }

    done = (uint8_t)atomic_load_explicit(&stream->done, memory_order_acquire);
    stopped = (uint8_t)atomic_load_explicit(&stream->stop, memory_order_acquire);
    if ((done || stopped) && LT_IS_NULL(stream->pending_next) && stream->hook)
    {
        lt_remove_poll_hook(vm, stream->hook);
        did_work = 1;
    }
    return did_work ? LT_POLL_WORK : LT_POLL_PENDING;
}

static uint8_t web_fetch_stream(lt_VM* vm, uint8_t argc)
{
    if (argc < 1 || argc > 2) { lt_runtime_error(vm, "web.fetchStream expects url and optional init!"); return 0; }
    lt_Value init = argc == 2 ? lt_pop(vm) : LT_VALUE_NULL;
    lt_Value url_value = lt_pop(vm);
    if (!LT_IS_STRING(url_value)) { lt_runtime_error(vm, "web.fetchStream url must be a string!"); return 0; }
    if (!LT_IS_NULL(init) && !LT_IS_TABLE(init)) { lt_runtime_error(vm, "web.fetchStream init must be a table!"); return 0; }
    lt_native_root_internal(vm, url_value);
    lt_native_root_internal(vm, init);

    const char* method = "GET";
    const char* body = "";
    lt_Value headers = LT_VALUE_NULL;
    long redirects = 0;
    const char* proxy = "";
    int decompress = 0;
    int binary = 0;
    if (LT_IS_TABLE(init))
    {
        lt_Value value = lt_table_get(vm, init, lt_make_string(vm, "method"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_STRING(value)) { lt_runtime_error(vm, "web.fetchStream method must be a string!"); return 0; }
            method = lt_get_string(vm, value);
        }
        value = lt_table_get(vm, init, lt_make_string(vm, "body"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_STRING(value)) { lt_runtime_error(vm, "web.fetchStream body must be a string!"); return 0; }
            body = lt_get_string(vm, value);
        }
        value = lt_table_get(vm, init, lt_make_string(vm, "headers"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_TABLE(value)) { lt_runtime_error(vm, "web.fetchStream headers must be a table!"); return 0; }
            headers = value;
        }
        value = lt_table_get(vm, init, lt_make_string(vm, "redirects"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_NUMBER(value)) { lt_runtime_error(vm, "web.fetchStream redirects must be an integer from 0 to 20!"); return 0; }
            double n = lt_get_number(value);
            if (n < 0 || n > 20 || n != (double)(int)n) { lt_runtime_error(vm, "web.fetchStream redirects must be an integer from 0 to 20!"); return 0; }
            redirects = (long)n;
        }
        value = lt_table_get(vm, init, lt_make_string(vm, "proxy"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_STRING(value)) { lt_runtime_error(vm, "web.fetchStream proxy must be a string!"); return 0; }
            proxy = lt_get_string(vm, value);
        }
        value = lt_table_get(vm, init, lt_make_string(vm, "decompress"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_TRUE(value) && !LT_IS_FALSE(value)) { lt_runtime_error(vm, "web.fetchStream decompress must be a boolean!"); return 0; }
            decompress = LT_IS_TRUE(value);
        }
        value = lt_table_get(vm, init, lt_make_string(vm, "binary"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_TRUE(value) && !LT_IS_FALSE(value)) { lt_runtime_error(vm, "web.fetchStream binary must be a boolean!"); return 0; }
            binary = LT_IS_TRUE(value);
        }
    }

    const char* url = lt_get_string(vm, url_value);
    WebStreamState* stream = calloc(1, sizeof(*stream));
    if (!stream) { lt_runtime_error(vm, "Out of memory creating HTTP stream!"); return 0; }
    atomic_init(&stream->refs, 2);
    atomic_init(&stream->done, 0);
    atomic_init(&stream->stop, 0);
    atomic_init(&stream->headers_ready, 0);
    stream->vm = vm;
    stream->promise = LT_VALUE_NULL;
    stream->pending_next = LT_VALUE_NULL;
    stream->url = web_strdup(url);
    stream->method = web_strdup(method);
    stream->body = web_strdup(body);
    stream->proxy = web_strdup(proxy);
    stream->redirects = redirects;
    stream->decompress = decompress;
    stream->head = web_header_is(method, "HEAD");
    stream->binary = binary;
#if defined(_WIN32)
    InitializeCriticalSection(&stream->lock);
    InitializeConditionVariable(&stream->wake);
#else
    pthread_mutex_init(&stream->lock, 0);
    pthread_cond_init(&stream->wake, 0);
#endif
    if (!stream->url || !stream->method || !stream->body || !stream->proxy)
    {
        web_stream_release(stream);
        lt_runtime_error(vm, "Out of memory creating HTTP stream!");
        return 0;
    }

    if (LT_IS_TABLE(headers))
    {
        uint64_t cursor = 0;
        lt_Value key = LT_VALUE_NULL;
        lt_Value value = LT_VALUE_NULL;
        while (lt_table_next(vm, headers, &cursor, &key, &value))
        {
            if (!LT_IS_STRING(key) || !LT_IS_STRING(value)) { web_stream_release(stream); lt_runtime_error(vm, "web.fetchStream headers must contain string names and values!"); return 0; }
            const char* name = lt_get_string(vm, key);
            const char* text = lt_get_string(vm, value);
            if (!web_header_valid(name, text)) { web_stream_release(stream); lt_runtime_error(vm, "Invalid web.fetchStream header!"); return 0; }
            if (web_header_is(name, "Host") || web_header_is(name, "Connection") ||
                web_header_is(name, "Content-Length") || web_header_is(name, "Transfer-Encoding")) continue;
            size_t length = strlen(name) + strlen(text) + 3;
            char* line = malloc(length);
            if (!line) { web_stream_release(stream); lt_runtime_error(vm, "Out of memory creating HTTP stream!"); return 0; }
            snprintf(line, length, "%s: %s", name, text);
            struct curl_slist* grown = curl_slist_append(stream->headers, line);
            free(line);
            if (!grown) { web_stream_release(stream); lt_runtime_error(vm, "Out of memory creating HTTP stream!"); return 0; }
            stream->headers = grown;
        }
    }

    stream->promise = ltasync_make_promise(vm);
    lt_native_root_persistent_internal(vm, stream->promise);
    stream->hook = ltasync_add_owned_poll_hook(vm, web_stream_poll, stream, web_stream_hook_destroy);
    if (!stream->hook)
    {
        web_stream_release(stream);
        lt_runtime_error(vm, "Failed to register HTTP stream poll hook!");
        return 0;
    }
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, web_stream_thread, stream, 0, NULL);
    if (thread) CloseHandle(thread);
    else {
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, web_stream_thread, stream) == 0) pthread_detach(thread);
    else {
#endif
        web_stream_set_error(stream, "Failed to start HTTP stream worker");
        atomic_store(&stream->done, 1);
        web_stream_release(stream);
    }
    lt_push(vm, stream->promise);
    return 1;
}


/* ---------- serve ---------- */

static int web_http_message_begin(llhttp_t* parser)
{
    WebClientData* client = parser->data;
    client->method_len = 0;
    client->path_len = 0;
    client->header_name_len = 0;
    client->header_value_len = 0;
    client->body_len = 0;
    client->chunked_request = 0;
    client->message_complete = 0;
    client->method[0] = 0;
    client->path[0] = 0;
    client->body[0] = 0;
    return 0;
}

static int web_http_method(llhttp_t* parser, const char* at, size_t length)
{
    WebClientData* client = parser->data;
    if (client->method_len + length >= sizeof(client->method)) return HPE_USER;
    memcpy(client->method + client->method_len, at, length);
    client->method_len += length;
    client->method[client->method_len] = 0;
    return 0;
}

static int web_http_url(llhttp_t* parser, const char* at, size_t length)
{
    WebClientData* client = parser->data;
    if (client->path_len + length >= sizeof(client->path)) return HPE_USER;
    memcpy(client->path + client->path_len, at, length);
    client->path_len += length;
    client->path[client->path_len] = 0;
    return 0;
}

static int web_http_header_field(llhttp_t* parser, const char* at, size_t length)
{
    WebClientData* client = parser->data;
    if (client->header_name_len + length >= sizeof(client->header_name)) return HPE_USER;
    memcpy(client->header_name + client->header_name_len, at, length);
    client->header_name_len += length;
    client->header_name[client->header_name_len] = 0;
    return 0;
}

static int web_http_header_value(llhttp_t* parser, const char* at, size_t length)
{
    WebClientData* client = parser->data;
    if (client->header_value_len + length >= sizeof(client->header_value)) return HPE_USER;
    memcpy(client->header_value + client->header_value_len, at, length);
    client->header_value_len += length;
    client->header_value[client->header_value_len] = 0;
    return 0;
}

static int web_http_header_value_complete(llhttp_t* parser)
{
    WebClientData* client = parser->data;
    if (!web_header_valid(client->header_name, client->header_value)) return HPE_USER;
    if (web_header_is(client->header_name, "Transfer-Encoding")) client->chunked_request = 1;
    lt_table_set(client->vm, client->headers,
        lt_make_string(client->vm, client->header_name),
        lt_make_string(client->vm, client->header_value));
    client->header_name_len = 0;
    client->header_value_len = 0;
    client->header_name[0] = 0;
    client->header_value[0] = 0;
    return 0;
}

static int web_http_headers_complete(llhttp_t* parser)
{
    WebClientData* client = parser->data;
    if (client->chunked_request || (parser->flags & F_CHUNKED)) return HPE_USER;
    if (parser->content_length > sizeof(client->body) - 1) return HPE_USER;
    client->head_request = web_header_is(client->method, "HEAD");
    return 0;
}

static int web_http_body(llhttp_t* parser, const char* at, size_t length)
{
    WebClientData* client = parser->data;
    if (client->body_len + length >= sizeof(client->body)) return HPE_USER;
    memcpy(client->body + client->body_len, at, length);
    client->body_len += length;
    client->body[client->body_len] = 0;
    return 0;
}

static int web_http_message_complete(llhttp_t* parser)
{
    WebClientData* client = parser->data;
    client->message_complete = 1;
    return HPE_PAUSED;
}

static llhttp_settings_t web_http_settings;

static void web_http_settings_init(void)
{
    llhttp_settings_init(&web_http_settings);
    web_http_settings.on_message_begin = web_http_message_begin;
    web_http_settings.on_method = web_http_method;
    web_http_settings.on_url = web_http_url;
    web_http_settings.on_header_field = web_http_header_field;
    web_http_settings.on_header_value = web_http_header_value;
    web_http_settings.on_header_value_complete = web_http_header_value_complete;
    web_http_settings.on_headers_complete = web_http_headers_complete;
    web_http_settings.on_body = web_http_body;
    web_http_settings.on_message_complete = web_http_message_complete;
}

static void web_server_remove_client(lt_VM* vm, WebServerData* sd, size_t index)
{
    WebClientData* client = sd->clients[index];
    if (!client) return;
    web_close_socket(client->socket);
    free(client->output);
    if (!LT_IS_NULL(client->headers)) lt_native_unroot_persistent_internal(vm, client->headers);
    if (!LT_IS_NULL(client->pending)) lt_native_unroot_persistent_internal(vm, client->pending);
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
    lt_native_unroot_persistent_internal(vm, hook->handler);
    lt_native_unroot_persistent_internal(vm, hook->instance);
    free(hook);
}

static int web_server_output(lt_VM* vm, WebClientData* sd, lt_Value result, int failed)
{
    int status = failed ? 500 : 200;
    const char* status_text = failed ? "Internal Server Error" : "OK";
    const char* body = failed ? "Internal Server Error" : "";
    lt_Value headers = LT_VALUE_NULL;
    lt_native_root_internal(vm, result);
    if (!failed && LT_IS_STRING(result)) body = lt_get_string(vm, result);
    else if (!failed && LT_IS_INSTANCE(result)) {
        lt_Object* klass = LT_GET_OBJECT(result)->instance.klass;
        while (klass && klass->class_def.native_data_destroy != web_destroy_response) klass = klass->class_def.superclass;
        if (klass) {
            WebResponseData* rd = lt_instance_get_native_data(vm, result, LT_VALUE_OBJECT(klass));
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
    while (LT_IS_TABLE(headers) && lt_table_next(vm, headers, &cursor, &key, &value)) {
        if (!LT_IS_STRING(key) || !LT_IS_STRING(value)) return 0;
        const char* name = lt_get_string(vm, key);
        const char* text = lt_get_string(vm, value);
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

static int web_server_handle(lt_VM* vm, WebServerHook* hook, WebClientData* sd)
{
    lt_Value request = create_request_instance(vm, sd->path, sd->method, sd->body, sd->headers);
    lt_push(vm, request);
    lt_Value result = LT_VALUE_NULL;
    hook->server->dispatching = 1;
    if (ltasync_is_async_callable(hook->handler)) result = ltasync_call(vm, hook->handler, 1);
    else
    {
        uint16_t count = lt_exec(vm, hook->handler, 1);
        if (count) { result = lt_pop(vm); while (--count) lt_pop(vm); }
    }
    hook->server->dispatching = 0;
    if (hook->server->closed) { lt_remove_poll_hook(vm, hook->server->hook); return 1; }
    if (lt_native_is_promise_internal(result))
    {
        sd->pending = result;
        lt_native_root_persistent_internal(vm, result);
        return 1;
    }
    return web_server_output(vm, sd, result, 0);
}

static lt_PollResult web_server_poll_client(lt_VM* vm, WebServerHook* hook, size_t index)
{
    WebServerData* server = hook->server;
    WebClientData* sd = server->clients[index];
    if (!LT_IS_NULL(sd->pending))
    {
        lt_PromiseState state = lt_native_promise_state_internal(sd->pending);
        if (state == LT_PROMISE_PENDING) return LT_POLL_PENDING;
        lt_Value result = lt_native_promise_result_internal(sd->pending);
        lt_native_root_internal(vm, result);
        lt_native_unroot_persistent_internal(vm, sd->pending);
        sd->pending = LT_VALUE_NULL;
        if (!web_server_output(vm, sd, result, state == LT_PROMISE_REJECTED)) web_server_remove_client(vm, server, index);
        return LT_POLL_WORK;
    }
    if (sd->output)
    {
        size_t remaining = sd->output_len - sd->output_sent;
        int count = send(sd->socket, sd->output + sd->output_sent, remaining > INT_MAX ? INT_MAX : (int)remaining,
#ifdef MSG_NOSIGNAL
            MSG_NOSIGNAL
#else
            0
#endif
        );
        if (count > 0)
        {
            sd->output_sent += (size_t)count;
            if (sd->output_sent == sd->output_len) web_server_remove_client(vm, server, index);
            return LT_POLL_WORK;
        }
        if (count < 0 && web_would_block()) return LT_POLL_PENDING;
        web_server_remove_client(vm, server, index);
        return LT_POLL_WORK;
    }
    if (sd->message_complete)
    {
        if (!web_server_handle(vm, hook, sd)) web_server_remove_client(vm, server, index);
        return LT_POLL_WORK;
    }
    if (sd->input_len == sizeof(sd->input) - 1)
    {
        web_server_remove_client(vm, server, index);
        return LT_POLL_WORK;
    }
    int count = recv(sd->socket, sd->input + sd->input_len, (int)(sizeof(sd->input) - 1 - sd->input_len), 0);
    if (count < 0 && web_would_block()) return LT_POLL_PENDING;
    if (count <= 0 || memchr(sd->input + sd->input_len, 0, (size_t)(count > 0 ? count : 0)))
    {
        web_server_remove_client(vm, server, index);
        return LT_POLL_WORK;
    }
    size_t previous = sd->input_len;
    sd->input_len += (size_t)count;
    sd->input[sd->input_len] = 0;
    llhttp_errno_t error = llhttp_execute(&sd->parser, sd->input + previous, (size_t)count);
    if (error != HPE_OK && error != HPE_PAUSED)
    {
        web_server_remove_client(vm, server, index);
        return LT_POLL_WORK;
    }
    if (sd->message_complete && !web_server_handle(vm, hook, sd))
    {
        web_server_remove_client(vm, server, index);
    }
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
            client->vm = vm;
            client->headers = lt_make_table(vm);
            lt_native_root_persistent_internal(vm, client->headers);
            llhttp_init(&client->parser, HTTP_REQUEST, &web_http_settings);
            client->parser.data = client;
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
    if (argc < 1 || argc > 2) { lt_runtime_error(vm, "web.serve expects handler and optional options!"); return 0; }
    lt_Value options = argc == 2 ? lt_pop(vm) : LT_VALUE_NULL;
    lt_Value handler = lt_pop(vm);
    if (!ltasync_is_callable(handler)) { lt_runtime_error(vm, "web.serve handler must be callable!"); return 0; }
    lt_native_root_internal(vm, handler);
    lt_native_root_internal(vm, options);
    int port = 8000; const char* hostname = "0.0.0.0";
    if (LT_IS_TABLE(options)) {
        lt_Value value = lt_table_get(vm, options, lt_make_string(vm, "port"));
        if (LT_IS_NUMBER(value)) {
            double n = lt_get_number(value);
            if (!(n >= 0 && n <= 65535) || (double)(int)n != n) { lt_runtime_error(vm, "Invalid server port!"); return 0; }
            port = (int)n;
        }
        value = lt_table_get(vm, options, lt_make_string(vm, "hostname"));
        if (LT_IS_STRING(value)) hostname = lt_get_string(vm, value);
    }
    struct sockaddr_in address; memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET; address.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, hostname, &address.sin_addr) != 1) { lt_runtime_error(vm, "Server hostname must be an IPv4 address!"); return 0; }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { lt_runtime_error(vm, "Socket initialization failed!"); return 0; }
#endif
    int bound_port;
    const char* socket_error = NULL;
    WebSocket socket_fd = lt_net_listen_ipv4(&address, &bound_port, &socket_error);
    if (socket_fd == WEB_BAD_SOCKET) {
#ifdef _WIN32
        WSACleanup();
#endif
        lt_runtime_error(vm, socket_error); return 0;
    }
    WebServerData* sd = calloc(1, sizeof(*sd));
    WebServerHook* hook = calloc(1, sizeof(*hook));
    if (!sd || !hook) {
        free(sd); free(hook); web_close_socket(socket_fd);
#ifdef _WIN32
        WSACleanup();
#endif
        lt_runtime_error(vm, "Out of memory starting server!"); return 0;
    }
    sd->socket = socket_fd; sd->port = bound_port;
    uint16_t count = lt_exec(vm, web_class(vm, "Server"), 0);
    lt_Value instance = count ? lt_pop(vm) : LT_VALUE_NULL;
    while (count > 1) { lt_pop(vm); --count; }
    if (!LT_IS_INSTANCE(instance)) {
        free(sd); free(hook); web_close_socket(socket_fd);
#ifdef _WIN32
        WSACleanup();
#endif
        lt_runtime_error(vm, "Failed to create server instance!"); return 0;
    }
    lt_native_root_internal(vm, instance);
    lt_instance_set_native_data(vm, instance, web_class(vm, "Server"), sd);
    hook->server = sd; hook->instance = instance; hook->handler = handler;
    lt_native_root_persistent_internal(vm, instance);
    lt_native_root_persistent_internal(vm, handler);
    sd->hook = ltasync_add_owned_poll_hook(vm, web_server_poll, hook, web_server_hook_destroy);
    lt_push(vm, instance);
    return 1;
}

static uint8_t server_close(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) { lt_runtime_error(vm, "Server.close expects no arguments!"); return 0; }
    lt_Value instance = lt_pop(vm);
    WebServerData* sd = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_server));
    if (sd && !sd->closed) {
        sd->closed = 1;
        web_close_socket(sd->socket); sd->socket = WEB_BAD_SOCKET;
        for (size_t i = 0; i < WEB_SERVER_CLIENT_LIMIT; ++i) {
            if (sd->clients[i]) {
                web_close_socket(sd->clients[i]->socket);
                sd->clients[i]->socket = WEB_BAD_SOCKET;
            }
        }
        if (!sd->dispatching) lt_remove_poll_hook(vm, sd->hook);
    }
    return 0;
}

static uint8_t server_port(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = lt_pop(vm);
    WebServerData* sd = lt_instance_get_native_data(vm, instance, web_instance_class(vm, instance, web_destroy_server));
    lt_push(vm, lt_make_number(sd ? sd->port : 0));
    return 1;
}
void ltstd_open_web(lt_VM* vm)
{
    static int curl_initialized = 0;
    if (!curl_initialized)
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        {
            lt_runtime_error(vm, "Failed to initialize libcurl!");
            return;
        }
        curl_initialized = 1;
    }
    web_http_settings_init();

    lt_Value response_class, request_class, server_class, stream_class;

    response_class = lt_class_create(vm, "Response");
    lt_class_set_native_data_destroy(vm, response_class, web_destroy_response);
    lt_class_set_constructor(vm, response_class, response_constructor);
    lt_class_add_getter(vm, response_class, "status", response_status, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, response_class, "statusText", response_status_text, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, response_class, "ok", response_ok, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, response_class, "url", response_url, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, response_class, "headers", response_headers, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, response_class, "body", response_body, LT_VIS_PUBLIC, 0);
    lt_class_add_method(vm, response_class, "text", response_text, LT_VIS_PUBLIC, 0);
    lt_class_add_method(vm, response_class, "json", response_json, LT_VIS_PUBLIC, 0);

    request_class = lt_class_create(vm, "Request");
    lt_class_set_native_data_destroy(vm, request_class, web_destroy_request);
    lt_class_add_getter(vm, request_class, "url", request_url, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, request_class, "method", request_method, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, request_class, "body", request_body, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, request_class, "headers", request_headers, LT_VIS_PUBLIC, 0);

    server_class = lt_class_create(vm, "Server");
    lt_class_set_native_data_destroy(vm, server_class, web_destroy_server);
    lt_class_add_method(vm, server_class, "close", server_close, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, server_class, "port", server_port, LT_VIS_PUBLIC, 0);

    stream_class = lt_class_create(vm, "Stream");
    lt_class_set_native_data_destroy(vm, stream_class, web_stream_destroy_data);
    lt_class_add_method(vm, stream_class, "next", web_stream_next, LT_VIS_PUBLIC, 0);
    lt_class_add_method(vm, stream_class, "close", web_stream_close, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, stream_class, "status", web_stream_status, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, stream_class, "statusText", web_stream_status_text, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, stream_class, "ok", web_stream_ok, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, stream_class, "url", web_stream_url, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, stream_class, "headers", web_stream_headers, LT_VIS_PUBLIC, 0);
    lt_class_add_getter(vm, stream_class, "closed", web_stream_closed, LT_VIS_PUBLIC, 0);

    lt_Value registry = lt_make_table(vm);
    lt_table_set(vm, registry, lt_make_string(vm, "Response"), response_class);
    lt_table_set(vm, registry, lt_make_string(vm, "Request"), request_class);
    lt_table_set(vm, registry, lt_make_string(vm, "Server"), server_class);
    lt_table_set(vm, registry, lt_make_string(vm, "Stream"), stream_class);
    lt_table_set(vm, vm->global, lt_make_string(vm, "__little_web_classes"), registry);
    lt_Value web = lt_make_table(vm);
    /** [LittleBinding] web.fetch
     *  requests a URL and returns a promise resolving to a web.Response.
     *  @signature (url [, init])
     *  @param url absolute http or https URL to request
     *  @param init table with optional method, body, headers, redirects, proxy, and decompress fields
     */
    lt_table_set(vm, web, lt_make_string(vm, "fetch"), lt_make_native(vm, web_fetch));
    /** [LittleBinding] web.fetchStream
     *  requests a URL and returns a promise resolving to a web.Stream once response headers are available.
     *  @signature (url [, init])
     *  @param url absolute http or https URL to request
     *  @param init table with optional method, body, headers, redirects, proxy, decompress, and binary fields
     */
    lt_table_set(vm, web, lt_make_string(vm, "fetchStream"), lt_make_native(vm, web_fetch_stream));
    /** [LittleBinding] web.serve
     *  starts an HTTP server that passes each request to the handler and returns a web.Server.
     *  @signature (handler [, options])
     *  @param handler callable receiving a web.Request and returning a web.Response or a promise of one
     *  @param options table with optional port and hostname fields
     */
    lt_table_set(vm, web, lt_make_string(vm, "serve"), lt_make_native(vm, web_serve));
    /** [LittleBinding] web.Request
     *  represents an incoming server request exposing url, method, body, and headers.
     *  @signature ()
     */
    lt_table_set(vm, web, lt_make_string(vm, "Request"), request_class);
    /** [LittleBinding] web.Response
     *  represents an HTTP response with a status, headers, and body.
     *  @signature ([body [, options]])
     *  @param body response body string
     *  @param options table with optional status, statusText, and headers fields
     */
    lt_table_set(vm, web, lt_make_string(vm, "Response"), response_class);
    /** [LittleBinding] web.Server
     *  represents a running HTTP server exposing close() and the bound port.
     *  @signature ()
     */
    lt_table_set(vm, web, lt_make_string(vm, "Server"), server_class);
    /** [LittleBinding] web.Stream
     *  represents a streaming HTTP client response exposing next(), close(), status, statusText, ok, url, headers, and closed.
     *  @signature ()
     */
    lt_table_set(vm, web, lt_make_string(vm, "Stream"), stream_class);

    lt_table_set(vm, vm->global, lt_make_string(vm, "web"), web);
}
