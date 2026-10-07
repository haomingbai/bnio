# Design: Separating Local (AF_UNIX) Sockets from Network Sockets

Design document. Status: approved (2026-10-07); unimplemented. Nothing in
this document is implemented yet; the repository state it describes is
`main` as of the writing date.

`bnio` currently routes every stream socket — TCP and AF_UNIX alike —
through one untyped write path. This document proposes giving local sockets
their own protocol-ized type family, mirroring how Asio separates
`asio::ip::tcp` / `asio::ip::udp` from `asio::local::stream_protocol` /
`asio::local::datagram_protocol`, and making `io_context` model selection a
function of socket type rather than of kernel capability alone.

---

## 1. Status Quo and Problem Statement

### 1.1 The current type landscape

Layer 2 (`bnio::async_io`) vocabulary types:

| Type | Location | Notes |
|------|----------|-------|
| `socket_view` | `include/bnio/async_io/socket_view.h:25` | Descriptor access only, kind unspecified |
| `datagram_socket_view` | `include/bnio/async_io/socket_view.h:92` | `SOCK_DGRAM`, takes `ip::endpoint` in `bind`/`connect` |
| `stream_socket_view` | `include/bnio/async_io/socket_view.h:177` | `SOCK_STREAM`, takes `ip::endpoint` in `bind`/`connect` |
| `ip::address` | `include/bnio/async_io/ip/address.h:25` | IPv4/IPv6 only |
| `ip::endpoint` | `include/bnio/async_io/ip/endpoint.h:22` | Address + port |
| `ip::tcp`, `ip::udp` | `include/bnio/async_io/ip/tcp.h:20`, `include/bnio/async_io/ip/udp.h:20` | Protocol tags carrying an IP version |
| `tcp_endpoint` alias | `include/bnio/async_io/tcp_endpoint.h:17` | |

Layer 3 / high-level owning types:

| Type | Location |
|------|----------|
| `bnio::tcp::socket` | `include/bnio/tcp/socket.h:29` |
| `bnio::tcp::acceptor` | `include/bnio/tcp/acceptor.h:27` |
| `bnio::udp::socket` | `include/bnio/udp/socket.h:31` |
| `bnio::ip::tcp` / `bnio::ip::udp` facade (tag + nested `socket`/`acceptor` typedefs) | `include/bnio/ip.h:104`, `include/bnio/ip.h:192` |

Native address storage: `linux_native::socket_address`
(`include/bnio/async_io/linux/socket_address.h:20`) and
`bsd_native::socket_address`
(`include/bnio/async_io/bsd/socket_address.h:20`). Both wrap a
`sockaddr_storage` built exclusively from an `ip::endpoint`, both report only
`AF_INET` / `AF_INET6` (`family()`), and both `make_endpoint()` converters
return `std::nullopt` for any other family.

**AF_UNIX has no representation anywhere in the address/endpoint layer.**
No `sockaddr_un`, no path endpoint, no local protocol tag exists on `main`.

### 1.2 Where type information is lost

The `io_context` write path is keyed on `stream_socket_view`:

- `io_context::async_read/async_read_some/async_write/async_write_some(async_io::stream_socket_view, ...)` —
  `include/bnio/detail/posix/io_context/native_io.h:37-62`
- `async_accept` / `async_connect(view, const ip::endpoint&)` —
  `include/bnio/detail/posix/io_context/native_io.h:104-115`
- `basic_scheduler` pass-throughs —
  `include/bnio/detail/posix/io_context/native_io.h:141-204`

A `stream_socket_view` holds nothing but an `int`
(`include/bnio/async_io/socket_view.h:177-275`). The backend factories turn
it into models without ever recovering what family the descriptor belongs to:

- Linux: `make_stream_write_request` → `socket_write_model`
  (`include/bnio/detail/linux/io_context_native_io/factories.h:24-27`,
  `include/bnio/detail/linux/io_context_native_io/socket.h:55-90`)
- BSD: `make_stream_write_request` → `kqueue_send_request`
  (`include/bnio/detail/bsd/io_context_native_io/factories.h:30-34`)

The models themselves are family-agnostic by construction —
`socket_write_model` stores the view only to extract `native_handle()`.

