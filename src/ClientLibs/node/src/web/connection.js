const { createTransport } = require('./transports');
const { decodeFrame, wrapFrame, readFrameHeader, FRAME_HEADER_SIZE } = require('./protocol');
const { FRAME_RESPONSE, FRAME_EVENT, EVENT_DELIVERY, errorFromCode } = require('../types');

class Connection {
  constructor() {
    this.transport = null;
    this.nextRequestId = 1;
    this.pending = new Map();
    this.readBuf = new Uint8Array(65536);
    this.readLen = 0;
    this.defaultTimeout = 30000;
    this.deliveryCb = null;
    this.eventCb = null;
  }

  setTimeout(ms) { this.defaultTimeout = ms; }
  onDelivery(cb) { this.deliveryCb = cb; }
  onEvent(cb) { this.eventCb = cb; }

  async connect(url) {
    this.transport = createTransport(url);
    this.transport.onMessage = (data) => this._onData(data);
    this.transport.onClose = () => this._onClose();
    this.transport.onError = (err) => this._onError(err);
    await this.transport.connect();
  }

  async disconnect() {
    if (this.transport) {
      await this.transport.disconnect();
      this.transport = null;
    }
    for (const [id, pending] of this.pending) {
      pending.reject(new Error('Disconnected'));
      if (pending.timer) clearTimeout(pending.timer);
    }
    this.pending.clear();
  }

  sendRequest(frameData) {
    return new Promise((resolve, reject) => {
      if (!this.transport) { reject(new Error('Not connected')); return; }

      const requestId = this.nextRequestId++;
      const timer = setTimeout(() => {
        this.pending.delete(requestId);
        reject(new Error('Request ' + requestId + ' timed out'));
      }, this.defaultTimeout);

      this.pending.set(requestId, { resolve, reject, timer });
      this.transport.send(wrapFrame(frameData));
    });
  }

  _onData(data) {
    if (this.readLen + data.length > this.readBuf.length) {
      const newBuf = new Uint8Array(this.readBuf.length * 2);
      newBuf.set(this.readBuf.subarray(0, this.readLen), 0);
      this.readBuf = newBuf;
    }
    this.readBuf.set(data, this.readLen);
    this.readLen += data.length;

    while (this.readLen >= FRAME_HEADER_SIZE) {
      const frameLen = readFrameHeader(this.readBuf.subarray(0, FRAME_HEADER_SIZE));
      if (frameLen === 0 || frameLen > this.readBuf.length) break;

      const total = FRAME_HEADER_SIZE + frameLen;
      if (this.readLen < total) break;

      const payload = this.readBuf.subarray(FRAME_HEADER_SIZE, total);
      const frame = decodeFrame(new Uint8Array(payload));

      if (frame) {
        if (frame.frameType === FRAME_RESPONSE) this._handleResponse(frame);
        else if (frame.frameType === FRAME_EVENT) this._handleEvent(frame);
      }

      const remaining = this.readLen - total;
      this.readBuf.copyWithin(0, total, this.readLen);
      this.readLen = remaining;
    }
  }

  _handleResponse(frame) {
    const pending = this.pending.get(frame.requestId);
    if (!pending) return;

    this.pending.delete(frame.requestId);
    if (pending.timer) clearTimeout(pending.timer);

    if (frame.errorCode === 0) {
      pending.resolve(frame.resultData);
    } else {
      pending.reject(errorFromCode(frame.errorCode));
    }
  }

  _handleEvent(frame) {
    if (frame.eventType === EVENT_DELIVERY && this.deliveryCb) {
      this.deliveryCb(frame.topicId, frame.subtopic, frame.payload);
    } else if (this.eventCb) {
      this.eventCb(frame.eventType, frame.payload);
    }
  }

  _onClose() {
    for (const [, pending] of this.pending) {
      pending.reject(new Error('Connection closed'));
      if (pending.timer) clearTimeout(pending.timer);
    }
    this.pending.clear();
  }

  _onError(err) {
    if (this.eventCb) this.eventCb(0, Buffer.from(err.message));
  }
}

exports.Connection = Connection;