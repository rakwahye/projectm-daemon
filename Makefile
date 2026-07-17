SRC_DIR  := src
CONFIG ?= config.mk

SAN ?=
ifeq ($(SAN),)
  SAN_CFLAGS   :=
  SAN_LDFLAGS  :=
  OPT_FLAGS    := -O2
  SAN_SUBDIR   :=
else ifeq ($(SAN),asan)
  SAN_CFLAGS   := -fsanitize=address -fno-omit-frame-pointer -g
  SAN_LDFLAGS  := -fsanitize=address
  OPT_FLAGS    := -O1
  SAN_SUBDIR   := /asan
else ifeq ($(SAN),tsan)
  SAN_CFLAGS   := -fsanitize=thread -fno-omit-frame-pointer -g
  SAN_LDFLAGS  := -fsanitize=thread
  OPT_FLAGS    := -O1
  SAN_SUBDIR   := /tsan
else
  $(error SAN must be empty, 'asan', or 'tsan'; got '$(SAN)')
endif

BUILD_ROOT   := build
BUILD_DIR    := $(BUILD_ROOT)$(SAN_SUBDIR)
TEST_DIR     := tests
TEST_BIN_DIR := $(BUILD_DIR)/tests

include $(CONFIG)

REMOTE_ID := $(subst -daemon,-remote,$(APP_ID))
APP_DEFINES := -DAPP_ID=\"$(APP_ID)\" -DREMOTE_ID=\"$(REMOTE_ID)\"

VISUALIZER ?=
ifeq ($(VISUALIZER),null)
  VIS_SRCS     := visualizer_null.c
  VIS_SRCS_CXX :=
  VIS_FACTORY  := visualizer_null_create
  VIS_CFLAGS   :=
  VIS_LDFLAGS  :=
else ifneq ($(VISUALIZER),)
  $(error VISUALIZER must be 'null' or unset; got '$(VISUALIZER)')
endif
VIS_DEFINES := -DVISUALIZER_FACTORY=$(VIS_FACTORY)

ifneq ($(filter-out clean,$(or $(MAKECMDGOALS),all)),)
ifneq ($(VISUALIZER),null)
ifneq ($(PROJECTM_PC),)
ifeq ($(shell pkg-config --exists $(PROJECTM_PC) && echo yes),)
$(error pkg-config cannot find '$(PROJECTM_PC)' — install libprojectM 4.x (built with GLES=ON), or build with VISUALIZER=null)
endif
endif
endif
endif

CFLAGS   := -std=c11 -Wall -Wextra $(OPT_FLAGS) -I$(SRC_DIR) $(VIS_CFLAGS) $(BACKEND_CFLAGS) $(BUILD_DEFINES) $(APP_DEFINES) $(VIS_DEFINES) $(SAN_CFLAGS) $(shell pkg-config --cflags cairo)

CXXFLAGS := -Wall -Wextra $(OPT_FLAGS) -I$(SRC_DIR) $(VIS_CFLAGS) $(BACKEND_CFLAGS) $(BUILD_DEFINES) $(APP_DEFINES) $(VIS_DEFINES) $(SAN_CFLAGS)
DAEMON_CXX_OBJS := $(addprefix $(BUILD_DIR)/,$(VIS_SRCS_CXX:.cpp=.o))

LDFLAGS  := -lwayland-client -lgbm -lEGL -lGLESv2 $(BACKEND_LDFLAGS) \
            $(VIS_LDFLAGS) \
            -lpthread -ljpeg $(shell pkg-config --libs cairo) -lm $(SAN_LDFLAGS)

WL_SCANNER   := wayland-scanner
WL_PROTO_DIR := $(abspath $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null))

WLR_LAYER_SHELL_XML := $(firstword $(wildcard \
	protocol/wlr-layer-shell-unstable-v1.xml \
	/usr/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml))
ifeq ($(WLR_LAYER_SHELL_XML),)
$(error Cannot find wlr-layer-shell-unstable-v1.xml — restore protocol/ or install wlr-protocols)
endif

XDG_SHELL_XML := $(firstword $(wildcard \
	protocol/xdg-shell.xml \
	$(WL_PROTO_DIR)/stable/xdg-shell/xdg-shell.xml))
ifeq ($(XDG_SHELL_XML),)
$(error Cannot find xdg-shell.xml — restore protocol/ or install wayland-protocols)
endif

LINUX_DMABUF_XML := $(firstword $(wildcard \
	protocol/linux-dmabuf-unstable-v1.xml \
	$(WL_PROTO_DIR)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml \
	$(WL_PROTO_DIR)/stable/linux-dmabuf/linux-dmabuf-v1.xml))
