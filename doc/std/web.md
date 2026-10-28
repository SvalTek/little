# web

The `web` module provides HTTP requests and a local HTTP server. It is opened by
`ltstd_open_all(vm)` and can also be opened individually with
`ltstd_open_web(vm)`. `web.fetch` accepts `http://` and `https://` URLs.
HTTPS verifies the server certificate against Windows' certificate store on
Windows and OpenSSL's default trust paths on other platforms. It also checks
that the certificate matches the URL's hostname or IP address.
Certificate verification and TLS handshake failures reject the fetch promise.
OpenSSL's `SSL_CERT_FILE` and `SSL_CERT_DIR` environment variables can select
the trusted CA certificates for the process. On Windows, set `SSL_CERT_FILE`
to use OpenSSL's trust paths instead of the Windows certificate store. Point
these variables to CA certificates, not private keys. `web.serve` serves plain
HTTP.

`web.fetch(url [, init])` returns a promise for a `Response`. The optional
`init` table can contain `method`, `headers`, and `body`. The default method is
`GET`; headers are a table of string names and values, and the body is a
string. Network failures reject the promise.

Responses expose `status`, `statusText`, `ok`, `url`, `headers`, and `body`.
`ok` is true for status codes from 200 through 299. `response:text()` returns a
promise for the body string.

`web.Response([body [, options]])` constructs a response. The body is a string
and defaults to empty. The options table accepts `status` (an integer from 100
to 599, default 200), `statusText` (a string without carriage returns or
newlines), and `headers` (a table). If `statusText` is omitted, a standard
reason phrase is used for recognized statuses; otherwise it is empty. The
constructed response's `url` is empty.

```js
var response = web.Response("created", {
    status: 201
    statusText: "Created"
    headers: { "Content-Type": "text/plain" }
})
```

`web.serve(handler [, options])` starts a server and returns a `Server`.
The handler receives a `Request` with `url`, `method`, `headers`, and `body`
properties. The options table can specify `hostname` and `port`; the defaults
are `0.0.0.0` and `8000`. A handler can return a string as its response body,
a `web.Response`, or a promise that resolves to either. A rejected handler
promise produces a 500 response.
`server:close()` stops the server and its active connections.

Each server handles up to 8 clients at once. Additional connections can wait in
the listen backlog until a client slot is free, subject to backlog capacity.
Each request has a 65,535-byte
limit for the request line, headers, header terminator, and body combined. The
server closes a connection whose request exceeds this limit.

Web operations are advanced by the async event loop. A script can use
`mainloop.run()` or `mainloop.poll()` to advance pending work; see
[async](async.md). Close servers when finished so they do not keep the loop
active.
