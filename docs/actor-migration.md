# Poseidon Actor-Model Migration Plan

> **Status:** Draft v2.1 — open for red-line review. v2.1 folds in the **B4 decision** (wholesale `Platform/` vendoring, supersedes the v2 B2 "keep poseidon's own" framing) and the **log.h no-action** decision.
> **Scope:** Replace every mutex/condition-variable/refcounter-driven concurrency primitive in Poseidon with the liboffs actor model. Preserve poseidon's external behavior, modernize the C ABI, ship a hardened vendored actor system that is *poseidon-specific* in name, layout, and policy.
> **Branch:** *awaiting name from user — see § 0 row 0.*

---

## 0. Decisions Locked From The User

| #  | Decision                                                                                                              | Implication |
|----|-----------------------------------------------------------------------------------------------------------------------|-------------|
| 0  | **A new git branch is required for this work.** *(name locked: `actor-migration`)*                                    | All work in this plan lands on branch `actor-migration`. No commits land on the default branch until the migration is reviewable. The branch is the unit of work; review of this plan is the gate. |
| 1  | Plan is a written document                                                                                            | This file. Persisted at `poseidon/docs/actor-migration.md`. |
| 2  | **No symlink, no path reference to `/path/to/liboffs`. Vendor a copy and rename to poseidon-specific surface.**       | All actor/scheduler/queue machinery moves into `poseidon/src/Actor/` with `poseidon_` prefix and poseidon `STYLEGUIDE.md` rules. liboffs is the source of shape, not a build dependency. |
| 3  | **Surgical hand-transplant. No scripts. No commits pulled from liboffs.** *(user, verbatim: "This needs to be surgical we transplant what we need from liboffs with explicit attention to detail and context not using a script")* | Each file is copied by hand, read in full first, then adapted to poseidon naming and poseidon's `STYLEGUIDE.md`. No `vendor-sync.sh`, no `git subtree`, no `git filter-branch`, no sed-awk transform pipeline. Every transplant is a discrete commit with a justification comment. |
| 4a | **Add `good-actors` skill as the concurrency authority in `STYLEGUIDE.md`.**                                          | A new "Actor Concurrency" section is added once, citing the skill path. The skill is the rulebook; the section is a pointer + poseidon-specific context. |
| 4b | **Reshape Poseidon's C ABI to mirror liboffs's ClientAPI ABI.**                                                       | Public functions become `poseidon_<verb>_<noun>_action` style, with the same split: transport owns the I/O thread, handlers own the dispatch, results return through `message_t` from `actor_send`. |
| 4c | **Old `poseidon_*` ABI symbols are deleted outright. No deprecation, no two-symbol shim, no compatibility wrapper.** *(user, verbatim: "We do not need a deprecation of abi's they are not in production use. You can blow them away.")* | When the actor version of a function lands, the old function's `.c` body is removed in the same commit. Headers drop the old prototype in the same commit. There is exactly one source of truth per symbol. There is no "phase out" period. There is no deprecation `#warning`. |
| 5  | **RefCounter: single canonical version = liboffs's `packed_state` single-CAS layout. Poseidon's `src/RefCounter/` is deleted and replaced wholesale. No two-symbol shim.** *(user, verbatim: "I think you need to find the best version of refcounter and use that do not try to maintain two symbols. Integrate the best portions into one. I think liboffs' has been used more.")* | The directory `poseidon/src/RefCounter/` is replaced 1:1 with the liboffs `src/RefCounter/` contents, adapted to poseidon's prefix and naming. The diagnosis is in § 2 below. |
| 6  | **Vendoring target includes the `stream_t` actor + the HTTP transport. Both are load-bearing and essential.** *(user, verbatim: "You should also note a certain type of actor the stream_t. This a load bearing flow control pattern that may be needed in this codebase. Especially where we need http interfaces you should copy it and any http machinery as being essential.")* | `poseidon/src/Streams/stream.{c,h}` and `poseidon/src/ClientAPI/HTTP/*` (20 files) are part of the initial vendor set, not "stretch goals". Detail in § 3. |

### Open decisions still needing user input (after red-lines)

- **B.** Build wiring. The user's correction: "Compile the code as if it was native to poseidon use the cmake conventions already present." The exact shape of that follow-through is in § 7.5. To summarize: poseidon's root `CMakeLists.txt` uses a flat-list pattern with one `*_SRC_DIR` variable per directory and a single `POSEIDON_SOURCES` list that the library target pulls from. The transplanted code follows the same pattern — no `add_subdirectory`, no per-subdirectory `CMakeLists.txt`, no foreign build structure. The liboffs per-directory `CMakeLists.txt` files are *not* vendored; the sources are listed directly in poseidon's root `CMakeLists.txt` with poseidon's prefix. This is consistent with the § 0 row 2 rule: liboffs is the shape source, not a build dependency.

**The lock-in.** After Phase 1, this decision is not reopened. If a transplanted module grows enough sources to warrant its own `CMakeLists.txt` later, that is a separate refactor, not a migration concern.

---

## 1. What This Migration Is

Today Poseidon is:

- A **C pub/sub mesh** with five subsystems: Quasar (routing), Meridian (gossip/relay/rendezvous/ring/stream), Channel (manager/topic/subtopic/alias/notice/message), Workers (pool/queue/promise/work), WaveClient.
- Synchronization = **17+ headers** using `PLATFORMLOCKTYPE` + `PLATFORMCONDITIONTYPE` + `PLATFORMBARRIERTYPE` + a `refcounter_t`-first convention. Single-manager-wide mutexes in `poseidon_channel_manager_t` and `poseidon_quasar_t`.
- I/O = `msquic_singleton` as the single QUIC I/O thread + handle registry, talking to per-connection state via shared locks.
- ABI = `poseidon_*` C functions, currently *callable but not actor-shaped*.

