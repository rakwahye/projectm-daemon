APP_ID ?= projectm-daemon

PROJECTM_PC := projectM-4

VIS_SRCS    := visualizer_projectm.c visualizer_null.c
VIS_FACTORY := visualizer_projectm_create
VIS_CFLAGS  := $(shell pkg-config --cflags $(PROJECTM_PC) 2>/dev/null)
VIS_LDFLAGS := $(subst -l:,-l,$(shell pkg-config --libs $(PROJECTM_PC) 2>/dev/null))

DAEMON_SRC_NAMES := \
    main.c wiring.c wiring_render.c pending.c loop.c renderer.c renderer_gbm.c renderer_surfaceless.c \
    audio.c config.c ipc.c overlay.c art_decode.c filter.c nowplaying.c info_views.c gl_quad.c \
    color.c coord.c visualizer.c playlist.c scene_router.c scene.c render_params.c presets.c backend.c \
    app_paths.c module_registry.c cli.c util.c \
    wayland.c wayland_bringup.c headless.c

DAEMON_HDR_NAMES := \
    renderer.h renderer_platform.h output.h loop.h wiring.h wiring_render.h pending.h backend_bringup.h cli.h \
    audio.h config.h ipc.h overlay.h art_decode.h filter.h nowplaying.h info_views.h gl_quad.h \
    color.h coord.h visualizer.h scene_router.h playlist.h scene.h render_params.h presets.h backend.h \
    app_paths.h module_registry.h util.h runtime.h wayland.h layer.h

BUILD_DEFINES :=
BACKEND_CFLAGS  :=
BACKEND_LDFLAGS :=
EXTRA_PROTOS   =

TESTS := test-color test-module_registry test-wiring_render test-audio test-config-router test-overlay test-art_decode test-scene test-filter test-ipc test-render_params test-presets test-playlist test-backend test-cli test-visualizer-null test-visualizer-select test-scene_router test-info_views
SANITIZERS := test-asan test-tsan
SAN_SCRIPT := sanitize-smoke.sh
SMOKE_SCRIPT := smoke-headless.sh

test-color: $(TEST_BIN_DIR)/test-color
	@echo "[test] running test-color"
	@$(TEST_BIN_DIR)/test-color

$(TEST_BIN_DIR)/test-color: $(SRC_DIR)/color.c $(SRC_DIR)/color.h $(TEST_DIR)/color_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-color"
	@$(CC) $(TEST_CFLAGS) -o $@ $(SRC_DIR)/color.c $(TEST_DIR)/color_test.c $(TEST_LDFLAGS)

test-visualizer-null: $(TEST_BIN_DIR)/test-visualizer-null
	@echo "[test] running test-visualizer-null"
	@$(TEST_BIN_DIR)/test-visualizer-null

$(TEST_BIN_DIR)/test-visualizer-null: $(SRC_DIR)/visualizer_null.c $(SRC_DIR)/visualizer.h $(TEST_DIR)/visualizer_null_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-visualizer-null"
	@$(CC) $(TEST_CFLAGS) -o $@ $(SRC_DIR)/visualizer_null.c $(TEST_DIR)/visualizer_null_test.c $(TEST_LDFLAGS) -lGLESv2 -lm

test-visualizer-select: $(TEST_BIN_DIR)/test-visualizer-select
	@echo "[test] running test-visualizer-select"
	@$(TEST_BIN_DIR)/test-visualizer-select

$(TEST_BIN_DIR)/test-visualizer-select: $(SRC_DIR)/visualizer.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(SRC_DIR)/visualizer.h $(SRC_DIR)/module_registry.h $(TEST_DIR)/visualizer_select_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-visualizer-select"
	@$(CC) $(TEST_CFLAGS) -o $@ $(SRC_DIR)/visualizer.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(TEST_DIR)/visualizer_select_test.c $(TEST_LDFLAGS)

test-module_registry: $(TEST_BIN_DIR)/test-module_registry
	@echo "[test] running test-module_registry"
	@$(TEST_BIN_DIR)/test-module_registry

