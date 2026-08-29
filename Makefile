.PHONY: 3rdparty audiofile libgcrypt FuseGenerator FuseImporter mbedtls libssh2 clean-3rdparty list-teams fusex archive dist dmg appcast appcast-push release release-clean release-notarize release-export clean

# Signing parameters (can be overridden from command line)
# By default, use the signing settings already stored in the Xcode projects.
# Example override: make 3rdparty DEVELOPMENT_TEAM="ABC123DEFG" CODE_SIGN_IDENTITY="Apple Development"
DEVELOPMENT_TEAM ?=
CODE_SIGN_IDENTITY ?=
CODE_SIGN_STYLE ?=
PROVISIONING_PROFILE_SPECIFIER ?=

# Archive path (can be overridden from command line)
# Example: make archive ARCHIVE_PATH=./build/FuseX.xcarchive
ARCHIVE_PATH ?= ./build/FuseX.xcarchive

# Release paths and notarization profile
DIST_APP_PATH ?= dist/FuseX.app
ARCHIVE_APP_PATH = $(ARCHIVE_PATH)/Products/Applications/FuseX.app
RELEASE_ZIP_PATH ?= ./build/FuseX-notarize.zip
NOTARYTOOL_PROFILE ?=
FUSEX_VERSION ?= $(shell awk -F'= ' '/MARKETING_VERSION = / { gsub(/[";]/, "", $$2); print $$2; exit }' fusepb/FuseX.xcodeproj/project.pbxproj)
DMG_NAME ?= FuseX-$(FUSEX_VERSION).dmg
APPCAST_DIR ?= ./build/appcast
APPCAST_ARCHIVE ?= ./$(DMG_NAME)
APPCAST_OUTPUT ?= appcast.xml
APPCAST_URL_PREFIX ?= https://github.com/speccytools/fusex/releases/download/$(FUSEX_VERSION)/
APPCAST_PUSH_DIR ?= ../speccytools.github.io
SPARKLE_GENERATE_APPCAST ?= $(shell find "$(HOME)/Library/Developer/Xcode/DerivedData" -path "*/SourcePackages/artifacts/sparkle/Sparkle/bin/generate_appcast" -type f 2>/dev/null | head -1)
FUSEGENERATOR_DEVELOPMENT_TEAM ?= $(if $(DEVELOPMENT_TEAM),$(DEVELOPMENT_TEAM),48UV5KV3FB)
FUSEGENERATOR_CODE_SIGN_IDENTITY ?= $(if $(CODE_SIGN_IDENTITY),$(CODE_SIGN_IDENTITY),Developer ID Application)
FUSEGENERATOR_CODE_SIGN_STYLE ?= $(if $(CODE_SIGN_STYLE),$(CODE_SIGN_STYLE),Manual)

# Automatically set CODE_SIGN_STYLE=Manual if a code signing identity is specified
ifneq ($(CODE_SIGN_IDENTITY),)
	CODE_SIGN_STYLE := Manual
endif

# Build common xcodebuild arguments for signing
XCODEBUILD_SIGN_ARGS :=
ifneq ($(DEVELOPMENT_TEAM),)
	XCODEBUILD_SIGN_ARGS += DEVELOPMENT_TEAM="$(DEVELOPMENT_TEAM)"
endif
ifneq ($(CODE_SIGN_IDENTITY),)
	XCODEBUILD_SIGN_ARGS += CODE_SIGN_IDENTITY="$(CODE_SIGN_IDENTITY)"
	XCODEBUILD_SIGN_ARGS += CODE_SIGN_IDENTITY_ALL="$(CODE_SIGN_IDENTITY)"
endif
ifneq ($(CODE_SIGN_STYLE),)
	XCODEBUILD_SIGN_ARGS += CODE_SIGN_STYLE="$(CODE_SIGN_STYLE)"
endif
ifneq ($(PROVISIONING_PROFILE_SPECIFIER),)
	XCODEBUILD_SIGN_ARGS += PROVISIONING_PROFILE_SPECIFIER="$(PROVISIONING_PROFILE_SPECIFIER)"
endif

all: 3rdparty

fusepb:
	cd fusepb && make clean && make

3rdparty: audiofile libgcrypt FuseGenerator FuseImporter mbedtls libssh2

audiofile:
	@echo "Building audiofile Framework..."
	cd 3rdparty/audiofile && \
	xcodebuild -project audiofile.xcodeproj \
		-target "audiofile Framework" \
		-configuration Deployment \
		SYMROOT=build \
		BUILD_DIR=build \
		CONFIGURATION_BUILD_DIR=build/Deployment \
		$(XCODEBUILD_SIGN_ARGS)

libgcrypt:
	@echo "Building gcrypt Framework..."
	cd 3rdparty/libgcrypt && \
	xcodebuild -project libgcrypt.xcodeproj \
		-target "gcrypt Framework" \
		-configuration Deployment \
		SYMROOT=build \
		BUILD_DIR=build \
		CONFIGURATION_BUILD_DIR=build/Deployment \
		$(XCODEBUILD_SIGN_ARGS)