After this migration Poseidon is:

- A C pub/sub mesh where every long-lived stateful object is an **actor** (`poseidon_actor_t`).
- Synchronization = **`actor_send(message_t*)` + `actor_t::queue_state` (IDLE/QUEUED/RUNNING) + backpressure apply/release + mute-park discipline**, full stop. No `PLATFORMLOCKTYPE` in business code. Locks survive only inside actor internals and inside `Platform/` (which the actor code uses to park).
- I/O = transports (`HTTP/TCP/Unix/WS/WT`) own a thread per accepted connection (or a single-threaded epoll loop for HTTP), and **only** do I/O. They parse frames and call `poseidon_<verb>_<noun>_action(ctx, cbor_item_t*)` which dispatches a message into the appropriate actor's queue. Handlers never touch the socket.
- ABI = `poseidon_<verb>_<noun>_<action>` style functions and `poseidon_actor_send` / `poseidon_actor_recv` style message passing. Old `poseidon_*` functions that did I/O on the caller's thread are *gone*, not deprecated. See § 0 row 4c.

---

## 2. RefCounter — Why liboffs's Version Wins

Poseidon's `src/RefCounter/refcounter.h` (36 lines) has **two independent atomics**:

```c
typedef struct refcounter_t {
  ATOMIC_TYPE(uint_fast16_t) count;
  ATOMIC_TYPE(uint_fast8_t)  yield;
} refcounter_t;
```

`poseidon/src/RefCounter/refcounter.c` (62 lines) implements yield as a CAS on the `yield` field, then a follow-up `count` add — two separate atomic operations, not one transaction.

liboffs's `src/RefCounter/refcounter.h` (56 lines) defines `OFFS_ATOMIC` unconditionally, making the `packed_state` path the only reachable layout:

```c
typedef union {
  struct __attribute__((packed)) {
    uint16_t count;    // references held
    uint8_t  yield;    // "destroy has begun, refuse new refs"
    uint8_t  pending;  // deferred-deref escrow
  } s;
  uint32_t bits;
} refcounter_state_t;

struct refcounter_t {
  OFFS_ATOMIC(refcounter_state_t) state;
};
```

The whole escrow (count + yield + pending) is a single 32-bit CAS. liboffs's `refcounter.c` (198 lines) also adds four functions poseidon lacks:

- `refcounter_init_actor` — initializer for the actor-thread destroy path
- `refcounter_dereference_is_zero` — peek without decrement (used in GC keep-filter)
- `refcounter_pending_derefs` — read the pending escrow (used in shutdown to drain)
- `refcounter_destroy_lock` — set the "destroy in progress" gate before teardown
- `refcounter_consume` — transfer a reference without bumping the count

**Why this matters for the migration.** Poseidon's split-atomics version is exactly the race liboffs's `packed_state` was designed to eliminate: yield-CAS succeeds, then a concurrent dereference runs, then the count-add runs — and the dereference that ran *during* the destroy window was never observed by the destroy path. The liboffs version packs the whole transfer into one CAS, so the destroy path sees a single coherent snapshot.

**Action.** Delete `poseidon/src/RefCounter/refcounter.{c,h}` and the corresponding `CMakeLists.txt` entry. Vendor `liboffs/src/RefCounter/refcounter.{c,h}` as-is into `poseidon/src/RefCounter/`, then apply poseidon's prefix and naming. **Keep the bare `refcounter_t` name** — every other header in poseidon forward-declares it, renaming ripples across the entire codebase. The poseidon `STYLEGUIDE.md` rule "refcounter is the first member of every long-lived struct" still holds; the only change is the underlying layout.

The atomic operations used by `refcounter` (`OFFS_ATOMIC_*` in liboffs) come from liboffs's `Util/atomic_compat.h`. After transplant, those macros are reachable through poseidon's own `Util/atomic_compat.h` — which is itself vendored from liboffs and re-guarded as `POSEIDON_ATOMIC_COMPAT_H` (see § 6 for the full B4 vendor order). Poseidon's prior `ATOMIC_TYPE` macro is renamed to `ATOMIC` to match the vendored surface. Three cascade sites: `Workers/join.h`, `Workers/queue.h`, `RefCounter/refcounter.h`.

---

## 3. The `stream_t` Actor — Load-Bearing Flow Control

The user flagged `stream_t` as a load-bearing pattern. Here is the actual shape in liboffs:

`liboffs/src/Streams/stream.h` defines `stream_t` as an **actor** (line 171: `actor_t actor;`):

```c
struct stream_t {
  refcounter_t refcounter;
  stream_type_e  type;        // readable / writeable / duplex / transform
  stream_force_e force;       // push / pull
  ATOMIC(size_t) next_handler_id;
  stream_event_handler_list_t* handlers[STREAM_HANDLER_COUNT];
  uint8_t readable;
  uint8_t is_piped;
  uint8_t auto_push;
  uint8_t is_pulling;
  uint8_t is_deactivated;
  uint8_t close_undelivered;
  stream_t* pullable_stream;
  stream_notifier_t* pipe_notifiers;
  scheduler_pool_t* pool;
  actor_t actor;                       // <-- this is the load-bearing line
  void (*on_data)(stream_t*, void*);
  void (*on_push)(stream_t*);
  void (*on_pull)(stream_t*);
  void (*on_read)(stream_t*, size_t, void*, void (*)(void*, void*));
  void (*on_write)(stream_t*, void*);
  void (*on_close)(stream_t*);
  void (*on_deactivated)(stream_t*);
  void (*on_close_read)(stream_t*);
  void (*on_close_write)(stream_t*);
  void (*on_pipe)(stream_t*, stream_t*);
  void (*on_piped)(stream_t*, stream_t*);
  void (*destructor)(stream_t*);
};
```

