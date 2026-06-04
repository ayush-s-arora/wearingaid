package com.example.wearingaid.presentation.theme

import androidx.compose.runtime.Composable
import androidx.wear.compose.material3.ColorScheme
import androidx.wear.compose.material3.MaterialTheme

private val WearingAidColorScheme = ColorScheme(
    background = CompanionBackground,
    onBackground = CompanionText,
    surfaceContainer = CompanionSurface,
    onSurface = CompanionText,
    onSurfaceVariant = CompanionTextSecondary,
    primary = CompanionPrimaryBlue,
    onPrimary = CompanionText,
    tertiary = CompanionAccentGreen,
    onTertiary = CompanionBackground
)

@Composable
fun WearingAidTheme(
    content: @Composable () -> Unit
) {
    MaterialTheme(
        colorScheme = WearingAidColorScheme,
        typography = androidx.wear.compose.material3.Typography(),
        content = content
    )
}