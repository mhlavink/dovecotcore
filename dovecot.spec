%global __provides_exclude_from %{_docdir}
%global __requires_exclude_from %{_docdir}
%global with_pigeonhole 1
%global with_check 1
%global with_downstreamchanges 0

Summary: Secure imap and pop3 server
Name: dovecot
Epoch: 1
Version: 2.4.0
%global prever %{nil}
Release: 4.20250702114813705866.main.16204.gf67500a599%{?dist}
#dovecot itself is MIT, a few sources are PD, pigeonhole is LGPLv2
License: MIT AND LGPL-2.1-only

URL: https://www.dovecot.org/
Source: dovecot-2.4.0.tar.gz
Source1: dovecot.init
Source2: dovecot.pam
%if 0%{?with_pigeonhole}
%global pigeonholever %{version}%{?prever}
#Source8: https://pigeonhole.dovecot.org/releases/2.4/dovecot-pigeonhole-%{pigeonholever}.tar.gz
%endif
Source9: dovecot.sysconfig
Source10: dovecot.tmpfilesd

#our own
Source14: dovecot.conf.5
Source15: prestartscript
Source16: dovecot.sysusers

# 3x Fedora/RHEL specific
Patch1: dovecot-2.0-defaultconfig.patch
Patch2: dovecot-1.0.beta2-mkcert-permissions.patch
Patch3: dovecot-1.0.rc7-mkcert-paths.patch

#wait for network
Patch6: dovecot-2.1.10-waitonline.patch

Patch8: dovecot-2.2.20-initbysystemd.patch
Patch9: dovecot-2.2.22-systemd_w_protectsystem.patch
Patch15: dovecot-2.3.11-bigkey.patch

# do not use own implementation of HMAC, use OpenSSL for certification purposes
# not sent upstream as proper fix would use dovecot's lib-dcrypt but it introduces
# hard to break circular dependency between lib and lib-dcrypt
Patch16: dovecot-2.4.1-opensslhmac3.patch

# FTBFS
Patch17: dovecot-2.3.15-fixvalcond.patch
Patch18: dovecot-2.3.15-valbasherr.patch

# Fedora/RHEL specific, drop OTP which uses SHA1 so we dont use SHA1 for crypto purposes
Patch23: dovecot-2.4.1-nolibotp.patch

BuildRequires: gcc, gcc-c++, openssl-devel, pam-devel, zlib-devel, bzip2-devel, libcap-devel
BuildRequires: libtool, autoconf, automake, pkgconfig
BuildRequires: sqlite-devel
BuildRequires: libpq-devel
BuildRequires: mariadb-connector-c-devel
BuildRequires: libxcrypt-devel
BuildRequires: openldap-devel
BuildRequires: krb5-devel
BuildRequires: quota-devel
BuildRequires: xz-devel
BuildRequires: lz4-devel
BuildRequires: libzstd-devel
%if %{?rhel}0 == 0
BuildRequires: libsodium-devel
BuildRequires: lua-devel
BuildRequires: lua-json
#BuildRequires: libexttextcat-devel
%endif
BuildRequires: libicu-devel
%if %{?rhel}0 == 0
BuildRequires: libstemmer-devel
%endif
BuildRequires: multilib-rpm-config
BuildRequires: flex, bison
BuildRequires: perl-version
BuildRequires: systemd-devel
BuildRequires: systemd-rpm-macros

# as we skip make dist, we have to download some stuff during build
BuildRequires: wget
%if %{?fedora}0 >= 350
#BuildRequires: glibc-gconv-extra
%endif

# gettext-devel is needed for running autoconf because of the
# presence of AM_ICONV
BuildRequires: gettext-devel

# Explicit Runtime Requirements for executalbe
Requires: openssl >= 0.9.7f-4

# Package includes an initscript service file, needs to require initscripts package
Requires(pre): shadow-utils
Requires: systemd
Requires(post): systemd-units
Requires(preun): systemd-units
Requires(postun): systemd-units

