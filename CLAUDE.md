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
- **One actor, one thread**: a synchronous wrapper MUST drain with `actor_run_inline(&actor, 32)`, never `actor_run`. The plain run does not claim the actor, and `actor_send` injects the actor into the scheduler pool when the mailbox was empty — a pool worker can then run the very same actor while the wrapper runs it, and two threads popping one mailbox free the same node. `meridian_conn`'s wrappers were the last ones still on the plain run; they were moved in `758dcf1`.
- **2.16 (committed)**: the Meridian relay client is actor-owned and its mutex is gone. The public API and the msquic callbacks both hand their work to the relay's actor; the two re-entrancy rules (an event that arrives while the actor runs on this thread, and an application callback that calls back in) are in the 2.16 section below.

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

## Phase 7 — WebSocket backend split landed (commit `68e2c55`)

The WebSocket backend is the third backend moved to the actor model. It
follows the TCP split: the difference is the WebSocket framing and the
HTTP upgrade, not the threading.

### New file layout (src/ClientAPIs/)

| File | Role |
|---|---|
| `transport_ws.h` / `transport_ws.c` | The listener: listening socket, accept watcher, TLS context, I/O thread, connection table. |
| `ws_connection.h` / `ws_connection.c` | The per-connection actor: TLS handshake, HTTP upgrade, WebSocket frame decoder, CBOR request handling, write path, watcher ownership. |
| `ws_connection_messages.h` | Payload structs for the `POSEIDON_WS_*` message family. |

### The message family

`POSEIDON_WS_CONNECTION_READ / _HANGUP / _ERROR / _WRITABLE / _WRITE /
_CLOSE` — 6 entries, appended to the global `message_type_e`. The family
mirrors the TCP one for the same reason: a TLS connection's SSL object
may only be used from one thread, so the I/O thread's watcher callback
only signals readiness and the actor performs the read, the write and
the handshake.

### The wire format

A WebSocket **binary** frame carrying a 4-byte big-endian length prefix
followed by the CBOR payload. The connection owns the framing in both
directions: `poseidon_ws_connection_write()` builds the length prefix
and wraps it in a binary frame, and the frame decoder strips both.

### The HTTP upgrade

The connection actor owns the upgrade: it accumulates the request in its
receive buffer, computes the `Sec-WebSocket-Accept` value (the
base64-encoded SHA-1 of the client's key concatenated with the RFC 6455
handshake GUID) and queues the 101 response **on the write queue**, so
the response is ordered before the first WebSocket frame. The bytes
that follow the request in the same read stay in the receive buffer and
are handed to the frame decoder.

### Fixed along the way

- **`Sec-WebSocket-Accept` was a hardcoded constant.** The old backend
  answered every upgrade with the RFC's sample value
  (`dGhlIHNhbXBsZSBub25jZQ==`), so no real client could complete the
  handshake. The accept value is now computed from the client's key.
- **The send path was a stub.** `ws_transport_send` returned -1
  unconditionally, so the client session's deliveries never reached a
  WebSocket client. The send path is now a framed queue on the
  connection's write actor, like the TCP one.
- **The upgrade parser scanned past the receive buffer.** The old code
  walked the key with an unbounded `while (*p == ' ')` and then used
  `strstr` on a buffer that is not NUL-terminated. Both scans are now
  bounded by the number of bytes that are actually in the buffer.
- **A 64-bit frame length could overflow the length check.** The old
  decoder added the announced payload length to the header size without
  bounding it first, so a client that announced a huge length could
  make the check pass and push the parser past the buffer. The length
  is now bounded against the receive buffer first.
- **The pong frame's length byte was wrong for payloads over 125
  bytes.** The pong is now built by the same frame encoder as every
  other frame.
- **The blocking `SSL_accept` in the accept callback** stalled the I/O
  thread, as in the TCP backend. The handshake moved to the connection
  actor and retries on `SSL_ERROR_WANT_READ` / `SSL_ERROR_WANT_WRITE`.