### 1.3 The failure this produced

The `feat/send-zc` branch (commit `83798dc`, not merged, kept as reference
only) selected the io_uring `IORING_OP_SEND_ZC` model purely from an opcode
probe: `probe_send_zc_support()` asks the kernel whether the opcode exists,
and `make_stream_write_request` takes a `use_zc` flag derived from that probe
alone. The probe reports a kernel-wide capability; it cannot know what kind
of socket any given descriptor is.

On Linux, `IORING_OP_SEND_ZC` requires the socket to opt in via
`SOCK_SUPPORT_ZC`, which only `tcp_init_sock` / `udp_init_sock` set. An
AF_UNIX stream socket submitted through that path fails with `EOPNOTSUPP`
on every write. Because the branch tied model selection to the probe, the
only way to keep AF_UNIX stream tests alive was to mask them
(`TEST_FILTER` in `tests/integration/io_context/CMakeLists.txt` and
`tests/stress/io_context/CMakeLists.txt` — 25 cases across 8 entries).

This is the structural problem stated plainly: **the write path cannot
distinguish TCP from AF_UNIX, so model selection can only look at kernel
capability — and the two answers genuinely diverge per socket type.** The
same descriptor-level blindness is latent in the read path and in the eager
(`try_immediate`) path, both of which are fine today only because their
models happen to be family-safe.

### 1.4 Goal

Local sockets become a first-class, separate type family with their own
protocol tags, endpoint, views, and owners. `io_context` operations are
overloaded per socket family, so model choice is made by the type system at
the factory boundary. TCP/UDP keep their existing surface unchanged.

---

## 2. Asio's Protocol-ized Design, Mapped onto bnio

### 2.1 What Asio does

Asio (headers verified in the local Boost.Asio installation; standalone
Asio is structurally identical):

- A **Protocol** is a small value type exposing `type()` / `protocol()` /
  `family()` plus nested typedefs. `asio::local::stream_protocol`
  (`local/stream_protocol.hpp:47-81`) returns `SOCK_STREAM` / `0` /
  `AF_UNIX` and names `endpoint`, `socket`, `acceptor`. Same shape for
  `local::datagram_protocol` (`SOCK_DGRAM`) and
  `local::seq_packet_protocol` (`SOCK_SEQPACKET`).
- `asio::ip::tcp` (`ip/tcp.hpp`) is the same concept with state: `v4()` /
  `v6()` select the family, `type()` is `SOCK_STREAM`, `protocol()` is
  `IPPROTO_TCP`.
- The endpoint is one template, `basic_endpoint<Protocol>`
  (`local/basic_endpoint.hpp:48-177`), with the `data()` / `size()` /
  `resize()` / `capacity()` / `path()` surface. Abstract-namespace
  addresses are just paths whose first byte is `'\0'`; unnamed sockets
  surface as a zero-length path.
- Socket types are class templates over the protocol:
  `basic_stream_socket<Protocol>` and `basic_socket_acceptor<Protocol>`
  carry a `protocol_type` typedef and are constructed from
  `(executor, protocol)` or `(executor, endpoint)` — the endpoint's
  `protocol()` tells the socket what to `open()`.
- Dispatch is therefore compile-time: each `basic_stream_socket<P>`
  instantiation binds to a service typed on `P`, and no operation ever
  reinterprets a descriptor across protocol families.
- `local::connect_pair()` (`local/connect_pair.hpp:37-66`) is a free
  function over any local protocol. `generic::stream_protocol` exists as
  an escape hatch for arbitrary family/type pairs.

### 2.2 Correspondence in bnio's bexec style

bnio does not need the template machinery to get the same discipline:

| Asio mechanism | bnio equivalent |
|----------------|-----------------|
| `basic_X_socket<Protocol>` instantiations | Concrete owner classes per family (`tcp::socket` today; `local::stream_socket` proposed) |
| `basic_endpoint<Protocol>` | One concrete `local::endpoint` value type (all three Asio local protocols share one endpoint shape; bnio has no need to duplicate it per protocol) |
| Service dispatch typed on `Protocol` | Overload resolution on distinct view types at the `io_context` factory boundary |
| `endpoint.protocol()` → `open()` | `open(protocol_tag)` overloads (already the bnio pattern: `tcp::socket::open(ip::tcp)`, `include/bnio/tcp/socket.h:175`) |

