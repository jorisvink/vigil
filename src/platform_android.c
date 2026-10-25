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

static jbyteArray	android_chat_events(JNIEnv *, jclass);
static jbyteArray	android_chat_data_take(JNIEnv *, jclass, jlong, jint);
static void		android_chat_group(JNIEnv *, jclass, jint);
static void		android_chat_forget(JNIEnv *, jclass, jint);
static void		android_chat_stored(JNIEnv *, jclass, jlong);
static jboolean		android_chat_nick(JNIEnv *, jclass, jstring);
static void		android_chat_open(JNIEnv *, jclass, jint, jboolean);
static void		android_chat_typing(JNIEnv *, jclass, jint, jboolean);
static void		android_chat_group_typing(JNIEnv *, jclass, jboolean);
static jboolean		android_chat_send(JNIEnv *, jclass, jint, jlong,
			    jbyteArray);
static jboolean		android_chat_group_send(JNIEnv *, jclass, jlong,
			    jbyteArray);
static jboolean		android_chat_data_send(JNIEnv *, jclass, jint, jlong,
			    jint, jbyteArray, jbyteArray);
static jboolean		android_chat_group_data_send(JNIEnv *, jclass, jlong,
			    jint, jbyteArray, jbyteArray);
static jboolean		android_chat_data_queue(JNIEnv *, jint, jlong, jint,
			    jbyteArray, jbyteArray);

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
	{ "chatSend", "(IJ[B)Z", (void *)android_chat_send },
	{ "chatForget", "(I)V", (void *)android_chat_forget },
	{ "chatStored", "(J)V", (void *)android_chat_stored },
	{ "chatOpen", "(IZ)V", (void *)android_chat_open },
	{ "chatTyping", "(IZ)V", (void *)android_chat_typing },
	{ "chatNick", "(Ljava/lang/String;)Z", (void *)android_chat_nick },
	{ "chatGroup", "(I)V", (void *)android_chat_group },
	{ "chatGroupTyping", "(Z)V", (void *)android_chat_group_typing },
	{ "chatGroupSend", "(J[B)Z", (void *)android_chat_group_send },
	{ "chatEvents", "()[B", (void *)android_chat_events },
	{ "chatDataSend", "(IJI[B[B)Z", (void *)android_chat_data_send },
	{ "chatGroupDataSend", "(JI[B[B)Z",
	    (void *)android_chat_group_data_send },
	{ "chatDataTake", "(JI)[B", (void *)android_chat_data_take },
	{ "gospel", "([B)Z", (void *)android_gospel },
};

/*
 * The JNIEnv and VpnService for the thread that is running vigil_run()
 * and if it has a tunnel, only valid and only used on that thread.
 */
static int		run_tunnel = 0;
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
 * returns -1 if that was not possible. Without a tunnel there is
 * nothing to bypass.
 */