FuseGenerator:
	@echo "Building Quick Look extensions..."
	cd 3rdparty/FuseGenerator && \
	xcodebuild -project FuseGenerator.xcodeproj \
		-target "FuseQLThumbnail" \
		-target "FuseQLPreview" \
		-parallelizeTargets \
		-configuration Deployment \
		SYMROOT=build \
		BUILD_DIR=build \
		CONFIGURATION_BUILD_DIR=build/Deployment \
		DEVELOPMENT_TEAM="$(FUSEGENERATOR_DEVELOPMENT_TEAM)" \
		CODE_SIGN_IDENTITY="$(FUSEGENERATOR_CODE_SIGN_IDENTITY)" \
		CODE_SIGN_STYLE="$(FUSEGENERATOR_CODE_SIGN_STYLE)" \
		$(XCODEBUILD_SIGN_ARGS)

FuseImporter:
	@echo "Building FuseImporter Framework..."
	cd 3rdparty/FuseImporter && \
	xcodebuild -project FuseImporter.xcodeproj \
		-target "FuseImporter" \
		-configuration Deployment \
		SYMROOT=build \
		BUILD_DIR=build \
		CONFIGURATION_BUILD_DIR=build/Deployment \
		$(XCODEBUILD_SIGN_ARGS)

mbedtls:
	@echo "Building mbedtls Framework..."
	cd 3rdparty/mbedtls && \
	xcodebuild -project mbedtls.xcodeproj \
		-target "mbedtls" \
		-configuration Deployment \
		SYMROOT=build \
		BUILD_DIR=build \
		CONFIGURATION_BUILD_DIR=build/Deployment \
		$(XCODEBUILD_SIGN_ARGS)

libssh2: mbedtls
	@echo "Building libssh2 Framework..."
	cd 3rdparty/libssh2 && \
	xcodebuild -project libssh2.xcodeproj \
		-target "libssh2" \
		-configuration Deployment \
		SYMROOT=build \
		BUILD_DIR=build \
		CONFIGURATION_BUILD_DIR=build/Deployment \
		$(XCODEBUILD_SIGN_ARGS)

list-teams:
	@echo "Available code signing identities and development teams:"
	@echo ""
	@if security find-identity -v -p codesigning 2>/dev/null | grep -q "valid identities found"; then \
		echo "Unique Development Team IDs:"; \
		security find-identity -v -p codesigning 2>/dev/null | \
		grep -oE '\([A-Z0-9]{10}\)' | \
		sed 's/[()]//g' | \
		sort -u | \
		while read team; do \
			echo "  $$team"; \
		done; \
		echo ""; \
		echo "Full identity list:"; \
		security find-identity -v -p codesigning 2>/dev/null; \
	else \
		echo "No code signing identities found. Make sure you have certificates installed in your keychain."; \
	fi

fusex: 3rdparty fusepb
	@echo "Building FuseX app..."
	cd fusepb && \
	xcodebuild -project FuseX.xcodeproj \
		-target FuseX \
		-configuration Deployment \
		SYMROOT=build \
		BUILD_DIR=build \
		CONFIGURATION_BUILD_DIR=build/Deployment \
		$(XCODEBUILD_SIGN_ARGS)

release-clean:
	@echo "Cleaning FuseX build folder..."
	@rm -rf fusepb/build
	@rm -rf build
	@echo "Removing existing app bundle from dist/..."
	@rm -rf "$(DIST_APP_PATH)"

archive: 3rdparty fusepb
	@echo "Archiving FuseX app..."
	@mkdir -p build
	cd fusepb && \
	xcodebuild archive \
		-project FuseX.xcodeproj \
		-scheme FuseX \
		-configuration Deployment \
		-archivePath "$(CURDIR)/$(ARCHIVE_PATH)" \
		SYMROOT=build \
		BUILD_DIR=build \
		CONFIGURATION_BUILD_DIR=build/Deployment \
			$(XCODEBUILD_SIGN_ARGS)
	@echo "Archive created at: $(ARCHIVE_PATH)"

release-notarize: archive
	@echo "Submitting archived app for notarization..."
	@if [ -z "$(NOTARYTOOL_PROFILE)" ]; then \
		echo "Error: NOTARYTOOL_PROFILE is required for make release"; \
		echo "Create one with: xcrun notarytool store-credentials <profile> --apple-id ... --team-id ... --password ..."; \
		exit 1; \
	fi
	@if [ ! -d "$(ARCHIVE_APP_PATH)" ]; then \
		echo "Error: archived app not found at $(ARCHIVE_APP_PATH)"; \
		exit 1; \
	fi
	@mkdir -p build
	@rm -f "$(RELEASE_ZIP_PATH)"
	@ditto -c -k --keepParent "$(ARCHIVE_APP_PATH)" "$(RELEASE_ZIP_PATH)"
	@xcrun notarytool submit "$(RELEASE_ZIP_PATH)" --keychain-profile "$(NOTARYTOOL_PROFILE)" --wait
	@xcrun stapler staple "$(ARCHIVE_APP_PATH)"