The header's own comment (lines 152-156) states the discipline:

> *"The handler lists are owned by the stream's actor: stream_subscribe, stream_unsubscribe, stream_once and stream_notify all route through actor messages, so the lists are only ever read and mutated on the actor thread — no lock is needed."*

**This is the pattern.** A `stream_t` is a Node-EventEmitter-shaped actor: callers register handlers, then events are dispatched to those handlers **on the actor thread**. Subscribe / unsubscribe / once / notify are all message sends. There is exactly one thread that ever reads or mutates `handlers[]`. There is no mutex on the handler list. There is no read-copy-update. There is one writer (the actor) and many readers (handler call sites) that run *under* the actor.

`stream_t` is the **generic** flow-control actor. The specialized flavors live in two other directories and are **consumers** of `stream_t`, not separate patterns:

- `liboffs/src/OFFStreams/` — `readable_off_stream_t`, `writeable_off_stream_t` (each contains a `stream_t stream` member). These are the HTTP request/response flow-control vocabulary: an `off_stream` wraps the body of an HTTP exchange as a stream so request parsing, response writing, and chunked transfer all use the same handler-on-actor-thread discipline. Paired with `representation_actor_t` (which is the actor that walks representation descriptor chains and is the consumer of the keep-filter logic), this is the *essential* shape for any HTTP interface in poseidon.
- `liboffs/src/Streams/file-stream.{c,h}` — `readable_push_file_stream_t`, `writeable_push_file_stream_t`, `readable_pull_file_stream_t`, `writeable_pull_file_stream_t`. These wrap file I/O as streams. Poseidon's existing `src/Streams/` directory may or may not overlap; check before transplanting.

**Action for poseidon.** The initial vendor set includes:

1. `poseidon/src/Streams/stream.{c,h}` — the generic `stream_t` actor, its dispatch function `stream_dispatch`, and the message payload structs (`stream_subscribe_payload_t`, `stream_notify_payload_t`, `stream_close_payload_t`, `stream_pipe_payload_t`, `stream_deactivate_payload_t`, etc.). Namespaced to `poseidon_stream_t` etc.
2. `poseidon/src/Streams/stream_event_handler.{c,h}` — `stream_event_handler_t`, the `STREAM_HANDLER_COUNT` enum, the intrusive-list nodes. Same handler-on-actor-thread discipline.
3. `poseidon/src/OFFStreams/` — `representation_actor.{c,h}`, `readable_off_stream.{c,h}`, `writeable_off_stream.{c,h}`, plus the supporting types: `readable_descriptor.{c,h}`, `writeable_descriptor.{c,h}`, `block_recipe.{c,h}`, `ofd.{c,h}`, `ofd_cache.{c,h}`, `off_url.{c,h}`, `ori.{c,h}`, `tuple.{c,h}`, `tuple_cache.{c,h}`. Namespaced to `poseidon_representation_actor_t` etc.
4. **Read poseidon's existing `src/Streams/` first** to see what overlaps with liboffs's `Streams/file-stream.{c,h}`. Some of poseidon's existing streams may already do the right thing; if so, port them to the `stream_t`-actor shape instead of wholesale-replacing.

**What `stream_t` replaces in poseidon.** The pattern of "a long-lived object with a per-instance mutex that protects a callback list" — anything currently using `PLATFORMLOCKTYPE` to guard a `void (*on_data)(...)` array. The dominant candidate is poseidon's `Meridian/stream.h` (relay/stream subsystem), which currently has the lock-and-callback shape `stream_t` was built to eliminate.

---

## 4. The HTTP Transport — Why It's Essential

The user called this out explicitly: "Especially where we need http interfaces you should copy it and any http machinery as being essential."

`liboffs/src/ClientAPI/HTTP/` contains 20 files (per `find`):

```
http_server.{c,h}            — epoll-based HTTP listener
http_connection.{c,h}        — accepted-connection state
http_request.{c,h}           — request parser
http_response.{c,h}          — response writer
http_route.{c,h}             — route table + dispatch
http_headers.{c,h}           — header parsing
http_status.h                — status-code enum
cors.{c,h}                   — CORS preflight + headers
auth_middleware.{c,h}        — auth gate per route
```

Plus the route handlers:

```
block_routes.{c,h}           — block-level operations over HTTP
config_routes.{c,h}          — configuration get/set over HTTP
health_routes.{c,h}          — health probes
off_routes.{c,h}             — off-stream operations
peer_routes.{c,h}            — peer management
```

The shared handler contract is the `block_handler_ctx_t` (defined in `liboffs/src/ClientAPI/block_handlers.h`):

```c
typedef struct {
  block_connection_t* conn;
  block_cache_t*      bc;
  actor_t*            actor;
  uint8_t             is_authenticated;
  block_send_frame_fn send_frame;
  block_send_error_fn send_error;
  pending_op_t*       pending;
  put_encoding_e      put_encoding;
  const char*         data_dir;
} block_handler_ctx_t;

int block_handle_cache_result(block_handler_ctx_t* ctx, message_t* msg);
```

**Why HTTP is essential and not "later".** Poseidon's existing `ClientAPI/` directory has six transport families (`quic tcp unix ws webtransport xpc`) but **no HTTP**. The user is signalling that HTTP is a required target surface — the migration must not land in a state where the actor model is in but HTTP is still missing. The HTTP machinery is what makes the `stream_t` actor flow control visible to clients.

