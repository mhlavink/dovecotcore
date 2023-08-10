%global __provides_exclude_from %{_docdir}
%global __requires_exclude_from %{_docdir}
%global with_pigeonhole 1
%global with_check 0

Summary: Secure imap and pop3 server
Name: dovecot
Epoch: 1
Version: 2.4.0
%global prever %{nil}
Release: 4.20230810213831572145.main.12677.g9ef94bbbfb%{?dist}
#dovecot itself is MIT, a few sources are PD, pigeonhole is LGPLv2
License: MIT AND LGPL-2.1-only

URL: https://www.dovecot.org/
Source: dovecot-2.4.0.tar.gz
Source1: dovecot.init
Source2: dovecot.pam
%if 0%{?with_pigeonhole}
%global pigeonholever 0.5.20
#Source8: https://pigeonhole.dovecot.org/releases/2.3/dovecot-2.3-pigeonhole-%{pigeonholever}.tar.gz
%endif
Source9: dovecot.sysconfig
Source10: dovecot.tmpfilesd

#our own
Source14: dovecot.conf.5

# 3x Fedora/RHEL specific
Patch1: dovecot-2.0-defaultconfig.patch
Patch2: dovecot-1.0.beta2-mkcert-permissions.patch
Patch3: dovecot-1.0.rc7-mkcert-paths.patch

#wait for network
Patch6: dovecot-2.1.10-waitonline.patch

Patch8: dovecot-2.2.20-initbysystemd.patch
Patch9: dovecot-2.2.22-systemd_w_protectsystem.patch
Patch10: dovecot-2.3.0.1-libxcrypt.patch
Patch15: dovecot-2.3.11-bigkey.patch

# do not use own implementation of HMAC, use OpenSSL for certification purposes
# not sent upstream as proper fix would use dovecot's lib-dcrypt but it introduces
# hard to break circular dependency between lib and lib-dcrypt
Patch16: dovecot-2.3.6-opensslhmac3.patch

# FTBFS
Patch17: dovecot-2.3.15-fixvalcond.patch
Patch18: dovecot-2.3.15-valbasherr.patch
Patch20: dovecot-2.3.14-opensslv3.patch
Patch21: dovecot-2.3.19.1-7bad6a24.patch
Patch22: dovecot-configure-c99.patch

# Fedora/RHEL specific, drop OTP which uses SHA1 so we dont use SHA1 for crypto purposes
Patch23: dovecot-2.3.20-nolibotp.patch

Source15: prestartscript

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
BuildRequires: libexttextcat-devel
%endif
BuildRequires: libicu-devel
BuildRequires: libstemmer-devel
BuildRequires: multilib-rpm-config
BuildRequires: flex, bison
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
#%patch -P1 -p1 -b .default-settings
%patch -P2 -p1 -b .mkcert-permissions
%patch -P3 -p1 -b .mkcert-paths
#%patch -P6 -p1 -b .waitonline
#%patch -P8 -p1 -b .initbysystemd
#%patch -P9 -p1 -b .systemd_w_protectsystem
#%patch -P15 -p1 -b .bigkey
#%patch -P16 -p1 -b .opensslhmac
#%patch -P17 -p1 -b .fixvalcond
#%patch -P18 -p1 -b .valbasherr
#%patch -P20 -p1 -b .opensslv3

#%patch -P21 -p1 -b .7bad6a24
#%patch -P22 -p1 -b .c99
#%patch -P23 -p1 -b .nolibotp

%if 0%{?with_pigeonhole}
cp run-test-valgrind.supp dovecot-pigeonhole/
# valgrind would fail with shell wrapper
echo "testsuite" >dovecot-pigeonhole/run-test-valgrind.exclude

pushd dovecot-pigeonhole
./autogen.sh
popd
%endif

#sed -i '/DEFAULT_INCLUDES *=/s|$| '"$(pkg-config --cflags libclucene-core)|" src/plugins/fts-lucene/Makefile.in


# drop OTP which uses SHA1 so we dont use SHA1 for crypto purposes
#rm -rf src/lib-otp FIXME NOCOMMIT

%build
#required for fdpass.c line 125,190: dereferencing type-punned pointer will break strict-aliasing rules
%global _hardened_build 1
export CFLAGS="%{__global_cflags} -fno-strict-aliasing -fstack-reuse=none"
export LDFLAGS="-Wl,-z,now -Wl,-z,relro %{?__global_ldflags}"
mkdir -p m4
autoreconf -I . -fiv #required for aarch64 support

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
    --with-lua=plugin            \
%endif
    --with-lucene                \
    --with-ssl=openssl           \
    --with-ssldir=%{ssldir}      \
    --with-solr                  \
    --with-docs                  \
    systemdsystemunitdir=%{_unitdir}

