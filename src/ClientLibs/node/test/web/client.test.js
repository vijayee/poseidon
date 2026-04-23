const { PoseidonClient } = require('../../src/web/client');
const {
  encodeResponse, encodeEvent, wrapFrame,
} = require('../../src/web/protocol');
const {
  METHOD_CHANNEL_CREATE, METHOD_CHANNEL_JOIN, METHOD_CHANNEL_LEAVE,
  METHOD_CHANNEL_DESTROY, METHOD_CHANNEL_MODIFY,
  METHOD_SUBSCRIBE, METHOD_UNSUBSCRIBE, METHOD_PUBLISH,
  METHOD_ALIAS_REGISTER, METHOD_ALIAS_UNREGISTER,
  EVENT_MESSAGE,
} = require('../../src/types');

// Mock transport
class MockTransport {
  constructor() {
    this.onMessage = null;
    this.onClose = null;
    this.onError = null;
    this.sent = [];
    this.connected = false;
  }

  async connect() { this.connected = true; }
  async disconnect() { this.connected = false; }
  send(data) { this.sent.push(new Uint8Array(data)); }

  receive(data) {
    if (this.onMessage) this.onMessage(new Uint8Array(data));
  }
  close() { if (this.onClose) this.onClose(); }
  error(err) { if (this.onError) this.onError(err); }
}

jest.mock('../../src/web/transports', () => ({
  createTransport: () => new MockTransport(),
}));

jest.mock('@noble/ed25519', () => ({
  sign: jest.fn().mockResolvedValue(new Uint8Array(64)),
}));

describe('PoseidonClient', () => {
  let client;

  beforeEach(() => {
    client = new PoseidonClient();
  });

  afterEach(async () => {
    try { await client.disconnect(); } catch {}
  });

  async function connectAndWait() {
    await client.connect('mock://test');
  }

  // Helper: find the requestId from the pending map
  function getPendingRequestIds() {
    return Array.from(client._conn.pending.keys());
  }

  // Helper: simulate server response for the current pending request
  async function respondToLatest(errorCode, resultData) {
    const ids = getPendingRequestIds();
    if (ids.length === 0) throw new Error('No pending requests');
    const requestId = ids[ids.length - 1];
    const responseFrame = encodeResponse(requestId, errorCode, resultData);
    client._conn.transport.receive(wrapFrame(responseFrame));
  }

  describe('connect/disconnect', () => {
    it('connects without error', async () => {
      await connectAndWait();
      expect(client._conn.transport).not.toBeNull();
    });

    it('disconnects without error', async () => {
      await connectAndWait();
      await client.disconnect();
      expect(client._conn.transport).toBeNull();
    });
  });

  describe('channel operations', () => {
    beforeEach(async () => {
      await connectAndWait();
    });

    it('createChannel sends request and returns topic ID', async () => {
      const promise = client.createChannel('test-channel');
      await respondToLatest(0, 'topic-123');
      const result = await promise;
      expect(result).toBe('topic-123');
    });

    it('joinChannel sends request and returns topic ID', async () => {
      const promise = client.joinChannel('test-channel');
      await respondToLatest(0, 'topic-456');
      const result = await promise;
      expect(result).toBe('topic-456');
    });

    it('joinChannel works with topic ID', async () => {
      const promise = client.joinChannel('topic-456');
      await respondToLatest(0, 'topic-456');
      const result = await promise;
      expect(result).toBe('topic-456');
    });

    it('leaveChannel sends request and resolves on success', async () => {
      const promise = client.leaveChannel('topic-123');
      await respondToLatest(0, '');
      await expect(promise).resolves.toBeUndefined();
    });
  });

  describe('subscribe/unsubscribe/publish', () => {
    beforeEach(async () => {
      await connectAndWait();
    });

    it('subscribe sends request and resolves', async () => {
      const promise = client.subscribe('topic-1/events');
      await respondToLatest(0, '');
      await expect(promise).resolves.toBeUndefined();
    });

    it('unsubscribe sends request and resolves', async () => {
      const promise = client.unsubscribe('topic-1/events');
      await respondToLatest(0, '');
      await expect(promise).resolves.toBeUndefined();
    });

    it('publish sends request with data', async () => {
      const promise = client.publish('topic-1/events', Buffer.from('hello'));
      await respondToLatest(0, '');
      await expect(promise).resolves.toBeUndefined();
    });
  });

  describe('aliases', () => {
    beforeEach(async () => {
      await connectAndWait();
    });

    it('registerAlias sends request', async () => {
      const promise = client.registerAlias('my-alias', 'topic-1');
      await respondToLatest(0, '');
      await expect(promise).resolves.toBeUndefined();
    });

    it('unregisterAlias sends request', async () => {
      const promise = client.unregisterAlias('my-alias');
      await respondToLatest(0, '');
      await expect(promise).resolves.toBeUndefined();
    });
  });

  describe('message events', () => {
    beforeEach(async () => {
      await connectAndWait();
    });

    it('onMessage receives message events', () => {
      const deliveries = [];
      client.onMessage((topicId, subtopic, data) => {
        deliveries.push({ topicId, subtopic, data: Buffer.from(data).toString() });
      });

      const eventFrame = encodeEvent(EVENT_MESSAGE, 'topic-1', 'sub-1', Buffer.from('hello'));
      client._conn.transport.receive(wrapFrame(eventFrame));

      expect(deliveries).toHaveLength(1);
      expect(deliveries[0].topicId).toBe('topic-1');
      expect(deliveries[0].subtopic).toBe('sub-1');
      expect(deliveries[0].data).toBe('hello');
    });

    it('onEvent receives non-message events', () => {
      const events = [];
      client.onEvent((eventType, data) => {
        events.push({ eventType, data });
      });

      const eventFrame = encodeEvent(2, 'topic-1', '', Buffer.from('joined'));
      client._conn.transport.receive(wrapFrame(eventFrame));

      expect(events).toHaveLength(1);
      expect(events[0].eventType).toBe(2);
    });
  });

  describe('error handling', () => {
    it('throws when calling methods before connect', async () => {
      await expect(client.createChannel('x')).rejects.toThrow('Not connected');
      await expect(client.subscribe('x')).rejects.toThrow('Not connected');
    });
  });
});