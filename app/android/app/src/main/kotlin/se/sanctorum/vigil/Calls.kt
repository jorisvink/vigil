package se.sanctorum.vigil

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Person
import android.content.ComponentName
import android.content.Intent
import android.graphics.drawable.Icon
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.RingtoneManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.telecom.CallAudioState
import android.telecom.Connection
import android.telecom.ConnectionRequest
import android.telecom.ConnectionService
import android.telecom.DisconnectCause
import android.telecom.PhoneAccount
import android.telecom.PhoneAccountHandle
import android.telecom.TelecomManager
import android.util.Log
import org.json.JSONObject

object Calls {
    enum class Phase { RINGING, DIALING, ALERTING, CONNECTING, ACTIVE }

    private var service: VigilVpnService? = null
    private var connection: CallConnection? = null
    private var proximity: PowerManager.WakeLock? = null

    var peer = 0
        private set
    private var incoming = false
    private var phase: Phase? = null
    private var muted = false
    private var speaker = false
    var video = false
        private set
    private var started = 0L
    private var sessions = 0L

    private val ignored = mutableSetOf<Int>()

    val busy: Boolean
        get() = phase != null

    fun attach(service: VigilVpnService) {
        this.service = service

        val notifications = service.getSystemService(NotificationManager::class.java)

        notifications.createNotificationChannel(
            NotificationChannel(CHANNEL_CALL, "Calls", NotificationManager.IMPORTANCE_LOW)
        )

        notifications.createNotificationChannel(
            NotificationChannel(
                CHANNEL_RING, "Incoming calls", NotificationManager.IMPORTANCE_HIGH
            ).apply {
                setSound(
                    RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE),
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                        .build()
                )
            }
        )

