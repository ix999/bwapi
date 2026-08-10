// Desktop smoke test for the shared replay core.
//
// Exercises the same calls the Android JNI bridge makes: init the engine
// against a directory of mpqs, feed a replay in from memory, then drive the
// tick loop while issuing seek/pause/speed commands. Run it headless with
// SDL_VIDEODRIVER=dummy.

#include "bwreplay_core.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

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

void print_status(const char* label, const bwreplay::Core& core) {
	bwreplay::Status s = core.status();
	printf("%-18s frame=%d/%d paused=%d speed=%.2f done=%d\n", label, s.current_frame, s.end_frame,
	       (int)s.paused, s.speed, (int)s.done);
}

}  // namespace

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <data-dir-with-mpqs> <replay.rep>\n", argv[0]);
		return 2;
	}
	const std::string data_dir = argv[1];
	const std::string replay_path = argv[2];

	bwreplay::Core core;

	std::string err;
	if (!core.init(data_dir, 640, 480, &err)) {
		fprintf(stderr, "init failed: %s\n", err.c_str());
		return 1;
	}
	printf("engine initialized against %s\n", data_dir.c_str());

	std::vector<uint8_t> replay;
	if (!read_file(replay_path, &replay)) {
		fprintf(stderr, "could not read %s\n", replay_path.c_str());
		return 1;
	}

	if (!core.load_replay(replay.data(), replay.size(), &err)) {
		fprintf(stderr, "load_replay failed: %s\n", err.c_str());
		return 1;
	}

	bwreplay::ReplayInfo info = core.info();
	printf("map: %s\n", info.map_name.c_str());
	for (const std::string& name : info.player_names) printf("player: %s\n", name.c_str());
	printf("end frame: %d\n", info.end_frame);

	// Play forward.
	for (int i = 0; i < 120; ++i) {
		if (!core.tick()) break;
	}
	print_status("after playback", core);

	// Seek to the middle, then let the engine catch up.
	core.cmd_seek_fraction(0.5);
	for (int i = 0; i < 240; ++i) {
		if (!core.tick()) break;
	}
	print_status("after seek 50%", core);

	// Rewind, which forces a snapshot restore rather than forward simulation.
	core.cmd_seek_fraction(0.1);
	for (int i = 0; i < 240; ++i) {
		if (!core.tick()) break;
	}
	print_status("after rewind 10%", core);

	// Pause and confirm the frame stops advancing.
	core.cmd_set_paused(true);
	for (int i = 0; i < 30; ++i) core.tick();
	int paused_at = core.status().current_frame;
	for (int i = 0; i < 30; ++i) core.tick();
	if (core.status().current_frame != paused_at) {
		fprintf(stderr, "FAIL: frame advanced while paused (%d -> %d)\n", paused_at,
		        core.status().current_frame);
		return 1;
	}
	print_status("after pause", core);

	// Speed change should be reflected in the published status.
	core.cmd_set_paused(false);
	core.cmd_set_speed(8.0);
	core.tick();
	print_status("after speed 8x", core);

	// Seek near the end, which simulates almost the entire replay and so
	// forces the snapshot cap to kick in repeatedly. Memory is expected to stay
	// bounded; run under a memory profiler to confirm.
	core.cmd_set_paused(true);
	core.cmd_seek_fraction(0.95);
	for (int i = 0; i < 4000; ++i) {
		if (!core.tick()) break;
		if (core.status().current_frame >= (int)(core.status().end_frame * 0.95)) break;
	}
	print_status("after seek 95%", core);

	const int target = (int)(core.status().end_frame * 0.95);
	if (core.status().current_frame < target) {
		fprintf(stderr, "FAIL: never reached 95%% (%d < %d)\n", core.status().current_frame, target);
		return 1;
	}

	// Swapping replays without tearing the engine down is what the viewer's
	// picker does. Verify the engine actually rebinds to the new replay rather
	// than carrying the old one's state forward.
	if (argc >= 4) {
		const std::string second_path = argv[3];
		const int first_end = core.status().end_frame;
		const std::string first_map = core.info().map_name;

		core.cmd_load_replay_path(second_path);
		for (int i = 0; i < 60; ++i) {
			if (!core.tick()) break;
		}

		bwreplay::ReplayInfo swapped = core.info();
		printf("swapped to: %s (end frame %d)\n", swapped.map_name.c_str(), swapped.end_frame);
		print_status("after swap", core);

		if (swapped.end_frame == 0) {
			fprintf(stderr, "FAIL: swapped replay reports no frames\n");
			return 1;
		}
		if (core.status().current_frame > swapped.end_frame) {
			fprintf(stderr, "FAIL: playhead past the end of the swapped replay\n");
			return 1;
		}
		if (swapped.end_frame == first_end && swapped.map_name == first_map) {
			fprintf(stderr, "WARN: swapped replay looks identical to the first\n");
		}
		// A stale seek from the previous replay must not survive the swap.
		if (core.status().current_frame > swapped.end_frame / 2) {
			fprintf(stderr, "FAIL: swap did not reset the playhead (%d)\n",
			        core.status().current_frame);
			return 1;
		}
	}

	printf("OK\n");
	return 0;
}
