let nativeBinding = null;
try {
  nativeBinding = require('../../build/Release/poseidon_node_native.node');
} catch {
  // Native addon not available — user must run cmake-js or use web fallback
}

const { PoseidonClient: WebClient } = require('../web/client');

class PoseidonClientNative {
  constructor() {
    this._handle = null;
  }

  async connect(url) {
    if (!nativeBinding) throw new Error('Native addon not available');
    this._handle = new nativeBinding.PoseidonClientNative();
    await this._handle.connect(url);
  }

  async disconnect() {
    if (this._handle) { await this._handle.disconnect(); this._handle = null; }
  }

  async createChannel(name) {
    if (!this._handle) throw new Error('Not connected');
    return await this._handle.createChannel(name);
  }

  async joinChannel(topicOrAlias) {
    if (!this._handle) throw new Error('Not connected');
    return await this._handle.joinChannel(topicOrAlias);
  }

  async leaveChannel(topicId) {
    if (!this._handle) throw new Error('Not connected');
    await this._handle.leaveChannel(topicId);
  }

  async destroyChannel(topicId, ownerKeyPem) {
    if (!this._handle) throw new Error('Not connected');
    await this._handle.destroyChannel(topicId, ownerKeyPem);
  }

  async modifyChannel(topicId, config, ownerKeyPem) {
    if (!this._handle) throw new Error('Not connected');
    await this._handle.modifyChannel(topicId, config, ownerKeyPem);
  }

  async subscribe(topicPath) {
    if (!this._handle) throw new Error('Not connected');
    await this._handle.subscribe(topicPath);
  }

  async unsubscribe(topicPath) {
    if (!this._handle) throw new Error('Not connected');
    await this._handle.unsubscribe(topicPath);
  }

  async publish(topicPath, data) {
    if (!this._handle) throw new Error('Not connected');
    await this._handle.publish(topicPath, Buffer.from(data));
  }

  async registerAlias(name, topicId) {
    if (!this._handle) throw new Error('Not connected');
    await this._handle.registerAlias(name, topicId);
  }

  async unregisterAlias(name) {
    if (!this._handle) throw new Error('Not connected');
    await this._handle.unregisterAlias(name);
  }

  onMessage(cb) { if (this._handle) this._handle.onMessage(cb); }
  onEvent(cb) { if (this._handle) this._handle.onEvent(cb); }
}

PoseidonClientNative.isNativeAvailable = function () {
  return nativeBinding !== null;
};

exports.PoseidonClient = nativeBinding ? PoseidonClientNative : WebClient;
exports.PoseidonClientNative = PoseidonClientNative;

const types = require('../types');
Object.keys(types).forEach(key => { exports[key] = types[key]; });