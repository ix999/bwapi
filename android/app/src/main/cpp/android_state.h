// Shared handle between the SDL thread (which owns the engine) and the JNI
// bridge (called from the Android UI thread).
//
// The engine lives entirely on the SDL thread. The UI thread only ever reaches
// it through Core's thread-safe cmd_*/status methods, and g_mutex exists purely
// to keep the pointer itself valid for the duration of such a call. Core never
// takes g_mutex, so there is no lock ordering to get wrong.

#ifndef BWREPLAY_ANDROID_STATE_H
#define BWREPLAY_ANDROID_STATE_H

#include <mutex>
#include <string>

#include "core/bwreplay_core.h"

namespace bwreplay_android {

extern std::mutex g_mutex;

// Non-null only while the SDL thread is inside its playback loop.
extern bwreplay::Core* g_core;

// Last fatal error, for the UI to show after the viewer exits early.
extern std::string g_error;

}  // namespace bwreplay_android

#endif  // BWREPLAY_ANDROID_STATE_H
