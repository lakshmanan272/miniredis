package main

import (
	"sync"
	"testing"
	"time"
)

func TestSetGetDel(t *testing.T) {
	s := NewStore()
	if !s.Set("k", "v", 0, false) {
		t.Fatal("set should succeed")
	}
	if v, ok := s.Get("k"); !ok || v != "v" {
		t.Fatalf("get k = %q,%v want v,true", v, ok)
	}
	if !s.Del("k") {
		t.Fatal("del should return true")
	}
	if _, ok := s.Get("k"); ok {
		t.Fatal("key should be gone")
	}
}

func TestSetNXLockSemantics(t *testing.T) {
	s := NewStore()
	if !s.Set("lock", "A", time.Second, true) {
		t.Fatal("first NX set should acquire")
	}
	if s.Set("lock", "B", time.Second, true) {
		t.Fatal("second NX set must fail while held")
	}
	// safe unlock: wrong token must not release
	if s.DelIfValue("lock", "B") {
		t.Fatal("must not release with wrong token")
	}
	if !s.DelIfValue("lock", "A") {
		t.Fatal("owner should release")
	}
	if !s.Set("lock", "C", time.Second, true) {
		t.Fatal("after release NX should acquire again")
	}
}

func TestTTLExpiry(t *testing.T) {
	s := NewStore()
	s.Set("k", "v", 40*time.Millisecond, false)
	if ttl := s.TTL("k"); ttl <= 0 {
		t.Fatalf("ttl should be positive, got %d", ttl)
	}
	time.Sleep(70 * time.Millisecond)
	if _, ok := s.Get("k"); ok {
		t.Fatal("key should have expired")
	}
	if ttl := s.TTL("k"); ttl != -2 {
		t.Fatalf("ttl of missing key = %d want -2", ttl)
	}
}

func TestIncr(t *testing.T) {
	s := NewStore()
	if n, ok := s.IncrBy("c", 1); !ok || n != 1 {
		t.Fatalf("incr = %d,%v", n, ok)
	}
	if n, _ := s.IncrBy("c", 4); n != 5 {
		t.Fatalf("incr = %d want 5", n)
	}
	s.Set("s", "abc", 0, false)
	if _, ok := s.IncrBy("s", 1); ok {
		t.Fatal("incr on non-int must fail")
	}
}

// TestConcurrentLock is the important one: many goroutines race for the same
// lock; exactly one must win. This proves the store is safe under concurrency —
// the same guarantee BookMyEvent relies on to never oversell.
func TestConcurrentLock(t *testing.T) {
	s := NewStore()
	const goroutines = 200
	var wg sync.WaitGroup
	var winners int64
	var mu sync.Mutex
	wg.Add(goroutines)
	for i := 0; i < goroutines; i++ {
		go func() {
			defer wg.Done()
			if s.Set("lock:event:1", "tok", 2*time.Second, true) {
				mu.Lock()
				winners++
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	if winners != 1 {
		t.Fatalf("exactly one goroutine must win the lock, got %d", winners)
	}
}