The CPO surface needs **zero structural change**. `bnio::async_read` /
`async_write` / `async_connect` / `async_accept`
(`include/bnio/io_context_cpo/read.h:18`, `write.h:18`,
`connection.h:17,41`) are duck-typed: they require a member
`async_read(provider, buffer)`-style call returning a sender, and the
concepts in `include/bnio/io_context_cpo/concepts.h:24-64`
(`reads_bytes`, `writes_bytes`, `accepts_connections`, `connects_stream`)
check exactly that. A `local::stream_socket` with the same member shape as
`tcp::socket` satisfies them without touching the CPOs. Completion
signatures stay identical everywhere: `set_value(std::error_code,
std::size_t)` for byte-count ops, `set_value(std::error_code, int)` for
accept, `set_value(std::error_code)` for connect, `set_stopped()`.

---

## 3. Proposal: the Local Socket Type Family

### 3.1 Naming

The namespace is **`bnio::local`** — decided (2026-10-07, user-approved).
It matches Asio's `asio::local`, is short, and leaks no kernel spelling
into the user API.

`bnio::unix` / `bnio::unix_domain` were excluded on a hard technical
ground: `unix` is a predefined macro under GNU dialects (`-std=gnu++*`) on
common toolchains, so a namespace of that name would be textually replaced
during preprocessing. The remaining candidates (`bnio::ipc`,
`bnio::af_unix`, `bnio::local_sockets`) were rejected as mismatched with
Asio vocabulary or redundant inside a socket library.

### 3.2 Vocabulary types (layer 2, `bnio::async_io::local`)

New headers `include/bnio/async_io/local/{endpoint.h,protocol.h,socket_view.h}`.
The nested view naming (`async_io::local::stream_socket_view`, Asio-like)
is decided (2026-10-07, user-approved); the flat
`async_io::local_stream_socket_view` spelling was rejected.

```cpp
namespace bnio::async_io::local {

enum class endpoint_kind { unspecified, path_name, abstract };

// Value type: fixed-capacity path buffer, no allocation, trivially copyable
// — consistent with ip::address / ip::endpoint. Decided (2026-10-07);
// a std::string member was rejected.
class endpoint {
public:
    static constexpr std::size_t max_path_length =
        sizeof(sockaddr_un::sun_path);       // 108 on Linux, 104 on BSD

    endpoint() noexcept;                     // unspecified (also: unnamed peers)
    explicit endpoint(std::string_view path) noexcept;   // path_name kind

    // Abstract namespace. Available on Linux only (compile-guarded); on
    // BSD backends this factory does not exist.
    static endpoint abstract(std::string_view name) noexcept;

    [[nodiscard]] endpoint_kind kind() const noexcept;
    [[nodiscard]] std::string_view path() const noexcept;  // without leading NUL
};

// Protocol tags — stateless, same role as ip::tcp / ip::udp.
class stream_protocol {
public:
    using endpoint = local::endpoint;
    int type() const noexcept;      // SOCK_STREAM
    int protocol() const noexcept;  // 0
    int family() const noexcept;    // AF_UNIX
};

class datagram_protocol {
public:
    using endpoint = local::endpoint;
    int type() const noexcept;      // SOCK_DGRAM
    int protocol() const noexcept;  // 0
    int family() const noexcept;    // AF_UNIX
};

// Views — same non-owning contract as stream_socket_view / datagram_socket_view.
class stream_socket_view {   // bind(endpoint), listen(), connect(endpoint),
                             // shutdown(), remote_endpoint(endpoint&)
};
class datagram_socket_view { // bind(endpoint), connect(endpoint),
                             // local_endpoint(), remote_endpoint()
};

}  // namespace bnio::async_io::local
```

Endpoint design notes:

- **Path-name endpoints** encode as `sockaddr_un` with `strlen(path) + 1`.
- **Abstract endpoints** encode with the leading `'\0'` included in
  `sun_path`; `addr_len = sizeof(sa_family_t) + 1 + name_length`.
