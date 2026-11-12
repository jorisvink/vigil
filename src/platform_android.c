/*
 * Copyright (c) 2026 Joris Vink <joris@sanctorum.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <android/log.h>

#include <jni.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

#include "vigil.h"

#include <libkyrka/libnyfe.h>

/* The Kotlin class that holds our native methods. */
#define ANDROID_VIGIL_CLASS	"se/sanctorum/vigil/Vigil"

/* The tag we log under. */
#define ANDROID_LOG_TAG		"vigil"

static jstring		android_status(JNIEnv *, jclass);
static jlong		android_run_id(JNIEnv *, jclass);
static jstring		android_version(JNIEnv *, jclass);
static void		android_stop(JNIEnv *, jclass, jlong);
static jstring		android_kyrka_version(JNIEnv *, jclass);
static jstring		android_string_new(JNIEnv *, const char *);
static jstring		android_check(JNIEnv *, jclass, jbyteArray);
static jboolean		android_gospel(JNIEnv *, jclass, jbyteArray);
static jbyteArray	android_edit(JNIEnv *, jclass, jbyteArray, jstring);
static jint		android_run(JNIEnv *, jclass, jobject, jint, jbyteArray,
			    jbyteArray, jstring, jlong);

static void	android_bytes_free(char *, size_t);
static void	android_bytes_wipe(JNIEnv *, jbyteArray, size_t);
static char	*android_bytes_copy(JNIEnv *, jbyteArray, size_t, size_t *);

static void	android_call_hangup(JNIEnv *, jclass);
static void	android_call_dial(JNIEnv *, jclass, jint);

static void	android_voice_hangup(JNIEnv *, jclass);
static void	android_voice_answer(JNIEnv *, jclass);
static jstring	android_voice_status(JNIEnv *, jclass);
static void	android_voice_mute(JNIEnv *, jclass, jboolean);
static void	android_voice_call(JNIEnv *, jclass, jint, jint);

static jint		android_remote_run(JNIEnv *, jclass);
static void		android_remote_stop(JNIEnv *, jclass);
static jbyteArray	android_remote_config(JNIEnv *, jclass);
static jstring		android_remote_word(JNIEnv *, jclass, jint);
static jstring		android_remote_words(JNIEnv *, jclass, jbyteArray);
static jboolean		android_remote_start(JNIEnv *, jclass, jstring,
			    jstring);

static jbyteArray	android_chat_events(JNIEnv *, jclass);
static jbyteArray	android_chat_data_take(JNIEnv *, jclass, jlong, jint);
static jboolean		android_chat_forget(JNIEnv *, jclass, jint);
static jboolean		android_chat_drop(JNIEnv *, jclass, jint, jlong);
static jboolean		android_chat_stored(JNIEnv *, jclass, jlong);
static jboolean		android_chat_nick(JNIEnv *, jclass, jstring);
static jboolean		android_chat_avatar(JNIEnv *, jclass, jbyteArray);
static void		android_chat_open(JNIEnv *, jclass, jint, jboolean);
static void		android_chat_typing(JNIEnv *, jclass, jint, jlong,
			    jboolean);
static void		android_chat_group_typing(JNIEnv *, jclass, jint,
			    jlong, jlong, jbyteArray);
static jboolean		android_chat_group_leave(JNIEnv *, jclass, jint, jlong,
			    jlong);
static jboolean		android_chat_send(JNIEnv *, jclass, jint, jlong,
			    jbyteArray, jint, jlong, jlong);
static jboolean		android_chat_topic(JNIEnv *, jclass, jint, jlong,
			    jlong, jint, jbyteArray);
static jboolean		android_chat_group_send(JNIEnv *, jclass, jint, jlong,
			    jint, jlong, jint, jlong, jbyteArray, jint, jlong);
static jboolean		android_chat_group_topic(JNIEnv *, jclass, jint, jlong,
			    jlong, jint, jlong, jint, jbyteArray);
static jboolean		android_chat_group_roster(JNIEnv *, jclass, jint,
			    jlong, jlong, jint, jboolean, jbyteArray,
			    jbyteArray);
static jboolean		android_chat_data_send(JNIEnv *, jclass, jint, jlong,
			    jint, jbyteArray, jbyteArray, jlong);
static jboolean		android_chat_group_data_send(JNIEnv *, jclass, jint,
			    jlong, jint, jlong, jint, jlong, jint, jbyteArray,
			    jbyteArray);
static jboolean		android_chat_data_queue(JNIEnv *, jint, jlong, jint,
			    jlong, jint, jint, jbyteArray, jbyteArray, jlong);

