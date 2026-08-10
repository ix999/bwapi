#include "bwreplay_core.h"

#include "common.h"
#include "ui.h"
#include "bwgame.h"
#include "replay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <map>

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
	quit,
};

struct Cmd {
	CmdType type = CmdType::none;
	bool flag = false;
	double value = 0.0;
	int x = 0;
	int y = 0;
};

// Native BW pixels are far too small on a phone, so the view is magnified by
// default. Bounds keep view_width/view_height from collapsing to zero.
constexpr double kMinZoom = 0.5;
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

	mutable std::mutex mutex;
	std::deque<Cmd> commands;
	Status published;
	ReplayInfo replay_info;

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
				// Drag distances arrive in screen pixels; convert to map pixels
				// so panning tracks the finger at any zoom level.
				ui->screen_pos = ui->screen_pos + xy((int)(c.x / zoom), (int)(c.y / zoom));
				break;
			case CmdType::set_zoom:
				zoom = std::max(kMinZoom, std::min(kMaxZoom, c.value));
				apply_view_scale();
				break;
			case CmdType::quit:
				ui->window_closed = true;
				break;
			}
		}
	}

	// engine thread. ui_functions::resize() hardcodes a 1:1 view scale, so
	// re-derive the view dimensions from the current zoom afterwards. Larger
	// view_scale means fewer game pixels across the window, i.e. bigger
	// sprites. view_scale is recomputed from the rounded view_width so the
	// scale the renderer uses matches the area it iterates over exactly.
	void apply_view_scale() {
		if (!ui) return;
		ui->view_scale = fp16::from_raw((int)(zoom * 65536.0));
		ui->view_width = (fp16::integer(ui->screen_width) / ui->view_scale).integer_part();
		ui->view_height = (fp16::integer(ui->screen_height) / ui->view_scale).integer_part();
		if (ui->view_width == 0) ui->view_width = 1;
		if (ui->view_height == 0) ui->view_height = 1;
		ui->view_scale = (ufp16::integer(ui->screen_width) / ui->view_width).as_signed();
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
		impl_->apply_view_scale();

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
	try {
		impl_->saved_states.clear();
		impl_->save_interval = kSaveIntervalFrames;
		impl_->ui->reset();
		impl_->ui->load_replay_data(data, len);
		impl_->ui->set_image_data();

		ui_functions& ui = *impl_->ui;
		ui.replay_frame = 0;
		ui.is_paused = false;

		// Start centred on the map rather than in the top-left corner.
		ui.screen_pos = xy((int)ui.game_st.map_width / 2 - (int)ui.screen_width / 2,
		                   (int)ui.game_st.map_height / 2 - (int)ui.screen_height / 2);

		ReplayInfo info;
		info.end_frame = ui.replay_st.end_frame;
		info.map_name = std::string(ui.replay_st.map_name.data(), ui.replay_st.map_name.size());
		for (auto& name : ui.replay_st.player_name) {
			if (name.empty()) continue;
			info.player_names.push_back(std::string(name.data(), name.size()));
		}

		{
			std::lock_guard<std::mutex> lock(impl_->mutex);
			impl_->replay_info = std::move(info);
			impl_->commands.clear();
		}

		impl_->replay_loaded = true;
		impl_->last_tick = impl_->clock.now();
		impl_->publish();
		return true;
	} catch (const std::exception& e) {
		if (err) *err = e.what();
		impl_->replay_loaded = false;
		return false;
	}
}

void Core::resize(int width, int height) {
	if (!impl_->ui) return;
	impl_->ui->resize(width, height);
	impl_->apply_view_scale();
}

bool Core::tick() {
	if (!impl_->initialized) return false;
	try {
		impl_->apply_commands();
		if (impl_->replay_loaded) impl_->advance();
		size_t was_width = impl_->ui->screen_width;
		size_t was_height = impl_->ui->screen_height;
		impl_->ui->update();
		// update() handles window resize events itself by calling resize(),
		// which restores a 1:1 view scale; re-apply the zoom when that happens.
		if (impl_->ui->screen_width != was_width || impl_->ui->screen_height != was_height) {
			impl_->apply_view_scale();
		}
		impl_->publish();
		return !impl_->ui->window_closed;
	} catch (const std::exception& e) {
		ui::log("tick failed: %s\n", a_string(e.what()));
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

}  // namespace bwreplay
