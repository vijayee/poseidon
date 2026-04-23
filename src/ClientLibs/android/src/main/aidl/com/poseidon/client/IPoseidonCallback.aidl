package com.poseidon.client;

/**
 * Callback interface for receiving events from the Poseidon daemon.
 */
oneway interface IPoseidonCallback {
    /**
     * Called when a message is delivered on a subscribed topic.
     */
    void onDelivery(String topicId, String subtopic, in byte[] data);

    /**
     * Called when a daemon event occurs.
     */
    void onEvent(int eventType, in byte[] data);
}