        try {
            telecom()?.registerPhoneAccount(
                PhoneAccount.builder(handle(service), "Vigil")
                    .setCapabilities(PhoneAccount.CAPABILITY_SELF_MANAGED)
                    .build()
            )
        } catch (e: Exception) {
            Log.w(TAG, "failed to register phone account", e)
        }
    }

    fun detach(service: VigilVpnService) {
        if (this.service !== service) return

        end(DisconnectCause.LOCAL)
        proximity()
        this.service = null
    }

    @Suppress("DEPRECATION")
    fun proximity() {
        val power = service?.getSystemService(PowerManager::class.java)
        val route = connection?.callAudioState?.route
        val earpiece = if (route != null) {
            route == CallAudioState.ROUTE_EARPIECE
        } else {
            !speaker
        }
        val wanted = busy && phase != Phase.RINGING && earpiece

        if (wanted && proximity == null && power != null &&
            power.isWakeLockLevelSupported(PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK)
        ) {
            proximity = power.newWakeLock(
                PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK, "vigil:proximity"
            ).apply {
                setReferenceCounted(false)
                acquire()
            }
        } else if (!wanted) {
            proximity?.release(PowerManager.RELEASE_FLAG_WAIT_FOR_NO_PROXIMITY)
            proximity = null
        }
    }

    fun json(): JSONObject? {
        val phase = phase ?: return null

        return JSONObject()
            .put("peer", hex(peer))
            .put("incoming", incoming)
            .put("state", phase.name.lowercase())
            .put("muted", muted)
            .put("speaker", speaker)
            .put("video", video)
            .put("front", Video.front)
            .put("since", if (phase == Phase.ACTIVE) started else 0L)
    }

    fun dial(peer: Int, video: Boolean) {
        val service = service ?: return
        if (busy || peer <= 0 || peer >= 0xff) return

        begin(peer, false, Phase.DIALING)
        camera(video)

        Vigil.callDial(peer)
        Vigil.voiceCall(peer, VOICE_CALL)

        try {
            telecom()?.placeCall(
                address(peer),
                Bundle().apply {
                    putParcelable(TelecomManager.EXTRA_PHONE_ACCOUNT_HANDLE, handle(service))
                    putBundle(
                        TelecomManager.EXTRA_OUTGOING_CALL_EXTRAS,
                        Bundle().apply { putInt(EXTRA_PEER, peer) }
                    )
                }
            )
        } catch (e: Exception) {
            Log.w(TAG, "telecom did not take our call", e)
        }

        service.callChanged()
    }

    fun answer(video: Boolean) {
        if (phase != Phase.RINGING) return

        phase = Phase.CONNECTING
        camera(video)
        Vigil.voiceAnswer()

        connection?.setActive()
        cancelRing()
        service?.callChanged()
    }

    fun decline() {
        if (phase != Phase.RINGING) return

        ignored.add(peer)
        Vigil.voiceHangup()

        end(DisconnectCause.REJECTED)
    }

    fun hangup() {
        if (!busy) return

        if (phase == Phase.RINGING) {
            decline()
            return
        }

        Vigil.voiceHangup()
        Vigil.callHangup()

        end(DisconnectCause.LOCAL)
    }

    fun camera(on: Boolean) {
        video = on
        Video.camera(on)

        if (on && !speaker) speaker(true)

        service?.callChanged()
    }

    fun mute(on: Boolean) {
        if (!busy) return

        muted = on
        Vigil.voiceMute(on)
        service?.callChanged()
    }

    @Suppress("DEPRECATION")
    fun speaker(on: Boolean) {
        if (!busy) return

        speaker = on

        val connection = connection
        if (connection != null) {
            connection.setAudioRoute(
                if (on) CallAudioState.ROUTE_SPEAKER else CallAudioState.ROUTE_WIRED_OR_EARPIECE
            )
        } else {
            service?.getSystemService(AudioManager::class.java)?.isSpeakerphoneOn = on
        }

        service?.callChanged()
    }

    fun update(status: JSONObject?, voice: JSONObject?) {
        val service = service ?: return
        val call = status?.optJSONObject("call")
        val callers = mutableSetOf<Int>()

        call?.optJSONArray("incoming")?.let { list ->
            for (i in 0 until list.length()) callers.add(list.getString(i).toInt(16))
        }

        ignored.retainAll(callers)

        if (!busy) {
            val caller = callers.firstOrNull { it !in ignored } ?: return
            ring(service, caller)
            return
        }

        if (status?.optBoolean("running") != true) {
            end(DisconnectCause.ERROR)
            return
        }

        val done = (voice?.optLong("calls") ?: 0L) > sessions
        val talking = voice?.optString("state") == "talking"

        if (phase == Phase.RINGING) {
            if (done) {
                end(DisconnectCause.MISSED)
            } else if (peer !in callers) {
                Vigil.voiceHangup()
                end(DisconnectCause.MISSED)
            }
            return
        }

        if (done) {
            end(
                when (voice?.optString("end")) {
                    "remote" -> DisconnectCause.REMOTE
                    "timeout" -> if (phase == Phase.ACTIVE) DisconnectCause.REMOTE
                        else DisconnectCause.BUSY
                    "local" -> DisconnectCause.LOCAL
                    else -> DisconnectCause.ERROR
                }
            )
            return
        }

        if (phase == Phase.DIALING && voice?.optString("state") == "online") {
            phase = Phase.ALERTING
            service.callChanged()
        }

        if (talking && phase != Phase.ACTIVE) {
            phase = Phase.ACTIVE
            started = System.currentTimeMillis()
            connection?.setActive()
            service.callChanged()
        }
    }

    fun notification(service: VigilVpnService): Notification? {
        if (!busy || phase == Phase.RINGING) return null

        val person = person(peer)
        val hangup = serviceIntent(service, VigilVpnService.ACTION_HANGUP, 1)

        val builder = Notification.Builder(service, CHANNEL_CALL)
            .setSmallIcon(R.drawable.ic_notification)
            .setOngoing(true)
            .setCategory(Notification.CATEGORY_CALL)
            .setContentTitle(person.name)
            .setContentText(
                when (phase) {
                    Phase.DIALING -> "Calling…"
                    Phase.ALERTING -> "Ringing…"
                    Phase.CONNECTING -> "Connecting…"
                    else -> if (muted) "Muted" else "In call"
                }
            )

        launch(service, VigilVpnService.ACTION_SHOW, 2)?.let { builder.setContentIntent(it) }

        if (phase == Phase.ACTIVE) {
            builder.setWhen(started).setUsesChronometer(true).setShowWhen(true)
        } else {
            builder.setShowWhen(false)
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            builder.setStyle(Notification.CallStyle.forOngoingCall(person, hangup))
        } else {
            builder.addAction(Notification.Action.Builder(null as Icon?, "Hang up", hangup).build())
        }

        return builder.build()
    }

    internal fun connected(connection: CallConnection) {
        if (!busy || connection.peer != peer) {
            connection.setDisconnected(DisconnectCause(DisconnectCause.CANCELED))
            connection.destroy()
            return
        }

        this.connection = connection

        when (phase) {
            Phase.RINGING -> connection.setRinging()
            Phase.DIALING, Phase.ALERTING -> connection.setDialing()
            else -> connection.setActive()
        }
    }

    internal fun showRing() {
        val service = service ?: return
        if (phase != Phase.RINGING) return

        val person = person(peer)
        val answer = launch(service, VigilVpnService.ACTION_ANSWER, 3) ?: return
        val decline = serviceIntent(service, VigilVpnService.ACTION_DECLINE, 4)

        val builder = Notification.Builder(service, CHANNEL_RING)
            .setSmallIcon(R.drawable.ic_notification)
            .setOngoing(true)
            .setCategory(Notification.CATEGORY_CALL)
            .setContentTitle(person.name)
            .setContentText("Incoming call")
            .setFullScreenIntent(launch(service, VigilVpnService.ACTION_SHOW, 5), true)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            builder.setStyle(Notification.CallStyle.forIncomingCall(person, decline, answer))
        } else {
            builder.addAction(Notification.Action.Builder(null as Icon?, "Decline", decline).build())
            builder.addAction(Notification.Action.Builder(null as Icon?, "Answer", answer).build())
        }

        val notification = builder.build()
        notification.flags = notification.flags or Notification.FLAG_INSISTENT

        service.getSystemService(NotificationManager::class.java).notify(NOTIFICATION_RING, notification)
    }

    private fun ring(service: VigilVpnService, caller: Int) {
        begin(caller, true, Phase.RINGING)
        Vigil.voiceCall(caller, VOICE_RING)

        val telecom = telecom()

        try {
            if (telecom == null) {
                showRing()
            } else {
                telecom.addNewIncomingCall(
                    handle(service),
                    Bundle().apply {
                        putParcelable(TelecomManager.EXTRA_INCOMING_CALL_ADDRESS, address(caller))
                        putBundle(
                            TelecomManager.EXTRA_INCOMING_CALL_EXTRAS,
                            Bundle().apply { putInt(EXTRA_PEER, caller) }
                        )
                    }
                )
            }
        } catch (e: Exception) {
            Log.w(TAG, "telecom did not take the incoming call", e)
            showRing()
        }

        service.callChanged()
    }

    private fun begin(peer: Int, incoming: Boolean, phase: Phase) {
        this.peer = peer
        this.incoming = incoming
        this.phase = phase
        this.muted = false
        this.speaker = false
        this.started = 0L
        this.sessions = voiceSessions()
    }

    private fun end(cause: Int) {
        if (!busy) return

        ignored.add(peer)
        phase = null
        muted = false
        speaker = false
        video = false
        Video.camera(false)

        connection?.let {
            it.setDisconnected(DisconnectCause(cause))
            it.destroy()
        }

        connection = null

        cancelRing()
        service?.callChanged()
    }

    private fun cancelRing() {
        service?.getSystemService(NotificationManager::class.java)?.cancel(NOTIFICATION_RING)
    }

    private fun voiceSessions(): Long =
        try {
            JSONObject(Vigil.voiceStatus()).optLong("calls")
        } catch (e: Exception) {
            0L
        }

    private fun telecom(): TelecomManager? =
        service?.getSystemService(TelecomManager::class.java)

    private fun person(peer: Int): Person =
        Person.Builder().setName(name(peer)).setImportant(true).build()

    private fun launch(service: VigilVpnService, action: String, code: Int): PendingIntent? =
        service.packageManager.getLaunchIntentForPackage(service.packageName)?.let {
            PendingIntent.getActivity(
                service, code, it.setAction(action).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            )
        }

    private fun serviceIntent(service: VigilVpnService, action: String, code: Int): PendingIntent =
        PendingIntent.getService(
            service, code, Intent(service, VigilVpnService::class.java).setAction(action),
            PendingIntent.FLAG_IMMUTABLE
        )

    private fun handle(service: VigilVpnService) =
        PhoneAccountHandle(ComponentName(service, VigilConnectionService::class.java), "vigil")

    private fun address(peer: Int): Uri = Uri.fromParts(PhoneAccount.SCHEME_SIP, hex(peer), null)

    fun hex(peer: Int) = "%02x".format(peer)

    fun name(peer: Int): String =
        service?.let { PeerNames.name(it, peer) } ?: "Peer ${hex(peer)}"

    private const val TAG = "vigil"
    private const val CHANNEL_CALL = "call"
    private const val CHANNEL_RING = "ring"
    private const val NOTIFICATION_RING = 2
    private const val VOICE_CALL = 1
    private const val VOICE_RING = 4
    const val EXTRA_PEER = "peer"
}