- **The response frame was built in a stack VLA** whose size was the
  payload length. The frame builders allocate a `buffer_t` instead.
- **The sessions were never registered with the channel manager**, so a
  WebSocket client never received a channel delivery. The sessions are
  registered and unregistered now, like the Unix and TCP backends.
- The transport stored the listen port in the vtable's name field. The
  port has a real field now and the name is a name.

### API surface added (backend-specific, in `transport_ws.h`)

- `poseidon_transport_ws_create(port, cert_path, key_path, manager)` —
  same signature as before, so `poseidond.c` is unchanged.
- `poseidon_transport_ws_destroy` — the backend's own destroy, also
  installed in the vtable's `destroy` slot.
- `poseidon_transport_ws_start` / `_stop` / `_send`.

### Verification performed for this commit

- `cmake --build build/` green, **25/25 ctest**, **9/9 conn suite**.
- A full round trip over a real socket, **plain and over TLS**: the
  HTTP upgrade (with the accept value checked against the RFC 6455
  sample vector), a masked binary frame carrying a length-prefixed CBOR
  request, the framed answer, and a ping answered with a pong.
- 30 consecutive plain runs and 30 consecutive TLS runs, no failures.
- valgrind on the round trip: **0 errors**.

### Pre-existing issues hit by this commit (NOT from this commit)

- **The channel manager's synchronous session wrappers corrupt the heap
  when they are called concurrently.** `poseidon_channel_manager_register_session`
  and `..._unregister_session` do `actor_send` + an inline
  `actor_run(actor, 32)`. The `actor_send` injects the actor into the
  scheduler pool, and the pool worker then runs the **same** actor while
  the wrapper is still running it inline. `message_queue_pop` is not
  safe against two concurrent pops: both threads read the same tail,
  both advance the queue and both free the same node — the sentinel
  included — which glibc reports as "double free or corruption
  (fasttop)".
  Reproduces with no transport at all: four threads hammering
  register/unregister on one manager crash 5 runs out of 5. The
  transport teardown paths reach the same code when they unregister a
  client's session, so a WebSocket transport teardown hits it
  occasionally (about 1 run in 15 over TLS).
  The fix belongs in the actor machinery, not in a backend: an actor
  must only ever be run by one thread at a time. It is not in this
  commit.

## Phase 7 — WebTransport backend split landed (commit `5e88f99`)

The WebTransport backend is the fourth backend moved to the actor model.
It is the first one that does **not** sit on a poll-dancer loop: libwtf
owns the I/O, runs its own worker threads and delivers the data through
the transport's session and stream callbacks.

### New file layout (src/ClientAPIs/)

| File | Role |
|---|---|
| `transport_webtransport.h` / `transport_webtransport.c` | The listener: libwtf context and server, the session and stream callbacks, the connection table. |
| `wt_connection.h` / `wt_connection.c` | The per-connection actor: the stream, the frame decoder, the CBOR request handling, the write path. |
| `wt_connection_messages.h` | Payload structs for the `POSEIDON_WT_*` message family. |

### The message family

`POSEIDON_WT_CONNECTION_DATA / _WRITE / _CLOSE / _STREAM_OPENED /
_STREAM_CLOSED` — 5 entries, appended to the global `message_type_e`.

The family mirrors the TCP one with one difference: there is no socket
to read from. libwtf hands the received bytes to the transport's
callback, so `DATA` carries the bytes themselves instead of a
readiness signal. `STREAM_OPENED` announces the session's bidirectional
stream, so the actor is the only thread that ever touches it.

### The payloads are plain heap buffers, not `buffer_t`

libwtf frees the buffer of a completed send itself, and the buffers it
hands over for received data belong to libwtf as well. Both directions
therefore use plain `malloc`'d memory instead of the pooled
`buffer_t`: the `buffer_t` struct comes from the memory pool and must
not be freed with `free()`.

