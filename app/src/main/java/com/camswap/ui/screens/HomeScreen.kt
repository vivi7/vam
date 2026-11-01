package com.camswap.ui.screens

import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.camswap.R
import com.camswap.control.DeviceConfig
import com.camswap.control.ModuleControl
import com.camswap.control.ModuleStatus
import com.camswap.preview.DraggableCameraPreview
import com.camswap.preview.PreviewEffect
import com.camswap.ui.uiText
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.io.File

@Composable
fun HomeScreen(
    blackTheme: Boolean,
    onBlackTheme: (Boolean) -> Unit,
    lang: String,
    onLang: (String) -> Unit
) {
    val context = LocalContext.current
    val text = uiText(lang)
    val control = remember(context) { ModuleControl(File(context.cacheDir, "su")) }
    control.lang = lang
    val scope = rememberCoroutineScope()
    var status by remember { mutableStateOf<ModuleStatus?>(null) }
    var config by remember { mutableStateOf(DeviceConfig()) }
    var rawEnv by remember { mutableStateOf("") }
    var loaded by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var message by remember { mutableStateOf<String?>(null) }
    var showPreview by remember { mutableStateOf(false) }
    var identityBitmap by remember { mutableStateOf<Bitmap?>(null) }

    LaunchedEffect(showPreview, config.identity) {
        if (!showPreview || config.identity.isBlank()) {
            identityBitmap = null
            return@LaunchedEffect
        }
        val dest = File(context.cacheDir, "preview-identity")
        val error = control.copyIdentityTo(config.identity, dest)
        identityBitmap = if (error == null) BitmapFactory.decodeFile(dest.absolutePath) else null
        if (error != null) message = error
    }

    suspend fun reload(fillForm: Boolean) {
        val next = control.status()
        status = next
        if (fillForm && next.error == null) {
            config = next.config
            rawEnv = next.rawEnv
            loaded = true
        }
    }

    LaunchedEffect(Unit) {
        reload(fillForm = true)
        while (true) {
            delay(2000)
            if (!busy) reload(fillForm = !loaded)
        }
    }

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            busy = true
            message = null
            val name = context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                ?.use { cursor ->
                    if (cursor.moveToFirst()) cursor.getString(0) else null
                } ?: "identity.bin"
            val cache = File(context.cacheDir, "identity-import")
            val copied = runCatching {
                context.contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input)
                    cache.outputStream().use { output -> input.copyTo(output) }
                }
            }
            message = if (copied.isFailure) {
                copied.exceptionOrNull()?.message ?: text.readFail
            } else {
                control.importIdentity(cache, name)
            }
            busy = false
            reload(fillForm = false)
        }
    }

    Box(Modifier.fillMaxSize()) {
    Scaffold { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text("Camswap", style = MaterialTheme.typography.headlineMedium)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    TextButton(onClick = { onLang("it") }) {
                        Text(if (lang == "it") "IT" else "it")
                    }
                    TextButton(onClick = { onLang("en") }) {
                        Text(if (lang == "en") "EN" else "en")
                    }
                    IconButton(onClick = { showPreview = !showPreview }) {
                        Icon(
                            painter = painterResource(R.drawable.ic_preview),
                            contentDescription = text.preview,
                            tint = if (showPreview) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface
                        )
                    }
                    Text(text.black, style = MaterialTheme.typography.labelLarge)
                    Switch(checked = blackTheme, onCheckedChange = onBlackTheme)
                }
            }
            Text(
                text.subtitle,
                style = MaterialTheme.typography.bodyMedium
            )
            if (busy) LinearProgressIndicator(modifier = Modifier.fillMaxWidth())

            val current = status
            if (current == null) {
                Text(text.reading)
            } else if (current.error != null && !current.root) {
                Text(current.error, color = MaterialTheme.colorScheme.error)
                Text(text.rootHint)
            } else {
                Text(
                    if (current.running) text.daemonOn.format(current.pid) else text.daemonOff,
                    style = MaterialTheme.typography.titleMedium
                )
                if (current.modulePath.isBlank()) {
                    Text(text.moduleMissing)
                } else {
                    Text(current.modulePath, style = MaterialTheme.typography.bodySmall)
                }

                ToggleRow(text.active, current.running, enabled = !busy && current.modulePath.isNotBlank()) { on ->
                    scope.launch {
                        busy = true
                        message = control.setRunning(config, rawEnv, start = on)
                        busy = false
                        reload(fillForm = false)
                    }
                }
                ToggleRow(text.boot, current.bootEnabled, enabled = !busy) { on ->
                    scope.launch {
                        busy = true
                        message = control.setBootEnabled(on)
                        busy = false
                        reload(fillForm = false)
                    }
                }

                Text(text.settings, style = MaterialTheme.typography.titleMedium)
                ChipRow(text.lens, listOf("all", "front", "back"), config.lensPolicy, mapOf("all" to text.lensAll, "front" to text.lensFront, "back" to text.lensBack)) {
                    config = config.copy(lensPolicy = it)
                }
                ChipRow(text.backend, listOf("gpu_gl", "cpu"), config.accelBackend, mapOf("gpu_gl" to "GPU", "cpu" to "CPU")) {
                    config = config.copy(accelBackend = it)
                }
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    OutlinedTextField(
                        value = config.fpsTarget,
                        onValueChange = { config = config.copy(fpsTarget = it.filter(Char::isDigit)) },
                        label = { Text(text.fpsTarget) },
                        modifier = Modifier.weight(1f),
                        singleLine = true
                    )
                    OutlinedTextField(
                        value = config.fpsFloor,
                        onValueChange = { config = config.copy(fpsFloor = it.filter(Char::isDigit)) },
                        label = { Text(text.fpsFloor) },
                        modifier = Modifier.weight(1f),
                        singleLine = true
                    )
                }
                OutlinedTextField(
                    value = config.rssBudgetMb,
                    onValueChange = { config = config.copy(rssBudgetMb = it.filter(Char::isDigit)) },
                    label = { Text(text.ram) },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true
                )
                OutlinedTextField(
                    value = config.thermalLadder,
                    onValueChange = { config = config.copy(thermalLadder = it.trim()) },
                    label = { Text(text.thermal) },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true
                )
                Text(text.blur, style = MaterialTheme.typography.titleMedium)
                ToggleRow(text.blurFace, config.blurEnabled, enabled = !busy) { on ->
                    config = config.copy(blurEnabled = on)
                }
                ToggleRow(text.trackFace, config.blurTrack, enabled = !busy) { on ->
                    config = config.copy(blurTrack = on)
                }
                Text(text.level.format(config.blurLevel))
                Slider(
                    value = (config.blurLevel.toFloatOrNull() ?: 40f).coerceIn(0f, 100f),
                    onValueChange = { config = config.copy(blurLevel = it.toInt().toString()) },
                    valueRange = 0f..100f,
                    enabled = config.blurEnabled
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = {
                        config = config.copy(fpsTarget = "20", fpsFloor = "15")
                    }) { Text(text.presetKyc) }
                    OutlinedButton(onClick = {
                        config = config.copy(fpsTarget = "30", fpsFloor = "15")
                    }) { Text(text.presetPreview) }
                }
                Button(
                    onClick = {
                        scope.launch {
                            busy = true
                            message = control.save(config, rawEnv, restartIfRunning = current.running)
                            busy = false
                            reload(fillForm = true)
                        }
                    },
                    enabled = !busy
                ) { Text(text.save) }

                Text(text.identity, style = MaterialTheme.typography.titleMedium)
                if (current.identities.isEmpty()) {
                    Text(text.noFiles)
                } else {
                    current.identities.forEach { name ->
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.SpaceBetween
                        ) {
                            TextButton(onClick = { config = config.copy(identity = name) }) {
                                Text(if (config.identity == name) "● $name" else name)
                            }
                            TextButton(
                                onClick = {
                                    scope.launch {
                                        busy = true
                                        message = control.deleteIdentity(name)
                                        if (config.identity == name) config = config.copy(identity = "")
                                        busy = false
                                        reload(fillForm = false)
                                    }
                                },
                                enabled = !busy
                            ) { Text(text.remove) }
                        }
                    }
                }
                Text(
                    if (config.identity.isBlank()) text.noneSelected else text.selected.format(config.identity),
                    style = MaterialTheme.typography.bodySmall
                )
                Text(
                    text.caption,
                    style = MaterialTheme.typography.bodySmall
                )
                OutlinedButton(onClick = { picker.launch(arrayOf("*/*")) }, enabled = !busy) {
                    Text(text.addFile)
                }

                if (!current.log.isBlank()) {
                    Text(text.log, style = MaterialTheme.typography.titleMedium)
                    Text(
                        current.log.takeLast(1200),
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace
                    )
                }
            }
            if (!message.isNullOrBlank()) {
                Text(message!!, color = MaterialTheme.colorScheme.error)
            }
        }
    }
    if (showPreview) {
        DraggableCameraPreview(
            effect = PreviewEffect(
                blur = config.blurEnabled,
                track = config.blurTrack,
                level = config.blurLevel.toIntOrNull() ?: 0,
                identity = identityBitmap,
                frontCamera = config.lensPolicy != "back"
            ),
            dragLabel = text.drag,
            allowCamera = text.allowCamera,
            closeLabel = text.close,
            modelWait = text.modelWait,
            identityNoFace = text.identityNoFace,
            frameFail = text.frameFail,
            cameraBusy = text.cameraBusy,
            onClose = { showPreview = false }
        )
    }
    }
}

@Composable
private fun ToggleRow(label: String, checked: Boolean, enabled: Boolean, onChecked: (Boolean) -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(label, style = MaterialTheme.typography.titleMedium)
        Switch(checked = checked, onCheckedChange = onChecked, enabled = enabled)
    }
}

@Composable
private fun ChipRow(
    label: String,
    values: List<String>,
    selected: String,
    labels: Map<String, String>,
    onSelect: (String) -> Unit
) {
    Text(label, style = MaterialTheme.typography.labelLarge)
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        values.forEach { value ->
            FilterChip(
                selected = selected == value,
                onClick = { onSelect(value) },
                label = { Text(labels[value] ?: value) }
            )
        }
    }
}
