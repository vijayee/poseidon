// Method codes — must match client_protocol.h
exports.METHOD_CHANNEL_CREATE = 1;
exports.METHOD_CHANNEL_JOIN = 2;
exports.METHOD_CHANNEL_LEAVE = 3;
exports.METHOD_CHANNEL_DESTROY = 4;
exports.METHOD_CHANNEL_MODIFY = 5;
exports.METHOD_SUBSCRIBE = 6;
exports.METHOD_UNSUBSCRIBE = 7;
exports.METHOD_PUBLISH = 8;
exports.METHOD_ALIAS_REGISTER = 9;
exports.METHOD_ALIAS_UNREGISTER = 10;
exports.METHOD_ALIAS_RESOLVE = 11;

// Frame types
exports.FRAME_REQUEST = 0x01;
exports.FRAME_RESPONSE = 0x02;
exports.FRAME_EVENT = 0x03;

// Error codes
exports.ERROR_OK = 0;
exports.ERROR_UNKNOWN_METHOD = 1;
exports.ERROR_INVALID_PARAMS = 2;
exports.ERROR_CHANNEL_NOT_FOUND = 3;
exports.ERROR_ALIAS_AMBIGUOUS = 4;
exports.ERROR_NOT_AUTHORIZED = 5;
exports.ERROR_CHANNEL_EXISTS = 6;
exports.ERROR_TOO_MANY_CHANNELS = 7;
exports.ERROR_TRANSPORT = 8;

// Event types
exports.EVENT_MESSAGE = 1;
exports.EVENT_CHANNEL_JOINED = 2;
exports.EVENT_CHANNEL_LEFT = 3;
exports.EVENT_PEER_EVENT = 4;

// Protocol limits
exports.MAX_TOPIC_PATH = 256;
exports.MAX_RESULT_DATA = 1024;
exports.MAX_SUBTOPIC = 256;
exports.MAX_PAYLOAD = 65536;
exports.MAX_SIGNATURE = 64;

const ERROR_MESSAGES = {
  0: 'OK',
  1: 'Unknown method',
  2: 'Invalid parameters',
  3: 'Channel not found',
  4: 'Alias is ambiguous',
  5: 'Not authorized',
  6: 'Channel already exists',
  7: 'Too many channels',
  8: 'Transport error',
};

class PoseidonError extends Error {
  constructor(message, code) {
    super(message);
    this.name = 'PoseidonError';
    this.code = code;
  }
}
exports.PoseidonError = PoseidonError;

exports.errorFromCode = function (code) {
  return new PoseidonError(ERROR_MESSAGES[code] || `Unknown error (${code})`, code);
};