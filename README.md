# Mini-Redis

A small **Redis clone built from scratch in Go** — it speaks the real Redis wire protocol (RESP) over TCP, keeps an in-memory key/value store with TTL expiry, and adds an HTTPS gateway so it can run on a free-tier cloud host and back a real application's distributed lock.

Built as **Project 2**, and it plugs directly into **Project 1 ([BookMyEvent](https://github.com/lakshmanan-2007/bookmyevent))**: BookMyEvent's booking distributed lock can run on this Mini-Redis.

**Stack:** Go (standard library only — no external dependencies) · RESP protocol · `net` TCP server · `net/http` gateway · Docker.

---

## What it does

- **Real RESP protocol** over TCP (default `:6379`) — connect with `redis-cli`, or any Redis client (Java Lettuce, Node ioredis, Python redis).
- **Commands:** `PING`, `ECHO`, `SET key value [NX] [EX s | PX ms]`, `GET`, `DEL`, `EXISTS`, `EXPIRE`, `TTL`, `INCR`/`DECR`/`INCRBY`/`DECRBY`, `KEYS`, `DBSIZE`, `FLUSHALL`, `INFO`.
- **TTL key expiry** — lazy (on access) + active (a background janitor sweeps every second), exactly like real Redis.
- **`SET ... NX PX` = a distributed lock primitive** — atomic "set if not exists" with a timeout is all you need to build a lock.
- **HTTPS gateway** (on `$PORT`) — a live dashboard, `/healthz`, `/stats`, a generic `/api/cmd`, and lock endpoints (`/api/lock/acquire`, `/api/lock/release`).

### Why an HTTP gateway on top of RESP?
Free-tier hosts (Render, etc.) route **HTTPS** to your service but do **not** expose a raw Redis TCP port to the public internet. So Mini-Redis keeps the real RESP/TCP interface (for `redis-cli` and local clients) **and** offers a tiny HTTPS lock API so a cloud app (BookMyEvent on Render) can use it as a distributed lock over the internet. Recognising that infra limit and engineering around it is itself a good systems-design talking point.

---

## How it connects to Project 1 (BookMyEvent)

BookMyEvent's booking has a pluggable `DistributedLock` with a `app.lock.provider` switch:

| provider | what it uses |
|----------|--------------|
| `db` (default) | DB pessimistic lock — single instance |
| `redis` | a real Redis via Lettuce (`SET NX PX`) |
| **`miniredis`** | **this project, over its HTTPS gateway** |

Set on the BookMyEvent host:

```
APP_LOCK_PROVIDER=miniredis
APP_MINIREDIS_URL=https://<your-miniredis>.onrender.com
```

Now every booking calls Mini-Redis to acquire `lock:event:{id}` before touching the database — **the live site's oversell safety is powered by your own Redis clone.**

The Java side is `MiniRedisHttpLock.java`; it does acquire → run booking → safe release (release only if the token still matches, so you never release a lock that already expired and was taken by someone else).

---

## Run it locally

```bash
go run .          # HTTP gateway on :8080, RESP on :6379
```

Then, with a real Redis client:

```bash
redis-cli -p 6379 PING              # PONG
redis-cli -p 6379 SET k v NX PX 5000
redis-cli -p 6379 TTL k
redis-cli -p 6379 GET k
```

Or over HTTP (open `http://localhost:8080` for the live dashboard), and the lock API:

```bash
curl -X POST "http://localhost:8080/api/lock/acquire?key=lock:event:1&ttl=10000"
# {"acquired":true,"token":"...."}
curl -X POST "http://localhost:8080/api/lock/release?key=lock:event:1&token=...."
# {"released":true}
```

### Local end-to-end with BookMyEvent
1. Run Mini-Redis (`go run .`).
2. Run BookMyEvent backend with `APP_LOCK_PROVIDER=miniredis` and `APP_MINIREDIS_URL=http://localhost:8080`.
3. Book tickets — the booking now serialises through Mini-Redis. Watch the dashboard's command counter climb.

---

## Tests

```bash
go test ./... -v
```

Covers RESP parsing (array + inline + binary-safe), `SET NX` lock semantics, safe unlock, TTL expiry, `INCR`, and a **200-goroutine race** proving exactly one caller wins a lock (the same guarantee BookMyEvent relies on to never oversell).

---

## Deploy (Render, free)

Push to GitHub, then in Render: **New → Blueprint → pick this repo → Apply** (uses `render.yaml`). Render builds the Docker image and routes HTTPS to the gateway. Health check: `/healthz`.

- Live dashboard: `https://<service>.onrender.com/`
- Health: `https://<service>.onrender.com/healthz` → `{"status":"UP"}`

---

## Design notes / interview points

- **Single-writer safety:** the store is guarded by a `RWMutex`; real Redis gets the same safety from a single-threaded event loop. Same guarantee, different mechanism — worth being able to explain both.
- **Lock correctness:** `SET NX PX` is atomic (acquire + timeout in one op); release checks the token so a caller only unlocks what it still owns; the `PX` TTL prevents a crashed holder from deadlocking everyone (auto-expiry).
- **Expiry strategy:** lazy + active, matching Redis.
- **Zero dependencies:** standard library only, so it compiles offline and ships as a tiny static binary.
- **One-line summary:** *"I built a Redis clone in Go — RESP protocol, TTL expiry, and a distributed lock — and wired my other project's booking lock to run on it."*
