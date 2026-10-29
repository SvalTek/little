#define _GNU_SOURCE
#include "little_std.h"
#include "little_async.h"
#include "little_internal.h"

#include <wslay/wslay.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>

#include <ctype.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#else
#include <sys/socket.h>
#include <sys/select.h>
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

#define WS_CLIENT_LIMIT 8
#define WS_MAX_MESSAGE 4194304
#define WS_MAX_QUEUE 4194304

#ifdef _WIN32
typedef SOCKET ws_Socket;
#define WS_BAD_SOCKET INVALID_SOCKET
#define WS_SHUTDOWN_BOTH SD_BOTH
#else
typedef int ws_Socket;
#define WS_BAD_SOCKET (-1)
#define WS_SHUTDOWN_BOTH SHUT_RDWR
#endif

typedef struct WsServer WsServer;

typedef struct WsMsg {
    struct WsMsg* next;
    int binary;
    size_t length;
    char* data;
} WsMsg;

typedef struct WsConn {
    lt_VM* owner;
    atomic_int refs;
    atomic_uintptr_t socket;
    atomic_int stop;
    atomic_int worker_running;
    atomic_int handshake_ready;
    atomic_int handshake_ok;
    SSL* ssl;
    SSL_CTX* tls_ctx;
    wslay_event_context_ptr wslay;
    uint32_t hook;
    WsServer* server;
    int is_server;
    int secure;
    int open;
    int closed;
    int peer_closed;
    int local_close;
    atomic_int io_closed;
    int close_code;
    char* close_reason;
    char* error;
    unsigned char* prebuffer;
    size_t prebuffer_len;
    size_t prebuffer_pos;
    char* host;
    int port;
    char* path;
    char* url;
    char* request;
    char* expected_accept;
    char* method;
    char* request_path;
    char* headers_blob;
    char* body;
    lt_Value instance;
    lt_Value connect_promise;
    lt_Value pending_next;
    WsMsg* msg_head;
    WsMsg* msg_tail;
    size_t queued_bytes;
} WsConn;

struct WsServer {
    lt_VM* owner;
    atomic_int refs;
    ws_Socket listener;
    SSL_CTX* tls_ctx;
    uint32_t hook;
    int closed;
    lt_Value instance;
    lt_Value handler;
    WsConn* clients[WS_CLIENT_LIMIT];
};

#define ws_api (ltstd_native_api())

static lt_Value ws_class(lt_VM* vm, const char* name)
{
    lt_Value registry = ws_api->table_get(vm, vm->global, ws_api->make_string(vm, "__little_ws_classes"));
    lt_Value klass = ws_api->table_get(vm, registry, ws_api->make_string(vm, name));
    if (!LT_IS_CLASS(klass)) ws_api->runtime_error(vm, "WebSocket class registry is unavailable!");
    return klass;
}

static char* ws_strdup(const char* text)
{
    if (!text) return 0;
    size_t length = strlen(text);
    char* copy = malloc(length + 1);
    if (copy) memcpy(copy, text, length + 1);
    return copy;
}

static void ws_close_socket(ws_Socket socket_fd)
{
    if (socket_fd == WS_BAD_SOCKET) return;
#ifdef _WIN32
    closesocket(socket_fd);
#else
    close(socket_fd);
#endif
}

static void ws_shutdown_socket(ws_Socket socket_fd)
{
    if (socket_fd == WS_BAD_SOCKET) return;
    shutdown(socket_fd, WS_SHUTDOWN_BOTH);
}

static void ws_no_sigpipe(ws_Socket socket_fd)
{
#if defined(SO_NOSIGPIPE)
    int enabled = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
    (void)socket_fd;
#endif
}

static int ws_nonblocking(ws_Socket socket_fd)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(socket_fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(socket_fd, F_GETFL, 0);
    return flags >= 0 && fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static int ws_would_block(void)
{
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
}

static int ws_io_read(WsConn* conn, unsigned char* buffer, size_t length)
{
    ws_Socket socket_fd = (ws_Socket)atomic_load(&conn->socket);
    if (socket_fd == WS_BAD_SOCKET) return 0;
    if (conn->ssl)
    {
        int count = SSL_read(conn->ssl, buffer, length > INT_MAX ? INT_MAX : (int)length);
        if (count > 0) return count;
        int error = SSL_get_error(conn->ssl, count);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) return -2;
        if (error == SSL_ERROR_ZERO_RETURN) return 0;
        return -1;
    }
#ifdef _WIN32
    int count = recv(socket_fd, (char*)buffer, length > INT_MAX ? INT_MAX : (int)length, 0);
#else
    ssize_t count = recv(socket_fd, buffer, length, 0);
#endif
    if (count > 0) return (int)count;
    if (count < 0 && ws_would_block()) return -2;
    return count == 0 ? 0 : -1;
}

static int ws_io_write(WsConn* conn, const unsigned char* buffer, size_t length)
{
    ws_Socket socket_fd = (ws_Socket)atomic_load(&conn->socket);
    if (socket_fd == WS_BAD_SOCKET) return -1;
    if (conn->ssl)
    {
        int count = SSL_write(conn->ssl, buffer, length > INT_MAX ? INT_MAX : (int)length);
        if (count > 0) return count;
        int error = SSL_get_error(conn->ssl, count);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) return -2;
        return -1;
    }
#ifdef _WIN32
    int count = send(socket_fd, (const char*)buffer, length > INT_MAX ? INT_MAX : (int)length, 0);
#else
    ssize_t count = send(socket_fd, buffer, length, MSG_NOSIGNAL);
#endif
    if (count > 0) return (int)count;
    if (count < 0 && ws_would_block()) return -2;
    return -1;
}

static int ws_write_all(WsConn* conn, const char* data, size_t length)
{
    while (length)
    {
        int count = ws_io_write(conn, (const unsigned char*)data, length);
        if (count == -2) continue;
        if (count <= 0) return 0;
        data += count;
        length -= (size_t)count;
    }
    return 1;
}

static int ws_set_timeout(ws_Socket socket_fd, int seconds)
{
#ifdef _WIN32
    DWORD timeout = (DWORD)seconds * 1000;
    return setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == 0 &&
           setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout)) == 0;
#else
    struct timeval timeout = { seconds, 0 };
    return setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
           setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
#endif
}

static int ws_parse_url(const char* url, char* host, size_t host_size, int* port, int* secure, char* path, size_t path_size)
{
    const char* cursor;
    if (strncmp(url, "ws://", 5) == 0) { *secure = 0; cursor = url + 5; }
    else if (strncmp(url, "wss://", 6) == 0) { *secure = 1; cursor = url + 6; }
    else return 0;
    const char* start = cursor;
    if (*cursor == '[')
    {
        ++cursor;
        const char* end = strchr(cursor, ']');
        if (!end || (size_t)(end - cursor) >= host_size) return 0;
        memcpy(host, cursor, (size_t)(end - cursor));
        host[end - cursor] = 0;
        cursor = end + 1;
    }
    else
    {
        while (*cursor && *cursor != ':' && *cursor != '/' && *cursor != '?' && *cursor != '#') ++cursor;
        size_t length = (size_t)(cursor - start);
        if (!length || length >= host_size) return 0;
        memcpy(host, start, length);
        host[length] = 0;
    }
    int default_port = *secure ? 443 : 80;
    *port = default_port;
    if (*cursor == ':')
    {
        ++cursor;
        int value = 0;
        if (!isdigit((unsigned char)*cursor)) return 0;
        while (isdigit((unsigned char)*cursor))
        {
            value = value * 10 + (*cursor - '0');
            if (value > 65535) return 0;
            ++cursor;
        }
        *port = value;
    }
    if (*cursor == 0) { path[0] = '/'; path[1] = 0; return 1; }
    if (*cursor != '/') return 0;
    size_t length = strlen(cursor);
    if (length >= path_size) return 0;
    memcpy(path, cursor, length + 1);
    return 1;
}