$(TEST_BIN_DIR)/test-module_registry: $(SRC_DIR)/module_registry.c $(SRC_DIR)/module_registry.h $(SRC_DIR)/util.c $(SRC_DIR)/util.h $(TEST_DIR)/module_registry_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-module_registry"
	@$(CC) $(TEST_CFLAGS) -o $@ $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(TEST_DIR)/module_registry_test.c $(TEST_LDFLAGS)

test-wiring_render: $(TEST_BIN_DIR)/test-wiring_render
	@echo "[test] running test-wiring_render"
	@$(TEST_BIN_DIR)/test-wiring_render

$(TEST_BIN_DIR)/test-wiring_render: $(SRC_DIR)/wiring_render.c $(SRC_DIR)/wiring_render.h $(SRC_DIR)/pending.h $(SRC_DIR)/runtime.h $(TEST_DIR)/wiring_render_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-wiring_render"
	@$(CC) $(TEST_CFLAGS) -o $@ $(SRC_DIR)/wiring_render.c $(TEST_DIR)/wiring_render_test.c $(TEST_LDFLAGS)

test-audio: $(TEST_BIN_DIR)/test-audio
	@echo "[test] running test-audio"
	@$(TEST_BIN_DIR)/test-audio

$(TEST_BIN_DIR)/test-audio: $(SRC_DIR)/audio.c $(SRC_DIR)/audio.h $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(SRC_DIR)/module_registry.h $(TEST_DIR)/audio_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-audio"
	@$(CC) $(TEST_CFLAGS) -o $@ $(SRC_DIR)/audio.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(TEST_DIR)/audio_test.c $(TEST_LDFLAGS) -lpthread

test-config-router: $(TEST_BIN_DIR)/test-config-router
	@echo "[test] running test-config-router"
	@$(TEST_BIN_DIR)/test-config-router

$(TEST_BIN_DIR)/test-config-router: $(SRC_DIR)/config.c $(SRC_DIR)/color.c $(SRC_DIR)/coord.c $(SRC_DIR)/app_paths.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(SRC_DIR)/audio.c $(SRC_DIR)/render_params.c $(TEST_DIR)/config_router_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-config-router"
	@$(CC) $(TEST_CFLAGS) $(shell pkg-config --cflags cairo) -o $@ \
		$(SRC_DIR)/config.c $(SRC_DIR)/color.c $(SRC_DIR)/coord.c \
		$(SRC_DIR)/app_paths.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(SRC_DIR)/audio.c \
		$(SRC_DIR)/render_params.c \
		$(TEST_DIR)/config_router_test.c $(TEST_LDFLAGS) -lpthread

OVERLAY_TEST_PROTO_C := $(BUILD_DIR)/wlr-layer-shell-unstable-v1-protocol.c \
                        $(BUILD_DIR)/xdg-shell-protocol.c
