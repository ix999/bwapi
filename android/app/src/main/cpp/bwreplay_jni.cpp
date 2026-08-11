// JNI bridge for com.openbw.replays.NativeBridge.
//
// Every entry point is called from the Android UI thread while the engine runs
// on the SDL thread. Commands are queued and applied by the engine at the top
// of its next tick, and status is read from a snapshot the engine publishes
// under its own lock, so nothing here blocks on a frame being rendered.

#include <jni.h>

#include <string>
#include <vector>

#include "android_state.h"

using bwreplay_android::g_core;
using bwreplay_android::g_mutex;

namespace {

// Runs fn with the engine, if the viewer is still alive. Returns false when
// playback has already finished and the pointer has been cleared.
template <typename F>
bool with_core(F&& fn) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_core) return false;
	fn(*g_core);
	return true;
}

jstring to_jstring(JNIEnv* env, const std::string& s) {
	return env->NewStringUTF(s.c_str());
}

}  // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_openbw_replays_NativeBridge_nativeIsRunning(JNIEnv*, jclass) {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_core != nullptr ? JNI_TRUE : JNI_FALSE;
}

// Packs the whole playback state into one array to keep the polling the UI does
// each frame down to a single JNI crossing.
// [0] current frame        [1] end frame
// [2] paused (0/1)         [3] speed  x1000
// [4] replay loaded (0/1)  [5] finished (0/1)
// [6] zoom x1000            [7] following (0/1)
JNIEXPORT jintArray JNICALL
Java_com_openbw_replays_NativeBridge_nativeGetStatus(JNIEnv* env, jclass) {
	bwreplay::Status status;
	bool alive = with_core([&](bwreplay::Core& core) { status = core.status(); });

	jint values[8];
	values[0] = status.current_frame;
	values[1] = status.end_frame;
	values[2] = status.paused ? 1 : 0;
	values[3] = (jint)(status.speed * 1000.0);
	values[4] = (alive && status.replay_loaded) ? 1 : 0;
	values[5] = (!alive || status.done) ? 1 : 0;
	values[6] = (jint)(status.zoom * 1000.0);
	values[7] = status.following ? 1 : 0;

	jintArray result = env->NewIntArray(8);
	if (!result) return nullptr;
	env->SetIntArrayRegion(result, 0, 8, values);
	return result;
}

JNIEXPORT void JNICALL
Java_com_openbw_replays_NativeBridge_nativeSetPaused(JNIEnv*, jclass, jboolean paused) {
	with_core([&](bwreplay::Core& core) { core.cmd_set_paused(paused == JNI_TRUE); });
}

JNIEXPORT void JNICALL
Java_com_openbw_replays_NativeBridge_nativeTogglePaused(JNIEnv*, jclass) {
	with_core([](bwreplay::Core& core) { core.cmd_toggle_paused(); });
}

JNIEXPORT void JNICALL
Java_com_openbw_replays_NativeBridge_nativeSetSpeed(JNIEnv*, jclass, jfloat multiplier) {
	with_core([&](bwreplay::Core& core) { core.cmd_set_speed(multiplier); });
}

JNIEXPORT void JNICALL
Java_com_openbw_replays_NativeBridge_nativeSeekFraction(JNIEnv*, jclass, jfloat fraction) {
	with_core([&](bwreplay::Core& core) { core.cmd_seek_fraction(fraction); });
}

JNIEXPORT void JNICALL
Java_com_openbw_replays_NativeBridge_nativeSeekFrame(JNIEnv*, jclass, jint frame) {
	with_core([&](bwreplay::Core& core) { core.cmd_seek_frame(frame); });
}

JNIEXPORT void JNICALL
Java_com_openbw_replays_NativeBridge_nativePan(JNIEnv*, jclass, jint dx, jint dy) {
	with_core([&](bwreplay::Core& core) { core.cmd_pan(dx, dy); });
}

JNIEXPORT void JNICALL
Java_com_openbw_replays_NativeBridge_nativeSetZoom(JNIEnv*, jclass, jfloat zoom) {
	with_core([&](bwreplay::Core& core) { core.cmd_set_zoom(zoom); });
}

JNIEXPORT void JNICALL
Java_com_openbw_replays_NativeBridge_nativeSelectAt(JNIEnv*, jclass, jint x, jint y) {
	with_core([&](bwreplay::Core& core) { core.cmd_select_at(x, y); });
}

JNIEXPORT void JNICALL
Java_com_openbw_replays_NativeBridge_nativeToggleFollowAt(JNIEnv*, jclass, jint x, jint y) {
	with_core([&](bwreplay::Core& core) { core.cmd_toggle_follow_at(x, y); });
}

JNIEXPORT void JNICALL
Java_com_openbw_replays_NativeBridge_nativeSetHudVisible(JNIEnv*, jclass, jboolean visible) {
	with_core([&](bwreplay::Core& core) { core.cmd_set_hud_visible(visible == JNI_TRUE); });
}

JNIEXPORT void JNICALL
Java_com_openbw_replays_NativeBridge_nativeLoadReplay(JNIEnv* env, jclass, jstring path) {
	if (!path) return;
	const char* chars = env->GetStringUTFChars(path, nullptr);
	if (!chars) return;
	std::string value(chars);
	env->ReleaseStringUTFChars(path, chars);
	with_core([&](bwreplay::Core& core) { core.cmd_load_replay_path(value); });
}

JNIEXPORT void JNICALL
Java_com_openbw_replays_NativeBridge_nativeQuit(JNIEnv*, jclass) {
	with_core([](bwreplay::Core& core) { core.cmd_quit(); });
}

JNIEXPORT jstring JNICALL
Java_com_openbw_replays_NativeBridge_nativeGetError(JNIEnv* env, jclass) {
	std::lock_guard<std::mutex> lock(g_mutex);
	return to_jstring(env, bwreplay_android::g_error);
}

JNIEXPORT jstring JNICALL
Java_com_openbw_replays_NativeBridge_nativeGetMapName(JNIEnv* env, jclass) {
	std::string name;
	with_core([&](bwreplay::Core& core) { name = core.info().map_name; });
	return to_jstring(env, name);
}

JNIEXPORT jobjectArray JNICALL
Java_com_openbw_replays_NativeBridge_nativeGetPlayerNames(JNIEnv* env, jclass) {
	std::vector<std::string> names;
	with_core([&](bwreplay::Core& core) { names = core.info().player_names; });

	jclass string_class = env->FindClass("java/lang/String");
	if (!string_class) return nullptr;
	jobjectArray result = env->NewObjectArray((jsize)names.size(), string_class, nullptr);
	if (!result) return nullptr;
	for (jsize i = 0; i < (jsize)names.size(); ++i) {
		jstring value = to_jstring(env, names[(size_t)i]);
		if (!value) return nullptr;
		env->SetObjectArrayElement(result, i, value);
		env->DeleteLocalRef(value);
	}
	return result;
}

}  // extern "C"
