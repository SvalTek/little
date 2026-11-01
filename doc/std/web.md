# web

`web.fetch(url [, init])` requests a URL and returns a promise resolving to a web.Response.

- `url`: absolute http or https URL to request
- `init`: table with optional method, body, headers, redirects, proxy, and decompress fields

`web.fetchStream(url [, init])` requests a URL and returns a promise resolving to a web.Stream once response headers are available.

- `url`: absolute http or https URL to request
- `init`: table with optional method, body, headers, redirects, proxy, decompress, and binary fields

`web.serve(handler [, options])` starts an HTTP server that passes each request to the handler and returns a web.Server.

- `handler`: callable receiving a web.Request and returning a web.Response or a promise of one
- `options`: table with optional port and hostname fields

`web.Request()` represents an incoming server request exposing url, method, body, and headers.

`web.Response([body [, options]])` represents an HTTP response with a status, headers, and body.

- `body`: response body string
- `options`: table with optional status, statusText, and headers fields

`web.Server()` represents a running HTTP server exposing close() and the bound port.

`web.Stream()` represents a streaming HTTP client response returned by fetchStream, exposing next(), close(), status, statusText, ok, url, headers, and closed.