static int ws_connect_socket(const char* host, int port, ws_Socket* out, const char** error)
{
    struct addrinfo hints;
    struct addrinfo* addresses = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", port);
    if (getaddrinfo(host, port_text, &hints, &addresses) != 0) { *error = "DNS lookup failed"; return 0; }
    ws_Socket socket_fd = WS_BAD_SOCKET;
    for (struct addrinfo* address = addresses; address; address = address->ai_next)
    {
        socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd == WS_BAD_SOCKET) continue;
        ws_no_sigpipe(socket_fd);
        ws_set_timeout(socket_fd, 10);
        if (connect(socket_fd, address->ai_addr, (int)address->ai_addrlen) == 0) break;
        ws_close_socket(socket_fd);
        socket_fd = WS_BAD_SOCKET;
    }
    freeaddrinfo(addresses);
    if (socket_fd == WS_BAD_SOCKET) { *error = "Connection failed"; return 0; }
    *out = socket_fd;
    return 1;
}

static ws_Socket ws_create_listener(const char* hostname, int port, int* bound_port, const char** error)
{
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, hostname, &address.sin_addr) != 1) { *error = "Server hostname must be an IPv4 address!"; return WS_BAD_SOCKET; }
    ws_Socket socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == WS_BAD_SOCKET) { *error = "Failed to create server socket!"; return WS_BAD_SOCKET; }
    int reuse = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    if (bind(socket_fd, (struct sockaddr*)&address, sizeof(address)) != 0 ||
        listen(socket_fd, 16) != 0 || !ws_nonblocking(socket_fd))
    {
        ws_close_socket(socket_fd);
        *error = "Failed to start server socket!";
        return WS_BAD_SOCKET;
    }
    socklen_t length = sizeof(address);
    if (getsockname(socket_fd, (struct sockaddr*)&address, &length) != 0)
    {
        ws_close_socket(socket_fd);
        *error = "Failed to inspect server socket!";
        return WS_BAD_SOCKET;
    }
    *bound_port = ntohs(address.sin_port);
    return socket_fd;
}

#ifdef _WIN32
static PCCERT_CONTEXT ws_windows_certificate(X509* certificate)
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

static int ws_windows_verify(X509_STORE_CTX* verify, void* context)
{
    const char* host = context;
    X509* leaf = X509_STORE_CTX_get0_cert(verify);
    PCCERT_CONTEXT certificate = 0;
    PCCERT_CHAIN_CONTEXT chain = 0;
    HCERTSTORE extra = 0;
    int valid = 0;
    unsigned char address[16];
    int is_ip = inet_pton(AF_INET, host, address) == 1 || inet_pton(AF_INET6, host, address) == 1;
    if (!leaf || (is_ip ? X509_check_ip_asc(leaf, host, 0) : X509_check_host(leaf, host, 0, 0, 0)) != 1) goto done;
    certificate = ws_windows_certificate(leaf);
    extra = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, 0);
    if (!certificate || !extra) goto done;
    STACK_OF(X509)* peers = X509_STORE_CTX_get0_untrusted(verify);
    for (int i = 0; peers && i < sk_X509_num(peers); ++i)
    {
        PCCERT_CONTEXT peer = ws_windows_certificate(sk_X509_value(peers, i));
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

static SSL_CTX* ws_tls_client_context(const char* host, const char** error)
{
    SSL_CTX* context = SSL_CTX_new(TLS_client_method());
    if (!context) { *error = "TLS initialization failed"; return 0; }
#ifdef _WIN32
    if (!getenv("SSL_CERT_FILE")) SSL_CTX_set_cert_verify_callback(context, ws_windows_verify, (void*)host);
    else
#endif
    if (SSL_CTX_set_default_verify_paths(context) != 1)
    {
        SSL_CTX_free(context);
        *error = "TLS trust setup failed";
        return 0;
    }
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, 0);
    return context;
}

static SSL* ws_tls_client_connect(SSL_CTX* context, ws_Socket socket_fd, const char* host, const char** error)
{
    SSL* ssl = SSL_new(context);
    if (!ssl) { *error = "TLS initialization failed"; return 0; }
    unsigned char address[16];
    int is_ip = inet_pton(AF_INET, host, address) == 1 || inet_pton(AF_INET6, host, address) == 1;
    X509_VERIFY_PARAM* verify = SSL_get0_param(ssl);
    if ((is_ip ? X509_VERIFY_PARAM_set1_ip_asc(verify, host) :
                 (SSL_set_tlsext_host_name(ssl, host) && X509_VERIFY_PARAM_set1_host(verify, host, 0))) != 1 ||
        SSL_set_fd(ssl, (int)socket_fd) != 1)
    {
        SSL_free(ssl);
        *error = "TLS initialization failed";
        return 0;
    }
    ERR_clear_error();
    if (SSL_connect(ssl) != 1)
    {
        *error = SSL_get_verify_result(ssl) != X509_V_OK ? "TLS certificate verification failed" : "TLS handshake failed";
        SSL_free(ssl);
        return 0;
    }
    return ssl;
}

static SSL_CTX* ws_tls_server_context(const char* cert, const char* key, const char** error)
{
    SSL_CTX* context = SSL_CTX_new(TLS_server_method());
    if (!context) { *error = "TLS initialization failed"; return 0; }
    if (SSL_CTX_use_certificate_chain_file(context, cert) != 1 ||
        SSL_CTX_use_PrivateKey_file(context, key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(context) != 1)
    {
        SSL_CTX_free(context);
        *error = "TLS certificate or key could not be loaded";
        return 0;
    }
    return context;
}

static SSL* ws_tls_server_accept(SSL_CTX* context, ws_Socket socket_fd, const char** error)
{
    SSL* ssl = SSL_new(context);
    if (!ssl || SSL_set_fd(ssl, (int)socket_fd) != 1)
    {
        if (ssl) SSL_free(ssl);
        *error = "TLS initialization failed";
        return 0;
    }
    if (SSL_accept(ssl) != 1)
    {
        SSL_free(ssl);
        *error = "TLS handshake failed";
        return 0;
    }
    return ssl;
}

static int ws_base64_sha1(const char* input, char* output, size_t output_size)
{
    static const char* guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char combined[128];
    int length = snprintf(combined, sizeof(combined), "%s%s", input, guid);
    if (length <= 0 || (size_t)length >= sizeof(combined)) return 0;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    if (EVP_Digest(combined, (size_t)length, digest, &digest_length, EVP_sha1(), 0) != 1) return 0;
    int encoded = EVP_EncodeBlock((unsigned char*)output, digest, (int)digest_length);
    if (encoded < 0 || (size_t)encoded >= output_size) return 0;
    output[encoded] = 0;
    return 1;
}

static int ws_random_key(char* output, size_t output_size)
{
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) return 0;
    int encoded = EVP_EncodeBlock((unsigned char*)output, bytes, (int)sizeof(bytes));
    if (encoded < 0 || (size_t)encoded >= output_size) return 0;
    output[encoded] = 0;
    return 1;
}

static int ws_blocking(ws_Socket socket_fd)
{
#ifdef _WIN32
    u_long mode = 0;
    return ioctlsocket(socket_fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(socket_fd, F_GETFL, 0);
    return flags >= 0 && fcntl(socket_fd, F_SETFL, flags & ~O_NONBLOCK) == 0;
#endif
}

static int ws_header_name_is(const char* name, size_t length, const char* expected)
{
    size_t expected_length = strlen(expected);
    if (length != expected_length) return 0;
    for (size_t i = 0; i < length; ++i)
        if (tolower((unsigned char)name[i]) != tolower((unsigned char)expected[i])) return 0;
    return 1;
}

static int ws_header_blob_get(const char* blob, const char* name, char* out, size_t out_size)
{
    const char* cursor = blob;
    size_t wanted = strlen(name);
    while (cursor && *cursor)
    {
        const char* end = strchr(cursor, '\n');
        if (!end) end = cursor + strlen(cursor);
        const char* colon = memchr(cursor, ':', (size_t)(end - cursor));
        if (colon && ws_header_name_is(cursor, (size_t)(colon - cursor), name))
        {
            const char* value = colon + 1;
            while (value < end && (*value == ' ' || *value == '\t')) ++value;
            const char* tail = end;
            while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t' || tail[-1] == '\r')) --tail;
            size_t length = (size_t)(tail - value);
            if (length >= out_size) return 0;
            memcpy(out, value, length);
            out[length] = 0;
            return 1;
        }
        cursor = *end ? end + 1 : end;
    }
    (void)wanted;
    return 0;
}

static int ws_header_blob_has_token(const char* blob, const char* name, const char* token)
{
    char value[2048];
    if (!ws_header_blob_get(blob, name, value, sizeof(value))) return 0;
    const char* cursor = value;
    while (*cursor)
    {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') ++cursor;
        const char* start = cursor;
        while (*cursor && *cursor != ',') ++cursor;
        const char* end = cursor;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) --end;
        if ((size_t)(end - start) == strlen(token) && ws_header_name_is(start, (size_t)(end - start), token)) return 1;
    }
    return 0;
}