- **Unnamed** (an unbound or auto-bound peer reported by `recvfrom`) decodes
  to `kind = unspecified` when `addr_len == sizeof(sa_family_t)`.
- Oversized paths are rejected at the `sockaddr_un` conversion boundary with
  `std::errc::invalid_argument` (the same "decode failure" channel the IP
  `make_endpoint` converters use today,
  `include/bnio/async_io/linux/socket_address.h:84`).
- The endpoint is always constructible as a value, but on BSD only the
  `path_name` kind can reach a socket call, because the abstract namespace
  is a Linux convention.

### 3.3 Owning types (high level, `bnio::local`)

New headers `include/bnio/local/{socket.h,acceptor.h,async_operations.h}`
plus an aggregate `include/bnio/local.h`. The member shape mirrors
`bnio::tcp::socket` exactly.

```cpp
namespace bnio::local {

class stream_socket {          // RAII owner, move-only
public:
    using native_handle_type = async_io::local::stream_socket_view::native_handle_type;

    explicit stream_socket(native_handle_type fd) noexcept;  // + assign/release
    [[nodiscard]] async_io::local::stream_socket_view view() const noexcept;

    [[nodiscard]] std::error_code open(async_io::local::stream_protocol p) noexcept;

    template <class Scheduler>
    [[nodiscard]] auto async_connect(Scheduler s,
                                     const async_io::local::endpoint& e);
    template <class Scheduler, class Buffer>
    [[nodiscard]] auto async_read(Scheduler s, Buffer&& b, int flags = 0);
    template <class Scheduler, class Buffer>
    [[nodiscard]] auto async_read_some(Scheduler s, Buffer&& b, int flags = 0);
    template <class Scheduler, class Buffer>
    [[nodiscard]] auto async_write(Scheduler s, Buffer&& b, int flags = 0);
    template <class Scheduler, class Buffer>
    [[nodiscard]] auto async_write_some(Scheduler s, Buffer&& b, int flags = 0);

    [[nodiscard]] std::error_code shutdown(int how) noexcept;
    [[nodiscard]] std::error_code close() noexcept;
};

class stream_acceptor {        // open / bind(endpoint) / listen / async_accept
public:
    template <class Scheduler>
    [[nodiscard]] auto async_accept(Scheduler s, int flags = 0);
    // completes with local::stream_socket, same as tcp::acceptor → tcp::socket
};

class datagram_socket {        // open / bind / connect(default peer)
public:
    [[nodiscard]] std::error_code open(async_io::local::datagram_protocol p) noexcept;

    template <class Scheduler, class Buffer>
    [[nodiscard]] auto async_send(Scheduler s, Buffer&& b, int flags = 0);
    template <class Scheduler, class Buffer>
    [[nodiscard]] auto async_receive(Scheduler s, Buffer&& b, int flags = 0);
    template <class Scheduler, class Buffer>
    [[nodiscard]] auto async_send_to(Scheduler s, Buffer&& b,
                                     const async_io::local::endpoint& e,
                                     int flags = 0);
    template <class Scheduler, class Buffer>
    [[nodiscard]] auto async_receive_from(Scheduler s, Buffer&& b,
                                          async_io::local::endpoint& e,
                                          int flags = 0);
};

// Asio-style discovery, mirroring bnio::ip::tcp's nested typedefs
// (include/bnio/ip.h:104-125):
//   stream_protocol::socket    = stream_socket
//   stream_protocol::acceptor  = stream_acceptor
//   datagram_protocol::socket  = datagram_socket

}  // namespace bnio::local
```

### 3.4 v1 socket-semantics scope

Decided (2026-10-07, user-approved): stream and datagram ship in v1;
seqpacket stays out of scope (§8).

| Semantics | v1 | Rationale |
|-----------|----|-----------|
| `SOCK_STREAM` | **Yes** | Covers the entire existing test migration surface (every socketpair test is `SOCK_STREAM`) and closes the SEND_ZC incident class. |
| `SOCK_DGRAM` | **Yes** | Cheap: mirrors the UDP path 1:1 (connected `send`/`receive` + `send_to`/`receive_from`), and datagram completion semantics are already cleanly separated from stream in the dispatch chain. |
| `SOCK_SEQPACKET` | **No** | Adds a third completion semantics (ordered + message-preserving) with a distinct op surface (per-message receive with truncation reporting) and no internal user today. Deferred; the view/protocol naming above leaves room to add it without rework. |