class VigilConnectionService : ConnectionService() {
    override fun onCreateOutgoingConnection(
        account: PhoneAccountHandle?,
        request: ConnectionRequest
    ): Connection = connection(request)

    override fun onCreateIncomingConnection(
        account: PhoneAccountHandle?,
        request: ConnectionRequest
    ): Connection = connection(request)

    override fun onCreateIncomingConnectionFailed(
        account: PhoneAccountHandle?,
        request: ConnectionRequest?
    ) {
        Calls.showRing()
    }

    private fun connection(request: ConnectionRequest): Connection {
        Log.i("vigil", "telecom connection ${request.extras?.keySet()}")

        val peer = request.extras?.getInt(Calls.EXTRA_PEER, 0) ?: 0

        return CallConnection(if (peer != 0) peer else Calls.peer).also { Calls.connected(it) }
    }
}

class CallConnection(val peer: Int) : Connection() {
    init {
        connectionProperties = PROPERTY_SELF_MANAGED
        connectionCapabilities = CAPABILITY_MUTE
        audioModeIsVoip = true

        val name = Calls.name(peer)
        setAddress(Uri.fromParts(PhoneAccount.SCHEME_SIP, Calls.hex(peer), null),
            TelecomManager.PRESENTATION_ALLOWED)
        setCallerDisplayName(name, TelecomManager.PRESENTATION_ALLOWED)
    }

    override fun onShowIncomingCallUi() = Calls.showRing()

    @Deprecated("onCallEndpointChanged needs api 34")
    override fun onCallAudioStateChanged(state: CallAudioState?) = Calls.proximity()

    override fun onAnswer() = Calls.answer(false)

    override fun onReject() = Calls.decline()

    override fun onDisconnect() = Calls.hangup()

    override fun onAbort() = Calls.hangup()
}
