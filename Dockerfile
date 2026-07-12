# ---- build stage ----
FROM golang:1.22-alpine AS build
WORKDIR /src
# Pure standard-library project: no go.sum / no external modules to download.
COPY go.mod ./
COPY *.go ./
# Static binary (no libc), so it runs in a tiny scratch image.
RUN CGO_ENABLED=0 GOOS=linux go build -ldflags="-s -w" -o /miniredis .

# ---- runtime stage ----
FROM alpine:3.20
RUN adduser -D -u 10001 app
COPY --from=build /miniredis /usr/local/bin/miniredis
USER app
# HTTP gateway (Render routes $PORT here) + RESP/TCP for Redis clients.
EXPOSE 8080 6379
ENTRYPOINT ["/usr/local/bin/miniredis"]
