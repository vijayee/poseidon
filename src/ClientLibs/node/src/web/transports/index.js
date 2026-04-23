const { NodeNetTransport } = require('./node-net');
const { WebSocketTransport } = require('./websocket');
const { WebTransportTransport } = require('./webtransport');

exports.createTransport = function (url) {
  if (url.startsWith('unix://') || url.startsWith('tcp://')) {
    if (typeof window !== 'undefined') {
      throw new Error('URL scheme not available in browser: ' + url);
    }
    return new NodeNetTransport(url);
  }
  if (url.startsWith('ws://') || url.startsWith('wss://')) {
    return new WebSocketTransport(url);
  }
  if (url.startsWith('https://')) {
    return new WebTransportTransport(url);
  }
  throw new Error('Unsupported transport URL: ' + url);
};