#!/bin/bash

# Package creation script for rcraid-dkms
# Creates packages for different Linux distributions

set -e

PACKAGE_NAME="rcraid-dkms"
PACKAGE_VERSION="8.1.0"
ARCHITECTURE="amd64"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if running as root
if [ "$EUID" -eq 0 ]; then
    log_error "This script should not be run as root"
    exit 1
fi

# Change to repository root
cd "$(dirname "$0")/.."

# Create package directory
PACKAGE_DIR="${PACKAGE_NAME}-${PACKAGE_VERSION}"
rm -rf "$PACKAGE_DIR"
mkdir -p "$PACKAGE_DIR"

log_info "Creating package: $PACKAGE_NAME-$PACKAGE_VERSION"

# Copy source files
cp -r src/* "$PACKAGE_DIR/src/"
cp -r scripts/* "$PACKAGE_DIR/scripts/"
cp -r tests/* "$PACKAGE_DIR/tests/"
cp -r packages/* "$PACKAGE_DIR/packages/"
cp *.conf Makefile README.md "$PACKAGE_DIR/"

# Create Debian package
create_debian_package() {
    log_info "Creating Debian package..."
    
    DEB_DIR="${PACKAGE_NAME}-${PACKAGE_VERSION}_${ARCHITECTURE}"
    rm -rf "$DEB_DIR"
    mkdir -p "$DEB_DIR/DEBIAN"
    mkdir -p "$DEB_DIR/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"
    
    # Copy source files
    cp -r "$PACKAGE_DIR"/* "$DEB_DIR/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}/"
    
    # Create control file
    cat > "$DEB_DIR/DEBIAN/control" << EOF
Package: ${PACKAGE_NAME}
Version: ${PACKAGE_VERSION}
Section: kernel
Priority: optional
Architecture: ${ARCHITECTURE}
Depends: dkms, linux-headers
Maintainer: AMD RAID Driver Team <support@amd.com>
Description: AMD RAID Controller Driver (DKMS)
 This package provides the AMD RAID controller driver
 for Linux systems with DKMS support.
 .
 Supported hardware:
 - AMD X370/B350 chipsets
 - AMD X399 chipsets  
 - AMD X470/B450 chipsets
 - AMD X570/B550 chipsets
 .
 Supported distributions:
 - Ubuntu 18.04+
 - Debian 10+
 - Arch Linux
 - Red Hat/CentOS/Fedora
 - SUSE/openSUSE
EOF
    
    # Create postinst script
    cat > "$DEB_DIR/DEBIAN/postinst" << 'EOF'
#!/bin/bash
set -e

if [ "$1" = "configure" ]; then
    if command -v dkms >/dev/null 2>&1; then
        dkms add -m rcraid -v 8.1.0
        dkms build -m rcraid -v 8.1.0
        dkms install -m rcraid -v 8.1.0
    fi
fi

exit 0
EOF
    
    # Create prerm script
    cat > "$DEB_DIR/DEBIAN/prerm" << 'EOF'
#!/bin/bash
set -e

if [ "$1" = "remove" ]; then
    if command -v dkms >/dev/null 2>&1; then
        dkms remove -m rcraid -v 8.1.0 --all
    fi
fi

exit 0
EOF
    
    chmod +x "$DEB_DIR/DEBIAN/postinst"
    chmod +x "$DEB_DIR/DEBIAN/prerm"
    
    # Build package
    dpkg-deb --build "$DEB_DIR"
    log_info "Debian package created: ${DEB_DIR}.deb"
}

# Create RPM package
create_rpm_package() {
    log_info "Creating RPM package..."
    
    RPM_DIR="rpmbuild"
    rm -rf "$RPM_DIR"
    mkdir -p "$RPM_DIR"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
    
    # Create spec file
    cat > "$RPM_DIR/SPECS/${PACKAGE_NAME}.spec" << EOF
Name: ${PACKAGE_NAME}
Version: ${PACKAGE_VERSION}
Release: 1%{?dist}
Summary: AMD RAID Controller Driver (DKMS)
License: Proprietary
URL: https://github.com/joeytroy/amd-raid-driver
Source0: %{name}-%{version}.tar.gz
BuildArch: noarch
Requires: dkms, kernel-devel
BuildRequires: dkms, kernel-devel

%description
This package provides the AMD RAID controller driver
for Linux systems with DKMS support.

Supported hardware:
- AMD X370/B350 chipsets
- AMD X399 chipsets  
- AMD X470/B450 chipsets
- AMD X570/B550 chipsets

Supported distributions:
- Red Hat Enterprise Linux 7+
- CentOS 7+
- Fedora 30+
- SUSE Linux Enterprise 15+
- openSUSE 15+

%prep
%setup -q

%build
# No build step needed for DKMS

%install
mkdir -p %{buildroot}/usr/src/%{name}-%{version}
cp -r * %{buildroot}/usr/src/%{name}-%{version}/

%post
if command -v dkms >/dev/null 2>&1; then
    dkms add -m rcraid -v %{version}
    dkms build -m rcraid -v %{version}
    dkms install -m rcraid -v %{version}
fi

%preun
if [ \$1 -eq 0 ]; then
    if command -v dkms >/dev/null 2>&1; then
        dkms remove -m rcraid -v %{version} --all
    fi
fi

%files
/usr/src/%{name}-%{version}/*

%changelog
* $(date '+%a %b %d %Y') AMD RAID Driver Team <support@amd.com> - ${PACKAGE_VERSION}-1
- Initial package release
EOF
    
    # Create source tarball
    tar -czf "$RPM_DIR/SOURCES/${PACKAGE_NAME}-${PACKAGE_VERSION}.tar.gz" -C "$PACKAGE_DIR" .
    
    # Build RPM
    rpmbuild --define "_topdir $(pwd)/$RPM_DIR" -ba "$RPM_DIR/SPECS/${PACKAGE_NAME}.spec"
    
    # Copy built RPM
    cp "$RPM_DIR/RPMS/noarch/${PACKAGE_NAME}-${PACKAGE_VERSION}-1.*.rpm" .
    log_info "RPM package created: ${PACKAGE_NAME}-${PACKAGE_VERSION}-1.*.rpm"
}

# Create Arch Linux package
create_arch_package() {
    log_info "Creating Arch Linux package..."
    
    AUR_DIR="${PACKAGE_NAME}-${PACKAGE_VERSION}"
    mkdir -p "$AUR_DIR"
    
    # Create PKGBUILD
    cat > "$AUR_DIR/PKGBUILD" << EOF
# Maintainer: AMD RAID Driver Team <support@amd.com>
pkgname=${PACKAGE_NAME}
pkgver=${PACKAGE_VERSION}
pkgrel=1
pkgdesc="AMD RAID Controller Driver (DKMS)"
arch=('x86_64')
url="https://github.com/joeytroy/amd-raid-driver"
license=('Proprietary')
depends=('dkms' 'linux-headers')
makedepends=('git')
source=("\${pkgname}-\${pkgver}.tar.gz")
sha256sums=('SKIP')

package() {
    cd "\${pkgname}-\${pkgver}"
    
    # Install source
    install -dm755 "\${pkgdir}/usr/src/\${pkgname}-\${pkgver}"
    cp -r * "\${pkgdir}/usr/src/\${pkgname}-\${pkgver}/"
    
    # Install DKMS configuration
    install -Dm644 dkms.conf "\${pkgdir}/usr/src/\${pkgname}-\${pkgver}/"
}
EOF
    
    # Create source tarball
    tar -czf "${AUR_DIR}/${PACKAGE_NAME}-${PACKAGE_VERSION}.tar.gz" -C "$PACKAGE_DIR" .
    
    # Create .SRCINFO
    cat > "$AUR_DIR/.SRCINFO" << EOF
pkgbase = ${PACKAGE_NAME}
	pkgdesc = AMD RAID Controller Driver (DKMS)
	pkgver = ${PACKAGE_VERSION}
	pkgrel = 1
	url = https://github.com/joeytroy/amd-raid-driver
	arch = x86_64
	license = Proprietary
	depends = dkms
	depends = linux-headers
	makedepends = git
	source = ${PACKAGE_NAME}-${PACKAGE_VERSION}.tar.gz
	sha256sums = SKIP

pkgname = ${PACKAGE_NAME}
EOF
    
    log_info "Arch Linux package created in: $AUR_DIR"
}

# Main execution
main() {
    log_info "Starting package creation..."
    
    # Check dependencies
    if ! command -v dpkg-deb >/dev/null 2>&1; then
        log_warn "dpkg-deb not found - skipping Debian package"
    else
        create_debian_package
    fi
    
    if ! command -v rpmbuild >/dev/null 2>&1; then
        log_warn "rpmbuild not found - skipping RPM package"
    else
        create_rpm_package
    fi
    
    create_arch_package
    
    log_info "Package creation completed!"
    log_info "Created packages:"
    ls -la *.deb *.rpm 2>/dev/null || true
    log_info "Arch package directory: $AUR_DIR"
}

# Run main function
main "$@"
