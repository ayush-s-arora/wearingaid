package com.palindrome.wearingaid.presentation

import android.Manifest
import android.annotation.SuppressLint
import android.content.pm.PackageManager
import android.graphics.Color as AndroidColor
import android.os.Build
import android.os.Bundle
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.wear.compose.foundation.lazy.ScalingLazyColumn
import androidx.wear.compose.foundation.lazy.items
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.foundation.focusable
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.input.rotary.onRotaryScrollEvent
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.core.content.edit
import androidx.lifecycle.lifecycleScope
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.Button
import androidx.wear.compose.material3.Icon
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.ScreenScaffold
import androidx.wear.compose.material3.Text
import androidx.wear.compose.ui.tooling.preview.WearPreviewDevices
import com.palindrome.wearingaid.BleServerManager
import com.palindrome.wearingaid.presentation.icons.arrow_left
import com.palindrome.wearingaid.presentation.icons.arrow_right
import com.palindrome.wearingaid.presentation.icons.arrow_upload_progress
import com.palindrome.wearingaid.presentation.icons.check
import com.palindrome.wearingaid.presentation.icons.downloading
import com.palindrome.wearingaid.presentation.icons.error
import com.palindrome.wearingaid.presentation.theme.WearingAidTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.roundToInt
import kotlin.time.Duration.Companion.milliseconds

private const val PREFS_NAME = "WearingAidPrefs"
private const val PREF_GENRE_CODE = "GENRE_CODE"
private const val PREF_COMPANION_MODE = "COMPANION_MODE"
private const val PREF_FEATURES = "FEATURES"
private const val OUTPUT_TICK_SECONDS = 0.1f

private const val FEATURE_KEY = 1
private const val FEATURE_TEMPO = 2
private const val FEATURE_LISTENING = 4
private const val DEFAULT_FEATURES = FEATURE_KEY or FEATURE_TEMPO

private data class GenreProfile(
    val code: Int,
    val title: String,
    val subtitle: String
)

private val GENRE_PROFILES = listOf(
    GenreProfile(0, "Classical", "Jazz, traditional"),
    GenreProfile(1, "Band", "Rock, pop, live"),
    GenreProfile(2, "Electronic", "EDM, house, techno"),
    GenreProfile(3, "Acoustic", "Folk, solo, traditional"),
    GenreProfile(4, "Minimal", "Ambient, drone")
)

private val COMPANION_PAYLOAD_KEYS = arrayOf(
    "C Major", "C Minor", "C# Major", "C# Minor",
    "D Major", "D Minor", "D# Major", "D# Minor",
    "E Major", "E Minor", "F Major", "F Minor",
    "F# Major", "F# Minor", "G Major", "G Minor",
    "G# Major", "G# Minor", "A Major", "A Minor",
    "A# Major", "A# Minor", "B Major", "B Minor"
)

private val ENGINE_KEYS = arrayOf(
    "C Major", "C# Major", "D Major", "D# Major", "E Major", "F Major",
    "F# Major", "G Major", "G# Major", "A Major", "A# Major", "B Major",
    "C Minor", "C# Minor", "D Minor", "D# Minor", "E Minor", "F Minor",
    "F# Minor", "G Minor", "G# Minor", "A Minor", "A# Minor", "B Minor"
)

private val DEFAULT_PALETTE = mapOf(
    "C Major" to "#FF0000", "C Minor" to "#8B0000",
    "C# Major" to "#FF4500", "C# Minor" to "#8B2500",
    "D Major" to "#FFA500", "D Minor" to "#8B5A00",
    "D# Major" to "#FFD700", "D# Minor" to "#8B7500",
    "E Major" to "#FFFF00", "E Minor" to "#8B8B00",
    "F Major" to "#00FF00", "F Minor" to "#006400",
    "F# Major" to "#00FFFF", "F# Minor" to "#008B8B",
    "G Major" to "#0000FF", "G Minor" to "#00008B",
    "G# Major" to "#4B0082", "G# Minor" to "#2E0854",
    "A Major" to "#8A2BE2", "A Minor" to "#551A8B",
    "A# Major" to "#FF1493", "A# Minor" to "#8B0A50",
    "B Major" to "#FF00FF", "B Minor" to "#8B008B"
)

enum class SyncState {
    STANDALONE, SYNCING, UPLOADING, SYNCED, ERROR
}

