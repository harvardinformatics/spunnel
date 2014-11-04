Summary: Slurm spank plugin for arbitrary port forwarding support
Name: spunnel
Version: 0.1.0
Release: 1
License: GPL
Group: System Environment/Base
Source0: %{name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root

BuildRequires: slurm-devel
Requires: slurm

%description
SLURM spank plugin that supports port forwarding between submit and execution
hosts

%prep
%setup -q

%build
make

%install
rm -rf $RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT%{_libdir}
mkdir -p $RPM_BUILD_ROOT%{_libdir}/slurm
mkdir -p $RPM_BUILD_ROOT%{_libexecdir}
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}/slurm
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}/slurm/plugstack.conf.d
install -m 755 spunnel.so $RPM_BUILD_ROOT%{_libdir}/slurm
install -m 644 plugstack.conf $RPM_BUILD_ROOT%{_sysconfdir}/slurm/plugstack.conf.d/stunnel.conf.example

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root,-)
%{_libexecdir}/spunnel
%{_libdir}/slurm/spunnel.so
%config %{_sysconfdir}/slurm/plugstack.conf.d/stunnel.conf.example

%changelog
Aaron Kitzmiller <aaron_kitzmiller@harvard.edu> - 
- Initial build.
