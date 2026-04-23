const {
  encodeRequest, encodeAdminRequest, encodeResponse, encodeEvent,
  decodeFrame, writeFrameHeader, readFrameHeader, wrapFrame,
  FRAME_HEADER_SIZE,
} = require('../../src/web/protocol');
const {
  FRAME_REQUEST, FRAME_RESPONSE, FRAME_EVENT,
  METHOD_CHANNEL_CREATE, METHOD_CHANNEL_JOIN, METHOD_PUBLISH,
} = require('../../src/types');

describe('protocol', () => {
  describe('frame header', () => {
    it('writes and reads a 4-byte big-endian length', () => {
      const header = writeFrameHeader(0x01020304);
      expect(header).toHaveLength(4);
      expect(header[0]).toBe(0x01);
      expect(header[1]).toBe(0x02);
      expect(header[2]).toBe(0x03);
      expect(header[3]).toBe(0x04);
      expect(readFrameHeader(header)).toBe(0x01020304);
    });

    it('handles zero length', () => {
      const header = writeFrameHeader(0);
      expect(readFrameHeader(header)).toBe(0);
    });

    it('handles max 32-bit value', () => {
      const header = writeFrameHeader(0xffffffff);
      expect(readFrameHeader(header)).toBe(0xffffffff);
    });

    it('FRAME_HEADER_SIZE is 4', () => {
      expect(FRAME_HEADER_SIZE).toBe(4);
    });
  });

  describe('wrapFrame', () => {
    it('prepends 4-byte length header', () => {
      const payload = new Uint8Array([1, 2, 3]);
      const wrapped = wrapFrame(payload);
      expect(wrapped.length).toBe(7);
      // First 4 bytes = length (3) in big-endian
      expect(wrapped[0]).toBe(0);
      expect(wrapped[1]).toBe(0);
      expect(wrapped[2]).toBe(0);
      expect(wrapped[3]).toBe(3);
      // Remaining bytes = payload
      expect(wrapped[4]).toBe(1);
      expect(wrapped[5]).toBe(2);
      expect(wrapped[6]).toBe(3);
    });
  });

  describe('encodeRequest / decodeFrame', () => {
    it('round-trips a request without payload', () => {
      const encoded = encodeRequest(42, METHOD_CHANNEL_CREATE, '/test/channel');
      const decoded = decodeFrame(encoded);
      expect(decoded).not.toBeNull();
      expect(decoded.frameType).toBe(FRAME_REQUEST);
      expect(decoded.requestId).toBe(42);
      expect(decoded.method).toBe(METHOD_CHANNEL_CREATE);
      expect(decoded.topicPath).toBe('/test/channel');
      expect(decoded.payload).toBeInstanceOf(Uint8Array);
      expect(decoded.payload.length).toBe(0);
    });

    it('round-trips a request with payload', () => {
      const payload = new Uint8Array([0xde, 0xad, 0xbe, 0xef]);
      const encoded = encodeRequest(7, METHOD_PUBLISH, '/topic', payload);
      const decoded = decodeFrame(encoded);
      expect(decoded.frameType).toBe(FRAME_REQUEST);
      expect(decoded.requestId).toBe(7);
      expect(decoded.method).toBe(METHOD_PUBLISH);
      expect(decoded.topicPath).toBe('/topic');
      expect(decoded.payload).toBeInstanceOf(Uint8Array);
      expect(Array.from(decoded.payload)).toEqual([0xde, 0xad, 0xbe, 0xef]);
    });
  });

  describe('encodeAdminRequest / decodeFrame', () => {
    it('round-trips an admin request without config', () => {
      const sig = new Uint8Array(64).fill(0xab);
      const encoded = encodeAdminRequest(1, METHOD_CHANNEL_CREATE, '/admin/channel', sig);
      const decoded = decodeFrame(encoded);
      expect(decoded.frameType).toBe(FRAME_REQUEST);
      expect(decoded.requestId).toBe(1);
      expect(decoded.method).toBe(METHOD_CHANNEL_CREATE);
      expect(decoded.topicPath).toBe('/admin/channel');
      expect(decoded.signature).toBeInstanceOf(Uint8Array);
      expect(decoded.signature.length).toBe(64);
      expect(decoded.signature[0]).toBe(0xab);
    });

    it('round-trips an admin request with config data', () => {
      const sig = new Uint8Array(64).fill(0xcd);
      const config = new Uint8Array([0x01, 0x02, 0x03]);
      const encoded = encodeAdminRequest(2, METHOD_CHANNEL_CREATE, '/admin/channel', sig, config);
      const decoded = decodeFrame(encoded);
      expect(decoded.signature).toBeInstanceOf(Uint8Array);
      expect(decoded.configData).toBeInstanceOf(Uint8Array);
      expect(Array.from(decoded.configData)).toEqual([0x01, 0x02, 0x03]);
    });
  });

  describe('encodeResponse / decodeFrame', () => {
    it('round-trips a success response', () => {
      const encoded = encodeResponse(10, 0, 'result-topic-id');
      const decoded = decodeFrame(encoded);
      expect(decoded.frameType).toBe(FRAME_RESPONSE);
      expect(decoded.requestId).toBe(10);
      expect(decoded.errorCode).toBe(0);
      expect(decoded.resultData).toBe('result-topic-id');
    });

    it('round-trips an error response', () => {
      const encoded = encodeResponse(5, 3, '');
      const decoded = decodeFrame(encoded);
      expect(decoded.frameType).toBe(FRAME_RESPONSE);
      expect(decoded.errorCode).toBe(3);
      expect(decoded.resultData).toBe('');
    });
  });

  describe('encodeEvent / decodeFrame', () => {
    it('round-trips a message event', () => {
      const data = new Uint8Array([1, 2, 3, 4]);
      const encoded = encodeEvent(1, 'topic-abc', 'subtopic', data);
      const decoded = decodeFrame(encoded);
      expect(decoded.frameType).toBe(FRAME_EVENT);
      expect(decoded.eventType).toBe(1);
      expect(decoded.topicId).toBe('topic-abc');
      expect(decoded.subtopic).toBe('subtopic');
      expect(decoded.payload).toBeInstanceOf(Uint8Array);
      expect(Array.from(decoded.payload)).toEqual([1, 2, 3, 4]);
    });
  });

  describe('decodeFrame edge cases', () => {
    it('returns null for empty input', () => {
      expect(decodeFrame(new Uint8Array(0))).toBeNull();
    });

    it('returns null for invalid CBOR', () => {
      expect(decodeFrame(new Uint8Array([0xff, 0xff, 0xff]))).toBeNull();
    });

    it('returns null for unknown frame type', () => {
      // Manually encode a CBOR array with unknown frame type
      const { encode: cborEncode } = require('cbor-x');
      const buf = new Uint8Array(cborEncode([0x99, 1, 0, 'test', new Uint8Array(0)]));
      expect(decodeFrame(buf)).toBeNull();
    });
  });
});