### 3.5 CPO surface coverage

No CPO definition changes. The coverage comes from new member functions on
the owner types and new `io_context` overloads (§4):

- `async_connect` — `local::stream_socket` member; io_context overload keyed
  on `local::stream_socket_view`.
- `async_read` / `async_read_some` / `async_write` / `async_write_some` —
  stream members and io_context overloads; satisfy `reads_bytes` /
  `writes_bytes` unchanged.
- `async_accept` — `local::stream_acceptor` member yielding
  `local::stream_socket`; io_context overload yielding a raw fd at view
  level, exactly as `stream_socket_view` does today.
- `async_send` / `async_receive` / `async_send_to` / `async_receive_from` —
  datagram members and io_context overloads keyed on
  `local::datagram_socket_view`.
- `async_poll` — no change; `descriptor_view` is family-blind and stays so.

### 3.6 Relationship to scheduling (`schedule_kind`, eager path)

All new `io_context` overloads are member templates over
`io_context::schedule_kind`, exactly like the existing ones
(`include/bnio/detail/posix/io_context/native_io.h:36-121`). Concretely:

- **Eager (`try_immediate`) path**: local stream read/write models keep the
  immediate probe — nonblocking `recv`/`send` with `MSG_DONTWAIT` behaves on
  AF_UNIX exactly as on TCP. `has_immediate_io`
  (`include/bnio/detail/linux/io_context_native_io/common.h:45-48`) applies
  unchanged. Accept/connect stay submission-only, matching TCP.
- **defer kind**: publication is already type-blind (it operates on
  operation bases, not views), so `schedule_kind::defer` needs no local
  special-casing.
- **Runtime eager toggle** (`enable_immediate_io`) applies to local
  operations identically.

---

## 4. Dispatch-Layer Changes

### 4.1 Principle: type information is consumed at the factory boundary

The split point is the `detail` factory functions
(`include/bnio/detail/{linux,bsd}/io_context_native_io/factories.h`). Above
them, overload resolution on distinct view types carries the family; below
them, models need only a descriptor.

On Linux, the socket models currently store a view
(`include/bnio/detail/linux/io_context_native_io/socket.h:50,87,124,163,...`)
solely to extract `native_handle()`. Changing those members to a plain
`int descriptor` lets one model body serve both families, with factories
overloading on view type:

```cpp
// include/bnio/detail/linux/io_context_native_io/factories.h (after split)
auto make_stream_write_request(async_io::stream_socket_view socket,
                               const_buffer buffer, int flags,
                               bool use_zc);          // TCP path only
auto make_local_stream_write_request(async_io::local::stream_socket_view socket,
                                     const_buffer buffer, int flags);
                                                     // plain send model, never ZC
```

`posix/io_context/native_io.h` adds the local overloads (each a two-line
forward to `make_io_sender<Kind>` with the local model, mirroring
`native_io.h:37-115`), and `io_context::basic_scheduler` gains matching
pass-throughs.

The `io_context` write path (in `detail/posix/io_context/class.h` /
`write_all.h`) calls the TCP factory for `stream_socket_view` arguments and
the local factory for `local::stream_socket_view` arguments — overload
resolution, no runtime branch. **This is the fix for the §1.3 incident: the
SEND_ZC probe result can only be consulted from the TCP-stream factory.**

### 4.2 Linux / BSD symmetry

- **Linux models** (`detail/linux/io_context_native_io/socket.h`): read,
  write, datagram send/receive/send-to/receive-from models become
  descriptor-typed (§4.1). `accept_model` and `connect_model` gain local
  variants that take `const local::endpoint&` and build `sockaddr_un`.
- **BSD requests** (`async_io/bsd/kqueue_operations/socket.h`): already
  descriptor-typed for data transfer (`kqueue_receive_request` takes
  `int descriptor`, `socket.h:61`; `kqueue_send_request`, `socket.h:106`) —
  no model change needed. `kqueue_accept_request` (`socket.h:272`) and
  `kqueue_connect_request` (`socket.h:340`) gain local overloads the same
  way. `EVFILT_READ` / `EVFILT_WRITE` readiness works for AF_UNIX
  unconditionally; there is no kqueue-side capability question.
