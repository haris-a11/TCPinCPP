# TCPinCPP

A single-threaded HTTP/1.1 server for Linux, written in C++ using raw TCP sockets and `epoll`. It handles multiple clients concurrently without threads, processes, or external libraries, and supports persistent connections (keep-alive).

## Getting Started

Build and run the server:

```bash
make
./server
```

The server listens on port `8080`.

Test it with curl:

```bash
curl -i localhost:8080/
```

Expected response:

```http
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 20

hello from TCPinCPP
```

Test a request manually:

```bash
nc -C localhost 8080
```

Enter:

```http
GET /nope HTTP/1.1
Host: localhost

```

The server responds with `404 Not Found`.

To stop the server, press `Ctrl+C`.

## Features

- **Single-threaded concurrency:** Uses `epoll` to handle multiple clients in one event loop.
- **Non-blocking I/O:** Prevents slow clients from blocking other connections.
- **HTTP/1.1 keep-alive:** Supports multiple requests over the same connection.
- **Request parsing:** Handles incomplete requests and multiple pipelined requests.
- **Partial I/O handling:** Manages incomplete reads and writes.
- **Backpressure:** Pauses reading when the outgoing buffer reaches 64 KB.
- **Resource management:** Uses RAII to manage socket lifetimes.
- **Graceful shutdown:** Handles `SIGINT` and `SIGTERM` using `signalfd`.
- **Connection cleanup:** Closes idle connections after 30 seconds.
- **Error handling:** Handles malformed requests, oversized headers, and socket errors.

## Routes

| Request | Response |
|---|---|
| `GET /` | `200 OK` — `hello from TCPinCPP` |
| `GET /anything-else` | `404 Not Found` |
| Any other method | `405 Method Not Allowed` with `Allow: GET` |
| Malformed request | `400 Bad Request`, then close |
| Headers exceeding 8 KB | `431 Request Header Fields Too Large`, then close |

## Project Structure

| File | Description |
|---|---|
| `main.cpp` | Entry point; starts the server on port 8080. |
| `EventLoop.h/.cpp` | Manages the listening socket, `epoll`, signals, and client connections. |
| `Connection.h/.cpp` | Handles client sockets, request buffers, parsing flow, and responses. |
| `http.h/.cpp` | Defines HTTP requests and responses, parsing, routing, and serialization logic. |
| `Makefile` | Builds the server using `g++`. |

## Architecture

The server uses an event-driven architecture:

1. **Initialize:** Create the listening socket, configure `epoll`, and register signal handling.
2. **Accept clients:** Accept incoming connections and configure their sockets as non-blocking.
3. **Wait for events:** Use `epoll_wait()` to identify sockets ready for reading or writing.
4. **Process requests:** Read available bytes, parse complete HTTP requests, and generate responses.
5. **Send responses:** Write responses when the socket is ready, preserving any unsent bytes.
6. **Manage connections:** Update event registrations and close completed or idle connections.

Each client has a `Connection` object stored in an `unordered_map`, indexed by its file descriptor. Destroying the object closes its socket automatically.

## Key Concepts

### TCP Sockets
The server uses Linux socket system calls:

- `socket()` creates a TCP socket.
- `bind()` assigns an address and port.
- `listen()` enables incoming connections.
- `accept()` creates a socket for each client.
- `recv()` and `send()` receive and transmit data.
- `close()` releases a file descriptor.

### Non-blocking I/O

Client sockets are non-blocking, so read and write operations return immediately when they cannot proceed.

- `EAGAIN` / `EWOULDBLOCK`: No more data can be processed right now.
- `EINTR`: The operation was interrupted by a signal.
- `MSG_NOSIGNAL`: Prevents `SIGPIPE` when sending to a closed connection.

### epoll

`epoll` monitors file descriptors and reports which ones are ready for I/O.

- `epoll_ctl()`: Adds, modifies, or removes monitored descriptors.
- `epoll_wait()`: Waits for ready events.
- `EPOLLIN`: Ready for reading.
- `EPOLLOUT`: Ready for writing.
- `EPOLLERR` / `EPOLLHUP`: Error or hangup.

The server uses edge-triggered mode for client sockets and processes available data until `EAGAIN`.

### HTTP Parsing and Framing

TCP provides a byte stream, not complete messages. A request may arrive in multiple reads, or several requests may arrive together.

The server stores incoming bytes in a buffer and parses complete HTTP headers using `\r\n\r\n`. Incomplete requests remain buffered until more data arrives.

### Partial Writes and Backpressure

A `send()` call may transmit only part of a response. The remaining bytes stay in the output buffer until the socket becomes writable again.

When the output buffer reaches 64 KB, the server pauses reading from that client until the buffer has enough space.

### Connection States

- `Open`: The server can read requests and send responses.
- `Closing`: The server stops accepting new requests and closes the connection after pending output is sent.

Malformed requests receive an error response followed by connection closure.

## Configuration

| Constant | Default | Description |
|---|---:|---|
| `PORT` | `8080` | Listening port |
| `MAX_EVENTS` | `32` | Maximum events processed per `epoll_wait()` |
| `SWEEP_INTERVAL_MS` | `1000` | Interval between idle connection checks |
| `IDLE_TIMEOUT_S` | `30` | Idle connection timeout |
| `SHUTDOWN_GRACE_S` | `5` | Shutdown grace period |
| `BUFFER_SIZE` | `1024` | Size of each read buffer |
| `MAX_OUTBUF` | `64 KB` | Maximum output buffer threshold |
| `MAX_HEADER` | `8192` | Maximum allowed header size |

## Current Limitations

- Only `GET` requests are routed.
- Request bodies, including `Content-Length` and chunked bodies, are not supported.
- `Connection: close` and HTTP/1.0 behavior are not implemented.
- Responses are hard-coded; static file serving is not available.
- The server does not use TLS/HTTPS.

## Build Commands

```bash
make          # Build the server
./server      # Start the server
make clean    # Remove the compiled binary
```

The project is intended to explore low-level networking, Linux system calls, non-blocking I/O, event-driven concurrency, HTTP request parsing, and resource management in C++.