**Action for poseidon.** The initial vendor set adds `poseidon/src/ClientAPI/HTTP/` with all 20 files, namespaced. Poseidon's existing six transport families (`quic tcp unix ws webtransport xpc`) are kept but reshaped to match the liboffs transport contract: each transport becomes a thin I/O-only layer that parses frames and calls into the shared `block_handlers.{c,h}` / `cache_handlers.{c,h}` / `peer_handlers.{c,h}` / `config_handlers.{c,h}` / `health_handler.{c,h}` / `update_status_handler.{c,h}` / `representation_api.{c,h}` set. The handler functions are transport-agnostic — they take a `block_handler_ctx_t*` and a CBOR item, and they `actor_send` a message into the appropriate actor.

**The `block_connection_t` typedef is a deliberately incomplete type** (`typedef void block_connection_t;` in liboffs). This is what lets the same `block_handler_ctx_t` work across all transports: each transport defines its own concrete connection struct in private, and the handler only ever sees a `void*` it can pass back through `send_frame` / `send_error`. Poseidon's equivalent must use the same incomplete-typedef trick to avoid leaking transport internals into the handlers.

---

## 5. The Full Vendor Set (Initial Transplant)

In dependency order. Each item is a hand-transplant commit; nothing lands as a bulk import.

| # | Source (liboffs)                                                        | Destination (poseidon)                                | Notes |
|---|-------------------------------------------------------------------------|-------------------------------------------------------|-------|
| 1 | `src/RefCounter/refcounter.{c,h}`                                       | `poseidon/src/RefCounter/refcounter.{c,h}`            | See § 2. Wholly replaces poseidon's existing files. |
| 2 | `src/Actor/actor.{c,h}` + `message.{c,h}` + `message_queue.{c,h}` + `pool.{c,h}` | `poseidon/src/Actor/`                                 | The foundation. Namespaced to `poseidon_actor_t`, `poseidon_message_t`, `poseidon_message_queue_t`, `poseidon_actor_pool_t`. |
| 3 | `src/Scheduler/deque.{c,h}` + `scheduler.{c,h}`                        | `poseidon/src/Scheduler/`                             | Work-stealing deques + scheduler pool. Namespaced. |
| 4 | `src/Streams/stream.{c,h}` + `stream_event_handler.{c,h}`               | `poseidon/src/Streams/stream.{c,h}` + `stream_event_handler.{c,h}` | See § 3. Read poseidon's existing `Streams/` first. |
| 5 | `src/OFFStreams/` (full directory)                                     | `poseidon/src/OFFStreams/`                            | See § 3. |
| 6 | `src/ClientAPI/HTTP/` (20 files)                                        | `poseidon/src/ClientAPI/HTTP/`                        | See § 4. |
| 7 | `src/ClientAPI/block_handlers.{c,h}` + `cache_handlers.{c,h}` + `config_handlers.{c,h}` + `health_handler.{c,h}` + `peer_handlers.{c,h}` + `update_status_handler.{c,h}` + `representation_api.{c,h}` + `client_api_wire.{c,h}` + `client_api_transport.h` | `poseidon/src/ClientAPI/`                             | Transport-agnostic handler set. Namespaced. |
| 8 | `src/ClientAPI/TCP/` + `Unix/` + `WS/` + `WT/`                          | `poseidon/src/ClientAPI/TCP/` + `Unix/` + `WS/` + `WT/` | Each transport adapted to poseidon's existing transport contract. (HTTP is in row 6.) |