- **Native address storage**: `linux_native::socket_address` and
  `bsd_native::socket_address` each gain a `local::endpoint` constructor
  alongside the `ip::endpoint` one (same `sockaddr_storage` +
  `size()` contract), plus a `make_local_endpoint(const sockaddr*, socklen_t)`
  decoder returning `std::optional<local::endpoint>`.
- **Layer-2 standalone contexts** (`io_uring_context` / `kqueue_context`
  sender factories): remain fd-generic in v1. The local family targets the
  `io_context` path, which is where every test and user flow lives; layer-2
  local surface can be added later by reusing the same models/requests.

### 4.3 Where capability probing belongs after the split

Capability probes (e.g. the cached `IORING_OP_SEND_ZC` opcode probe) stay at
the context level and remain *family-blind facts about the kernel*. What
changes: they are consulted only inside factories that already know the
socket family. The correct predicate for a per-socket-type model choice is
therefore `family × kernel capability` — with the family half supplied
statically by overload resolution. A future refinement (checking
`SOCK_SUPPORT_ZC` on the specific socket) would live in the same factory;
that is explicitly out of scope here.

---

## 5. Compatibility and Migration

### 5.1 Fate of the generic views

`stream_socket_view` and `datagram_socket_view` **stay**, with their
documented contract narrowed to network (IP) sockets in
[`async-io-layer.md`](async-io-layer.md) — a doc change, recorded here, not
part of any code change in this proposal. Their `io_context` overloads
remain the TCP/UDP path. No existing signature is removed; every current
user keeps compiling.

### 5.2 Affected APIs

Additive only: new headers (`async_io/local/*`, `local/*`, `local.h`),
new `io_context` overloads, new `socket_address` constructors/decoders.
Nothing in the TCP/UDP/SSL surface changes.

### 5.3 Test and example migration inventory

All AF_UNIX usage on `main` enters through raw `::socketpair` and wraps the
fds in `stream_socket_view`. Each site migrates to owning the fd in
`local::stream_socket` (constructor from native handle) and passing
`view()`. No test needs `connect_pair`-style helpers (§8).

| File | Sites | Migration |
|------|-------|-----------|
| `tests/integration/io_context/io_context_read_write_test.cpp` | 11 socketpair sites (110, 142, 174, 207, 274, 306, 585, 673, 750, 824, 862) | Views → `local::stream_socket(fd).view()`; assertions unchanged |
| `tests/integration/io_context/io_context_read_all_test.cpp` | 5 (34, 69, 105, 159, 190) | Same |
| `tests/integration/io_context/io_context_eager_optional_test.cpp` | `make_socketpair()` (198) + 7 call sites | Same; exercises eager path on the local overload |
| `tests/integration/io_context/io_context_defer_scheduler_test.cpp` | factory at 585, uses at 606, 655 | Same; exercises defer kind |
| `tests/integration/io_context/ssl_handshake_test.cpp` | 3 (30, 102, 137) | Same |
| `tests/integration/io_context/ssl_transfer_test.cpp` | 5 (27, 181, 300, 388, 470) | Same |
| `tests/integration/io_context/ssl_alpn_test.cpp` | 5 direct sites (82, 385, 487, 544, 580) + shared `run_socketpair_handshake` helper (77, driven at 174, 217, 261, 313) | Same |
| `tests/stress/io_context/read_write_stress_test.cpp` | 2 (79, 113) | Same |
| `tests/stress/io_context/tcp_stress_test.cpp` | 1 (146) | Same (stress payload uses a socketpair as transport) |
| `tests/integration/async_io/kqueue_sender_test.cpp` | 1 (180) | Same (layer-2 fd-generic path stays valid; the test itself keeps working) |
| `tests/integration/async_io/io_uring_eagain_rearm_test.cpp` | 1 (54) | Same |
| `tests/integration/io_context/lifecycle/finish_phase3_operation_leak_test.cpp` | 1 (109) | Same |
| `tests/integration/io_context/lifecycle/finish_no_new_io_after_stop_test.cpp` | 1 (113) | Same |

