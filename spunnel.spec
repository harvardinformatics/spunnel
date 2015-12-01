Summary: Slurm spank plugin for arbitrary port forwarding support
Name: spunnel
Version: 15.08.4
Release: 1 
License: GPL
Group: System Environment/Base
Source0: %{name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root

#BuildRequires: slurm-devel
#Requires: slurm

%description
SLURM spank plugin that supports port forwarding between submit and execution
hosts

%{!?_slurm_libdir: %global _slurm_libdir %{_libdir}/slurm}
%define _libdir %{_slurm_libdir}

%prep
%setup -q

%build
%configure
make

%install
rm -rf $RPM_BUILD_ROOT
make install DESTDIR=$RPM_BUILD_ROOT
rm  -f $RPM_BUILD_ROOT/%{_libdir}/libspunnel.a
rm  -f $RPM_BUILD_ROOT/%{_libdir}/libspunnel.la


%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root,-)
%{_libdir}/libspunnel.so
%{_libdir}/libspunnel.so.0
%{_libdir}/libspunnel.so.0.0.7
%{_datadir}/doc/spunnel/AUTHORS
%{_datadir}/doc/spunnel/COPYING
%{_datadir}/doc/spunnel/README.md
%{_datadir}/doc/spunnel/plugstack.conf.example


%changelog
* Mon Nov 17 2014 Aaron Kitzmiller <aaron_kitzmiller@harvard.edu>
- Initial rpmbuild
- 
