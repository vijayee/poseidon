const net = require('net');
const { Transport } = require('./base');

class NodeNetTransport extends Transport {
  constructor(url) {
    super();
    this.url = url;
    this.socket = null;
  }

  async connect() {
    return new Promise((resolve, reject) => {
      const socket = new net.Socket();

      socket.on('data', (data) => {
        if (this.onMessage) this.onMessage(new Uint8Array(data));
      });

      socket.on('close', () => {
        if (this.onClose) this.onClose();
      });

      socket.on('error', (err) => {
        if (this.onError) this.onError(err);
        if (!this.socket) reject(err);
      });

      if (this.url.startsWith('unix://')) {
        const path = this.url.slice(7);
        socket.connect(path, () => { this.socket = socket; resolve(); });
      } else if (this.url.startsWith('tcp://')) {
        const hostPort = this.url.slice(6);
        const colon = hostPort.lastIndexOf(':');
        if (colon < 0) { reject(new Error('Invalid TCP URL: ' + this.url)); return; }
        const host = hostPort.slice(0, colon);
        const port = parseInt(hostPort.slice(colon + 1), 10);
        socket.connect(port, host, () => { this.socket = socket; resolve(); });
      } else {
        reject(new Error('Unsupported URL scheme: ' + this.url));
      }
    });
  }

  async disconnect() {
    if (this.socket) { this.socket.destroy(); this.socket = null; }
  }

  send(data) {
    if (!this.socket) throw new Error('Not connected');
    this.socket.write(Buffer.from(data));
  }
}

exports.NodeNetTransport = NodeNetTransport;