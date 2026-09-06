# Makefile for building FuseX third-party libraries on Windows (MSYS2 MinGW-w64).
# Run from MSYS2 "MinGW 64-bit" shell: cd 3rdparty && make

SCRIPT_DIR := $(CURDIR)
DIST_PREFIX := $(SCRIPT_DIR)/dist

export PKG_CONFIG_PATH := $(DIST_PREFIX)/lib/pkgconfig:$(PKG_CONFIG_PATH)

RED := \033[0;31m
GREEN := \033[0;32m
YELLOW := \033[1;33m
NC := \033[0m

# Optional audiofile: only the legacy `audiofile-patched/` tree (from zip), not the
# submodule path, so default Windows deps stay minimal and build with current GCC.
AUDIOFILE_SRC := $(shell if [ -d "audiofile-patched" ]; then echo "audiofile-patched"; else echo ""; fi)
HOST_CONFIGURE_ARG := $(if $(MACOS_MINGW_CROSS),--host=$(MINGW_HOST),)
CMAKE_CROSS_ARGS := $(if $(MACOS_MINGW_CROSS),-DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$(CC),)

.PHONY: all clean distclean libspectrum audiofile libxml2 mbedtls libssh2 check-tools check-libspectrum check-libssh2 extract-audiofile extract-libxml2 libspectrum-with-audiofile

# Default: libspectrum (+ optional libxml2 / mbedtls / libssh2 if present).
all: check-tools check-libspectrum dist-dirs libspectrum
	@if [ -d "libxml2-2.9.12" ] || [ -f "libxml2-2.9.12.tar.gz" ]; then \
		$(MAKE) libxml2; \
	fi
	@if [ -d "mbedtls/src" ] && [ -f "mbedtls/src/Makefile" ]; then \
		$(MAKE) mbedtls; \
	fi
	@if [ -d "libssh2/src" ] && [ -f "libssh2/src/CMakeLists.txt" ]; then \
		$(MAKE) libssh2; \
	fi
	@echo ""
	@echo "$(GREEN)=== Library build complete! ===$(NC)"
	@echo ""
	@echo "Libraries installed to: $(DIST_PREFIX)"
	@echo "  - DLLs: $(DIST_PREFIX)/bin/"
	@echo "  - Headers: $(DIST_PREFIX)/include/"
	@echo "  - pkg-config: $(DIST_PREFIX)/lib/pkgconfig/"
	@echo ""
	@echo "Built libraries:"
	@echo "  ✓ libspectrum"
	@if [ -n "$(AUDIOFILE_SRC)" ]; then echo "  ✓ audiofile"; fi
	@if [ -d "libxml2-2.9.12" ]; then echo "  ✓ libxml2"; fi
	@if [ -d "mbedtls/src" ] && [ -f "mbedtls/src/Makefile" ]; then echo "  ✓ mbedtls"; fi
	@if [ -d "libssh2/src" ] && [ -f "libssh2/src/CMakeLists.txt" ]; then echo "  ✓ libssh2"; fi
	@echo ""
	@echo "You can now run ./build_win32.sh from the repository root."

dist-dirs:
	@mkdir -p $(DIST_PREFIX)/bin
	@mkdir -p $(DIST_PREFIX)/lib/pkgconfig
	@mkdir -p $(DIST_PREFIX)/include
	@echo "$(GREEN)Using local dist prefix: $(DIST_PREFIX)$(NC)"

check-tools:
	@echo "$(GREEN)Checking for required tools...$(NC)"
	@for tool in gcc make pkg-config cmake; do \
		if ! command -v $$tool >/dev/null 2>&1; then \
			echo "$(RED)Error: $$tool is not installed$(NC)"; \
			echo "Install with: pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-pkg-config mingw-w64-x86_64-cmake"; \
			exit 1; \
		fi; \
		echo "  ✓ $$tool found"; \
	done

check-libspectrum:
	@echo ""
	@echo "$(GREEN)Checking for libspectrum...$(NC)"
	@if [ ! -d "libspectrum" ]; then \
		echo "$(YELLOW)libspectrum submodule missing; initializing from repo root...$(NC)"; \
		cd .. && git submodule update --init --recursive 3rdparty/libspectrum && cd 3rdparty; \
	fi

check-libssh2:
	@echo ""
	@echo "$(GREEN)Checking for libssh2...$(NC)"
	@if [ ! -d "libssh2/src" ] || [ ! -f "libssh2/src/CMakeLists.txt" ]; then \
		echo "$(YELLOW)libssh2 submodule missing; initializing from repo root...$(NC)"; \
		cd .. && git submodule update --init --recursive 3rdparty/libssh2 && cd 3rdparty; \
	fi

extract-audiofile:
	@if [ -f "audiofile-patched.zip" ] && [ ! -d "audiofile-patched" ]; then \
		echo "$(GREEN)Extracting audiofile-patched.zip...$(NC)"; \
		unzip -q audiofile-patched.zip; \
	fi

extract-libxml2:
	@if [ -f "libxml2-2.9.12.tar.gz" ] && [ ! -d "libxml2-2.9.12" ]; then \
		echo "$(GREEN)Extracting libxml2...$(NC)"; \
		tar -xzf libxml2-2.9.12.tar.gz; \
	fi

libspectrum: dist-dirs check-libspectrum
	@echo ""
	@echo "$(GREEN)Building libspectrum (required)...$(NC)"
	@cd libspectrum && \
	if [ -f Makefile ]; then \
		$(MAKE) distclean || true; \
	fi && \
	if [ ! -f configure ]; then \
		./autogen.sh; \
	fi && \
	./configure $(HOST_CONFIGURE_ARG) --with-fake-glib --without-libaudiofile --without-libgcrypt \
	            --without-zlib --without-bzip2 --prefix="$(DIST_PREFIX)" && \
	$(MAKE) && \
	$(MAKE) install

audiofile: extract-audiofile
	@afdir="$(AUDIOFILE_SRC)"; \
	if test -z "$$afdir" && test -d audiofile-patched; then afdir=audiofile-patched; fi; \
	if test -n "$$afdir"; then \
		echo ""; \
		echo "$(GREEN)Building audiofile (optional)...$(NC)"; \
		cd "$$afdir" && \
		if [ -f Makefile ]; then \
			$(MAKE) distclean || true; \
		fi && \
		if [ ! -f configure ]; then \
			./autogen.sh --disable-static --disable-flac --disable-docs --disable-examples; \
		fi && \
		./configure --disable-static --disable-flac --disable-docs --disable-examples --prefix="$(DIST_PREFIX)" && \
		$(MAKE) && \
		$(MAKE) install-strip; \
	fi

libspectrum-with-audiofile: audiofile
	@echo ""
	@echo "$(GREEN)Rebuilding libspectrum with audiofile...$(NC)"
	@cd libspectrum && \
	$(MAKE) distclean || true && \
	./autogen.sh && \
	./configure --with-fake-glib --with-libaudiofile --without-libgcrypt \
	            --without-zlib --without-bzip2 --prefix="$(DIST_PREFIX)" && \
	$(MAKE) && \
	$(MAKE) install

libxml2: extract-libxml2
	@if [ -d "libxml2-2.9.12" ]; then \
		echo ""; \
		echo "$(GREEN)Building libxml2 (optional)...$(NC)"; \
		cd libxml2-2.9.12 && \
		if [ -f Makefile ]; then \
			$(MAKE) distclean || true; \
		fi && \
		./configure --disable-static --without-iconv --without-python --without-lzma --prefix="$(DIST_PREFIX)" && \
		$(MAKE) && \
		$(MAKE) install-strip; \
	fi

mbedtls:
	@if [ -d "mbedtls/src" ] && [ -f "mbedtls/src/Makefile" ]; then \
		echo ""; \
		echo "$(GREEN)Building mbedtls (optional)...$(NC)"; \
		cd mbedtls/src && \
		if [ -f library/Makefile ]; then \
			$(MAKE) clean || true; \
		fi && \
		$(MAKE) WINDOWS=1 SHARED=1 no_test && \
		mkdir -p "$(DIST_PREFIX)/include/mbedtls" && \
		cp -rp include/mbedtls/* "$(DIST_PREFIX)/include/mbedtls/" && \
		mkdir -p "$(DIST_PREFIX)/lib" && \
		mkdir -p "$(DIST_PREFIX)/bin" && \
		cd library && \
		for lib in libmbedtls libmbedx509 libmbedcrypto; do \
			if [ -f "$$lib.a" ]; then cp -f "$$lib.a" "$(DIST_PREFIX)/lib/" || true; fi; \
			if [ -f "$$lib.dll.a" ]; then cp -f "$$lib.dll.a" "$(DIST_PREFIX)/lib/" || true; fi; \
			if [ -f "$$lib.dll" ]; then cp -f "$$lib.dll" "$(DIST_PREFIX)/bin/" || true; fi; \
		done; \
	fi

libssh2: mbedtls dist-dirs check-libssh2
	@if [ -d "libssh2/src" ] && [ -f "libssh2/src/CMakeLists.txt" ]; then \
		if [ ! -f "$(DIST_PREFIX)/lib/libmbedcrypto.a" ]; then \
			echo "$(RED)Error: libssh2 needs mbedTLS in $(DIST_PREFIX) (run: make mbedtls)$(NC)"; \
			exit 1; \
		fi; \
		echo ""; \
		echo "$(GREEN)Building libssh2 (Spectranet SSH)...$(NC)"; \
		rm -rf libssh2/src/build; \
		mkdir -p libssh2/src/build; \
		cd libssh2/src/build && \
		cmake .. \
			-G "Unix Makefiles" \
			$(CMAKE_CROSS_ARGS) \
			-DCMAKE_INSTALL_PREFIX="$(DIST_PREFIX)" \
			-DCMAKE_BUILD_TYPE=Release \
			-DBUILD_SHARED_LIBS=OFF \
			-DBUILD_STATIC_LIBS=ON \
			-DBUILD_EXAMPLES=OFF \
			-DBUILD_TESTING=OFF \
			-DCRYPTO_BACKEND=mbedTLS \
			-DMBEDTLS_INCLUDE_DIR="$(DIST_PREFIX)/include" \
			-DMBEDCRYPTO_LIBRARY="$(DIST_PREFIX)/lib/libmbedcrypto.a" && \
		$(MAKE) && \
		$(MAKE) install; \
	fi

clean:
	@echo "$(YELLOW)Cleaning build artifacts...$(NC)"
	@if [ -d "libspectrum" ] && [ -f "libspectrum/Makefile" ]; then \
		cd libspectrum && $(MAKE) distclean || true; \
	fi
	@if [ -n "$(AUDIOFILE_SRC)" ] && [ -f "$(AUDIOFILE_SRC)/Makefile" ]; then \
		cd "$(AUDIOFILE_SRC)" && $(MAKE) distclean || true; \
	fi
	@if [ -d "audiofile-patched" ] && [ -f "audiofile-patched/Makefile" ]; then \
		cd audiofile-patched && $(MAKE) distclean || true; \
	fi
	@if [ -d "libxml2-2.9.12" ] && [ -f "libxml2-2.9.12/Makefile" ]; then \
		cd libxml2-2.9.12 && $(MAKE) distclean || true; \
	fi
	@if [ -d "mbedtls/src" ] && [ -f "mbedtls/src/library/Makefile" ]; then \
		cd mbedtls/src && $(MAKE) clean || true; \
	fi
	@if [ -d "libssh2/src/build" ]; then \
		rm -rf libssh2/src/build; \
	fi

distclean: clean
	@echo "$(YELLOW)Removing dist directory...$(NC)"
	@rm -rf $(DIST_PREFIX)
