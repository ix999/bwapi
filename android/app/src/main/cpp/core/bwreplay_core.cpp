#include "bwreplay_core.h"

#include "common.h"
#include "ui.h"
#include "bwgame.h"
#include "replay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <map>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

// openbw declares these in ui/common.h and expects the application to define
// them. gfxtest.cpp terminates on a fatal error; we throw instead so that a
// missing or corrupt mpq surfaces as an error message in the UI rather than
// taking the whole process down.
namespace bwgame {
namespace ui {

void log_str(a_string str) {
#ifdef __ANDROID__
	__android_log_print(ANDROID_LOG_INFO, "bwreplay", "%.*s", (int)str.size(), str.data());
#else
	fwrite(str.data(), str.size(), 1, stdout);
	fflush(stdout);
#endif
}

void fatal_error_str(a_string str) {
	log("fatal error: %s\n", str);
	throw std::runtime_error(std::string(str.data(), str.size()));
}

}  // namespace ui
}  // namespace bwgame

namespace bwreplay {

using namespace bwgame;

namespace {

// Rewind support: playback keeps periodic full copies of the simulation so a
// backwards seek can restore the nearest earlier snapshot and re-simulate
// forward from there. Same approach as gfxtest.cpp.
struct SavedState {
	state st;
	action_state action_st;
	std::array<apm_t, 12> apm;
};

// One snapshot per ~10 seconds of game time, to begin with.
constexpr int kSaveIntervalFrames = 10 * 1000 / 42;

// A full state copy is multiple megabytes, so unlike gfxtest.cpp (which keeps
// every snapshot and only frees under a malloc failure handler) playback caps
// how many it holds. On reaching the cap the interval doubles and the now
// off-interval snapshots are dropped, which halves memory while keeping the
// remaining snapshots evenly spread over the replay. Seeking stays bounded:
// the worst case is re-simulating one interval's worth of frames.
constexpr size_t kMaxSnapshots = 24;

// Wall-clock budget for a single tick, so seeking across a long replay stays
// responsive instead of blocking the render thread until it arrives.
constexpr auto kTickBudget = std::chrono::milliseconds(50);

enum class CmdType {
	none,
	set_paused,
	toggle_paused,
	set_speed,
	seek_frame,
	seek_fraction,
	pan,
	set_zoom,
	select_at,
	toggle_follow_at,
	set_hud_visible,
	load_replay_path,
	quit,
};

struct Cmd {
	CmdType type = CmdType::none;
	bool flag = false;
	double value = 0.0;
	int x = 0;
	int y = 0;
	std::string text;
};

// Native BW pixels are far too small on a phone, so the host magnifies by
// default. This is only the device-pixel to map-pixel conversion factor; the
// magnification itself is the host shrinking the SDL surface.
constexpr double kMinZoom = 1.0;
constexpr double kMaxZoom = 8.0;
constexpr double kDefaultZoom = 2.0;

using Loader = data_loading::data_files_loader<data_loading::mpq_file<>>;

}  // namespace

struct Core::Impl {
	std::unique_ptr<Loader> loader;
	std::unique_ptr<ui_functions> ui;

	std::chrono::high_resolution_clock clock;
	std::chrono::high_resolution_clock::time_point last_tick;

	std::map<int, std::unique_ptr<SavedState>> saved_states;
	int save_interval = kSaveIntervalFrames;

	bool initialized = false;
	bool replay_loaded = false;
	double zoom = kDefaultZoom;

	// Unit the camera is locked to. get_unit() returns null once it dies or its
	// slot is recycled, which is how following ends on its own.
	unit_id followed{};

	mutable std::mutex mutex;
	std::deque<Cmd> commands;
	Status published;
	ReplayInfo replay_info;
	std::string last_error;

	void push(Cmd c) {
		std::lock_guard<std::mutex> lock(mutex);
		commands.push_back(c);
	}

