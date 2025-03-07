GIT_COMMIT		:= $(shell git rev-parse --short HEAD || echo 'unknown')
GIT_BRANCH		:= $(shell echo $${WORKFLOW_BRANCH_OR_TAG-$$(git rev-parse --abbrev-ref HEAD || echo 'unknown')})
GIT_BRANCH_NUM	:= $(shell git rev-list --count HEAD || echo 'nan')
BUILD_DATE		:= $(shell date '+%d-%m-%Y' || echo 'unknown')
BUILD_TIME		:= $(shell date '+%H:%M:%S' || echo 'unknown')
VERSION			:= $(shell git describe --tags --abbrev=0 --exact-match 2>/dev/null || echo '0.60.2')
GIT_DIRTY_BUILD := $(shell git diff --quiet ; echo $$?)

GIT_DIRTY_SUFFIX :=-UL
ifeq (false, 1)
	GIT_DIRTY_SUFFIX := -dirty
endif

CFLAGS += \
	-DGIT_COMMIT=\"$(GIT_COMMIT)\" \
	-DGIT_BRANCH=\"$(GIT_BRANCH)\" \
	-DGIT_BRANCH_NUM=\"$(GIT_BRANCH_NUM)\" \
	-DBUILD_DATE=\"$(BUILD_DATE)\" \
	-DVERSION=\"$(VERSION)\" \
	-DTARGET=$(HARDWARE_TARGET) \
	-DBUILD_DIRTY=false

# if suffix is set in environment (by Github), use it
ifeq (${DIST_SUFFIX},)
	DIST_SUFFIX	:= local-$(GIT_COMMIT)$(GIT_DIRTY_SUFFIX)
else
	DIST_SUFFIX := ${DIST_SUFFIX}$(GIT_DIRTY_SUFFIX)
endif

#VERSION_STRING  :=  $(VERSION) ($(GIT_BRANCH) @ $(GIT_COMMIT)), built $(BUILD_DATE) $(BUILD_TIME)
VERSION_STRING  := $(DIST_SUFFIX), $(GIT_BRANCH)