/* The native methods we register with the Vigil class. */
static JNINativeMethod	methods[] = {
	{ "run",
	    "(Landroid/net/VpnService;I[B[BLjava/lang/String;J)I",
	    (void *)android_run },
	{ "stop", "(J)V", (void *)android_stop },
	{ "runId", "()J", (void *)android_run_id },
	{ "check", "([B)Ljava/lang/String;", (void *)android_check },
	{ "edit", "([BLjava/lang/String;)[B", (void *)android_edit },
	{ "status", "()Ljava/lang/String;", (void *)android_status },
	{ "version", "()Ljava/lang/String;", (void *)android_version },
	{ "kyrkaVersion", "()Ljava/lang/String;",
	    (void *)android_kyrka_version },
	{ "callDial", "(I)V", (void *)android_call_dial },
	{ "callHangup", "()V", (void *)android_call_hangup },
	{ "voiceCall", "(II)V", (void *)android_voice_call },
	{ "voiceHangup", "()V", (void *)android_voice_hangup },
	{ "voiceAnswer", "()V", (void *)android_voice_answer },
	{ "voiceMute", "(Z)V", (void *)android_voice_mute },
	{ "voiceStatus", "()Ljava/lang/String;",
	    (void *)android_voice_status },
	{ "chatSend", "(IJ[BIJJ)Z", (void *)android_chat_send },
	{ "chatTopic", "(IJJI[B)Z", (void *)android_chat_topic },
	{ "chatForget", "(I)Z", (void *)android_chat_forget },
	{ "chatDrop", "(IJ)Z", (void *)android_chat_drop },
	{ "chatStored", "(J)Z", (void *)android_chat_stored },
	{ "chatOpen", "(IZ)V", (void *)android_chat_open },
	{ "chatTyping", "(IJZ)V", (void *)android_chat_typing },
	{ "chatNick", "(Ljava/lang/String;)Z", (void *)android_chat_nick },
	{ "chatAvatar", "([B)Z", (void *)android_chat_avatar },
	{ "chatGroupTyping", "(IJJ[B)V", (void *)android_chat_group_typing },
	{ "chatGroupSend", "(IJIJIJ[BIJ)Z", (void *)android_chat_group_send },
	{ "chatGroupTopic", "(IJJIJI[B)Z", (void *)android_chat_group_topic },
	{ "chatGroupRoster", "(IJJIZ[B[B)Z",
	    (void *)android_chat_group_roster },
	{ "chatGroupLeave", "(IJJ)Z", (void *)android_chat_group_leave },
	{ "chatEvents", "()[B", (void *)android_chat_events },
	{ "chatDataSend", "(IJI[B[BJ)Z", (void *)android_chat_data_send },
	{ "chatGroupDataSend", "(IJIJIJI[B[B)Z",
	    (void *)android_chat_group_data_send },
	{ "chatDataTake", "(JI)[B", (void *)android_chat_data_take },
	{ "gospel", "([B)Z", (void *)android_gospel },
	{ "remoteWord", "(I)Ljava/lang/String;", (void *)android_remote_word },
	{ "remoteWords", "([B)Ljava/lang/String;",
	    (void *)android_remote_words },
	{ "remoteStart", "(Ljava/lang/String;Ljava/lang/String;)Z",
	    (void *)android_remote_start },
	{ "remoteRun", "()I", (void *)android_remote_run },
	{ "remoteConfig", "()[B", (void *)android_remote_config },
	{ "remoteStop", "()V", (void *)android_remote_stop },
};

/*
 * The JNIEnv and VpnService for the thread that is running vigil_run(),
 * only valid and only used on that thread.
 */
static JNIEnv		*run_env = NULL;
static jobject		run_service = NULL;
static jmethodID	run_protect = NULL;

/*
 * Called when our library is loaded, registers our native methods.
 */
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved)
{
	jclass		cls;
	JNIEnv		*env;

	if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
		return (JNI_ERR);

	cls = (*env)->FindClass(env, ANDROID_VIGIL_CLASS);
	if (cls == NULL)
		return (JNI_ERR);

	if ((*env)->RegisterNatives(env, cls, methods,
	    sizeof(methods) / sizeof(methods[0])) != 0)
		return (JNI_ERR);

	(*env)->DeleteLocalRef(env, cls);

	if (vigil_android_video_load(vm, env) == -1)
		return (JNI_ERR);

	return (JNI_VERSION_1_6);
}

/*
 * Log a message to logcat.
 */
void
vigil_platform_log(int prio, const char *msg)
{
	int		level;

	PRECOND(msg != NULL);

	switch (prio) {
	case LOG_ERR:
		level = ANDROID_LOG_ERROR;
		break;
	case LOG_NOTICE:
		level = ANDROID_LOG_WARN;
		break;
	default:
		level = ANDROID_LOG_INFO;
		break;
	}

	__android_log_print(level, ANDROID_LOG_TAG, "%s", msg);
}

/*
 * Protect the given socket so its traffic bypasses our tunnel,
 * returns -1 if that was not possible.
 */
int
vigil_platform_socket(int fd)
{
	jboolean	ok;

	PRECOND(fd >= 0);
	PRECOND(run_env != NULL);
	PRECOND(run_service != NULL);

	ok = (*run_env)->CallBooleanMethod(run_env,
	    run_service, run_protect, (jint)fd);

	if ((*run_env)->ExceptionCheck(run_env)) {
		(*run_env)->ExceptionClear(run_env);
		ok = JNI_FALSE;
	}

	if (ok != JNI_TRUE) {
		vigil_log(LOG_ERR, "failed to protect socket %d", fd);
		return (-1);
	}

	return (0);
}

/*
 * A protected socket needs nothing when it is closed.
 */