int
vigil_platform_socket(int fd)
{
	jboolean	ok;

	PRECOND(fd >= 0);

	if (!run_tunnel)
		return (0);

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
	run_tunnel = fd != -1;

	ret = vigil_run((u_int64_t)id, fd, cfg, len, gcfg, glen, dir);

	run_env = NULL;
	run_service = NULL;
	run_tunnel = 0;

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

	(void)vigil_config_check(cfg, len, out, sizeof(out));
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
 * utf-8 in a byte array as JNI strings are not quite utf-8.
 */
static jboolean
android_chat_send(JNIEnv *env, jclass cls, jint peer, jlong ref,
    jbyteArray text)
{
	int		ret;
	size_t		len;
	char		*buf;

	if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	if (text == NULL)
		return (JNI_FALSE);

	buf = android_bytes_copy(env, text, VIGIL_CONFIG_MAX, &len);
	if (buf == NULL)
		return (JNI_FALSE);

	if (len == 0 || len > VIGIL_CHAT_MESSAGE_MAX) {
		android_bytes_free(buf, len);
		return (JNI_FALSE);
	}

	ret = vigil_chat_send(peer, (u_int64_t)ref, buf, len);
	android_bytes_free(buf, len);

	if (ret == -1)
		return (JNI_FALSE);

	return (JNI_TRUE);
}

/*
 * Vigil.chatForget(), drops the messages we hold for the given peer.
 */
static void
android_chat_forget(JNIEnv *env, jclass cls, jint peer)
{
	if (peer > 0 && peer < KYRKA_PEERS_PER_FLOCK)
		vigil_chat_forget(peer);
}

/*
 * Vigil.chatStored(), the message with the given token was stored.
 */
static void
android_chat_stored(JNIEnv *env, jclass cls, jlong token)
{
	vigil_chat_stored((u_int64_t)token);
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
 * Vigil.chatTyping(), tells the given peer if we are typing.
 */
static void
android_chat_typing(JNIEnv *env, jclass cls, jint peer, jboolean on)
{
	if (peer > 0 && peer < KYRKA_PEERS_PER_FLOCK)
		vigil_chat_typing(peer, on == JNI_TRUE);
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
 * Vigil.chatGroup(), joins the given group chat, 0 leaves ours.
 */
static void
android_chat_group(JNIEnv *env, jclass cls, jint group)
{
	if (group >= 0 && group <= USHRT_MAX)
		vigil_chat_group(group);
}

/*
 * Vigil.chatGroupTyping(), tells our group if we are typing.
 */
static void
android_chat_group_typing(JNIEnv *env, jclass cls, jboolean on)
{
	vigil_chat_group_typing(on == JNI_TRUE);
}

/*
 * Vigil.chatGroupSend(), queues a message for our group. The text is
 * utf-8 in a byte array as JNI strings are not quite utf-8.
 */
static jboolean
android_chat_group_send(JNIEnv *env, jclass cls, jlong ref, jbyteArray text)
{
	int		ret;
	size_t		len;
	char		*buf;

	if (text == NULL)
		return (JNI_FALSE);

	buf = android_bytes_copy(env, text, VIGIL_CONFIG_MAX, &len);
	if (buf == NULL)
		return (JNI_FALSE);

	if (len == 0 || len > VIGIL_CHAT_MESSAGE_MAX) {
		android_bytes_free(buf, len);
		return (JNI_FALSE);
	}

	ret = vigil_chat_group_send((u_int64_t)ref, buf, len);
	android_bytes_free(buf, len);

	if (ret == -1)
		return (JNI_FALSE);

	return (JNI_TRUE);
}

/*
 * Vigil.chatDataSend(), queues data of the given kind under the given
 * name for the given peer. The name is utf-8 in a byte array.
 */
static jboolean
android_chat_data_send(JNIEnv *env, jclass cls, jint peer, jlong ref,
    jint kind, jbyteArray name, jbyteArray data)
{
	if (peer <= 0 || peer >= KYRKA_PEERS_PER_FLOCK)
		return (JNI_FALSE);

	return (android_chat_data_queue(env, peer, ref, kind, name, data));
}

/*
 * Vigil.chatGroupDataSend(), queues data of the given kind under the
 * given name for our group. The name is utf-8 in a byte array.
 */
static jboolean
android_chat_group_data_send(JNIEnv *env, jclass cls, jlong ref, jint kind,
    jbyteArray name, jbyteArray data)
{
	return (android_chat_data_queue(env, 0, ref, kind, name, data));
}

/*
 * Hand the given data to the core for the given peer, or for our group
 * if the peer is 0.
 */
static jboolean
android_chat_data_queue(JNIEnv *env, jint peer, jlong ref, jint kind,
    jbyteArray name, jbyteArray data)
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
	} else if (peer == 0) {
		ret = vigil_chat_group_data_send((u_int64_t)ref, kind, nbuf,
		    buf, len);
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