BuildRequires: clucene-core-devel

%global ssldir %{_sysconfdir}/pki/%{name}

BuildRequires: libcurl-devel expat-devel
BuildRequires: make

%if 0%{?fedora} > 39
# as per https://fedoraproject.org/wiki/Changes/EncourageI686LeafRemoval
ExcludeArch:    %{ix86}
%endif

%global restart_flag /run/%{name}/%{name}-restart-after-rpm-install

%description
Dovecot is an IMAP server for Linux/UNIX-like systems, written with security 
primarily in mind.  It also contains a small POP3 server.  It supports mail 
in either of maildir or mbox formats.

The SQL drivers and authentication plug-ins are in their subpackages.

%if 0%{?with_pigeonhole}
%package pigeonhole
Requires: %{name} = %{epoch}:%{version}-%{release}
Summary: Sieve and managesieve plug-in for dovecot
License: MIT AND LGPL-2.1-only

%description pigeonhole
This package provides sieve and managesieve plug-in for dovecot LDA.
%endif

%package pgsql
Requires: %{name} = %{epoch}:%{version}-%{release}
Summary: Postgres SQL back end for dovecot
%description pgsql
This package provides the Postgres SQL back end for dovecot-auth etc.

%package mysql
Requires: %{name} = %{epoch}:%{version}-%{release}
Summary: MySQL back end for dovecot
%description mysql
This package provides the MySQL back end for dovecot-auth etc.

%package devel
Requires: %{name} = %{epoch}:%{version}-%{release}
Summary: Development files for dovecot
%description devel
This package provides the development files for dovecot.

%prep
%setup -q -n dovecot-2.4.0
./autogen.sh
#-a 8

# standardize name, so we don't have to update patches and scripts
#mv dovecot-pigeonhole-%{pigeonholever} dovecot-pigeonhole

%patch -P 1 -p2 -b .default-settings
%patch -P 2 -p1 -b .mkcert-permissions
%patch -P 3 -p1 -b .mkcert-paths
%patch -P 6 -p2 -b .waitonline
%patch -P 8 -p2 -b .initbysystemd
%patch -P 9 -p1 -b .systemd_w_protectsystem
%patch -P 15 -p1 -b .bigkey
%if 0%{?with_downstreamchanges}
%patch -P 16 -p2 -b .opensslhmac3
%patch -P 17 -p2 -b .fixvalcond
%patch -P 18 -p1 -b .valbasherr
%patch -P 23 -p2 -b .nolibotp
%endif

%if 0%{?with_pigeonhole}
cp run-test-valgrind.supp dovecot-pigeonhole/
# valgrind would fail with shell wrapper
echo "testsuite" >dovecot-pigeonhole/run-test-valgrind.exclude

pushd dovecot-pigeonhole
./autogen.sh
popd
%endif


%if 0%{?with_downstreamchanges}
# drop OTP which uses SHA1 so we dont use SHA1 for crypto purposes
#rm -rf src/lib-otp
echo >src/auth/mech-otp-common.c
echo >src/auth/mech-otp-common.h
echo >src/auth/mech-otp.c
echo >src/lib-auth/password-scheme-otp.c
pushd src/lib-otp
for f in *.c *.h
do
  echo >$f
done
popd
%endif

%build
#required for fdpass.c line 125,190: dereferencing type-punned pointer will break strict-aliasing rules
%global _hardened_build 1
export CFLAGS="%{__global_cflags} -fno-strict-aliasing -fstack-reuse=none"
export LDFLAGS="-Wl,-z,now -Wl,-z,relro %{?__global_ldflags}"
mkdir -p m4
if [ -d /usr/share/gettext/m4 ]
then
  #required for aarch64 support
  # point to gettext explicitely, autoreconf cant find iconv.m4 otherwise
  autoreconf -I . -I /usr/share/gettext/m4 