	// engine thread
	void apply_commands() {
		std::deque<Cmd> pending;
		{
			std::lock_guard<std::mutex> lock(mutex);
			pending.swap(commands);
		}
		for (const Cmd& c : pending) {
			switch (c.type) {
			case CmdType::none:
				break;
			case CmdType::set_paused:
				ui->is_paused = c.flag;
				break;
			case CmdType::toggle_paused:
				ui->is_paused = !ui->is_paused;
				break;
			case CmdType::set_speed: {
				// game_speed is fp8 (8 fractional bits). Clamp to a sane range
				// so a bad host value cannot divide by zero in tick().
				double v = std::max(0.05, std::min(64.0, c.value));
				ui->game_speed = fp8::from_raw((int)(v * 256.0));
				break;
			}
			case CmdType::seek_frame:
				ui->replay_frame = clamp_frame((int)c.value);
				break;
			case CmdType::seek_fraction:
				ui->replay_frame = clamp_frame((int)(ui->replay_st.end_frame * c.value));
				break;
			case CmdType::pan:
				// Dragging releases a followed unit. Otherwise the camera would
				// snap back to it every frame and the drag would feel dead.
				followed = unit_id{};
				// Drag distances arrive in screen pixels; convert to map pixels
				// so panning tracks the finger at any zoom level.
				ui->screen_pos = ui->screen_pos + xy((int)(c.x / zoom), (int)(c.y / zoom));
				break;
			case CmdType::set_zoom:
				// Only the input conversion factor. The actual magnification is
				// the host resizing the SDL surface.
				zoom = std::max(kMinZoom, std::min(kMaxZoom, c.value));
				break;
			case CmdType::select_at: {
				unit_t* u = ui->select_get_unit_at(screen_to_map(c.x, c.y));
				ui->current_selection_clear();
				if (u) ui->current_selection_add(u);
				break;
			}
			case CmdType::toggle_follow_at: {
				unit_t* u = ui->select_get_unit_at(screen_to_map(c.x, c.y));
				if (!u) break;
				unit_id id = ui->get_unit_id(u);
				// A second long press on the unit already being followed
				// releases it.
				followed = (followed == id) ? unit_id{} : id;
				ui->current_selection_clear();
				ui->current_selection_add(u);
				break;
			}
			case CmdType::set_hud_visible:
				ui->draw_ui_elements = c.flag;
				break;
			case CmdType::load_replay_path: {
				std::string err;
				if (!load_replay_file(c.text, &err)) {
					ui::log("could not load replay: %s\n", a_string(err.c_str()));
					std::lock_guard<std::mutex> lock(mutex);
					last_error = err;
				}
				// The rest of this batch was aimed at the previous replay.
				return;
			}
			case CmdType::quit:
				ui->window_closed = true;
				break;
			}
		}
	}

	// engine thread. Swaps in a new replay, keeping the engine and all the
	// loaded image data in place.
	bool do_load_replay(const uint8_t* data, size_t len, std::string* err) {
		try {
			saved_states.clear();
			save_interval = kSaveIntervalFrames;
			ui->reset();
			ui->load_replay_data(data, len);
			ui->set_image_data();

			ui->replay_frame = 0;
			ui->is_paused = false;

			// Start centred on the map rather than in the top-left corner.
			ui->screen_pos = xy((int)ui->game_st.map_width / 2 - (int)ui->screen_width / 2,
			                    (int)ui->game_st.map_height / 2 - (int)ui->screen_height / 2);

			ReplayInfo info;
			info.end_frame = ui->replay_st.end_frame;
			info.map_name = std::string(ui->replay_st.map_name.data(), ui->replay_st.map_name.size());
			for (auto& name : ui->replay_st.player_name) {
				if (name.empty()) continue;
				info.player_names.push_back(std::string(name.data(), name.size()));
			}

			{
				std::lock_guard<std::mutex> lock(mutex);
				replay_info = std::move(info);
				// Drop anything still queued: a seek aimed at the previous
				// replay means nothing against this one.
				commands.clear();
				last_error.clear();
			}

			replay_loaded = true;
			last_tick = clock.now();
			publish();
			return true;
		} catch (const std::exception& e) {
			if (err) *err = e.what();
			replay_loaded = false;
			return false;
		}
	}

	// engine thread
	bool load_replay_file(const std::string& path, std::string* err) {
		std::ifstream f(path, std::ios::binary);
		if (!f) {
			if (err) *err = "could not open " + path;
			return false;
		}
		f.seekg(0, std::ios::end);
		std::streamoff len = f.tellg();
		if (len <= 0) {
			if (err) *err = "empty replay: " + path;
			return false;
		}
		f.seekg(0, std::ios::beg);
		std::vector<uint8_t> bytes((size_t)len);
		f.read(reinterpret_cast<char*>(bytes.data()), len);
		if (!f) {
			if (err) *err = "could not read " + path;
			return false;
		}
		return do_load_replay(bytes.data(), bytes.size(), err);
	}

	// engine thread. Touch coordinates are screen pixels; the view is magnified
	// by `zoom`, so divide before offsetting into map space.
	xy screen_to_map(int screen_x, int screen_y) const {
		return ui->screen_pos + xy((int)(screen_x / zoom), (int)(screen_y / zoom));
	}

	// engine thread. Keeps a followed unit centred. Returns false once the unit
	// is gone, which releases the camera.
	bool apply_follow() {
		if (followed.index() == 0) return false;
		unit_t* u = ui->get_unit(followed);
		if (!u) {
			followed = unit_id{};
			return false;
		}
		ui->screen_pos = u->position - xy((int)ui->view_width / 2, (int)ui->view_height / 2);
		return true;
	}

