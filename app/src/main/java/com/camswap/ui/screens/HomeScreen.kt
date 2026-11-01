// HomeScreen.kt - Home screen for camswap app

package com.camswap.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.navigation.NavController

@Composable
fun HomeScreen(navController: NavController) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Camswap") }
            )
        }
    ) { padding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding),
            contentAlignment = Alignment.Center
        ) {
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(16.dp)
            ) {
                Text("Camswap Control", style = MaterialTheme.typography.headlineMedium)

                Button(onClick = { /* TODO */ }) {
                    Text("Toggle Camswap")
                }

                Button(onClick = { /* TODO */ }) {
                    Text("Manage Identities")
                }

                Button(onClick = { /* TODO */ }) {
                    Text("Settings")
                }
            }
        }
    }
}
