package com.palindrome.wearingaid

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.os.SystemClock
import android.util.Log
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

        val service = BluetoothGattService(serviceUuid, BluetoothGattService.SERVICE_TYPE_PRIMARY)
        service.addCharacteristic(configCharacteristic)
        service.addCharacteristic(heartbeatCharacteristic)
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
                connectionWatchdogHandler.removeCallbacks(connectionWatchdogRunnable)
                onConnectionStateChanged(false, null)
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

    fun stopServer() {
        connectionWatchdogHandler.removeCallbacks(connectionWatchdogRunnable)
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