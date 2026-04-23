const { encode: cborEncode, decode: cborDecode } = require('cbor-x');
const { FRAME_REQUEST, FRAME_RESPONSE, FRAME_EVENT } = require('../types');

exports.FRAME_HEADER_SIZE = 4;

function toUint8Array(val) {
  if (val instanceof Uint8Array) return val;
  if (val instanceof ArrayBuffer) return new Uint8Array(val);
  if (ArrayBuffer.isView(val)) return new Uint8Array(val.buffer, val.byteOffset, val.byteLength);
  if (typeof val === 'string') return Buffer.from(val, 'utf-8');
  return new Uint8Array(0);
}

exports.encodeRequest = function (requestId, method, topicPath, payload) {
  const arr = payload
    ? [FRAME_REQUEST, requestId, method, topicPath, Buffer.from(payload)]
    : [FRAME_REQUEST, requestId, method, topicPath, new Uint8Array(0)];
  return new Uint8Array(cborEncode(arr));
};

exports.encodeAdminRequest = function (requestId, method, topicPath, signature, configData) {
  const arr = configData
    ? [FRAME_REQUEST, requestId, method, topicPath, Buffer.from(signature), Buffer.from(configData)]
    : [FRAME_REQUEST, requestId, method, topicPath, Buffer.from(signature), new Uint8Array(0)];
  return new Uint8Array(cborEncode(arr));
};

exports.encodeResponse = function (requestId, errorCode, resultData) {
  return new Uint8Array(cborEncode([FRAME_RESPONSE, requestId, errorCode, resultData]));
};

exports.encodeEvent = function (eventType, topicId, subtopic, data) {
  return new Uint8Array(cborEncode([FRAME_EVENT, eventType, topicId, subtopic, Buffer.from(data)]));
};

exports.decodeFrame = function (buf) {
  try {
    const arr = cborDecode(buf);
    if (!Array.isArray(arr) || arr.length < 1) return null;

    const frameType = arr[0];

    if (frameType === FRAME_REQUEST && arr.length >= 6) {
      return {
        frameType,
        requestId: arr[1],
        method: arr[2],
        topicPath: String(arr[3]),
        signature: toUint8Array(arr[4]),
        configData: toUint8Array(arr[5]),
      };
    }

    if (frameType === FRAME_REQUEST && arr.length >= 5) {
      return {
        frameType,
        requestId: arr[1],
        method: arr[2],
        topicPath: String(arr[3]),
        payload: toUint8Array(arr[4]),
      };
    }

    if (frameType === FRAME_RESPONSE && arr.length >= 4) {
      return {
        frameType,
        requestId: arr[1],
        errorCode: arr[2],
        resultData: String(arr[3]),
      };
    }

    if (frameType === FRAME_EVENT && arr.length >= 5) {
      return {
        frameType,
        eventType: arr[1],
        topicId: String(arr[2]),
        subtopic: String(arr[3]),
        payload: toUint8Array(arr[4]),
      };
    }

    return null;
  } catch {
    return null;
  }
};

exports.writeFrameHeader = function (len) {
  const buf = new Uint8Array(4);
  buf[0] = (len >> 24) & 0xff;
  buf[1] = (len >> 16) & 0xff;
  buf[2] = (len >> 8) & 0xff;
  buf[3] = len & 0xff;
  return buf;
};

exports.readFrameHeader = function (buf) {
  return ((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]) >>> 0;
};

exports.wrapFrame = function (payload) {
  const header = exports.writeFrameHeader(payload.length);
  const frame = new Uint8Array(4 + payload.length);
  frame.set(header, 0);
  frame.set(payload, 4);
  return frame;
};