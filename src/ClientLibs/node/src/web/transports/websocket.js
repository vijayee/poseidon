const { Transport } = require('./base');

class WebSocketTransport extends Transport {
  constructor(url) {
    super();
    this.url = url;
    this.ws = null;
  }

  async connect() {
    return new Promise((resolve, reject) => {
      let WS;
      if (typeof WebSocket !== 'undefined') {
        WS = WebSocket;
      } else {
        try { WS = require('ws'); } catch { reject(new Error('WebSocket not available')); return; }
      }

      const ws = new WS(this.url);
      ws.binaryType = 'arraybuffer';

      ws.onopen = () => { this.ws = ws; resolve(); };

      ws.onmessage = (event) => {
        if (this.onMessage) {
          const data = event.data instanceof ArrayBuffer
            ? new Uint8Array(event.data)
            : new Uint8Array(event.data);
          this.onMessage(data);
        }
      };

      ws.onclose = () => { if (this.onClose) this.onClose(); };

      ws.onerror = () => {
        const err = new Error('WebSocket error: ' + this.url);
        if (!this.ws) reject(err);
        else if (this.onError) this.onError(err);
      };
    });
  }

  async disconnect() {
    if (this.ws) { this.ws.close(); this.ws = null; }
  }

  send(data) {
    if (!this.ws) throw new Error('Not connected');
    this.ws.send(data);
  }
}

exports.WebSocketTransport = WebSocketTransport;