// SDL entry point for the replay viewer.
//
// SDLActivity spawns a dedicated thread and calls SDL_main on it; including
// SDL.h renames main() to SDL_main. Arguments come from
// ViewerActivity.getArguments(): argv[1] is the directory holding the mpqs and
// argv[2] is the .rep to play. Both are ordinary files in app-private storage
// (the Kotlin side has already copied anything imported through SAF), so the
// engine can open them directly.

#include <SDL.h>

#include <android/log.h>

#include <fstream>
#include <string>
#include <vector>

#include "android_state.h"

namespace bwreplay_android {

std::mutex g_mutex;
bwreplay::Core* g_core = nullptr;
std::string g_error;

}  // namespace bwreplay_android

namespace {

void log_error(const std::string& message) {
	__android_log_print(ANDROID_LOG_ERROR, "bwreplay", "%s", message.c_str());
	std::lock_guard<std::mutex> lock(bwreplay_android::g_mutex);
	bwreplay_android::g_error = message;
}

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	f.seekg(0, std::ios::end);
	std::streamoff len = f.tellg();
	if (len < 0) return false;
	f.seekg(0, std::ios::beg);
	out->resize((size_t)len);
	if (len > 0) f.read(reinterpret_cast<char*>(out->data()), len);
	return (bool)f;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 3) {
		log_error("viewer started without a data directory and replay path");
		return 1;
	}
	const std::string data_dir = argv[1];
	const std::string replay_path = argv[2];

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		log_error(std::string("SDL_Init failed: ") + SDL_GetError());
		return 1;
	}

	// openbw's UI is written against mouse events, so let SDL synthesise them
	// from touches. That gives taps, drag-select and the minimap for free.
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");

	SDL_Rect bounds{0, 0, 1280, 720};
	if (SDL_GetDisplayBounds(0, &bounds) != 0 || bounds.w <= 0 || bounds.h <= 0) {
		bounds = SDL_Rect{0, 0, 1280, 720};
	}

	bwreplay::Core core;

	std::string err;
	if (!core.init(data_dir, bounds.w, bounds.h, &err)) {
		log_error("could not load the StarCraft data files: " + err);
		SDL_Quit();
		return 1;
	}

	std::vector<uint8_t> replay;
	if (!read_file(replay_path, &replay)) {
		log_error("could not read replay: " + replay_path);
		SDL_Quit();
		return 1;
	}

	if (!core.load_replay(replay.data(), replay.size(), &err)) {
		log_error("could not load replay: " + err);
		SDL_Quit();
		return 1;
	}
	// The bytes are parsed into engine state during load_replay, so the raw
	// file does not need to stay resident for the length of the session.
	replay.clear();
	replay.shrink_to_fit();

	{
		std::lock_guard<std::mutex> lock(bwreplay_android::g_mutex);
		bwreplay_android::g_core = &core;
		bwreplay_android::g_error.clear();
	}

	__android_log_print(ANDROID_LOG_INFO, "bwreplay", "playback started (%d frames)",
	                    core.status().end_frame);

	while (core.tick()) {
		// The engine paces the simulation against wall time internally; this
		// only keeps the render loop from spinning a core flat out.
		SDL_Delay(8);
	}

	{
		std::lock_guard<std::mutex> lock(bwreplay_android::g_mutex);
		bwreplay_android::g_core = nullptr;
	}

	SDL_Quit();
	return 0;
}
