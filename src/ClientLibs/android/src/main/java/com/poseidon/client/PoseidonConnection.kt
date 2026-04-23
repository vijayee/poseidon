package com.poseidon.client

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import android.util.Log
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/**
 * Manages Binder connection to the Poseidon daemon.
 * Handles CBOR frame serialization/deserialization, request ID tracking,
 * and response matching.
 */
class PoseidonConnection(private val context: Context) {
    companion object {
        private const val TAG = "PoseidonConn"
        private const val FRAME_HEADER_SIZE = 4
    }

    private var binder: IPoseidonService? = null
    private var requestId = 0
    private val pendingRequests = mutableMapOf<Int, (ByteArray?) -> Unit>()
    private var messageCallback: ((String, String, ByteArray) -> Unit)? = null
    private var eventCallback: ((Int, ByteArray) -> Unit)? = null
    private var connected = false

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            binder = IPoseidonService.Stub.asInterface(service)
            connected = true
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            binder = null
            connected = false
        }
    }

    suspend fun connect(serviceIntent: ComponentName): PoseidonConnection {
        return suspendCancellableCoroutine { continuation ->
            val intent = Intent().setComponent(serviceIntent)
            context.bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE)
            continuation.resume(this@PoseidonConnection)
        }
    }

    fun disconnect() {
        try {
            context.unbindService(serviceConnection)
        } catch (e: Exception) {
            Log.w(TAG, "Error unbinding service", e)
        }
        connected = false
        binder = null
    }

    fun isConnected(): Boolean = connected && binder != null

    suspend fun sendRequest(method: Int, topicPath: String, payload: ByteArray? = null): ByteArray {
        val id = requestId++
        val frame = encodeRequest(id, method, topicPath, payload)
        return suspendCancellableCoroutine { continuation ->
            pendingRequests[id] = { response ->
                if (response != null) {
                    continuation.resume(response)
                } else {
                    continuation.resumeWithException(PoseidonException("No response"))
                }
            }
            try {
                binder?.sendFrame(frame)
            } catch (e: Exception) {
                pendingRequests.remove(id)
                continuation.resumeWithException(e)
            }
        }
    }

    fun handleResponse(requestId: Int, data: ByteArray?) {
        pendingRequests.remove(requestId)?.invoke(data)
    }

    fun handleMessage(topicId: String, subtopic: String, data: ByteArray) {
        messageCallback?.invoke(topicId, subtopic, data)
    }

    fun handleEvent(eventType: Int, data: ByteArray) {
        eventCallback?.invoke(eventType, data)
    }

    fun onMessage(callback: (topicId: String, subtopic: String, data: ByteArray) -> Unit) {
        messageCallback = callback
    }

    fun onEvent(callback: (eventType: Int, data: ByteArray) -> Unit) {
        eventCallback = callback
    }

    // CBOR frame encoding: [type=0x01, request_id, method, topic_path, payload]
    private fun encodeRequest(requestId: Int, method: Int, topicPath: String, payload: ByteArray?): ByteArray {
        // Simplified — production would use a CBOR library
        val payloadSize = payload?.size ?: 0
        val frame = ByteArray(4 + 4 + 1 + topicPath.toByteArray().size + payloadSize)
        var offset = 0

        // Frame type: 0x01 (request)
        frame[offset++] = 0x01
        // Request ID (4 bytes, big-endian)
        frame[offset++] = (requestId shr 24).toByte()
        frame[offset++] = (requestId shr 16).toByte()
        frame[offset++] = (requestId shr 8).toByte()
        frame[offset++] = requestId.toByte()
        // Method
        frame[offset++] = method.toByte()
        // Topic path (length-prefixed)
        val topicBytes = topicPath.toByteArray()
        frame[offset++] = (topicBytes.size shr 8).toByte()
        frame[offset++] = topicBytes.size.toByte()
        System.arraycopy(topicBytes, 0, frame, offset, topicBytes.size)
        offset += topicBytes.size
        // Payload
        if (payload != null) {
            System.arraycopy(payload, 0, frame, offset, payload.size)
        }

        return frame
    }
}

class PoseidonException(message: String) : Exception(message)