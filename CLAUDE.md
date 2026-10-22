# Poseidon Project

Always read and follow the coding conventions in [STYLEGUIDE.md](./STYLEGUIDE.md) when writing or modifying C code in this project.

## Git Commit Conventions

- **Do NOT add "Co-Authored-By" lines to commit messages.** All commits should have only the author's information.
- Use clear, descriptive commit messages following conventional commit format (e.g., "feat:", "fix:", "docs:", "test:", "refactor:", "chore:")
- Keep commits focused and atomic - one logical change per commit

## Key Patterns
- Reference-counted structs have `refcounter_t refcounter` as the first member
- Types use `_t` suffix, functions follow `type_action()` naming
- Create functions use `get_clear_memory()` and call `refcounter_init()` last
- Destroy functions check count==0 before freeing
- Directories in `src/` are organized by semantic purpose (Buffer/, Workers/, Time/, etc.)

## Actor Migration State
- **2.10 (committed)**: `poseidon_channel_manager_t` is actor-model. `_create` leaves the dial channel passive; production callers (currently `src/poseidond.c`) MUST invoke `poseidon_channel_manager_start(mgr)` after construction. Synchronous public wrappers (`handle_bootstrap_reply`, etc.) follow the pattern: build payload → `actor_send` + `actor_run(32)` → read out-field from `mgr` → perform I/O seam → return out-field. Each `_impl` return value is captured into a dedicated out-field (e.g. `hbr_return_value`).
- **Dial channel is passive**: explicit `poseidon_channel_subscribe(dial_channel, dial_topic, ...)` is REDUNDANT. Intercept is wired via direct field assignment `mgr->dial_channel->intercept_cb = ...`.
- **Build verification gate**: `ctest` MUST be run from `build/` (project root has no `CTestTestfile.cmake`). Test binary path is `build/<name>`, NOT `build/tests/<name>`. Floor: 25/25 ctest + 9/9 conn suite.
- **CRITICAL allocator rule**: `poseidon_alloc` / `poseidon_free` are liboffs-isms. NEVER USE — use `calloc` / `malloc` / `free`. `get_clear_memory` IS a poseidon API and is still in use.
- **`actor_send` contract**: wraps caller-provided `message_t*` in heap `message_node_t`, returns `false` on DESTROY or queue rejection. Caller MUST NOT wrap, MUST NOT free.
- **Wrapper→actor return-value rule**: every public wrapper that has an `_impl` variant MUST capture `_impl`'s return value into a manager out-field, then return that field. Audit ALL wrappers when actorizing.

## Phase 2 / 3 — `client_session` actorization (committed `f88699d`)

- **Per-FD single-actor shape**: `client_session_t` embeds `scheduler_pool_t* pool;` + `actor_t actor;` by value. NO `lock` field. All state mutations route through a dispatched message; the actor's own thread serialises them.
- **Wrapper canonical pattern** (`src/ClientAPIs/client_session.c:525-540` for `client_session_subscribe`): pre-init out-field → build stack payload → `message_t msg = {.type, .payload, .payload_destroy=NULL};` → `actor_send` → `actor_run(&s->actor, 32)` → `return s-><out_field>;`. Mirrors `channel_manager.c:1198-1219` from 2.11.
- **Pool derivation**: `s->pool = (manager != NULL) ? manager->actor.pool : NULL;` inside `client_session_create`. `actor_init` with that pool; `pool=NULL` = caller-thread dispatch (works for test, which passes `manager=nullptr`).
- **`mgr->pool` is the field name** on `poseidon_channel_manager_t`, NOT `mgr->work_pool` (`channel_manager.h:68`).
- **Manager external lock**: field is `external_lock` (`platform_mutex_t*`), NOT `lock`. API: `platform_mutex_lock(mgr->external_lock)` / `platform_mutex_unlock(mgr->external_lock)`. **There is no `platform_lock` / `platform_unlock` in poseidon** — only `platform_mutex_lock` / `platform_mutex_unlock` taking a `platform_mutex_t*`.
- **`vec_free` does not exist.** Use `vec_deinit(&v)` (the macro that frees `v->data`).
- **Session-side subscription table is canonical**: `handle_subscribe` records `subscriptions[].active`, `.loopback`, `.topic_path`, and bumps `num_subscriptions` BEFORE any channel-side work. Channel attach via `find_channel_by_topic` → `poseidon_channel_create` (if NULL) → `poseidon_channel_set_message_callback` + `poseidon_channel_subscribe` is opportunistic and only runs when `s->manager != NULL`. Channel-attach failure is non-fatal — session-side record stands. Tests assert on session-side table directly.
- **`poseidon_channel_create` real signature** (`src/Channel/channel.h:91-96`): `(poseidon_key_pair_t* key_pair, const char* name, uint16_t listen_port, const poseidon_channel_config_t* config, work_pool_t* pool, hierarchical_timing_wheel_t* wheel)`. Requires `config != NULL && pool != NULL && wheel != NULL` (returns NULL otherwise). `key_pair==NULL` → auto-creates `ED25519` keypair. `listen_port=0` accepted (no static `allocate_port` available in `client_session.c`).
- **`poseidon_channel_config_defaults()`** declared `src/Channel/channel_config.h:28`; defined `src/Channel/channel_config_defaults.c:8` and `src/Channel/channel.c:22`.
- **Deliver-payload contract**: `handle_deliver` rejects `topic_path == NULL || p->buf == NULL`. Fan-out MUST pass non-NULL `topic_path`. `handle_deliver` does NOT free `p->buf` (caller owns).
- **Buffer ownership for `client_session_msg_deliver_t`**: wrapper `client_session_dispatch_event` stack-payloads it, `actor_send`s, `actor_run`s, then frees `topic_copy` and `buf` after `actor_run` returns. The fan-out `session_dispatch_message` callback (in `client_session.c`) follows the same per-recipient pattern: each session gets its own `malloc`'d `buf_copy` + `strdup`'d `topic_copy` (NON-NULL, required by `handle_deliver`); callback frees both after `actor_run`.
- **cbor placement policy**: ALL cbor encode/decode must run INSIDE the actor (on its thread). `session_dispatch_message` (channel-side fan-out callback) currently encodes cbor in the callback — **deferred carve-out**, will move into a `_impl` for Phase 4. Session wrappers already comply.
- **Test contract**: `tests/client_session_test.cpp` reads `session->num_subscriptions` and `session->subscriptions[0].topic_path` directly on the struct post-call. Test ALSO exercises manager-less session (passes `nullptr` to `client_session_create`) — the impl must record subscriptions in memory even without a channel manager. Floor: 2/2 on this test (CreateDestroy + SubscribeUnsubscribe).

