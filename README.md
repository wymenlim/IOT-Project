## Distributed Multi-Hop Wireless Button Game System

A fair, low-latency, distributed multiplayer button-press game built on ESP-NOW and AODV routing, where the winner is determined by true reaction time — not packet arrival order.

# Protocol Reference

This document describes the packet types used in the project, the high-level packet flow during route discovery and gameplay, and how each packet type is handled by button nodes and the server.

## Packet Types

All packets use `GamePacket` from [game_protocol.h](/Users/wymenlim/Documents/IOT/IOT-Project/game_protocol.h).

Fields:

- `type`: packet kind
- `origin_mac`: original creator of the packet
- `dest_mac`: final destination
- `sender_mac`: current sender on this hop
- `packet_id`: packet identifier
- `hop_count`: number of relays so far
- `reaction_ms`: used by `PRESS`, otherwise `0`
- `ttl`: remaining relay budget

Defined packet types:

- `PACKET_RREQ = 1`
- `PACKET_RREP = 2`
- `PACKET_GO = 3`
- `PACKET_PRESS = 4`
- `PACKET_RERR = 5`
- `PACKET_ACK = 6`
- `PACKET_RESULT = 7`

Currently used in the protocol:

- `RREQ`
- `RREP`
- `GO`
- `PRESS`
- `ACK`
- `RESULT`

Currently defined but not used:

- `RERR`

## AODV-Style Discovery Phase

The project uses a simplified AODV-like flow to discover routes.

### 1. Button node sends `RREQ`

Button nodes send an initial `RREQ` on boot. They can also send a new `RREQ`:

- when `PRESS` cannot be routed
- when ACK times out
- when `BtnB` is pressed manually

The `RREQ` is broadcast.

### 2. Intermediate nodes handle `RREQ`

When a node receives an `RREQ`:

- it ignores self-originated requests
- it learns a reverse route to `origin_mac`
- it drops duplicates using the seen table
- if it is not the final destination and `ttl > 1`, it relays the `RREQ`

### 3. Server receives `RREQ`

If the server is the destination of the `RREQ`:

- it creates an `RREP`
- it sends the `RREP` back toward the requester using the sender it just heard from

### 4. Intermediate nodes handle `RREP`

When a node receives an `RREP`:

- it drops duplicates
- it learns a forward route to `pkt.origin_mac` (the node that originated the `RREP`)
- if it is not the final requester, it forwards the `RREP` along the reverse path toward `pkt.dest_mac`

### 5. Requester finishes discovery

When the original requester receives the `RREP`:

- route discovery is complete
- the route table now contains a route to the server

## Game Phase

The gameplay runs on top of the discovered routes.

### 1. Server starts a round with `GO`

When the server button is pressed:

- the server tries to send `GO` to each player using `sendViaRoute(...)`
- only players with valid routes become active for that round
- if no player has a route, the round does not start

### 2. Button node receives `GO`

If `GO` is for another node:

- the node forwards it using the route table
- duplicate `GO` packets are dropped using the seen table

If `GO` is for this node:

- the node starts the reaction timer
- any stale pending press / ACK wait state is cleared

### 3. Button node sends `PRESS`

When the user presses `BtnA` during an active round:

- the node builds a `PRESS`
- it tries to send it toward the server using `sendViaRoute(...)`

If routing fails:

- the node stores the packet as `pendingPress`
- it sends a new `RREQ`
- it retries later when a route is available

If sending succeeds:

- the node enters `awaitingAck`
- it waits up to `PRESS_ACK_TIMEOUT_MS`

### 4. Intermediate nodes forward `PRESS`

If a button node receives a non-local `PRESS`:

- it drops duplicates
- it forwards the packet using the route table

### 5. Server accepts `PRESS`

When the server receives a valid `PRESS` for the current round:

- it records the player result
- it immediately sends an `ACK` back to `pkt.origin_mac`
- if that player already sent a press, it re-sends the same ACK instead of changing round state

### 6. Button node receives `ACK`

If `ACK` is non-local:

- the node drops duplicates
- forwards it using the route table

If `ACK` is local:

- the node checks that it matches the pending `PRESS`
- clears `pendingPressValid`
- clears `awaitingAck`
- stops retry behavior

If ACK does not arrive before timeout:

- the node invalidates the server route
- sends a new `RREQ`
- retries the queued `PRESS`

### 7. Server decides the winner and sends `RESULT`

When all active players for the round have pressed:

- the server marks `winnerPending = true` in the receive callback
- `loop()` later calls `declareWinner()`
- the server sends `RESULT` only to active players using `sendViaRoute(...)`

### 8. Button node receives `RESULT`

If `RESULT` is non-local:

- the node drops duplicates
- forwards it using the route table

If `RESULT` is local:

- the node resets round state
- goes back to waiting for `GO`

## How Each Packet Type Is Handled

### `RREQ`

Origin:

- button node

Purpose:

- discover a route to the server

Button node handling:

- ignore if self-origin
- learn reverse route to requester
- drop duplicates
- if local destination, send `RREP`
- otherwise relay if `ttl > 1`

Server handling:

- same reverse-route learning
- if destination is server, send `RREP`

### `RREP`

Origin:

- server or destination node answering an `RREQ`

Purpose:

- return route information to the requester

Button node handling:

- drop duplicates
- learn route to `origin_mac`
- if local destination, discovery is complete
- otherwise forward using route table

Server handling:

- same forwarding pattern when needed

### `GO`

Origin:

- server

Purpose:

- start a reaction round

Button node handling:

- if non-local, forward using route table
- if local, start timer and show `GO`
- duplicate `GO` packets are dropped

Server handling:

- sent to each active player using `sendViaRoute(...)`

### `PRESS`

Origin:

- button node

Purpose:

- submit reaction time to server

Button node handling:

- local node creates and sends it
- non-local button nodes relay it
- source retries if route or ACK fails

Server handling:

- only accepted if round is active
- only accepted for active players in that round
- duplicates cause ACK resend

### `ACK`

Origin:

- server

Purpose:

- confirm end-to-end receipt of `PRESS`

Button node handling:

- non-local nodes relay it
- local destination clears pending press state

Server handling:

- created immediately after accepted `PRESS`
- created again for duplicate `PRESS` retries
- uses the same `packet_id` as the original `PRESS`

### `RESULT`

Origin:

- server

Purpose:

- tell players the round is complete

Button node handling:

- non-local nodes relay it
- local destination resets to waiting state

Server handling:

- sent only to active players
- uses `sendViaRoute(...)`

## Reliability Notes

- Route discovery is demand-driven, not fully periodic.
- `PRESS` reliability is end-to-end because of `ACK`.
- If a route exists but first-hop transmit fails immediately, `sendViaRoute(...)` now returns `false`.
- Packet dedup currently uses the seen table to suppress repeated relays.
- Button-node receive callbacks should stay lightweight; expensive UI work is deferred to the main loop.

## Current Manual Controls

Button nodes:

- `BtnA`: reaction press during game
- `BtnB`: manual route rediscovery (`RREQ`)

Server:

- `BtnA`: start round, or end early if a round is already active
