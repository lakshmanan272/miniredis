# ---- build stage ----
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base
WORKDIR /src
COPY *.c *.h ./
RUN gcc -O2 -Wall -pthread -o /miniredis dict.c store.c util.c proto.c http.c main.c

# ---- runtime stage ----
FROM alpine:3.20
RUN adduser -D -u 10001 app
COPY --from=build /miniredis /usr/local/bin/miniredis
USER app
# HTTP gateway (Render routes $PORT here) + RESP/TCP for Redis clients.
EXPOSE 8080 6379
ENTRYPOINT ["/usr/local/bin/miniredis"]
