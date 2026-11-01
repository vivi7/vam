package com.camswap.control

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.util.Base64
import java.util.concurrent.TimeUnit

data class DeviceConfig(
    val lensPolicy: String = "all",
    val accelBackend: String = "gpu_gl",
    val fpsTarget: String = "20",
    val fpsFloor: String = "15",
    val rssBudgetMb: String = "350",
    val thermalLadder: String = "38:detect_rada,40:roi_128,42:480p,44:passthrough",
    val identity: String = "",
    val blurEnabled: Boolean = false,
    val blurTrack: Boolean = true,
    val blurLevel: String = "40"
)

data class ModuleStatus(
    val root: Boolean,
    val modulePath: String,
    val running: Boolean,
    val pid: String,
    val bootEnabled: Boolean,
    val config: DeviceConfig,
    val rawEnv: String,
    val identities: List<String>,
    val log: String,
    val error: String?
)

class ModuleControl(private val workDir: File) {
    @Volatile var lang: String = "it"
    private val suCandidates = listOf("/debug_ramdisk/su", "/system/bin/su")

    private fun t(it: String, en: String) = if (lang == "en") en else it
    private val lock = Any()

    suspend fun status(): ModuleStatus = withContext(Dispatchers.IO) {
        val result = su(STATUS_SCRIPT)
        if (!result.ok) {
            return@withContext ModuleStatus(
                root = false,
                modulePath = "",
                running = false,
                pid = "",
                bootEnabled = true,
                config = DeviceConfig(),
                rawEnv = "",
                identities = emptyList(),
                log = "",
                error = result.error ?: t("su non disponibile", "su is not available")
            )
        }
        val sections = parseSections(result.output)
        val suId = sections["SU"].orEmpty()
        val root = suId.contains("uid=0")
        val modulePath = sections["MODULE"].orEmpty().lineSequence().firstOrNull().orEmpty().trim()
        val pid = sections["PID"].orEmpty().lineSequence().firstOrNull { it.any(Char::isDigit) }.orEmpty().trim()
        val bootEnabled = sections["BOOT"].orEmpty().trim() != "0"
        val rawEnv = sections["ENV"].orEmpty().trim()
        val identities = sections["IDS"].orEmpty().lineSequence().map { it.trim() }.filter { it.isNotEmpty() }.toList()
        val log = sections["LOG"].orEmpty().trim()
        ModuleStatus(
            root = root,
            modulePath = modulePath,
            running = pid.isNotEmpty(),
            pid = pid.substringBefore(" "),
            bootEnabled = bootEnabled,
            config = parseConfig(rawEnv),
            rawEnv = rawEnv,
            identities = identities,
            log = log,
            error = if (root) null else t("Magisk non ha concesso root", "Magisk did not grant root")
        )
    }

    suspend fun setRunning(config: DeviceConfig, rawEnv: String, start: Boolean): String? =
        withContext(Dispatchers.IO) {
            if (start) {
                val written = writeConfig(config, rawEnv)
                if (written != null) return@withContext written
            }
            val script = if (start) startScript(config) else "killall camswap_daemon 2>/dev/null || true"
            val result = su(script)
            if (!result.ok) result.error ?: t("comando fallito", "command failed") else null
        }

    suspend fun setBootEnabled(enabled: Boolean): String? = withContext(Dispatchers.IO) {
        val script = if (enabled) {
            "rm -f /data/adb/modules/camswap/disable"
        } else {
            "mkdir -p /data/adb/modules/camswap && touch /data/adb/modules/camswap/disable"
        }
        val result = su(script)
        if (!result.ok) result.error ?: t("impossibile aggiornare il boot", "could not update boot") else null
    }

    suspend fun save(config: DeviceConfig, rawEnv: String, restartIfRunning: Boolean): String? =
        withContext(Dispatchers.IO) {
            val written = writeConfig(config, rawEnv)
            if (written != null) return@withContext written
            if (!restartIfRunning) return@withContext null
            val result = su(startScript(config))
            if (!result.ok) result.error ?: t("riavvio del daemon fallito", "daemon restart failed") else null
        }