sed -i 's|/etc/ssl|/etc/pki/dovecot|' doc/mkcert.sh doc/example-config/conf.d/10-ssl.conf

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
install -p -m 644 docinstall/example-config/dovecot.conf $RPM_BUILD_ROOT%{_sysconfdir}/dovecot
install -p -m 644 docinstall/example-config/conf.d/*.conf $RPM_BUILD_ROOT%{_sysconfdir}/dovecot/conf.d
install -p -m 644 docinstall/example-config/conf.d/*.conf.ext $RPM_BUILD_ROOT%{_sysconfdir}/dovecot/conf.d
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
#dovecot uid and gid are reserved, see /usr/share/doc/setup-*/uidgid 
getent group dovecot >/dev/null || groupadd -r --gid 97 dovecot
getent passwd dovecot >/dev/null || \
useradd -r --uid 97 -g dovecot -d /usr/libexec/dovecot -s /sbin/nologin -c "Dovecot IMAP server" dovecot

getent group dovenull >/dev/null || groupadd -r dovenull
getent passwd dovenull >/dev/null || \
useradd -r -g dovenull -d /usr/libexec/dovecot -s /sbin/nologin -c "Dovecot's unauthorized user" dovenull

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
%doc docinstall/* AUTHORS ChangeLog COPYING COPYING.LGPL COPYING.MIT NEWS 
#README
%{_sbindir}/dovecot

%{_bindir}/doveadm
%{_bindir}/doveconf
#%{_bindir}/dsync
%{_bindir}/dovecot-sysreport


%_tmpfilesdir/dovecot.conf
%{_unitdir}/dovecot.service
#%{_unitdir}/dovecot-init.service
%{_unitdir}/dovecot.socket

%dir %{_sysconfdir}/dovecot
%dir %{_sysconfdir}/dovecot/conf.d
%config(noreplace) %{_sysconfdir}/dovecot/dovecot.conf
#list all so we'll be noticed if upstream changes anything
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/10-auth.conf
#%config(noreplace) %{_sysconfdir}/dovecot/conf.d/10-director.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/10-logging.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/10-mail.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/10-master.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/10-metrics.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/10-ssl.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/15-lda.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/15-mailboxes.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/20-imap.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/20-lmtp.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/20-pop3.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/20-submission.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/90-acl.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/90-quota.conf
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/90-plugin.conf
#%config(noreplace) %{_sysconfdir}/dovecot/conf.d/auth-checkpassword.conf.ext
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/auth-deny.conf.ext
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/auth-dict.conf.ext
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/auth-ldap.conf.ext
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/auth-master.conf.ext
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/auth-passwdfile.conf.ext
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/auth-sql.conf.ext
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/auth-static.conf.ext
%config(noreplace) %{_sysconfdir}/dovecot/conf.d/auth-system.conf.ext
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
%{_libdir}/dovecot/*.so.*
%if 0%{?with_pigeonhole}
%exclude %{_libdir}/dovecot/doveadm/*sieve*
%exclude %{_libdir}/dovecot/*_sieve_plugin.so
%exclude %{_libexecdir}/%{name}/managesieve*
%endif
#these (*.so files) are plugins, not devel files
%{_libdir}/dovecot/*_plugin.so
%{_libdir}/dovecot/auth/lib20_auth_var_expand_crypt.so
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
%{_libdir}/dovecot/libfs_mail_crypt.so
%{_libdir}/dovecot/libdcrypt_openssl.so
%{_libdir}/dovecot/lib20_var_expand_crypt.so
#%{_libdir}/dovecot/old-stats/libold_stats_mail.so
#%{_libdir}/dovecot/old-stats/libstats_auth.so

%if 0%{?with_pigeonhole}
%dir %{_libdir}/dovecot/settings
%endif

%{_libexecdir}/%{name}

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
#%{_mandir}/man1/dsync.1*
%{_mandir}/man5/dovecot.conf.5*
%{_mandir}/man7/doveadm-search-query.7*

%files devel
%{_includedir}/dovecot
%{_datadir}/aclocal/dovecot*.m4
%{_libdir}/dovecot/libdovecot*.so
%{_libdir}/dovecot/dovecot-config

%if 0%{?with_pigeonhole}
%files pigeonhole
%doc README
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
* Thu Aug 10 2023 Michal Hlavinka <mhlavink@redhat.com> - 1:2.4.0-4.20230810213831572145.main.12677.g9ef94bbbfb
New release ${PACKIT_PROJECT_VERSION}

* Mon Jul 31 2023 Michal Hlavinka <mhlavink@redhat.com> - 1:2.2.20-4.20230731233539804152.main.12671.g34a18f5a79
- dict-redis: Fix error handling for failed synchronous commits (Timo Sirainen)

