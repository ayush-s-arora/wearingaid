package com.example.wearingaid.presentation

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.content.pm.PackageManager
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
import androidx.core.content.ContextCompat
import androidx.core.content.edit
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.Button
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.ScreenScaffold
import androidx.wear.compose.material3.Text
import androidx.wear.compose.ui.tooling.preview.WearPreviewDevices
import com.example.wearingaid.presentation.theme.WearingAidTheme
import java.util.UUID
import android.widget.Toast

class MainActivity : ComponentActivity() {

    private lateinit var bleServerManager: BleServerManager
    private var deviceId: String = ""

    // 1. Reactive State
    private var isBroadcasting by mutableStateOf(false)

    // 2. Permission Callback
    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val allGranted = permissions.entries.all { it.value }
        if (allGranted) {
            startGattServer()
        } else {
            isBroadcasting = false // Reset UI if they dismiss or deny
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val prefs = getSharedPreferences("WearingAidPrefs", MODE_PRIVATE)
        deviceId = prefs.getString("DEVICE_ID", null) ?: run {
            val newId = UUID.randomUUID().toString().take(4).uppercase()
            prefs.edit { putString("DEVICE_ID", newId) }
            newId
        }

        bleServerManager = BleServerManager(this)

        setContent {
            WearingAidApp(
                deviceId = deviceId,
                isBroadcasting = isBroadcasting,
                onToggleBroadcast = { toggleBroadcasting() }
            )
        }
    }

    private fun toggleBroadcasting() {
        if (isBroadcasting) {
            stopGattServer()
        } else {
            // Determine required permissions based on Android version
            val requiredPermissions = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                arrayOf(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_ADVERTISE)
            } else {
                arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
            }

            // Check if we already have them
            val allGranted = requiredPermissions.all {
                ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
            }

            if (allGranted) {
                startGattServer()
            } else {
                // Fire the OS prompt
                permissionLauncher.launch(requiredPermissions)
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun startGattServer() {
        val bluetoothManager = getSystemService(BLUETOOTH_SERVICE) as BluetoothManager
        bluetoothManager.adapter?.name = "WearingAid - $deviceId"
        val actualName = bluetoothManager.adapter?.name ?: "Unknown"
        bleServerManager.startServer()
        isBroadcasting = true
        Toast.makeText(this, "Broadcasting as: $actualName", Toast.LENGTH_LONG).show()
    }

    private fun stopGattServer() {
        bleServerManager.stopServer()
        isBroadcasting = false
    }

    override fun onDestroy() {
        super.onDestroy()
        stopGattServer()
    }
}

// Extracted UI
@Composable
fun WearingAidApp(
    deviceId: String,
    isBroadcasting: Boolean,
    onToggleBroadcast: () -> Unit
) {
    WearingAidTheme {
        AppScaffold {
            ScreenScaffold(modifier = Modifier.fillMaxSize()) {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .background(MaterialTheme.colorScheme.background),
                    contentAlignment = Alignment.Center
                ) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(
                            text = "WearingAid",
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            style = MaterialTheme.typography.titleMedium
                        )
                        Spacer(modifier = Modifier.height(4.dp))
                        Text(
                            text = deviceId,
                            color = MaterialTheme.colorScheme.onBackground,
                            style = MaterialTheme.typography.displayLarge,
                            fontWeight = FontWeight.Bold
                        )
                        Spacer(modifier = Modifier.height(2.dp))

                        // Dynamic Status Text
                        Text(
                            text = if (isBroadcasting) "Broadcasting" else "Idle",
                            color = if (isBroadcasting) MaterialTheme.colorScheme.tertiary else MaterialTheme.colorScheme.error,
                            style = MaterialTheme.typography.labelMedium
                        )

                        Spacer(modifier = Modifier.height(12.dp))

                        // Interaction Button
                        Button(
                            onClick = onToggleBroadcast,
                            modifier = Modifier.fillMaxWidth(0.6f)
                        ) {
                            Box(
                                modifier = Modifier.fillMaxWidth(),
                                contentAlignment = Alignment.Center
                            ) {
                                Text(text = if (isBroadcasting) "Stop" else "Start")
                            }
                        }
                    }
                }
            }
        }
    }
}

// Preview
@WearPreviewDevices
@Composable
fun WearingAidAppPreview() {
    WearingAidApp(deviceId = "A7X2", isBroadcasting = false, onToggleBroadcast = {})
}