    suspend fun importIdentity(source: File, name: String): String? = withContext(Dispatchers.IO) {
        val safe = sanitizeName(name)
        if (safe.isEmpty()) return@withContext t("nome file non valido", "invalid file name")
        val script = """
            set -e
            MOD=${'$'}(module_dir)
            if [ -z "${'$'}MOD" ]; then echo 'modulo assente'; exit 1; fi
            mkdir -p "${'$'}MOD/system/etc/camswap/identities"
            cp '${source.absolutePath}' "${'$'}MOD/system/etc/camswap/identities/$safe"
            chmod 644 "${'$'}MOD/system/etc/camswap/identities/$safe"
        """.trimIndent()
        val result = su(script)
        if (!result.ok) result.error ?: result.output.ifBlank { t("copia fallita", "copy failed") } else null
    }

    suspend fun copyIdentityTo(name: String, dest: File): String? = withContext(Dispatchers.IO) {
        val safe = sanitizeName(name)
        if (safe.isEmpty()) return@withContext t("nome file non valido", "invalid file name")
        dest.parentFile?.mkdirs()
        val script = """
            MOD=${'$'}(module_dir)
            SRC="${'$'}MOD/system/etc/camswap/identities/$safe"
            if [ ! -f "${'$'}SRC" ]; then echo 'file assente'; exit 1; fi
            cp "${'$'}SRC" '${dest.absolutePath}'
            chmod 644 '${dest.absolutePath}'
        """.trimIndent()
        val result = su(script)
        if (!result.ok) result.error ?: result.output.ifBlank { t("copia identità fallita", "identity copy failed") } else null
    }

    suspend fun deleteIdentity(name: String): String? = withContext(Dispatchers.IO) {
        val safe = sanitizeName(name)
        if (safe.isEmpty()) return@withContext t("nome file non valido", "invalid file name")
        val script = """
            MOD=${'$'}(module_dir)
            if [ -z "${'$'}MOD" ]; then echo 'modulo assente'; exit 1; fi
            rm -f "${'$'}MOD/system/etc/camswap/identities/$safe"
        """.trimIndent()
        val result = su(script)
        if (!result.ok) result.error ?: t("rimozione fallita", "remove failed") else null
    }

    fun validate(config: DeviceConfig): String? {
        if (config.lensPolicy !in setOf("all", "front", "back")) return t("lente non valida", "invalid lens")
        if (config.accelBackend !in setOf("gpu_gl", "cpu")) return t("backend non valido", "invalid backend")
        val target = config.fpsTarget.toIntOrNull() ?: return t("fps target non valido", "invalid target fps")
        val floor = config.fpsFloor.toIntOrNull() ?: return t("fps minimo non valido", "invalid minimum fps")
        val rss = config.rssBudgetMb.toIntOrNull() ?: return t("budget memoria non valido", "invalid memory budget")
        if (target !in 1..60 || floor !in 1..60) return t("fps fuori da 1-60", "fps must be from 1 to 60")
        if (floor > target) return t("il minimo non può superare il target", "minimum cannot exceed the target")
        if (rss !in 50..2000) return t("budget fra 50 e 2000 MB", "budget must be from 50 to 2000 MB")
        if (!config.thermalLadder.matches(Regex("[0-9A-Za-z:,_]+"))) return t("scala termica non valida", "invalid thermal ladder")
        if (config.identity.isNotEmpty() && sanitizeName(config.identity) != config.identity) {
            return t("nome identità non valido", "invalid identity name")
        }
        val blur = config.blurLevel.toIntOrNull() ?: return t("livello blur non valido", "invalid blur level")
        if (blur !in 0..100) return t("livello blur fra 0 e 100", "blur level must be from 0 to 100")
        return null
    }