**Order matters.** RefCounter (#1) is the foundation. Actor + Scheduler (#2, #3) are the runtime. `stream_t` (#4) and `OFFStreams/` (#5) build on Actor + Scheduler. HTTP and the shared handler set (#6, #7) build on Actor + Stream. Transports (#8) are the last layer because they depend on the handler context that #6, #7 define.

Each row above is a series of commits, not one commit. The "row commit" boundary is: one logical concern transplanted, fully compilable, poseidon's `STYLEGUIDE.md` honored, no dangling references.

---

## 6. What Stays Poseidon-Specific (Not Vendored)

These subsystems are poseidon's domain knowledge and do not have a liboffs equivalent that adds value:

- `poseidon/src/Quasar/` — the routing core (per-topic, per-pattern, per-alias routing)
- `poseidon/src/Meridian/` — gossip, relay, rendezvous, ring. (The `stream` *subsystem* here is candidate for `stream_t` port, per § 3.)
- `poseidon/src/Channel/` — topic/subtopic/alias/notice/message. Re-architected as actors but the schema is poseidon's.
- `poseidon/src/Workers/` — pool/queue/promise/work. The `work_pool_t` becomes an actor; the worker functions become `WORK_*` message handlers.
- `poseidon/src/Network/WaveClient/` — poseidon's wave protocol
- `poseidon/src/HashMap/`, `Buffer/`, `Util/`, `Bloom/` — shared utility code, kept under poseidon's own conventions
- `poseidon/src/Util/threadding.h` — poseidon's own platform-primitive layer (`platform_lock`/`platform_unlock`/`platform_condition_*`/`platform_barrier_*`/`platform_usleep`/`cpu_relax`/`spinlock_t`/`CACHE_LINE_SIZE`/`CACHE_ALIGNED`). Different API surface from liboffs's `Platform/platform_thread.h` (which uses `platform_mutex_*`/`platform_condvar_*`/`platform_barrier_*` — no symbol-name collision). Stays as poseidon's own. Poseidon's own code calls these primitives directly; the vendored liboffs code calls its own vendored `Platform/*` directly.
- `poseidon/src/Platform/` — **NOT kept.** This directory is **wholesale-replaced** by the vendored liboffs `Platform/` (24 source files, ~3,339 lines, see § 6.1 below). Poseidon's own threadding/atomic primitives above are **not** in this directory; they live in `Util/`. So the vendoring is a clean directory replacement, not an in-place merge.
- `poseidon/deps/` — blake3, googletest, hashmap, libcbor, libwtf, msquic, poll-dancer, xxhash. Not liboffs.

The `BlockCache/` subsystem in liboffs has a poseidon analog (the block storage layer) — **read both before transplanting**. They may not match. Poseidon's block storage has poseidon-specific metadata that liboffs's doesn't.

---

## 6.1 The B4 Vendor Order — Platform-First

The earlier draft of this plan (v2 § 6) kept `poseidon/src/Platform/` as poseidon's own. The user rejected that: the socket layer should match liboffs's, and the actor internals (`Actor/message_queue.c`, `Actor/pool.c`, `Scheduler/scheduler.c`) call `platform_*` primitives by name. The vendoring must bring the primitives in too, otherwise the vendored code won't compile in poseidon's tree.

The replacement is **wholesale**, not surgical. Poseidon's existing `src/Platform/` directory is a small handful of files (one platform-primitive header, possibly an atomic shim). Liboffs's `src/Platform/` is 24 source files. The directories are not file-by-file compatible. The surgical rule is preserved at the **file** level (one file at a time, hand-transplant, with vendored-modifications comment), not at the **directory** level.

### 6.1.1 What `Platform/` Vendors

All 24 source files in `liboffs/src/Platform/`, verbatim (header guards renamed `OFFS_*` → `POSEIDON_*` where present):

```
Platform/platform.h                       # umbrella, 14 lines, includes 9 subheaders
Platform/platform_atomic.{c,h}            # atomic operations
Platform/platform_compiler.h              # compiler/platform attribute macros
Platform/platform_condition.h             # legacy condition var wrapper
Platform/platform_local.{c,h}             # thread-local storage
Platform/platform_mutex.h                 # mutex typedef + ops
Platform/platform_posix_compat.h          # POSIX compatibility shim
Platform/platform_rwlock.h                # read/write lock
Platform/platform_signal.h                # signal handling
Platform/platform_socket.h                # socket API
Platform/platform_socket_internal.h       # socket internal helpers
Platform/platform_spinlock.{c,h}          # spinlock
Platform/platform_thread.{c,h}            # thread + mutex + rwlock + condvar + barrier typedefs and ops
Platform/platform_time.{c,h}              # time, sleep
```

**Not vendored:** `Platform/CMakeLists.txt`. Liboffs's per-directory CMakeLists is build glue; poseidon has its own build glue (root `CMakeLists.txt` flat-list with explicit per-file listing — see § 7.5). The 24 source files are added to poseidon's root `CMakeLists.txt` near line 51, mirroring the `REFCOUNTER_SRC_DIR` pattern at line 51 and the source listing at line 85.

### 6.1.2 The Order (Phase 1 Commit Sequence)

The vendor order is fixed by **build dependency**, not by the order in § 5. The first commit in Phase 1 is the smallest one that compiles, and each commit compiles independently. The order is:

| Commit | What lands | Why this order |
|---|---|---|
| 1.1 | Replace `Util/atomic_compat.h` with vendored liboffs copy (guard `OFFS_ATOMIC_COMPAT_H` → `POSEIDON_ATOMIC_COMPAT_H`). Cascade `ATOMIC_TYPE` → `ATOMIC` in `Workers/join.h:19`, `Workers/queue.h:29`, `RefCounter/refcounter.h:36`. Update vendored-modifications comment block in `RefCounter/refcounter.{c,h}` (the `ATOMIC_TYPE` reversion is now correct, not a future task). | Atomic compat is the foundation. Same-name file conflict — poseidon's existing `atomic_compat.h` and vendored liboffs's cannot coexist in one dir. Cascade = 3 lines in 3 files + 2 comment lines. |
| 1.2 | Replace `Util/allocator.{c,h}` with vendored liboffs copy (guard rename only, byte-equivalent otherwise). Cascade = 0. | Allocator is the next-lowest dependency. Both `Util/allocator.h` and `Util/allocator.c` are byte-equivalent to liboffs's; only the guard differs (`WAVEDB_ALLOCATOR_H` → `POSEIDON_ALLOCATOR_H`). |
| 1.3 | Vendor all 24 files of `Platform/` into `poseidon/src/Platform/`. Update root `CMakeLists.txt`: add `PLATFORM_SRC_DIR` near line 51, list the 24 `.c` files explicitly in `POSEIDON_SOURCES` near line 85 (poseidon folds flat-list; no glob). No `Platform/CMakeLists.txt` is created or vendored. | The actor and scheduler internals call `platform_*` primitives by name. Without the primitives vendored, those files don't compile. The vendored primitives compile standalone (no actor / scheduler / stream code involved). |
| 1.4 | Vendor all 7 files of `Actor/` into `poseidon/src/Actor/`: `actor.{c,h}`, `message.h` (448 lines), `message_queue.{c,h}`, `pool.{c,h}`. Update `CMakeLists.txt`: add `ACTOR_SRC_DIR`, list the 4 `.c` files explicitly. | Actors depend on `Platform/` (call `platform_mutex_*`, `platform_condvar_*`, `platform_atomic_*`, `PLATFORM_THREAD_LOCAL`, `cpu_relax` via `Platform/platform_atomic.h` etc.) and on the `RefCounter` (shipped in commit `944c2d8`). RefCounter is already vendored. Platform/ is now vendored (commit 1.3). |
| 1.5 | Vendor all 4 files of `Scheduler/` into `poseidon/src/Scheduler/`: `scheduler.{c,h}`, `deque.{c,h}`. Update `CMakeLists.txt`: add `SCHEDULER_SRC_DIR`, list the 2 `.c` files explicitly. | Scheduler depends on Actor (calls `actor_send`, `actor_init`) and on `Platform/` (uses `PLATFORM_THREAD_LOCAL`, `platform_thread_*`, `platform_condvar_*`). Both vendored. |
| 1.6 | Add unit tests for `actor_t` standalone and `scheduler_pool_t` dispatch in `tests/`, using poseidon's existing `add_meridian_test()` helper. | Phase 1 deliverable: `poseidon_actor_t` exists, `poseidon_scheduler_pool_t` dispatches an actor, both unit-tested. No poseidon subsystem touched. |

### 6.1.3 What This Means for Phase 5 (Transport Reshape)

Under B4, poseidon's existing transport families — msquic/TCP/Unix/WS/WebTransport/XPC, in `poseidon/src/ClientAPIs/` — are **not** in Phase 1 scope. They keep using poseidon's own `Util/threadding.h` primitives and poseidon's own I/O path.

A future phase (5+) may opt to migrate them onto vendored `Platform/platform_socket.h`. That is **not** a Phase 1 deliverable and is not in the current plan. § 5 row 8 (liboffs `ClientAPI/TCP/Unix/WS/WT/`) is therefore **out of scope** for the current plan. If the user later wants the liboffs transports vendored, that is a new plan, not a re-spin of this one.

### 6.1.4 What About the Existing `Platform/` Files in Poseidon?

Poseidon's current `src/Platform/` is, in practice, very small: most platform-primitive code lives in `Util/threadding.h` and `Util/atomic_compat.h`. The wholesale-replacement framing assumes the directory is either empty or contains a small amount of poseidon-specific code. **Action when the vendor begins:** `ls poseidon/src/Platform/` first. If files exist there that are not from liboffs, they are either (a) deleted if liboffs has a functional equivalent, or (b) moved to `Util/` if they are poseidon-specific primitives that the vendored liboffs code does not need. This is decided at vendor-time, not plan-time.

### 6.1.5 What About the `log.h`?

Poseidon's existing `Util/log.h` (0.1.0) provides the standard macros `log_info` / `log_warn` / `log_error` / `log_debug` / `log_trace` / `log_fatal` that the vendored liboffs code uses (Platform/ has 16 such calls — 14 in `platform_local.c` plus 1 each in `platform_thread.c` and `platform_atomic.c`; Scheduler/ has 2 in `scheduler.c`; Actor/ has zero). No `log.h` mutation is required for this vendoring pass. If a future vendored file (e.g. one in a later phase) needs a structured/module-level log signature, that is a future round, not Phase 1.

---

## 7. Migration Phases

Each phase is a series of commits. Each commit compiles. Each commit passes poseidon's existing tests (the existing tests are the safety net for the migration's behavior preservation goal).

