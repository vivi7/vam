package com.camswap.preview

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.ColorMatrix
import android.graphics.ColorMatrixColorFilter
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PointF
import android.graphics.PorterDuff
import android.graphics.PorterDuffXfermode
import android.graphics.Rect
import android.graphics.RectF
import com.google.android.gms.tasks.Tasks
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.face.Face
import com.google.mlkit.vision.face.FaceContour
import com.google.mlkit.vision.face.FaceDetection
import com.google.mlkit.vision.face.FaceDetector
import com.google.mlkit.vision.face.FaceDetectorOptions
import com.google.mlkit.vision.face.FaceLandmark
import kotlin.math.cos
import kotlin.math.sin
import java.util.concurrent.TimeUnit
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

enum class FrameNote { NONE, MODEL, IDENTITY }

class FaceEffect {
    var note: FrameNote = FrameNote.NONE
        private set

    @Volatile private var closed = false
    private var detector: FaceDetector? = null
    private var smooth: Array<PointF>? = null
    private var missed = 0
    private var preparedFor: Bitmap? = null
    private var prepared: Prepared? = null
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
    private val matrix = Matrix()
    private val path = Path()

    fun close() {
        closed = true
        detector?.close()
        detector = null
        prepared?.bitmap?.recycle()
        prepared = null
        preparedFor = null
        smooth = null
    }

    fun apply(frame: Bitmap, blur: Boolean, track: Boolean, level: Int, identity: Bitmap?): Bitmap {
        note = FrameNote.NONE
        if (closed) return frame
        val src = scale(frame, 400)
        val face = detect(src)
        val live = face?.let { mesh(it) }
        val followed = follow(live, track)
        val radius = (level.coerceIn(0, 100) * 22) / 100
        if (followed != null && blur && radius > 0) {
            blurOval(src, followed, radius, if (identity == null) 1.08f else 1.42f)
        } else if (followed == null && face != null && blur && radius > 0) {
            blurBox(src, RectF(face.boundingBox), radius)
        }
        if (identity != null && followed != null) {
            val id = prepare(identity)
            if (id == null) {
                if (note == FrameNote.NONE) note = FrameNote.IDENTITY
            } else {
                warp(src, id, followed)
            }
        }
        return src
    }

    private fun follow(next: Array<PointF>?, track: Boolean): Array<PointF>? {
        if (!track) {
            if (smooth == null && next != null) smooth = copyOf(next)
            return smooth
        }
        if (next == null) {
            missed += 1
            return if (missed <= 8) smooth else null
        }
        missed = 0
        val prev = smooth
        if (prev == null || prev.size != next.size) {
            smooth = copyOf(next)
            return smooth
        }
        for (i in next.indices) {
            prev[i].x = prev[i].x * 0.55f + next[i].x * 0.45f
            prev[i].y = prev[i].y * 0.55f + next[i].y * 0.45f
        }
        return prev
    }

    private fun prepare(identity: Bitmap): Prepared? {
        if (preparedFor === identity && prepared != null) return prepared
        prepared?.bitmap?.recycle()
        prepared = null
        preparedFor = identity
        val scaled = scale(identity, 360)
        val face = detect(scaled)
        val pts = face?.let { mesh(it) }
        if (pts == null) {
            scaled.recycle()
            note = FrameNote.IDENTITY
            return null
        }
        val ready = Prepared(scaled, pts, triangulate(pts))
        prepared = ready
        return ready
    }

    private fun warp(dst: Bitmap, id: Prepared, live: Array<PointF>) {
        if (id.triangles.isEmpty() || live.size != id.points.size) return
        val canvas = Canvas(dst)
        canvas.save()
        canvas.clipPath(oval(live, 1.04f))
        paint.colorFilter = tint(id.bitmap, id.points, dst, live)
        for (tri in id.triangles) {
            val s0 = id.points[tri[0]]
            val s1 = id.points[tri[1]]
            val s2 = id.points[tri[2]]
            val d0 = live[tri[0]]
            val d1 = live[tri[1]]
            val d2 = live[tri[2]]
            if (abs(area(d0, d1, d2)) < 6f || abs(area(s0, s1, s2)) < 6f) continue
            path.rewind()
            path.moveTo(d0.x, d0.y)
            path.lineTo(d1.x, d1.y)
            path.lineTo(d2.x, d2.y)
            path.close()
            val ok = matrix.setPolyToPoly(
                floatArrayOf(s0.x, s0.y, s1.x, s1.y, s2.x, s2.y),
                0,
                floatArrayOf(d0.x, d0.y, d1.x, d1.y, d2.x, d2.y),
                0,
                3
            )
            if (!ok) continue
            canvas.save()
            canvas.clipPath(path)
            canvas.drawBitmap(id.bitmap, matrix, paint)
            canvas.restore()
        }
        canvas.restore()
        paint.colorFilter = null
    }