    private fun writeConfig(config: DeviceConfig, rawEnv: String): String? {
        val problem = validate(config)
        if (problem != null) return problem
        val body = renderEnv(rawEnv, config)
        val encoded = Base64.getEncoder().encodeToString(body.toByteArray(Charsets.UTF_8))
        val script = """
            echo '$encoded' | base64 -d > /data/local/tmp/camswap.device.env
            for p in /data/adb/modules/camswap /data/adb/modules_update/camswap; do
              if [ -d "${'$'}p" ]; then
                mkdir -p "${'$'}p/system/etc/camswap"
                cp /data/local/tmp/camswap.device.env "${'$'}p/system/etc/camswap/device.env"
                chmod 644 "${'$'}p/system/etc/camswap/device.env"
              fi
            done
        """.trimIndent()
        val result = su(script)
        return if (result.ok) null else result.error ?: t("scrittura config fallita", "could not write config")
    }

    private fun startScript(config: DeviceConfig): String {
        val identityArg = if (config.identity.isBlank()) "" else "--identity=${config.identity}"
        val blurArg = "--blur=${if (config.blurEnabled) 1 else 0} --blur-track=${if (config.blurTrack) 1 else 0} --blur-level=${config.blurLevel}"
        return """
            DAEMON=${'$'}(module_dir)/system/bin/camswap_daemon
            if [ ! -x "${'$'}DAEMON" ]; then echo 'daemon assente'; exit 1; fi
            killall camswap_daemon 2>/dev/null || true
            if command -v setsid >/dev/null 2>&1; then
              setsid "${'$'}DAEMON" --device=surfaceduo --lens-policy=${config.lensPolicy} --accel-backend=${config.accelBackend} --fps-target=${config.fpsTarget} --fps-floor=${config.fpsFloor} --rss-budget=${config.rssBudgetMb} --thermal-ladder=${config.thermalLadder} $blurArg $identityArg </dev/null >/data/local/tmp/camswap.daemon.log 2>&1 &
            else
              "${'$'}DAEMON" --device=surfaceduo --lens-policy=${config.lensPolicy} --accel-backend=${config.accelBackend} --fps-target=${config.fpsTarget} --fps-floor=${config.fpsFloor} --rss-budget=${config.rssBudgetMb} --thermal-ladder=${config.thermalLadder} $blurArg $identityArg </dev/null >/data/local/tmp/camswap.daemon.log 2>&1 &
            fi
            sleep 0.4
            pidof camswap_daemon || true
        """.trimIndent()
    }

    private fun renderEnv(rawEnv: String, config: DeviceConfig): String {
        val updates = linkedMapOf(
            "lens_policy" to config.lensPolicy,
            "accel_backend" to config.accelBackend,
            "fps_target" to config.fpsTarget,
            "fps_floor" to config.fpsFloor,
            "rss_budget_mb" to config.rssBudgetMb,
            "thermal_ladder" to config.thermalLadder,
            "identity" to config.identity,
            "blur" to if (config.blurEnabled) "1" else "0",
            "blur_track" to if (config.blurTrack) "1" else "0",
            "blur_level" to config.blurLevel
        )
        val source = if (rawEnv.isBlank()) DEFAULT_ENV else rawEnv
        val lines = source.lines().filter { it.isNotBlank() }.toMutableList()
        val seen = mutableSetOf<String>()
        for (i in lines.indices) {
            val eq = lines[i].indexOf('=')
            if (eq <= 0) continue
            val key = lines[i].substring(0, eq).trim()
            val value = updates[key] ?: continue
            lines[i] = "$key=$value"
            seen += key
        }
        for ((key, value) in updates) {
            if (key !in seen) lines += "$key=$value"
        }
        return lines.joinToString("\n") + "\n"
    }

    private fun parseConfig(raw: String): DeviceConfig {
        val map = raw.lineSequence().mapNotNull { line ->
            val eq = line.indexOf('=')
            if (eq <= 0) null else line.substring(0, eq).trim() to line.substring(eq + 1).trim()
        }.toMap()
        val base = DeviceConfig()
        return base.copy(
            lensPolicy = map["lens_policy"] ?: base.lensPolicy,
            accelBackend = map["accel_backend"] ?: base.accelBackend,
            fpsTarget = map["fps_target"] ?: base.fpsTarget,
            fpsFloor = map["fps_floor"] ?: base.fpsFloor,
            rssBudgetMb = map["rss_budget_mb"] ?: base.rssBudgetMb,
            thermalLadder = map["thermal_ladder"] ?: base.thermalLadder,
            identity = map["identity"] ?: "",
            blurEnabled = map["blur"] == "1",
            blurTrack = map["blur_track"]?.let { it == "1" } ?: base.blurTrack,
            blurLevel = map["blur_level"] ?: base.blurLevel
        )
    }

