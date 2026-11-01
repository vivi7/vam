package com.camswap.ui.theme

import android.app.Activity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.core.view.WindowCompat

private val BlackColorScheme = darkColorScheme(
    background = Color(0xFF000000),
    surface = Color(0xFF000000),
    surfaceVariant = Color(0xFF161616),
    primary = Color(0xFFF2F2F2),
    onPrimary = Color(0xFF000000),
    secondary = Color(0xFFC8C8C8),
    onSecondary = Color(0xFF000000),
    onBackground = Color(0xFFF5F5F5),
    onSurface = Color(0xFFF5F5F5),
    onSurfaceVariant = Color(0xFFBDBDBD),
    outline = Color(0xFF3A3A3A)
)

private val LightColorScheme = lightColorScheme()

@Composable
fun CamswapTheme(black: Boolean, content: @Composable () -> Unit) {
    val scheme = if (black) BlackColorScheme else LightColorScheme
    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as Activity).window
            window.statusBarColor = scheme.background.toArgb()
            window.navigationBarColor = scheme.background.toArgb()
            WindowCompat.getInsetsController(window, view).isAppearanceLightStatusBars = !black
        }
    }
    MaterialTheme(
        colorScheme = scheme,
        content = content
    )
}
