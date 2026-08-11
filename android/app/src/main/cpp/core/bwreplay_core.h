// Platform-independent driver for OpenBW replay playback.
//
// This wraps bwgame::ui_functions (the same engine + renderer that openbw's
// gfxtest.cpp uses) behind an API that a host UI can drive from another thread.
// The engine itself is single threaded: every method marked "engine thread"
// must be called from the thread running tick(), while the cmd_*/status
// methods are safe to call from anywhere and are applied at the top of tick().
//
// Android drives this from SDL's main thread with the Kotlin UI queueing
// commands; the Linux test harness drives it from main().

#ifndef BWREPLAY_CORE_H
#define BWREPLAY_CORE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bwreplay {

// Names/frame count pulled out of the replay header, published to the host UI.
struct ReplayInfo {
	std::string map_name;
	std::vector<std::string> player_names;
	int end_frame = 0;
};

// Snapshot of playback state. Copied under a lock so the host UI never reads a
// torn value while the engine thread is mid-frame.
struct Status {
	int current_frame = 0;
	int end_frame = 0;
	bool paused = false;
	double speed = 1.0;
	bool replay_loaded = false;
	bool done = false;
	bool quit_requested = false;
	double zoom = 1.0;
	bool following = false;
};

class Core {
public:
	Core();
	~Core();

	Core(const Core&) = delete;
	Core& operator=(const Core&) = delete;

	// engine thread. data_path is the directory holding Stardat.mpq,
	// Broodat.mpq and Patch_rt.mpq; it may be empty for the process cwd.
	// A trailing '/' is added if missing. Returns false and fills err on
	// failure (missing/corrupt mpqs report here rather than aborting).
	bool init(const std::string& data_path, int width, int height, std::string* err);

	// engine thread. Feeds replay bytes straight from memory, so the host can
	// read a .rep through Android's SAF without materialising a real file.
	bool load_replay(const uint8_t* data, size_t len, std::string* err);

	// Any thread. Queues a switch to another replay, applied on the next tick.
	// Lets the viewer change replay without tearing down the engine, which
	// would mean reloading the mpqs and all the image data again.
	void cmd_load_replay_path(const std::string& path);

	// engine thread.
	void resize(int width, int height);

	// engine thread. Advances the simulation and draws one frame. Returns
	// false once the host should stop looping (window closed or quit queued).
	bool tick();

	// Any thread.
	void cmd_set_paused(bool paused);
	void cmd_toggle_paused();
	void cmd_set_speed(double multiplier);
	void cmd_seek_frame(int frame);
	void cmd_seek_fraction(double fraction);
	// Screen-pixel drag. Positive dx moves the camera right, so a host wanting
	// the map to follow the finger should negate the touch delta.
	void cmd_pan(int dx, int dy);

	// Selects whatever unit is under a screen-pixel point, or clears the
	// selection if there is nothing there.
	void cmd_select_at(int screen_x, int screen_y);

	// Starts keeping the unit under this point centred, or stops if that same
	// unit is already being followed. Following ends by itself when the unit
	// dies.
	void cmd_toggle_follow_at(int screen_x, int screen_y);

	// openbw draws its own console, minimap and replay slider. They are input
	// driven, so a host that owns all interaction itself should turn them off.
	void cmd_set_hud_visible(bool visible);
	// Magnification. 1.0 draws the game at native resolution; higher values
	// enlarge it, which is what makes the view legible on a phone screen.
	void cmd_set_zoom(double zoom);
	void cmd_quit();

	// Any thread.
	Status status() const;
	ReplayInfo info() const;

	// Any thread. The error that ended playback, or empty. tick() returning
	// false is ambiguous on its own — it means either a clean exit or a
	// failure — so the host reads this to tell the two apart.
	std::string last_error() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace bwreplay

#endif  // BWREPLAY_CORE_H
