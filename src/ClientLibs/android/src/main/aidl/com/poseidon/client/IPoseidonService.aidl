package com.poseidon.client;

import com.poseidon.client.IPoseidonCallback;

/**
 * AIDL interface for the Poseidon daemon service.
 * The daemon implements this interface and exposes it via Binder.
 */
interface IPoseidonService {
    /**
     * Sends a framed request to the Poseidon daemon.
     *
     * @param frame  CBOR-encoded frame bytes (4-byte length prefix + payload)
     * @return       CBOR-encoded response frame bytes
     */
    byte[] sendFrame(in byte[] frame);

    /**
     * Registers a callback for receiving delivery and event notifications.
     */
    void registerCallback(IPoseidonCallback callback);

    /**
     * Unregisters a callback.
     */
    void unregisterCallback(IPoseidonCallback callback);
}