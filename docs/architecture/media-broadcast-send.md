# Media broadcast sends on Windows

The video-broadcast watchdog caught the send thread inside `WSASendMsg`.
Previously, that call and its per-datagram fallback ran under the shared live
session lock. Unregistering any session needed that lock exclusively, so
one stalled media send could also stall session join.

`WSASendMsg` with both completion arguments null takes the synchronous
path even when the socket supports overlapped I/O. A blocking UDP send
can wait for transport send-buffer capacity. Without a send timeout the
default wait is unbounded. `SO_SNDTIMEO` applies to blocking calls and
requires the overlapped creation flag for sockets created with
`WSASocket`. A timeout also carries a documented socket recovery
requirement. These facts explain how a wait can outlast the watchdog.
USO batches place a larger message into the send path and may rely on the
miniport and NIC for segmentation. The dump alone cannot distinguish send
buffer pressure from a problem in the driver or the NIC.

The media queues carry a registration ID. On dequeue, the broadcaster
takes a strong reference under the registry mutex, releases the mutex,
and captures the destination and source addresses and port. Unregistering
removes the registration without waiting for a reference already in use.
The senders check the session state before submission and between fallback
sends. One send already in progress can still finish. The session join
thread releases the shared broadcast context so the last snapshot cannot
shut down its own worker. Queued video packets own any encoder replacements
they still need.

Boost.Asio 1.92.0 sets the two native Windows UDP sockets to nonblocking
mode at startup. Asynchronous ping operations continue to use Asio.
The video sender retains `UDP_SEND_MSG_SIZE` with `WSASendMsg`. It uses
individual datagrams when the batch path fails for another reason. On
`WSAEWOULDBLOCK` it drops the unaccepted portion of a paced batch without
retrying or queueing it. Audio data and parity both drop the affected
datagram on backpressure. Sending progress still advances for live MAT,
and genuine send failures still stop it. The stream diagnostic counter
`send_would_block_packets` records these datagrams even in windows with
too few sent video frames for latency statistics. A chunk with these
drops can be reported even if every window was idle. Pacing still
charges the full batch.
The Windows sockets close after their receive and media send workers exit.
Asio completion handlers retain the receive buffers and sender endpoints
through cancellation. If setting nonblocking mode fails, startup closes
the sockets it opened.

The steady-state batch still uses one send call and the existing
scatter/gather buffers. Taking a snapshot adds one reference acquisition
and release per dequeued frame or audio packet and copies endpoint
metadata. It adds no media payload copy or per-send allocation. Encoder
replacement strings are copied only when the codec creates those
replacements. The design target is zero socket-buffer wait in the Windows
media sender and zero registry-lock wait on an in-flight send. Hardware
timings and throughput have not been measured.

Microsoft's MsQuic 2.6.4 `DatagramSend` supports queueing and optional
cancellation when blocked. This existing UDP path instead opts out of
creating another media queue. An overlapped send with cancellation would
require completion tracking and longer buffer ownership, and a transport
change would require separate peer negotiation. The packet format is
unchanged. RFC 8085 §3.1 covers the existing concern of paced UDP sending.
RFC 8899 updates path MTU discovery guidance and RFC 9868 specifies UDP
options. This change does not alter path MTU discovery or use UDP options
on the wire. Linux keeps its existing GSO/`sendmmsg` path and macOS keeps
its `sendmsg` path.

Primary source records, reviewed 2026-11-20, are in
[`source-index.tsv`](../authority/source-index.tsv). The Windows behavior
uses MS-WSASENDMSG-20261120 (Return value, Remarks),
MS-SO-SOCKET-20261120 (SO_SNDTIMEO), MS-OVERLAPPED-IO-20261120 (Overview),
MS-SENDTO-20261120 (Remarks), MS-UDP-OPTIONS-20261120 (UDP_SEND_MSG_SIZE),
MS-UDP-USO-20261120 (Overview, Transmitting UDP packets with USO),
MS-CLOSESOCKET-20261120 (Remarks), and BOOST-ASIO-NATIVE-NONBLOCKING-192
(Parameters, Example). Handler lifetime follows BOOST-ASIO-ASYNC-RECV-192
(Parameters) and BOOST-ASIO-IO-CONTEXT-DTOR-192 (Remarks).
