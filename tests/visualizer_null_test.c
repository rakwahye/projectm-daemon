#include <stdio.h>
#include <string.h>

#include "visualizer.h"

#include "check.h"

struct visualizer *visualizer_null_create(void);

int main(void) {
	struct visualizer *v = visualizer_null_create();
	CHECK(v != NULL, "the factory returns an instance");

	CHECK(v->init != NULL, "init is present");
	CHECK(v->destroy != NULL, "destroy is present");
	CHECK(v->apply_config != NULL, "apply_config is present");
	CHECK(v->set_window_size != NULL, "set_window_size is present");
	CHECK(v->set_fps != NULL, "set_fps is present");
	CHECK(v->set_mesh_size != NULL, "set_mesh_size is present");
	CHECK(v->render != NULL, "render is present");
	CHECK(v->load_preset != NULL, "load_preset is present");
	CHECK(v->feed_pcm != NULL, "feed_pcm is present");
	CHECK(v->soft_cut_duration != NULL, "soft_cut_duration is present");

	CHECK(v->deposit == NULL, "a visualizer with no target canvas leaves deposit NULL, forcing the caller to composite");
	CHECK(v->sprite == NULL, "sprite is reserved and stays NULL");

	CHECK(v->init(v, 1920, 1080), "bring-up succeeds with no GL context, because the stub only stores dimensions");

	/* Nothing below returns a value. They are survivability probes: an item is
	 * a plain string rather than an engine's item path, so opaque bytes and
	 * NULL must both be tolerated. Reaching the check underneath is the proof. */
	v->apply_config(v);
	v->set_window_size(v, 1280, 720);
	v->set_fps(v, 60);
	v->set_mesh_size(v, 48, 32);
	v->load_preset(v, "::not-a-preset::\x01 opaque handle \xff", true);
	v->load_preset(v, "", false);
	v->load_preset(v, NULL, true);

	float pcm[512 * 2];
	for (int i = 0; i < 512 * 2; i++) pcm[i] = (i % 7) * 0.01f - 0.03f;
	v->feed_pcm(v, pcm, 512);
	v->feed_pcm(v, NULL, 0);

	CHECK(v->soft_cut_duration(v) == 0.0, "a visualizer with no notion of cuts reports a zero soft-cut duration");

	v->destroy(v);

	CHECK_PASS("test-visualizer-null");
}