void
vigil_platform_socket_close(int fd)
{
	PRECOND(fd >= 0);
}

/*
 * Read a single packet from the tunnel interface.
 */
ssize_t
vigil_platform_tun_read(int fd, u_int8_t *buf, size_t len)
{
	PRECOND(fd >= 0);
	PRECOND(buf != NULL);
	PRECOND(len > 0);

	return (read(fd, buf, len));
}

/*
 * Write a single packet to the tunnel interface.
 */
ssize_t
vigil_platform_tun_write(int fd, const u_int8_t *buf, size_t len)
{
	PRECOND(fd >= 0);
	PRECOND(buf != NULL);
	PRECOND(len > 0);

	return (write(fd, buf, len));
}

/*
 * Give up hard, the keys have already been erased at this point.
 */
void
vigil_platform_fatal(void)
{
	abort();
}

/*
 * Vigil.run(), runs the tunnel and calls via the Gospel configuration if
 * one is given on the calling thread until stopped. The configurations
 * are wiped from the given arrays once we copied them.
 */
static jint
android_run(JNIEnv *env, jclass cls, jobject service, jint fd,
    jbyteArray config, jbyteArray gospel, jstring datadir, jlong id)
{
	jint		ret;
	jclass		vpn;
	const char	*dir;
	size_t		len, glen;
	char		*cfg, *gcfg;

	if (service == NULL)
		return (-1);

	if (config == NULL)
		return (-1);

	if (fd < -1)
		return (-1);

	if (id <= 0)
		return (-1);

	vpn = (*env)->GetObjectClass(env, service);
	if (vpn == NULL)
		return (-1);

	run_protect = (*env)->GetMethodID(env, vpn, "protect", "(I)Z");
	(*env)->DeleteLocalRef(env, vpn);

	if (run_protect == NULL)
		return (-1);

	cfg = android_bytes_copy(env, config, VIGIL_CONFIG_MAX, &len);
	if (cfg == NULL)
		return (-1);

	android_bytes_wipe(env, config, len);

	glen = 0;
	gcfg = NULL;

	if (gospel != NULL)
		gcfg = android_bytes_copy(env, gospel, VIGIL_CONFIG_MAX, &glen);

	if (gcfg != NULL)
		android_bytes_wipe(env, gospel, glen);

	dir = NULL;

	if (datadir != NULL) {
		dir = (*env)->GetStringUTFChars(env, datadir, NULL);
		if (dir == NULL) {
			android_bytes_free(cfg, len);
			if (gcfg != NULL)
				android_bytes_free(gcfg, glen);
			return (-1);
		}
	}

	run_env = env;
	run_service = service;

	ret = vigil_run((u_int64_t)id, fd, cfg, len, gcfg, glen, dir);

	run_env = NULL;
	run_service = NULL;

	android_bytes_free(cfg, len);

	if (gcfg != NULL)
		android_bytes_free(gcfg, glen);

	if (dir != NULL)
		(*env)->ReleaseStringUTFChars(env, datadir, dir);

	return (ret);
}

/*
 * Vigil.stop(), requests the run with the given id to stop.
 */
static void
android_stop(JNIEnv *env, jclass cls, jlong id)
{
	if (id > 0)
		vigil_stop((u_int64_t)id);
}

/*
 * Vigil.runId(), returns the id for the next Vigil.run().
 */
static jlong
android_run_id(JNIEnv *env, jclass cls)
{
	return ((jlong)vigil_run_id());
}

/*
 * Vigil.check(), validates the given configuration and returns its
 * json summary, or a json object holding the error.
 */
static jstring
android_check(JNIEnv *env, jclass cls, jbyteArray config)
{
	size_t		len;
	char		*cfg, out[4096];

	if (config == NULL)
		return (android_string_new(env, "{\"error\":\"no data\"}"));

	cfg = android_bytes_copy(env, config, VIGIL_CONFIG_MAX, &len);
	if (cfg == NULL) {
		return (android_string_new(env,
		    "{\"error\":\"configuration has an invalid size\"}"));
	}

	if (vigil_config_check(cfg, len, out, sizeof(out)) == -1) {
		android_bytes_free(cfg, len);
		return (android_string_new(env, out));
	}

	android_bytes_free(cfg, len);

	return (android_string_new(env, out));
}

/*
 * Vigil.edit(), returns the configuration with the edits applied or
 * throws an IllegalArgumentException with the reason it is invalid.
 */