static int ws_parse_request_headers(const char* text, size_t length, char** method, char** path, char** headers)
{
    if (length >= 65536 || memchr(text, 0, length)) return 0;
    const char* end = strstr(text, "\r\n\r\n");
    if (!end) return 0;
    const char* first_end = strstr(text, "\r\n");
    if (!first_end || first_end > end) return 0;
    const char* space1 = memchr(text, ' ', (size_t)(first_end - text));
    if (!space1) return 0;
    const char* space2 = memchr(space1 + 1, ' ', (size_t)(first_end - space1 - 1));
    if (!space2) return 0;
    size_t method_length = (size_t)(space1 - text);
    size_t path_length = (size_t)(space2 - space1 - 1);
    if (!method_length || !path_length || path_length >= 2048) return 0;
    *method = malloc(method_length + 1);
    *path = malloc(path_length + 1);
    size_t header_length = (size_t)(end - (first_end + 2));
    *headers = malloc(header_length + 1);
    if (!*method || !*path || !*headers) return 0;
    memcpy(*method, text, method_length);
    (*method)[method_length] = 0;
    memcpy(*path, space1 + 1, path_length);
    (*path)[path_length] = 0;
    const char* cursor = first_end + 2;
    char* out = *headers;
    while (cursor < end)
    {
        const char* next = strstr(cursor, "\r\n");
        if (!next || next > end) next = end;
        size_t line_length = (size_t)(next - cursor);
        if (line_length)
        {
            memcpy(out, cursor, line_length);
            out += line_length;
            *out++ = '\n';
        }
        cursor = next + 2;
    }
    *out = 0;
    return 1;
}

static void ws_conn_cleanup_io(WsConn* conn);
static void ws_conn_release(WsConn* conn);

static void ws_client_worker(WsConn* conn)
{
    atomic_store_explicit(&conn->worker_running, 1, memory_order_release);
    const char* error = 0;
    ws_Socket socket_fd = WS_BAD_SOCKET;
    if (!ws_connect_socket(conn->host, conn->port, &socket_fd, &error)) goto fail;
    atomic_store(&conn->socket, (uintptr_t)socket_fd);
    if (conn->secure)
    {
        conn->tls_ctx = ws_tls_client_context(conn->host, &error);
        if (!conn->tls_ctx) goto fail;
        conn->ssl = ws_tls_client_connect(conn->tls_ctx, socket_fd, conn->host, &error);
        if (!conn->ssl) goto fail;
    }
    if (!ws_write_all(conn, conn->request, strlen(conn->request))) { error = "Request send failed"; goto fail; }

    char response[16384] = { 0 };
    size_t length = 0;
    while (!strstr(response, "\r\n\r\n"))
    {
        if (length + 1 >= sizeof(response)) { error = "Invalid HTTP response framing"; goto fail; }
        int count = ws_io_read(conn, (unsigned char*)response + length, sizeof(response) - 1 - length);
        if (count <= 0) { error = "Invalid HTTP response framing"; goto fail; }
        length += (size_t)count;
        response[length] = 0;
    }
    int status = 0;
    if (sscanf(response, "HTTP/%*u.%*u %d", &status) != 1 || status != 101) { error = "WebSocket upgrade failed"; goto fail; }
    char headers[16384];
    const char* end = strstr(response, "\r\n\r\n");
    const char* first = strstr(response, "\r\n");
    size_t header_length = (size_t)(end - (first + 2));
    memcpy(headers, first + 2, header_length);
    headers[header_length] = 0;
    if (!ws_header_blob_has_token(headers, "Upgrade", "websocket") ||
        !ws_header_blob_has_token(headers, "Connection", "Upgrade"))
    {
        error = "WebSocket upgrade failed";
        goto fail;
    }
    char accept[128];
    if (!ws_header_blob_get(headers, "Sec-WebSocket-Accept", accept, sizeof(accept)) ||
        strcmp(accept, conn->expected_accept) != 0)
    {
        error = "WebSocket accept validation failed";
        goto fail;
    }
    size_t header_total = (size_t)(end + 4 - response);
    size_t leftover = length - header_total;
    if (leftover)
    {
        conn->prebuffer = malloc(leftover);
        if (!conn->prebuffer) { error = "Out of memory"; goto fail; }
        memcpy(conn->prebuffer, response + header_total, leftover);
        conn->prebuffer_len = leftover;
    }
    if (!ws_nonblocking(socket_fd)) { error = "Socket initialization failed"; goto fail; }
    atomic_store_explicit(&conn->handshake_ok, 1, memory_order_release);
    atomic_store_explicit(&conn->handshake_ready, 1, memory_order_release);
    atomic_store_explicit(&conn->worker_running, 0, memory_order_release);
    ws_conn_release(conn);
    return;

fail:
    conn->error = ws_strdup(error ? error : "WebSocket connection failed");
    atomic_store_explicit(&conn->handshake_ready, 1, memory_order_release);
    ws_conn_cleanup_io(conn);
    atomic_store_explicit(&conn->worker_running, 0, memory_order_release);
    ws_conn_release(conn);
}

static void ws_server_worker(WsConn* conn)
{
    atomic_store_explicit(&conn->worker_running, 1, memory_order_release);
    const char* error = 0;
    ws_Socket socket_fd = (ws_Socket)atomic_load(&conn->socket);
    ws_blocking(socket_fd);
    ws_set_timeout(socket_fd, 10);
    if (conn->secure)
    {
        conn->ssl = ws_tls_server_accept(conn->tls_ctx, socket_fd, &error);
        if (!conn->ssl) goto fail;
    }

    char request[65536] = { 0 };
    size_t length = 0;
    while (!strstr(request, "\r\n\r\n"))
    {
        if (length + 1 >= sizeof(request)) { error = "Invalid HTTP request"; goto fail; }
        int count = ws_io_read(conn, (unsigned char*)request + length, sizeof(request) - 1 - length);
        if (count <= 0) { error = "Invalid HTTP request"; goto fail; }
        length += (size_t)count;
        request[length] = 0;
    }
    if (!ws_parse_request_headers(request, length, &conn->method, &conn->request_path, &conn->headers_blob))
    {
        error = "Invalid WebSocket upgrade request";
        goto fail;
    }
    if (!ws_header_blob_has_token(conn->headers_blob, "Upgrade", "websocket") ||
        !ws_header_blob_has_token(conn->headers_blob, "Connection", "Upgrade"))
    {
        error = "Invalid WebSocket upgrade request";
        goto fail;
    }
    char version[32], key[128], accept[128];
    if (!ws_header_blob_get(conn->headers_blob, "Sec-WebSocket-Version", version, sizeof(version)) || strcmp(version, "13") != 0 ||
        !ws_header_blob_get(conn->headers_blob, "Sec-WebSocket-Key", key, sizeof(key)) ||
        !ws_base64_sha1(key, accept, sizeof(accept)))
    {
        error = "Invalid WebSocket upgrade request";
        goto fail;
    }
    char response[512];
    snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (!ws_write_all(conn, response, strlen(response))) { error = "Response send failed"; goto fail; }
    if (!ws_nonblocking(socket_fd)) { error = "Socket initialization failed"; goto fail; }
    conn->url = ws_strdup(conn->request_path);
    atomic_store_explicit(&conn->handshake_ok, 1, memory_order_release);
    atomic_store_explicit(&conn->handshake_ready, 1, memory_order_release);
    atomic_store_explicit(&conn->worker_running, 0, memory_order_release);
    ws_conn_release(conn);
    return;

fail:
    conn->error = ws_strdup(error ? error : "WebSocket handshake failed");
    atomic_store_explicit(&conn->handshake_ready, 1, memory_order_release);
    ws_conn_cleanup_io(conn);
    atomic_store_explicit(&conn->worker_running, 0, memory_order_release);
    ws_conn_release(conn);
}

