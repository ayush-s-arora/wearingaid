package com.example.wearingaid.presentation

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.core.content.edit
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.ScreenScaffold
import androidx.wear.compose.material3.Text
import androidx.wear.compose.ui.tooling.preview.WearPreviewDevices
import com.example.wearingaid.presentation.theme.WearingAidTheme
import java.util.UUID

class MainActivity : ComponentActivity() {

    private lateinit var bleServerManager: BleServerManager
    private var deviceId: String = ""

    @SuppressLint("MissingPermission")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 1. Generate or Retrieve the 4-character Device ID
        val prefs = getSharedPreferences("WearingAidPrefs", MODE_PRIVATE)
        deviceId = prefs.getString("DEVICE_ID", null) ?: run {
            val newId = UUID.randomUUID().toString().take(4).uppercase()
            prefs.edit { putString("DEVICE_ID", newId) }
            newId
        }

        bleServerManager = BleServerManager(this)

        // 2. Permission Launcher & Server Initialization
        val permissionLauncher = registerForActivityResult(
            ActivityResultContracts.RequestMultiplePermissions()
        ) { permissions ->
            val allGranted = permissions.entries.all { it.value }
            if (allGranted) {
                // Force the adapter name so the Next.js namePrefix filter catches it
                val bluetoothManager = getSystemService(BLUETOOTH_SERVICE) as BluetoothManager
                bluetoothManager.adapter?.name = "WearingAid - $deviceId"

                // Fire up the GATT Server
                bleServerManager.startServer()
            }
        }

        // 3. Request strict Android 12+ BLE permissions on boot
        val requiredPermissions = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.BLUETOOTH_ADVERTISE
            )
        } else {
            arrayOf(
                Manifest.permission.ACCESS_FINE_LOCATION
            )
        }
        permissionLauncher.launch(requiredPermissions)

        // 4. Set the UI
        setContent {
            WearingAidApp(deviceId = deviceId)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        bleServerManager.stopServer()
    }
}

// Extracted UI into a standalone Composable
@Composable
fun WearingAidApp(deviceId: String) {
    WearingAidTheme {
        AppScaffold {
            ScreenScaffold(
                modifier = Modifier.fillMaxSize()
            ) {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .background(MaterialTheme.colorScheme.background),
                    contentAlignment = Alignment.Center
                ) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(
                            text = "WearingAid",
                            color = MaterialTheme.colorScheme.onSurfaceVariant, // Matches text-gray-400
                            style = MaterialTheme.typography.titleMedium
                        )
                        Spacer(modifier = Modifier.height(8.dp))
                        Text(
                            text = deviceId,
                            color = MaterialTheme.colorScheme.onBackground, // text-white
                            style = MaterialTheme.typography.displayLarge,
                            fontWeight = FontWeight.Bold
                        )
                        Spacer(modifier = Modifier.height(8.dp))
                        Text(
                            text = "Broadcasting...",
                            color = MaterialTheme.colorScheme.tertiary, // text-green-400
                            style = MaterialTheme.typography.labelMedium
                        )
                    }
                }
            }
        }
    }
}

// Preview block
@WearPreviewDevices
@Composable
fun WearingAidAppPreview() {
    // We pass a dummy ID just for the visual layout in the editor
    WearingAidApp(deviceId = "A7X2")
}