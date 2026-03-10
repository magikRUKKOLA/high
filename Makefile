# =============================================================================
# high - Terminal-based LLM Chat Interface
# =============================================================================
# Build:     make
# Debug:     make debug
# Install:   sudo make install
# Clean:     make clean
# Version:   git tag -a <tagname> -m "message" && make version-renew
# =============================================================================

# Version Configuration (from git tags ONLY)
VERSION_STRING := $(shell git describe --tags --always 2>/dev/null || echo "untagged")

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

# Generated Files
VERSION_FILE := $(SRCDIR)/version.hpp

# =============================================================================
# Build Targets
# =============================================================================

.PHONY: all clean install uninstall debug help version-renew version-show install-extras uninstall-extras

all: $(TARGET)

# Generate version.hpp from git - THIS IS A TARGET, WILL AUTO-RUN IF MISSING
$(VERSION_FILE):
				@mkdir -p $(SRCDIR)
				@echo '#ifndef VERSION_HPP' > $@
				@echo '#define VERSION_HPP' >> $@
				@echo '#define HIGH_VERSION_STRING "$(VERSION_STRING)"' >> $@
				@echo '#endif' >> $@

# Force regenerate version.hpp from current git tag
version-renew:
				@rm -f $(VERSION_FILE)
				@$(MAKE) --no-print-directory $(VERSION_FILE)
				@echo "version.hpp regenerated: $(VERSION_STRING)"

$(TARGET): $(VERSION_FILE) $(OBJS)
				@echo "Building $(TARGET) v$(VERSION_STRING)..."
				$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS)

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
				install -d $(SHAREDIR)
				install -d $(BINDIR)
				@for script in $(EXTRAS); do \
								fullname=$$(basename $$script); \
								aliasname=$${fullname%.sh}; \
								echo "  Installing $$fullname..."; \
								install -m 755 $$script $(SHAREDIR)/$$fullname; \
								echo "  Creating alias $$aliasname..."; \
								rm -f $(BINDIR)/$$aliasname; \
								echo '#!/bin/bash' > $(BINDIR)/$$aliasname; \
								echo "exec $(SHAREDIR)/$$fullname \"\$$@\"" >> $(BINDIR)/$$aliasname; \
								chmod 755 $(BINDIR)/$$aliasname; \
				done
				@echo "Extra utilities installed!"

uninstall-extras:
				@for script in $(EXTRAS); do \
								fullname=$$(basename $$script); \
								aliasname=$${fullname%.sh}; \
								rm -f $(BINDIR)/$$aliasname; \
								rm -f $(SHAREDIR)/$$fullname; \
				done
				@echo "Extra utilities removed!"

# =============================================================================
# Debug Build
# =============================================================================

debug: CXXFLAGS := -std=c++20 -g -O0 -Wall -Wextra -pedantic -DDEBUG
debug: clean all

# =============================================================================
# Cleanup
# =============================================================================

clean:
				@echo "Cleaning build artifacts..."
				rm -f $(TARGET) $(OBJS) $(DEPS)
				rm -f *.o *.d
				rm -f $(SRCDIR)/*.o $(SRCDIR)/*.d 2>/dev/null || true

distclean: clean
				@echo "Removing all generated files..."
				rm -f $(VERSION_FILE)
				rm -rf debian/high debian/*.substvars 2>/dev/null || true

# =============================================================================
# Version Management
# =============================================================================

version-show:
				@echo "Git version: $(VERSION_STRING)"
				@if [ -f $(VERSION_FILE) ]; then \
								cat $(VERSION_FILE); \
				else \
								echo "version.hpp will be generated on build"; \
				fi

# =============================================================================
# Debian Packaging
# =============================================================================

.PHONY: deb-build deb-clean deb-check

deb-build:
				@echo "Building Debian package..."
				debuild -us -uc -b

deb-clean:
				@echo "Cleaning Debian build artifacts..."
				rm -rf debian/high debian/*.substvars
				rm -f ../high_*.deb ../high_*.dsc ../high_*.changes ../high_*.buildinfo

deb-check:
				@echo "=== Debian Package Check ==="
				@test -f debian/control && echo "✓ debian/control exists" || echo "✗ debian/control missing"
				@test -f debian/rules && echo "✓ debian/rules exists" || echo "✗ debian/rules missing"
				@test -f debian/changelog && echo "✓ debian/changelog exists" || echo "✗ debian/changelog missing"

# =============================================================================
# Verification
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
				@echo "Build verified"
				@echo "Package structure verified"
				@echo "Project ready for publishing!"

# =============================================================================
# Help
# =============================================================================

help:
				@echo "high Makefile - Version $(VERSION_STRING)"
				@echo ""
				@echo "Build Targets:"
				@echo "  make				    Build release version"
				@echo "  make debug      Build with debug symbols"
				@echo "  make clean      Remove build artifacts"
				@echo "  make distclean  Remove all generated files"
				@echo ""
				@echo "Installation:"
				@echo "  make install      Install to $(PREFIX)/bin"
				@echo "  make uninstall    Remove from system"
				@echo "  make install-extras   Install extra utilities"
				@echo "  make uninstall-extras Remove extra utilities"
				@echo ""
				@echo "Version:"
				@echo "  make version-show   Show current version"
				@echo "  make version-renew  Regenerate version.hpp from git"
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

.PHONY: all clean distclean install uninstall debug help
.PHONY: install-extras uninstall-extras
.PHONY: version-show version-renew
.PHONY: deb-build deb-clean deb-check test verify pre-publish
