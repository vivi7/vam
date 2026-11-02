package com.camswap.preview

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Matrix
import android.util.Log
import android.util.Size
import android.view.Surface
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.ui.draw.clip
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicReference
import kotlin.math.roundToInt

data class PreviewEffect(
    val maskRadius: Int = 40,
    val track: Boolean = true,
    val bgBlur: Boolean = false,
    val bgLevel: Int = 40,
    val identity: Bitmap? = null,
    val frontCamera: Boolean = true
)

@Composable
fun DraggableCameraPreview(
    effect: PreviewEffect,
    dragLabel: String,
    allowCamera: String,
    closeLabel: String,
    modelWait: String,
    identityNoFace: String,
    frameFail: String,
    cameraBusy: String,
    onClose: () -> Unit
) {
    val context = LocalContext.current
    var granted by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        )
    }
    val ask = androidx.activity.compose.rememberLauncherForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.RequestPermission()
    ) { granted = it }

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val density = LocalDensity.current
        val boxW = maxWidth / 2
        val boxH = maxHeight / 2
        var offset by remember { mutableStateOf<Offset?>(null) }
        LaunchedEffect(maxWidth, maxHeight) {
            if (offset == null) {
                offset = with(density) {
                    Offset((maxWidth - boxW - 12.dp).toPx(), (maxHeight - boxH - 12.dp).toPx())
                }
            }
        }
        val place = offset ?: Offset.Zero
        val maxX = with(density) { (maxWidth - boxW).toPx().coerceAtLeast(0f) }
        val maxY = with(density) { (maxHeight - boxH).toPx().coerceAtLeast(0f) }

        Box(
            modifier = Modifier
                .offset { IntOffset(place.x.roundToInt(), place.y.roundToInt()) }
                .size(boxW, boxH)
                .border(1.dp, MaterialTheme.colorScheme.outline)
                .background(MaterialTheme.colorScheme.background)
        ) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .pointerInput(maxX, maxY) {
                        detectDragGestures { change, drag ->
                            change.consume()
                            offset = Offset(
                                ((offset?.x ?: 0f) + drag.x).coerceIn(0f, maxX),
                                ((offset?.y ?: 0f) + drag.y).coerceIn(0f, maxY)
                            )
                        }
                    }
            ) {
                if (!granted) {
                    Button(
                        onClick = { ask.launch(Manifest.permission.CAMERA) },
                        modifier = Modifier.align(Alignment.Center)
                    ) { Text(allowCamera) }
                } else {
                    CameraEffect(effect, dragLabel, modelWait, identityNoFace, frameFail, cameraBusy)
                }
            }
            Text(
                "×",
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .padding(4.dp)
                    .size(32.dp)
                    .clip(CircleShape)
                    .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.92f))
                    .clickable(onClick = onClose)
                    .semantics { contentDescription = closeLabel }
                    .padding(top = 2.dp),
                textAlign = TextAlign.Center,
                style = MaterialTheme.typography.titleLarge,
                color = MaterialTheme.colorScheme.onSurface
            )
        }
    }
}

