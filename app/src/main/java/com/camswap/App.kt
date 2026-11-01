package com.camswap

import android.content.Context
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import com.camswap.ui.screens.HomeScreen
import com.camswap.ui.theme.CamswapTheme

@Composable
fun App() {
    val context = LocalContext.current
    val prefs = remember { context.getSharedPreferences("camswap", Context.MODE_PRIVATE) }
    var blackTheme by remember { mutableStateOf(prefs.getBoolean("black", true)) }
    var lang by remember {
        mutableStateOf(
            prefs.getString("lang", null) ?: "it"
        )
    }
    CamswapTheme(black = blackTheme) {
        Surface(
            modifier = Modifier.fillMaxSize(),
            color = MaterialTheme.colorScheme.background
        ) {
            HomeScreen(
                blackTheme = blackTheme,
                onBlackTheme = { enabled ->
                    blackTheme = enabled
                    prefs.edit().putBoolean("black", enabled).apply()
                },
                lang = lang,
                onLang = { next ->
                    lang = next
                    prefs.edit().putString("lang", next).apply()
                }
            )
        }
    }
}