#ifdef _WIN32
static DWORD WINAPI ws_client_thread(LPVOID data) { ws_client_worker(data); return 0; }
static DWORD WINAPI ws_server_thread(LPVOID data) { ws_server_worker(data); return 0; }
#else
static void* ws_client_thread(void* data) { ws_client_worker(data); return 0; }
static void* ws_server_thread(void* data) { ws_server_worker(data); return 0; }
#endif

static void ws_conn_cleanup_io(WsConn* conn)
{
    if (atomic_exchange(&conn->io_closed, 1)) return;
    if (conn->wslay) { wslay_event_context_free(conn->wslay); conn->wslay = 0; }
    if (conn->ssl) { SSL_free(conn->ssl); conn->ssl = 0; }
    if (conn->tls_ctx) { SSL_CTX_free(conn->tls_ctx); conn->tls_ctx = 0; }
    ws_Socket socket_fd = (ws_Socket)atomic_exchange(&conn->socket, (uintptr_t)WS_BAD_SOCKET);
    ws_close_socket(socket_fd);
}

static void ws_msg_clear(WsMsg* msg)
{
    while (msg)
    {
        WsMsg* next = msg->next;
        free(msg->data);
        free(msg);
        msg = next;
    }
}

static void ws_conn_free(WsConn* conn)
{
    ws_conn_cleanup_io(conn);
    ws_msg_clear(conn->msg_head);
    free(conn->close_reason);
    free(conn->error);
    free(conn->prebuffer);
    free(conn->host);
    free(conn->path);
    free(conn->url);
    free(conn->request);
    free(conn->expected_accept);
    free(conn->method);
    free(conn->request_path);
    free(conn->headers_blob);
    free(conn->body);
    free(conn);
}

static void ws_conn_release(WsConn* conn)
{
    if (atomic_fetch_sub(&conn->refs, 1) != 1) return;
    ws_conn_free(conn);
}

static void ws_conn_request_stop(WsConn* conn)
{
    atomic_store_explicit(&conn->stop, 1, memory_order_release);
    ws_shutdown_socket((ws_Socket)atomic_load(&conn->socket));
}

static void ws_server_release(WsServer* server);

static void ws_conn_detach_server(WsConn* conn)
{
    if (!conn->server) return;
    for (size_t i = 0; i < WS_CLIENT_LIMIT; ++i)
        if (conn->server->clients[i] == conn) conn->server->clients[i] = 0;
    WsServer* server = conn->server;
    conn->server = 0;
    ws_server_release(server);
}

static void ws_conn_destroy_native(void* data)
{
    WsConn* conn = data;
    lt_VM* vm = conn->owner;
    atomic_store_explicit(&conn->stop, 1, memory_order_release);
    ws_shutdown_socket((ws_Socket)atomic_load(&conn->socket));
    if (!atomic_load(&conn->worker_running)) ws_conn_cleanup_io(conn);
    if (conn->hook) { ws_api->remove_poll_hook(vm, conn->hook); conn->hook = 0; }
    if (!LT_IS_NULL(conn->connect_promise)) { ws_api->unroot_persistent(vm, conn->connect_promise); conn->connect_promise = LT_VALUE_NULL; }
    if (!LT_IS_NULL(conn->pending_next)) { ws_api->unroot_persistent(vm, conn->pending_next); conn->pending_next = LT_VALUE_NULL; }
    if (!LT_IS_NULL(conn->instance)) { ws_api->unroot_persistent(vm, conn->instance); conn->instance = LT_VALUE_NULL; }
    ws_conn_detach_server(conn);
    ws_conn_release(conn);
}

static void ws_msg_push(WsConn* conn, int binary, const uint8_t* data, size_t length)
{
    if (length > WS_MAX_MESSAGE || conn->queued_bytes + length > WS_MAX_MESSAGE)
    {
        conn->error = ws_strdup("WebSocket message too large");
        conn->closed = 1;
        return;
    }
    WsMsg* msg = calloc(1, sizeof(*msg));
    if (!msg) { conn->error = ws_strdup("Out of memory"); conn->closed = 1; return; }
    msg->data = malloc(length ? length : 1);
    if (!msg->data) { free(msg); conn->error = ws_strdup("Out of memory"); conn->closed = 1; return; }
    if (length) memcpy(msg->data, data, length);
    msg->length = length;
    msg->binary = binary;
    if (conn->msg_tail) conn->msg_tail->next = msg;
    else conn->msg_head = msg;
    conn->msg_tail = msg;
    conn->queued_bytes += length;
}

static lt_Value ws_msg_value(lt_VM* vm, WsMsg* msg)
{
    if (!msg->binary)
    {
        char* text = malloc(msg->length + 1);
        if (!text) return LT_VALUE_NULL;
        memcpy(text, msg->data, msg->length);
        text[msg->length] = 0;
        if (strlen(text) != msg->length) { free(text); return LT_VALUE_NULL; }
        lt_Value result = lt_make_string(vm, text);
        free(text);
        return result;
    }
    lt_Value array = lt_make_array(vm);
    lt_push(vm, array);
    for (size_t i = 0; i < msg->length; ++i)
        lt_array_push(vm, array, lt_make_number((double)(unsigned char)msg->data[i]));
    lt_pop(vm);
    return array;
}

static void ws_conn_deliver_next(lt_VM* vm, WsConn* conn)
{
    if (LT_IS_NULL(conn->pending_next)) return;
    if (!conn->msg_head)
    {
        if (conn->error)
        {
            ltasync_settle_promise(vm, conn->pending_next, LT_PROMISE_REJECTED, lt_make_string(vm, conn->error));
        }
        else if (conn->closed)
        {
            ltasync_settle_promise(vm, conn->pending_next, LT_PROMISE_FULFILLED, LT_VALUE_NULL);
        }
        else return;
    }
    else
    {
        WsMsg* msg = conn->msg_head;
        conn->msg_head = msg->next;
        if (!conn->msg_head) conn->msg_tail = 0;
        conn->queued_bytes -= msg->length;
        lt_Value value = ws_msg_value(vm, msg);
        ws_api->root(vm, value);
        ltasync_settle_promise(vm, conn->pending_next, LT_PROMISE_FULFILLED, value);
        free(msg->data);
        free(msg);
    }
    ws_api->unroot_persistent(vm, conn->pending_next);
    conn->pending_next = LT_VALUE_NULL;
}

static ssize_t ws_wslay_recv(wslay_event_context_ptr ctx, uint8_t* buffer, size_t length, int flags, void* userdata)
{
    (void)flags;
    WsConn* conn = userdata;
    if (conn->prebuffer_pos < conn->prebuffer_len)
    {
        size_t available = conn->prebuffer_len - conn->prebuffer_pos;
        size_t count = available < length ? available : length;
        memcpy(buffer, conn->prebuffer + conn->prebuffer_pos, count);
        conn->prebuffer_pos += count;
        return (ssize_t)count;
    }
    int count = ws_io_read(conn, buffer, length);
    if (count == -2) return WSLAY_ERR_WOULDBLOCK;
    if (count <= 0)
    {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }
    return (ssize_t)count;
}

