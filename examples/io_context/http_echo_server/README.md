# io_context HTTP Echo Server

This example shows an application-level event loop built on `bupp::io_context`.
It accepts TCP connections, reads one HTTP/1.1 request at a time, and writes an
HTTP response whose body echoes the request body. Requests without a body echo
the request method and target instead.

Build the default examples:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target bupp_io_context_http_echo_server
```

Run the server:

```sh
./build/examples/io_context/http_echo_server/bupp_io_context_http_echo_server 8080
```

Try it with curl:

```sh
curl -v http://127.0.0.1:8080/hello
curl -v -d 'hello from bupp' http://127.0.0.1:8080/echo
```

The important lifetime rule is that every sender is connected to an operation
state and that operation state must stay alive until completion. This example
uses a small local operation holder to keep pending accept, receive, send, and
timer operations alive while `ctx.run()` drives completions.
