# Roadmap

The next major milestone for bnio is QUIC transport support.

The groundwork for this is in place. The sender/receiver execution
model already gives every asynchronous operation a lazy, composable shape,
and the platform-native event loops for `io_uring` and `kqueue` provide the
completion-driven foundation a QUIC implementation needs. The existing UDP
transport covers the datagram layer QUIC builds on, and the TLS facilities —
the certificate and configuration surface of the internal `ssl` base layer,
together with ALPN negotiation — map directly onto the QUIC handshake,
which embeds TLS 1.3 and relies on ALPN for protocol selection.

The goal is a QUIC transport that feels native to bnio: first-class
sender/receiver interfaces for QUIC connections and streams, an internal
layering consistent with the existing `ssl` design, and support across
both platform backends.

Design details will be documented under [`design/`](design/) as the
implementation takes shape.