### The thread split

- libwtf's worker threads: the session and stream callbacks. They copy
  what libwtf handed them and hand it to the actor. They never decode
  a frame and never answer a request.
- Scheduler pool workers: the per-connection actors. They own the
  stream, decode the frames, answer the client session and write to
  the stream.
- Caller thread: create / start / stop / destroy.

### Teardown order

1. `poseidon_transport_webtransport_stop` — the libwtf server stops
   accepting sessions and the connections are asked to close.
2. `actor_destroy` on every connection actor. This has to happen
   **before** the libwtf context goes away: a running dispatch may
   still be writing to a stream, and the stream belongs to the context.
3. `wtf_server_destroy` and `wtf_context_destroy`. The context owns the
   worker threads that call the callbacks, so after this no callback can
   fire. A callback that arrives after the actors are quiesced is
   harmless: `actor_send` on a destroyed actor drops the message and
   frees its payload.
4. `scheduler_pool_wait_for_idle`.
5. Free the connections: detach the session, unregister it from the
   manager, free the connection.

### Fixed along the way

- **The transport kept its state in the vtable's `loop` field.** The
  `void* loop` slot now holds the poll-dancer loop (NULL here), and
  the backend state lives in the backend struct next to the shared
  actor-shaped body.
- **The port was stored in the vtable's name field** in the older
  backends. The WebTransport transport already used a real name; it
  keeps it.
- **The libwtf handles were destroyed before the clients.** The old
  teardown destroyed the client structs while libwtf could still call
  back into them. The order above closes that window.
- The transport had two mutexes (the vtable's `lock` and the backend's
  own) and destroyed both; there is one now.

### Verification performed for this commit

- `cmake --build build/` green, **25/25 ctest**, **9/9 conn suite**.
- The transport's lifecycle — create, start (the server binds and
  listens), stop, destroy — over 15 consecutive runs, no failures.
- **The data path is not covered by a test**: a WebTransport client
  needs HTTP/3, and neither the in-tree client library nor a test
  harness speaks it. The frame decoding and the request handling are
  the same code path the TCP backend exercises.

### Pre-existing issues hit by this commit (NOT from this commit)

- The channel manager's synchronous session wrappers corrupt the heap
  when they are called concurrently. The WebTransport teardown
  unregisters a session, so it reaches the same code. The defect and
  its reproduction are described in the WebSocket section above.

## Phase 7 — QUIC backend split landed (commit `9e2c2e8`)

The QUIC backend is the fifth backend moved to the actor model. It
follows the WebTransport split: msquic owns the I/O and delivers the
data through the transport's callbacks.

### New file layout (src/ClientAPIs/)

| File | Role |
|---|---|
| `transport_quic.h` / `transport_quic.c` | The listener: the msquic registration, configuration and listener, the connection and stream callbacks, the connection table. |
| `quic_connection.h` / `quic_connection.c` | The per-connection actor: the stream, the frame decoder, the CBOR request handling, the write path. |
| `quic_connection_messages.h` | Payload structs for the `POSEIDON_QUIC_*` message family. |

### The message family

`POSEIDON_QUIC_CONNECTION_DATA / _WRITE / _CLOSE / _STREAM_READY /
_PEER_SHUTDOWN` — 5 entries, appended to the global `message_type_e`.
The family mirrors the WebTransport one: msquic owns the I/O, so `DATA`
carries the bytes themselves instead of a readiness signal.

### The connection is per stream, not per connection

A QUIC connection can carry several streams. The old backend created
one client per peer stream and looked the client up by its session id.
The split keeps that: the connection callback's `PEER_STREAM_STARTED`
event creates the connection actor, and the stream handle is handed to
it at creation.

### Fixed along the way

- **The backend state was kept in the vtable's `loop` field.** The
  `void* loop` slot now holds the poll-dancer loop (NULL here), and
  the msquic handles live in the backend struct next to the shared
  actor-shaped body.