    private fun tint(src: Bitmap, srcPts: Array<PointF>, dst: Bitmap, dstPts: Array<PointF>): ColorMatrixColorFilter {
        val from = average(src, srcPts)
        val to = average(dst, dstPts)
        val shift = ColorMatrix(
            floatArrayOf(
                1f, 0f, 0f, 0f, (to[0] - from[0]).coerceIn(-28f, 28f),
                0f, 1f, 0f, 0f, (to[1] - from[1]).coerceIn(-28f, 28f),
                0f, 0f, 1f, 0f, (to[2] - from[2]).coerceIn(-28f, 28f),
                0f, 0f, 0f, 1f, 0f
            )
        )
        return ColorMatrixColorFilter(shift)
    }

    private fun average(bitmap: Bitmap, pts: Array<PointF>): FloatArray {
        var r = 0f
        var g = 0f
        var b = 0f
        var n = 0
        for (i in 0 until FACE_N step 2) {
            val x = pts[i].x.toInt().coerceIn(0, bitmap.width - 1)
            val y = pts[i].y.toInt().coerceIn(0, bitmap.height - 1)
            val color = bitmap.getPixel(x, y)
            r += (color shr 16) and 0xff
            g += (color shr 8) and 0xff
            b += color and 0xff
            n++
        }
        val d = n.coerceAtLeast(1).toFloat()
        return floatArrayOf(r / d, g / d, b / d)
    }

    private fun blurOval(dst: Bitmap, pts: Array<PointF>, radius: Int, scale: Float) {
        val shape = oval(pts, scale)
        val bounds = RectF()
        shape.computeBounds(bounds, true)
        blurMasked(dst, bounds, shape, radius)
    }

    private fun blurBox(dst: Bitmap, rect: RectF, radius: Int) {
        val bounds = RectF(
            rect.left - rect.width() * 0.08f,
            rect.top - rect.height() * 0.08f,
            rect.right + rect.width() * 0.08f,
            rect.bottom + rect.height() * 0.08f
        )
        val shape = Path().apply { addOval(bounds, Path.Direction.CW) }
        blurMasked(dst, bounds, shape, radius)
    }

