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

#include <jni.h>
#include <stdint.h>

#include "vigil.h"

/*
 * The camera, encoder and decoder live in Video.kt, we call into it from
 * the voice thread and it hands us its frames via direct byte buffers.
 */

/* The Kotlin object that does the video. */
#define ANDROID_VIDEO_CLASS	"se/sanctorum/vigil/Video"

static JNIEnv	*video_env(void);
static void	video_call(jmethodID, ...);

static jlong	video_take(JNIEnv *, jclass, jobject);
static void	video_refresh(JNIEnv *, jclass);
static void	video_camera(JNIEnv *, jclass, jboolean);
static jboolean	video_input(JNIEnv *, jclass, jobject, jint, jint);

/* The native methods we register with the Video object. */
static JNINativeMethod	methods[] = {
	{ "input", "(Ljava/nio/ByteBuffer;II)Z", (void *)video_input },
	{ "take", "(Ljava/nio/ByteBuffer;)J", (void *)video_take },
	{ "camera", "(Z)V", (void *)video_camera },
	{ "refresh", "()V", (void *)video_refresh },
};

/* Our VM, the Video object and the methods we call on it. */
static JavaVM		*vm = NULL;
static jclass		video = NULL;
static jmethodID	video_start = NULL;
static jmethodID	video_stop = NULL;
static jmethodID	video_camera_on = NULL;
static jmethodID	video_keyframe = NULL;
static jmethodID	video_bitrate = NULL;

/*
 * Called from JNI_OnLoad(), registers our native methods and looks up
 * those we call. Returns -1 if Video.kt is not what we expect.
 */
int
vigil_android_video_load(JavaVM *jvm, JNIEnv *env)
{
	jclass		cls;

	PRECOND(jvm != NULL);
	PRECOND(env != NULL);

	cls = (*env)->FindClass(env, ANDROID_VIDEO_CLASS);
	if (cls == NULL)
		return (-1);

	if ((*env)->RegisterNatives(env, cls, methods,
	    sizeof(methods) / sizeof(methods[0])) != 0)
		return (-1);

	video_start = (*env)->GetStaticMethodID(env, cls, "coreStart", "()Z");
	video_stop = (*env)->GetStaticMethodID(env, cls, "coreStop", "()V");
	video_camera_on = (*env)->GetStaticMethodID(env, cls,
	    "coreCamera", "(Z)V");
	video_keyframe = (*env)->GetStaticMethodID(env, cls,
	    "coreKeyframe", "()V");
	video_bitrate = (*env)->GetStaticMethodID(env, cls,
	    "coreBitrate", "(I)V");

	if (video_start == NULL || video_stop == NULL ||
	    video_camera_on == NULL || video_keyframe == NULL ||
	    video_bitrate == NULL)
		return (-1);

	video = (*env)->NewGlobalRef(env, cls);
	(*env)->DeleteLocalRef(env, cls);

	if (video == NULL)
		return (-1);

	vm = jvm;

	return (0);
}

/*
 * Start the decoder, returns -1 if there is none.
 */
int
vigil_platform_video_start(void)
{
	jboolean	ok;
	JNIEnv		*env;

	env = video_env();
	if (env == NULL)
		return (-1);

	ok = (*env)->CallStaticBooleanMethod(env, video, video_start);

	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
		return (-1);
	}

	if (ok == JNI_FALSE)
		return (-1);

	return (0);
}

/*
 * Stop the camera, encoder and decoder, they do not call us after this.
 */
void
vigil_platform_video_stop(void)
{
	video_call(video_stop);
}

/*
 * Turn the camera and encoder on or off.
 */
void
vigil_platform_video_camera(int on)
{
	PRECOND(on == 0 || on == 1);

	video_call(video_camera_on, on ? JNI_TRUE : JNI_FALSE);
}

/*
 * Ask the encoder for a keyframe.
 */
void
vigil_platform_video_keyframe(void)
{
	video_call(video_keyframe);
}

/*
 * Give the encoder the bitrate to aim for.
 */
void
vigil_platform_video_bitrate(u_int32_t bps)
{
	if (bps > INT32_MAX)
		bps = INT32_MAX;

	video_call(video_bitrate, (jint)bps);
}

/*
 * Returns the JNIEnv for the calling thread, attaching it if needed.
 */
static JNIEnv *
video_env(void)
{
	JNIEnv		*env;

	if (vm == NULL || video == NULL)
		return (NULL);

	if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK)
		return (env);

	if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK)
		return (NULL);

	return (env);
}

/*
 * Call the given static void method on the Video object.
 */
static void
video_call(jmethodID method, ...)
{
	va_list		args;
	JNIEnv		*env;

	PRECOND(method != NULL);

	env = video_env();
	if (env == NULL)
		return;

	va_start(args, method);
	(*env)->CallStaticVoidMethodV(env, video, method, args);
	va_end(args);

	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionDescribe(env);
		(*env)->ExceptionClear(env);
	}
}

/*
 * Video.input(), the encoder has a frame of len bytes in buf for us.
 */
static jboolean
video_input(JNIEnv *env, jclass cls, jobject buf, jint len, jint flags)
{
	u_int8_t	*data;
	jlong		cap;

	if (buf == NULL || len <= 0)
		return (JNI_FALSE);

	data = (*env)->GetDirectBufferAddress(env, buf);
	cap = (*env)->GetDirectBufferCapacity(env, buf);

	if (data == NULL || cap < len)
		return (JNI_FALSE);

	if (vigil_video_input(data, len, flags) == -1)
		return (JNI_FALSE);

	return (JNI_TRUE);
}

/*
 * Video.take(), copies the next frame to decode into buf. Returns its
 * length with its flags in the upper 32 bits, or 0 if there is none.
 */
static jlong
video_take(JNIEnv *env, jclass cls, jobject buf)
{
	size_t		len;
	int		flags;
	u_int8_t	*data;
	jlong		cap;

	if (buf == NULL)
		return (0);

	data = (*env)->GetDirectBufferAddress(env, buf);
	cap = (*env)->GetDirectBufferCapacity(env, buf);

	if (data == NULL || cap < VIGIL_VIDEO_FRAME_MAX)
		return (0);

	len = vigil_video_take(data, VIGIL_VIDEO_FRAME_MAX, &flags);
	if (len == 0)
		return (0);

	return ((jlong)len | ((jlong)(flags & 0xff) << 32));
}

/*
 * Video.camera(), turn our camera on or off for the next or current call.
 */
static void
video_camera(JNIEnv *env, jclass cls, jboolean on)
{
	vigil_video_camera(on ? 1 : 0);
}

/*
 * Video.refresh(), the decoder starts over and needs a keyframe.
 */
static void
video_refresh(JNIEnv *env, jclass cls)
{
	vigil_video_refresh();
}
