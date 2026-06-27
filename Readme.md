# proxy — tiny HTTP/HTTPS proxy for tcpsvd

Tiny C proxy that reads from stdin and writes to stdout (inetd/tcpsvd style). Handles HTTP CONNECT (HTTPS tunneling) and plain HTTP forwarding.

## Build
```sh
meson setup build
ninja -C build
```

## Usage

```sh
tcpsvd -vE 0.0.0.0 8080 ./proxy
```

or

```sh
socat TCP-LISTEN:8080,reuseaddr,fork EXEC:./proxy
```