Rough size: **13 files, ~45 wrap sites**. The
`tests/basic/async_io/{linux,bsd}_socket_address_test.cpp` AF_UNIX cases
(`linux_socket_address_test.cpp:116`, `bsd_socket_address_test.cpp:85` —
`make_endpoint` returns `nullopt` for `AF_UNIX`) stay **unchanged**: they
pin the IP decoder's rejection behavior, which remains correct after the
split.

Examples (`examples/`) contain no AF_UNIX usage; nothing migrates there.

**Recovery of the masked branch tests**: the 25 cases masked on
`feat/send-zc` are unmasked by this split — the AF_UNIX stream-write cases
run on the local factory (plain send model, never ZC), the TCP cases on the
ZC-capable factory. That branch's test files are not part of this
proposal; they are listed to show the split is what unblocks them.

---

## 6. Error Surface

- **Codes**: `std::error_code` with `std::generic_category()`
  end-to-end, identical to TCP/UDP today. Local-specific errnos surface
  unchanged from the kernel: `ECONNREFUSED` (connect to a bound-but-not-
  listening or stale path), `EADDRINUSE` (bind over an existing socket
  file), `EAFNOSUPPORT`, `EPIPE` (peer closed on send), `ENOTCONN`.
- **Endpoint decode failures** reuse the established pattern:
  `make_local_endpoint` returns `nullopt`, and receive-from-style
  completions report `std::errc::address_family_not_supported` with the
  endpoint reset — byte-for-byte the behavior of the IP decoders today
  (`include/bnio/detail/linux/io_context_native_io/socket.h:207-225`).
- **Oversized paths** report `std::errc::invalid_argument` from the
  `sockaddr_un` conversion, before any syscall.
- **Type mismatch** (an AF_UNIX fd assigned into a `tcp::socket`, or vice
  versa, through the `native_handle_type` escape hatch): a **precondition
  violation with no runtime family validation** — consistent with the
  library-wide rule that internal layers do not defend against internal
  misuse. The descriptor is used verbatim and the first kernel operation
  surfaces the kernel's own verdict (`EAFNOSUPPORT`, `ENOTSOCK`, `EINVAL`).
  No `getsockopt(SO_DOMAIN)` validation is added: `SO_DOMAIN` is
  Linux-only and the check would tax every constructor on the hot path.
- **Exceptions**: none anywhere in the new surface, matching the existing
  socket types (all fallible calls return `std::error_code`).

---

## 7. Test Plan

### 7.1 Contracts that must survive untouched

- The complete TCP/UDP suite is the regression anchor and passes
  unmodified: `tests/integration/io_context/tcp_test.cpp`,
  `udp_test.cpp`, the accept/connect tests, the TCP stress suite, and the
  DNS path feeding `tcp::socket`.
- The IP `make_endpoint` rejection tests (§5.3) stay as-is.
- **Pre-implementation gate**: the full suite is green on unmodified
  `main` before any commit touches the tree. This pins the baseline every
  later diff is judged against.

### 7.2 Migration tests

Each §5.3 file keeps its assertions byte-identical (byte counts, error
codes, stop/arbitration semantics, eager/defer behavior) and only changes
how the fds are wrapped. The migrated set becomes the local family's
stream-semantics contract.

### 7.3 New tests for the local family

- **Endpoint unit tests**: path roundtrip, `path_name` / `abstract` /
  `unspecified` kinds, oversize rejection, `abstract()` absent on BSD.
- **Native address storage** (both backends): `sockaddr_un` encode for
  path and abstract forms, `make_local_endpoint` decode, unnamed decode,
  wrong-family decode → `nullopt`.
- **Stream lifecycle**: `open(stream_protocol)` → `bind(path)` →
  `listen` → `async_accept` → `async_connect` over a filesystem path;
  same over an abstract address (Linux); full-duplex echo through the
  owner types; `shutdown`/`close` semantics.
- **io_context integration**: eager on/off toggle for local reads/writes;
  defer-kind local writes; `async_accept` completing with
  `local::stream_socket`.
- **Datagram**: connected `async_send`/`async_receive`;
  `async_send_to`/`async_receive_from` with endpoint capture; unnamed peer
  decode to `kind = unspecified`.