    private fun parseSections(text: String): Map<String, String> {
        val out = linkedMapOf<String, StringBuilder>()
        var current: StringBuilder? = null
        for (line in text.lines()) {
            if (line.startsWith("##") && line.endsWith("##") && line.length > 4) {
                val name = line.removePrefix("##").removeSuffix("##")
                current = StringBuilder()
                out[name] = current
            } else if (current != null) {
                current.append(line).append('\n')
            }
        }
        return out.mapValues { it.value.toString() }
    }

    private fun sanitizeName(name: String): String {
        val base = name.substringAfterLast('/').substringAfterLast('\\')
        return base.filter { it.isLetterOrDigit() || it == '.' || it == '_' || it == '-' }
    }

    private fun su(script: String): SuResult {
        val full = "module_dir() { for p in /data/adb/modules/camswap /data/adb/modules_update/camswap; do if [ -x \"\$p/system/bin/camswap_daemon\" ]; then echo \"\$p\"; return 0; fi; done; }\n" + script
        return synchronized(lock) { suFile(full) }
    }

    private fun suFile(script: String): SuResult {
        workDir.mkdirs()
        val file = File(workDir, "cmd.sh")
        file.writeText("#!/system/bin/sh\n$script\n")
        val cmd = "sh ${file.absolutePath}"
        var lastError = t("su non trovato", "su not found")
        for (bin in suCandidates) {
            try {
                val process = ProcessBuilder(bin, "-c", cmd)
                    .redirectErrorStream(true)
                    .start()
                val finished = process.waitFor(90, TimeUnit.SECONDS)
                val output = process.inputStream.bufferedReader().readText()
                if (!finished) {
                    process.destroy()
                    return SuResult(false, output, t("timeout in attesa di root", "timed out waiting for root"))
                }
                if (process.exitValue() == 0) return SuResult(true, output, null)
                return SuResult(false, output, output.trim().ifBlank { "su exit ${process.exitValue()}" })
            } catch (error: Exception) {
                lastError = error.message ?: lastError
            }
        }
        return SuResult(false, "", lastError)
    }

    private data class SuResult(val ok: Boolean, val output: String, val error: String?)

    companion object {
        private val DEFAULT_ENV = """
            device=surfaceduo
            soc=sd855
            gpu=adreno640
            ram_gb=6
            sdk_allow=32
            build_allow=2022.902.48
            lens_policy=all
            accel_backend=gpu_gl
            fps_target=20
            fps_floor=15
            rss_budget_mb=350
            thermal_ladder=38:detect_rada,40:roi_128,42:480p,44:passthrough
            identity=
        """.trimIndent()

        private val STATUS_SCRIPT = """
            echo '##SU##'
            id
            echo '##MODULE##'
            module_dir || true
            echo '##PID##'
            pidof camswap_daemon || true
            echo '##BOOT##'
            if [ -f /data/adb/modules/camswap/disable ]; then echo 0; else echo 1; fi
            echo '##ENV##'
            MOD=${'$'}(module_dir || true)
            if [ -n "${'$'}MOD" ] && [ -f "${'$'}MOD/system/etc/camswap/device.env" ]; then
              cat "${'$'}MOD/system/etc/camswap/device.env"
            fi
            echo
            echo '##IDS##'
            if [ -n "${'$'}MOD" ]; then
              ls -1 "${'$'}MOD/system/etc/camswap/identities" 2>/dev/null || true
            fi
            echo '##LOG##'
            tail -n 20 /data/local/tmp/camswap.daemon.log 2>/dev/null || true
            logcat -d -t 15 -s CamswapDaemon:V 2>/dev/null || true
        """.trimIndent()
    }
}
