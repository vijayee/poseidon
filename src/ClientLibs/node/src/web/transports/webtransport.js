const { Transport } = require('./base');

class WebTransportTransport extends Transport {
  constructor(url) {
    super();
    this.url = url;
    this.session = null;
    this.reader = null;
  }

  async connect() {
    if (typeof WebTransport === 'undefined') {
      throw new Error('WebTransport not available in this environment');
    }

    this.session = new WebTransport(this.url);
    await this.session.ready;

    const stream = await this.session.createBidirectionalStream();
    this.reader = stream.readable.getReader();

    this._readLoop(this.reader).catch(() => {
      if (this.onClose) this.onClose();
    });
  }

  async _readLoop(reader) {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      if (this.onMessage && value) this.onMessage(value);
    }
  }

  async disconnect() {
    if (this.reader) { await this.reader.cancel(); this.reader = null; }
    if (this.session) { this.session.close(); this.session = null; }
  }

  async send(data) {
    if (!this.session) throw new Error('Not connected');
    const stream = await this.session.createBidirectionalStream();
    const writer = stream.writable.getWriter();
    await writer.write(data);
    await writer.close();
  }
}

exports.WebTransportTransport = WebTransportTransport;