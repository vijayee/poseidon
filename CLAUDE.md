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

