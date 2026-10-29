# ws

`ws` provides WebSocket client and server support. It is opened by
`ltstd_open_all(vm)` or individually with `ltstd_open_ws(vm)`. The client
supports `ws://` and `wss://`; the server supports `ws://` and can serve
`wss://` when TLS options are supplied.

`ws.connect(url [, init])` returns a promise for a `WebSocket`. The optional
`init.headers` table adds request headers. Certificate verification for
`wss` follows the same trust configuration as the HTTP client.

`ws.serve(handler [, options])` starts a server. The handler receives
`(socket, request)`; `request` has `url`, `method`, `headers`, and `body`.
Options are `hostname`, `port`, and an optional `tls` table containing
`cert` and `key` file paths. `server.port` reports the bound port and
`server:close()` stops the listener and closes active connections.

`socket:next()` returns a promise resolving to:

- a string for a text message;
- an array of byte values (`0..255`) for a binary message;
- `null` when the connection closes normally.

Protocol and transport failures reject the promise. Only one `next()` may be
pending at a time.

`socket:send(value)` sends a text frame for a string or a binary frame for a
byte array. It returns `true` when the message is queued and `false` when the
connection is closed or the outbound queue is full. `socket:close([code [,
reason]])` starts the close handshake. The only user-supplied close codes are
`1000` and `3000..4999`.

`socket.url` and `socket.closed` are getters. Ping/pong frames are handled by
the WebSocket layer. Subprotocols, extensions, and `permessage-deflate` are not
negotiated.

Use `.next()` chains for independent readers. The current async runtime polls
`await` inline, so having both ends of a connection wait with `await` in the
same VM can stall; the promise stream is the intended concurrent API.
