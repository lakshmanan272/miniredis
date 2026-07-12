CC      = gcc
CFLAGS  = -O2 -Wall -pthread
SRC     = dict.c store.c util.c proto.c http.c main.c
HDR     = mr.h dashboard.h

miniredis: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o miniredis $(SRC)

run: miniredis
	./miniredis

clean:
	rm -f miniredis

.PHONY: run clean
