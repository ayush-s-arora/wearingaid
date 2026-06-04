package com.example.wearingaid.presentation

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.content.Context
import android.os.ParcelUuid
import android.util.Log
import java.util.UUID

@SuppressLint("MissingPermission") // Checked in MainActivity before calling
class BleServerManager(private val context: Context) {

    private val bluetoothManager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val bluetoothAdapter = bluetoothManager.adapter
    private var gattServer: BluetoothGattServer? = null

    private val SERVICE_UUID = UUID.fromString("696afb11-bed7-42cc-b178-dd49bac6c8ef")
    private val CONFIG_UUID = UUID.fromString("692580ea-d39f-49f8-bb81-ae799d99de8d")

    fun startServer() {
        if (bluetoothAdapter == null || !bluetoothAdapter.isEnabled) return

        gattServer = bluetoothManager.openGattServer(context, gattServerCallback)

        val service = BluetoothGattService(SERVICE_UUID, BluetoothGattService.SERVICE_TYPE_PRIMARY)

        // This line enforces the AES pairing protocol
        val configCharacteristic = BluetoothGattCharacteristic(
            CONFIG_UUID,
            BluetoothGattCharacteristic.PROPERTY_WRITE,
            BluetoothGattCharacteristic.PERMISSION_WRITE_ENCRYPTED
        )

        service.addCharacteristic(configCharacteristic)
        gattServer?.addService(service)

        startAdvertising()
    }

    private fun startAdvertising() {
        val advertiser = bluetoothAdapter.bluetoothLeAdvertiser ?: return

        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
            .setConnectable(true)
            .build()

        // Adjust for 31-byte BLE limit

        // Packet 1: UUID (18 bytes)
        val advertiseData = AdvertiseData.Builder()
            .setIncludeDeviceName(false) // Disabled to save space
            .addServiceUuid(ParcelUuid(SERVICE_UUID))
            .build()

        // Packet 2: Device Name (19 bytes)
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
            Log.e("WearingAid_BLE", "Advertising failed with error code: $errorCode")
        }
    }

    private val gattServerCallback = object : BluetoothGattServerCallback() {
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

            if (characteristic.uuid == CONFIG_UUID) {
                val jsonPayload = String(value)
                Log.d("WearingAid_BLE", "Packet received: $jsonPayload")

                // Acknowledge the write so Next.js promise resolves
                if (responseNeeded) {
                    gattServer?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, 0, null)
                }
            }
        }
    }

    fun stopServer() {
        gattServer?.close()
        bluetoothAdapter?.bluetoothLeAdvertiser?.stopAdvertising(advertiseCallback)
    }
}