- **The connection callback freed the client structs.** The old
  `SHUTDOWN_COMPLETE` path removed the clients from the table and
  freed them while msquic could still call back into them. The
  callback now only closes the connections; the transport's teardown
  frees them once their actors are quiesced.
- **The transport had two mutexes** (the vtable's `lock` and the
  backend's own) and destroyed both; there is one now.

### Verification performed for this commit

- `cmake --build build/` green, **25/25 ctest**, **9/9 conn suite**.
- The transport's lifecycle — create, start (the listener binds), stop,
  destroy — over ten consecutive runs, no failures.
- **The data path cannot be exercised in this tree.** The in-tree QUIC
  client fails to establish its stream against the QUIC server:
  "quic client: failed to establish stream". The pre-split backend
  fails identically (verified by linking the previous commit's
  `transport_quic.c` against the same test), so the split does not
  regress anything — the QUIC transport simply does not work in this
  tree yet, for reasons that predate the actor conversion.

## Phase 7 — Binder and XPC backends moved to the actor shape (commits `cb0f13e`, `5385823`)

The last two backends are placeholders: the Binder service is
registered through JNI and XPC connections are managed by launchd.
Neither has a connection path yet — no accept callback, no per-client
state, a `send` that always returns -1.

What they own now is the actor-shaped transport body every backend
shares: the backend struct carries `poseidon_transport_internal_t` as
its first member, the transport owns an actor bound to the manager's
scheduler pool, and the vtable-shaped public view is filled from the
same allocation. That is the starting point the platform bridge needs.

**There is no per-connection actor in either backend.** With no
connection path there would be nothing for one to own, and the plan's
"splits" for these two are therefore listener-only. When the Android
and iOS work lands, the connection follows the Unix and TCP shape: an
actor per client that owns the frame decoder and the write path, fed
by the platform bridge.

Fixed along the way in both: the service name was a borrowed pointer
to the caller's string (it is copied now and freed in the destroy
path), the backend state was kept in the vtable's `void* loop` slot
(which holds the poll-dancer loop as everywhere else), and the
transport had a lock that was never taken and never freed.

Neither file is compiled on this platform (`__ANDROID__` and
`__APPLE__` guards). Both were checked by compiling them with the
platform guard defined: clean, no warnings.

## Phase 7 — the transport vtable is gone (commit `b35a579`)

`poseidon_transport_t` is the actor-shaped transport body now, and it is
declared in `transport.h`. The vtable that used to sit on it — `start`,
`stop`, `send` and `destroy` as function pointers — is gone, and with
it the second view of the transport.

### One view instead of two

A backend used to hold **both** the actor-shaped body and a
vtable-shaped public view, and converted between them with `offsetof`:

```c
typedef struct poseidon_unix_transport_t {
    poseidon_transport_internal_t base;   /* FIRST */
    poseidon_transport_t public;          /* the vtable view */
    ...
} poseidon_unix_transport_t;
```

There is one view now. The body is the FIRST member of the backend
struct, so a pointer to the transport **is** a pointer to the backend
and the conversion is a plain upcast:

```c
static poseidon_unix_transport_t* _unix_from_base(poseidon_transport_t* transport) {
    return (poseidon_unix_transport_t*)transport;
}
```

`poseidon_transport_internal_t` and `transport_internal.h` are gone: the
body, the destroy-stack types and the seven per-backend forward
declarations all live in the public header.

### The generic lifecycle functions

The vtable is replaced by four functions that dispatch on the body's
type field:

```c
int  poseidon_transport_start(poseidon_transport_t* transport);
int  poseidon_transport_stop(poseidon_transport_t* transport);
int  poseidon_transport_send(poseidon_transport_t* transport, int client_id,
                              const uint8_t* data, size_t len);
void poseidon_transport_destroy(poseidon_transport_t* transport);
```

