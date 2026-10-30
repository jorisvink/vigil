package se.sanctorum.vigil

import android.Manifest
import android.app.NotificationManager
import android.content.ClipboardManager
import android.content.ContentValues
import android.content.Context
import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.os.Environment
import android.provider.MediaStore
import android.provider.OpenableColumns
import android.provider.Settings
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import android.content.pm.PackageManager
import android.net.VpnService
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.util.Log
import android.view.Surface
import android.view.WindowManager
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.view.TextureRegistry
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.security.KeyStore
import org.json.JSONObject

/**
 * Answers the user interface on the se.sanctorum.vigil/core channel.
 */
class MainActivity : FlutterActivity() {
    private lateinit var store: TunnelStore
    private var pending: Pair<MethodChannel.Result, () -> Unit>? = null
    private var picking: Pair<String, MethodChannel.Result>? = null
    private var saving: Triple<String, String, MethodChannel.Result>? = null
    private var microphone: Pair<Intent, MethodChannel.Result?>? = null
    private var textures: TextureRegistry? = null
    private var remoteTexture: TextureRegistry.SurfaceTextureEntry? = null
    private var localTexture: TextureRegistry.SurfaceTextureEntry? = null
    private var scanTexture: TextureRegistry.SurfaceTextureEntry? = null
    private var scanning: MethodChannel.Result? = null
    private var locked = false

