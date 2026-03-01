# =============================================================================
# high - Terminal-based LLM Chat Interface
# =============================================================================
# Build:     make
# Debug:     make debug
# Install:   sudo make install
# Extras:    sudo make install-extras
# Clean:     make clean
# =============================================================================

# Version Configuration
VERSION_MAJOR := 1
VERSION_MINOR := 0
VERSION_PATCH := 0
VERSION_STRING := $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)

# Compiler Configuration
CXX := g++
CXXFLAGS := -std=c++20 -O3 -Wall -Wextra -pedantic
LDFLAGS := -lcurl -ljansson

# Suppress specific warnings
CXXFLAGS += -Wno-format-truncation

# Platform-specific flags
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LDFLAGS += -pthread
    CXXFLAGS += -march=native
endif
ifeq ($(UNAME_S),Darwin)
    CXXFLAGS += -mmacosx-version-min=10.15
endif

# Directories
SRCDIR := src
PREFIX := /usr/local
BINDIR := $(DESTDIR)$(PREFIX)/bin
EXTRASDIR := extras
SHAREDIR := $(DESTDIR)$(PREFIX)/share/high

# Target
TARGET := high

# Source Files
SRCS := $(wildcard $(SRCDIR)/*.cpp)
ifeq ($(SRCS),)
    SRCS := $(wildcard *.cpp)
endif

OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)

# Extra Scripts
EXTRAS := $(wildcard $(EXTRASDIR)/*.sh)
EXTRAS_BASENAMES := $(notdir $(EXTRAS))

# =============================================================================
# Build Targets
# =============================================================================

.PHONY: all clean install uninstall debug help install-extras uninstall-extras

all: $(TARGET)

$(TARGET): $(OBJS)
				@echo "Building $(TARGET) v$(VERSION_STRING)..."
				$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS)
				@echo "Build complete: $(TARGET) v$(VERSION_STRING)"

%.o: %.cpp
				$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

# =============================================================================
# Installation
# =============================================================================

MANDIR := $(DESTDIR)$(PREFIX)/share/man
MAN1DIR := $(MANDIR)/man1

install: $(TARGET)
				@echo "Installing $(TARGET) v$(VERSION_STRING) to $(BINDIR)..."
				install -d $(BINDIR)
				install -m 755 $(TARGET) $(BINDIR)
				@echo "Installing manpage to $(MAN1DIR)..."
				install -d $(MAN1DIR)
				install -m 644 docs/high.1 $(MAN1DIR)
				@echo "Installation complete!"

uninstall:
				@echo "Removing $(TARGET) from $(BINDIR)..."
				rm -f $(BINDIR)/$(TARGET)
				rm -f $(MAN1DIR)/high.1
				@echo "Uninstallation complete!"

# =============================================================================
# Extra Utilities Installation
# =============================================================================

install-extras: $(EXTRAS)
				@echo "Installing extra utilities..."
				@echo "Installing scripts to $(SHAREDIR)..."
				install -d $(SHAREDIR)
				install -d $(BINDIR)
				@for script in $(EXTRAS); do \
								fullname=$$(basename $$script); \
								aliasname=$${fullname%.sh}; \
								echo "  Installing $$fullname..."; \
								install -m 755 $$script $(SHAREDIR)/$$fullname; \
								echo "  Creating alias $$aliasname in $(BINDIR)..."; \
								rm -f $(BINDIR)/$$aliasname; \
								echo '#!/bin/bash' > $(BINDIR)/$$aliasname; \
								echo "exec $(SHAREDIR)/$$fullname \"$$""@\"" >> $(BINDIR)/$$aliasname; \
								chmod 755 $(BINDIR)/$$aliasname; \
				done
				@echo ""
				@echo "Extra utilities installed successfully!"
				@echo ""
				@echo "Available commands:"
				@echo "  pre   - File context provider for LLM prompts"
				@echo "  cb    - Interactive codeblock extractor"
				@echo ""
				@echo "Usage examples:"
				@echo "  pre src/*.cpp | high 'Review this code'"
				@echo "  high -r -s conversation | cb"
				@echo ""
				@echo "Add to ~/.bashrc or ~/.zshrc for convenience:"
				@echo "  export PATH=$(BINDIR):$$PATH"

uninstall-extras:
				@echo "Removing extra utilities..."
				@for script in $(EXTRAS); do \
								fullname=$$(basename $$script); \
								aliasname=$${fullname%.sh}; \
								echo "  Removing $$aliasname..."; \
								rm -f $(BINDIR)/$$aliasname; \
								echo "  Removing $$fullname..."; \
								rm -f $(SHAREDIR)/$$fullname; \
				done
				@echo "Extra utilities removed!"

# =============================================================================
# Debug Build
# =============================================================================

debug: CXXFLAGS := -std=c++20 -g -O0 -Wall -Wextra -pedantic -DDEBUG
debug: clean $(TARGET)
				@echo "Debug build complete"

# =============================================================================
# Cleanup
# =============================================================================

clean:
				@echo "Cleaning build artifacts..."
				rm -f $(TARGET) $(OBJS) $(DEPS)
				rm -f *.o *.d
				rm -f $(SRCDIR)/*.o $(SRCDIR)/*.d 2>/dev/null || true
				@echo "Clean complete"

distclean: clean
				@echo "Removing all generated files..."
				rm -f version.hpp
				rm -rf debian/high debian/*.substvars 2>/dev/null || true
				@echo "Distclean complete"

# =============================================================================
# Version Management
# =============================================================================

VERSION_FILE := version.hpp

version-show:
				@echo "Current version: $(VERSION_STRING)"

version-bump-patch:
				@eval PATCH=$$(( $(VERSION_PATCH) + 1 )); \
				echo "Bumping patch: $(VERSION_STRING) -> $(VERSION_MAJOR).$(VERSION_MINOR).$$PATCH"; \
				echo '#ifndef VERSION_HPP' > $(VERSION_FILE); \
				echo '#define VERSION_HPP' >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_MAJOR $(VERSION_MAJOR)' >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_MINOR $(VERSION_MINOR)' >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_PATCH '$$PATCH >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_STRING "$(VERSION_MAJOR).$(VERSION_MINOR).$$PATCH"' >> $(VERSION_FILE); \
				echo '#endif' >> $(VERSION_FILE)

version-bump-minor:
				@eval MINOR=$$(( $(VERSION_MINOR) + 1 )); \
				echo "Bumping minor: $(VERSION_STRING) -> $(VERSION_MAJOR).$$MINOR.0"; \
				echo '#ifndef VERSION_HPP' > $(VERSION_FILE); \
				echo '#define VERSION_HPP' >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_MAJOR $(VERSION_MAJOR)' >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_MINOR '$$MINOR >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_PATCH 0' >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_STRING "$(VERSION_MAJOR).$$MINOR.0"' >> $(VERSION_FILE); \
				echo '#endif' >> $(VERSION_FILE)

version-bump-major:
				@eval MAJOR=$$(( $(VERSION_MAJOR) + 1 )); \
				echo "Bumping major: $(VERSION_STRING) -> $$MAJOR.0.0"; \
				echo '#ifndef VERSION_HPP' > $(VERSION_FILE); \
				echo '#define VERSION_HPP' >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_MAJOR '$$MAJOR >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_MINOR 0' >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_PATCH 0' >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_STRING "$$MAJOR.0.0"' >> $(VERSION_FILE); \
				echo '#endif' >> $(VERSION_FILE)

version-set:
				@if [ -z "$(VER)" ]; then echo "Usage: make version-set VER=1.2.3"; exit 1; fi
				@echo "Setting version to: $(VER)"
				@MAJOR=$$(echo $(VER) | cut -d. -f1); \
				MINOR=$$(echo $(VER) | cut -d. -f2); \
				PATCH=$$(echo $(VER) | cut -d. -f3); \
				echo '#ifndef VERSION_HPP' > $(VERSION_FILE); \
				echo '#define VERSION_HPP' >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_MAJOR '$$MAJOR >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_MINOR '$$MINOR >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_PATCH '$$PATCH >> $(VERSION_FILE); \
				echo '#define HIGH_VERSION_STRING "$(VER)"' >> $(VERSION_FILE); \
				echo '#endif' >> $(VERSION_FILE)

# =============================================================================
# Debian Packaging
# =============================================================================

.PHONY: deb-build deb-clean deb-check

deb-build:
				@echo "Building Debian package..."
				debuild -us -uc -b
				@echo "Package built: ../high_*.deb"

deb-clean:
				@echo "Cleaning Debian build artifacts..."
				rm -rf debian/high debian/*.substvars
				rm -f ../high_*.deb ../high_*.dsc ../high_*.changes ../high_*.buildinfo
				@echo "Clean complete"

deb-check:
				@echo "=== Debian Package Check ==="
				@test -f debian/control && echo "✓ debian/control exists" || echo "✗ debian/control missing"
				@test -f debian/rules && echo "✓ debian/rules exists" || echo "✗ debian/rules missing"
				@test -f debian/changelog && echo "✓ debian/changelog exists" || echo "✗ debian/changelog missing"

# =============================================================================
# Verification (CI Simulation)
# =============================================================================

.PHONY: test verify pre-publish

test:
				@echo "=== Running Tests ==="
				@make clean > /dev/null 2>&1
				@make > /dev/null 2>&1 && echo "✓ Build passed" || (echo "✗ Build failed"; exit 1)
				@./high --version > /dev/null 2>&1 && echo "✓ Version check passed" || (echo "✗ Version check failed"; exit 1)
				@./high --help > /dev/null 2>&1 && echo "✓ Help check passed" || (echo "✗ Help check failed"; exit 1)
				@make clean > /dev/null 2>&1
				@echo "=== All Tests Passed ==="

verify:
				@echo "=== Verification ==="
				@make test
				@make deb-check
				@echo "=== Verification Complete ==="

pre-publish: verify
				@echo "=== Pre-Publish Check ==="
				@echo "✓ Build verified"
				@echo "✓ Package structure verified"
				@echo "Project ready for publishing!"

# =============================================================================
# Help
# =============================================================================

help:
				@echo "high Makefile - Version $(VERSION_STRING)"
				@echo ""
				@echo "Build Targets:"
				@echo "  make				      Build release version"
				@echo "  make debug				Build with debug symbols"
				@echo "  make clean				Remove build artifacts"
				@echo "  make distclean    Remove all generated files"
				@echo ""
				@echo "Installation:"
				@echo "  make install      Install to $(PREFIX)/bin"
				@echo "  make uninstall    Remove from system"
				@echo "  make install-extras  Install extra utilities (pre, cb)"
				@echo "  make uninstall-extras Remove extra utilities"
				@echo ""
				@echo "Debian Packaging:"
				@echo "  make deb-build    Build Debian package"
				@echo "  make deb-clean    Clean Debian artifacts"
				@echo "  make deb-check    Check packaging status"
				@echo ""
				@echo "Verification:"
				@echo "  make test				 Run build tests"
				@echo "  make verify       Full verification"
				@echo "  make pre-publish  Pre-publish check"
				@echo ""
				@echo "Version Management:"
				@echo "  make version-show       Show current version"
				@echo "  make version-bump-patch Bump patch version"
				@echo "  make version-bump-minor Bump minor version"
				@echo "  make version-bump-major Bump major version"
				@echo "  make version-set VER=X.Y.Z Set specific version"
				@echo ""
				@echo "Examples:"
				@echo "  make CXX=clang++				      Build with clang"
				@echo "  make DESTDIR=/tmp/pkg install Stage for packaging"
				@echo "  make pre-publish				      Before releasing"
				@echo "  sudo make install-extras      Install pre and cb utilities"

.PHONY: all clean distclean install uninstall debug help
.PHONY: install-extras uninstall-extras
.PHONY: version-show version-bump-patch version-bump-minor version-bump-major version-set
.PHONY: deb-build deb-clean deb-check test verify pre-publish