They live in `transport.c`, which is the one place that includes every
backend header. They exist because not every caller knows the backend
at compile time: the client session sends through whatever transport its
session was created on (`client_session.c` calls
`poseidon_transport_send`), and the daemon drives five transports from
one place. A caller that does know the backend can still call
`poseidon_transport_tcp_start(...)` and friends directly.

### The plan's commits 9, 10 and 11 landed as one

The header rewrite breaks every vtable call site at once, so it cannot
be separated from the call-site updates without leaving the tree
unbuildable. They are one commit.

### Fixed along the way

- `poseidon_transport_unix_send` cast the public pointer to the
  internal body and then took the backend's address off it, which read
  past the end of the allocation. The cast is a no-op now.
- The transport carried a `lock` that the generic destroy released and
  nothing else ever used. It is gone; each backend owns the lock that
  guards its connection table.

### Where Phase 7 landed

| Commit | What |
|---|---|
| `afb9f4e` | `transport_internal.h` — the shared actor-shaped body |
| `97ea4a8` | The Unix backend split |
| `e2efe87` | The TCP backend split |
| `68e2c55` | The WebSocket backend split |
| `5e88f99` | The WebTransport backend split |
| `9e2c2e8` | The QUIC backend split |
| `cb0f13e` | The Binder backend, moved to the actor shape |
| `5385823` | The XPC backend, moved to the actor shape |
| `b35a579` | The vtable removed, the body renamed and made public |

Each backend commit has a CLAUDE.md record next to it.

### The shape every backend now has

- A listener (`transport_<backend>.c`) that owns the I/O: the
  poll-dancer loop and the I/O thread for the socket backends, the
  libwtf context and server for WebTransport, the msquic registration,
  configuration and listener for QUIC.
- A per-connection actor (`<backend>_connection.c`) on the transport's
  scheduler pool that owns the frame decoder, the CBOR request
  handling and the write path. The Binder and XPC backends have none
  yet: they have no connection path.
- A `POSEIDON_<BACKEND>_CONNECTION_*` message family in the global
  `message_type_e`.
- The transport body as the FIRST member of the backend struct.
- Watchers released through the transport's `destroy_stack_t`, so they
  are unregistered on the loop thread.

### Open items

1. **The QUIC and WebTransport data paths are not exercised.** The
   in-tree QUIC client fails to establish its stream against the server,
   and the pre-split backend fails identically, so this predates the
   actor conversion. WebTransport needs an HTTP/3 client, which neither
   the in-tree client library nor a test harness speaks. Both need a
   client that speaks the protocol, not a backend change.

## Phase 7 — the actor runs on one thread at a time (commits `be36bff`, `d2703e3`)

Two defects that the backend work surfaced, both fixed.

### An actor may only be run by one thread at a time (`be36bff`)

The synchronous wrappers — build a payload, `actor_send`, run the
actor, read the out-field — ran the actor inline on the calling
thread. The `actor_send` injects the actor into the scheduler pool when
the mailbox was empty, so a pool worker could pick the very same actor
up and run it while the wrapper was still running it.

`message_queue_pop` is not safe against two concurrent pops: both
threads read the same tail, both advance the queue and both free the
same node, the sentinel included. glibc reports that as "double free or
corruption (fasttop)".

It reproduces with **no transport at all**: four threads hammering the
channel manager's register/unregister wrappers crashed five runs out of
five before the fix and pass five out of five after it. The upstream
`liboffs` has no inline `actor_run` callers at all — the pattern is
poseidon's.

`actor_run_inline` claims the actor for the caller: a pool worker that
finds the claim taken re-queues the actor instead of running it, and an
inline caller waits for a worker that is already running the actor. The
wrapper's contract is unchanged — when it returns, the message it sent
has been dispatched and the out-field is visible.

The claim closes a use-after-free too: `actor_destroy` waits for
`ACTOR_FLAG_RUNNING`, so it now waits for an inline run as well. A
thread that destroys an actor from inside the actor's own inline
dispatch is recognised and does not wait for itself.

