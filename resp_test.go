package main

import (
	"bufio"
	"strings"
	"testing"
)

func TestReadCommandArray(t *testing.T) {
	// *3 SET foo bar
	in := "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n"
	r := bufio.NewReader(strings.NewReader(in))
	args, err := readCommand(r)
	if err != nil {
		t.Fatal(err)
	}
	if len(args) != 3 || args[0] != "SET" || args[1] != "foo" || args[2] != "bar" {
		t.Fatalf("parsed %v", args)
	}
}

func TestReadCommandInline(t *testing.T) {
	r := bufio.NewReader(strings.NewReader("PING\r\n"))
	args, err := readCommand(r)
	if err != nil {
		t.Fatal(err)
	}
	if len(args) != 1 || args[0] != "PING" {
		t.Fatalf("parsed %v", args)
	}
}

func TestReadCommandBinarySafe(t *testing.T) {
	// value contains spaces and a CR — bulk-string framing must preserve it.
	in := "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\na b c\r\n"
	r := bufio.NewReader(strings.NewReader(in))
	args, err := readCommand(r)
	if err != nil {
		t.Fatal(err)
	}
	if args[2] != "a b c" {
		t.Fatalf("value = %q want 'a b c'", args[2])
	}
}