else
  autoreconf -I . -fiv #required for aarch64 support
fi

%configure                       \
    INSTALL_DATA="install -c -p -m644" \
    --with-rundir=%{_rundir}/%{name}   \
    --with-systemd               \
    --docdir=%{_docdir}/%{name}  \
    --disable-static             \
    --disable-rpath              \
    --with-nss                   \
    --with-shadow                \
    --with-pam                   \
    --with-gssapi=plugin         \
    --with-ldap=plugin           \
    --with-sql=plugin            \
    --with-pgsql                 \
    --with-mysql                 \
    --with-sqlite                \
    --with-zlib                  \
    --with-zstd                  \
    --with-libcap                \
    --with-icu                   \
%if %{?rhel}0 == 0
    --with-libstemmer            \
    --with-lua=plugin            \
%else
    --without-libstemmer         \
    --without-lua                \
%endif
    --without-lucene             \
    --without-exttextcat         \
    --with-ssl=openssl           \
    --with-ssldir=%{ssldir}      \
    --with-solr                  \
    --with-docs                  \
    systemdsystemunitdir=%{_unitdir}

sed -i 's|/etc/ssl|/etc/pki/dovecot|' doc/mkcert.sh # doc/example-config/conf.d/10-ssl.conf

%make_build

%if 0%{?with_pigeonhole}
#pigeonhole
pushd dovecot-pigeonhole

# required for snapshot
[ -f configure ] || autoreconf -fiv
[ -f ChangeLog ] || echo "Pigeonhole ChangeLog is not available, yet" >ChangeLog

%configure                             \
    INSTALL_DATA="install -c -p -m644" \
    --disable-static                   \
    --with-dovecot=../                 \
    --without-unfinished-features

%make_build
popd
%endif 

%install
rm -rf $RPM_BUILD_ROOT

%make_install

# move doc dir back to build dir so doc macro in files section can use it
mv $RPM_BUILD_ROOT/%{_docdir}/%{name} %{_builddir}/%{name}-%{version}%{?prever}/docinstall

# fix multilib issues
%multilib_fix_c_header --file %{_includedir}/dovecot/config.h

%if 0%{?with_pigeonhole}
pushd dovecot-pigeonhole
%make_install

mv $RPM_BUILD_ROOT/%{_docdir}/%{name} $RPM_BUILD_ROOT/%{_docdir}/%{name}-pigeonhole

install -m 644 AUTHORS ChangeLog COPYING COPYING.LGPL INSTALL NEWS README $RPM_BUILD_ROOT/%{_docdir}/%{name}-pigeonhole
popd
%endif

install -p -D -m 644 %{SOURCE2} $RPM_BUILD_ROOT%{_sysconfdir}/pam.d/dovecot

#install man pages
install -p -D -m 644 %{SOURCE14} $RPM_BUILD_ROOT%{_mandir}/man5/dovecot.conf.5

#install waitonline script
install -p -D -m 755 %{SOURCE15} $RPM_BUILD_ROOT%{_libexecdir}/dovecot/prestartscript

install -p -D -m 0644 %{SOURCE16} $RPM_BUILD_ROOT%{_sysusersdir}/dovecot.conf

# generate ghost .pem files
mkdir -p $RPM_BUILD_ROOT%{ssldir}/certs
mkdir -p $RPM_BUILD_ROOT%{ssldir}/private
touch $RPM_BUILD_ROOT%{ssldir}/certs/dovecot.pem
chmod 600 $RPM_BUILD_ROOT%{ssldir}/certs/dovecot.pem
touch $RPM_BUILD_ROOT%{ssldir}/private/dovecot.pem
chmod 600 $RPM_BUILD_ROOT%{ssldir}/private/dovecot.pem

install -p -D -m 644 %{SOURCE10} $RPM_BUILD_ROOT%{_tmpfilesdir}/dovecot.conf