OVERLAY_TEST_PROTO_H := $(BUILD_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h \
                        $(BUILD_DIR)/xdg-shell-client-protocol.h
test-overlay: $(TEST_BIN_DIR)/test-overlay
	@echo "[test] running test-overlay"
	@$(TEST_BIN_DIR)/test-overlay

$(TEST_BIN_DIR)/test-overlay: $(SRC_DIR)/overlay.c $(SRC_DIR)/overlay.h \
		$(SRC_DIR)/art_decode.c $(SRC_DIR)/art_decode.h \
		$(SRC_DIR)/gl_quad.c $(SRC_DIR)/color.c $(SRC_DIR)/coord.c \
		$(SRC_DIR)/app_paths.c $(SRC_DIR)/app_paths.h \
		$(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(SRC_DIR)/module_registry.h \
		$(OVERLAY_TEST_PROTO_C) $(OVERLAY_TEST_PROTO_H) \
		$(TEST_DIR)/overlay_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-overlay"
	@$(CC) $(TEST_CFLAGS) -I$(BUILD_DIR) $(shell pkg-config --cflags cairo wayland-client) -o $@ \
		$(SRC_DIR)/overlay.c $(SRC_DIR)/art_decode.c $(SRC_DIR)/gl_quad.c $(SRC_DIR)/color.c \
		$(SRC_DIR)/coord.c $(SRC_DIR)/app_paths.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(OVERLAY_TEST_PROTO_C) \
		$(TEST_DIR)/overlay_test.c \
		$(TEST_LDFLAGS) -lwayland-client -lEGL -lGLESv2 -ljpeg \
		$(shell pkg-config --libs cairo) -lm -lpthread

test-art_decode: $(TEST_BIN_DIR)/test-art_decode
	@echo "[test] running test-art_decode"
	@$(TEST_BIN_DIR)/test-art_decode

$(TEST_BIN_DIR)/test-art_decode: $(SRC_DIR)/art_decode.c $(SRC_DIR)/art_decode.h \
		$(TEST_DIR)/art_decode_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-art_decode"
	@$(CC) $(TEST_CFLAGS) $(shell pkg-config --cflags cairo) -o $@ \
		$(SRC_DIR)/art_decode.c $(TEST_DIR)/art_decode_test.c \
		$(TEST_LDFLAGS) -ljpeg $(shell pkg-config --libs cairo) -lm

test-scene: $(TEST_BIN_DIR)/test-scene
	@echo "[test] running test-scene"
	@$(TEST_BIN_DIR)/test-scene

$(TEST_BIN_DIR)/test-scene: $(SRC_DIR)/scene.c $(SRC_DIR)/scene.h $(SRC_DIR)/color.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(SRC_DIR)/module_registry.h $(TEST_DIR)/scene_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-scene"
	@$(CC) $(TEST_CFLAGS) -o $@ \
		$(SRC_DIR)/scene.c $(SRC_DIR)/color.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c \
		$(TEST_DIR)/scene_test.c $(TEST_LDFLAGS) -lGLESv2 -lm

test-scene_router: $(TEST_BIN_DIR)/test-scene_router
	@echo "[test] running test-scene_router"
	@$(TEST_BIN_DIR)/test-scene_router

$(TEST_BIN_DIR)/test-scene_router: $(SRC_DIR)/scene_router.c $(SRC_DIR)/scene_router.h $(SRC_DIR)/visualizer.h $(SRC_DIR)/gl_quad.h $(TEST_DIR)/scene_router_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-scene_router"
	@$(CC) $(TEST_CFLAGS) -o $@ \
		$(SRC_DIR)/scene_router.c $(TEST_DIR)/scene_router_test.c $(TEST_LDFLAGS) -lm

test-filter: $(TEST_BIN_DIR)/test-filter
	@echo "[test] running test-filter"
	@$(TEST_BIN_DIR)/test-filter

$(TEST_BIN_DIR)/test-filter: $(SRC_DIR)/filter.c $(SRC_DIR)/filter.h $(SRC_DIR)/color.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(SRC_DIR)/module_registry.h $(TEST_DIR)/filter_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-filter"
	@$(CC) $(TEST_CFLAGS) -o $@ \
		$(SRC_DIR)/filter.c $(SRC_DIR)/color.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c \
		$(TEST_DIR)/filter_test.c $(TEST_LDFLAGS) -lpthread

test-ipc: $(TEST_BIN_DIR)/test-ipc
	@echo "[test] running test-ipc"
	@$(TEST_BIN_DIR)/test-ipc

$(TEST_BIN_DIR)/test-ipc: $(SRC_DIR)/ipc.c $(SRC_DIR)/ipc.h $(SRC_DIR)/coord.c $(SRC_DIR)/app_paths.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(SRC_DIR)/module_registry.h $(TEST_DIR)/ipc_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-ipc"
	@$(CC) $(TEST_CFLAGS) $(shell pkg-config --cflags wayland-client) -o $@ \
		$(SRC_DIR)/ipc.c $(SRC_DIR)/coord.c $(SRC_DIR)/app_paths.c \
		$(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(TEST_DIR)/ipc_test.c \
		$(TEST_LDFLAGS) -lpthread

test-render_params: $(TEST_BIN_DIR)/test-render_params
	@echo "[test] running test-render_params"
	@$(TEST_BIN_DIR)/test-render_params

$(TEST_BIN_DIR)/test-render_params: $(SRC_DIR)/render_params.c $(SRC_DIR)/render_params.h $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(SRC_DIR)/module_registry.h $(TEST_DIR)/render_params_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-render_params"
	@$(CC) $(TEST_CFLAGS) -o $@ \
		$(SRC_DIR)/render_params.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c \
		$(TEST_DIR)/render_params_test.c $(TEST_LDFLAGS)

test-presets: $(TEST_BIN_DIR)/test-presets
	@echo "[test] running test-presets"
	@$(TEST_BIN_DIR)/test-presets

$(TEST_BIN_DIR)/test-presets: $(SRC_DIR)/presets.c $(SRC_DIR)/presets.h $(SRC_DIR)/module_registry.c $(SRC_DIR)/module_registry.h $(SRC_DIR)/util.c $(SRC_DIR)/util.h $(TEST_DIR)/presets_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-presets"
	@$(CC) $(TEST_CFLAGS) -o $@ \
		$(SRC_DIR)/presets.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c \
		$(TEST_DIR)/presets_test.c $(TEST_LDFLAGS)

test-playlist: $(TEST_BIN_DIR)/test-playlist
	@echo "[test] running test-playlist"
	@$(TEST_BIN_DIR)/test-playlist

$(TEST_BIN_DIR)/test-playlist: $(SRC_DIR)/playlist.c $(SRC_DIR)/playlist.h $(SRC_DIR)/config.c $(SRC_DIR)/app_paths.c $(SRC_DIR)/presets.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/module_registry.h $(SRC_DIR)/util.c $(SRC_DIR)/util.h $(TEST_DIR)/playlist_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-playlist"
	@$(CC) $(TEST_CFLAGS) $(shell pkg-config --cflags wayland-client) -o $@ \
		$(SRC_DIR)/playlist.c $(SRC_DIR)/config.c $(SRC_DIR)/app_paths.c $(SRC_DIR)/presets.c \
		$(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(TEST_DIR)/playlist_test.c \
		$(TEST_LDFLAGS) -lpthread

test-backend: $(TEST_BIN_DIR)/test-backend
	@echo "[test] running test-backend"
	@$(TEST_BIN_DIR)/test-backend

$(TEST_BIN_DIR)/test-backend: $(SRC_DIR)/backend.c $(SRC_DIR)/backend.h $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(SRC_DIR)/module_registry.h $(TEST_DIR)/backend_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-backend"
	@$(CC) $(TEST_CFLAGS) -o $@ \
		$(SRC_DIR)/backend.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c \
		$(TEST_DIR)/backend_test.c $(TEST_LDFLAGS)

test-cli: $(TEST_BIN_DIR)/test-cli
	@echo "[test] running test-cli"
	@$(TEST_BIN_DIR)/test-cli

$(TEST_BIN_DIR)/test-cli: $(SRC_DIR)/cli.c $(SRC_DIR)/cli.h $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(SRC_DIR)/module_registry.h $(SRC_DIR)/app_paths.c $(SRC_DIR)/app_paths.h $(TEST_DIR)/cli_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-cli"
	@$(CC) $(TEST_CFLAGS) -o $@ \
		$(SRC_DIR)/cli.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(SRC_DIR)/app_paths.c \
		$(TEST_DIR)/cli_test.c $(TEST_LDFLAGS)

test-info_views: $(TEST_BIN_DIR)/test-info_views
	@echo "[test] running test-info_views"
	@$(TEST_BIN_DIR)/test-info_views

$(TEST_BIN_DIR)/test-info_views: $(SRC_DIR)/info_views.c $(SRC_DIR)/info_views.h $(SRC_DIR)/overlay.h $(SRC_DIR)/playlist.h $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c $(SRC_DIR)/module_registry.h $(TEST_DIR)/info_views_test.c $(TEST_DIR)/check.h | $(TEST_BIN_DIR)
	@echo "[test] compiling test-info_views"
	@$(CC) $(TEST_CFLAGS) $(shell pkg-config --cflags wayland-client) -o $@ \
		$(SRC_DIR)/info_views.c $(SRC_DIR)/module_registry.c $(SRC_DIR)/util.c \
		$(TEST_DIR)/info_views_test.c $(TEST_LDFLAGS)
