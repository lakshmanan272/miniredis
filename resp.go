package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"strconv"
	"strings"
)

// RESP (REdis Serialization Protocol) reader/writer.
// This is the wire format real Redis clients speak. Because we implement it,
// tools like `redis-cli` and Java's Lettuce client can talk to Mini-Redis as if
// it were a real Redis server.

var errProtocol = errors.New("protocol error")

// readCommand reads exactly one command from the client. It supports the two
// forms a Redis client may send:
//   - RESP arrays of bulk strings:  *2\r\n$4\r\nLLEN\r\n$3\r\nfoo\r\n
//   - inline commands (plain text): PING\r\n   (used by simple/telnet clients)
func readCommand(r *bufio.Reader) ([]string, error) {
	prefix, err := r.ReadByte()
	if err != nil {
		return nil, err
	}
	if prefix != '*' {
		// inline command: read the whole line and split on spaces.
		if err := r.UnreadByte(); err != nil {
			return nil, err
		}
		line, err := readLine(r)
		if err != nil {
			return nil, err
		}
		line = strings.TrimSpace(line)
		if line == "" {
			return []string{}, nil
		}
		return strings.Fields(line), nil
	}

	countLine, err := readLine(r)
	if err != nil {
		return nil, err
	}
	n, err := strconv.Atoi(countLine)
	if err != nil || n < 0 {
		return nil, errProtocol
	}
	args := make([]string, 0, n)
	for i := 0; i < n; i++ {
		b, err := r.ReadByte()
		if err != nil {
			return nil, err
		}
		if b != '$' {
			return nil, errProtocol
		}
		lenLine, err := readLine(r)
		if err != nil {
			return nil, err
		}
		length, err := strconv.Atoi(lenLine)
		if err != nil || length < 0 {
			return nil, errProtocol
		}
		buf := make([]byte, length+2) // include trailing \r\n
		if _, err := io.ReadFull(r, buf); err != nil {
			return nil, err
		}
		args = append(args, string(buf[:length]))
	}
	return args, nil
}

// readLine reads until \r\n and returns the line without the terminator.
func readLine(r *bufio.Reader) (string, error) {
	line, err := r.ReadString('\n')
	if err != nil {
		return "", err
	}
	line = strings.TrimRight(line, "\r\n")
	return line, nil
}

// --- response writers ---

func writeSimple(w *bufio.Writer, s string) { fmt.Fprintf(w, "+%s\r\n", s) }
func writeError(w *bufio.Writer, s string)  { fmt.Fprintf(w, "-%s\r\n", s) }
func writeInt(w *bufio.Writer, n int64)     { fmt.Fprintf(w, ":%d\r\n", n) }

func writeBulk(w *bufio.Writer, s string) {
	fmt.Fprintf(w, "$%d\r\n%s\r\n", len(s), s)
}

func writeNull(w *bufio.Writer) { w.WriteString("$-1\r\n") }

func writeArrayBulk(w *bufio.Writer, items []string) {
	fmt.Fprintf(w, "*%d\r\n", len(items))
	for _, it := range items {
		writeBulk(w, it)
	}
}