mkdir -p $RPM_BUILD_ROOT/run/dovecot/{login,empty,token-login}

# Install dovecot configuration and dovecot-openssl.cnf
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}/dovecot/conf.d
#install -p -m 644 docinstall/example-config/dovecot.conf $RPM_BUILD_ROOT%{_sysconfdir}/dovecot
#install -p -m 644 docinstall/example-config/conf.d/*.conf $RPM_BUILD_ROOT%{_sysconfdir}/dovecot/conf.d
#install -p -m 644 docinstall/example-config/conf.d/*.conf.ext $RPM_BUILD_ROOT%{_sysconfdir}/dovecot/conf.d
%if 0%{?with_pigeonhole}
install -p -m 644 $RPM_BUILD_ROOT/%{_docdir}/%{name}-pigeonhole/example-config/conf.d/*.conf $RPM_BUILD_ROOT%{_sysconfdir}/dovecot/conf.d
install -p -m 644 $RPM_BUILD_ROOT/%{_docdir}/%{name}-pigeonhole/example-config/conf.d/*.conf.ext $RPM_BUILD_ROOT%{_sysconfdir}/dovecot/conf.d ||:
%endif
install -p -m 644 doc/dovecot-openssl.cnf $RPM_BUILD_ROOT%{ssldir}/dovecot-openssl.cnf

install -p -m755 doc/mkcert.sh $RPM_BUILD_ROOT%{_libexecdir}/%{name}/mkcert.sh

mkdir -p $RPM_BUILD_ROOT/var/lib/dovecot

#remove the libtool archives
find $RPM_BUILD_ROOT%{_libdir}/%{name}/ -name '*.la' | xargs rm -f

#remove what we don't want
rm -f $RPM_BUILD_ROOT%{_sysconfdir}/dovecot/README
pushd docinstall
rm -f securecoding.txt thread-refs.txt
popd


%pre
%if 0%{?fedora} < 42
#dovecot uid and gid are reserved, see /usr/share/doc/setup-*/uidgid 
%sysusers_create_compat %{SOURCE16}
%endif

# do not let dovecot run during upgrade rhbz#134325
if [ "$1" = "2" ]; then
  rm -f %restart_flag
  /bin/systemctl is-active %{name}.service >/dev/null 2>&1 && touch %restart_flag ||:
  /bin/systemctl stop %{name}.service >/dev/null 2>&1
fi

%post
if [ $1 -eq 1 ]
then
  %systemd_post dovecot.service
fi

install -d -m 0755 -g dovecot -d /run/dovecot
install -d -m 0755 -d /run/dovecot/empty
install -d -m 0750 -g dovenull -d /run/dovecot/login
install -d -m 0750 -g dovenull -d /run/dovecot/token-login
[ -x /sbin/restorecon ] && /sbin/restorecon -R /run/dovecot ||:

%preun
if [ $1 = 0 ]; then
        /bin/systemctl disable dovecot.service dovecot.socket >/dev/null 2>&1 || :
        /bin/systemctl stop dovecot.service dovecot.socket >/dev/null 2>&1 || :
    rm -rf /run/dovecot
fi

%postun
/bin/systemctl daemon-reload >/dev/null 2>&1 || :

if [ "$1" -ge "1" -a -e %restart_flag ]; then
    /bin/systemctl start dovecot.service >/dev/null 2>&1 || :
rm -f %restart_flag
fi

%posttrans
# dovecot should be started again in %%postun, but it's not executed on reinstall
# if it was already started, restart_flag won't be here, so it's ok to test it again
if [ -e %restart_flag ]; then
    /bin/systemctl start dovecot.service >/dev/null 2>&1 || :
rm -f %restart_flag
fi

%check
%if 0%{?with_check}
%ifnarch aarch64
# some aarch64 tests timeout, skip for now
make check
%if 0%{?with_pigeonhole}
cd dovecot-pigeonhole
make check
%endif
%endif
%endif