	// engine thread. Magnification is NOT done with openbw's view_scale: that
	// field is not applied to rendering at all. screen_tile_bounds() uses
	// view_width/view_height to choose which tiles to draw, and update() ends
	// with a 1:1 rgba->window blit, so shrinking the view simply left most of
	// the window undrawn. The host magnifies by giving SDL a smaller surface and
	// letting the compositor scale it up, which keeps openbw rendering 1:1.
	//
	// `zoom` is still tracked here because input arrives in device pixels and has
	// to be converted to map pixels.
	void centre_on(xy map_point) {
		if (!ui) return;
		ui->screen_pos = map_point - xy((int)ui->view_width / 2, (int)ui->view_height / 2);
	}

	xy view_centre() const {
		return ui->screen_pos + xy((int)ui->view_width / 2, (int)ui->view_height / 2);
	}

	int clamp_frame(int frame) const {
		if (frame < 0) return 0;
		if (frame > ui->replay_st.end_frame) return ui->replay_st.end_frame;
		return frame;
	}

	// engine thread. Halve the snapshot count by doubling the interval and
	// dropping everything that no longer lands on it. Frame 0 is a multiple of
	// every interval, so the replay start is never evicted.
	void thin_snapshots() {
		save_interval *= 2;
		for (auto it = saved_states.begin(); it != saved_states.end();) {
			if (it->first % save_interval != 0) it = saved_states.erase(it);
			else ++it;
		}
	}

	// engine thread. Advance exactly one simulation frame, snapshotting first
	// if this frame is a checkpoint.
	void next_frame() {
		if (ui->st.current_frame % save_interval == 0) {
			if (saved_states.find(ui->st.current_frame) == saved_states.end()) {
				auto v = std::make_unique<SavedState>();
				v->st = copy_state(ui->st);
				v->action_st = copy_state(ui->action_st, ui->st, v->st);
				v->apm = ui->apm;
				saved_states[ui->st.current_frame] = std::move(v);
				if (saved_states.size() > kMaxSnapshots) thin_snapshots();
			}
		}
		ui->replay_functions::next_frame();
		for (auto& v : ui->apm) v.update(ui->st.current_frame);
	}

	// engine thread. Simulation half of a tick; mirrors main_t::update() in
	// gfxtest.cpp.
	void advance() {
		auto now = clock.now();
		auto tick_speed = std::chrono::milliseconds((fp8::integer(42) / ui->game_speed).integer_part());

		if (ui->is_done() && ui->st.current_frame == ui->replay_frame) return;

		if (ui->st.current_frame != ui->replay_frame) {
			// Seek. Restore the nearest snapshot at or before the target when
			// we need to go backwards, then re-simulate forwards in bounded
			// chunks so one tick cannot stall the UI.
			auto i = saved_states.lower_bound(ui->replay_frame);
			if (i != saved_states.begin()) --i;
			if (i != saved_states.end()) {
				auto& v = i->second;
				if (ui->st.current_frame > ui->replay_frame || v->st.current_frame > ui->st.current_frame) {
					ui->st = copy_state(v->st);
					ui->action_st = copy_state(v->action_st, v->st, ui->st);
					ui->apm = v->apm;
				}
			}
			if (ui->st.current_frame < ui->replay_frame) {
				for (size_t n = 0; n != 32 && ui->st.current_frame != ui->replay_frame; ++n) {
					for (size_t k = 0; k != 4 && ui->st.current_frame != ui->replay_frame; ++k) {
						next_frame();
					}
					if (clock.now() - now >= kTickBudget) break;
				}
			}
			last_tick = now;
			return;
		}

		if (ui->is_paused) {
			last_tick = now;
			return;
		}

		// Normal playback: run however many simulation frames the elapsed wall
		// time calls for, capped so a stall cannot snowball.
		auto elapsed = now - last_tick;
		if (elapsed >= tick_speed * 16) {
			last_tick = now - tick_speed * 16;
			elapsed = tick_speed * 16;
		}
		auto count = tick_speed.count() == 0 ? 128 : elapsed / tick_speed;
		for (auto n = count; n;) {
			--n;
			last_tick += tick_speed;
			if (ui->is_done()) break;
			next_frame();
			if (n % 4 == 3 && clock.now() - now >= kTickBudget) break;
		}
		ui->replay_frame = ui->st.current_frame;
	}

