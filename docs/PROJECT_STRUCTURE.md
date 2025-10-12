# Project Structure

This document describes the organization of the rcraid-dkms repository.

## Directory Layout

```
rcraid-dkms/
├── src/                    # Source code files
│   ├── *.c                # C source files
│   ├── *.h                # Header files
│   ├── rcblob.*           # Binary blob files
│   └── rcraid.mod         # Module file
├── scripts/               # Installation and utility scripts
│   ├── install_*          # Distribution-specific installers
│   ├── uninstall_*        # Uninstall scripts
│   └── common_shell       # Common shell functions
├── tests/                 # Testing framework
│   └── test.sh            # Main test script
├── packages/              # Package creation scripts
│   └── package.sh         # Package builder
├── docs/                  # Documentation
│   ├── PROJECT_STRUCTURE.md
│   └── KERNEL_COMPATIBILITY.md
├── .github/               # GitHub Actions workflows
│   └── workflows/
│       └── build-test.yml
├── Makefile              # Main build configuration
├── dkms.conf             # DKMS configuration
├── INSTALL.md             # Complete installation guide
├── README.md             # Main documentation
├── .gitignore            # Git ignore rules
└── version.h             # Version information
```

## File Descriptions

### Source Code (`src/`)
- **C Files**: Main driver implementation files
- **Header Files**: Interface definitions and type declarations
- **Binary Blobs**: Platform-specific binary data files
- **Module File**: Kernel module definition

### Scripts (`scripts/`)
- **Install Scripts**: Distribution-specific installation procedures
  - `install_debian`: Ubuntu/Debian installation
  - `install_arch`: Arch Linux installation
  - `install_generic`: Generic Linux installation
  - `install_rh`: Red Hat/CentOS/Fedora installation
  - `install_suse`: SUSE/openSUSE installation
- **Uninstall Scripts**: Clean removal procedures
- **Common Shell**: Shared shell functions

### Tests (`tests/`)
- **test.sh**: Comprehensive test suite for the driver

### Packages (`packages/`)
- **package.sh**: Script to create distribution packages (.deb, .rpm, AUR)

### Documentation (`docs/`)
- **PROJECT_STRUCTURE.md**: This file

### Root Files
- **Makefile**: Build configuration with distribution detection
- **dkms.conf**: DKMS module configuration
- **README.md**: User documentation and installation guide
- **.gitignore**: Git ignore patterns for build artifacts
- **version.h**: Version and build information

## Build Process

1. **Source Compilation**: C files in `src/` are compiled using the kernel build system
2. **Distribution Detection**: Makefile automatically detects the Linux distribution
3. **Installation**: Appropriate script from `scripts/` is called based on distribution
4. **Testing**: Test suite in `tests/` validates the build and installation

## Adding New Files

- **Source Code**: Add to `src/` directory
- **Scripts**: Add to `scripts/` directory
- **Tests**: Add to `tests/` directory
- **Documentation**: Add to `docs/` directory
- **Packages**: Add to `packages/` directory

## Maintenance

- Update `.gitignore` when adding new build artifacts
- Update `Makefile` when adding new source files
- Update test scripts when adding new functionality
- Update documentation when making structural changes
