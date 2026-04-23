const { Transport } = require('./base');

class WebTransportTransport extends Transport {
  constructor(url) {
    super();
    this.url = url;
    this.session = null;
    this.stream = null;
    this.reader = null;
    this.writer = null;
  }

  async connect() {
    if (typeof WebTransport === 'undefined') {
      throw new Error('WebTransport not available in this environment');
    }

    this.session = new WebTransport(this.url);
    await this.session.ready;

    this.stream = await this.session.createBidirectionalStream();
    this.reader = this.stream.readable.getReader();
    this.writer = this.stream.writable.getWriter();

    this._readLoop().catch(() => {
      if (this.onClose) this.onClose();
    });
  }

  async _readLoop() {
    while (true) {
      const { value, done } = await this.reader.read();
      if (done) break;
      if (this.onMessage && value) this.onMessage(value);
    }
  }

  async disconnect() {
    if (this.writer) { await this.writer.close().catch(() => {}); this.writer = null; }
    if (this.reader) { await this.reader.cancel().catch(() => {}); this.reader = null; }
    this.stream = null;
    if (this.session) { this.session.close(); this.session = null; }
  }

  async send(data) {
    if (!this.writer) throw new Error('Not connected');
    await this.writer.write(data);
  }
}

exports.WebTransportTransport = WebTransportTransport;