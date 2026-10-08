# TCPinCPP

A single-threaded HTTP/1.1 server for Linux, written from scratch in C++ on top
of raw TCP sockets. It serves many clients at once using **epoll** (no threads,
no fork, no libraries). Clients send HTTP requests and get HTTP responses back;
connections stay open (keep-alive) so one client can send many requests.

```
$ make && ./server                    # listens on port 8080
$ curl -i localhost:8080/             # in another terminal
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 20

hello from TCPinCPP
```

Or by hand, to see the raw bytes (`-C` makes `nc` send `\r\n`):

```
$ nc -C localhost 8080
GET /nope HTTP/1.1
Host: x

HTTP/1.1 404 Not Found
Content-Type: text/plain
Content-Length: 17

not found: /nope
```

### Routes

| Request | Response |
|---|---|
| `GET /` | `200 OK`, body `hello from TCPinCPP` |
| `GET` anything else | `404 Not Found`, body `not found: <path>` |
| any other method | `405 Method Not Allowed` with an `Allow: GET` header |
| malformed request | `400 Bad Request`, then the connection is closed |
| headers larger than 8 KB | `431 Request Header Fields Too Large`, then closed |

---

## Project layout

| File | What it is |
|---|---|
| `main.cpp` | Entry point. Creates an `EventLoop` on port 8080 and calls `run()`. |
| `EventLoop.h/.cpp` | The server engine: owns the listening socket and epoll, and decides who to read from or write to. |
| `Connection.h/.cpp` | One object per connected client: its socket, its buffers, framing, and routing requests to responses. |
| `http.h/.cpp` | Pure HTTP code with no sockets: `HttpRequest`, `HttpResponse`, the request parser, and the response serializer. |
| `Makefile` | Builds `server` with `g++ -Wall -Wextra -g`. `make clean` deletes it. |
| `Approaches.md` | Working notes on turning the TCP server into an HTTP server. |
| `learn/` | Older versions of the server and notes, kept locally to show how the design evolved (git-ignored; see the end of this file). |

---

## How it all connects

```
main()
  └─ EventLoop loop(8080)
       ├─ setupListenSocket()  socket → bind → listen
       ├─ epoll_create1()      create the "watch list"
       ├─ setupSignals()       Ctrl+C becomes a readable fd
       └─ run()  ── loop forever ──────────────────────────────┐
             epoll_wait()  "which fds are ready?"              │
               ├─ listen fd ready  → handleAccept()            │
               │     accept new clients, make a Connection     │
               ├─ signal fd ready  → handleSignal()            │
               │     start graceful shutdown                   │
               └─ client fd ready  → handleClientEvent()       │
                     Connection::onReadable()  read + parse    │
                       parseRequest()  → handleRequest()       │
                       serializeResponse() → outbuf_           │
                     Connection::flush()       send replies    │
                     updateInterest()          adjust epoll    │
             closeIdleConnections()  drop silent clients ──────┘
```

The idea: one thread asks the kernel "which sockets have something for me?",
handles only those, and goes back to waiting. Nothing ever blocks, so one slow
client can't stall the others.

`EventLoop` stores clients in `std::unordered_map<int, Connection> conns_`,
keyed by file descriptor. Erasing an entry runs `~Connection()`, which closes
the socket. That's RAII: the fd lives exactly as long as the object.

---

## EventLoop — function by function

| Function | Job |
|---|---|
| `EventLoop(port)` | Sets up the listen socket, epoll, and signals, and reserves a spare fd. |
| `~EventLoop()` | Closes every fd it owns (clients first, via `conns_.clear()`). |
| `setupListenSocket()` | `socket()` → `setsockopt(SO_REUSEADDR)` → `bind()` → `listen()`. |
| `addFd / modFd / removeFd` | Thin wrappers around `epoll_ctl` ADD / MOD / DEL. `removeFd` also erases the Connection. |
| `handleAccept()` | Accepts every waiting client in a loop until `EAGAIN`, makes each one non-blocking, and registers it. |
| `handleClientEvent()` | For one ready client: on error/hangup it closes; otherwise it reads, flushes, and updates interest. |
| `updateInterest()` | Recomputes whether we care about reads and/or writes for a client, and calls `epoll_ctl` only if that changed. |
| `setupSignals()` / `handleSignal()` | Turns SIGINT/SIGTERM into an fd. The first Ctrl+C starts a graceful shutdown; a second one forces it. |
| `beginShutdown()` | Stops accepting, queues the text `ERR server shutting down` to every client (a leftover from the old line protocol, not an HTTP response), and waits up to 5 s for them to receive it. |
| `closeIdleConnections()` | About once a second, drops clients that haven't sent or received anything for 30 s. |
| `run()` | The main loop shown above. |

## Connection — function by function