    @Volatile
    private var shared = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_SECURE)

        if (savedInstanceState == null && (intent.flags and Intent.FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY) == 0) {
            calling(intent)
        }
    }

    override fun onStart() {
        super.onStart()
        Chats.foreground = true

        try {
            Chats.vault(this).prepare()
        } catch (e: Exception) {
            Log.w("vigil", "failed to prepare the chat keys", e)
        }
    }

    override fun onStop() {
        Voice.finish()?.fill(0)
        Voice.stop()
        scanStop()
        Chats.background(this)
        super.onStop()
    }

    override fun onRestart() {
        super.onRestart()

        if (shared) {
            shared = false
            Files.wipe(this)
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        calling(intent)
    }

    override fun configureFlutterEngine(engine: FlutterEngine) {
        super.configureFlutterEngine(engine)

        store = TunnelStore(this)
        textures = engine.renderer
        Video.context = applicationContext

        MethodChannel(engine.dartExecutor.binaryMessenger, CHANNEL)
            .setMethodCallHandler(::handle)
    }

    private fun handle(call: MethodCall, result: MethodChannel.Result) {
        try {
            when (call.method) {
                "list" -> result.success(store.list().toString())
                "check" -> wiped(call.arguments as ByteArray) { result.success(store.check(it)) }
                "import" -> wiped(call.arguments as ByteArray) { result.success(store.import(it).toString()) }
                "handoff" -> wiped(Handoff.take() ?: ByteArray(0)) { result.success(it) }
                "edit" -> result.success(
                    store.edit(call.argument<String>("id")!!, call.argument<String>("edits")!!)
                        .toString()
                )
                "remove" -> {
                    store.remove(call.arguments as String)
                    result.success("")
                }
                "connect" -> connect(call.arguments as String, result)
                "disconnect" -> {
                    startService(service(VigilVpnService.ACTION_DISCONNECT))
                    result.success("")
                }
                "gospel" -> {
                    val on = call.arguments as Boolean
                    val intent = service(VigilVpnService.ACTION_GOSPEL)
                        .putExtra(VigilVpnService.EXTRA_ON, on)

                    if (on) {
                        withVpn(result) { withMicrophone(intent, result) }
                    } else {
                        startService(intent)
                        result.success("")
                    }
                }
                "status" -> {
                    val status = JSONObject(Vigil.status())
                    val voice = JSONObject(Vigil.voiceStatus())

                    if (VigilVpnService.active != null) {
                        Calls.update(status, voice)
                    }

                    if (locked && !Calls.busy) {
                        unlocked()
                    }

                    result.success(
                    JSONObject()
                        .put("tunnel", VigilVpnService.active ?: JSONObject.NULL)
                        .put("lockdown", VigilVpnService.lockdown)
                        .put("status", status)
                        .put("voice", voice)
                        .put("phone", Calls.json() ?: JSONObject.NULL)
                        .put("chats", Chats.json(this))
                        .put("gospel", store.gospel)
                        .put("background", background())
                        .toString()
                    )
                }
                "call" -> {
                    val video = call.argument<Boolean>("video") == true

                    withMicrophone(
                        service(VigilVpnService.ACTION_CALL)
                            .putExtra(VigilVpnService.EXTRA_PEER, call.argument<String>("peer")!!.toInt(16))
                            .putExtra(VigilVpnService.EXTRA_VIDEO, video),
                        result,
                        video
                    )
                }
                "answer" -> {
                    val video = call.arguments as Boolean? == true

                    withMicrophone(
                        service(VigilVpnService.ACTION_ANSWER).putExtra(VigilVpnService.EXTRA_VIDEO, video),
                        result,
                        video
                    )
                }
                "camera" -> withMicrophone(
                    service(VigilVpnService.ACTION_CAMERA)
                        .putExtra(VigilVpnService.EXTRA_ON, call.arguments as Boolean),
                    result,
                    call.arguments as Boolean
                )
                "flip" -> {
                    Thread { Video.flip() }.start()
                    result.success("")
                }
                "videoTextures" -> result.success(videoTextures().toString())
                "videoState" -> result.success(videoState().toString())
                "videoRelease" -> {
                    videoRelease()
                    result.success("")
                }
                "scanStart" -> {
                    if (granted(Manifest.permission.CAMERA)) {
                        scanStart(result)
                        return
                    }

                    scanning?.error("cancelled", "The scanner was started again.", null)
                    scanning = result
                    requestPermissions(arrayOf(Manifest.permission.CAMERA), REQUEST_SCAN)
                }
                "scanState" -> result.success(Scanner.state().toString())
                "scanStop" -> {
                    scanStop()
                    result.success("")
                }
                "scanImage" -> {
                    val path = call.arguments as String
                    background(result) { Scanner.image(path) }
                }
                "decline" -> {
                    startService(service(VigilVpnService.ACTION_DECLINE))
                    result.success("")
                }
                "hangup" -> {
                    startService(service(VigilVpnService.ACTION_HANGUP))
                    result.success("")
                }
                "mute" -> {
                    startService(
                        service(VigilVpnService.ACTION_MUTE)
                            .putExtra(VigilVpnService.EXTRA_ON, call.arguments as Boolean)
                    )
                    result.success("")
                }
                "speaker" -> {
                    startService(
                        service(VigilVpnService.ACTION_SPEAKER)
                            .putExtra(VigilVpnService.EXTRA_ON, call.arguments as Boolean)
                    )
                    result.success("")
                }
                "names" -> {
                    val names = call.arguments as String?
                    result.success(
                        (if (names == null) PeerNames.load(this)
                        else PeerNames.save(this, JSONObject(names))).toString()
                    )
                }
                "chats" -> {
                    val vault = Chats.vault(this)
                    vault.merge()
                    result.success(vault.conversations().toString())
                }
                "chat" -> {
                    val vault = Chats.vault(this)
                    vault.merge()
                    result.success(vault.messages(chat(call.arguments as String)).toString())
                }
                "chatSend" -> {
                    val id = chat(call.argument<String>("peer")!!)
                    val text = call.argument<String>("text")!!
                    val reply = call.argument<Number>("reply")?.toLong() ?: 0L
                    val bytes = text.toByteArray()
                    val group = ChatVault.group(id)
                    val vault = Chats.vault(this)
                    var max = MESSAGE_MAX
                    var quote: ChatVault.Quote? = null

                    if (reply != 0L) {
                        quote = vault.quoted(id, reply)
                        max = REPLY_MAX
                    }

                    require(bytes.isNotEmpty() && bytes.size <= max) {
                        "A message is at most $max bytes."
                    }

                    require(group == null || group == Chats.group(this)) {
                        "You are not in this group anymore."
                    }

                    val ref = vault.ref()
                    val author = quote?.peer ?: 0
                    val hash = quote?.digest ?: 0L

                    require(group == null || Vigil.chatGroupSend(ref, bytes, author, hash)) {
                        "Gospel is not running, group messages only go to members that are around."
                    }

                    vault.send(id, text, ref, quote)
                    if (group == null) {
                        Vigil.chatSend(id.toInt(16), ref, bytes, author, hash)
                    }

                    Chats.changed()
                    result.success(ref.toString())
                }
                "chatImageSend" -> {
                    val id = chat(call.argument<String>("peer")!!)
                    val path = call.argument<String>("path")!!

                    background(result) {
                        val image = Images.jpeg(this, path)

                        try {
                            dataSend(id, "image", Images.name(), image)
                        } finally {
                            image.fill(0)
                        }
                    }
                }
                "clipboardImage" -> {
                    val description = getSystemService(ClipboardManager::class.java).primaryClipDescription
                    result.success((description?.hasMimeType("image/*") == true).toString())
                }
                "clipboardImageData" -> {
                    val clip = getSystemService(ClipboardManager::class.java).primaryClip
                    val uri = clip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.uri
                        ?: throw IllegalArgumentException("There is no image to paste.")

                    background(result) { Images.jpeg(this, uri) }
                }
                "chatImageData" -> {
                    val id = chat(call.argument<String>("peer")!!)
                    val image = call.argument<ByteArray>("data")!!

                    background(result) {
                        try {
                            dataSend(id, "image", Images.name(), image)
                        } finally {
                            image.fill(0)
                        }
                    }
                }
                "chatFileSend" -> {
                    val id = chat(call.arguments as String)

                    require(picking == null) { "Already picking a file." }

                    picking = Pair(id, result)
                    startActivityForResult(
                        Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"),
                        REQUEST_FILE
                    )
                }
                "voiceRecord" -> {
                    chat(call.arguments as String)

                    if (!granted(Manifest.permission.RECORD_AUDIO)) {
                        requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQUEST_VOICE)
                        result.error("denied", "Allow the microphone, then hold to record.", null)
                        return
                    }

                    Voice.record(this)
                    result.success("")
                }
                "voiceSend" -> {
                    val id = chat(call.arguments as String)

                    background(result) {
                        val data = Voice.finish() ?: throw IllegalArgumentException("Nothing was recorded.")

                        try {
                            dataSend(id, "voice", Voice.name(), data)
                        } finally {
                            data.fill(0)
                        }
                    }
                }
                "voiceCancel" -> background(result) {
                    Voice.finish()?.fill(0)
                    ""
                }
                "voicePlay" -> {
                    val id = chat(call.argument<String>("peer")!!)
                    val file = call.argument<String>("data")!!
                    val at = call.argument<Int>("at") ?: 0
                    val rate = call.argument<Double>("rate") ?: 1.0

                    voicePlay(id, file, at, rate.toFloat(), result)
                }
                "voiceStop" -> {
                    Voice.stop()
                    result.success("")
                }
                "voiceSeek" -> {
                    Voice.seek(call.arguments as Int)
                    result.success("")
                }
                "voiceSpeed" -> {
                    Voice.speed((call.arguments as Double).toFloat())
                    result.success("")
                }
                "voicePosition" -> result.success(Voice.position())
                "chatData" -> {
                    val id = chat(call.argument<String>("peer")!!)
                    val file = call.argument<String>("data")!!

                    background(result) { Chats.vault(this).data(id, file) }
                }
                "chatDataOpen" -> {
                    val id = chat(call.argument<String>("peer")!!)
                    val file = call.argument<String>("data")!!
                    val name = call.argument<String>("name")!!

                    background(result) { dataOpen(id, file, name) }
                }
                "chatDataSave" -> {
                    val id = chat(call.argument<String>("peer")!!)
                    val file = call.argument<String>("data")!!
                    val name = call.argument<String>("name")!!
                    val photo = call.argument<String>("media") == "image"

                    if (photo && Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                        background(result) { photoSave(id, file, name) }
                    } else {
                        require(saving == null) { "Already saving a file." }

                        saving = Triple(id, file, result)
                        startActivityForResult(
                            Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                                .setType(Files.type(name)).putExtra(Intent.EXTRA_TITLE, name),
                            REQUEST_SAVE
                        )
                    }
                }
                "chatRead" -> {
                    val id = chat(call.arguments as String)
                    Chats.vault(this).read(id)
                    Chats.seen(this, id)
                    Chats.changed()
                    result.success("")
                }
                "chatFavorite" -> {
                    val id = chat(call.argument<String>("peer")!!)
                    Chats.vault(this).favorite(
                        id, call.argument<Number>("ref")!!.toLong(), call.argument<Boolean>("on")!!
                    )
                    Chats.changed()
                    result.success("")
                }
                "chatPin" -> {
                    val id = chat(call.argument<String>("peer")!!)
                    Chats.vault(this).pin(
                        id, call.argument<Number>("ref")!!.toLong(), call.argument<Boolean>("on")!!
                    )
                    Chats.changed()
                    result.success("")
                }
                "chatOpen" -> {
                    Chats.open(this, chat(call.argument<String>("peer")!!), call.argument<Boolean>("on")!!)
                    result.success("")
                }
                "chatTyping" -> {
                    Chats.typing(chat(call.argument<String>("peer")!!), call.argument<Boolean>("on")!!)
                    result.success("")
                }
                "chatDelete" -> {
                    val id = chat(call.arguments as String)
                    Chats.vault(this).delete(id)
                    if (ChatVault.group(id) == null) {
                        Vigil.chatForget(id.toInt(16))
                    }
                    Chats.changed()
                    result.success("")
                }
                "chatGroup" -> {
                    val group = (call.arguments as String).ifEmpty { "0" }
                    require(group.matches(Regex("[0-9a-fA-F]{1,4}"))) { "A group is 1 to 4 hex digits." }
                    Chats.join(this, group.toInt(16))
                    result.success("")
                }
                "chatSettings" -> result.success(
                    Chats.settings(
                        this, call.argument<String>("nick"), call.argument<Boolean>("preview")
                    ).toString()
                )
                "flag" -> {
                    val key = call.argument<String>("key")!!
                    val prefs = getSharedPreferences("ui", Context.MODE_PRIVATE)

                    call.argument<Boolean>("value")?.let { prefs.edit().putBoolean(key, it).apply() }
                    result.success(prefs.getBoolean(key, false).toString())
                }
                "eraseAll" -> {
                    Voice.finish()?.fill(0)
                    Voice.stop()
                    scanStop()

                    Thread {
                        erase()
                        runOnUiThread {
                            result.success("")
                            restart()
                        }
                    }.start()
                }
                "allowBackground" -> {
                    allowBackground()
                    result.success("")
                }
                "version" -> result.success(Vigil.version())
                "kyrkaVersion" -> result.success(Vigil.kyrkaVersion())
                else -> result.notImplemented()
            }
        } catch (e: TunnelStore.Invalid) {
            result.error("invalid", e.message, null)
        } catch (e: ChatVault.Locked) {
            result.error("locked", e.message, null)
        } catch (e: IllegalArgumentException) {
            result.error("invalid", e.message, null)
        } catch (e: Exception) {
            result.error("failed", e.message ?: e.toString(), null)
        }
    }

    private fun background(): Boolean =
        getSystemService(PowerManager::class.java).isIgnoringBatteryOptimizations(packageName)

    private fun allowBackground() {
        try {
            startActivity(
                Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName"))
            )
        } catch (e: ActivityNotFoundException) {
            startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
        }
    }

    private fun erase() {
        VigilVpnService.disconnect()?.join(ERASE_WAIT_MS)
        stopService(Intent(this, VigilVpnService::class.java))
        getSystemService(NotificationManager::class.java).cancelAll()

        val keys = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        keys.aliases().toList().forEach { keys.deleteEntry(it) }

        File(dataDir, "shared_prefs").list()?.forEach { deleteSharedPreferences(it.removeSuffix(".xml")) }
        dataDir.listFiles()?.filter { it.name != "lib" }?.forEach { it.deleteRecursively() }
    }

    private fun restart() {
        val launch = packageManager.getLaunchIntentForPackage(packageName) ?: return

        startActivity(launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK))
        Runtime.getRuntime().exit(0)
    }

    private fun wiped(config: ByteArray, work: (ByteArray) -> Unit) {
        try {
            work(config)
        } finally {
            config.fill(0)
        }
    }

    private fun connect(id: String, result: MethodChannel.Result) {
        val wanted = mutableListOf<String>()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            !granted(Manifest.permission.POST_NOTIFICATIONS)
        ) {
            wanted.add(Manifest.permission.POST_NOTIFICATIONS)
        }

        if (store.voice() != null && !granted(Manifest.permission.RECORD_AUDIO)) {
            wanted.add(Manifest.permission.RECORD_AUDIO)
        }

        if (wanted.isNotEmpty()) {
            requestPermissions(wanted.toTypedArray(), REQUEST_NOTIFY)
        }

        withVpn(result) { start(id, result) }
    }

    private fun withVpn(result: MethodChannel.Result, then: () -> Unit) {
        val consent = VpnService.prepare(this)

        if (consent == null) {
            then()
            return
        }

        pending?.first?.error("cancelled", "Another tunnel was connected.", null)
        pending = result to then
        startActivityForResult(consent, REQUEST_VPN)
    }

    override fun onDestroy() {
        pending?.first?.error("cancelled", "The VPN permission was not answered.", null)
        pending = null
        super.onDestroy()
    }

    private fun start(id: String, result: MethodChannel.Result) {
        store.used(id)
        startService(service(VigilVpnService.ACTION_CONNECT).putExtra(VigilVpnService.EXTRA_ID, id))
        result.success("")
    }

    @Deprecated("startActivityForResult is all a FlutterActivity offers")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)

        if (requestCode == REQUEST_FILE) {
            val (id, result) = picking ?: return
            picking = null

            val uri = data?.data

            if (resultCode != RESULT_OK || uri == null) {
                result.success(null)
                return
            }

            background(result) { fileSend(id, uri) }
            return
        }

        if (requestCode == REQUEST_SAVE) {
            val (id, file, result) = saving ?: return
            saving = null

            val uri = data?.data

            if (resultCode != RESULT_OK || uri == null) {
                result.success("")
                return
            }

            background(result) { dataSave(id, file, uri) }
            return
        }

        if (requestCode != REQUEST_VPN) {
            return
        }

        val (result, then) = pending ?: return
        pending = null

        if (resultCode == RESULT_OK) {
            then()
        } else {
            result.error("denied", "Permission to set up a VPN was not granted.", null)
        }
    }

    private fun calling(intent: Intent?) {
        if (intent?.component?.className != VigilVpnService.NOTIFIED) {
            return
        }

        when (intent.action) {
            VigilVpnService.ACTION_ANSWER -> {
                show()
                withMicrophone(service(VigilVpnService.ACTION_ANSWER), null)
            }
            VigilVpnService.ACTION_SHOW -> show()
            Chats.ACTION_CHAT -> Chats.requested = intent.getStringExtra(Chats.EXTRA_CHAT) ?: ""
        }
    }

    private fun show() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
            setShowWhenLocked(true)
            setTurnScreenOn(true)
            locked = true
        }
    }

    private fun unlocked() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
            setShowWhenLocked(false)
            setTurnScreenOn(false)
        }

        locked = false
    }

    private fun withMicrophone(intent: Intent, result: MethodChannel.Result?, camera: Boolean = false) {
        val wanted = mutableListOf(Manifest.permission.RECORD_AUDIO)
        if (camera) {
            wanted.add(Manifest.permission.CAMERA)
        }

        if (wanted.all { granted(it) }) {
            startService(intent)
            result?.success("")
            return
        }

        microphone?.second?.error("cancelled", "Another call was started.", null)
        microphone = intent to result
        requestPermissions(wanted.toTypedArray(), REQUEST_MICROPHONE)
    }

    private fun videoTextures(): JSONObject {
        val registry = textures ?: throw IllegalStateException("No textures.")

        val remote = remoteTexture ?: registry.createSurfaceTexture().also {
            it.surfaceTexture().setDefaultBufferSize(VIDEO_WIDTH, VIDEO_HEIGHT)
            Video.remoteSurface(Surface(it.surfaceTexture()))
            remoteTexture = it
        }

        val local = localTexture ?: registry.createSurfaceTexture().also {
            Video.localSurface(it.surfaceTexture())
            localTexture = it
        }

        return JSONObject().put("remote", remote.id()).put("local", local.id())
    }

    private fun videoState(): JSONObject =
        JSONObject()
            .put("decoding", Video.decoding)
            .put("capturing", Video.capturing)
            .put("front", Video.front)
            .put("rotation", Video.remoteRotation)
            .put("width", Video.remoteWidth)
            .put("height", Video.remoteHeight)
            .put("localRotation", 0)
            .put("localMirror", false)
            .put("localWidth", Video.localWidth)
            .put("localHeight", Video.localHeight)

    private fun scanStart(result: MethodChannel.Result) {
        val registry = textures ?: throw IllegalStateException("No textures.")
        val entry = scanTexture ?: registry.createSurfaceTexture().also { scanTexture = it }

        Scanner.start(this, entry.surfaceTexture()) { e ->
            if (e == null) {
                result.success(JSONObject().put("texture", entry.id()).toString())
            } else {
                Log.w("vigil", "failed to start the scanner", e)
                scanStop()
                result.error("unsupported", "The camera could not be started.", null)
            }
        }
    }

    private fun scanStop() {
        Scanner.stop()
        scanTexture?.release()
        scanTexture = null
    }

    private fun videoRelease() {
        Video.remoteSurface(null)
        Video.localSurface(null)

        remoteTexture?.release()
        localTexture?.release()
        remoteTexture = null
        localTexture = null
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)

        if (requestCode == REQUEST_SCAN) {
            val result = scanning ?: return
            scanning = null

            if (granted(Manifest.permission.CAMERA)) {
                scanStart(result)
            } else {
                result.error("denied", "Camera access was denied.", null)
            }
            return
        }

        if (requestCode != REQUEST_MICROPHONE) {
            return
        }

        val (intent, result) = microphone ?: return
        microphone = null

        if (!granted(Manifest.permission.RECORD_AUDIO)) {
            if (intent.action == VigilVpnService.ACTION_ANSWER) {
                startService(service(VigilVpnService.ACTION_DECLINE))
            }
            result?.error("denied", "Calls need access to the microphone.", null)
            return
        }

        if (intent.getBooleanExtra(VigilVpnService.EXTRA_VIDEO, false) && !granted(Manifest.permission.CAMERA)) {
            intent.putExtra(VigilVpnService.EXTRA_VIDEO, false)
        }

        if (intent.action == VigilVpnService.ACTION_CAMERA && !granted(Manifest.permission.CAMERA)) {
            result?.error("denied", "Video needs access to the camera.", null)
            return
        }

        startService(intent)
        result?.success("")
    }

    private fun dataSend(id: String, media: String, name: String, data: ByteArray): String {
        val group = ChatVault.group(id)
        val kind = ChatVault.kind(media)
        val label = name.toByteArray()

        require(group == null || group == Chats.group(this)) { "You are not in this group anymore." }
        require(data.isNotEmpty() && data.size <= DATA_MAX) { "A file is at most 4 MB." }

        val vault = Chats.vault(this)
        val ref = vault.ref()

        require(group == null || Vigil.chatGroupDataSend(ref, kind, label, data)) {
            "Gospel is not running or still sending other files, group files only go to members that are around."
        }

        vault.sendData(id, media, name, data, ref)
        if (group == null && !Vigil.chatDataSend(id.toInt(16), ref, kind, label, data)) {
            Chats.refused(ref)
        }

        Chats.changed()

        return ref.toString()
    }

    private fun fileSend(id: String, uri: Uri): String {
        var name = "file"
        var size = -1L

        contentResolver.query(uri, null, null, null, null)?.use {
            if (it.moveToFirst()) {
                val column = it.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (column >= 0) {
                    name = it.getString(column) ?: name
                }

                val length = it.getColumnIndex(OpenableColumns.SIZE)
                if (length >= 0 && !it.isNull(length)) {
                    size = it.getLong(length)
                }
            }
        }

        require(size <= DATA_MAX) { "A file is at most 4 MB." }

        val data = contentResolver.openInputStream(uri)?.use { input ->
            val out = ByteArrayOutputStream()
            val buf = ByteArray(64 * 1024)

            while (true) {
                val n = input.read(buf)
                if (n == -1) {
                    break
                }

                out.write(buf, 0, n)
                require(out.size() <= DATA_MAX) { "A file is at most 4 MB." }
            }

            out.toByteArray()
        } ?: throw IllegalArgumentException("That file cannot be read.")

        try {
            return dataSend(id, "file", Files.name(name), data)
        } finally {
            data.fill(0)
        }
    }

    private fun dataOpen(id: String, file: String, name: String): String {
        val data = Chats.vault(this).data(id, file) ?: throw IllegalArgumentException("That file is gone.")
        val uri = Files.share(this, name, data)

        data.fill(0)
        shared = true

        val view = Intent(Intent.ACTION_VIEW)
            .setDataAndType(uri, Files.type(name))
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)

        runOnUiThread {
            try {
                startActivity(Intent.createChooser(view, name))
            } catch (e: ActivityNotFoundException) {
                Log.w("vigil", "nothing opens $name", e)
            }
        }

        return ""
    }

    private fun voicePlay(id: String, file: String, at: Int, rate: Float, result: MethodChannel.Result) {
        Thread {
            try {
                val data = Chats.vault(this).data(id, file) ?: throw IllegalArgumentException("That voice message is gone.")
                Voice.play(this, data, at, rate) { runOnUiThread { result.success("") } }
            } catch (e: ChatVault.Locked) {
                runOnUiThread { result.error("locked", e.message, null) }
            } catch (e: IllegalArgumentException) {
                runOnUiThread { result.error("invalid", e.message, null) }
            } catch (e: Exception) {
                runOnUiThread { result.error("failed", e.message ?: e.toString(), null) }
            }
        }.start()
    }

    private fun dataSave(id: String, file: String, uri: Uri): String {
        val data = Chats.vault(this).data(id, file) ?: throw IllegalArgumentException("That file is gone.")

        try {
            val out = contentResolver.openOutputStream(uri, "wt") ?: throw IOException("Cannot write to $uri.")
            out.use { it.write(data) }
        } finally {
            data.fill(0)
        }

        return "saved"
    }

    private fun photoSave(id: String, file: String, name: String): String {
        val data = Chats.vault(this).data(id, file) ?: throw IllegalArgumentException("That image is gone.")
        val values = ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, name)
            put(MediaStore.Images.Media.MIME_TYPE, Files.type(name))
            put(MediaStore.Images.Media.RELATIVE_PATH, "${Environment.DIRECTORY_PICTURES}/Vigil")
            put(MediaStore.Images.Media.IS_PENDING, 1)
        }

        try {
            val uri = contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                ?: throw IOException("Cannot add the image to the gallery.")

            try {
                val out = contentResolver.openOutputStream(uri) ?: throw IOException("Cannot write to $uri.")
                out.use { it.write(data) }
            } catch (e: IOException) {
                contentResolver.delete(uri, null, null)
                throw e
            }

            values.clear()
            values.put(MediaStore.Images.Media.IS_PENDING, 0)
            contentResolver.update(uri, values, null, null)
        } finally {
            data.fill(0)
        }

        return "saved"
    }

    private fun background(result: MethodChannel.Result, work: () -> Any?) {
        Thread {
            val reply: () -> Unit = try {
                val value = work();
                { result.success(value) }
            } catch (e: ChatVault.Locked) {
                { result.error("locked", e.message, null) }
            } catch (e: IllegalArgumentException) {
                { result.error("invalid", e.message, null) }
            } catch (e: Exception) {
                { result.error("failed", e.message ?: e.toString(), null) }
            }

            runOnUiThread(reply)
        }.start()
    }

    private fun chat(id: String): String {
        require(
            ChatVault.group(id) != null || (id.matches(Regex("[0-9a-f]{2}")) && id.toInt(16) in 1..0xfe)
        ) { "There is no chat $id." }

        return id
    }

    private fun granted(permission: String) =
        checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    private fun service(action: String) =
        Intent(this, VigilVpnService::class.java).setAction(action)

    private companion object {
        const val CHANNEL = "se.sanctorum.vigil/core"
        const val REQUEST_VPN = 0x5a
        const val REQUEST_NOTIFY = 0x5b
        const val REQUEST_MICROPHONE = 0x5c
        const val REQUEST_FILE = 0x5d
        const val REQUEST_SAVE = 0x5e
        const val REQUEST_VOICE = 0x5f
        const val REQUEST_SCAN = 0x60
        const val ERASE_WAIT_MS = 3000L
        const val DATA_MAX = 4 * 1024 * 1024
        const val MESSAGE_MAX = 512
        const val REPLY_MAX = MESSAGE_MAX - 9
        const val VIDEO_WIDTH = 960
        const val VIDEO_HEIGHT = 540
    }
}