release-export: release-notarize
	@echo "Exporting notarized app to dist/..."
	@mkdir -p dist
	@rm -rf "$(DIST_APP_PATH)"
	@ditto "$(ARCHIVE_APP_PATH)" "$(DIST_APP_PATH)"

release: release-clean release-export
	@$(MAKE) dmg

dist:
	@echo "Checking for dist/FuseX.app..."
	@if [ ! -d "dist/FuseX.app" ]; then \
		echo "Error: dist/FuseX.app not found. Please copy FuseX.app to dist/ first."; \
		exit 1; \
	fi
	@echo "dist/FuseX.app found."

dmg: dist
	@echo "Creating $(DMG_NAME)..."
	@if ! command -v create-dmg >/dev/null 2>&1; then \
		echo "Error: create-dmg is not installed. Install it with: brew install create-dmg"; \
		exit 1; \
	fi
	@rm -f "$(DMG_NAME)"
	@if create-dmg \
		--volname "FuseX" \
		--window-pos 200 120 \
		--window-size 600 300 \
		--icon-size 100 \
		--icon "FuseX.app" 175 120 \
		--hide-extension "FuseX.app" \
		--app-drop-link 425 120 \
		"$(DMG_NAME)" \
		dist/; then \
		if [ -f "$(DMG_NAME)" ]; then \
			echo "DMG created successfully: $(DMG_NAME)"; \
		else \
			echo "Error: DMG file was not created"; \
			exit 1; \
		fi \
	else \
		echo "Error: create-dmg failed"; \
		exit 1; \
	fi

appcast:
	@echo "Generating Sparkle appcast..."
	@if [ -z "$(SPARKLE_GENERATE_APPCAST)" ] || [ ! -x "$(SPARKLE_GENERATE_APPCAST)" ]; then \
		echo "Error: Sparkle generate_appcast tool not found."; \
		echo "Build/resolve Sparkle first, or pass SPARKLE_GENERATE_APPCAST=/path/to/generate_appcast"; \
		exit 1; \
	fi
	@if [ ! -f "$(APPCAST_ARCHIVE)" ]; then \
		echo "Error: update archive not found: $(APPCAST_ARCHIVE)"; \
		echo "Create it first with: make dmg"; \
		exit 1; \
	fi
	@url_prefix="$(APPCAST_URL_PREFIX)"; \
	if [ -z "$$url_prefix" ]; then \
		echo "Error: APPCAST_URL_PREFIX is required"; \
		exit 1; \
	fi; \
	rm -rf "$(APPCAST_DIR)"; \
	mkdir -p "$(APPCAST_DIR)"; \
	cp -f "$(APPCAST_ARCHIVE)" "$(APPCAST_DIR)/"; \
	"$(SPARKLE_GENERATE_APPCAST)" \
		--download-url-prefix "$$url_prefix" \
		-o "$(APPCAST_DIR)/$(APPCAST_OUTPUT)" \
		"$(APPCAST_DIR)"; \
	echo "Appcast generated: $(APPCAST_DIR)/$(APPCAST_OUTPUT)"

appcast-push: appcast
	@echo "Publishing Sparkle appcast..."
	@if [ ! -d "$(APPCAST_PUSH_DIR)/.git" ]; then \
		echo "Error: appcast push repository not found: $(APPCAST_PUSH_DIR)"; \
		exit 1; \
	fi
	@if [ ! -f "$(APPCAST_DIR)/$(APPCAST_OUTPUT)" ]; then \
		echo "Error: appcast not found: $(APPCAST_DIR)/$(APPCAST_OUTPUT)"; \
		exit 1; \
	fi
	cp -f "$(APPCAST_DIR)/$(APPCAST_OUTPUT)" "$(APPCAST_PUSH_DIR)/$(APPCAST_OUTPUT)"
	git -C "$(APPCAST_PUSH_DIR)" add "$(APPCAST_OUTPUT)"
	@if git -C "$(APPCAST_PUSH_DIR)" diff --cached --quiet -- "$(APPCAST_OUTPUT)"; then \
		echo "No appcast changes to commit."; \
	else \
		git -C "$(APPCAST_PUSH_DIR)" commit -m "Update FuseX $(FUSEX_VERSION)"; \
		git -C "$(APPCAST_PUSH_DIR)" push; \
	fi

clean-3rdparty:
	@echo "Cleaning 3rdparty build artifacts..."
	rm -rf 3rdparty/audiofile/build
	rm -rf 3rdparty/libgcrypt/build
	rm -rf 3rdparty/FuseGenerator/build
	rm -rf 3rdparty/FuseImporter/build
	rm -rf 3rdparty/mbedtls/build
	rm -rf 3rdparty/libssh2/build

clean: clean-3rdparty
	@echo "Cleaning FuseX build artifacts..."
	rm -rf fusepb/build
	rm -rf build
	rm -rf dist
	rm -f FuseX*.dmg
