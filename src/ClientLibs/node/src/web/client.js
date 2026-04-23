const { Connection } = require('./connection');
const { encodeRequest, encodeAdminRequest } = require('./protocol');
const {
  METHOD_CHANNEL_CREATE, METHOD_CHANNEL_JOIN, METHOD_CHANNEL_LEAVE,
  METHOD_CHANNEL_DESTROY, METHOD_CHANNEL_MODIFY,
  METHOD_SUBSCRIBE, METHOD_UNSUBSCRIBE, METHOD_PUBLISH,
  METHOD_ALIAS_REGISTER, METHOD_ALIAS_UNREGISTER,
} = require('../types');
const { sign } = require('@noble/ed25519');

class PoseidonClient {
  constructor() {
    this._conn = new Connection();
  }

  async connect(url) { await this._conn.connect(url); }
  async disconnect() { await this._conn.disconnect(); }

  async createChannel(name) {
    const frame = encodeRequest(0, METHOD_CHANNEL_CREATE, name);
    return await this._conn.sendRequest(frame);
  }

  async joinChannel(topicOrAlias) {
    const frame = encodeRequest(0, METHOD_CHANNEL_JOIN, topicOrAlias);
    return await this._conn.sendRequest(frame);
  }

  async leaveChannel(topicId) {
    const frame = encodeRequest(0, METHOD_CHANNEL_LEAVE, topicId);
    await this._conn.sendRequest(frame);
  }

  async destroyChannel(topicId, ownerKeyPem) {
    const { signature } = await this._signAdminPayload(METHOD_CHANNEL_DESTROY, topicId, ownerKeyPem);
    const frame = encodeAdminRequest(0, METHOD_CHANNEL_DESTROY, topicId, signature);
    await this._conn.sendRequest(frame);
  }

  async modifyChannel(topicId, config, ownerKeyPem) {
    const { signature } = await this._signAdminPayload(METHOD_CHANNEL_MODIFY, topicId, ownerKeyPem);
    const configData = this._encodeConfig(config);
    const frame = encodeAdminRequest(0, METHOD_CHANNEL_MODIFY, topicId, signature, configData);
    await this._conn.sendRequest(frame);
  }

  async subscribe(topicPath) {
    const frame = encodeRequest(0, METHOD_SUBSCRIBE, topicPath);
    await this._conn.sendRequest(frame);
  }

  async unsubscribe(topicPath) {
    const frame = encodeRequest(0, METHOD_UNSUBSCRIBE, topicPath);
    await this._conn.sendRequest(frame);
  }

  async publish(topicPath, data) {
    const frame = encodeRequest(0, METHOD_PUBLISH, topicPath, data);
    await this._conn.sendRequest(frame);
  }

  async registerAlias(name, topicId) {
    const frame = encodeRequest(0, METHOD_ALIAS_REGISTER, name, Buffer.from(topicId));
    await this._conn.sendRequest(frame);
  }

  async unregisterAlias(name) {
    const frame = encodeRequest(0, METHOD_ALIAS_UNREGISTER, name);
    await this._conn.sendRequest(frame);
  }

  onDelivery(cb) { this._conn.onDelivery(cb); }
  onEvent(cb) { this._conn.onEvent(cb); }

  async _signAdminPayload(method, topicId, ownerKeyPem) {
    const topicBytes = Buffer.from(topicId, 'utf-8');
    const timestamp = BigInt(Date.now()) * 1000n;
    const payload = Buffer.alloc(1 + topicBytes.length + 8);
    payload[0] = method;
    topicBytes.copy(payload, 1);
    payload.writeBigUInt64BE(timestamp, 1 + topicBytes.length);

    const privateKey = this._extractEd25519PrivateKey(ownerKeyPem);
    const signature = await sign(payload, privateKey);
    return { signature };
  }

  _extractEd25519PrivateKey(pem) {
    const lines = pem.trim().split('\n');
    const b64 = lines.filter(l => !l.startsWith('-----')).join('');
    const der = Buffer.from(b64, 'base64');
    // PKCS8 Ed25519: last 32 bytes are the private key seed
    if (der.length < 32) throw new Error('Invalid Ed25519 PEM: too short');
    return new Uint8Array(der.subarray(der.length - 32));
  }

  _encodeConfig(config) {
    // Raw bytes matching sizeof(poseidon_channel_config_t): 10 + 8 uint32 LE = 72 bytes
    const buf = Buffer.alloc(18 * 4);
    let offset = 0;
    for (let i = 0; i < 10; i++) {
      buf.writeUInt32LE(config.ringSizes[i] || 0, offset);
      offset += 4;
    }
    buf.writeUInt32LE(config.gossipInitIntervalS, offset); offset += 4;
    buf.writeUInt32LE(config.gossipSteadyIntervalS, offset); offset += 4;
    buf.writeUInt32LE(config.gossipNumInitIntervals, offset); offset += 4;
    buf.writeUInt32LE(config.quasarMaxHops, offset); offset += 4;
    buf.writeUInt32LE(config.quasarAlpha, offset); offset += 4;
    buf.writeUInt32LE(config.quasarSeenSize, offset); offset += 4;
    buf.writeUInt32LE(config.quasarSeenHashes, offset); offset += 4;
    return new Uint8Array(buf);
  }
}

exports.PoseidonClient = PoseidonClient;