ifeq ($(LINUX_DMABUF_XML),)
$(error Cannot find linux-dmabuf XML — restore protocol/ or install wayland-protocols)
endif

PROTOS := \
	xdg-shell|$(XDG_SHELL_XML) \
	linux-dmabuf-unstable-v1|$(LINUX_DMABUF_XML) \
	wlr-layer-shell-unstable-v1|$(WLR_LAYER_SHELL_XML) \
	$(EXTRA_PROTOS)

PROTO_NAMES := $(foreach p,$(PROTOS),$(word 1,$(subst |, ,$(p))))
PROTO_SRCS  := $(addprefix $(BUILD_DIR)/,$(addsuffix -protocol.c,$(PROTO_NAMES)))
PROTO_HDRS  := $(addprefix $(BUILD_DIR)/,$(addsuffix -client-protocol.h,$(PROTO_NAMES)))

DAEMON := $(BUILD_DIR)/$(APP_ID)
REMOTE := $(BUILD_DIR)/$(REMOTE_ID)

DAEMON_SRC_NAMES := $(DAEMON_SRC_NAMES) $(VIS_SRCS)
REMOTE_SRC_NAMES := remote.c coord.c app_paths.c
REMOTE_HDR_NAMES := coord.h app_paths.h

DAEMON_SRCS := $(addprefix $(SRC_DIR)/,$(DAEMON_SRC_NAMES))
DAEMON_HDRS := $(addprefix $(SRC_DIR)/,$(DAEMON_HDR_NAMES))
REMOTE_SRCS := $(addprefix $(SRC_DIR)/,$(REMOTE_SRC_NAMES))
REMOTE_HDRS := $(addprefix $(SRC_DIR)/,$(REMOTE_HDR_NAMES))

.DEFAULT_GOAL := all
.PHONY: all clean check-lib

all: $(DAEMON) $(REMOTE)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

define proto_rule
$(BUILD_DIR)/$(word 1,$(subst |, ,$(1)))-protocol.c: $(word 2,$(subst |, ,$(1))) | $(BUILD_DIR)
	$(WL_SCANNER) private-code $$< $$@
$(BUILD_DIR)/$(word 1,$(subst |, ,$(1)))-client-protocol.h: $(word 2,$(subst |, ,$(1))) | $(BUILD_DIR)
	$(WL_SCANNER) client-header $$< $$@
endef
$(foreach p,$(PROTOS),$(eval $(call proto_rule,$(p))))

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp $(DAEMON_HDRS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(BUILD_DIR) -c $< -o $@

$(DAEMON): $(DAEMON_SRCS) $(DAEMON_CXX_OBJS) $(DAEMON_HDRS) $(PROTO_SRCS) $(PROTO_HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(BUILD_DIR) -o $@ $(DAEMON_SRCS) $(DAEMON_CXX_OBJS) $(PROTO_SRCS) $(LDFLAGS)

$(REMOTE): $(REMOTE_SRCS) $(REMOTE_HDRS) | $(BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -O2 -I$(SRC_DIR) $(APP_DEFINES) -o $@ $(REMOTE_SRCS)

TEST_CFLAGS  := -std=c11 -Wall -Wextra -O1 -g -I$(SRC_DIR) $(APP_DEFINES) $(SAN_CFLAGS)
TEST_LDFLAGS := $(SAN_LDFLAGS)

.PHONY: test test-boundary test-sanitizers test-asan test-tsan $(TESTS)

test: test-boundary test-sanitizers

test-boundary: $(TESTS)
	@echo "[test] boundary tests: all passed"

test-sanitizers: $(SANITIZERS)

test-asan:
	@APP_ID=$(APP_ID) REMOTE_ID=$(REMOTE_ID) $(TEST_DIR)/$(SAN_SCRIPT) asan

test-tsan:
	@APP_ID=$(APP_ID) REMOTE_ID=$(REMOTE_ID) $(TEST_DIR)/$(SAN_SCRIPT) tsan

ifneq ($(SMOKE_SCRIPT),)
.PHONY: smoke-null
smoke-null:
	@$(MAKE) VISUALIZER=null
	@DAEMON_BIN=$(BUILD_DIR)/$(APP_ID) REMOTE_BIN=$(BUILD_DIR)/$(REMOTE_ID) \
		$(TEST_DIR)/$(SMOKE_SCRIPT)
endif

$(TEST_BIN_DIR):
	@mkdir -p $(TEST_BIN_DIR)

check-lib: $(DAEMON)
	@ldd $(DAEMON) | grep -i projectM || echo "(no projectM in ldd output — link failed?)"

clean:
	rm -rf $(BUILD_ROOT)