## Vendoring hygiene (per boss directive)

- **No "liboffs" in runtime strings, env-vars, comments, commit messages.** Carveouts: L2 `// Vendored from liboffs/src/X.h` stays; `/etc/offs`/`offs.json` stays; `platform_dirs.h` "Local modifications" audit stays. **Commit subject/body must NOT contain "liboffs"**; L2 comments inside vendored files are the only allowed appearance and may stay. NEW files need no L2 provenance comment.
- **No deprecation, no shim**: old `poseidon_*` symbols deleted outright. Rename goes straight.
- **Standing instruction**: `do both` — Phase 2 prep (L2 comments, audit) + this NOTES refresh each commit.
- **`.h` files in this project do NOT carry a "Local modifications" EOF block** — that carveout lives on the `.c` only.

## Pending: Phase 4 (next)

- `client_session_dispatch_event` wrapper cbor decode currently runs out-of-actor (decode payload on caller's thread, build stack struct, actor_send) — partial compliance. Revisit when Phase 4 widens: probably need a `_impl` for it too.
- Audit `handle_cleanup_subs` for `external_lock` migration (build is green; no `platform_lock` references; likely already clean).
- Convert remaining public API in `client_session.h` to actor-model wrappers + `_impl` (only `_create` / `_destroy` / `_subscribe` / `_unsubscribe` done so far; `handle_request`, `dispatch_event`, `cleanup_subs` and others still to be converted/audited).
- Next file to migrate: scan `src/ClientAPIs/` and `src/ClientLibs/c/` for the next vendored upstream file that hasn't been actorized yet.

## Phase 4 — in-actor cbor for client_session (committed `4dc6cb5`)

- **`client_session_dispatch_event` cbor now runs in-actor**. New `POSEIDON_CLIENT_SESSION_MSG_DISPATCH_EVENT (0x08)` message type and `client_session_msg_dispatch_event_t` payload struct. Wrapper heap-copies `topic_path`, `subtopic`, and `payload` (`strdup` / `malloc`+`memcpy`), builds the payload, `actor_send`+`actor_run`, and returns. The in-actor `handle_dispatch_event` runs `client_protocol_encode_event` + `client_protocol_serialize` + `transport->send` + `free(buf)`, then frees all three input copies. No out-field (wrapper returns void).
- **Ownership rule for the new payload**: the actor owns the three heap-copied buffers; the wrapper does not free them after `actor_run` (it freed nothing in the success path). `handle_dispatch_event` runs the frees unconditionally after the send step, even if the wire-send path was skipped (transport or client_fd missing), so no buffer leaks on the closed-session path.
- **`handle_cleanup_subs` audit clean** (Phase 4 step 1, no separate commit — folded into the same audit pass). No `mgr->lock` / `platform_lock` / `platform_unlock` references; uses `external_lock` via `manager_has_other_subscriber`; `vec_deinit` (not `vec_free`); the actor serialises the iteration.
- **Remaining deferred carve-out**: `session_dispatch_message` (`src/ClientAPIs/client_session.c`, channel fan-out callback invoked by the channel layer) still encodes the wire-event cbor on the calling thread before fanning out `MSG_DELIVER` to subscribers. Documented as a known carve-out; out of scope for this phase because it is called by the channel layer's dispatch path, not by a session wrapper, and reworking it requires an actor on the publisher side, not just the per-FD session actor.
- **Remaining public API**: 9 functions in `client_session.h`, all wrapper-shaped after this phase. `dispatch_event` was the last wrapper that did cbor work on the caller's thread; it now defers entirely to the actor. `cleanup_subscriptions` was already in-actor (no out-field, single `MSG_CLEANUP_SUBS` message).
- **Local modifications EOF block** in `client_session.c` updated to note `dispatch_event`'s cbor work is in-actor alongside `handle_request`'s `CHANNEL_DESTROY` / `CHANNEL_MODIFY` cases.

## Phase 4 step 3 — remaining public API verified

- All 9 public functions in `client_session.h` are wrapper-shaped. After Phase 4 step 2, `dispatch_event` no longer does cbor work on the caller's thread; `cleanup_subscriptions` was already in-actor; `is_subscribed` / `has_loopback` were already in-actor with `is_subscribed_out` / `has_loopback_out` out-fields populated by `handle_is_subscribed` / `handle_has_loopback` and read by the wrappers.
- No `_impl` function in the file lacks its wrapper, and every wrapper with a corresponding `_impl` propagates the `_impl` return through the out-field pattern. Step 3 was a verification pass only — no code change.
- **Phase 4 closed**. The only deferred carve-out is `session_dispatch_message` (channel-side fan-out, called by the channel layer, not a session wrapper). It stays as a documented carve-out; addressing it requires an actor on the publisher side and is not part of the per-FD session-actor work.

## Standing rule: prefer the upstream canonical pattern (per boss directive)

- **"Always refer back to the upstream codebase if the problem you face is likely already solved there."** The first move on a design problem is to read the upstream reference (`liboffs/`) and mirror the existing solution. Do not invent new patterns when the upstream already encodes one.
- **In-tree equivalent when a design problem touches an already-existing subsystem**: look at the closest in-tree precedent before writing anything new. For actor+pd_loop+thread problems in poseidon, the canonical in-tree reference is `src/Timer/timer_actor.c` (its own `actor_t`, its own `pd_loop_t* loop`, its own `platform_thread_t* thread`, `atomic` running flag, `_dispatch` registered at create time, the same `pd_loop_async_send` wakeup + `platform_thread_join` shutdown used in upstream).

## Phase 5 (next) — transport migration study (in progress, no code yet)

- **Target pick under consideration**: `src/ClientAPIs/transport.h` + 7 `transport_*.c` backends (`unix`, `tcp`, `ws`, `quic`, `wt`, `binder`, `xpc`). `client_protocol.{c,h}` is a stateless cbor codec — no actor needed.
- **Current `transport.h` shape (pre-Phase 5)**:
  - L60 opaque `poseidon_transport_t` typedef.
  - L74 `platform_mutex_t* lock` — to be removed (liboffs has no general lock on the transport).
  - L77-81 function-pointer vtable (`start`, `stop`, `send`, `destroy`) — to be removed. Liboffs has no vtable; per-backend struct is exposed by header.
  - L95-178 seven `poseidon_transport_<backend>_create` factories all taking `(path_or_port, cert, key, manager)` and returning `poseidon_transport_t*`.
  - L72-73 already has `void* loop` and `platform_thread_t* thread` — the I/O pump skeleton is in place, currently under-used.
- **Canonical upstream transport shape (from `liboffs/src/ClientAPI/Unix/unix_transport.h`)**:
  - `actor_t actor;` + `scheduler_pool_t* pool;` embedded by value in the struct.
  - `pd_loop_t* loop;` + `platform_thread_t* thread;` for the I/O thread.
  - ATOMIC flags (`running`, `loop_running`).
  - `vec_t(connection_t*) connections;` — mutated from the loop thread, no separate lock.
  - One specialized `platform_mutex_t* destroy_lock;` (or `loop_lock` in poseidon's `Timer/timer_actor.c` — same role, different name) protecting the deferred-destroy stack only.
  - `actor_init(&transport->actor, transport, _<backend>_<role>_dispatch, transport->pool);` at create time.
- **Canonical upstream lifecycle (`unix_transport.c` L456-478)**:
  - `start` = `atomic_store(&running, 1); platform_thread_create(_server_thread, transport);` (no `actor_send` — I/O thread just starts).
  - `stop` = `atomic_store(&running, 0); pd_loop_async_send(loop, transport); platform_thread_join(thread);` — async-send wakes the loop, join blocks until the loop thread exits.
  - Per-iteration pump (POSIX AF_UNIX): `while (atomic_load(&running)) { _destroy_stack_drain(transport); pd_loop_run_once(loop, 100); }` (100ms tick).
  - Per-iteration pump (Windows IOCP): `while (atomic_load(&running)) { platform_local_accept; pd_loop_run_once(loop, 10); }` (10ms tick, no destroy-stack drain — UAF risk on in-flight overlapped).
- **Canonical upstream write path (`unix_connection.c:1558-1568`)**:
  - External callers (e.g. a session wanting to write to a connection) call `unix_connection_write(conn, data, len)`, which heap-copies the data into `buffer_t*`, builds a `message_t` with `payload_destroy = (void (*)(void*))buffer_destroy`, and `actor_send(&conn->actor, &msg)`. The I/O thread is the actor's runner, so the actual socket write happens on the I/O thread.
  - Same shape for `close`: `actor_send` with `type=UNIX_CONNECTION_CLOSE`, `payload=NULL`, `payload_destroy=NULL`.
- **In-tree equivalent**: `src/Timer/timer_actor.c` — same pattern, `actor_t` + `pd_loop_t*` + `platform_thread_t*` + `atomic running` + `actor_init` at create time + `pd_loop_async_send` to wake + `platform_thread_join` to stop. Poseidon's timer-actor uses `loop_lock` instead of `destroy_lock` (same role).
- **Poseidon-only backends (no upstream)**: `quic`, `binder`, `xpc` — must be designed from first principles using the same shape (`actor_t` + `pool` + `loop` + `thread` + atomic + actor-init dispatch). Poseidon already uses `pd_loop_t` for all 6 backends (TCP/Unix/WS/QUIC/WT/Binder/XPC, per `transport.h`), so the I/O scaffolding is already in place.
- **Pending "do 1" decision**: the `transport.h` + 7 `transport_*.c` migration is the natural next step, but the exact delta (single transport-level `actor_t` vs. liboffs's per-connection `actor_t`; per-backend struct exposure vs. opaque `poseidon_transport_t`; how to keep `client_session.c:407`'s `s->transport->send` call working) is not yet finalised. Plan presentation pending.

## Phase 6 — MSQUIC init in Platform with keyed registration refcount (committed `2e9ca50`)

- **Module location**: `src/Platform/poseidon_msquic.{c,h}`. Naming is `poseidon_msquic_*` (a higher-level Platform module), NOT `platform_msquic_*` (would collide with `platform_*` namespace).
- **API table is process-wide**: one `MsQuicOpen2` per process, single `g_api_table` + `g_api_table_refcount` + `g_api_table_lock` (init-once via `g_api_table_lock_once`).
- **Registration table is keyed by app identity**: each entry is `{char* app_name; HQUIC reg; uint32_t refcount;}`. `app_name` is BOTH the slot key AND the `RegConfig.AppName` string passed to MSQUIC. `_open_registration(name, &reg)` is find-or-insert; first insert calls `RegistrationOpen` and stores; subsequent calls return the same HQUIC and increment refcount. `_close_registration(name)` decrements; at 0 it `RegistrationClose`s, removes the slot, frees the name, and drops the API refcount. Helper is the SOLE path to `RegistrationOpen` / `RegistrationClose`.
- **Helper hardcodes `QUIC_EXECUTION_PROFILE_LOW_LATENCY`** — all 3 callers (meridian, meridian_relay, poseidon_client) use it. No knob exposed.
- **App identities in process (3 total)**: `"meridian"`, `"meridian_relay"`, `"poseidon_client"`. Each maps to exactly one registration, shared across all `ConfigurationOpen` calls for that identity's ALPNs.
- **Refcount flow invariant**: `_open_registration` internally calls `_open()` (API ref++); `_close_registration` internally calls `_close()` on the last ref (API ref--). `_create` failure paths must call BOTH `_close_registration(name)` AND `_close()` to keep the API refcount balanced.
- **`_open_registration` failure-mode contract**: returns `false` on any internal failure (NULL args, `_open` failed, `RegistrationOpen` failed, `strdup` failed, `vec_push` failed). All failure cases internally balance the API ref via `_close()`. **The caller does NOT need to call `_close()` on failure** — only `_close_registration(name)` to release the slot, plus the standard alloc cleanup.
- **Registration lifetime is owned by the transport**, not by `_start`/`_stop`: open in `_create`, close in `_destroy`. `_start` does not touch the registration at all (no `RegistrationOpen`/`RegistrationClose` rollback paths); `_stop` does not close it. Pattern shared by `meridian_relay_server.c` and `transport_quic.c`.
- **`poseidon_msquic_api()` is the single accessor** for the API table: 1-line `return g_api_table;` at end of `poseidon_msquic.c`. Callers never reach into the singleton.
- **Singletons removed**: `src/Network/Meridian/msquic_singleton.{c,h}` deleted. All three `CMakeLists.txt` (root, `relay/`, `ClientLibs/c/`) now list `${PLATFORM_SRC_DIR}/poseidon_msquic.c`. All five include sites (`transport_quic.c`, `poseidon_client.c`, `meridian_protocol.c`, `poseidond.c`, `relay_main.c`) use `"Platform/poseidon_msquic.h"`.
- **meridian_relay_server API quirk**: `meridian_relay_server_create(..., const QUIC_API_TABLE* msquic, ...)` keeps the `msquic` parameter for binary compatibility but now IGNORES it — the API table is sourced from `poseidon_msquic_api()`.

## Phase 7 — transport actorization begins (committed `afb9f4e`)

- **New internal header `src/ClientAPIs/transport_internal.h`**: cross-backend types every per-listener actor needs. Public `poseidon_transport_t` struct body in `transport.h` stays untouched through commit 8; this new header adds a sibling type, `poseidon_transport_internal_t`, that the per-backend `_create` factories will populate and `actor_init()` will bind.
- **Path-(a) design adopted**: new type is named `poseidon_transport_internal_t` (NOT `poseidon_transport_t`) so it can co-exist with the vtable struct in `transport.h`. **Commit 9** (the `transport.h` rewrite) is the one that drops the vtable, renames the internal type to `poseidon_transport_t`, and moves its body into the public header.
- **Header body fields**: `actor_t actor;` + `pd_loop_t* loop;` + `platform_thread_t* thread;` + `ATOMIC(uint8_t) running;` + `ATOMIC(size_t) active_connections;` + `struct poseidon_channel_manager_t* manager;` + `poseidon_transport_on_message_cb on_message;` + `poseidon_transport_type_t type;` + `const char* name;` + `vec_void_t connections;` + `scheduler_pool_t* pool;` + `destroy_stack_t destroy_stack;`. No struct-level lock; per-actor thread + per-actor FD is the model.
- **Destroy-stack types**: `destroy_node_t` = `{ pd_watcher_t* watcher; struct destroy_node_t* next; }`; `destroy_stack_t` = `{ platform_mutex_t* lock; destroy_node_t* head; }`. Single source of truth for the destroy mutex is `destroy_stack_t.lock` — there is no outer `poseidon_transport_internal_t.destroy_lock;` field.
- **Destroy-stack helpers are declarations only in the header**: 4 lines — `destroy_stack_init`, `destroy_stack_destroy`, `destroy_stack_push(stack, pd_watcher_t* watcher)`, `destroy_stack_drain`. Their impls land in `src/ClientAPIs/transport_destroy_stack.c` during **commit 2** (the Unix split). Keeping them declarations-only avoids recompiling every backend TU for changes to the helpers.
- **Header reaches `pd_watcher_t` via the include chain**: `transport_internal.h` → `../Platform/platform_thread.h` → `../Platform/platform.h` → `../Platform/platform_socket.h` → `<poll-dancer/poll-dancer.h>`. **No forward decl of `pd_watcher_t` in this header** — the real typedef is `typedef struct pd_watcher pd_watcher_t;` (struct tag is `pd_watcher`, NOT `pd_watcher_t`); forward-declaring `struct pd_watcher_t;` would create a different incomplete type and break later cast sites. Note: `scheduler.h` is also pulled in via `platform.h`, but the explicit `#include "../Scheduler/scheduler.h"` is kept for clarity.
- **Forward decls in the header**: `struct poseidon_channel_manager_t;` + per-backend `struct <x>_connection_t;` × 7 (Unix, TCP, WS, QUIC, WT, BINDER, XPC). The `connections` vec stores them as `void*`; backends cast at use. Lets the header stay self-contained without dragging all seven `<backend>_connection.h` headers into every TU.
- **Resolution of L102 ("Pending 'do 1' decision")**: chosen single transport-level `actor_t` (matches poseidon's per-listener model); per-backend struct exposure via `poseidon_transport_internal_t`; `s->transport->send` (now `s->transport->send` in commit 10/11) will route through a small adapter that uses the actor's `state` pointer.
- **12-commit plan approved by boss** with inline-helpers correction applied: commit 1 = this header; commit 2 = Unix split + `transport_destroy_stack.c`; commits 3-8 = TCP/WS/WT/QUIC/BINDER/XPC splits; commit 9 = `transport.h` rewrite + rename; commits 10-11 = call-site updates; commit 12 = `CLAUDE.md` memory refresh.
- **No L2 provenance comment on this new file**: standing rule — `NEW files need no L2 provenance comment`, that carveout is for vendored files only.
- **Build verification**: `cmake --build build/` green, 25/25 ctest, 9/9 conn suite pass, hygiene check on the new header returns 0.


## Phase 7 — Unix backend split landed (commit `97ea4a8`)

The Unix domain socket transport is the first backend moved to the actor
model. Everything below is now established, not planned.

### New file layout (src/ClientAPIs/)

| File | Role |
|---|---|
| `transport_unix.h` / `transport_unix.c` | The listener: listening socket, accept watcher, I/O thread, connection table. |
| `unix_connection.h` / `unix_connection.c` | The per-connection actor: frame decoding, write path, watcher ownership. |
| `unix_connection_messages.h` | Payload structs for the `POSEIDON_UNIX_*` message family. |
| `transport_destroy_stack.c` | Shared `destroy_stack_t` helpers (deferred `pd_watcher_destroy` on the loop thread). |
| `transport.c` | `poseidon_transport_config_defaults` + the generic `poseidon_transport_destroy`, relocated out of the old `transport_unix.c` so the un-migrated backends still link. |

### The two-view transport struct (important — gotcha)

`poseidon_transport_t` (the vtable-shaped public view, in `transport.h`) and
`poseidon_transport_internal_t` (the actor-shaped body, in
`transport_internal.h`) have **different layouts**. They cannot be
overlaid on one another. The backend struct therefore holds **both**:

```c
typedef struct poseidon_unix_transport_t {
    poseidon_transport_internal_t base;   /* FIRST: a pointer to the
                                              backend IS a pointer to the
                                              shared body */
    poseidon_transport_t public;          /* the vtable view */
    platform_socket_t* listen_sock;
    pd_watcher_t* listen_watcher;
    char* socket_path;
    void* ctx;
    platform_mutex_t* conn_lock;
    scheduler_pool_t* own_pool;
} poseidon_unix_transport_t;
```

Conversions: `_unix_from_public()` uses `offsetof(..., public)`;
`_unix_from_base()` is a plain upcast (base is first). Commit 9 removes
the vtable and collapses this to one view.

### Threading model

- I/O thread: the poll-dancer loop, the accept watcher, the destroy-stack
  drain. Nothing else.
- Scheduler pool workers: the per-connection actors. They own frame
  decoding and every write to the socket.
- Caller thread: create / start / stop / destroy.

A watcher may only be unregistered on the loop thread, so a connection
that must tear its watcher down pushes it onto the transport's
`destroy_stack_t`; the I/O thread drains the stack at the bottom of each
`pd_loop_run_once` iteration. The socket is closed **after** the
unregister, so the descriptor number cannot be recycled under a live
registration.

### Teardown order (this order is load-bearing)

1. `poseidon_transport_unix_stop` — join the I/O thread.
2. `destroy_stack_drain` — free the watchers the connections asked for.
   **Before** touching the connection watchers, or they are freed twice.
3. Destroy the listen socket.
4. `actor_destroy` on **every** connection actor. This sets DESTROY,
   waits for a running dispatch to finish, and drains the mailbox. The
   connection's fields (`sock`, `watcher`, `write_buffer`, `transport`)
   may only be touched after this — touching them earlier is a data race
   (the thread sanitizer flagged exactly this).
5. `scheduler_pool_wait_for_idle` — let the rest of the pool go quiet.
6. Free the connections: socket, watcher, write buffer, session.
7. `actor_destroy` on the transport's own actor, `destroy_stack_destroy`,
   `pd_loop_destroy`, free.

`actor_destroy` already detaches the actor from the pool registry and
destroys the mailbox, so it must **not** be combined with a separate
`actor_detach_pool` + `message_queue_destroy` — that tears the same
queue down twice.

### Bugs fixed in this pass

- **Partial send stalled forever.** The write path had no retry when the
  kernel's send buffer filled up: the frame stayed in `write_buffer` and
  nothing re-armed the socket. Added `POSEIDON_UNIX_CONNECTION_WRITABLE`,
  delivered by the read callback on `PD_EVENT_WRITE`, so the actor
  retries the pending bytes. (Upstream has the same message; the
  in-between poseidon draft had dropped it and folded it away.)
- **Teardown data race.** The transport touched `conn->sock` and
  `conn->is_closing` while a connection actor's dispatch could still be
  running. Fixed by quiescing the actors first (see the teardown order
  above). Two sanitizers confirmed the fix.
- **No scheduler pool without a manager.** A transport created with a
  NULL channel manager got `pool = NULL`, so its connection actors were
  never scheduled and their mailboxes filled up undrained. The transport
  now creates and owns a pool (`own_pool`) in that case, and stops and
  destroys it in its own teardown.

### API surface added (backend-specific, in `transport_unix.h`)

- `poseidon_transport_unix_create(socket_path, manager, on_message, ctx)` — 4 args, returns `poseidon_transport_t*`, NULL on failure. Matches `transport.h` exactly.
- `poseidon_transport_unix_destroy` — the backend's own destroy. The generic `poseidon_transport_destroy` does not know about the connections, the loop or the transport's actor, so the Unix backend must not use it.
- `poseidon_transport_unix_start` / `_stop` / `_send`.
- `poseidon_transport_unix_send_to_client(internal, client_id, data, len)` — the form the `on_message` callback uses, since the callback receives the internal body.
- `poseidon_transport_unix_session_for_client_id(internal, client_id)` — the session bound to a client.
- `poseidon_transport_unix_context(internal)` — the opaque ctx.

`client_id` **is** the file descriptor of the accepted socket.

### The `on_message` callback

```c
typedef void (*poseidon_transport_on_message_cb)(
    struct poseidon_transport_internal_t* transport,
    int client_id, const uint8_t* data, size_t len);
```

No `void* ctx` parameter — the ctx is stored by the backend and read
back through the backend's context accessor. The callback receives one
**decoded** frame: the 4-byte big-endian length prefix is already
stripped. `poseidond.c` implements it in `unix_on_message`: decode the
cbor frame, hand the request to `client_session_handle_request`,
serialize the response, send it back with
`poseidon_transport_unix_send_to_client` (which re-adds the prefix).

### Wire protocol

4-byte big-endian length prefix + payload, both directions. Same as
`stream_frame_encode` upstream and as `poseidon_client.c` in-tree.

### Verification performed for this commit

- `cmake --build build/` green, **25/25 ctest**, **9/9 conn suite**.
- A request/response round trip over the socket, verified end to end
  (framed request → decoded → framed response → decoded).
- 20 concurrent clients, 400 requests, half the clients disconnecting
  abruptly: 410 frames, 0 send failures.
- valgrind: **0 errors, 0 leaks** (11,903 allocs / 11,903 frees).
- Thread sanitizer: **0 races** after the teardown fix. (TSAN needs
  `setarch -R` in this container; it aborts with "unexpected memory
  mapping" otherwise.)
- The manager path (transport created with a real `poseidon_channel_manager_t`)
  round-trips and tears down cleanly.

### Pre-existing issues found (NOT from this commit — do not attribute to it)

- `scheduler_pool_destroy` reads the actor registry of an already-freed
  manager (`poseidon_channel_manager_destroy` frees the manager, and the
  pool still walks its registry): 5 invalid reads under valgrind. The
  daemon's own teardown order triggers it.
- `poseidon_channel_manager_unregister_session` leaks the message node
  (32 bytes): the payload is stack-allocated but the node is heap.
- `poseidond` fails at "failed to start channel manager dial channel"
  before any transport is created. Reproduces identically on a clean
  checkout of the parent commit, so it is not from this work.


## Phase 7 — TCP backend split landed (commit `e2efe87`)

The TCP backend is the second backend moved to the actor model. The Unix
split is the template; every difference below is a consequence of TLS.

### New file layout (src/ClientAPIs/)

| File | Role |
|---|---|
| `transport_tcp.h` / `transport_tcp.c` | The listener: listening socket, accept watcher, TLS context, I/O thread, connection table. |
| `tcp_connection.h` / `tcp_connection.c` | The per-connection actor: TLS handshake, frame decoding, CBOR request handling, write path, watcher ownership. |
| `tcp_connection_messages.h` | Payload structs for the `POSEIDON_TCP_*` message family. |

### The message family

`POSEIDON_TCP_CONNECTION_READ / _HANGUP / _ERROR / _WRITABLE / _WRITE /
_CLOSE` — 6 entries, appended to the global `message_type_e` in
`src/Actor/message.h` (the type tags live there, the payload structs in
`tcp_connection_messages.h`).

`READ` takes the place of the Unix family's `DATA`: READ carries no
payload, because the connection actor pulls the bytes off the socket
itself instead of receiving them from the I/O thread.

### The TLS rule (the one real difference from Unix)

**An SSL object may only be used from one thread.** The Unix backend
reads on the I/O thread and hands the bytes to the actor; the TCP
backend cannot, because the connection's `SSL*` is used for the read,
the write and the handshake. So:

- The I/O thread's watcher callback only *signals* readiness. It never
  touches the socket, never touches the `SSL*`, and never parses a
  frame. It sends `READ` / `WRITABLE` / `HANGUP` / `ERROR` to the
  connection actor.
- The connection actor performs `SSL_accept` (retrying on
  `SSL_ERROR_WANT_READ` / `SSL_ERROR_WANT_WRITE`), `SSL_read`,
  `SSL_write`, the frame decoding and the CBOR handling.
- The `SSL*` is created on the actor thread, not in the accept path.
  The `SSL_CTX*` is created by the listener and borrowed.

The handshake used to be a **blocking** `SSL_accept` inside the accept
callback, which stalled the whole I/O thread for the duration of the
handshake. That is gone.

### The write path

The old `tcp_transport_send` issued two separate `send()` calls (header,
then body) on a raw descriptor. Two replies could interleave their bytes
on the same socket, and the call was not safe from the session actor's
thread.

Now the connection owns a **write queue**: `write_buffer` is the head
(the frame being written) and `write_queue` holds the frames behind it.
`poseidon_tcp_connection_write(conn, data, len)` frames the payload with
the 4-byte big-endian length prefix, copies it, and hands it to the
actor through `actor_send`. The actor serializes the writes, so the
vtable's `send` — which the client session calls from the session
actor's own thread — is now thread-safe and cannot interleave.

A TLS write that cannot be completed keeps the head buffer **exactly
where it is**: OpenSSL has to be retried with the very same buffer and
length, so the tail is never slid forward while a write is in flight.
The plain-socket path does slide the tail, as the Unix backend does.

### Connection identity

`client_id` is the file descriptor of the accepted socket, as in the Unix
backend. The connection keeps that number in an immutable `client_fd`
field, and a `ATOMIC(uint8_t) closed` flag that is raised **before** the
descriptor is closed. The listener skips closed connections when it
resolves a `client_id`, so a recycled descriptor number is never
mistaken for a live connection.

### Teardown order

1. `poseidon_transport_tcp_stop` — join the I/O thread. The connections
   are asked to close gracefully, so their pending frames are flushed
   while the scheduler pool is still alive.
2. `destroy_stack_drain` — free the watchers the connections asked for.
3. Destroy the listen socket.
4. `actor_destroy` on every connection actor, then
   `destroy_stack_drain` **again**: an actor that ran its close path
   while it was being quiesced pushed its watcher onto the stack.
5. `scheduler_pool_wait_for_idle`.
6. Free the connections under `conn_lock`: socket, watcher, SSL, write
   buffers, session. The lock is deliberately **not** held while the
   actors are quiesced in step 4 (a dispatch may reach back into the
   transport's send path, which takes the same lock) but **is** held
   while the connections are freed, because the send path resolves a
   connection under it.
7. `actor_destroy` on the transport's own actor, `destroy_stack_destroy`,
   `pd_loop_destroy`, `SSL_CTX_free`, free.

The SSL session is released **before** the pending write buffers: a
write that could not be completed leaves the frame inside the SSL
object, so the frame's memory has to outlive it.

### API surface added (backend-specific, in `transport_tcp.h`)

- `poseidon_transport_tcp_create(port, cert_path, key_path, manager)` —
  same signature as before, so `poseidond.c` is unchanged. The transport
  still requires a channel manager.
- `poseidon_transport_tcp_destroy` — the backend's own destroy. It is
  installed in the vtable's `destroy` slot, because `poseidond.c` tears
  the TCP transport down through the generic
  `poseidon_transport_destroy`.
- `poseidon_transport_tcp_start` / `_stop` / `_send`.

The listener binds the wildcard address in `create` (not in `start` as
before) through `platform_listen_socket_create`, so a taken port fails
the factory instead of the start call.

### Verification performed for this commit

- `cmake --build build/` green, **25/25 ctest**, **9/9 conn suite**.
- A request/response round trip over a real socket, **plain and over
  TLS** (self-signed certificate): framed request → decoded → framed
  response → decoded, in both cases.
- 25 consecutive plain runs and 25 consecutive TLS runs, no failures.
- valgrind on the TLS round trip: **0 errors**.
- The teardown path runs clean; the transport is stopped and destroyed
  with the pool still alive.

### Pre-existing issues hit again (NOT from this commit)

- The `scheduler_pool_destroy` / freed-manager defect documented above
  was hit again by the round-trip harness: the harness destroys the
  channel manager and then the scheduler pool, and the pool's registry
  walk touches the freed manager. Reproduces with a manager and a pool
  and **no transport at all**, so it is independent of this work. The
  fix belongs to `poseidon_channel_manager_destroy` (it frees the
  manager without detaching the manager's actor from the pool registry)
  and is not in this commit.