static ssize_t ws_wslay_send(wslay_event_context_ptr ctx, const uint8_t* data, size_t length, int flags, void* userdata)
{
    (void)flags;
    WsConn* conn = userdata;
    int count = ws_io_write(conn, data, length);
    if (count == -2) return WSLAY_ERR_WOULDBLOCK;
    if (count <= 0)
    {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }
    return (ssize_t)count;
}

static void ws_wslay_msg(wslay_event_context_ptr ctx, const struct wslay_event_on_msg_recv_arg* arg, void* userdata)
{
    WsConn* conn = userdata;
    if (arg->opcode == WSLAY_CONNECTION_CLOSE)
    {
        conn->peer_closed = 1;
        conn->closed = 1;
        conn->close_code = arg->status_code;
        wslay_event_queue_close(ctx, arg->status_code, arg->msg, arg->msg_length);
        return;
    }
    if (arg->opcode == WSLAY_TEXT_FRAME || arg->opcode == WSLAY_BINARY_FRAME)
        ws_msg_push(conn, arg->opcode == WSLAY_BINARY_FRAME, arg->msg ? arg->msg : (const uint8_t*)"", arg->msg_length);
}

static int ws_wslay_genmask(wslay_event_context_ptr ctx, uint8_t* buffer, size_t length, void* userdata)
{
    (void)ctx;
    (void)userdata;
    return RAND_bytes(buffer, (int)length) == 1 ? 0 : -1;
}

static struct wslay_event_callbacks ws_wslay_callbacks = {
    ws_wslay_recv,
    ws_wslay_send,
    ws_wslay_genmask,
    0,
    0,
    0,
    ws_wslay_msg
};

static int ws_append(char* buffer, size_t capacity, size_t* length, const char* format, ...)
{
    if (*length >= capacity) return 0;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + *length, capacity - *length, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= capacity - *length) return 0;
    *length += (size_t)written;
    return 1;
}

static lt_Value ws_request_table(lt_VM* vm, WsConn* conn)
{
    lt_Value request = ws_api->make_table(vm);
    ws_api->root(vm, request);
    ws_api->table_set(vm, request, ws_api->make_string(vm, "url"), ws_api->make_string(vm, conn->request_path ? conn->request_path : "/"));
    ws_api->table_set(vm, request, ws_api->make_string(vm, "method"), ws_api->make_string(vm, conn->method ? conn->method : "GET"));
    ws_api->table_set(vm, request, ws_api->make_string(vm, "body"), ws_api->make_string(vm, conn->body ? conn->body : ""));
    lt_Value headers = ws_api->make_table(vm);
    ws_api->root(vm, headers);
    const char* cursor = conn->headers_blob;
    while (cursor && *cursor)
    {
        const char* end = strchr(cursor, '\n');
        if (!end) end = cursor + strlen(cursor);
        const char* colon = memchr(cursor, ':', (size_t)(end - cursor));
        if (colon)
        {
            const char* value = colon + 1;
            while (value < end && (*value == ' ' || *value == '\t')) ++value;
            const char* tail = end;
            while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t' || tail[-1] == '\r')) --tail;
            char* name = malloc((size_t)(colon - cursor) + 1);
            char* text = malloc((size_t)(tail - value) + 1);
            if (name && text)
            {
                memcpy(name, cursor, (size_t)(colon - cursor));
                name[colon - cursor] = 0;
                memcpy(text, value, (size_t)(tail - value));
                text[tail - value] = 0;
                ws_api->table_set(vm, headers, ws_api->make_string(vm, name), ws_api->make_string(vm, text));
            }
            free(name);
            free(text);
        }
        cursor = *end ? end + 1 : end;
    }
    ws_api->table_set(vm, request, ws_api->make_string(vm, "headers"), headers);
    return request;
}

static void ws_invoke_server_handler(lt_VM* vm, WsConn* conn)
{
    if (!conn->server || LT_IS_NULL(conn->server->handler)) return;
    lt_Value request = ws_request_table(vm, conn);
    ws_api->push(vm, conn->instance);
    ws_api->push(vm, request);
    if (ltasync_is_async_callable(conn->server->handler)) ltasync_call(vm, conn->server->handler, 2);
    else
    {
        uint16_t count = ws_api->exec(vm, conn->server->handler, 2);
        while (count--) ws_api->pop(vm);
    }
}

static lt_PollResult ws_conn_poll(lt_VM* vm, void* context)
{
    WsConn* conn = context;
    if (!atomic_load_explicit(&conn->handshake_ready, memory_order_acquire)) return LT_POLL_PENDING;
    if (!atomic_load_explicit(&conn->handshake_ok, memory_order_acquire))
    {
        if (!LT_IS_NULL(conn->connect_promise))
        {
            ltasync_settle_promise(vm, conn->connect_promise, LT_PROMISE_REJECTED,
                lt_make_string(vm, conn->error ? conn->error : "WebSocket connection failed"));
            ws_api->unroot_persistent(vm, conn->connect_promise);
            conn->connect_promise = LT_VALUE_NULL;
        }
        if (!LT_IS_NULL(conn->instance))
        {
            ws_api->unroot_persistent(vm, conn->instance);
            conn->instance = LT_VALUE_NULL;
        }
        if (!conn->closed)
        {
            conn->closed = 1;
            ws_conn_cleanup_io(conn);
            if (conn->hook) { ws_api->remove_poll_hook(vm, conn->hook); conn->hook = 0; }
        }
        return LT_POLL_WORK;
    }

    if (!conn->open)
    {
        int result = conn->is_server
            ? wslay_event_context_server_init(&conn->wslay, &ws_wslay_callbacks, conn)
            : wslay_event_context_client_init(&conn->wslay, &ws_wslay_callbacks, conn);
        if (result != 0)
        {
            conn->error = ws_strdup("WebSocket initialization failed");
            conn->closed = 1;
            ws_conn_cleanup_io(conn);
            if (conn->hook) { ws_api->remove_poll_hook(vm, conn->hook); conn->hook = 0; }
            return LT_POLL_WORK;
        }
        wslay_event_config_set_max_recv_msg_length(conn->wslay, WS_MAX_MESSAGE);
        conn->open = 1;
        if (!conn->is_server)
        {
            if (!LT_IS_NULL(conn->connect_promise))
            {
                ltasync_settle_promise(vm, conn->connect_promise, LT_PROMISE_FULFILLED, conn->instance);
                ws_api->unroot_persistent(vm, conn->connect_promise);
                conn->connect_promise = LT_VALUE_NULL;
            }
            if (!LT_IS_NULL(conn->instance))
            {
                ws_api->unroot_persistent(vm, conn->instance);
                conn->instance = LT_VALUE_NULL;
            }
        }
        else
        {
            ws_invoke_server_handler(vm, conn);
            if (!LT_IS_NULL(conn->instance))
            {
                ws_api->unroot_persistent(vm, conn->instance);
                conn->instance = LT_VALUE_NULL;
            }
        }
    }

    int did_work = 0;
    int result = wslay_event_recv(conn->wslay);
    if (result != 0 && result != WSLAY_ERR_WOULDBLOCK && result != WSLAY_ERR_WANT_READ)
    {
        conn->error = ws_strdup("WebSocket receive failed");
        conn->closed = 1;
    }
    else if (result == 0) did_work = 1;

    if (!conn->closed)
    {
        result = wslay_event_send(conn->wslay);
        if (result != 0 && result != WSLAY_ERR_WOULDBLOCK && result != WSLAY_ERR_WANT_WRITE)
        {
            conn->error = ws_strdup("WebSocket send failed");
            conn->closed = 1;
        }
        else if (result == 0) did_work = 1;
    }

    if (conn->local_close && wslay_event_get_queued_msg_count(conn->wslay) == 0) conn->closed = 1;
    if (conn->closed)
    {
        ws_conn_deliver_next(vm, conn);
        ws_conn_cleanup_io(conn);
        if (conn->hook) { ws_api->remove_poll_hook(vm, conn->hook); conn->hook = 0; }
    }
    else ws_conn_deliver_next(vm, conn);
    return (did_work || conn->closed) ? LT_POLL_WORK : LT_POLL_PENDING;
}

