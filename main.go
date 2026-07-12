package main

import (
	"context"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

// Mini-Redis: a small Redis clone in Go.
//   - RESP protocol over TCP (default :6379) so real Redis clients / redis-cli / Lettuce connect.
//   - An HTTPS gateway (on $PORT) so a free-tier cloud app can use it as a distributed lock.
// Built to pair with Project 1 (BookMyEvent), whose booking lock can run on this server.
func main() {
	httpPort := getenv("PORT", "8080")   // Render injects PORT for the web service
	respPort := getenv("RESP_PORT", "6379")

	store := NewStore()
	stop := make(chan struct{})
	store.StartJanitor(time.Second, stop) // active TTL expiry every second

	// RESP/TCP server (best-effort; on some hosts the port may be internal only).
	if ln, err := net.Listen("tcp", ":"+respPort); err != nil {
		log.Printf("RESP listener on :%s not started: %v (HTTP gateway still runs)", respPort, err)
	} else {
		log.Printf("RESP (Redis protocol) listening on :%s", respPort)
		go serveTCP(ln, store)
		defer ln.Close()
	}

	// HTTP gateway + dashboard.
	srv := &http.Server{
		Addr:              ":" + httpPort,
		Handler:           httpMux(store),
		ReadHeaderTimeout: 5 * time.Second,
	}
	go func() {
		log.Printf("HTTP gateway + dashboard on :%s", httpPort)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("http server failed: %v", err)
		}
	}()

	// Graceful shutdown on SIGINT/SIGTERM.
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	close(stop)
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = srv.Shutdown(ctx)
	log.Println("shutting down")
}

func getenv(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