class MainActivity : ComponentActivity() {

    private lateinit var bleServerManager: BleServerManager
    private val audioViewModel: AudioViewModel by viewModels()

    private var localDeviceName: String? = null
    private var connectedDeviceName by mutableStateOf<String?>(null)
    private var isPeerConnected by mutableStateOf(false)
    private var isBroadcasting by mutableStateOf(false)
    private var isRecording by mutableStateOf(false)
    private var debugMode by mutableStateOf(false)
    private var featuresEnabled by mutableIntStateOf(DEFAULT_FEATURES)
    private var syncState by mutableStateOf(SyncState.STANDALONE)
    private var companionModeEnabled by mutableStateOf(false)
    private var selectedGenre by mutableStateOf<GenreProfile?>(null)
    private var activeKeyIndex by mutableIntStateOf(-1)
    private var estimatedBpm by mutableFloatStateOf(120f)
    private var vibrationSensitivity by mutableIntStateOf(5)
    private val palette = mutableStateMapOf<String, String>().apply { putAll(DEFAULT_PALETTE) }

    private val vibrator: Vibrator by lazy {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val manager = getSystemService(VibratorManager::class.java)
            manager.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            getSystemService(VIBRATOR_SERVICE) as Vibrator
        }
    }

    private val blePermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val allGranted = permissions.entries.all { it.value }
        if (allGranted && companionModeEnabled) {
            startGattServer()
        } else {
            isBroadcasting = false
            syncState = SyncState.ERROR
        }
    }

    private val audioPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { isGranted ->
        if (isGranted) {
            startRecording()
        } else {
            isRecording = false
            activeKeyIndex = -1
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        selectedGenre = prefs.getInt(PREF_GENRE_CODE, -1)
            .takeIf { it in 0..4 }
            ?.let { code -> GENRE_PROFILES.first { it.code == code } }
        companionModeEnabled = prefs.getBoolean(PREF_COMPANION_MODE, false)
        featuresEnabled = prefs.getInt(PREF_FEATURES, DEFAULT_FEATURES)
        selectedGenre?.let { audioViewModel.setGenre(it.code) }
        audioViewModel.setFeatures(featuresEnabled)

        bleServerManager = BleServerManager(
            this,
            onPacketReceived = { rawPayload ->
                lifecycleScope.launch {
                    syncState = SyncState.SYNCING
                    delay(150.milliseconds)
                    val parsed = parseCompanionPayload(rawPayload)
                    syncState = when {
                        parsed && isPeerConnected -> SyncState.SYNCED
                        parsed -> SyncState.STANDALONE
                        else -> SyncState.ERROR
                    }
                }
            },
            onConnectionStateChanged = { connected, name ->
                runOnUiThread {
                    isPeerConnected = connected
                    connectedDeviceName = if (connected) name else null
                    syncState = if (connected) SyncState.SYNCED else SyncState.STANDALONE
                    if (connected) bleServerManager.notifyFeatureState(featuresEnabled or (if (isRecording) FEATURE_LISTENING else 0), selectedGenre?.code ?: -1)
                }
            }
        )

        setContent {
            WearingAidApp(
                selectedGenre = selectedGenre,
                companionModeEnabled = companionModeEnabled,
                isBroadcasting = isBroadcasting,
                isPeerConnected = isPeerConnected,
                localDeviceName = localDeviceName,
                connectedDeviceName = connectedDeviceName,
                isRecording = isRecording,
                debugMode = debugMode,
                syncState = syncState,
                activeKeyIndex = { activeKeyIndex },
                activeKeyColor = { colorForKey(activeKeyIndex) },
                estimatedBpm = { estimatedBpm },
                onGenreSelected = { genre -> selectGenre(genre) },
                featuresEnabled = featuresEnabled,
                onToggleListening = { toggleRecording() },
                onToggleCompanionMode = { enabled -> setCompanionMode(enabled) },
                onToggleDebugMode = { debugMode = !debugMode },
                onToggleFeature = { bit -> toggleFeature(bit) },
                onAudioTick = { readAudioOutput() },
                onDebugTick = { sendDebugPacket() }
            )
        }

        if (companionModeEnabled) {
            ensureBroadcasting()
        }
    }

    private fun selectGenre(genre: GenreProfile) {
        selectedGenre = genre
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit { putInt(PREF_GENRE_CODE, genre.code) }
        audioViewModel.setGenre(genre.code)
        if (isPeerConnected) {
            syncState = SyncState.UPLOADING
            lifecycleScope.launch {
                val sent = bleServerManager.notifyFeatureState(featuresEnabled or (if (isRecording) FEATURE_LISTENING else 0), genre.code)
                delay(150.milliseconds)
                syncState = if (sent) SyncState.SYNCED else SyncState.ERROR
            }
        }
    }

    private fun applyGenreFromCompanion(genre: GenreProfile) {
        selectedGenre = genre
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit { putInt(PREF_GENRE_CODE, genre.code) }
        audioViewModel.setGenre(genre.code)
    }

    private fun setCompanionMode(enabled: Boolean) {
        companionModeEnabled = enabled
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit { putBoolean(PREF_COMPANION_MODE, enabled) }
        if (enabled) {
            ensureBroadcasting()
        } else {
            stopGattServer()
        }
    }

    private fun ensureRecording() {
        if (selectedGenre == null || isRecording) return
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
            startRecording()
        } else {
            audioPermissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
        }
    }

    private fun toggleRecording() {
        if (isRecording) {
            audioViewModel.stopRecording()
            isRecording = false
            activeKeyIndex = -1
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            notifyListeningState()
        } else {
            ensureRecording()
        }
    }

    private fun startRecording() {
        audioViewModel.startRecording()
        isRecording = true
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        notifyListeningState()
    }

    private fun notifyListeningState() {
        if (!isPeerConnected) return
        syncState = SyncState.UPLOADING
        lifecycleScope.launch {
            val sent = bleServerManager.notifyFeatureState(
                featuresEnabled or (if (isRecording) FEATURE_LISTENING else 0),
                selectedGenre?.code ?: -1
            )
            delay(150.milliseconds)
            syncState = if (sent) SyncState.SYNCED else SyncState.ERROR
        }
    }

    private fun ensureBroadcasting() {
        if (isBroadcasting) return
        val requiredPermissions = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_ADVERTISE)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

        val allGranted = requiredPermissions.all {
            ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
        }

        if (allGranted) {
            startGattServer()
        } else {
            blePermissionLauncher.launch(requiredPermissions)
        }
    }

    @SuppressLint("MissingPermission")
    private fun startGattServer() {
        bleServerManager.startServer()
        localDeviceName = bleServerManager.localName
        isBroadcasting = true
    }

    private fun stopGattServer() {
        isPeerConnected = false
        connectedDeviceName = null
        isBroadcasting = false
        syncState = SyncState.STANDALONE
        bleServerManager.stopServer()
    }

    private suspend fun readAudioOutput() {
        if (!isRecording) return
        val output = withContext(Dispatchers.Default) {
            audioViewModel.readOutput(OUTPUT_TICK_SECONDS)
        }
        activeKeyIndex = if (output.keyIndex in 0..ENGINE_KEYS.lastIndex) output.keyIndex else -1
        estimatedBpm = output.bpm
        if (output.shouldVibrate) {
            vibrateForBeat()
        }
    }

    private fun toggleFeature(featureBit: Int) {
        val newFeatures = featuresEnabled xor featureBit
        featuresEnabled = newFeatures
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit { putInt(PREF_FEATURES, newFeatures) }
        audioViewModel.setFeatures(newFeatures)
        if (isPeerConnected) {
            syncState = SyncState.UPLOADING
            lifecycleScope.launch {
                val sent = bleServerManager.notifyFeatureState(newFeatures or (if (isRecording) FEATURE_LISTENING else 0), selectedGenre?.code ?: -1)
                delay(150.milliseconds)
                syncState = if (sent) SyncState.SYNCED else SyncState.ERROR
            }
        }
    }

    private fun applyFeaturesFromCompanion(newFeatures: Int) {
        featuresEnabled = newFeatures
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit { putInt(PREF_FEATURES, newFeatures) }
        audioViewModel.setFeatures(newFeatures)
        // Don't notify companion back; it sent these values, would cause a loop
    }

    private fun sendDebugPacket() {
        if (!isRecording || !debugMode || !isPeerConnected) return
        bleServerManager.sendDebugPacket(audioViewModel.getDebugPacket(activeKeyIndex, estimatedBpm))
    }

    private fun parseCompanionPayload(rawPayload: String): Boolean {
        return try {
            val parts = rawPayload.split(",")
            // Format: features,sensitivity,genre,color0,...,color23
            val rawFeatures = parts.getOrNull(0)?.toIntOrNull()
            val newFeatures = rawFeatures?.and(0x3)
            val listenRequested = rawFeatures?.and(FEATURE_LISTENING)?.let { it != 0 }
            vibrationSensitivity = parts.getOrNull(1)?.toInt()?.coerceIn(1, 10) ?: vibrationSensitivity
            val newGenreCode = parts.getOrNull(2)?.toIntOrNull()?.takeIf { it in 0..4 }
            for (i in COMPANION_PAYLOAD_KEYS.indices) {
                val hex = parts.getOrNull(i + 3)
                if (!hex.isNullOrBlank()) palette[COMPANION_PAYLOAD_KEYS[i]] = hex
            }
            if (newFeatures != null) applyFeaturesFromCompanion(newFeatures)
            if (listenRequested != null && listenRequested != isRecording) toggleRecording()
            if (newGenreCode != null) {
                val genre = GENRE_PROFILES.firstOrNull { it.code == newGenreCode }
                if (genre != null) applyGenreFromCompanion(genre)
            }
            true
        } catch (_: Exception) {
            false
        }
    }

    private fun colorForKey(keyIndex: Int): String {
        val keyName = ENGINE_KEYS.getOrNull(keyIndex) ?: return "#000000"
        return palette[keyName] ?: DEFAULT_PALETTE[keyName] ?: "#000000"
    }

    private fun vibrateForBeat() {
        val durationMs = (12L + vibrationSensitivity * 4L).coerceIn(16L, 52L)
        val amplitude = (40 + vibrationSensitivity * 20).coerceIn(1, 255)
        val effect = VibrationEffect.createOneShot(durationMs, amplitude)
        vibrator.vibrate(effect)
    }

    override fun onResume() {
        super.onResume()
        // Oboe stream is killed when WearOS suspends the activity; restart it if we
        // were recording so the UI doesn't lie about being active.
        if (isRecording) audioViewModel.restartRecording()
    }

    override fun onDestroy() {
        super.onDestroy()
        stopGattServer()
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun WearingAidApp(
    selectedGenre: GenreProfile?,
    companionModeEnabled: Boolean,
    isBroadcasting: Boolean,
    isPeerConnected: Boolean,
    localDeviceName: String?,
    connectedDeviceName: String?,
    isRecording: Boolean,
    debugMode: Boolean,
    featuresEnabled: Int,
    syncState: SyncState,
    activeKeyIndex: () -> Int,
    activeKeyColor: () -> String,
    estimatedBpm: () -> Float,
    onGenreSelected: (GenreProfile) -> Unit,
    onToggleListening: () -> Unit,
    onToggleCompanionMode: (Boolean) -> Unit,
    onToggleDebugMode: () -> Unit,
    onToggleFeature: (Int) -> Unit,
    onAudioTick: suspend () -> Unit,
    onDebugTick: () -> Unit
) {
    WearingAidTheme {
        AppScaffold {
            ScreenScaffold(modifier = Modifier.fillMaxSize()) {
                if (selectedGenre == null) {
                    GenreSelectionScreen(
                        selectedGenre = null,
                        syncState = syncState,
                        onGenreSelected = onGenreSelected
                    )
                } else {
                    val pagerState = rememberPagerState(pageCount = { 4 })
                    val pagerScope = rememberCoroutineScope()
                    val pagerFocusRequester = remember { FocusRequester() }

                    LaunchedEffect(isRecording) {
                        while (isRecording) {
                            delay(100.milliseconds)
                            onAudioTick()
                        }
                    }
                    LaunchedEffect(isRecording, debugMode, isPeerConnected) {
                        while (isRecording && debugMode && isPeerConnected) {
                            delay(500.milliseconds)
                            onDebugTick()
                        }
                    }
                    LaunchedEffect(pagerState.currentPage) {
                        pagerFocusRequester.requestFocus()
                    }

                    Box(
                        modifier = Modifier
                            .fillMaxSize()
                            .focusRequester(pagerFocusRequester)
                            .onRotaryScrollEvent { event ->
                                pagerScope.launch {
                                    val next = if (event.verticalScrollPixels > 0)
                                        (pagerState.currentPage + 1).coerceAtMost(3)
                                    else
                                        (pagerState.currentPage - 1).coerceAtLeast(0)
                                    pagerState.animateScrollToPage(next)
                                }
                                true
                            }
                            .focusable()
                    ) {
                        HorizontalPager(
                            state = pagerState,
                            modifier = Modifier.fillMaxSize(),
                            beyondViewportPageCount = 3
                        ) { page ->
                            when (page) {
                                0 -> ListeningScreen(
                                    selectedGenre = selectedGenre,
                                    isRecording = isRecording,
                                    activeKeyIndex = activeKeyIndex,
                                    activeKeyColor = activeKeyColor,
                                    estimatedBpm = estimatedBpm,
                                    syncState = syncState,
                                    onToggleListening = onToggleListening
                                )
                                1 -> GenreSelectionScreen(
                                    selectedGenre = selectedGenre,
                                    syncState = syncState,
                                    onGenreSelected = onGenreSelected
                                )
                                2 -> FeaturesScreen(
                                    featuresEnabled = featuresEnabled,
                                    syncState = syncState,
                                    onToggleFeature = onToggleFeature
                                )
                                3 -> SettingsScreen(
                                    companionModeEnabled = companionModeEnabled,
                                    isBroadcasting = isBroadcasting,
                                    isPeerConnected = isPeerConnected,
                                    localDeviceName = localDeviceName,
                                    connectedDeviceName = connectedDeviceName,
                                    syncState = syncState,
                                    debugMode = debugMode,
                                    onToggleCompanionMode = onToggleCompanionMode,
                                    onToggleDebugMode = onToggleDebugMode
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun ListeningScreen(
    selectedGenre: GenreProfile,
    isRecording: Boolean,
    activeKeyIndex: () -> Int,
    activeKeyColor: () -> String,
    estimatedBpm: () -> Float,
    syncState: SyncState,
    onToggleListening: () -> Unit
) {
    val currentKeyIndex = activeKeyIndex()
    val hasDetectedKey = isRecording && currentKeyIndex in ENGINE_KEYS.indices
    val backgroundColor = if (hasDetectedKey) parseHexColor(activeKeyColor()) else MaterialTheme.colorScheme.background
    val foregroundColor = if (hasDetectedKey && backgroundColor.luminanceEstimate() > 0.55f) Color.Black else MaterialTheme.colorScheme.onBackground

    Box(modifier = Modifier.fillMaxSize().background(backgroundColor)) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontal = 16.dp)
                .padding(top = 4.dp, bottom = 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            SyncStatusIcon(syncState = syncState)
            Box(modifier = Modifier.weight(1f), contentAlignment = Alignment.Center) {
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.Center,
                    modifier = Modifier.fillMaxWidth()
                ) {
                    if (hasDetectedKey) {
                        Text(
                            text = ENGINE_KEYS[currentKeyIndex],
                            color = foregroundColor,
                            style = MaterialTheme.typography.displayMedium,
                            fontWeight = FontWeight.Bold,
                            textAlign = TextAlign.Center
                        )
                        Text(
                            text = "${estimatedBpm().roundToInt()} BPM",
                            color = foregroundColor.copy(alpha = 0.78f),
                            style = MaterialTheme.typography.labelMedium
                        )
                    } else {
                        Text(
                            text = if (isRecording) "Listening" else "Paused",
                            color = foregroundColor,
                            style = MaterialTheme.typography.displayMedium,
                            fontWeight = FontWeight.Bold
                        )
                        Text(
                            text = selectedGenre.title,
                            color = foregroundColor.copy(alpha = 0.78f),
                            style = MaterialTheme.typography.labelMedium
                        )
                    }
                    Spacer(modifier = Modifier.height(12.dp))
                    Button(
                        onClick = onToggleListening,
                        modifier = Modifier.fillMaxWidth(0.58f)
                    ) {
                        Box(modifier = Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                            Text(text = if (isRecording) "Pause" else "Resume")
                        }
                    }
                }
            }
        }
        Icon(
            imageVector = arrow_right,
            contentDescription = null,
            tint = foregroundColor.copy(alpha = 0.65f),
            modifier = Modifier.align(Alignment.CenterEnd).size(18.dp)
        )
    }
}

@Composable
private fun GenreSelectionScreen(
    selectedGenre: GenreProfile?,
    syncState: SyncState,
    onGenreSelected: (GenreProfile) -> Unit
) {
    val iconTint = MaterialTheme.colorScheme.onBackground.copy(alpha = 0.65f)
    Box(modifier = Modifier.fillMaxSize()) {
        ScalingLazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .background(MaterialTheme.colorScheme.background),
            contentPadding = PaddingValues(start = 10.dp, top = 4.dp, end = 10.dp, bottom = 16.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
            autoCentering = null,
        ) {
            item {
                Box(
                    modifier = Modifier.fillMaxWidth(),
                    contentAlignment = Alignment.Center
                ) { SyncStatusIcon(syncState = syncState) }
            }
            item {
                Text(
                    text = "Genre",
                    color = MaterialTheme.colorScheme.onBackground,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold
                )
            }
            items(GENRE_PROFILES) { genre ->
                val isSelected = selectedGenre?.code == genre.code
                Button(
                    onClick = { onGenreSelected(genre) },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text(
                                text = genre.title,
                                style = MaterialTheme.typography.labelMedium,
                                fontWeight = if (isSelected) FontWeight.Bold else FontWeight.Normal
                            )
                            Text(
                                text = genre.subtitle,
                                style = MaterialTheme.typography.labelSmall
                            )
                        }
                        if (isSelected) {
                            Icon(
                                imageVector = check,
                                contentDescription = null,
                                modifier = Modifier.size(14.dp)
                            )
                        }
                    }
                }
            }
        }
        Icon(imageVector = arrow_left, contentDescription = null, tint = iconTint,
            modifier = Modifier.align(Alignment.CenterStart).size(18.dp))
        Icon(imageVector = arrow_right, contentDescription = null, tint = iconTint,
            modifier = Modifier.align(Alignment.CenterEnd).size(18.dp))
    }
}

@Composable
private fun FeaturesScreen(
    featuresEnabled: Int,
    syncState: SyncState,
    onToggleFeature: (Int) -> Unit
) {
    val keyOn = featuresEnabled and FEATURE_KEY != 0
    val tempoOn = featuresEnabled and FEATURE_TEMPO != 0
    val iconTint = MaterialTheme.colorScheme.onBackground.copy(alpha = 0.65f)

    Box(modifier = Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontal = 16.dp)
                .padding(top = 4.dp, bottom = 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            SyncStatusIcon(syncState = syncState)
            Box(modifier = Modifier.weight(1f), contentAlignment = Alignment.Center) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(
                        text = "Features",
                        color = MaterialTheme.colorScheme.onBackground,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold
                    )
                    Spacer(modifier = Modifier.height(10.dp))
                    Button(
                        onClick = { onToggleFeature(FEATURE_KEY) },
                        modifier = Modifier.fillMaxWidth(0.82f)
                    ) {
                        Box(modifier = Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                            Text(text = if (keyOn) "Key Detection: On" else "Key Detection: Off")
                        }
                    }
                    Spacer(modifier = Modifier.height(6.dp))
                    Button(
                        onClick = { onToggleFeature(FEATURE_TEMPO) },
                        modifier = Modifier.fillMaxWidth(0.82f)
                    ) {
                        Box(modifier = Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                            Text(text = if (tempoOn) "Tempo Tracking: On" else "Tempo Tracking: Off")
                        }
                    }
                }
            }
        }
        Icon(imageVector = arrow_left, contentDescription = null, tint = iconTint,
            modifier = Modifier.align(Alignment.CenterStart).size(18.dp))
        Icon(imageVector = arrow_right, contentDescription = null, tint = iconTint,
            modifier = Modifier.align(Alignment.CenterEnd).size(18.dp))
    }
}

@Composable
private fun SettingsScreen(
    companionModeEnabled: Boolean,
    isBroadcasting: Boolean,
    isPeerConnected: Boolean,
    localDeviceName: String?,
    connectedDeviceName: String?,
    syncState: SyncState,
    debugMode: Boolean,
    onToggleCompanionMode: (Boolean) -> Unit,
    onToggleDebugMode: () -> Unit
) {
    val settingsIconTint = MaterialTheme.colorScheme.onBackground.copy(alpha = 0.65f)
    Box(modifier = Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontal = 16.dp)
                .padding(top = 4.dp, bottom = 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            SyncStatusIcon(syncState = syncState)
            Box(modifier = Modifier.weight(1f), contentAlignment = Alignment.Center) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(
                        text = "Settings",
                        color = MaterialTheme.colorScheme.onBackground,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold
                    )
                    Spacer(modifier = Modifier.height(10.dp))
                    Button(
                        onClick = { onToggleCompanionMode(!companionModeEnabled) },
                        modifier = Modifier.fillMaxWidth(0.72f)
                    ) {
                        Box(modifier = Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                            Text(text = if (companionModeEnabled) "Companion On" else "Companion Off")
                        }
                    }
                    if (companionModeEnabled) {
                        Spacer(modifier = Modifier.height(10.dp))
                        Text(
                            text = when {
                                isPeerConnected && !connectedDeviceName.isNullOrBlank() -> connectedDeviceName
                                isPeerConnected -> "Connected"
                                isBroadcasting && !localDeviceName.isNullOrBlank() -> localDeviceName
                                isBroadcasting -> "Broadcasting"
                                else -> "Preparing"
                            },
                            color = if (isPeerConnected || isBroadcasting) MaterialTheme.colorScheme.tertiary else MaterialTheme.colorScheme.error,
                            style = MaterialTheme.typography.titleSmall,
                            fontWeight = FontWeight.Bold,
                            textAlign = TextAlign.Center
                        )
                        if (isPeerConnected) {
                            Spacer(modifier = Modifier.height(6.dp))
                            Button(
                                onClick = onToggleDebugMode,
                                modifier = Modifier.fillMaxWidth(0.72f)
                            ) {
                                Box(modifier = Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                                    Text(text = if (debugMode) "Debug On" else "Debug Off")
                                }
                            }
                        }
                    }
                }
            }
        }
        Icon(imageVector = arrow_left, contentDescription = null, tint = settingsIconTint,
            modifier = Modifier.align(Alignment.CenterStart).size(18.dp))
    }
}

