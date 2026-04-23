const {
  METHOD_CHANNEL_CREATE, METHOD_CHANNEL_JOIN, METHOD_CHANNEL_LEAVE,
  METHOD_CHANNEL_DESTROY, METHOD_CHANNEL_MODIFY,
  METHOD_SUBSCRIBE, METHOD_UNSUBSCRIBE, METHOD_PUBLISH,
  METHOD_ALIAS_REGISTER, METHOD_ALIAS_UNREGISTER, METHOD_ALIAS_RESOLVE,
  FRAME_REQUEST, FRAME_RESPONSE, FRAME_EVENT,
  ERROR_OK, ERROR_UNKNOWN_METHOD, ERROR_INVALID_PARAMS,
  ERROR_CHANNEL_NOT_FOUND, ERROR_ALIAS_AMBIGUOUS, ERROR_NOT_AUTHORIZED,
  ERROR_CHANNEL_EXISTS, ERROR_TOO_MANY_CHANNELS, ERROR_TRANSPORT,
  EVENT_MESSAGE, EVENT_CHANNEL_JOINED, EVENT_CHANNEL_LEFT, EVENT_PEER_EVENT,
  PoseidonError, errorFromCode,
} = require('../../src/types');

describe('types', () => {
  describe('method codes', () => {
    it('assigns sequential codes 1-11', () => {
      expect(METHOD_CHANNEL_CREATE).toBe(1);
      expect(METHOD_CHANNEL_JOIN).toBe(2);
      expect(METHOD_CHANNEL_LEAVE).toBe(3);
      expect(METHOD_CHANNEL_DESTROY).toBe(4);
      expect(METHOD_CHANNEL_MODIFY).toBe(5);
      expect(METHOD_SUBSCRIBE).toBe(6);
      expect(METHOD_UNSUBSCRIBE).toBe(7);
      expect(METHOD_PUBLISH).toBe(8);
      expect(METHOD_ALIAS_REGISTER).toBe(9);
      expect(METHOD_ALIAS_UNREGISTER).toBe(10);
      expect(METHOD_ALIAS_RESOLVE).toBe(11);
    });
  });

  describe('frame types', () => {
    it('defines distinct frame types', () => {
      expect(FRAME_REQUEST).toBe(0x01);
      expect(FRAME_RESPONSE).toBe(0x02);
      expect(FRAME_EVENT).toBe(0x03);
      expect(new Set([FRAME_REQUEST, FRAME_RESPONSE, FRAME_EVENT]).size).toBe(3);
    });
  });

  describe('error codes', () => {
    it('assigns sequential codes 0-8', () => {
      expect(ERROR_OK).toBe(0);
      expect(ERROR_UNKNOWN_METHOD).toBe(1);
      expect(ERROR_INVALID_PARAMS).toBe(2);
      expect(ERROR_CHANNEL_NOT_FOUND).toBe(3);
      expect(ERROR_ALIAS_AMBIGUOUS).toBe(4);
      expect(ERROR_NOT_AUTHORIZED).toBe(5);
      expect(ERROR_CHANNEL_EXISTS).toBe(6);
      expect(ERROR_TOO_MANY_CHANNELS).toBe(7);
      expect(ERROR_TRANSPORT).toBe(8);
    });
  });

  describe('event types', () => {
    it('assigns sequential codes 1-4', () => {
      expect(EVENT_MESSAGE).toBe(1);
      expect(EVENT_CHANNEL_JOINED).toBe(2);
      expect(EVENT_CHANNEL_LEFT).toBe(3);
      expect(EVENT_PEER_EVENT).toBe(4);
    });
  });

  describe('PoseidonError', () => {
    it('is an Error subclass with code', () => {
      const err = new PoseidonError('test', 42);
      expect(err).toBeInstanceOf(Error);
      expect(err.name).toBe('PoseidonError');
      expect(err.message).toBe('test');
      expect(err.code).toBe(42);
    });
  });

  describe('errorFromCode', () => {
    it('returns PoseidonError for known codes', () => {
      const err = errorFromCode(ERROR_CHANNEL_NOT_FOUND);
      expect(err).toBeInstanceOf(PoseidonError);
      expect(err.code).toBe(3);
      expect(err.message).toBe('Channel not found');
    });

    it('returns PoseidonError for unknown codes', () => {
      const err = errorFromCode(99);
      expect(err).toBeInstanceOf(PoseidonError);
      expect(err.code).toBe(99);
      expect(err.message).toContain('Unknown error');
    });

    it('returns OK for code 0', () => {
      const err = errorFromCode(ERROR_OK);
      expect(err.message).toBe('OK');
    });
  });
});