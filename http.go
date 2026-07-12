package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"
)

// The HTTP layer exists for one practical reason: managed free-tier hosts
// (Render, etc.) route HTTP/HTTPS to your service but do NOT expose a raw TCP
// port to the public internet. So Mini-Redis speaks real RESP over TCP for
// local/Redis-client use, AND offers this small HTTPS gateway so a cloud app
// (BookMyEvent on Render) can use it as a distributed lock over the internet.

func newToken() string {
	b := make([]byte, 12)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func httpMux(store *Store) *http.ServeMux {
	mux := http.NewServeMux()

	// Liveness — mirrors the actuator /health idea in Project 1.
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]string{"status": "UP"})
	})

	// Machine-readable stats.
	mux.HandleFunc("/stats", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, store.Stats())
	})

	// Distributed-lock ACQUIRE: SET key <token> NX PX ttl.
	// Returns {"acquired":true,"token":"..."} or {"acquired":false}.
	mux.HandleFunc("/api/lock/acquire", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			writeJSON(w, 405, map[string]string{"error": "use POST"})
			return
		}
		key := r.URL.Query().Get("key")
		if key == "" {
			writeJSON(w, 400, map[string]string{"error": "key required"})
			return
		}
		ttlMs := int64(10000)
		if v := r.URL.Query().Get("ttl"); v != "" {
			if n, err := strconv.ParseInt(v, 10, 64); err == nil && n > 0 {
				ttlMs = n
			}
		}
		store.countCommand()
		token := newToken()
		ok := store.Set(key, token, time.Duration(ttlMs)*time.Millisecond, true)
		if ok {
			writeJSON(w, 200, map[string]any{"acquired": true, "token": token})
		} else {
			writeJSON(w, 200, map[string]any{"acquired": false})
		}
	})

	// Distributed-lock RELEASE: delete key only if token still matches (safe unlock).
	mux.HandleFunc("/api/lock/release", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			writeJSON(w, 405, map[string]string{"error": "use POST"})
			return
		}
		key := r.URL.Query().Get("key")
		token := r.URL.Query().Get("token")
		store.countCommand()
		released := store.DelIfValue(key, token)
		writeJSON(w, 200, map[string]any{"released": released})
	})

	// Generic command endpoint for the dashboard console and quick demos.
	mux.HandleFunc("/api/cmd", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			writeJSON(w, 405, map[string]string{"error": "use POST"})
			return
		}
		var body struct {
			Args []string `json:"args"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil || len(body.Args) == 0 {
			writeJSON(w, 400, map[string]string{"error": "send {\"args\":[...]}"})
			return
		}
		store.countCommand()
		writeJSON(w, 200, httpExec(body.Args, store))
	})

	// Live dashboard.
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		fmt.Fprint(w, dashboardHTML(store))
	})

	return mux
}

// httpExec runs a small subset of commands and returns a JSON-friendly result.
func httpExec(args []string, store *Store) map[string]any {
	cmd := strings.ToUpper(args[0])
	switch cmd {
	case "PING":
		return map[string]any{"result": "PONG"}
	case "SET":
		if len(args) < 3 {
			return map[string]any{"error": "SET needs key value"}
		}
		var ttl time.Duration
		nx := false
		for i := 3; i < len(args); i++ {
			switch strings.ToUpper(args[i]) {
			case "NX":
				nx = true
			case "EX":
				if i+1 < len(args) {
					n, _ := parseInt(args[i+1])
					ttl = time.Duration(n) * time.Second
					i++
				}
			case "PX":
				if i+1 < len(args) {
					n, _ := parseInt(args[i+1])
					ttl = time.Duration(n) * time.Millisecond
					i++
				}
			}
		}
		ok := store.Set(args[1], args[2], ttl, nx)
		if ok {
			return map[string]any{"result": "OK"}
		}
		return map[string]any{"result": nil}
	case "GET":
		if len(args) != 2 {
			return map[string]any{"error": "GET needs key"}
		}
		if v, ok := store.Get(args[1]); ok {
			return map[string]any{"result": v}
		}
		return map[string]any{"result": nil}
	case "DEL":
		if len(args) != 2 {
			return map[string]any{"error": "DEL needs key"}
		}
		return map[string]any{"result": store.Del(args[1])}
	case "TTL":
		if len(args) != 2 {
			return map[string]any{"error": "TTL needs key"}
		}
		return map[string]any{"result": store.TTL(args[1])}
	case "INCR":
		if len(args) != 2 {
			return map[string]any{"error": "INCR needs key"}
		}
		n, _ := store.IncrBy(args[1], 1)
		return map[string]any{"result": n}
	case "KEYS":
		return map[string]any{"result": store.Keys()}
	default:
		return map[string]any{"error": "unsupported over HTTP: " + cmd}
	}
}

func infoText(store *Store) string {
	s := store.Stats()
	return fmt.Sprintf(
		"# Server\r\nname:mini-redis\r\nversion:1.0.0\r\nuptime_in_seconds:%d\r\n"+
			"# Stats\r\ncommands_processed:%d\r\nkeyspace_hits:%d\r\nkeyspace_misses:%d\r\n"+
			"expired_keys:%d\r\ndb0:keys=%d\r\n",
		s.UptimeSeconds, s.Commands, s.Hits, s.Misses, s.Expired, s.Keys)
}
