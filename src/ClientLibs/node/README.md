# poseidon-client

Node.js and browser client for the Poseidon P2P pub/sub network.

Two implementations, one API:

- **Native** — N-API addon wrapping the C client library. Supports `unix://` and `tcp://` transports. Node.js only.
- **Web** — Pure JavaScript with CBOR wire protocol. Supports WebSocket (`ws://`/`wss://`) and QUIC via WebTransport (`https://`). Works in browsers and Node.js.

The package auto-selects native in Node.js and web in browsers via conditional exports.

## Install

```bash
npm install poseidon-client
```

For the native addon, you also need CMake and a C11 compiler:

```bash
npm run build:native
```

If the native addon can't be built, the web fallback is used automatically.

## Usage

```js
const { PoseidonClient } = require('poseidon-client');

const client = new PoseidonClient();

// Connect — use any supported URL
await client.connect('tcp://localhost:7000');     // native only
await client.connect('ws://localhost:7001');      // web + native
await client.connect('wss://example.com:7001');  // web + native
await client.connect('https://example.com');      // web only (WebTransport/QUIC)

// Channels
const topicId = await client.createChannel('my-channel');
await client.joinChannel(topicId);
await client.subscribe(topicId + '/events');

// Publish
await client.publish(topicId + '/events', Buffer.from('hello'));

// Receive messages
client.onMessage((topicId, subtopic, data) => {
  console.log('Got message:', subtopic, data.toString());
});

// Admin operations (requires Ed25519 owner key PEM)
await client.destroyChannel(topicId, ownerKeyPem);
await client.modifyChannel(topicId, config, ownerKeyPem);

// Aliases
await client.registerAlias('my-alias', topicId);
await client.unregisterAlias('my-alias');

// Disconnect
await client.disconnect();
```

## API

### `PoseidonClient`

| Method | Returns | Description |
|--------|---------|-------------|
| `connect(url)` | `Promise<void>` | Connect to a Poseidon node |
| `disconnect()` | `Promise<void>` | Disconnect from the node |
| `createChannel(name)` | `Promise<string>` | Create a channel, returns topic ID |
| `joinChannel(topicOrAlias)` | `Promise<string>` | Join a channel |
| `leaveChannel(topicId)` | `Promise<void>` | Leave a channel |
| `destroyChannel(topicId, ownerKeyPem)` | `Promise<void>` | Destroy a channel (owner only) |
| `modifyChannel(topicId, config, ownerKeyPem)` | `Promise<void>` | Modify channel config (owner only) |
| `subscribe(topicPath)` | `Promise<void>` | Subscribe to a topic path |
| `unsubscribe(topicPath)` | `Promise<void>` | Unsubscribe from a topic path |
| `publish(topicPath, data)` | `Promise<void>` | Publish data to a topic path |
| `registerAlias(name, topicId)` | `Promise<void>` | Register a channel alias |
| `unregisterAlias(name)` | `Promise<void>` | Unregister a channel alias |
| `onMessage(cb)` | `void` | Set message callback: `(topicId, subtopic, data) => void` |
| `onEvent(cb)` | `void` | Set event callback: `(eventType, data) => void` |

### `ChannelConfig`

Used with `modifyChannel`:

```js
{
  ringSizes: [8, 8, 8, 8, 8, 8, 8, 8, 8, 8],
  gossipInitIntervalS: 5,
  gossipSteadyIntervalS: 30,
  gossipNumInitIntervals: 5,
  quasarMaxHops: 6,
  quasarAlpha: 3,
  quasarSeenSize: 128,
  quasarSeenHashes: 8,
}
```

### Transport URLs

| Scheme | Transport | Native | Web |
|--------|-----------|--------|-----|
| `unix://` | Unix domain socket | Yes | No |
| `tcp://` | TCP socket | Yes | No |
| `ws://` | WebSocket | Yes | Yes |
| `wss://` | WebSocket over TLS | Yes | Yes |
| `https://` | WebTransport (QUIC) | No | Yes |

### Native Availability

```js
const { PoseidonClientNative } = require('poseidon-client');

if (PoseidonClientNative.isNativeAvailable()) {
  // Native addon is loaded and ready
}
```

## Building the Native Addon

Requires CMake 3.16+, a C11 compiler, and OpenSSL development libraries.

```bash
npm run build:native
# or
npx cmake-js compile
```

The addon is built against the C client library at `../c/` and produces a `.node` file in `build/Release/`.

## License

GPLv3 — see project root LICENSE file.