Two consequences the fix had to absorb:

- **`poseidon_channel_manager_destroy` now calls `actor_destroy`** instead
  of setting the DESTROY flag by hand. It frees the manager, so the
  actor has to be detached from the pool's registry first: the pool's
  teardown walked the freed manager and wrote into it (five invalid
  reads under valgrind, and the daemon's own teardown order triggers
  it).
- **The transports' teardown releases the sessions outside the
  connection lock.** The wrappers wait for a pool worker now, and that
  worker may be reaching back into the transport's send path, which
  takes the same lock. Only the connection structs are freed under it.

### A closing Unix connection drains its pending write (`d2703e3`)

The Unix connection's dispatch dropped every message once the
connection was closing. A graceful close sets the closing flag and
waits for the pending write to drain — but the drain is driven by the
writable message, which the very same early return dropped. The frame
was never flushed and the socket was never closed; only the transport's
teardown released it.

The dispatch now drops the messages of a **closed** connection and lets
a **closing** one finish its work, which is the shape the TCP and
WebSocket backends already have.

## Phase 2 (2.16) — the Meridian relay client is actor-owned

The relay client is the first Meridian subsystem that had to be moved
whole rather than one mutator at a time: it is a **foreign-thread**
subsystem. msquic delivers the connection and the stream events on its
own worker threads, and the application calls the public API from its
own thread. Both had to end up on the same actor, or the relay would
need two synchronization mechanisms.

### The shape

- `meridian_relay_t` embeds `actor_t actor;` and `scheduler_pool_t* pool;`
  by value. **The mutex is gone** — `platform_mutex_t* lock` was deleted
  from the struct, along with every `platform_mutex_lock` /
  `platform_mutex_unlock` in the file and the `;;` double-semicolon
  typos that came with it.
- `meridian_relay_create` takes the pool as its last argument. A NULL
  pool means the actor runs on whichever thread drains it.
- The message family is `MERIDIAN_RELAY_*` — 13 entries in the global
  `message_type_e`, payload structs in
  `src/Network/Meridian/meridian_relay_messages.h`. Six are the public
  API calls, seven are the msquic events.
- Every public function is a wrapper: build the payload, `actor_send`,
  `actor_run_inline(&relay->actor, 32)`, read the result. The result
  slot is the wrapper's own stack frame, borrowed by the handler — the
  same pattern the Meridian connection migration settled on.

### What the callbacks do and do not do

The msquic callbacks still **parse** — the wire format and the CBOR —
because that is pure work on the callback's own thread. What they no
longer do is touch the relay's state. The connection events, the peer
streams, the stream closes, the address response and the forwarded
datagrams are all handed to the actor.

The forwarded datagram is **copied** on the way in: its bytes live in
the stream buffer, which msquic releases as soon as the callback
returns, while the actor is the one that hands them to the application.

### The two re-entrancy cases

A handler that calls into msquic can get its answer on the same thread,
and that answer is a callback of ours. Two rules follow:

- A callback that arrives while the actor is already running on this
  thread **cannot be drained**: the actor is claimed by this thread, and
  a second claim would spin forever. The event stays in the mailbox and
  the run in progress picks it up. The stream close is the exception:
  the entry dies with the callback, so that one drops the entry
  directly.
- An application callback that calls back into the relay reaches a
  public wrapper on the actor's thread. The wrapper notices and runs its
  handler inline instead of queueing a message it could never wait for.

The second rule is what `actor_is_running_on_this_thread` exists for —
a poseidon-only addition to `src/Actor/actor.{c,h}`, next to
`actor_run_inline`, which is the other poseidon-only piece of the
actor's API. Upstream has no inline `actor_run` caller, so upstream has
no need for either.

### Build

Green, 25/25 ctest. The relay's own test still covers only the
creation paths; the connection paths need a real msquic peer.
