# Cross compiler image for the Windows programs - builds ce_cores.exe (Cygwin program) and webserver.exe + ce_run.exe
# (Go) on Linux, for testing during development, see build_windows_cross.sh. No sources inside - those are mounted
# at run time.
#
# The Cygwin cross compiler (x86_64-pc-cygwin-gcc) is not in Fedora itself, but in the COPR repo of a Cygwin
# maintainer: https://copr.fedorainfracloud.org/coprs/yselkowitz/cygwin/ - it has builds only for some Fedora
# versions, check there when bumping the version below. Its Cygwin runtime is older than the current Cygwin, that's
# fine - newer cygwin1.dll runs programs built against an older one.
#
# Releases are still built by build_windows.sh in a real Cygwin (CI), this is only for development.

FROM fedora:43

RUN dnf -y install dnf-plugins-core \
 && dnf -y copr enable yselkowitz/cygwin \
 && dnf -y install cygwin64-gcc-c++ make rsync golang git \
 && dnf clean all

ENV CYGWIN_SYSROOT=/usr/x86_64-pc-cygwin/sys-root
