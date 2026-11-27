package se.sanctorum.vigil

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.net.VpnService
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.system.OsConstants
import android.util.Log
import android.view.KeyEvent
import org.json.JSONObject

/**
 * Builds the tunnel interface from the summary of the configuration
 * and runs the core on its own thread until we stop it.
 */
class VigilVpnService : VpnService() {
    private val handler = Handler(Looper.getMainLooper())
    private val refresh = Runnable { refresh() }
    private var summary: JSONObject? = null
    private var shown: String? = null
    private var buttons: MediaSession? = null
    private var voiced = false

    override fun onCreate() {
        super.onCreate()

        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL, "Tunnel", NotificationManager.IMPORTANCE_LOW)
        )

        Video.context = applicationContext
        Calls.attach(this)
        Chats.attach(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val store = TunnelStore(this)

        when (intent?.action) {
            ACTION_CALL, ACTION_ANSWER, ACTION_DECLINE, ACTION_HANGUP, ACTION_MUTE,
            ACTION_SPEAKER, ACTION_CAMERA -> {
                if (active == null) {
                    idle()
                    return START_NOT_STICKY
                }

                when (intent.action) {
                    ACTION_CALL -> Calls.dial(
                        intent.getStringExtra(EXTRA_PEER) ?: "", intent.getBooleanExtra(EXTRA_VIDEO, false)
                    )
                    ACTION_ANSWER -> Calls.answer(intent.getBooleanExtra(EXTRA_VIDEO, false))
                    ACTION_CAMERA -> Calls.camera(intent.getBooleanExtra(EXTRA_ON, false))
                    ACTION_DECLINE -> Calls.decline()
                    ACTION_HANGUP -> Calls.hangup()
                    ACTION_MUTE -> Calls.mute(intent.getBooleanExtra(EXTRA_ON, false))
                    ACTION_SPEAKER -> Calls.speaker(intent.getBooleanExtra(EXTRA_ON, false))
                }

                return START_STICKY
            }
            ACTION_TALK -> {
                if (active == null) {
                    idle()
                    return START_NOT_STICKY
                }

                talk(intent.getBooleanExtra(EXTRA_ON, false))

                return START_STICKY
            }
            ACTION_VOICE -> {
                if (active == null) {
                    idle()
                    return START_NOT_STICKY
                }

                val chat = intent.getStringExtra(EXTRA_PEER)

                try {
                    if (chat != null) {
                        GroupVoice.on(this, chat)
                    } else {
                        talk(false)
                        GroupVoice.off(this)
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "failed to change our group voice", e)
                }

                voiceChanged()

                return START_STICKY
            }
            ACTION_DISCONNECT -> {
                val voice = store.voice()

                if (store.gospel && voice != null && active != null && active != voice) {
                    connect(store, voice)
                    return START_STICKY
                }

                disconnect()
                idle()
                return START_NOT_STICKY
            }
            ACTION_GOSPEL -> {
                store.gospel = intent.getBooleanExtra(EXTRA_ON, true)
                return gospel(store)
            }
            ACTION_CONNECT -> intent.getStringExtra(EXTRA_ID)?.let { connect(store, it) }
            else -> store.last?.let { connect(store, it) } ?: idle()
        }

        return START_STICKY
    }

    override fun onRevoke() {
        disconnect()
        idle()
    }

    override fun onDestroy() {
        handler.removeCallbacks(refresh)
        buttons?.release()
        buttons = null
        Calls.detach(this)
        Chats.detach(this)
        disconnect()
        super.onDestroy()
    }

    private fun talk(on: Boolean) {
        GroupVoice.talk(on && !Calls.busy) { callChanged() }
    }

    private fun voiceChanged() {
        voiced = GroupVoice.active
        callChanged()

        if (voiced && buttons == null) {
            buttons = MediaSession(this, "vigil-voice").apply {
                setCallback(object : MediaSession.Callback() {
                    override fun onMediaButtonEvent(intent: Intent): Boolean = press(intent)
                })
                setPlaybackState(
                    PlaybackState.Builder()
                        .setState(PlaybackState.STATE_PLAYING, 0, 1f)
                        .setActions(PlaybackState.ACTION_PLAY_PAUSE)
                        .build()
                )
                isActive = true
            }
        }

        if (!voiced) {
            buttons?.release()
            buttons = null
        }
    }

    @Suppress("DEPRECATION")
    private fun press(intent: Intent): Boolean {
        val event = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(Intent.EXTRA_KEY_EVENT, KeyEvent::class.java)
        } else {
            intent.getParcelableExtra(Intent.EXTRA_KEY_EVENT)
        } ?: return false

        if (event.keyCode !in TALK_KEYS) {
            return false
        }

        if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
            talk(true)
        }

        if (event.action == KeyEvent.ACTION_UP) {
            talk(false)
        }

        return true
    }

    fun callChanged() {
        Calls.proximity()

        val summary = summary ?: return

        val call = Calls.notification(this)
        if (call != null) {
            startForeground(call, true)
            return
        }

        shown = null
        startForeground(notification(summary, state(status())), false)
    }

    private fun connect(store: TunnelStore, id: String) {
        val previous = disconnect()

        val config: ByteArray
        val summary: JSONObject
        var voice: ByteArray? = null

        try {
            summary = store.summary(id)
            config = store.load(id)
        } catch (e: Exception) {
            Log.e(TAG, "failed to load tunnel $id", e)
            idle()
            return
        }

        val alone = voiceOnly(summary)

        if (store.gospel) {
            try {
                voice = store.gospelSet()
            } catch (e: Exception) {
                Log.e(TAG, "failed to load the Gospel configurations", e)
            }
        }

        val fd = try {
            (if (alone) placeholder(summary) else build(summary)).establish()
        } catch (e: Exception) {
            Log.e(TAG, "failed to establish tunnel interface", e)
            null
        }

        if (fd == null) {
            config.fill(0)
            voice?.fill(0)
            idle()
            return
        }

        split = if (alone || full(summary)) null else this

        if (lockdown) {
            Log.w(TAG, "block connections without vpn is on, only the tunnel routes work")
        }

        store.last = id

        synchronized(lock) {
            store.companionApply()
            Chats.safetyApplyAll(this)

            val number = Vigil.runId()

            val worker = Thread({
                // Only one run at a time, the previous one is stopping.
                previous?.join()

                val ret = Vigil.run(
                    this, if (alone) -1 else fd.fd, config, voice, noBackupFilesDir.absolutePath,
                    number
                )
                Log.i(TAG, "vigil exited with $ret")

                config.fill(0)
                voice?.fill(0)
                fd.close()

                synchronized(lock) {
                    if (thread === Thread.currentThread()) {
                        thread = null
                        active = null
                        split = null
                        idle()
                    }
                }
            }, "vigil")

            thread = worker
            run = number
            active = id
            worker.start()
        }

        foreground(summary)
    }

    private fun gospel(store: TunnelStore): Int {
        val voice = store.voice()
        val running = active

        if (running == null) {
            if (store.gospel && voice != null) {
                connect(store, voice)
                return START_STICKY
            }

            idle()
            return START_NOT_STICKY
        }

        if (summary?.let { voiceOnly(it) } == true) {
            if (!store.gospel || voice == null) {
                disconnect()
                idle()
                return START_NOT_STICKY
            }

            if (running != voice) {
                connect(store, voice)
                return START_STICKY
            }
        }

        val config = if (store.gospel && voice != null) {
            try {
                store.gospelSet()
            } catch (e: Exception) {
                Log.e(TAG, "failed to load the Gospel configurations", e)
                return START_STICKY
            }
        } else {
            null
        }

        store.companionApply()

        if (!Vigil.gospel(config)) {
            Log.w(TAG, "Gospel was not attached, vigil is not running")
        }

        config?.fill(0)

        return START_STICKY
    }

    private fun idle() {
        handler.removeCallbacks(refresh)
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun foreground(summary: JSONObject) {
        this.summary = summary
        shown = state(status())

        startForeground(notification(summary, shown!!), false)

        handler.removeCallbacks(refresh)
        handler.postDelayed(refresh, REFRESH_MS)
    }

    private fun startForeground(notification: Notification, call: Boolean, voice: Boolean = GroupVoice.active) {
        var type = 0

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            type = ServiceInfo.FOREGROUND_SERVICE_TYPE_SYSTEM_EXEMPTED
        }

        if (call && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_PHONE_CALL
        }

        if ((call || voice) && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        }

        if (call && Calls.video && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
        }

        try {
            if (type != 0) {
                startForeground(NOTIFICATION, notification, type)
            } else {
                startForeground(NOTIFICATION, notification)
            }
        } catch (e: Exception) {
            Log.w(TAG, "failed to start as foreground service ($type)", e)

            if (call || voice) {
                startForeground(notification, false, false)
            }
        }
    }

    private fun refresh() {
        val summary = summary ?: return
        if (active == null) {
            return
        }

        val status = status()
        Calls.update(status, voice())
        Chats.poll(this, status)
        GroupVoice.poll(this)

        if (GroupVoice.active != voiced) {
            voiceChanged()
        }

        if (Calls.notification(this) == null) {
            val text = state(status)
            if (text != shown) {
                shown = text
                getSystemService(NotificationManager::class.java)
                    .notify(NOTIFICATION, notification(summary, text))
            }
        }

        handler.postDelayed(refresh, REFRESH_MS)
    }

    private fun status(): JSONObject? =
        try {
            JSONObject(Vigil.status())
        } catch (e: Exception) {
            null
        }

    private fun voice(): JSONObject? =
        try {
            JSONObject(Vigil.voiceStatus())
        } catch (e: Exception) {
            null
        }

    private fun state(status: JSONObject?): String {
        if (status?.optBoolean("running") != true) {
            return "Connecting…"
        }

        if (summary?.let { voiceOnly(it) } == true) {
            return if (status.optJSONObject("call")?.optBoolean("alive") == true) {
                "Active"
            } else {
                "Waiting for the cathedral…"
            }
        }

        val peers = status.optJSONArray("peers")
        val online = peers != null &&
            (0 until peers.length()).any { peers.getJSONObject(it).optBoolean("online") }

        return if (online) "Online · ${summary?.getString("address")}" else "Negotiating keys…"
    }

    private fun notification(summary: JSONObject, text: String): Notification {
        val disconnect = PendingIntent.getService(
            this, 0, Intent(this, VigilVpnService::class.java).setAction(ACTION_DISCONNECT),
            PendingIntent.FLAG_IMMUTABLE
        )

        val builder = Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(summary.getString("instance"))
            .setContentText(text)
            .setSubText(tag(summary))
            .setOngoing(true)
            .setShowWhen(false)
            .addAction(Notification.Action.Builder(null as Icon?, "Disconnect", disconnect).build())

        packageManager.getLaunchIntentForPackage(packageName)?.let {
            builder.setContentIntent(
                PendingIntent.getActivity(this, 0, it, PendingIntent.FLAG_IMMUTABLE)
            )
        }

        return builder.build()
    }

    private fun tag(summary: JSONObject): String {
        val mode = when (summary.optString("mode")) {
            "cathedral" -> "Cathedral"
            "liturgy" -> if (summary.optBoolean("tier6")) "Tier6" else "Liturgy"
            "gospel" -> "Gospel"
            else -> "Direct"
        }

        return if (summary.optBoolean("shroud")) "$mode · Shroud" else mode
    }

    private fun build(summary: JSONObject): Builder {
        val builder = Builder()
            .setSession(summary.getString("instance"))
            .setMtu(summary.getInt("mtu"))
            .setBlocking(false)

        val (address, prefix) = cidr(summary.getString("address"))
        builder.addAddress(address, prefix)

        val routes = summary.getJSONArray("routes")
        for (i in 0 until routes.length()) {
            val (net, bits) = cidr(routes.getString(i))
            builder.addRoute(net, bits)
        }

        val dns = summary.getJSONArray("dns")
        for (i in 0 until dns.length()) {
            builder.addDnsServer(dns.getString(i))
        }

        if (full(summary) && summary.optBoolean("block_ipv6", true)) {
            builder.addAddress(IPV6_SINK, 128)
            builder.addRoute("::", 0)
        } else {
            builder.allowFamily(OsConstants.AF_INET6)
        }

        return configure(builder)
    }

    private fun placeholder(summary: JSONObject): Builder {
        val builder = Builder()
            .setSession(summary.getString("instance"))
            .setBlocking(false)
            .addAddress(GOSPEL_ADDRESS, 32)
            .addAllowedApplication(packageName)
            .allowFamily(OsConstants.AF_INET6)

        return configure(builder)
    }

    private fun configure(builder: Builder): Builder {
        packageManager.getLaunchIntentForPackage(packageName)?.let {
            builder.setConfigureIntent(
                PendingIntent.getActivity(this, 0, it, PendingIntent.FLAG_IMMUTABLE)
            )
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            builder.setMetered(false)
        }

        return builder
    }

    private fun voiceOnly(summary: JSONObject) = summary.optString("mode") == "gospel"

    private fun full(summary: JSONObject): Boolean {
        val routes = summary.getJSONArray("routes")
        return (0 until routes.length()).any { cidr(routes.getString(it)).second == 0 }
    }

    private fun cidr(value: String): Pair<String, Int> {
        val parts = value.split("/")
        return parts[0] to parts[1].toInt()
    }

    companion object {
        const val ACTION_CONNECT = "se.sanctorum.vigil.CONNECT"
        const val ACTION_DISCONNECT = "se.sanctorum.vigil.DISCONNECT"
        const val ACTION_CALL = "se.sanctorum.vigil.CALL"
        const val ACTION_ANSWER = "se.sanctorum.vigil.ANSWER"
        const val ACTION_DECLINE = "se.sanctorum.vigil.DECLINE"
        const val ACTION_HANGUP = "se.sanctorum.vigil.HANGUP"
        const val ACTION_MUTE = "se.sanctorum.vigil.MUTE"
        const val ACTION_SPEAKER = "se.sanctorum.vigil.SPEAKER"
        const val ACTION_CAMERA = "se.sanctorum.vigil.CAMERA"
        const val ACTION_SHOW = "se.sanctorum.vigil.SHOW"
        const val ACTION_GOSPEL = "se.sanctorum.vigil.GOSPEL"
        const val ACTION_TALK = "se.sanctorum.vigil.TALK"
        const val ACTION_VOICE = "se.sanctorum.vigil.VOICE"
        const val EXTRA_ID = "id"
        const val EXTRA_PEER = "peer"
        const val EXTRA_ON = "on"
        const val EXTRA_VIDEO = "video"
        const val NOTIFIED = "se.sanctorum.vigil.Notified"

        private const val TAG = "vigil"
        private const val CHANNEL = "tunnel"
        private const val NOTIFICATION = 1
        private const val REFRESH_MS = 1000L
        private val TALK_KEYS = setOf(
            KeyEvent.KEYCODE_HEADSETHOOK, KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE,
            KeyEvent.KEYCODE_MEDIA_PLAY, KeyEvent.KEYCODE_MEDIA_PAUSE
        )
        private const val IPV6_SINK = "fd70:6172:6973:6800::1"
        private const val GOSPEL_ADDRESS = "169.254.86.1"

        private val lock = Any()
        private var thread: Thread? = null
        private var run = 0L

        @Volatile
        var active: String? = null
            private set

        fun disconnect(): Thread? {
            synchronized(lock) {
                val worker = thread ?: return null

                thread = null
                active = null
                split = null
                Vigil.stop(run)

                return worker
            }
        }

        fun notified(context: Context, action: String): Intent =
            Intent(action).setClassName(context, NOTIFIED)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)

        @Volatile
        private var split: VigilVpnService? = null

        val lockdown: Boolean
            get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q &&
                split?.isLockdownEnabled == true
    }
}
