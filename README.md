# Mini-Redis (C)

A small **Redis clone written from scratch in C** — it speaks the real Redis wire protocol (RESP) over TCP, keeps an in-memory key/value store with TTL expiry, and adds an HTTPS gateway so it can run on a free-tier cloud host and back a real application's distributed lock.

Built as **Project 2**, and it plugs directly into **Project 1 ([BookMyEvent](https://github.com/lakshmanan-2007/bookmyevent))**: BookMyEvent's booking distributed lock runs on this Mini-Redis.

**Stack:** C (C11) · POSIX sockets · pthreads · standard library only (no external deps) · Docker (Alpine).

---

## What it does

- **Real RESP protocol** over TCP (default `:6379`) — connect with `redis-cli` or any Redis client (Java Lettuce, Node ioredis, Python redis).
- **Commands:** `PING`, `ECHO`, `SET key value [NX] [EX s | PX ms]`, `GET`, `DEL`, `EXISTS`, `EXPIRE`, `TTL`, `INCR`/`DECR`/`INCRBY`/`DECRBY`, `KEYS`, `DBSIZE`, `FLUSHALL`, `INFO`.
- **TTL key expiry** — lazy (on access) + active (a background sweeper thread), like real Redis.
- **`SET ... NX PX` = a distributed-lock primitive** — atomic "set if absent" + timeout + safe token-checked release.
- **HTTPS gateway** (on `$PORT`) — a live dashboard, `/healthz`, `/stats`, `/api/cmd`, and lock endpoints (`/api/lock/acquire`, `/api/lock/release`).

### Why an HTTP gateway on top of RESP?
Free-tier hosts (Render) route **HTTPS** to your service but do **not** expose a raw Redis TCP port publicly. So Mini-Redis keeps the real RESP/TCP interface (for `redis-cli` / local clients) **and** offers a tiny HTTPS lock API so a cloud app (BookMyEvent on Render) can use it as a distributed lock over the internet.

---

## Architecture (C)

- **`dict.c`** — separate-chaining hash table (FNV-1a), string→value with an expiry timestamp.
- **`store.c`** — thread-safe store: one `pthread_mutex` guards the dict; TTL semantics (lazy + `store_sweep`); stats.
- **`util.c`** — `now_ms`, full-write, random token (`/dev/urandom`), URL-decode, a buffered socket reader (`rio`).
- **`proto.c`** — RESP: parse a command (array or inline), reply writers (`+OK`, `-ERR`, `:n`, `$bulk`, `$-1`), command dispatch.
- **`http.c`** — minimal HTTP/1.1 parser + routes + JSON + the dashboard (`dashboard.h`).
- **`main.c`** — a listener for RESP and one for HTTP, **thread-per-connection** (a handler thread per client), plus the sweeper thread.

> **Concurrency model:** thread-per-connection with a single mutex around the store. Real Redis instead uses a single-threaded event loop — same correctness guarantee, different mechanism. (Both are worth being able to explain.)

---

## How it connects to Project 1 (BookMyEvent)

BookMyEvent's booking has a pluggable `DistributedLock` with an `app.lock.provider` switch: `db` / `redis` / **`miniredis`**. Set on the BookMyEvent host:

```
APP_LOCK_PROVIDER=miniredis
APP_MINIREDIS_URL=https://<your-miniredis>.onrender.com
```

Every booking then calls Mini-Redis to acquire `lock:event:{id}` (via the HTTP gateway) before touching the database — **the live site's oversell safety is powered by your own Redis clone.** The HTTP API contract is identical to the earlier build, so swapping the implementation to C required **no change** in BookMyEvent.

---

## Build & run locally (Linux / WSL / macOS)

```bash
make            # or: gcc -O2 -pthread -o miniredis dict.c store.c util.c proto.c http.c main.c
./miniredis     # HTTP gateway on :8080, RESP on :6379
```

Then:

```bash
redis-cli -p 6379 PING                 # PONG
redis-cli -p 6379 SET k v NX PX 5000
redis-cli -p 6379 TTL k

curl http://localhost:8080/healthz     # {"status":"UP"}
curl -X POST "http://localhost:8080/api/lock/acquire?key=lock:event:1&ttl=10000"
```

Open `http://localhost:8080/` for the live dashboard.

> Note: uses POSIX sockets + epoll-era APIs, so it builds/runs on Linux (and macOS). On Windows, use WSL or just run it via Docker.

---

## Tested

Verified locally: RESP parsing, `SET NX` lock semantics + safe token release, TTL expiry, `INCR`, `/api/cmd`, and a **60-thread concurrency race** — exactly one caller wins a contested lock (the same guarantee BookMyEvent relies on to never oversell).

---

## Deploy (Render, free)

Push to GitHub, then in Render: **New → Web Service → this repo → Docker → Free**. The Dockerfile compiles the C with `gcc` (Alpine) into a tiny image; Render routes HTTPS to the gateway (`/healthz` health check).

- Live dashboard: `https://<service>.onrender.com/`
- Health: `https://<service>.onrender.com/healthz` → `{"status":"UP"}`

---

## Design / interview points

- **Manual memory + sockets:** `malloc`/`free` for entries, `SO_REUSEADDR`, `bind`/`listen`/`accept`, buffered reads.
- **Lock correctness:** `SET NX PX` is atomic (acquire + timeout); release checks the token (only unlock what you own); the TTL prevents a crashed holder from deadlocking everyone.
- **Expiry strategy:** lazy on access + active sweeper — matching Redis.
- **Robustness:** `SIGPIPE` ignored (a client disconnecting mid-write can't kill the server); bounded buffers on all parsers.
- **Zero dependencies:** C standard library + POSIX only → compiles anywhere, ships as a tiny static-ish Alpine image.
- **One-liner:** *"I wrote a Redis clone in C — RESP protocol, TTL expiry, a distributed lock, HTTP gateway — and my booking app's lock runs on it live."*
