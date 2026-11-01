# ws

`ws.connect(url [, init])` opens a WebSocket connection to the URL and returns a promise resolving to a ws.WebSocket.

- `url`: ws or wss URL to connect to
- `init`: table with an optional headers table

`ws.serve(handler [, options])` starts a WebSocket server that passes each accepted connection to the handler and returns a ws.Server.

- `handler`: callable receiving each accepted ws.WebSocket
- `options`: table with optional hostname, port, and tls fields, where tls holds cert and key paths

`ws.WebSocket()` represents a WebSocket connection exposing next(), send(), close(), url, and closed; it can be used as an async for source.

`ws.Server()` represents a running WebSocket server exposing close() and the bound port.