| Function | Job |
|---|---|
| `onReadable()` | Reads until `EAGAIN` into `inbuf_`, then parses. Returns `Drained`, `Paused` (output full), or `Closed` (error). |
| `processMessages()` | Calls `parseRequest()` repeatedly to pull every complete request out of `inbuf_`, then erases the used bytes and keeps any partial request for next time. A bad request gets a 400; a header block over 8 KB gets a 431. |
| `handleRequest()` | The router: picks the status and body for a parsed request (see *Routes* above). |
| `sendResponse()` | Serializes an `HttpResponse` and queues it with `reply()`. |
| `sendError()` | For framing errors: sends a response with `Connection: close` and moves to `Closing`, because we no longer know where the next request starts. |
| `reply()` | Appends raw bytes to `outbuf_`. Nothing is sent yet. |
| `flush()` | Sends as much of `outbuf_` as the kernel will take and keeps the rest. Returns `false` on a real error. |
| `wantsRead / wantsWrite / done` | Small questions `EventLoop` asks to decide what to watch for and when to close. |
| `beginClose()` | Queues a final message and stops reading; the connection closes once that message has been sent. |

**States:** `Open` means reading and replying. `Closing` means no more reading;
the connection is closed as soon as `outbuf_` is empty. A client closing its
side (`read()` returns 0) also moves to `Closing`, so replies already queued
still get sent.

## http — function by function

