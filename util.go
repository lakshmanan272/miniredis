package main

import "strconv"

func parseInt(s string) (int64, error) { return strconv.ParseInt(s, 10, 64) }
func formatInt(n int64) string         { return strconv.FormatInt(n, 10) }
