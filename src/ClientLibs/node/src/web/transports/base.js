class Transport {
  constructor() {
    this.onMessage = null;
    this.onClose = null;
    this.onError = null;
  }

  async connect() { throw new Error('Not implemented'); }
  async disconnect() { throw new Error('Not implemented'); }
  send(data) { throw new Error('Not implemented'); }
}

exports.Transport = Transport;