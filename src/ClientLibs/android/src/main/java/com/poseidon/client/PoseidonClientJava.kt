package com.poseidon.client

import kotlinx.coroutines.runBlocking

/**
 * Java-compatible blocking wrappers for PoseidonClient.
 * Each method wraps the corresponding suspend function in runBlocking.
 * Intended for use from Java code; Kotlin callers should use PoseidonClient directly.
 */
class PoseidonClientJava(private val client: PoseidonClient) {

    fun createChannelBlocking(name: String): String =
        runBlocking { client.createChannel(name) }

    fun joinChannelBlocking(topicOrAlias: String): String =
        runBlocking { client.joinChannel(topicOrAlias) }

    fun leaveChannelBlocking(topicId: String) =
        runBlocking { client.leaveChannel(topicId) }

    fun destroyChannelBlocking(topicId: String, ownerKey: ByteArray) =
        runBlocking { client.destroyChannel(topicId, ownerKey) }

    fun modifyChannelBlocking(topicId: String, config: Map<String, Any>, ownerKey: ByteArray) =
        runBlocking { client.modifyChannel(topicId, config, ownerKey) }

    fun subscribeBlocking(topicPath: String) =
        runBlocking { client.subscribe(topicPath) }

    fun unsubscribeBlocking(topicPath: String) =
        runBlocking { client.unsubscribe(topicPath) }

    fun publishBlocking(topicPath: String, data: ByteArray) =
        runBlocking { client.publish(topicPath, data) }

    fun registerAliasBlocking(name: String, topicId: String) =
        runBlocking { client.registerAlias(name, topicId) }

    fun unregisterAliasBlocking(name: String) =
        runBlocking { client.unregisterAlias(name) }
}