    private fun blurMasked(dst: Bitmap, boundsIn: RectF, shape: Path, radius: Int) {
        val bounds = Rect(
            boundsIn.left.toInt().coerceIn(0, dst.width - 1),
            boundsIn.top.toInt().coerceIn(0, dst.height - 1),
            boundsIn.right.toInt().coerceIn(0, dst.width),
            boundsIn.bottom.toInt().coerceIn(0, dst.height)
        )
        val w = bounds.width()
        val h = bounds.height()
        if (w < 8 || h < 8) return
        val patch = Bitmap.createBitmap(dst, bounds.left, bounds.top, w, h)
        val blurred = boxBlur(patch, radius)
        patch.recycle()
        val masked = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(masked)
        canvas.drawBitmap(blurred, 0f, 0f, null)
        blurred.recycle()
        val local = Path(shape)
        local.offset(-bounds.left.toFloat(), -bounds.top.toFloat())
        val mask = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            xfermode = PorterDuffXfermode(PorterDuff.Mode.DST_IN)
        }
        canvas.drawPath(local, mask)
        Canvas(dst).drawBitmap(masked, bounds.left.toFloat(), bounds.top.toFloat(), null)
        masked.recycle()
    }

    private fun oval(pts: Array<PointF>, scale: Float): Path {
        var cx = 0f
        var cy = 0f
        for (i in 0 until FACE_N) {
            cx += pts[i].x
            cy += pts[i].y
        }
        cx /= FACE_N
        cy /= FACE_N
        val out = Path()
        val x0 = cx + (pts[0].x - cx) * scale
        val y0 = cy + (pts[0].y - cy) * scale
        out.moveTo(x0, y0)
        for (i in 1 until FACE_N) {
            out.lineTo(cx + (pts[i].x - cx) * scale, cy + (pts[i].y - cy) * scale)
        }
        out.close()
        return out
    }

    private fun detect(bitmap: Bitmap): Face? {
        return try {
            val image = InputImage.fromBitmap(bitmap, 0)
            val faces = Tasks.await(client().process(image), 700, TimeUnit.MILLISECONDS)
            faces.maxByOrNull { it.boundingBox.width() * it.boundingBox.height() }
        } catch (failure: Exception) {
            val text = failure.message.orEmpty() + " " + failure.cause?.message.orEmpty()
            if (text.contains("download", ignoreCase = true) || text.contains("model", ignoreCase = true)) {
                note = FrameNote.MODEL
            }
            null
        }
    }

    private fun client(): FaceDetector {
        detector?.let { return it }
        val options = FaceDetectorOptions.Builder()
            .setPerformanceMode(FaceDetectorOptions.PERFORMANCE_MODE_ACCURATE)
            .setLandmarkMode(FaceDetectorOptions.LANDMARK_MODE_ALL)
            .setContourMode(FaceDetectorOptions.CONTOUR_MODE_ALL)
            .setMinFaceSize(0.08f)
            .build()
        return FaceDetection.getClient(options).also { detector = it }
    }

    private fun mesh(face: Face): Array<PointF>? {
        val box = RectF(face.boundingBox)
        if (box.width() < 8f || box.height() < 8f) return null
        val oval = contour(face, FaceContour.FACE, FACE_N, closed = true) ?: ovalFromBox(box)
        val leftEye = contour(face, FaceContour.LEFT_EYE, EYE_N, closed = true)
            ?: circle(mark(face, FaceLandmark.LEFT_EYE) ?: at(box, 0.34f, 0.42f), box.width() * 0.07f, EYE_N)
        val rightEye = contour(face, FaceContour.RIGHT_EYE, EYE_N, closed = true)
            ?: circle(mark(face, FaceLandmark.RIGHT_EYE) ?: at(box, 0.66f, 0.42f), box.width() * 0.07f, EYE_N)
        val mouthLeft = mark(face, FaceLandmark.MOUTH_LEFT) ?: at(box, 0.36f, 0.72f)
        val mouthRight = mark(face, FaceLandmark.MOUTH_RIGHT) ?: at(box, 0.64f, 0.72f)
        val mouthBottom = mark(face, FaceLandmark.MOUTH_BOTTOM) ?: at(box, 0.50f, 0.80f)
        val upper = contour(face, FaceContour.UPPER_LIP_TOP, LIP_N, closed = false)
            ?: curve(mouthLeft, mouthRight, -box.height() * 0.02f, LIP_N)
        val lower = contour(face, FaceContour.LOWER_LIP_BOTTOM, LIP_N, closed = false)
            ?: curve(mouthLeft, mouthBottom, mouthRight, LIP_N)
        val leftBrow = contour(face, FaceContour.LEFT_EYEBROW_TOP, BROW_N, closed = false)
            ?: between(oval, 0.18f, 0.34f, BROW_N)
        val rightBrow = contour(face, FaceContour.RIGHT_EYEBROW_TOP, BROW_N, closed = false)
            ?: between(oval, 0.66f, 0.82f, BROW_N)
        val noseTop = mid(leftEye).let { l -> mid(rightEye).let { r -> PointF((l.x + r.x) / 2f, (l.y + r.y) / 2f) } }
        val nose = contour(face, FaceContour.NOSE_BRIDGE, NOSE_N, closed = false)
            ?: line(noseTop, mark(face, FaceLandmark.NOSE_BASE) ?: at(box, 0.50f, 0.58f), NOSE_N)
        val noseBottom = contour(face, FaceContour.NOSE_BOTTOM, NOSE_N, closed = false)
            ?: line(at(box, 0.42f, 0.60f), at(box, 0.58f, 0.60f), NOSE_N)
        val all = ArrayList<PointF>(COUNT)
        all.addAll(oval)
        all.addAll(leftEye)
        all.addAll(rightEye)
        all.addAll(leftBrow)
        all.addAll(rightBrow)
        all.addAll(nose)
        all.addAll(noseBottom)
        all.addAll(upper)
        all.addAll(lower)
        return if (all.size == COUNT) all.toTypedArray() else null
    }

    private fun mark(face: Face, type: Int): PointF? {
        val found = face.getLandmark(type) ?: return null
        return PointF(found.position.x, found.position.y)
    }

    private fun ovalFromBox(box: RectF): Array<PointF> {
        val cx = box.centerX()
        val cy = box.centerY()
        val rx = box.width() / 2f
        val ry = box.height() / 2f
        return Array(FACE_N) { i ->
            val t = (i.toFloat() / FACE_N) * (Math.PI * 2.0) - Math.PI / 2.0
            PointF(cx + rx * cos(t).toFloat(), cy + ry * sin(t).toFloat())
        }
    }

    private fun circle(c: PointF, radius: Float, n: Int): Array<PointF> {
        val r = radius.coerceAtLeast(4f)
        return Array(n) { i ->
            val t = (i.toFloat() / n) * (Math.PI * 2.0)
            PointF(c.x + r * cos(t).toFloat(), c.y + r * sin(t).toFloat())
        }
    }

    private fun curve(a: PointF, b: PointF, drop: Float, n: Int): Array<PointF> {
        return Array(n) { i ->
            val t = i.toFloat() / (n - 1).coerceAtLeast(1)
            PointF(
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t + drop * sin(Math.PI * t).toFloat()
            )
        }
    }

    private fun curve(a: PointF, mid: PointF, b: PointF, n: Int): Array<PointF> {
        return Array(n) { i ->
            val t = i.toFloat() / (n - 1).coerceAtLeast(1)
            if (t <= 0.5f) mix(a, mid, t * 2f) else mix(mid, b, (t - 0.5f) * 2f)
        }
    }

    private fun line(a: PointF, b: PointF, n: Int): Array<PointF> {
        return Array(n) { i -> mix(a, b, i.toFloat() / (n - 1).coerceAtLeast(1)) }
    }

    private fun at(box: RectF, x: Float, y: Float) = PointF(box.left + box.width() * x, box.top + box.height() * y)

    private fun contour(face: Face, type: Int, n: Int, closed: Boolean): Array<PointF>? {
        val pts = face.getContour(type)?.points ?: return null
        if (pts.size < 2) return null
        return if (closed) sampleClosed(pts, n) else sampleOpen(pts, n)
    }

    private fun sampleClosed(pts: List<PointF>, n: Int): Array<PointF> {
        return Array(n) { i ->
            val f = i * pts.size.toFloat() / n
            val i0 = f.toInt() % pts.size
            val i1 = (i0 + 1) % pts.size
            val t = f - f.toInt()
            mix(pts[i0], pts[i1], t)
        }
    }

    private fun sampleOpen(pts: List<PointF>, n: Int): Array<PointF> {
        if (n <= 1) return arrayOf(PointF(pts[0].x, pts[0].y))
        return Array(n) { i ->
            val f = i * (pts.size - 1).toFloat() / (n - 1)
            val i0 = f.toInt().coerceIn(0, pts.size - 2)
            mix(pts[i0], pts[i0 + 1], f - i0)
        }
    }

    private fun between(pts: Array<PointF>, from: Float, to: Float, n: Int): Array<PointF> {
        return Array(n) { i ->
            val t = from + (to - from) * i / (n - 1).coerceAtLeast(1)
            val f = t * pts.size
            val i0 = f.toInt().coerceIn(0, pts.size - 1)
            val i1 = (i0 + 1) % pts.size
            mix(pts[i0], pts[i1], f - f.toInt())
        }
    }

    private fun mid(pts: Array<PointF>): PointF {
        var x = 0f
        var y = 0f
        for (p in pts) {
            x += p.x
            y += p.y
        }
        return PointF(x / pts.size, y / pts.size)
    }

    private fun mix(a: PointF, b: PointF, t: Float) = PointF(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t)

    private fun copyOf(pts: Array<PointF>) = Array(pts.size) { PointF(pts[it].x, pts[it].y) }

    private fun scale(src: Bitmap, maxWidth: Int): Bitmap {
        var w = src.width
        var h = src.height
        if (w > maxWidth) {
            h = h * maxWidth / w
            w = maxWidth
        }
        w = w.coerceAtLeast(2)
        h = h.coerceAtLeast(2)
        val scaled = if (w == src.width && h == src.height) src else Bitmap.createScaledBitmap(src, w, h, true)
        if (scaled !== src && scaled.isMutable && scaled.config == Bitmap.Config.ARGB_8888) return scaled
        val copy = scaled.copy(Bitmap.Config.ARGB_8888, true)
        if (scaled !== src) scaled.recycle()
        return copy
    }

    private fun boxBlur(src: Bitmap, radius: Int): Bitmap {
        val w = src.width
        val h = src.height
        val pixels = IntArray(w * h)
        val tmp = IntArray(w * h)
        src.getPixels(pixels, 0, w, 0, 0, w, h)
        val r = radius.coerceIn(1, 18)
        blurAxis(pixels, tmp, w, h, r, horizontal = true)
        blurAxis(tmp, pixels, w, h, r, horizontal = false)
        val out = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        out.setPixels(pixels, 0, w, 0, 0, w, h)
        return out
    }

    private fun blurAxis(src: IntArray, dst: IntArray, w: Int, h: Int, radius: Int, horizontal: Boolean) {
        val span = if (horizontal) w else h
        val lines = if (horizontal) h else w
        for (line in 0 until lines) {
            for (i in 0 until span) {
                var a = 0
                var r = 0
                var g = 0
                var b = 0
                var n = 0
                val start = max(0, i - radius)
                val end = min(span - 1, i + radius)
                for (p in start..end) {
                    val index = if (horizontal) line * w + p else p * w + line
                    val color = src[index]
                    a += color ushr 24
                    r += (color shr 16) and 0xff
                    g += (color shr 8) and 0xff
                    b += color and 0xff
                    n++
                }
                val index = if (horizontal) line * w + i else i * w + line
                dst[index] = (a / n shl 24) or (r / n shl 16) or (g / n shl 8) or (b / n)
            }
        }
    }

    private data class Prepared(val bitmap: Bitmap, val points: Array<PointF>, val triangles: List<IntArray>)

    companion object {
        private const val FACE_N = 20
        private const val EYE_N = 8
        private const val BROW_N = 5
        private const val NOSE_N = 3
        private const val LIP_N = 7
        private const val COUNT = FACE_N + EYE_N * 2 + BROW_N * 2 + NOSE_N * 2 + LIP_N * 2

        private fun area(a: PointF, b: PointF, c: PointF): Float {
            return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)
        }

        private fun triangulate(pts: Array<PointF>): List<IntArray> {
            val n = pts.size
            if (n < 3) return emptyList()
            var minX = Float.POSITIVE_INFINITY
            var minY = Float.POSITIVE_INFINITY
            var maxX = Float.NEGATIVE_INFINITY
            var maxY = Float.NEGATIVE_INFINITY
            for (p in pts) {
                minX = min(minX, p.x)
                minY = min(minY, p.y)
                maxX = max(maxX, p.x)
                maxY = max(maxY, p.y)
            }
            val span = max(maxX - minX, maxY - minY).coerceAtLeast(1f) * 8f
            val midX = (minX + maxX) / 2f
            val midY = (minY + maxY) / 2f
            val extra = arrayOf(
                PointF(midX - span, midY - span),
                PointF(midX + span, midY - span),
                PointF(midX, midY + span)
            )
            val all = Array(n + 3) { i -> if (i < n) pts[i] else extra[i - n] }
            val tris = ArrayList<IntArray>()
            tris.add(intArrayOf(n, n + 1, n + 2))
            for (pi in 0 until n) {
                val bad = ArrayList<IntArray>()
                val it = tris.iterator()
                while (it.hasNext()) {
                    val t = it.next()
                    if (inside(all[pi], all[t[0]], all[t[1]], all[t[2]])) {
                        bad.add(t)
                        it.remove()
                    }
                }
                val edges = ArrayList<Long>()
                for (t in bad) {
                    edge(edges, t[0], t[1])
                    edge(edges, t[1], t[2])
                    edge(edges, t[2], t[0])
                }
                for (e in edges) {
                    tris.add(intArrayOf((e ushr 32).toInt(), e.toInt(), pi))
                }
            }
            return tris.filter { t -> t[0] < n && t[1] < n && t[2] < n }
        }

        private fun edge(edges: MutableList<Long>, i: Int, j: Int) {
            val a = min(i, j)
            val b = max(i, j)
            val key = (a.toLong() shl 32) or b.toLong()
            val at = edges.indexOf(key)
            if (at >= 0) edges.removeAt(at) else edges.add(key)
        }

        private fun inside(p: PointF, a: PointF, b: PointF, c: PointF): Boolean {
            val ax = (a.x - p.x).toDouble()
            val ay = (a.y - p.y).toDouble()
            val bx = (b.x - p.x).toDouble()
            val by = (b.y - p.y).toDouble()
            val cx = (c.x - p.x).toDouble()
            val cy = (c.y - p.y).toDouble()
            val det = (ax * ax + ay * ay) * (bx * cy - cx * by) -
                (bx * bx + by * by) * (ax * cy - cx * ay) +
                (cx * cx + cy * cy) * (ax * by - bx * ay)
            val orient = ax * (by - cy) + bx * (cy - ay) + cx * (ay - by)
            return if (orient > 0.0) det > 0.0 else det < 0.0
        }
    }
}