static WsConn* ws_instance_conn(lt_VM* vm, lt_Value instance)
{
    return ws_api->instance_get_native_data(vm, instance, ws_class(vm, "WebSocket"));
}

static uint8_t ws_socket_next(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) ws_api->runtime_error(vm, "WebSocket.next expects no arguments!");
    lt_Value instance = ws_api->pop(vm);
    WsConn* conn = ws_instance_conn(vm, instance);
    lt_Value promise = ltasync_make_promise(vm);
    if (!LT_IS_NULL(conn->pending_next))
    {
        ltasync_settle_promise(vm, promise, LT_PROMISE_REJECTED, lt_make_string(vm, "WebSocket.next is already pending!"));
    }
    else if (conn->msg_head || conn->closed)
    {
        conn->pending_next = promise;
        ws_api->root_persistent(vm, promise);
        ws_conn_deliver_next(vm, conn);
    }
    else
    {
        conn->pending_next = promise;
        ws_api->root_persistent(vm, promise);
    }
    ws_api->push(vm, promise);
    return 1;
}

static uint8_t ws_socket_send(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) ws_api->runtime_error(vm, "WebSocket.send expects one argument!");
    lt_Value value = ws_api->pop(vm);
    lt_Value instance = ws_api->pop(vm);
    WsConn* conn = ws_instance_conn(vm, instance);
    if (!conn->open || conn->closed || !conn->wslay)
    {
        ws_api->push(vm, LT_VALUE_FALSE);
        return 1;
    }
    struct wslay_event_msg message;
    memset(&message, 0, sizeof(message));
    char* binary = 0;
    if (LT_IS_STRING(value))
    {
        message.opcode = WSLAY_TEXT_FRAME;
        message.msg = (const uint8_t*)ws_api->get_string(vm, value);
        message.msg_length = strlen((const char*)message.msg);
    }
    else if (LT_IS_ARRAY(value))
    {
        uint32_t length = lt_array_length(value);
        binary = malloc(length ? length : 1);
        if (!binary) { ws_api->push(vm, LT_VALUE_FALSE); return 1; }
        for (uint32_t i = 0; i < length; ++i)
        {
            lt_Value item = lt_array_get(vm, value, i);
            if (!LT_IS_NUMBER(item)) { free(binary); ws_api->runtime_error(vm, "WebSocket binary message values must be bytes!"); return 0; }
            double byte = ws_api->get_number(item);
            if (byte < 0 || byte > 255 || byte != (double)(unsigned char)byte)
            {
                free(binary);
                ws_api->runtime_error(vm, "WebSocket binary message values must be bytes!");
                return 0;
            }
            binary[i] = (char)(unsigned char)byte;
        }
        message.opcode = WSLAY_BINARY_FRAME;
        message.msg = (const uint8_t*)binary;
        message.msg_length = length;
    }
    else
    {
        ws_api->runtime_error(vm, "WebSocket.send expects a string or byte array!");
        return 0;
    }
    if (wslay_event_get_queued_msg_length(conn->wslay) + message.msg_length > WS_MAX_QUEUE)
    {
        free(binary);
        ws_api->push(vm, LT_VALUE_FALSE);
        return 1;
    }
    int result = wslay_event_queue_msg(conn->wslay, &message);
    free(binary);
    ws_api->push(vm, result == 0 ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t ws_socket_close(lt_VM* vm, uint8_t argc)
{
    if (argc < 1 || argc > 3) ws_api->runtime_error(vm, "WebSocket.close expects optional code and reason!");
    lt_Value reason_value = argc == 3 ? ws_api->pop(vm) : LT_VALUE_NULL;
    lt_Value code_value = argc >= 2 ? ws_api->pop(vm) : LT_VALUE_NULL;
    lt_Value instance = ws_api->pop(vm);
    WsConn* conn = ws_instance_conn(vm, instance);
    int code = 1000;
    const char* reason = "";
    if (!LT_IS_NULL(code_value))
    {
        if (!LT_IS_NUMBER(code_value)) { ws_api->runtime_error(vm, "WebSocket.close code must be an integer!"); return 0; }
        double number = ws_api->get_number(code_value);
        if (number != (double)(int)number || (number != 1000 && (number < 3000 || number > 4999)))
        {
            ws_api->runtime_error(vm, "WebSocket.close code must be 1000 or 3000-4999!");
            return 0;
        }
        code = (int)number;
    }
    if (!LT_IS_NULL(reason_value))
    {
        if (!LT_IS_STRING(reason_value)) { ws_api->runtime_error(vm, "WebSocket.close reason must be a string!"); return 0; }
        reason = ws_api->get_string(vm, reason_value);
        if (strlen(reason) > 123) { ws_api->runtime_error(vm, "WebSocket.close reason is too long!"); return 0; }
    }
    if (conn->open && conn->wslay && !conn->closed)
    {
        wslay_event_queue_close(conn->wslay, (uint16_t)code, (const uint8_t*)reason, strlen(reason));
        conn->local_close = 1;
    }
    return 0;
}

static uint8_t ws_socket_url(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = ws_api->pop(vm);
    WsConn* conn = ws_instance_conn(vm, instance);
    ws_api->push(vm, ws_api->make_string(vm, conn && conn->url ? conn->url : ""));
    return 1;
}

static uint8_t ws_socket_closed(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = ws_api->pop(vm);
    WsConn* conn = ws_instance_conn(vm, instance);
    ws_api->push(vm, conn && conn->closed ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static int ws_build_client_request(lt_VM* vm, WsConn* conn, lt_Value headers)
{
    char key[64], accept[64];
    if (!ws_random_key(key, sizeof(key)) || !ws_base64_sha1(key, accept, sizeof(accept))) return 0;
    conn->expected_accept = ws_strdup(accept);
    if (!conn->expected_accept) return 0;

    char buffer[8192];
    size_t length = 0;
    int ipv6 = strchr(conn->host, ':') != 0;
    int default_port = conn->secure ? 443 : 80;
    if (!ws_append(buffer, sizeof(buffer), &length, "GET %s HTTP/1.1\r\n", conn->path)) return 0;
    if (conn->port == default_port)
    {
        if (ipv6) { if (!ws_append(buffer, sizeof(buffer), &length, "Host: [%s]\r\n", conn->host)) return 0; }
        else if (!ws_append(buffer, sizeof(buffer), &length, "Host: %s\r\n", conn->host)) return 0;
    }
    else
    {
        if (ipv6) { if (!ws_append(buffer, sizeof(buffer), &length, "Host: [%s]:%d\r\n", conn->host, conn->port)) return 0; }
        else if (!ws_append(buffer, sizeof(buffer), &length, "Host: %s:%d\r\n", conn->host, conn->port)) return 0;
    }
    if (!ws_append(buffer, sizeof(buffer), &length,
        "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n", key)) return 0;

    if (LT_IS_TABLE(headers))
    {
        uint64_t cursor = 0;
        lt_Value name = LT_VALUE_NULL;
        lt_Value value = LT_VALUE_NULL;
        while (ws_api->table_next(vm, headers, &cursor, &name, &value))
        {
            if (!LT_IS_STRING(name) || !LT_IS_STRING(value)) { ws_api->runtime_error(vm, "ws.connect headers must contain strings!"); return 0; }
            const char* field = ws_api->get_string(vm, name);
            const char* text = ws_api->get_string(vm, value);
            if (strchr(field, ':') || strpbrk(field, "\r\n") || strpbrk(text, "\r\n"))
            {
                ws_api->runtime_error(vm, "Invalid WebSocket header!");
                return 0;
            }
            if (ws_header_name_is(field, strlen(field), "Host") || ws_header_name_is(field, strlen(field), "Connection") ||
                ws_header_name_is(field, strlen(field), "Upgrade") || ws_header_name_is(field, strlen(field), "Sec-WebSocket-Key") ||
                ws_header_name_is(field, strlen(field), "Sec-WebSocket-Version")) continue;
            if (!ws_append(buffer, sizeof(buffer), &length, "%s: %s\r\n", field, text)) return 0;
        }
    }
    if (!ws_append(buffer, sizeof(buffer), &length, "\r\n")) return 0;
    conn->request = ws_strdup(buffer);
    return conn->request != 0;
}

static uint8_t ws_connect(lt_VM* vm, uint8_t argc)
{
    if (argc < 1 || argc > 2) { ws_api->runtime_error(vm, "ws.connect expects url and optional init!"); return 0; }
    lt_Value init = argc == 2 ? ws_api->pop(vm) : LT_VALUE_NULL;
    lt_Value url_value = ws_api->pop(vm);
    if (!LT_IS_STRING(url_value)) { ws_api->runtime_error(vm, "ws.connect url must be a string!"); return 0; }
    if (!LT_IS_NULL(init) && !LT_IS_TABLE(init)) { ws_api->runtime_error(vm, "ws.connect init must be a table!"); return 0; }
    ws_api->root(vm, url_value);
    ws_api->root(vm, init);
    lt_Value headers = LT_VALUE_NULL;
    if (LT_IS_TABLE(init))
    {
        headers = ws_api->table_get(vm, init, ws_api->make_string(vm, "headers"));
        if (!LT_IS_NULL(headers) && !LT_IS_TABLE(headers)) { ws_api->runtime_error(vm, "ws.connect headers must be a table!"); return 0; }
    }
    char host[256], path[2048];
    int port = 0, secure = 0;
    if (!ws_parse_url(ws_api->get_string(vm, url_value), host, sizeof(host), &port, &secure, path, sizeof(path)))
    {
        ws_api->runtime_error(vm, "Unsupported WebSocket URL!");
        return 0;
    }
    WsConn* conn = calloc(1, sizeof(*conn));
    if (!conn) { ws_api->runtime_error(vm, "Out of memory creating WebSocket!"); return 0; }
    conn->owner = vm;
    conn->instance = LT_VALUE_NULL;
    conn->connect_promise = LT_VALUE_NULL;
    conn->pending_next = LT_VALUE_NULL;
    atomic_init(&conn->refs, 2);
    atomic_init(&conn->socket, (uintptr_t)WS_BAD_SOCKET);
    atomic_init(&conn->stop, 0);
    atomic_init(&conn->worker_running, 0);
    atomic_init(&conn->handshake_ready, 0);
    atomic_init(&conn->handshake_ok, 0);
    atomic_init(&conn->io_closed, 0);
    conn->host = ws_strdup(host);
    conn->port = port;
    conn->secure = secure;
    conn->path = ws_strdup(path);
    conn->url = ws_strdup(ws_api->get_string(vm, url_value));
    if (!conn->host || !conn->path || !conn->url || !ws_build_client_request(vm, conn, headers))
    {
        ws_conn_release(conn);
        ws_api->runtime_error(vm, "Out of memory creating WebSocket!");
        return 0;
    }
    uint16_t count = ws_api->exec(vm, ws_class(vm, "WebSocket"), 0);
    lt_Value instance = count ? ws_api->pop(vm) : LT_VALUE_NULL;
    while (count > 1) { ws_api->pop(vm); --count; }
    if (!LT_IS_INSTANCE(instance))
    {
        ws_conn_release(conn);
        ws_api->runtime_error(vm, "Failed to create WebSocket instance!");
        return 0;
    }
    conn->instance = instance;
    ws_api->root_persistent(vm, instance);
    ws_api->instance_set_native_data(vm, instance, ws_class(vm, "WebSocket"), conn);
    conn->connect_promise = ltasync_make_promise(vm);
    ws_api->root_persistent(vm, conn->connect_promise);
    conn->hook = lt_add_poll_hook(vm, ws_conn_poll, conn);
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, ws_client_thread, conn, 0, NULL);
    if (thread) CloseHandle(thread);
    else {
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, ws_client_thread, conn) == 0) pthread_detach(thread);
    else {
#endif
        conn->error = ws_strdup("Failed to start WebSocket worker");
        atomic_store(&conn->handshake_ready, 1);
        ws_conn_release(conn);
    }
    ws_api->push(vm, conn->connect_promise);
    return 1;
}

static void ws_server_ref(WsServer* server) { atomic_fetch_add(&server->refs, 1); }

static void ws_server_release(WsServer* server)
{
    if (!server || atomic_fetch_sub(&server->refs, 1) != 1) return;
    if (server->listener != WS_BAD_SOCKET) ws_close_socket(server->listener);
    if (server->tls_ctx) SSL_CTX_free(server->tls_ctx);
    free(server);
}

static void ws_server_hook_destroy(lt_VM* vm, void* context)
{
    WsServer* server = context;
    if (server->listener != WS_BAD_SOCKET) { ws_close_socket(server->listener); server->listener = WS_BAD_SOCKET; }
    if (!LT_IS_NULL(server->handler)) { ws_api->unroot_persistent(vm, server->handler); server->handler = LT_VALUE_NULL; }
    if (!LT_IS_NULL(server->instance)) { ws_api->unroot_persistent(vm, server->instance); server->instance = LT_VALUE_NULL; }
    ws_server_release(server);
}

static void ws_server_destroy_native(void* data)
{
    WsServer* server = data;
    server->closed = 1;
    if (server->listener != WS_BAD_SOCKET) { ws_close_socket(server->listener); server->listener = WS_BAD_SOCKET; }
    for (size_t i = 0; i < WS_CLIENT_LIMIT; ++i)
        if (server->clients[i]) ws_conn_request_stop(server->clients[i]);
    ws_server_release(server);
}

static lt_PollResult ws_server_poll(lt_VM* vm, void* context)
{
    WsServer* server = context;
    if (server->closed) return LT_POLL_IDLE;
    lt_PollResult result = LT_POLL_PENDING;
    for (size_t slot = 0; slot < WS_CLIENT_LIMIT; ++slot)
    {
        if (server->clients[slot]) continue;
        ws_Socket socket_fd = accept(server->listener, 0, 0);
        if (socket_fd == WS_BAD_SOCKET) break;
        WsConn* conn = calloc(1, sizeof(*conn));
        if (!conn) { ws_close_socket(socket_fd); continue; }
        conn->owner = vm;
        conn->instance = LT_VALUE_NULL;
        conn->connect_promise = LT_VALUE_NULL;
        conn->pending_next = LT_VALUE_NULL;
        atomic_init(&conn->refs, 2);
        atomic_init(&conn->socket, (uintptr_t)socket_fd);
        atomic_init(&conn->stop, 0);
        atomic_init(&conn->worker_running, 0);
        atomic_init(&conn->handshake_ready, 0);
        atomic_init(&conn->handshake_ok, 0);
        atomic_init(&conn->io_closed, 0);
        conn->is_server = 1;
        conn->server = server;
        ws_server_ref(server);
        if (server->tls_ctx)
        {
            conn->secure = 1;
            conn->tls_ctx = server->tls_ctx;
            SSL_CTX_up_ref(conn->tls_ctx);
        }
        uint16_t count = ws_api->exec(vm, ws_class(vm, "WebSocket"), 0);
        lt_Value instance = count ? ws_api->pop(vm) : LT_VALUE_NULL;
        while (count > 1) { ws_api->pop(vm); --count; }
        if (!LT_IS_INSTANCE(instance))
        {
            ws_conn_release(conn);
            ws_close_socket(socket_fd);
            continue;
        }
        conn->instance = instance;
        ws_api->root_persistent(vm, instance);
        ws_api->instance_set_native_data(vm, instance, ws_class(vm, "WebSocket"), conn);
        server->clients[slot] = conn;
        conn->hook = lt_add_poll_hook(vm, ws_conn_poll, conn);
#ifdef _WIN32
        HANDLE thread = CreateThread(NULL, 0, ws_server_thread, conn, 0, NULL);
        if (thread) CloseHandle(thread);
        else {
#else
        pthread_t thread;
        if (pthread_create(&thread, NULL, ws_server_thread, conn) == 0) pthread_detach(thread);
        else {
#endif
            conn->error = ws_strdup("Failed to start WebSocket worker");
            atomic_store(&conn->handshake_ready, 1);
            ws_conn_release(conn);
        }
        result = LT_POLL_WORK;
    }
    return result;
}

static uint8_t ws_serve(lt_VM* vm, uint8_t argc)
{
    if (argc < 1 || argc > 2) { ws_api->runtime_error(vm, "ws.serve expects handler and optional options!"); return 0; }
    lt_Value options = argc == 2 ? ws_api->pop(vm) : LT_VALUE_NULL;
    lt_Value handler = ws_api->pop(vm);
    if (!ltasync_is_callable(handler)) { ws_api->runtime_error(vm, "ws.serve handler must be callable!"); return 0; }
    ws_api->root(vm, handler);
    ws_api->root(vm, options);
    const char* hostname = "0.0.0.0";
    int port = 8000;
    const char* cert = 0;
    const char* key = 0;
    if (LT_IS_TABLE(options))
    {
        lt_Value value = ws_api->table_get(vm, options, ws_api->make_string(vm, "hostname"));
        if (LT_IS_STRING(value)) hostname = ws_api->get_string(vm, value);
        value = ws_api->table_get(vm, options, ws_api->make_string(vm, "port"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_NUMBER(value)) { ws_api->runtime_error(vm, "Invalid WebSocket server port!"); return 0; }
            double number = ws_api->get_number(value);
            if (number < 0 || number > 65535 || number != (double)(int)number) { ws_api->runtime_error(vm, "Invalid WebSocket server port!"); return 0; }
            port = (int)number;
        }
        value = ws_api->table_get(vm, options, ws_api->make_string(vm, "tls"));
        if (!LT_IS_NULL(value))
        {
            if (!LT_IS_TABLE(value)) { ws_api->runtime_error(vm, "WebSocket tls options must be a table!"); return 0; }
            lt_Value cert_value = ws_api->table_get(vm, value, ws_api->make_string(vm, "cert"));
            lt_Value key_value = ws_api->table_get(vm, value, ws_api->make_string(vm, "key"));
            if (!LT_IS_STRING(cert_value) || !LT_IS_STRING(key_value))
            {
                ws_api->runtime_error(vm, "WebSocket tls requires cert and key strings!");
                return 0;
            }
            cert = ws_api->get_string(vm, cert_value);
            key = ws_api->get_string(vm, key_value);
        }
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { ws_api->runtime_error(vm, "Socket initialization failed!"); return 0; }
#endif
    const char* error = 0;
    int bound_port = 0;
    ws_Socket listener = ws_create_listener(hostname, port, &bound_port, &error);
    if (listener == WS_BAD_SOCKET) { ws_api->runtime_error(vm, error ? error : "Failed to start server socket!"); return 0; }
    SSL_CTX* tls_ctx = 0;
    if (cert)
    {
        tls_ctx = ws_tls_server_context(cert, key, &error);
        if (!tls_ctx) { ws_close_socket(listener); ws_api->runtime_error(vm, error ? error : "TLS initialization failed"); return 0; }
    }
    WsServer* server = calloc(1, sizeof(*server));
    if (!server) { ws_close_socket(listener); if (tls_ctx) SSL_CTX_free(tls_ctx); ws_api->runtime_error(vm, "Out of memory starting WebSocket server!"); return 0; }
    server->owner = vm;
    server->instance = LT_VALUE_NULL;
    server->handler = LT_VALUE_NULL;
    atomic_init(&server->refs, 2);
    server->listener = listener;
    server->tls_ctx = tls_ctx;
    server->handler = handler;
    ws_api->root_persistent(vm, handler);
    uint16_t count = ws_api->exec(vm, ws_class(vm, "Server"), 0);
    lt_Value instance = count ? ws_api->pop(vm) : LT_VALUE_NULL;
    while (count > 1) { ws_api->pop(vm); --count; }
    if (!LT_IS_INSTANCE(instance))
    {
        ws_close_socket(listener);
        if (tls_ctx) SSL_CTX_free(tls_ctx);
        free(server);
        ws_api->runtime_error(vm, "Failed to create WebSocket server instance!");
        return 0;
    }
    server->instance = instance;
    ws_api->root_persistent(vm, instance);
    ws_api->instance_set_native_data(vm, instance, ws_class(vm, "Server"), server);
    server->hook = ltasync_add_owned_poll_hook(vm, ws_server_poll, server, ws_server_hook_destroy);
    ws_api->push(vm, instance);
    return 1;
}

static uint8_t ws_server_close(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) ws_api->runtime_error(vm, "WebSocket server close expects no arguments!");
    lt_Value instance = ws_api->pop(vm);
    WsServer* server = ws_api->instance_get_native_data(vm, instance, ws_class(vm, "Server"));
    if (server && !server->closed)
    {
        server->closed = 1;
        if (server->listener != WS_BAD_SOCKET) { ws_close_socket(server->listener); server->listener = WS_BAD_SOCKET; }
        for (size_t i = 0; i < WS_CLIENT_LIMIT; ++i)
            if (server->clients[i]) ws_conn_request_stop(server->clients[i]);
    }
    return 0;
}

static uint8_t ws_server_port(lt_VM* vm, uint8_t argc)
{
    (void)argc;
    lt_Value instance = ws_api->pop(vm);
    WsServer* server = ws_api->instance_get_native_data(vm, instance, ws_class(vm, "Server"));
    int port = 0;
    if (server && server->listener != WS_BAD_SOCKET)
    {
        struct sockaddr_in address;
        socklen_t length = sizeof(address);
        if (getsockname(server->listener, (struct sockaddr*)&address, &length) == 0) port = ntohs(address.sin_port);
    }
    ws_api->push(vm, ws_api->make_number(port));
    return 1;
}

void ltstd_open_ws(lt_VM* vm)
{
    lt_Value socket_class = ws_api->class_create(vm, "WebSocket");
    ws_api->class_set_native_data_destroy(vm, socket_class, ws_conn_destroy_native);
    ws_api->class_add_method(vm, socket_class, "next", ws_socket_next, LT_VIS_PUBLIC, 0);
    ws_api->class_add_method(vm, socket_class, "send", ws_socket_send, LT_VIS_PUBLIC, 0);
    ws_api->class_add_method(vm, socket_class, "close", ws_socket_close, LT_VIS_PUBLIC, 0);
    ws_api->class_add_getter(vm, socket_class, "url", ws_socket_url, LT_VIS_PUBLIC, 0);
    ws_api->class_add_getter(vm, socket_class, "closed", ws_socket_closed, LT_VIS_PUBLIC, 0);

    lt_Value server_class = ws_api->class_create(vm, "Server");
    ws_api->class_set_native_data_destroy(vm, server_class, ws_server_destroy_native);
    ws_api->class_add_method(vm, server_class, "close", ws_server_close, LT_VIS_PUBLIC, 0);
    ws_api->class_add_getter(vm, server_class, "port", ws_server_port, LT_VIS_PUBLIC, 0);

    lt_Value registry = ws_api->make_table(vm);
    ws_api->table_set(vm, registry, ws_api->make_string(vm, "WebSocket"), socket_class);
    ws_api->table_set(vm, registry, ws_api->make_string(vm, "Server"), server_class);
    ws_api->table_set(vm, vm->global, ws_api->make_string(vm, "__little_ws_classes"), registry);

    lt_Value ws = ws_api->make_table(vm);
    ws_api->table_set(vm, ws, ws_api->make_string(vm, "connect"), ws_api->make_native(vm, ws_connect));
    ws_api->table_set(vm, ws, ws_api->make_string(vm, "serve"), ws_api->make_native(vm, ws_serve));
    ws_api->table_set(vm, ws, ws_api->make_string(vm, "WebSocket"), socket_class);
    ws_api->table_set(vm, ws, ws_api->make_string(vm, "Server"), server_class);
    ws_api->table_set(vm, vm->global, ws_api->make_string(vm, "ws"), ws);
}