static jbyteArray
android_edit(JNIEnv *env, jclass cls, jbyteArray config, jstring edits)
{
	const char	*p;
	jclass		exc;
	jbyteArray	ret;
	const char	*ed;
	char		*cfg;
	int		valid;
	size_t		len, outlen;
	char		out[VIGIL_CONFIG_MAX + 1], err[128];

	if (config == NULL || edits == NULL) {
		(void)snprintf(err, sizeof(err), "no data");
		goto fail;
	}

	cfg = android_bytes_copy(env, config, VIGIL_CONFIG_MAX, &len);
	if (cfg == NULL) {
		(void)snprintf(err, sizeof(err),
		    "configuration has an invalid size");
		goto fail;
	}

	ed = (*env)->GetStringUTFChars(env, edits, NULL);
	if (ed == NULL) {
		android_bytes_free(cfg, len);
		return (NULL);
	}

	valid = vigil_config_edit(cfg, len, ed, strlen(ed),
	    out, sizeof(out), err, sizeof(err));

	(*env)->ReleaseStringUTFChars(env, edits, ed);
	android_bytes_free(cfg, len);

	if (valid == -1)
		goto fail;

	outlen = strlen(out);

	ret = (*env)->NewByteArray(env, outlen);
	if (ret != NULL) {
		(*env)->SetByteArrayRegion(env, ret, 0, outlen,
		    (const jbyte *)out);
	}

	nyfe_mem_zero(out, sizeof(out));

	return (ret);

fail:
	for (p = err; *p != '\0'; p++) {
		if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e)
			break;
	}

	if (*p != '\0')
		(void)snprintf(err, sizeof(err), "invalid configuration");

	exc = (*env)->FindClass(env,
	    "java/lang/IllegalArgumentException");
	if (exc != NULL)
		(void)(*env)->ThrowNew(env, exc, err);

	return (NULL);
}

/*
 * Vigil.status(), returns the current status as json.
 */
static jstring
android_status(JNIEnv *env, jclass cls)
{
	jstring		ret;
	char		*buf;

	buf = calloc(1, VIGIL_STATUS_MAX);
	if (buf == NULL)
		return (NULL);

	if (vigil_status(buf, VIGIL_STATUS_MAX) == 0)
		(void)snprintf(buf, VIGIL_STATUS_MAX, "{\"running\":false}");

	ret = android_string_new(env, buf);
	free(buf);

	return (ret);
}

/*
 * Vigil.version(), returns the vigil version string.
 */
static jstring
android_version(JNIEnv *env, jclass cls)
{
	return (android_string_new(env, vigil_version()));
}

/*
 * Vigil.kyrkaVersion(), returns the libkyrka version string.
 */
static jstring
android_kyrka_version(JNIEnv *env, jclass cls)
{
	return (android_string_new(env, kyrka_version()));
}

/*
 * Vigil.callDial(), signals the given peer we want to call it.
 */
static void
android_call_dial(JNIEnv *env, jclass cls, jint peer)
{
	if (peer > 0 && peer < KYRKA_PEERS_PER_FLOCK)
		vigil_call_dial(peer);
}

/*
 * Vigil.callHangup(), stops signaling any peer.
 */
static void
android_call_hangup(JNIEnv *env, jclass cls)
{
	vigil_call_hangup();
}

/*
 * Vigil.voiceCall(), starts a voice session with the given peer.
 */
static void
android_voice_call(JNIEnv *env, jclass cls, jint peer, jint mode)
{
	if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
		return;

	if (mode != VIGIL_VOICE_MODE_CALL && mode != VIGIL_VOICE_MODE_RING &&
	    mode != VIGIL_VOICE_MODE_ANSWER && mode != VIGIL_VOICE_MODE_DECLINE)
		return;

	vigil_voice_call(peer, mode);
}

/*
 * Vigil.voiceHangup(), hangs up the voice session.
 */
static void
android_voice_hangup(JNIEnv *env, jclass cls)
{
	vigil_voice_hangup();
}

/*
 * Vigil.voiceAnswer(), answers the ringing voice session.
 */
static void
android_voice_answer(JNIEnv *env, jclass cls)
{
	vigil_voice_answer();
}

/*
 * Vigil.voiceMute(), mutes or unmutes our side of the call.
 */
static void
android_voice_mute(JNIEnv *env, jclass cls, jboolean on)
{
	vigil_voice_mute(on == JNI_TRUE);
}

/*
 * Vigil.voiceStatus(), returns the state of the voice session as json.
 */
static jstring
android_voice_status(JNIEnv *env, jclass cls)
{
	char		buf[1024];

	if (vigil_voice_status(buf, sizeof(buf)) == 0)
		(void)snprintf(buf, sizeof(buf), "{\"running\":false}");

	return (android_string_new(env, buf));
}

/*
 * Vigil.chatSend(), queues a message for the given peer. The text is
 * utf-8 in a byte array as JNI strings are not quite utf-8. A hash that
 * is not 0 makes it a reply to the message by author with that hash, a
 * topic that is not 0 puts it in that topic.
 */