- **Error paths**: connect to a nonexistent path (`ECONNREFUSED`), bind
  over an existing file (`EADDRINUSE`), oversized path
  (`invalid_argument`), wrong-family decode
  (`address_family_not_supported`).

---

## 8. Out of Scope

- `connect_pair` / socketpair encapsulation (Asio's `local::connect_pair`
  analog). Tests keep using raw `::socketpair` plus the owning
  constructors.
- `SOCK_SEQPACKET` (§3.4).
- Credentials and ancillary data: `SO_PEERCRED` / `SCM_CREDENTIALS`,
  `SCM_RIGHTS` descriptor passing.
- Autobind (`sun_path` auto-assignment) beyond decoding unnamed peers.
- Windows AF_UNIX and named pipes.
- A `generic::*_protocol` escape hatch for arbitrary family/type pairs.
- Layer-2 standalone-context (`io_uring_context` / `kqueue_context`)
  local sender surface (§4.2).
- Reviving the SEND_ZC feature itself: this split only removes the
  structural blocker; that feature lands on its own track.
- iostream-style facade over the local stream socket.
- RAII unlink-on-destroy for bound path sockets (§9).

---

## 9. Risks and Open Questions

All decisions affecting this design are closed (2026-10-07, user-approved);
nothing below is marked [DECIDE]. Items 1-4 record the decisions; items 5-9
follow the document's recommendations as settled.

1. **Decided (2026-10-07, user-approved) — namespace name**: `bnio::local`
   (§3.1).
2. **Decided (2026-10-07, user-approved) — v1 protocol range**: stream +
   datagram (§3.4); seqpacket deferred.
3. **Decided (2026-10-07, user-approved) — endpoint storage**: fixed-capacity
   `char` array (allocation-free, trivially copyable, consistent with
   `ip::address`).
4. **Decided (2026-10-07, user-approved) — view naming**:
   `async_io::local::stream_socket_view` (nested, Asio-like); the flat
   `async_io::local_stream_socket_view` spelling was rejected.
5. **Settled per the document's recommendation.** Model-sharing depth:
   descriptor-typed models shared by both
   families (§4.1, recommended) vs per-family model classes. Sharing
   keeps the diff minimal; per-family classes duplicate ~100 lines per
   backend for no behavioral gain today.
6. **Settled per the document's recommendation.** Stale-socket-file
   policy: bind over an existing path fails with
   `EADDRINUSE` and the caller owns `::unlink`. Asio behaves the same.
   Should bnio later offer an optional unlink helper or acceptor flag?
   Deferred, but the acceptor API should not preclude it.
7. **Settled per the document's recommendation.** Abstract namespace on
   BSD: proposed as a Linux-only factory
   (compile-guarded, §3.2). Alternative: always-present factory that
   fails at socket-call time. The guard keeps the type honest.
8. **Settled per the document's recommendation.** `sun_path` portability:
   `max_path_length` differs (108 Linux,
   104 BSD). Fixed at compile time per platform — acceptable, or should
   bnio clamp to 104 everywhere for portable path interchange?
9. **Settled per the document's recommendation.** Documentation follow-ups
   (recorded, to be done with the
   implementation, not before): `docs/project-structure.md` and
   `docs/design/architecture/header-namespace-map.md` gain the new
   headers; `docs/design/architecture.md` links this document;
   `async-io-layer.md` narrows the view table wording (§5.1);
   `roadmap.md` needs no change (QUIC-focused, no conflict).

---

## 10. References

- Asio: `local/stream_protocol.hpp`, `local/datagram_protocol.hpp`,
  `local/seq_packet_protocol.hpp`, `local/basic_endpoint.hpp`,
  `local/connect_pair.hpp`, `ip/tcp.hpp`, `basic_stream_socket.hpp`,
  `basic_socket_acceptor.hpp` (Boost.Asio local install; standalone Asio
  identical).
- Repository: types in §1.1, dispatch chain in §1.2/§4, CPOs in §2.2,
  migration inventory in §5.3.
- Historical evidence for the §1.3 incident: branch `feat/send-zc`
  (commit `83798dc`) — referenced for diagnosis only; this proposal does
  not depend on it.
