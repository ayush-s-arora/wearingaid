package com.palindrome.wearingaid

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.os.SystemClock
import android.util.Log
import java.util.Collections
import java.util.UUID

@SuppressLint("MissingPermission")
class BleServerManager(
    private val context: Context,
    private val onPacketReceived: (String) -> Unit,
    private val onConnectionStateChanged: (connected: Boolean, deviceName: String?) -> Unit
) {
    private val bluetoothManager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val bluetoothAdapter: BluetoothAdapter? = bluetoothManager.adapter
    private var gattServer: BluetoothGattServer? = null

    private val serviceUuid = UUID.fromString("696afb11-bed7-42cc-b178-dd49bac6c8ef")
    private val configUuid = UUID.fromString("692580ea-d39f-49f8-bb81-ae799d99de8d")
    private val heartbeatUuid = UUID.fromString("3f2b0ed6-6df7-4f1a-8d77-fc7f2f76d211")
    private val debugUuid = UUID.fromString("b2e7f3c1-9d4a-4f58-a6e0-3c8d12b54e71")
    private val featuresUuid = UUID.fromString("52b0e5e0-c1a5-4bce-b68b-8a8b41b5ca5c")
    private val cccDescriptorUuid = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    private var debugCharacteristic: BluetoothGattCharacteristic? = null
    private var featuresCharacteristic: BluetoothGattCharacteristic? = null
    private val debugSubscribers: MutableSet<BluetoothDevice> = Collections.synchronizedSet(mutableSetOf())
    private val featureSubscribers: MutableSet<BluetoothDevice> = Collections.synchronizedSet(mutableSetOf())
    private var connectedDevice: BluetoothDevice? = null
    private var lastActivityAt = 0L
    private val connectionWatchdogHandler = Handler(Looper.getMainLooper())
    private val connectionWatchdogRunnable = object : Runnable {
        override fun run() {
            val device = connectedDevice ?: return

            val now = SystemClock.elapsedRealtime()
            if (now - lastActivityAt > CONNECTION_TIMEOUT_MS) {
                Log.d("WearingAid_BLE", "Heartbeat timeout; marking peer disconnected")
                connectedDevice = null
                onConnectionStateChanged(false, null)
                gattServer?.cancelConnection(device)
                return
            }

            connectionWatchdogHandler.postDelayed(this, CONNECTION_TIMEOUT_MS)
        }
    }

    val localName: String? get() = bluetoothAdapter?.name

    fun startServer() {
        if (bluetoothAdapter == null || !bluetoothAdapter.isEnabled) return

        gattServer = bluetoothManager.openGattServer(context, gattServerCallback)

        val configCharacteristic = BluetoothGattCharacteristic(
            configUuid,
            BluetoothGattCharacteristic.PROPERTY_WRITE,
            BluetoothGattCharacteristic.PERMISSION_WRITE
        )

        val heartbeatCharacteristic = BluetoothGattCharacteristic(
            heartbeatUuid,
            BluetoothGattCharacteristic.PROPERTY_WRITE,
            BluetoothGattCharacteristic.PERMISSION_WRITE
        )

        val debugChar = BluetoothGattCharacteristic(
            debugUuid,
            BluetoothGattCharacteristic.PROPERTY_NOTIFY,
            BluetoothGattCharacteristic.PERMISSION_READ
        )
        val cccDescriptor = BluetoothGattDescriptor(
            cccDescriptorUuid,
            BluetoothGattDescriptor.PERMISSION_READ or BluetoothGattDescriptor.PERMISSION_WRITE
        )
        debugChar.addDescriptor(cccDescriptor)
        debugCharacteristic = debugChar

        val featuresChar = BluetoothGattCharacteristic(
            featuresUuid,
            BluetoothGattCharacteristic.PROPERTY_NOTIFY or BluetoothGattCharacteristic.PROPERTY_READ,
            BluetoothGattCharacteristic.PERMISSION_READ
        )
        val featuresCcc = BluetoothGattDescriptor(
            cccDescriptorUuid,
            BluetoothGattDescriptor.PERMISSION_READ or BluetoothGattDescriptor.PERMISSION_WRITE
        )
        featuresChar.addDescriptor(featuresCcc)
        featuresCharacteristic = featuresChar

        val service = BluetoothGattService(serviceUuid, BluetoothGattService.SERVICE_TYPE_PRIMARY)
        service.addCharacteristic(configCharacteristic)
        service.addCharacteristic(heartbeatCharacteristic)
        service.addCharacteristic(debugChar)
        service.addCharacteristic(featuresChar)
        gattServer?.addService(service)

        startAdvertising()
    }

    private fun startAdvertising() {
        val advertiser = bluetoothAdapter?.bluetoothLeAdvertiser ?: return

        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
            .setConnectable(true)
            .build()

        val advertiseData = AdvertiseData.Builder()
            .setIncludeDeviceName(false) // UUID alone is 18 bytes; adding the name overflows the 31-byte limit
            .addServiceUuid(ParcelUuid(serviceUuid))
            .build()

        val scanResponseData = AdvertiseData.Builder()
            .setIncludeDeviceName(true)
            .build()

        advertiser.startAdvertising(settings, advertiseData, scanResponseData, advertiseCallback)
    }

    private val advertiseCallback = object : AdvertiseCallback() {
        override fun onStartSuccess(settingsInEffect: AdvertiseSettings) {
            Log.d("WearingAid_BLE", "Advertising started successfully")
        }

        override fun onStartFailure(errorCode: Int) {
            Log.e("WearingAid_BLE", "Advertising failed: $errorCode")
        }
    }

    private val gattServerCallback = object : BluetoothGattServerCallback() {
        override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
            super.onConnectionStateChange(device, status, newState)
            val state = if (newState == BluetoothProfile.STATE_CONNECTED) "connected" else "disconnected"
            Log.d("WearingAid_BLE", "Device $state")
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                connectedDevice = device
                lastActivityAt = SystemClock.elapsedRealtime()
                connectionWatchdogHandler.removeCallbacks(connectionWatchdogRunnable)
                connectionWatchdogHandler.postDelayed(connectionWatchdogRunnable, CONNECTION_TIMEOUT_MS)
                onConnectionStateChanged(true, device.name)
            } else {
                if (connectedDevice?.address == device.address) {
                    connectedDevice = null
                }
                debugSubscribers.remove(device)
                featureSubscribers.remove(device)
                connectionWatchdogHandler.removeCallbacks(connectionWatchdogRunnable)
                onConnectionStateChanged(false, null)
            }
        }

        override fun onDescriptorWriteRequest(
            device: BluetoothDevice, requestId: Int,
            descriptor: BluetoothGattDescriptor, preparedWrite: Boolean,
            responseNeeded: Boolean, offset: Int, value: ByteArray
        ) {
            if (descriptor.uuid == cccDescriptorUuid) {
                val enable = value.contentEquals(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                val charUuid = descriptor.characteristic.uuid
                when (charUuid) {
                    debugUuid -> if (enable) debugSubscribers.add(device) else debugSubscribers.remove(device)
                    featuresUuid -> if (enable) {
                        featureSubscribers.add(device)
                        val char = featuresCharacteristic
                        val snapshot = char?.value
                        if (char != null && snapshot != null) {
                            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                                gattServer?.notifyCharacteristicChanged(device, char, false, snapshot)
                            } else {
                                @Suppress("DEPRECATION")
                                gattServer?.notifyCharacteristicChanged(device, char, false)
                            }
                        }
                    } else featureSubscribers.remove(device)
                }
                if (responseNeeded) {
                    gattServer?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, 0, null)
                }
            }
        }

        override fun onCharacteristicReadRequest(
            device: BluetoothDevice, requestId: Int, offset: Int,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (characteristic.uuid == featuresUuid) {
                @Suppress("DEPRECATION")
                val value = characteristic.value ?: "3".toByteArray(Charsets.UTF_8)
                gattServer?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, value)
            }
        }

        override fun onCharacteristicWriteRequest(
            device: BluetoothDevice,
            requestId: Int,
            characteristic: BluetoothGattCharacteristic,
            preparedWrite: Boolean,
            responseNeeded: Boolean,
            offset: Int,
            value: ByteArray
        ) {
            super.onCharacteristicWriteRequest(device, requestId, characteristic, preparedWrite, responseNeeded, offset, value)

            if (characteristic.uuid == configUuid || characteristic.uuid == heartbeatUuid) {
                try {
                    lastActivityAt = SystemClock.elapsedRealtime()

                    if (characteristic.uuid == heartbeatUuid) {
                        if (responseNeeded) {
                            gattServer?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, 0, null)
                        }
                        return
                    }

                    val payload = String(value, Charsets.UTF_8)
                    Log.d("WearingAid_BLE", "Packet received: $payload")
                    onPacketReceived(payload)
                    if (responseNeeded) {
                        gattServer?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, 0, null)
                    }
                } catch (e: Exception) {
                    Log.e("WearingAid_BLE", "Packet handling failed: ${e.message}")
                    if (responseNeeded) {
                        gattServer?.sendResponse(device, requestId, BluetoothGatt.GATT_FAILURE, 0, null)
                    }
                }
            }
        }
    }

    @SuppressLint("MissingPermission")
    fun notifyFeatureState(features: Int, genreCode: Int = -1): Boolean {
        val char = featuresCharacteristic ?: return false
        val bytes = "$features,$genreCode".toByteArray(Charsets.UTF_8)
        @Suppress("DEPRECATION")
        char.value = bytes
        val snapshot = synchronized(featureSubscribers) { featureSubscribers.toList() }
        if (snapshot.isEmpty()) return false
        snapshot.forEach { device ->
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                gattServer?.notifyCharacteristicChanged(device, char, false, bytes)
            } else {
                @Suppress("DEPRECATION")
                gattServer?.notifyCharacteristicChanged(device, char, false)
            }
        }
        return true
    }

    @SuppressLint("MissingPermission")
    fun sendDebugPacket(payload: String) {
        val char = debugCharacteristic ?: return
        val bytes = payload.toByteArray(Charsets.UTF_8).take(20).toByteArray()
        val snapshot = synchronized(debugSubscribers) { debugSubscribers.toList() }
        snapshot.forEach { device ->
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                gattServer?.notifyCharacteristicChanged(device, char, false, bytes)
            } else {
                @Suppress("DEPRECATION")
                char.value = bytes
                @Suppress("DEPRECATION")
                gattServer?.notifyCharacteristicChanged(device, char, false)
            }
        }
    }

    fun stopServer() {
        connectionWatchdogHandler.removeCallbacks(connectionWatchdogRunnable)
        debugSubscribers.clear()
        featureSubscribers.clear()
        connectedDevice?.let { device ->
            gattServer?.cancelConnection(device)
            connectedDevice = null
        }
        gattServer?.close()
        bluetoothAdapter?.bluetoothLeAdvertiser?.stopAdvertising(advertiseCallback)
    }

    private companion object {
        const val CONNECTION_TIMEOUT_MS = 10000L
    }
}