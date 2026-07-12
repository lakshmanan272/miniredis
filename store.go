package main

import (
	"sync"
	"time"
)

// entry is a single stored value with an optional expiry time.
type entry struct {
	value     string
	expiresAt time.Time // zero == never expires
}

func (e entry) expired(now time.Time) bool {
	return !e.expiresAt.IsZero() && now.After(e.expiresAt)
}

// Store is an in-memory, thread-safe key/value store with TTL support.
// It is the heart of Mini-Redis — the same data structure a real Redis keeps
// in memory, guarded here by a RWMutex instead of Redis's single event loop.
type Store struct {
	mu   sync.RWMutex
	data map[string]entry

	// stats
	startedAt time.Time
	commands  uint64
	hits      uint64
	misses    uint64
	expiredN  uint64
}

func NewStore() *Store {
	return &Store{
		data:      make(map[string]entry),
		startedAt: time.Now(),
	}
}

// StartJanitor runs active expiry: periodically sweeps and removes expired keys
// (Redis does the same — a mix of lazy expiry on access + a background cycle).
func (s *Store) StartJanitor(every time.Duration, stop <-chan struct{}) {
	go func() {
		t := time.NewTicker(every)
		defer t.Stop()
		for {
			select {
			case <-stop:
				return
			case <-t.C:
				s.sweep()
			}
		}
	}()
}

func (s *Store) sweep() {
	now := time.Now()
	s.mu.Lock()
	for k, e := range s.data {
		if e.expired(now) {
			delete(s.data, k)
			s.expiredN++
		}
	}
	s.mu.Unlock()
}

// Set stores key=value. ttl <= 0 means no expiry. If nx is true, the write only
// happens when the key does not already exist (this is what SET NX gives us, and
// it is exactly the primitive a distributed lock is built on). Returns whether
// the value was written.
func (s *Store) Set(key, value string, ttl time.Duration, nx bool) bool {
	now := time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()
	if nx {
		if e, ok := s.data[key]; ok && !e.expired(now) {
			return false // key already held
		}
	}
	var exp time.Time
	if ttl > 0 {
		exp = now.Add(ttl)
	}
	s.data[key] = entry{value: value, expiresAt: exp}
	return true
}

// Get returns the value and whether it was present (and not expired).
func (s *Store) Get(key string) (string, bool) {
	now := time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()
	e, ok := s.data[key]
	if !ok || e.expired(now) {
		if ok { // lazy expiry
			delete(s.data, key)
			s.expiredN++
		}
		s.misses++
		return "", false
	}
	s.hits++
	return e.value, true
}

// Del removes a key. Returns true if it existed.
func (s *Store) Del(key string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.data[key]; ok {
		delete(s.data, key)
		return true
	}
	return false
}

// DelIfValue removes key only if its current value equals want. This is the
// SAFE unlock a distributed lock needs: you must only release a lock you still
// own, never one that already expired and was taken by someone else.
func (s *Store) DelIfValue(key, want string) bool {
	now := time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()
	e, ok := s.data[key]
	if !ok || e.expired(now) || e.value != want {
		return false
	}
	delete(s.data, key)
	return true
}

// Exists reports whether key is present and unexpired.
func (s *Store) Exists(key string) bool {
	_, ok := s.Get(key)
	return ok
}

// Expire sets a TTL on an existing key. Returns true if the key existed.
func (s *Store) Expire(key string, ttl time.Duration) bool {
	now := time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()
	e, ok := s.data[key]
	if !ok || e.expired(now) {
		return false
	}
	if ttl > 0 {
		e.expiresAt = now.Add(ttl)
	} else {
		e.expiresAt = time.Time{}
	}
	s.data[key] = e
	return true
}

// TTL returns remaining seconds: -2 if the key doesn't exist, -1 if no expiry.
func (s *Store) TTL(key string) int64 {
	now := time.Now()
	s.mu.RLock()
	defer s.mu.RUnlock()
	e, ok := s.data[key]
	if !ok || e.expired(now) {
		return -2
	}
	if e.expiresAt.IsZero() {
		return -1
	}
	d := time.Until(e.expiresAt).Seconds()
	if d < 0 {
		d = 0
	}
	return int64(d + 0.999)
}

// IncrBy atomically adds delta to the integer value at key (creating it at 0
// first). Returns the new value and ok=false if the existing value isn't an int.
func (s *Store) IncrBy(key string, delta int64) (int64, bool) {
	now := time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()
	var cur int64
	if e, ok := s.data[key]; ok && !e.expired(now) {
		n, err := parseInt(e.value)
		if err != nil {
			return 0, false
		}
		cur = n
	}
	cur += delta
	// preserve existing expiry if any
	exp := time.Time{}
	if e, ok := s.data[key]; ok && !e.expired(now) {
		exp = e.expiresAt
	}
	s.data[key] = entry{value: formatInt(cur), expiresAt: exp}
	return cur, true
}

// Keys returns all live keys (used by KEYS * and the dashboard).
func (s *Store) Keys() []string {
	now := time.Now()
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]string, 0, len(s.data))
	for k, e := range s.data {
		if !e.expired(now) {
			out = append(out, k)
		}
	}
	return out
}

// FlushAll clears everything.
func (s *Store) FlushAll() {
	s.mu.Lock()
	s.data = make(map[string]entry)
	s.mu.Unlock()
}

func (s *Store) countCommand() {
	s.mu.Lock()
	s.commands++
	s.mu.Unlock()
}

// Stats is a snapshot for the dashboard / INFO / /stats.
type Stats struct {
	UptimeSeconds int64  `json:"uptime_seconds"`
	Keys          int    `json:"keys"`
	Commands      uint64 `json:"commands_processed"`
	Hits          uint64 `json:"keyspace_hits"`
	Misses        uint64 `json:"keyspace_misses"`
	Expired       uint64 `json:"expired_keys"`
}

func (s *Store) Stats() Stats {
	now := time.Now()
	s.mu.RLock()
	defer s.mu.RUnlock()
	live := 0
	for _, e := range s.data {
		if !e.expired(now) {
			live++
		}
	}
	return Stats{
		UptimeSeconds: int64(time.Since(s.startedAt).Seconds()),
		Keys:          live,
		Commands:      s.commands,
		Hits:          s.hits,
		Misses:        s.misses,
		Expired:       s.expiredN,
	}
}