	// engine thread
	void publish() {
		std::lock_guard<std::mutex> lock(mutex);
		published.current_frame = ui ? ui->st.current_frame : 0;
		published.end_frame = ui ? ui->replay_st.end_frame : 0;
		published.paused = ui ? ui->is_paused : false;
		published.speed = ui ? (double)ui->game_speed.raw_value / 256.0 : 1.0;
		published.replay_loaded = replay_loaded;
		published.done = ui ? ui->is_done() : false;
		published.quit_requested = ui ? ui->window_closed : false;
		published.zoom = zoom;
		published.following = followed.index() != 0;
	}
};

Core::Core() : impl_(new Impl()) {}
Core::~Core() = default;

bool Core::init(const std::string& data_path, int width, int height, std::string* err) {
	try {
		impl_->loader.reset(new Loader(data_loading::data_files_directory<Loader>(a_string(data_path.c_str()))));

		auto load_data_file = [this](a_vector<uint8_t>& dst, a_string filename) {
			(*impl_->loader)(dst, std::move(filename));
		};

		game_player player;
		player.init(load_data_file);

		impl_->ui.reset(new ui_functions(std::move(player)));
		ui_functions& ui = *impl_->ui;

		// Never let the engine call std::exit() out from under the host.
		ui.exit_on_close = false;

		ui.load_all_image_data(load_data_file);
		ui.load_data_file = load_data_file;
		ui.init();

		ui.wnd.create("OpenBW Replays", 0, 0, width, height);
		ui.resize(width, height);

		impl_->last_tick = impl_->clock.now();
		impl_->initialized = true;
		impl_->publish();
		return true;
	} catch (const std::exception& e) {
		if (err) *err = e.what();
		impl_->ui.reset();
		impl_->loader.reset();
		return false;
	}
}

bool Core::load_replay(const uint8_t* data, size_t len, std::string* err) {
	if (!impl_->initialized) {
		if (err) *err = "engine not initialized";
		return false;
	}
	return impl_->do_load_replay(data, len, err);
}

void Core::cmd_load_replay_path(const std::string& path) {
	Cmd c{CmdType::load_replay_path};
	c.text = path;
	impl_->push(c);
}

void Core::resize(int width, int height) {
	if (!impl_->ui) return;
	impl_->ui->resize(width, height);
}

bool Core::tick() {
	if (!impl_->initialized) return false;
	try {
		impl_->apply_commands();
		if (impl_->replay_loaded) impl_->advance();
		// After the simulation moves, before the frame is drawn, so a followed
		// unit stays centred rather than lagging a frame behind.
		impl_->apply_follow();
		size_t was_width = impl_->ui->screen_width;
		size_t was_height = impl_->ui->screen_height;
		const xy centre_before = impl_->view_centre();
		impl_->ui->update();
		// update() handles surface resizes itself. The host resizes the surface
		// to zoom, so hold the map centre across it rather than the top-left
		// corner, which would slide the view on every zoom step.
		if (impl_->ui->screen_width != was_width || impl_->ui->screen_height != was_height) {
			impl_->centre_on(centre_before);
		}
		impl_->publish();
		return !impl_->ui->window_closed;
	} catch (const std::exception& e) {
		ui::log("tick failed: %s\n", a_string(e.what()));
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->last_error = e.what();
		return false;
	}
}

void Core::cmd_set_paused(bool paused) {
	Cmd c{CmdType::set_paused};
	c.flag = paused;
	impl_->push(c);
}

void Core::cmd_toggle_paused() { impl_->push(Cmd{CmdType::toggle_paused}); }

void Core::cmd_set_speed(double multiplier) {
	Cmd c{CmdType::set_speed};
	c.value = multiplier;
	impl_->push(c);
}

void Core::cmd_seek_frame(int frame) {
	Cmd c{CmdType::seek_frame};
	c.value = frame;
	impl_->push(c);
}

void Core::cmd_seek_fraction(double fraction) {
	Cmd c{CmdType::seek_fraction};
	c.value = std::max(0.0, std::min(1.0, fraction));
	impl_->push(c);
}

void Core::cmd_pan(int dx, int dy) {
	Cmd c{CmdType::pan};
	c.x = dx;
	c.y = dy;
	impl_->push(c);
}

void Core::cmd_select_at(int screen_x, int screen_y) {
	Cmd c{CmdType::select_at};
	c.x = screen_x;
	c.y = screen_y;
	impl_->push(c);
}

void Core::cmd_toggle_follow_at(int screen_x, int screen_y) {
	Cmd c{CmdType::toggle_follow_at};
	c.x = screen_x;
	c.y = screen_y;
	impl_->push(c);
}

void Core::cmd_set_hud_visible(bool visible) {
	Cmd c{CmdType::set_hud_visible};
	c.flag = visible;
	impl_->push(c);
}

void Core::cmd_set_zoom(double zoom) {
	Cmd c{CmdType::set_zoom};
	c.value = zoom;
	impl_->push(c);
}

void Core::cmd_quit() { impl_->push(Cmd{CmdType::quit}); }

Status Core::status() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->published;
}

ReplayInfo Core::info() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->replay_info;
}

std::string Core::last_error() const {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	return impl_->last_error;
}

}  // namespace bwreplay