| Function / type | Job |
|---|---|
| `HttpRequest` | `method`, `path`, `version`, and `headers` (names lower-cased, values trimmed). |
| `HttpResponse` | `status` (default 200), an ordered list of `headers`, and `body`. `setHeader()` appends one. |
| `parseRequest(buf, start, req, consumed)` | Looks for the blank line (`\r\n\r\n`) that ends the headers. Returns `Incomplete` if it isn't there yet, `Bad` if the request line or a header is malformed, or `Ok` with `consumed` set to the request's length. |
| `statusText()` | Maps a code to its reason phrase (`404` → `Not Found`). |
| `serializeResponse()` | Builds the status line, headers, `Content-Length` (always computed from the body, so it can't be wrong), the blank line, and the body. |

### Current HTTP limitations
- Only `GET` is routed; request bodies are not read (`Content-Length` /
  chunked bodies on requests are ignored).
- The client's `Connection: close` header and HTTP/1.0 are not special-cased:
  every connection is kept alive until the client closes it or it goes idle.
- No static file serving yet; responses are hard-coded in `handleRequest()`.

---

## The "not normal C++" parts (Linux system calls, flags, and ideas)

### Sockets
- **`socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)`**: creates a TCP socket.
  `AF_INET` means IPv4, `SOCK_STREAM` means TCP, and `SOCK_NONBLOCK` makes calls
  return right away instead of waiting.
- **`setsockopt(SO_REUSEADDR)`**: lets you restart the server right away
  instead of getting "Address already in use" for about a minute.
- **`sockaddr_in`, `INADDR_ANY`, `htons(port)`**: the address to bind to.
  `INADDR_ANY` means every network interface. `htons` converts the port to
  network byte order (big-endian).
- **`listen(fd, SOMAXCONN)`**: starts accepting connections. `SOMAXCONN` is the
  largest queue of waiting connections the OS allows.
- **`accept()`**: takes the next waiting client and returns a new fd for it.

### File descriptors (fd)
An `int` that names an open file, socket, pipe, or similar in the kernel. In
Linux, "everything is a file", so the same `read`/`write`/`close` work on all of them.

### Non-blocking I/O
- **`fcntl(fd, F_GETFL)` / `F_SETFL | O_NONBLOCK`**: reads the fd's flags, adds
  "non-blocking", and writes them back. Accepted client sockets don't inherit
  this from the listen socket, so `setNonBlocking()` sets it on each one.
- **`EAGAIN` / `EWOULDBLOCK`**: "nothing to do right now, try later". This isn't
  an error; it means you've read or written everything you can for now.
- **`EINTR`**: a signal interrupted the call, so just retry it.
- **`errno`**: a global variable where system calls store why they failed.
  `perror("x")` prints it.

### epoll: the core of the server
- **`epoll_create1(0)`**: creates a kernel "watch list" and gives you its fd.
- **`epoll_ctl(ADD/MOD/DEL)`**: adds, changes, or removes an fd on the watch list.
- **`epoll_wait(epfd, events, MAX_EVENTS, 1000)`**: sleeps until some fds are
  ready or 1000 ms pass, and returns only the ready ones. The timeout is what
  lets the idle sweep run even when nothing is happening.
- **Event flags:**
  - `EPOLLIN`: data to read (or a new client, on the listen socket)
  - `EPOLLOUT`: the socket can accept more outgoing data
  - `EPOLLERR` / `EPOLLHUP`: error or hangup
  - `EPOLLET`: **edge-triggered**. epoll tells you only when *new* data
    arrives, not while data is sitting there. That's why every read and accept
    loops until `EAGAIN`; if you stop early, you won't be woken again. The
    `rearm` flag in `updateInterest()` handles that case: `EPOLL_CTL_MOD` makes
    epoll check the fd again.
  - The signal fd is **level-triggered** (no `EPOLLET`) on purpose: epoll keeps
    reporting it while signals are pending.

### Backpressure
If a client sends requests faster than it reads the responses, `outbuf_` grows.
Once it reaches 64 KB (`MAX_OUTBUF`), the server stops reading from that client
(`wantsRead()` becomes false and `EPOLLIN` is dropped). Unread data stays in the
kernel until the client catches up. `EPOLLOUT` is only watched while there is
something to send; otherwise epoll would wake you constantly for nothing.

### Framing
TCP is a byte stream, not a stream of messages. One `read()` can hold half a
request or three pipelined requests. `inbuf_` collects bytes, and
`processMessages()` cuts them at each `\r\n\r\n` (the end of an HTTP header
block). `parseRequest()` takes a `start` offset so several requests can be
parsed from one buffer before erasing them all in one go.

### Partial writes
`send()` may accept only part of your buffer. `flush()` erases what was sent
and keeps the rest for the next `EPOLLOUT`.

### `send(..., MSG_NOSIGNAL)`
Normally, writing to a socket the peer has closed sends your process `SIGPIPE`,
which kills it. With this flag you get an `EPIPE` error instead.

### signalfd: Ctrl+C without signal handlers
- **`sigprocmask(SIG_BLOCK, ...)`**: stops SIGINT and SIGTERM from being
  delivered the normal way (as an interrupt).
- **`signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC)`**: turns those signals
  into an fd you can `read()`. epoll watches it like any other fd, so shutdown
  happens safely inside the loop instead of interrupting you partway through
  something.
- **`strsignal()`**: gives the signal's name as text, for printing.

### `O_CLOEXEC` / `SFD_CLOEXEC`
"Close this fd if the process ever calls `exec`." This stops fds leaking into
child programs.

### The spare fd trick (`EMFILE`)
When the process runs out of fds, `accept()` fails with `EMFILE`, but the client
stays in the queue. With edge triggering, epoll won't wake you for it again, so
the server gets stuck. The fix: keep one fd reserved on `/dev/null`. When you
hit `EMFILE`, close it, accept the client, close the client right away, and
reserve the fd again.

### C++ bits worth knowing
- **`#pragma once`**: include this header only once per compile.
- **`= delete` on the copy constructor and `operator=`**: the object can't be
  copied. A copy would close the same fd twice.
- **`explicit`**: blocks accidental conversions like `EventLoop e = 8080;`.
- **`try_emplace(fd, fd)`**: builds the `Connection` directly inside the map,
  which is needed because it can't be copied.
- **`enum class`**: a scoped enum (`State::Open`), which won't mix up with plain ints.
- **`std::chrono::steady_clock`**: a clock that never jumps backwards, which is
  what you want for timeouts.
- **Anonymous `namespace { }`**: the helper inside is private to that .cpp file.
- **`#define` constants**: `MAX_EVENTS`, `IDLE_TIMEOUT_S`, `MAX_OUTBUF`, and so on.
  `IDLE_TIMEOUT_S` is wrapped in `#ifndef`, so you can override it at build
  time, e.g. `g++ -DIDLE_TIMEOUT_S=5 ...` to test the idle timeout quickly.

---

## Tunable limits

| Constant | Value | Where | Meaning |
|---|---|---|---|
| `PORT` | 8080 | main.cpp | listening port |
| `MAX_EVENTS` | 32 | EventLoop.cpp | most events handled per `epoll_wait` |
| `SWEEP_INTERVAL_MS` | 1000 | EventLoop.cpp | how often idle clients are checked |
| `IDLE_TIMEOUT_S` | 30 | EventLoop.cpp | how long a silent client is kept |
| `SHUTDOWN_GRACE_S` | 5 | EventLoop.cpp | how long shutdown waits for clients |
| `BUFFER_SIZE` | 1024 | Connection.cpp | bytes per `read()` |
| `MAX_OUTBUF` | 64 KB | Connection.cpp | backpressure threshold |
| `MAX_HEADER` | 8192 | Connection.cpp | largest unfinished header block before a 431 |

---

## `learn/`: how the project got here

Each file is a standalone earlier version, following the git history:

1. **`server.cpp` + `child.cpp`**: blocking server that `fork()`s and `execl()`s
   a child program for each client. `signal(SIGCHLD, SIG_IGN)` cleans up dead
   children automatically.
2. **`server_thread.cpp`**: one `std::thread` per client, detached (compile with `-pthread`).
3. **`server_nonBlocking.cpp`**: one thread for all clients using `select()`
   (`FD_ZERO`/`FD_SET`). Simple, but capped at 1024 fds and it rescans every fd.
4. **`server_nonBlocking_poll.cpp`**: `poll()` with a `std::vector<pollfd>`.
   No fd cap, but still rescans the whole list.
5. **`server_epoll.cpp`**: `epoll`, where the kernel returns only the ready fds.
   This became the root `EventLoop`.

`LEARNING.md`, `Notes.md`, and `Tasks.md` are the study notes and roadmap.
