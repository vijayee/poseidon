const { Connection } = require('../../src/web/connection');
const {
  encodeResponse, encodeEvent, wrapFrame,
} = require('../../src/web/protocol');
const {
  FRAME_RESPONSE, FRAME_EVENT, EVENT_MESSAGE, ERROR_CHANNEL_NOT_FOUND,
} = require('../../src/types');

// Mock transport that records sends and allows injecting server frames
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

  send(data) {
    this.sent.push(new Uint8Array(data));
  }

  // Test helper: simulate server sending data
  receive(data) {
    if (this.onMessage) this.onMessage(new Uint8Array(data));
  }

  // Test helper: simulate connection close
  close() {
    if (this.onClose) this.onClose();
  }

  // Test helper: simulate error
  error(err) {
    if (this.onError) this.onError(err);
  }
}

// Replace createTransport with our mock
jest.mock('../../src/web/transports', () => ({
  createTransport: () => new MockTransport(),
}));

describe('Connection', () => {
  let conn;

  beforeEach(() => {
    conn = new Connection();
  });

  describe('connect/disconnect', () => {
    it('connects and sets up transport', async () => {
      await conn.connect('mock://test');
      expect(conn.transport).not.toBeNull();
      expect(conn.transport.connected).toBe(true);
    });

    it('disconnects and clears pending', async () => {
      await conn.connect('mock://test');
      await conn.disconnect();
      expect(conn.transport).toBeNull();
      expect(conn.pending.size).toBe(0);
    });
  });

  describe('sendRequest', () => {
    beforeEach(async () => {
      await conn.connect('mock://test');
    });

    afterEach(async () => {
      await conn.disconnect();
    });

    it('sends wrapped frame and resolves on response', async () => {
      const requestPromise = conn.sendRequest(new Uint8Array([1, 2, 3]));

      // Should have sent a wrapped frame
      expect(conn.transport.sent.length).toBe(1);

      // Simulate server response
      const requestId = conn.pending.keys().next().value;
      const responseFrame = encodeResponse(requestId, 0, 'topic-result');
      conn.transport.receive(wrapFrame(responseFrame));

      const result = await requestPromise;
      expect(result).toBe('topic-result');
    });

    it('rejects on error response', async () => {
      conn.defaultTimeout = 500;
      const requestPromise = conn.sendRequest(new Uint8Array([1, 2, 3]));

      const requestId = conn.pending.keys().next().value;
      const responseFrame = encodeResponse(requestId, ERROR_CHANNEL_NOT_FOUND, '');
      conn.transport.receive(wrapFrame(responseFrame));

      await expect(requestPromise).rejects.toThrow('Channel not found');
    });

    it('rejects on timeout', async () => {
      conn.defaultTimeout = 50;
      const requestPromise = conn.sendRequest(new Uint8Array([1, 2, 3]));

      await expect(requestPromise).rejects.toThrow('timed out');
    });

    it('rejects when not connected', async () => {
      await conn.disconnect();
      await expect(conn.sendRequest(new Uint8Array([1]))).rejects.toThrow('Not connected');
    });
  });

  describe('events', () => {
    beforeEach(async () => {
      await conn.connect('mock://test');
    });

    afterEach(async () => {
      await conn.disconnect();
    });

    it('dispatches message events to onMessage callback', () => {
      const deliveries = [];
      conn.onMessage((topicId, subtopic, data) => {
        deliveries.push({ topicId, subtopic, data });
      });

      const eventFrame = encodeEvent(EVENT_MESSAGE, 'topic-1', 'sub-1', new Uint8Array([0xaa]));
      conn.transport.receive(wrapFrame(eventFrame));

      expect(deliveries).toHaveLength(1);
      expect(deliveries[0].topicId).toBe('topic-1');
      expect(deliveries[0].subtopic).toBe('sub-1');
    });

    it('dispatches other events to onEvent callback', () => {
      const events = [];
      conn.onEvent((eventType, data) => {
        events.push({ eventType, data });
      });

      const eventFrame = encodeEvent(2, 'topic-1', '', new Uint8Array([0xbb]));
      conn.transport.receive(wrapFrame(eventFrame));

      expect(events).toHaveLength(1);
      expect(events[0].eventType).toBe(2);
    });
  });

  describe('frame buffering', () => {
    beforeEach(async () => {
      await conn.connect('mock://test');
    });

    afterEach(async () => {
      await conn.disconnect();
    });

    it('handles frames delivered in multiple chunks', async () => {
      const requestPromise = conn.sendRequest(new Uint8Array([1]));
      const requestId = conn.pending.keys().next().value;
      const responseFrame = encodeResponse(requestId, 0, 'chunked-result');
      const wrapped = wrapFrame(responseFrame);

      // Deliver first byte only
      conn.transport.receive(wrapped.subarray(0, 1));
      // Deliver the rest
      conn.transport.receive(wrapped.subarray(1));

      const result = await requestPromise;
      expect(result).toBe('chunked-result');
    });

    it('handles multiple frames in a single data chunk', async () => {
      const deliveries = [];
      conn.onMessage((topicId, subtopic, data) => {
        deliveries.push({ topicId, subtopic });
      });

      const event1 = encodeEvent(EVENT_MESSAGE, 'topic-a', 'sub-a', new Uint8Array([1]));
      const event2 = encodeEvent(EVENT_MESSAGE, 'topic-b', 'sub-b', new Uint8Array([2]));
      const wrapped1 = wrapFrame(event1);
      const wrapped2 = wrapFrame(event2);

      // Concatenate both frames into one chunk
      const combined = new Uint8Array(wrapped1.length + wrapped2.length);
      combined.set(wrapped1, 0);
      combined.set(wrapped2, wrapped1.length);
      conn.transport.receive(combined);

      expect(deliveries).toHaveLength(2);
      expect(deliveries[0].topicId).toBe('topic-a');
      expect(deliveries[1].topicId).toBe('topic-b');
    });
  });

  describe('connection close', () => {
    it('rejects all pending requests on close', async () => {
      await conn.connect('mock://test');
      const p1 = conn.sendRequest(new Uint8Array([1]));
      const p2 = conn.sendRequest(new Uint8Array([2]));

      conn.transport.close();

      await expect(p1).rejects.toThrow('Connection closed');
      await expect(p2).rejects.toThrow('Connection closed');
    });
  });
});