@Composable
private fun SyncStatusIcon(syncState: SyncState) {
    Box(
        modifier = Modifier
            .height(20.dp)
            .fillMaxWidth(),
        contentAlignment = Alignment.Center
    ) {
        when (syncState) {
            SyncState.STANDALONE -> Spacer(modifier = Modifier.size(18.dp))
            SyncState.SYNCING -> Icon(
                imageVector = downloading,
                contentDescription = "Syncing",
                tint = MaterialTheme.colorScheme.secondary,
                modifier = Modifier.size(18.dp)
            )
            SyncState.UPLOADING -> Icon(
                imageVector = arrow_upload_progress,
                contentDescription = "Uploading",
                tint = MaterialTheme.colorScheme.secondary,
                modifier = Modifier.size(18.dp)
            )
            SyncState.SYNCED -> Icon(
                imageVector = check,
                contentDescription = "Synced",
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.size(18.dp)
            )
            SyncState.ERROR -> Icon(
                imageVector = error,
                contentDescription = "Sync Error",
                tint = MaterialTheme.colorScheme.error,
                modifier = Modifier.size(18.dp)
            )
        }
    }
}

private fun parseHexColor(hex: String): Color {
    return try {
        Color(AndroidColor.parseColor(hex))
    } catch (_: Exception) {
        Color.Black
    }
}

private fun Color.luminanceEstimate(): Float {
    return 0.299f * red + 0.587f * green + 0.114f * blue
}

@WearPreviewDevices
@Composable
private fun WearingAidAppPreview() {
    WearingAidApp(
        selectedGenre = GENRE_PROFILES[1],
        companionModeEnabled = true,
        isBroadcasting = true,
        isPeerConnected = false,
        localDeviceName = "Galaxy Watch 6",
        connectedDeviceName = null,
        isRecording = true,
        debugMode = false,
        featuresEnabled = DEFAULT_FEATURES,
        syncState = SyncState.STANDALONE,
        activeKeyIndex = { -1 },
        activeKeyColor = { "#000000" },
        estimatedBpm = { 120f },
        onGenreSelected = {},
        onToggleListening = {},
        onToggleCompanionMode = {},
        onToggleDebugMode = {},
        onToggleFeature = {},
        onAudioTick = {},
        onDebugTick = {}
    )
}