**Phase 1 — Foundation (vendor rows 1, 2, 3)**
- Vendor RefCounter (§ 2). The first commit is the deletion of poseidon's `src/RefCounter/`; the second is the transplant. Refcounter is the foundation, and the only safe way to swap it is wholesale.
- Vendor Actor core. Build the actor + message + message_queue + pool machinery into `poseidon/src/Actor/`. At end of phase, `poseidon_actor_t` exists and is unit-testable in isolation.
- Vendor Scheduler. Build the work-stealing deque + scheduler pool. At end of phase, `poseidon_scheduler_pool_t` can dispatch an actor.
- **No poseidon subsystem is touched in this phase.** The new code lives alongside the old. It is exercised by new unit tests.

**Phase 2 — Flow control (vendor row 4, 5)**
- Vendor `stream_t` (§ 3) with new unit tests proving the handler-on-actor-thread discipline.
- Vendor `OFFStreams/` with the keep-filter / representation-walk tests ported from liboffs.
- Begin porting poseidon's `Meridian/stream.h` (relay/stream subsystem) from mutex+callback to `poseidon_stream_t` actor shape. This is the first poseidon subsystem to use the new machinery.

**Phase 3 — Shared handler layer (vendor rows 6, 7)**
- Vendor `ClientAPI/HTTP/` (§ 4). New HTTP transport exists alongside poseidon's existing `quic/tcp/unix/ws/webtransport/xpc`.
- Vendor the transport-agnostic handler set. `block_handler_ctx_t` is the public contract; the existing six transports are reshaped to it.
- **At this point HTTP exists in the build for the first time.** The user gets to see the migration's flagship feature working end-to-end.

**Phase 4 — Subsystem actorization**
- `poseidon_channel_manager_t` (the mega-struct) is split into multiple actors: `poseidon_channel_registry_actor_t`, `poseidon_topic_actor_t`, `poseidon_subtopic_actor_t`, `poseidon_alias_actor_t`. Each is an actor. The manager-wide mutex disappears.
- `poseidon_quasar_t` is split into `poseidon_route_table_actor_t` + `poseidon_pattern_match_actor_t`. The route-table mutex disappears.
- `poseidon_work_pool_t` becomes `poseidon_worker_actor_t`. The pool's condition variable + barrier + work-queue triple is replaced by a message queue + scheduler pool.
- Meridian gossip/relay/rendezvous/ring subsystems each become one or more actors.

**Phase 5 — Transport reshape (vendor row 8)**
- Reshape the existing six transport families to the liboffs contract: transport owns I/O, handler owns dispatch, results flow back as messages.
- Each transport's existing lock-and-callback shape is replaced with `actor_send`.

**Phase 6 — Old ABI removal**
- Per § 0 row 4c: every old `poseidon_*` function that did I/O on the caller's thread is **deleted in the same commit** as the new actor version lands. No two-symbol shim. No deprecation `#warning`. No compat header.

