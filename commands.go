package main

import (
	"bufio"
	"net"
	"strings"
	"time"
)

// serveTCP runs the RESP server. Each client connection is handled in its own
// goroutine; the shared Store is safe for concurrent use.
func serveTCP(ln net.Listener, store *Store) {
	for {
		conn, err := ln.Accept()
		if err != nil {
			return // listener closed
		}
		go handleConn(conn, store)
	}
}

func handleConn(conn net.Conn, store *Store) {
	defer conn.Close()
	r := bufio.NewReader(conn)
	w := bufio.NewWriter(conn)
	for {
		args, err := readCommand(r)
		if err != nil {
			return // client disconnected or protocol error
		}
		if len(args) == 0 {
			continue
		}
		store.countCommand()
		if !dispatch(args, store, w) {
			w.Flush()
			return // QUIT
		}
		if err := w.Flush(); err != nil {
			return
		}
	}
}

// dispatch executes one command. Returns false when the connection should close.
func dispatch(args []string, store *Store, w *bufio.Writer) bool {
	cmd := strings.ToUpper(args[0])
	switch cmd {
	case "PING":
		if len(args) > 1 {
			writeBulk(w, args[1])
		} else {
			writeSimple(w, "PONG")
		}

	case "ECHO":
		if len(args) != 2 {
			writeError(w, "ERR wrong number of arguments for 'echo'")
		} else {
			writeBulk(w, args[1])
		}

	case "SET":
		handleSet(args, store, w)

	case "GET":
		if len(args) != 2 {
			writeError(w, "ERR wrong number of arguments for 'get'")
			break
		}
		if v, ok := store.Get(args[1]); ok {
			writeBulk(w, v)
		} else {
			writeNull(w)
		}

	case "DEL":
		if len(args) < 2 {
			writeError(w, "ERR wrong number of arguments for 'del'")
			break
		}
		var n int64
		for _, k := range args[1:] {
			if store.Del(k) {
				n++
			}
		}
		writeInt(w, n)

	case "EXISTS":
		if len(args) < 2 {
			writeError(w, "ERR wrong number of arguments for 'exists'")
			break
		}
		var n int64
		for _, k := range args[1:] {
			if store.Exists(k) {
				n++
			}
		}
		writeInt(w, n)

	case "EXPIRE":
		if len(args) != 3 {
			writeError(w, "ERR wrong number of arguments for 'expire'")
			break
		}
		secs, err := parseInt(args[2])
		if err != nil {
			writeError(w, "ERR value is not an integer or out of range")
			break
		}
		if store.Expire(args[1], time.Duration(secs)*time.Second) {
			writeInt(w, 1)
		} else {
			writeInt(w, 0)
		}

	case "TTL":
		if len(args) != 2 {
			writeError(w, "ERR wrong number of arguments for 'ttl'")
			break
		}
		writeInt(w, store.TTL(args[1]))

	case "INCR":
		incrCmd(args, store, w, 1)
	case "DECR":
		incrCmd(args, store, w, -1)

	case "INCRBY", "DECRBY":
		if len(args) != 3 {
			writeError(w, "ERR wrong number of arguments")
			break
		}
		d, err := parseInt(args[2])
		if err != nil {
			writeError(w, "ERR value is not an integer or out of range")
			break
		}
		if cmd == "DECRBY" {
			d = -d
		}
		if n, ok := store.IncrBy(args[1], d); ok {
			writeInt(w, n)
		} else {
			writeError(w, "ERR value is not an integer or out of range")
		}

	case "KEYS":
		writeArrayBulk(w, store.Keys())

	case "DBSIZE":
		writeInt(w, int64(len(store.Keys())))

	case "FLUSHALL", "FLUSHDB":
		store.FlushAll()
		writeSimple(w, "OK")

	case "INFO":
		writeBulk(w, infoText(store))

	case "COMMAND":
		// redis-cli sends "COMMAND DOCS" on connect; an empty array is fine.
		w.WriteString("*0\r\n")

	case "QUIT":
		writeSimple(w, "OK")
		return false

	default:
		writeError(w, "ERR unknown command '"+args[0]+"'")
	}
	return true
}

func incrCmd(args []string, store *Store, w *bufio.Writer, delta int64) {
	if len(args) != 2 {
		writeError(w, "ERR wrong number of arguments")
		return
	}
	if n, ok := store.IncrBy(args[1], delta); ok {
		writeInt(w, n)
	} else {
		writeError(w, "ERR value is not an integer or out of range")
	}
}

// handleSet parses SET key value [NX] [EX seconds | PX milliseconds].
func handleSet(args []string, store *Store, w *bufio.Writer) {
	if len(args) < 3 {
		writeError(w, "ERR wrong number of arguments for 'set'")
		return
	}
	key, value := args[1], args[2]
	var ttl time.Duration
	nx := false
	i := 3
	for i < len(args) {
		opt := strings.ToUpper(args[i])
		switch opt {
		case "NX":
			nx = true
			i++
		case "XX":
			// not supported distinctly; ignore for simplicity
			i++
		case "EX", "PX":
			if i+1 >= len(args) {
				writeError(w, "ERR syntax error")
				return
			}
			n, err := parseInt(args[i+1])
			if err != nil || n <= 0 {
				writeError(w, "ERR invalid expire time in 'set'")
				return
			}
			if opt == "EX" {
				ttl = time.Duration(n) * time.Second
			} else {
				ttl = time.Duration(n) * time.Millisecond
			}
			i += 2
		default:
			writeError(w, "ERR syntax error")
			return
		}
	}
	if store.Set(key, value, ttl, nx) {
		writeSimple(w, "OK")
	} else {
		writeNull(w) // NX failed: key already exists
	}
}