@Composable
private fun CameraEffect(
    effect: PreviewEffect,
    dragLabel: String,
    modelWait: String,
    identityNoFace: String,
    frameFail: String,
    cameraBusy: String
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val holder = remember { AtomicReference(effect) }
    holder.set(effect)
    var frame by remember { mutableStateOf<Bitmap?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    val engine = remember { FaceEffect() }

    DisposableEffect(lifecycleOwner, effect.frontCamera) {
        val executor = Executors.newSingleThreadExecutor()
        val main = ContextCompat.getMainExecutor(context)
        val cameraFuture = ProcessCameraProvider.getInstance(context)
        val runnable = Runnable {
            try {
                val provider = cameraFuture.get()
                val displayRotation = context.display?.rotation ?: Surface.ROTATION_0
                // Sensor is 4032x2784. A square stream is a center crop and cuts off
                // the face, which sits at the bottom of the upright frame.
                val resolution = ResolutionSelector.Builder()
                    .setResolutionStrategy(
                        ResolutionStrategy(
                            Size(480, 640),
                            ResolutionStrategy.FALLBACK_RULE_CLOSEST_LOWER_THEN_HIGHER
                        )
                    )
                    .build()
                val analysis = ImageAnalysis.Builder()
                    .setResolutionSelector(resolution)
                    .setTargetRotation(displayRotation)
                    .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                    .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_RGBA_8888)
                    .build()
                var frameTick = 0
                analysis.setAnalyzer(executor) { image ->
                    var raw: Bitmap? = null
                    var upright: Bitmap? = null
                    var posted = false
                    try {
                        val current = holder.get()
                        raw = image.toBitmap()
                        upright = orient(raw, image.imageInfo.rotationDegrees, current.frontCamera)
                        if (frameTick++ % 30 == 0) {
                            Log.i(
                                "CamswapPreview",
                                "raw ${raw.width}x${raw.height} rot=${image.imageInfo.rotationDegrees} up ${upright.width}x${upright.height}"
                            )
                        }
                        val shown = engine.apply(
                            upright,
                            current.track,
                            current.maskRadius,
                            current.bgBlur,
                            current.bgLevel,
                            current.identity
                        )
                        if (shown !== upright) upright.recycle()
                        if (raw !== upright && raw !== shown) raw.recycle()
                        val note = when (engine.note) {
                            FrameNote.MODEL -> modelWait
                            FrameNote.IDENTITY -> identityNoFace
                            FrameNote.NONE -> null
                        }
                        posted = true
                        main.execute {
                            frame = shown
                            error = note
                        }
                    } catch (failure: Exception) {
                        if (!posted) {
                            raw?.recycle()
                            if (upright != null && upright !== raw) upright.recycle()
                        }
                        val message = failure.message ?: frameFail
                        main.execute { error = message }
                    } finally {
                        image.close()
                    }
                }
                val selector = if (effect.frontCamera) {
                    CameraSelector.DEFAULT_FRONT_CAMERA
                } else {
                    CameraSelector.DEFAULT_BACK_CAMERA
                }
                provider.unbindAll()
                provider.bindToLifecycle(lifecycleOwner, selector, analysis)
            } catch (failure: Exception) {
                error = failure.message ?: cameraBusy
            }
        }
        cameraFuture.addListener(runnable, ContextCompat.getMainExecutor(context))
        onDispose {
            runCatching { cameraFuture.get().unbindAll() }
            engine.close()
            executor.shutdown()
        }
    }

    Box(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)) {
        frame?.let {
            Image(
                bitmap = it.asImageBitmap(),
                contentDescription = "Preview",
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Fit
            )
        }
        effect.identity?.let {
            Image(
                bitmap = it.asImageBitmap(),
                contentDescription = "Identità",
                modifier = Modifier
                    .align(Alignment.BottomStart)
                    .padding(8.dp)
                    .size(42.dp)
                    .clip(CircleShape)
                    .border(1.dp, MaterialTheme.colorScheme.onSurface, CircleShape),
                contentScale = ContentScale.Crop
            )
        }
        Text(
            dragLabel,
            modifier = Modifier.align(Alignment.TopStart).padding(6.dp),
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurface
        )
        error?.let {
            Text(
                it,
                modifier = Modifier.align(Alignment.BottomCenter).padding(6.dp),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.error
            )
        }
    }
}

private fun orient(src: Bitmap, degrees: Int, front: Boolean): Bitmap {
    var current = src
    var owned = false
    fun turn(deg: Int, mirror: Boolean) {
        val steps = ((deg % 360) + 360) % 360
        if (steps == 0 && !mirror) return
        val matrix = Matrix()
        if (steps != 0) matrix.postRotate(steps.toFloat())
        if (mirror) matrix.postScale(-1f, 1f)
        val next = Bitmap.createBitmap(current, 0, 0, current.width, current.height, matrix, true)
        if (owned) current.recycle()
        current = next
        owned = true
    }
    if (current.width > 960 || current.height > 960) {
        val limit = 960f / maxOf(current.width, current.height)
        val small = Bitmap.createScaledBitmap(
            current,
            (current.width * limit).toInt().coerceAtLeast(2),
            (current.height * limit).toInt().coerceAtLeast(2),
            true
        )
        if (owned) current.recycle()
        current = small
        owned = true
    }
    turn(degrees, front)
    if (current.width > current.height) turn(if (front) 270 else 90, false)
    return if (owned) current else src.copy(Bitmap.Config.ARGB_8888, true)
}