**Phase 7 — Lock audit**
- Final pass: `grep -r "PLATFORMLOCKTYPE\|PLATFORMCONDITIONTYPE\|PLATFORMBARRIERTYPE" poseidon/src/` should return only matches inside `Platform/` and inside actor internals (`Actor/message_queue.c`, `Actor/pool.c`, `Scheduler/scheduler.c`). Any match in business code is a bug.
- `refcounter_init` should be the **last** call in every `_create` function (per poseidon's styleguide). Verify with a static check.
- `get_clear_memory` followed by `refcounter_init` is the standard pattern. Verify.

### 7.5 Build Wiring — Poseidon's Native CMake Convention

Per the user's correction: the transplanted code is **compiled as if it were native to poseidon** using **the cmake conventions already present**. Poseidon's existing convention is documented by reading `poseidon/CMakeLists.txt` directly:

- One `*_SRC_DIR` variable per source directory, set near the top of the root `CMakeLists.txt` (lines 49-60 show this pattern for the existing subsystems).
- One `POSEIDON_SOURCES` list that grows by source file as the project grows (lines 63-100+).
- The `add_library(poseidon ...)` call later in the file pulls from `POSEIDON_SOURCES`.
- The `add_meridian_test()` helper is used for tests, defined once near the test setup.
- `add_subdirectory(deps/...)` is used **only for third-party deps**, not for poseidon's own code.

**The transplanted code follows the same pattern.** No new `add_subdirectory` for `Actor/`, `Scheduler/`, `Streams/`, `OFFStreams/`, `ClientAPI/`. No per-subdirectory `CMakeLists.txt`. The liboffs per-directory `CMakeLists.txt` files are **not vendored**; they are not part of the source, they are part of the build glue, and poseidon has its own build glue.

**Concrete shape after Phase 1** (in the root `poseidon/CMakeLists.txt`):

```cmake
# Source directories — added to the existing block near line 49
set(ACTOR_SRC_DIR       ${CMAKE_CURRENT_SOURCE_DIR}/src/Actor)
set(SCHEDULER_SRC_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/src/Scheduler)
set(STREAMS_SRC_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/src/Streams)
set(OFFSTREAMS_SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/src/OFFStreams)
set(CLIENTAPI_SRC_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/src/ClientAPI)

# POSEIDON_SOURCES — added to the existing list near line 63
set(POSEIDON_SOURCES
    # ... existing entries ...
    ${REFCOUNTER_SRC_DIR}/refcounter.c
    ${ACTOR_SRC_DIR}/actor.c
    ${ACTOR_SRC_DIR}/message.c
    ${ACTOR_SRC_DIR}/message_queue.c
    ${ACTOR_SRC_DIR}/pool.c
    ${SCHEDULER_SRC_DIR}/deque.c
    ${SCHEDULER_SRC_DIR}/scheduler.c
    # ... continues as more sources are vendored in later phases
)
```

**The pattern is repeated for each new vendored directory in the corresponding phase.** Phase 2 adds `STREAMS_SRC_DIR` + `OFFSTREAMS_SRC_DIR` and the `stream.c` + `readable_off_stream.c` + `writeable_off_stream.c` + `representation_actor.c` sources. Phase 3 adds `CLIENTAPI_SRC_DIR` and the HTTP transport sources. Etc.

**Why this is the right shape:**

- It honors the user's stated rule: native to poseidon, poseidon's conventions.
- The existing `add_meridian_test()` helper, the existing test layout in `tests/`, the existing include path conventions in `include/`, and the existing `poseidon_*` symbol prefix are all unchanged.
- New unit tests for the vendored code drop into `tests/` alongside the existing ones. The `add_meridian_test()` helper is reused.
- A future refactor that wants per-directory `CMakeLists.txt` files can do that as a separate change. It is not part of the migration.

**What is explicitly not done:**

- No `add_subdirectory(src/Actor)`.
- No `src/Actor/CMakeLists.txt`, `src/Scheduler/CMakeLists.txt`, `src/Streams/CMakeLists.txt`, `src/OFFStreams/CMakeLists.txt`, `src/ClientAPI/CMakeLists.txt`.
- No `target_sources(poseidon PRIVATE ${ACTOR_SRC_DIR}/actor.c ...)` style. Poseidon does not use `target_sources`; it uses the `POSEIDON_SOURCES` list. Follow the existing pattern.
- No copying the liboffs `CMakeLists.txt` structure. Liboffs is the shape source for *code*; poseidon is the authority on *build*.

---

## 8. Styleguide Touchpoints

Poseidon's `STYLEGUIDE.md` is at the repo root, 1196 lines. The rules that the migration must preserve:

- **Refcounter is the first member of every long-lived struct.** After Phase 1 this rule is satisfied by a single, liboffs-derived `refcounter_t`.
- **`refcounter_init` is the last call in `_create` / `get_clear_memory` is the first.** All actor `_create` functions follow this.
- **`_t` suffix for typedefs.** Honored across the vendored code.
- **`type_action()` naming for functions.** Honored.
- **No TODO / FIXME / HACK / XXX in shipped code.** Honored.
- **No `Co-Authored-By:` trailer in commits.** Honored.

A new **"Actor Concurrency"** section is added to `poseidon/STYLEGUIDE.md` after Phase 1 lands. The section is a pointer to the `good-actors` skill (path `~/.claude/skills/good-actors/SKILL.md`) plus poseidon-specific context:

> "Concurrency in poseidon is expressed as actor message passing. The rules of actor construction, dispatch, destruction, and backpressure are defined in the `good-actors` skill. The skill is the source of truth. This section is a pointer, not a copy — the skill is updated more often than this guide, and copying it here will produce stale rules."

The section also records:

- The actor + scheduler + refcounter types live in `poseidon/src/Actor/`, `poseidon/src/Scheduler/`, `poseidon/src/RefCounter/`.
- The flow-control actor (`poseidon_stream_t`) lives in `poseidon/src/Streams/stream.h`.
- The HTTP transport lives in `poseidon/src/ClientAPI/HTTP/`.
- All handlers used by transports conform to `poseidon_block_handler_ctx_t` in `poseidon/src/ClientAPI/block_handlers.h`.

---

## 9. The Rule of Surgical Transplant (Restated)

To make the user's constraint unmissable:

> "This needs to be surgical we transplant what we need from liboffs with explicit attention to detail and context not using a script."

For every file moved from liboffs to poseidon:

1. **Read the liboffs file in full first.** Understand the dependencies (what headers it includes, what symbols it references, what its dispatch function assumes about the scheduler pool).
2. **Read the closest poseidon analog.** If poseidon has an equivalent, understand the difference. Do not assume the liboffs version is always right — sometimes poseidon's existing code has a constraint liboffs doesn't (e.g. poseidon's block storage has poseidon-specific metadata).
3. **Write the vendored file from scratch, copying structure line by line.** Do not run `cp` and then a rename script. The act of rewriting is the act of understanding.
4. **Apply the prefix and naming changes inline as you write.** `actor_t` → `poseidon_actor_t`. `actor_init` → `poseidon_actor_init`. `OFFS_ATOMIC` → poseidon's equivalent macro. Update the include paths. Update the doc comments to mention poseidon.
5. **Build.** The vendored file must compile in isolation first (its own translation unit). Then build poseidon. Fix all warnings as errors.
6. **Test.** Either port the liboffs unit test for the same component, or write a new one that proves the same invariant. The migration's safety net is "the existing tests still pass + the new tests pass".
7. **Commit.** One concern per commit. Conventional Commits format. No `Co-Authored-By:`. The commit message records: (a) which liboffs file was the source, (b) what was renamed, (c) what poseidon-specific changes were made, (d) which tests cover the change.

If at any step you find yourself wanting to script the rename — stop. The whole point of the surgical rule is that the renamer must understand each line. A script bypasses the understanding.

---

## 10. Risks and How They Are Caught

| Risk                                                                                          | Catch |
|-----------------------------------------------------------------------------------------------|-------|
| RefCounter transplant breaks UAF in existing code                                              | Phase 1 runs poseidon's full test suite after the swap. The existing tests were the canary. |
| Actor dispatch allows re-entrancy that a lock-based code path forbade                          | New actor unit tests exercise re-entrant send. STYLEGUIDE rule: actors never call into themselves from inside dispatch. |
| `stream_t` handler list mutation races with dispatch                                          | liboffs's `stream.h` comment lines 152-156 already document the discipline. Poseidon's vendored copy repeats the discipline. New unit test subscribes from a non-actor thread and dispatches a notify from another non-actor thread, asserts both ran. |
| HTTP transport leaks connection state into handlers                                           | `typedef void poseidon_block_connection_t;` (incomplete typedef, same as liboffs). The handler context holds a `void*` cast. Handlers cannot dereference. Compile-time check. |
| Old ABI deletion breaks callers                                                               | There are no production callers (per user, § 0 row 4c). Internal callers are updated in the same commit. |
| Vendored code drifts from liboffs and loses hardening                                          | Surgical-transplant rule + each commit's justification comment records the liboffs source SHA. Future hardening backports are explicit commits, not silent syncs. |
| A poseidon subsystem's existing code has a constraint liboffs doesn't                          | Phase 4 reads each subsystem before actorizing. The vendored liboffs types are extended in place, not replaced wholesale, when poseidon's constraint is real. |

---

## 11. What This Plan Is *Not*

- Not a script.
- Not a bulk import.
- Not a "deprecate then remove" two-phase plan.
- Not a parallel-symbols plan.
- Not a "make poseidon call into liboffs" plan — liboffs is the shape source, not a build dependency.
- Not a "free up old code paths" plan. The old code is removed in the same commit as the new code.

---

## 12. Items Still Needing User Input (After Red-Lines)

**Locked by user in this revision:**

- **A. Branch name** → `actor-migration`. (§ 0 row 0)
- **B. Build wiring** → "Compile the code as if it was native to poseidon use the cmake conventions already present." Concretely: poseidon's flat-list pattern in the root `CMakeLists.txt` — `*_SRC_DIR` per directory, sources listed in `POSEIDON_SOURCES`, no `add_subdirectory` for own code, no per-subdirectory `CMakeLists.txt`. Spelled out in § 7.5.
- **C. Question A: how to source `message.h`** → Option 3 (vendor whole liboffs `message.h`, 448 lines, mutate surgically).
- **D. Question A.1: does libons apply as a simpler source** → No. libons is a consumer project that vendored liboffs + ONS extensions. Not a simpler source. (Resolved by investigation, not by user red-line.)
- **E. Question B: poseidon's own `Platform/` directory** → **B4** — wholesale `Platform/` vendoring, supersedes the v2 B2 ("keep poseidon's own") framing. Spelled out in § 6.1.
- **F. log.h** → No action. Poseidon's existing 0.1.0 `log.h` provides every standard macro the vendored liboffs code calls. (Resolved by investigation, not by user red-line.)

**Still requires the user's final red-line before Phase 1 begins:**

- § 7.5 itself, as the operationalized form of the build-wiring rule (the textual restatement of lock B).
- § 6.1 itself, as the operationalized form of the B4 platform-vendoring rule (the textual restatement of lock E).

That's it. Every other open question from v1 was closed by the user's corrections in v2.

---

*End of plan v2.1. Awaiting final red-line on § 6.1 and § 7.5 before creating the `actor-migration` branch and starting Phase 1.*