%files
%doc docinstall/* AUTHORS ChangeLog COPYING COPYING.LGPL COPYING.MIT INSTALL.md NEWS README.md SECURITY.md
%{_sbindir}/dovecot

%{_bindir}/doveadm
%{_bindir}/doveconf
%{_bindir}/dovecot-sysreport

%_tmpfilesdir/dovecot.conf
%{_sysusersdir}/dovecot.conf
%{_unitdir}/dovecot.service
%{_unitdir}/dovecot-init.service
%{_unitdir}/dovecot.socket

%dir %{_sysconfdir}/dovecot
%dir %{_sysconfdir}/dovecot/conf.d
%config(noreplace) %{_sysconfdir}/dovecot/dovecot.conf
%config(noreplace) %{_sysconfdir}/pam.d/dovecot
%config(noreplace) %{ssldir}/dovecot-openssl.cnf

%dir %{ssldir}
%dir %{ssldir}/certs
%dir %{ssldir}/private
%attr(0600,root,root) %ghost %config(missingok,noreplace) %verify(not md5 size mtime) %{ssldir}/certs/dovecot.pem
%attr(0600,root,root) %ghost %config(missingok,noreplace) %verify(not md5 size mtime) %{ssldir}/private/dovecot.pem

%dir %{_libdir}/dovecot
%dir %{_libdir}/dovecot/auth
%dir %{_libdir}/dovecot/dict
%{_libdir}/dovecot/doveadm
%exclude %{_libdir}/dovecot/doveadm/*sieve*
%{_libdir}/dovecot/*.so.*
#these (*.so files) are plugins, not devel files
%{_libdir}/dovecot/*_plugin.so
%exclude %{_libdir}/dovecot/*_sieve_plugin.so
%{_libdir}/dovecot/auth/libauthdb_imap.so
%{_libdir}/dovecot/auth/libauthdb_ldap.so
%if %{?rhel}0 == 0
%{_libdir}/dovecot/auth/libauthdb_lua.so
%endif
%{_libdir}/dovecot/auth/libmech_gssapi.so
%{_libdir}/dovecot/auth/libdriver_sqlite.so
%{_libdir}/dovecot/dict/libdriver_sqlite.so
%{_libdir}/dovecot/dict/libdict_ldap.so
%{_libdir}/dovecot/libdriver_sqlite.so
%{_libdir}/dovecot/libssl_iostream_openssl.so
%{_libdir}/dovecot/libfs_compress.so
%{_libdir}/dovecot/libfs_crypt.so
%{_libdir}/dovecot/libdcrypt_openssl.so
%{_libdir}/dovecot//var_expand_crypt.so

%if 0%{?with_pigeonhole}
%dir %{_libdir}/dovecot/settings
%endif

%{_libexecdir}/%{name}
%exclude %{_libexecdir}/%{name}/managesieve*

%dir %attr(0755,root,dovecot) %ghost /run/dovecot
%attr(0750,root,dovenull) %ghost /run/dovecot/login
%attr(0750,root,dovenull) %ghost /run/dovecot/token-login
%attr(0755,root,root) %ghost /run/dovecot/empty

%attr(0750,dovecot,dovecot) /var/lib/dovecot

%{_datadir}/%{name}

%{_mandir}/man1/deliver.1*
%{_mandir}/man1/doveadm*.1*
%{_mandir}/man1/doveconf.1*
%{_mandir}/man1/dovecot*.1*
%{_mandir}/man5/dovecot.conf.5*
%{_mandir}/man7/doveadm-search-query.7*

%files devel
%{_includedir}/dovecot
%{_datadir}/aclocal/dovecot*.m4
%{_libdir}/dovecot/libdovecot*.so
%{_libdir}/dovecot/dovecot-config

%if 0%{?with_pigeonhole}
%files pigeonhole
%doc dovecot-pigeonhole/README
%{_bindir}/sieve-dump
%{_bindir}/sieve-filter
%{_bindir}/sieve-test
%{_bindir}/sievec
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/20-managesieve.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/90-sieve.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/90-sieve-extprograms.conf

%{_docdir}/%{name}-pigeonhole

%{_libexecdir}/%{name}/managesieve
%{_libexecdir}/%{name}/managesieve-login

%{_libdir}/dovecot/doveadm/*sieve*
%{_libdir}/dovecot/*_sieve_plugin.so
%{_libdir}/dovecot/settings/libmanagesieve_*.so
%{_libdir}/dovecot/settings/libpigeonhole_*.so
%{_libdir}/dovecot/sieve/

%{_mandir}/man1/sieve-dump.1*
%{_mandir}/man1/sieve-filter.1*
%{_mandir}/man1/sieve-test.1*
%{_mandir}/man1/sievec.1*
%{_mandir}/man1/sieved.1*
%{_mandir}/man7/pigeonhole.7*
%endif

%files mysql
%{_libdir}/%{name}/libdriver_mysql.so
%{_libdir}/%{name}/auth/libdriver_mysql.so
%{_libdir}/%{name}/dict/libdriver_mysql.so

%files pgsql
%{_libdir}/%{name}/libdriver_pgsql.so
%{_libdir}/%{name}/auth/libdriver_pgsql.so
%{_libdir}/%{name}/dict/libdriver_pgsql.so

%changelog
* Wed Jul 2 2025 Michal Hlavinka <mhlavink@redhat.com> - 1:2.4.0-4.20250702114813705866.main.16204.gf67500a599
- New release ${PACKIT_PROJECT_VERSION}

* Wed Jul 2 2025 Michal Hlavinka <mhlavink@redhat.com> - 1:2.4.0-4.20250702114117284953.main.16204.gf67500a599
- New release ${PACKIT_PROJECT_VERSION}

* Wed Jul 2 2025 Michal Hlavinka <mhlavink@redhat.com> - 1:2.4.0-4.20250702113945175022.main.16204.gf67500a599
- New release ${PACKIT_PROJECT_VERSION}

* Wed Jul 2 2025 Michal Hlavinka <mhlavink@redhat.com> - 1:2.4.0-4.20250702111724994708.main.16204.gf67500a599
- New release ${PACKIT_PROJECT_VERSION}

* Wed Jul 2 2025 Michal Hlavinka <mhlavink@redhat.com> - 1:2.4.0-4.20250702110031553097.main.16204.gf67500a599
- New release ${PACKIT_PROJECT_VERSION}

* Wed Jul 2 2025 Michal Hlavinka <mhlavink@redhat.com> - 1:2.4.0-4.20250702105842071073.main.16204.gf67500a599
- New release ${PACKIT_PROJECT_VERSION}

* Wed Jul 2 2025 Michal Hlavinka <mhlavink@redhat.com> - 1:2.4.0-4.20250702105445265062.main.16204.gf67500a599
- New release ${PACKIT_PROJECT_VERSION}

* Wed Jul 2 2025 Michal Hlavinka <mhlavink@redhat.com> - 1:2.4.0-4.20250702104555755029.main.16204.gf67500a599
- New release ${PACKIT_PROJECT_VERSION}

* Wed Jul 2 2025 Michal Hlavinka <mhlavink@redhat.com> - 1:2.4.0-4.20250702104523445434.main.16204.gf67500a599
- New release ${PACKIT_PROJECT_VERSION}

* Wed Jul 2 2025 Michal Hlavinka <mhlavink@redhat.com> - 1:2.4.0-4.20250702104315953337.main.16204.gf67500a599
- New release ${PACKIT_PROJECT_VERSION}

* Wed Jul 2 2025 Michal Hlavinka <mhlavink@redhat.com> - 1:2.4.0-4.20250702102824360413.main.16204.gf67500a599
- New release ${PACKIT_PROJECT_VERSION}

