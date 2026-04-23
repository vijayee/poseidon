package com.poseidon.client

/**
 * Kotlin coroutine API for Poseidon pub/sub.
 * All methods are suspend functions that can be called from coroutines.
 */
class PoseidonClient(private val connection: PoseidonConnection) {

    companion object {
        // Method codes matching client_protocol.h
        private const val METHOD_CHANNEL_CREATE = 1
        private const val METHOD_CHANNEL_JOIN = 2
        private const val METHOD_CHANNEL_LEAVE = 3
        private const val METHOD_CHANNEL_DESTROY = 4
        private const val METHOD_CHANNEL_MODIFY = 5
        private const val METHOD_SUBSCRIBE = 6
        private const val METHOD_UNSUBSCRIBE = 7
        private const val METHOD_PUBLISH = 8
        private const val METHOD_ALIAS_REGISTER = 9
        private const val METHOD_ALIAS_UNREGISTER = 10
    }

    suspend fun createChannel(name: String): String {
        val response = connection.sendRequest(METHOD_CHANNEL_CREATE, name)
        return decodeTopicId(response)
    }

    suspend fun joinChannel(topicOrAlias: String): String {
        val response = connection.sendRequest(METHOD_CHANNEL_JOIN, topicOrAlias)
        return decodeTopicId(response)
    }

    suspend fun leaveChannel(topicId: String) {
        connection.sendRequest(METHOD_CHANNEL_LEAVE, topicId)
    }

    suspend fun destroyChannel(topicId: String, ownerKey: ByteArray) {
        connection.sendRequest(METHOD_CHANNEL_DESTROY, topicId, ownerKey)
    }

    suspend fun modifyChannel(topicId: String, config: Map<String, Any>, ownerKey: ByteArray) {
        // Simplified — production would serialize config as CBOR
        connection.sendRequest(METHOD_CHANNEL_MODIFY, topicId, ownerKey)
    }

    suspend fun subscribe(topicPath: String) {
        connection.sendRequest(METHOD_SUBSCRIBE, topicPath)
    }

    suspend fun unsubscribe(topicPath: String) {
        connection.sendRequest(METHOD_UNSUBSCRIBE, topicPath)
    }

    suspend fun publish(topicPath: String, data: ByteArray) {
        connection.sendRequest(METHOD_PUBLISH, topicPath, data)
    }

    suspend fun registerAlias(name: String, topicId: String) {
        connection.sendRequest(METHOD_ALIAS_REGISTER, name, topicId.toByteArray())
    }

    suspend fun unregisterAlias(name: String) {
        connection.sendRequest(METHOD_ALIAS_UNREGISTER, name)
    }

    fun onDelivery(callback: (topicId: String, subtopic: String, data: ByteArray) -> Unit) {
        connection.onDelivery(callback)
    }

    private fun decodeTopicId(response: ByteArray): String {
        // Simplified — production would decode CBOR response
        return String(response, Charsets.UTF_8)
    }
}