static jboolean
android_chat_send(JNIEnv *env, jclass cls, jint peer, jlong ref,
    jbyteArray text, jint author, jlong hash, jlong topic)
{
	int		ret;
	size_t		len;
	size_t		max;
	char		*buf;

	if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	if (author < 0 || author >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	if (text == NULL)
		return (JNI_FALSE);

	if (topic != 0)
		max = VIGIL_CHAT_TOPIC_MAX;
	else if (hash != 0)
		max = VIGIL_CHAT_REPLY_MAX;
	else
		max = VIGIL_CHAT_MESSAGE_MAX;

	buf = android_bytes_copy(env, text, VIGIL_CONFIG_MAX, &len);
	if (buf == NULL)
		return (JNI_FALSE);

	if (len == 0 || len > max) {
		android_bytes_free(buf, len);
		return (JNI_FALSE);
	}

	if (topic != 0) {
		ret = vigil_chat_topic_send(peer, (u_int64_t)ref,
		    (u_int64_t)topic, author, (u_int64_t)hash, buf, len);
	} else if (hash != 0) {
		ret = vigil_chat_reply(peer, (u_int64_t)ref, author,
		    (u_int64_t)hash, buf, len);
	} else {
		ret = vigil_chat_send(peer, (u_int64_t)ref, buf, len);
	}

	android_bytes_free(buf, len);

	if (ret == -1)
		return (JNI_FALSE);

	return (JNI_TRUE);
}

/*
 * Vigil.chatTopic(), queues the given topic for the given peer. The name
 * is utf-8 in a byte array, an empty name removes the topic.
 */
static jboolean
android_chat_topic(JNIEnv *env, jclass cls, jint peer, jlong ref,
    jlong topic, jint color, jbyteArray name)
{
	int		ret;
	size_t		len;
	char		*buf;

	if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	if (topic == 0)
		return (JNI_FALSE);

	if (name == NULL)
		return (JNI_FALSE);

	buf = android_bytes_copy(env, name, VIGIL_CHAT_TOPIC_NAME_MAX, &len);
	if (buf == NULL)
		return (JNI_FALSE);

	ret = vigil_chat_topic(peer, (u_int64_t)ref, (u_int64_t)topic,
	    (u_int32_t)color, buf, len);

	android_bytes_free(buf, len);

	if (ret == -1)
		return (JNI_FALSE);

	return (JNI_TRUE);
}

/*
 * Vigil.chatForget(), drops the messages we hold for the given peer.
 */
static jboolean
android_chat_forget(JNIEnv *env, jclass cls, jint peer)
{
	if (peer <= 0)
		return (JNI_FALSE);

	if (peer >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	return (vigil_chat_forget(peer) == 0);
}

/*
 * Vigil.chatDrop(), drops the message with the given ref we hold for the
 * given peer.
 */
static jboolean
android_chat_drop(JNIEnv *env, jclass cls, jint peer, jlong ref)
{
	if (peer <= 0)
		return (JNI_FALSE);

	if (peer >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	return (vigil_chat_drop(peer, ref) == 0);
}

/*
 * Vigil.chatStored(), the message with the given token was stored.
 */
static jboolean
android_chat_stored(JNIEnv *env, jclass cls, jlong token)
{
	return (vigil_chat_stored((u_int64_t)token) == 0);
}

/*
 * Vigil.chatOpen(), marks the chat with the given peer as open or not.
 */
static void
android_chat_open(JNIEnv *env, jclass cls, jint peer, jboolean on)
{
	if (peer > 0 && peer < KYRKA_PEERS_PER_FLOCK)
		vigil_chat_open(peer, on == JNI_TRUE);
}

/*
 * Vigil.chatTyping(), tells the given peer if we are typing, in the given
 * topic if it is not 0.
 */
static void
android_chat_typing(JNIEnv *env, jclass cls, jint peer, jlong topic,
    jboolean on)
{
	if (peer > 0 && peer < KYRKA_PEERS_PER_FLOCK)
		vigil_chat_typing(peer, topic, on == JNI_TRUE);
}

/*
 * Vigil.chatNick(), sets the nickname we send to our peers.
 */
static jboolean
android_chat_nick(JNIEnv *env, jclass cls, jstring name)
{
	int		ret;
	const char	*str;

	if (name == NULL)
		return (JNI_FALSE);

	str = (*env)->GetStringUTFChars(env, name, NULL);
	if (str == NULL)
		return (JNI_FALSE);

	ret = vigil_chat_nick(str);
	(*env)->ReleaseStringUTFChars(env, name, str);

	if (ret == -1)
		return (JNI_FALSE);

	return (JNI_TRUE);
}

/*
 * Vigil.chatAvatar(), sets the avatar we send to our peers, null if we
 * have none.
 */
static jboolean
android_chat_avatar(JNIEnv *env, jclass cls, jbyteArray data)
{
	size_t		len;
	char		*buf;

	if (data == NULL) {
		vigil_chat_avatar(NULL, 0);
		return (JNI_TRUE);
	}

	buf = android_bytes_copy(env, data, VIGIL_CHAT_AVATAR_MAX, &len);
	if (buf == NULL)
		return (JNI_FALSE);

	vigil_chat_avatar(buf, len);
	android_bytes_free(buf, len);

	return (JNI_TRUE);
}

/*
 * Vigil.chatGroupTyping(), tells the owner and members of the given group
 * we type in it and the given topic, a group of 0 means we do not. The
 * members hold a bit for every member.
 */
static void
android_chat_group_typing(JNIEnv *env, jclass cls, jint owner, jlong group,
    jlong topic, jbyteArray members)
{
	size_t		mlen;
	char		*mbuf;

	if (group == 0) {
		vigil_chat_group_typing(0, 0, 0, NULL);
		return;
	}

	if (owner <= 0 || owner >= KYRKA_PEERS_PER_FLOCK)
		return;

	if (members == NULL)
		return;

	mbuf = android_bytes_copy(env, members, VIGIL_CHAT_GROUP_ROSTER, &mlen);
	if (mbuf == NULL)
		return;

	if (mlen == VIGIL_CHAT_GROUP_ROSTER) {
		vigil_chat_group_typing(owner, group, topic,
		    (const u_int8_t *)mbuf);
	}

	android_bytes_free(mbuf, mlen);
}

/*
 * Vigil.chatGroupSend(), queues a message in the given group for the given
 * peer, in the given topic if it is not 0, serial is that of the roster we
 * know. The text is utf-8 in a byte array as JNI strings are not quite
 * utf-8. A hash that is not 0 makes it a reply, see android_chat_send().
 */
static jboolean
android_chat_group_send(JNIEnv *env, jclass cls, jint peer, jlong ref,
    jint owner, jlong group, jint serial, jlong topic, jbyteArray text,
    jint author, jlong hash)
{
	int		ret;
	size_t		len;
	size_t		max;
	char		*buf;

	if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	if (owner <= 0 || owner >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	if (author < 0 || author >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	if (group == 0)
		return (JNI_FALSE);

	if (text == NULL)
		return (JNI_FALSE);

	if (topic != 0)
		max = VIGIL_CHAT_GROUP_TOPIC_MAX;
	else if (hash != 0)
		max = VIGIL_CHAT_GROUP_REPLY_MAX;
	else
		max = VIGIL_CHAT_GROUP_MAX;

	buf = android_bytes_copy(env, text, VIGIL_CONFIG_MAX, &len);
	if (buf == NULL)
		return (JNI_FALSE);

	if (len == 0 || len > max) {
		android_bytes_free(buf, len);
		return (JNI_FALSE);
	}

	ret = vigil_chat_group_send(peer, ref, owner, group, serial, topic,
	    author, hash, buf, len);

	android_bytes_free(buf, len);

	if (ret == -1)
		return (JNI_FALSE);

	return (JNI_TRUE);
}

/*
 * Vigil.chatGroupTopic(), queues the given topic of the given group we own
 * for the given peer, serial is that of our roster. The name is utf-8 in a
 * byte array, an empty name removes the topic.
 */
static jboolean
android_chat_group_topic(JNIEnv *env, jclass cls, jint peer, jlong ref,
    jlong group, jint serial, jlong topic, jint color, jbyteArray name)
{
	int		ret;
	size_t		len;
	char		*buf;

	if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	if (group == 0)
		return (JNI_FALSE);

	if (topic == 0)
		return (JNI_FALSE);

	if (name == NULL)
		return (JNI_FALSE);

	buf = android_bytes_copy(env, name, VIGIL_CHAT_TOPIC_NAME_MAX, &len);
	if (buf == NULL)
		return (JNI_FALSE);

	ret = vigil_chat_group_topic(peer, ref, group, serial, topic, color,
	    buf, len);

	android_bytes_free(buf, len);

	if (ret == -1)
		return (JNI_FALSE);

	return (JNI_TRUE);
}

/*
 * Vigil.chatGroupRoster(), queues the roster of the given group we own for
 * the given peer, members holds a bit for every member and the name is
 * utf-8 in a byte array.
 */
static jboolean
android_chat_group_roster(JNIEnv *env, jclass cls, jint peer, jlong ref,
    jlong group, jint serial, jboolean ended, jbyteArray members,
    jbyteArray name)
{
	int		ret;
	int		state;
	size_t		len, mlen;
	char		*buf, *mbuf;

	if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	if (group == 0)
		return (JNI_FALSE);

	if (members == NULL)
		return (JNI_FALSE);

	if (name == NULL)
		return (JNI_FALSE);

	mbuf = android_bytes_copy(env, members, VIGIL_CHAT_GROUP_ROSTER, &mlen);
	if (mbuf == NULL)
		return (JNI_FALSE);

	if (mlen != VIGIL_CHAT_GROUP_ROSTER) {
		android_bytes_free(mbuf, mlen);
		return (JNI_FALSE);
	}

	buf = android_bytes_copy(env, name, VIGIL_CHAT_GROUP_NAME_MAX, &len);
	if (buf == NULL) {
		android_bytes_free(mbuf, mlen);
		return (JNI_FALSE);
	}

	if (ended == JNI_TRUE)
		state = VIGIL_CHAT_GROUP_ENDED;
	else
		state = VIGIL_CHAT_GROUP_OPEN;

	ret = vigil_chat_group_roster(peer, ref, group, serial, state,
	    (const u_int8_t *)mbuf, buf, len);

	android_bytes_free(buf, len);
	android_bytes_free(mbuf, mlen);

	if (ret == -1)
		return (JNI_FALSE);

	return (JNI_TRUE);
}

/*
 * Vigil.chatGroupLeave(), queues a leave for the given group that the
 * given peer owns.
 */
static jboolean
android_chat_group_leave(JNIEnv *env, jclass cls, jint owner, jlong ref,
    jlong group)
{
	int		ret;

	if (owner <= 0 || owner >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	if (group == 0)
		return (JNI_FALSE);

	ret = vigil_chat_group_leave(owner, ref, group);
	if (ret == -1)
		return (JNI_FALSE);

	return (JNI_TRUE);
}

/*
 * Vigil.chatDataSend(), queues data of the given kind under the given
 * name for the given peer, in the given topic if it is not 0. The name is
 * utf-8 in a byte array.
 */
static jboolean
android_chat_data_send(JNIEnv *env, jclass cls, jint peer, jlong ref,
    jint kind, jbyteArray name, jbyteArray data, jlong topic)
{
	jboolean	ret;

	if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	ret = android_chat_data_queue(env, peer, ref, 0, 0, 0, kind,
	    name, data, topic);

	return (ret);
}

/*
 * Vigil.chatGroupDataSend(), queues data of the given kind under the
 * given name for the given peer in the given group, in the given topic if
 * it is not 0. Serial is that of the roster we know and the name is utf-8
 * in a byte array.
 */
static jboolean
android_chat_group_data_send(JNIEnv *env, jclass cls, jint peer, jlong ref,
    jint owner, jlong group, jint serial, jlong topic, jint kind,
    jbyteArray name, jbyteArray data)
{
	jboolean	ret;

	if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	if (owner <= 0 || owner >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	if (group == 0)
		return (JNI_FALSE);

	ret = android_chat_data_queue(env, peer, ref, owner, group, serial,
	    kind, name, data, topic);

	return (ret);
}

/*
 * Hand the given data to the core for the given peer, in the given group
 * and the given topic for those that are not 0.
 */
static jboolean
android_chat_data_queue(JNIEnv *env, jint peer, jlong ref, jint owner,
    jlong group, jint serial, jint kind, jbyteArray name, jbyteArray data,
    jlong topic)
{
	int		ret;
	size_t		len, nlen;
	char		*buf, *nbuf;

	if (name == NULL)
		return (JNI_FALSE);

	if (data == NULL)
		return (JNI_FALSE);

	nbuf = android_bytes_copy(env, name, VIGIL_CHAT_DATA_NAME_MAX, &nlen);
	if (nbuf == NULL)
		return (JNI_FALSE);

	buf = android_bytes_copy(env, data, VIGIL_CHAT_DATA_MAX, &len);
	if (buf == NULL) {
		android_bytes_free(nbuf, nlen);
		return (JNI_FALSE);
	}

	if (len == 0) {
		ret = -1;
	} else if (group != 0) {
		ret = vigil_chat_group_data_send(peer, ref, owner, group,
		    serial, topic, kind, nbuf, buf, len);
	} else if (topic != 0) {
		ret = vigil_chat_topic_data_send(peer, (u_int64_t)ref,
		    (u_int64_t)topic, kind, nbuf, buf, len);
	} else {
		ret = vigil_chat_data_send(peer, (u_int64_t)ref, kind, nbuf,
		    buf, len);
	}

	android_bytes_free(buf, len);
	android_bytes_free(nbuf, nlen);

	if (ret == -1)
		return (JNI_FALSE);

	return (JNI_TRUE);
}

/*
 * Vigil.chatDataTake(), returns the data that arrived under the given
 * token, null if it is gone.
 */
static jbyteArray
android_chat_data_take(JNIEnv *env, jclass cls, jlong token, jint size)
{
	ssize_t		len;
	jbyteArray	ret;
	char		*buf;

	if (size <= 0 || size > VIGIL_CHAT_DATA_MAX)
		return (NULL);

	buf = malloc(size);
	if (buf == NULL)
		return (NULL);

	ret = NULL;
	len = vigil_chat_data_take((u_int64_t)token, buf, size);

	if (len > 0)
		ret = (*env)->NewByteArray(env, len);

	if (ret != NULL) {
		(*env)->SetByteArrayRegion(env, ret, 0, len,
		    (const jbyte *)buf);
	}

	android_bytes_free(buf, size);

	return (ret);
}

/*
 * Vigil.chatEvents(), returns the chat events as a utf-8 json array.
 */
static jbyteArray
android_chat_events(JNIEnv *env, jclass cls)
{
	size_t		len;
	jbyteArray	ret;
	char		*buf;

	buf = calloc(1, VIGIL_CHAT_EVENTS_MAX);
	if (buf == NULL)
		return (NULL);

	len = vigil_chat_events(buf, VIGIL_CHAT_EVENTS_MAX);
	if (len == 0) {
		(void)snprintf(buf, VIGIL_CHAT_EVENTS_MAX, "[]");
		len = strlen(buf);
	}

	ret = (*env)->NewByteArray(env, len);
	if (ret != NULL) {
		(*env)->SetByteArrayRegion(env, ret, 0, len,
		    (const jbyte *)buf);
	}

	nyfe_mem_zero(buf, VIGIL_CHAT_EVENTS_MAX);
	free(buf);

	return (ret);
}

/*
 * Vigil.gospel(), attaches the given Gospel configuration to the running
 * core or detaches the one it has if config is null.
 */
static jboolean
android_gospel(JNIEnv *env, jclass cls, jbyteArray config)
{
	int		ret;
	size_t		len;
	char		*buf;

	len = 0;
	buf = NULL;

	if (config != NULL) {
		buf = android_bytes_copy(env, config, VIGIL_CONFIG_MAX, &len);
		if (buf == NULL)
			return (JNI_FALSE);
	}

	ret = vigil_gospel(buf, len);

	if (buf != NULL)
		android_bytes_free(buf, len);

	if (ret == -1)
		return (JNI_FALSE);

	return (JNI_TRUE);
}

/*
 * Vigil.remoteWord(), returns the word at idx in the word list or null
 * once past its end.
 */
static jstring
android_remote_word(JNIEnv *env, jclass cls, jint idx)
{
	const char	*word;

	if (idx < 0 || idx > USHRT_MAX)
		return (NULL);

	if ((word = vigil_remote_word((u_int16_t)idx)) == NULL)
		return (NULL);

	return (android_string_new(env, word));
}

/*
 * Vigil.remoteWords(), reads the route from the words covenant gave us
 * and wipes them from the array. Returns the flock, cathedral and our
 * cosk as json or null if the words are not right.
 */
static jstring
android_remote_words(JNIEnv *env, jclass cls, jbyteArray words)
{
	int		ret;
	size_t		len;
	char		*buf, out[256];

	if (words == NULL)
		return (NULL);

	buf = android_bytes_copy(env, words, VIGIL_CONFIG_MAX, &len);
	if (buf == NULL)
		return (NULL);

	android_bytes_wipe(env, words, len);

	ret = vigil_remote_words(buf, out, sizeof(out));
	android_bytes_free(buf, len);

	if (ret == -1)
		return (NULL);

	return (android_string_new(env, out));
}

/*
 * Vigil.remoteStart(), sets up the tunnel to covenant once we joined the
 * flock under the given cathedral id and secret.
 */
static jboolean
android_remote_start(JNIEnv *env, jclass cls, jstring id, jstring secret)
{
	int		ret;
	const char	*cid, *cs;

	if (id == NULL || secret == NULL)
		return (JNI_FALSE);

	if ((cid = (*env)->GetStringUTFChars(env, id, NULL)) == NULL)
		return (JNI_FALSE);

	if ((cs = (*env)->GetStringUTFChars(env, secret, NULL)) == NULL) {
		(*env)->ReleaseStringUTFChars(env, id, cid);
		return (JNI_FALSE);
	}

	ret = vigil_remote_start(cid, cs);

	(*env)->ReleaseStringUTFChars(env, secret, cs);
	(*env)->ReleaseStringUTFChars(env, id, cid);

	if (ret == -1)
		return (JNI_FALSE);

	return (JNI_TRUE);
}

/*
 * Vigil.remoteRun(), runs the tunnel to covenant for up to a second.
 */
static jint
android_remote_run(JNIEnv *env, jclass cls)
{
	return (vigil_remote_run());
}

/*
 * Vigil.remoteConfig(), the configuration covenant sent us, null if we
 * do not have it (yet).
 */
static jbyteArray
android_remote_config(JNIEnv *env, jclass cls)
{
	size_t		len;
	jbyteArray	ret;
	char		out[VIGIL_CONFIG_MAX];

	if (vigil_remote_config(out, sizeof(out)) == -1)
		return (NULL);

	len = strlen(out);

	if ((ret = (*env)->NewByteArray(env, len)) != NULL)
		(*env)->SetByteArrayRegion(env, ret, 0, len, (jbyte *)out);

	nyfe_mem_zero(out, sizeof(out));

	return (ret);
}

/*
 * Vigil.remoteStop(), forgets everything about joining remotely.
 */
static void
android_remote_stop(JNIEnv *env, jclass cls)
{
	vigil_remote_stop();
}

/*
 * Copy the given java byte array into memory we own, so that we
 * can wipe it. Release it via android_bytes_free().
 */
static char *
android_bytes_copy(JNIEnv *env, jbyteArray array, size_t max, size_t *len)
{
	jsize		size;
	char		*buf;

	PRECOND(env != NULL);
	PRECOND(array != NULL);
	PRECOND(len != NULL);

	size = (*env)->GetArrayLength(env, array);
	if (size < 0 || (size_t)size > max)
		return (NULL);

	buf = calloc(1, (size_t)size + 1);
	if (buf == NULL)
		fatal("calloc: failed to allocate java bytes");

	(*env)->GetByteArrayRegion(env, array, 0, size, (jbyte *)buf);
	*len = (size_t)size;

	return (buf);
}

/*
 * Overwrite the first len bytes of the given java byte array.
 */
static void
android_bytes_wipe(JNIEnv *env, jbyteArray array, size_t len)
{
	char		*zero;

	PRECOND(env != NULL);
	PRECOND(array != NULL);

	zero = calloc(1, len + 1);
	if (zero == NULL)
		fatal("calloc: failed to allocate wipe buffer");

	(*env)->SetByteArrayRegion(env, array, 0, len, (const jbyte *)zero);
	free(zero);
}

/*
 * Wipe and release a buffer from android_bytes_copy().
 */
static void
android_bytes_free(char *buf, size_t len)
{
	PRECOND(buf != NULL);

	if (len > 0)
		nyfe_mem_zero(buf, len);

	free(buf);
}

/*
 * Helper to create a java string from the given C string.
 */
static jstring
android_string_new(JNIEnv *env, const char *str)
{
	PRECOND(env != NULL);
	PRECOND(str != NULL);

	return ((*env)->NewStringUTF(env, str));
}
