%define debug_package %{nil}

Name:		slurm-spank-plugin-ebpf
Version:	0.0.1
Release:	1%{?dist}.edf
Summary:	Slurm SPANK plugin to ondemand ebpf_exporter activation.

Group:		System Environment/Base
License:	GPL-3.0+
#URL:
Source0:	%{name}-%{version}.tar.gz

BuildRequires: slurm-devel >= 25, slurm-devel < 26
Requires:	slurm >= 25, slurm < 26

%description
Slurm SPANK plugin starts `ebpf_exporter` for the lifetime of a Slurm
job and stops it once no job is using it on the node anymore. It avoids 
leaving the exporter running permanently across the whole cluster: 
eBPF probes are only loaded while a real job is present.

%prep
%setup -q


%build
make all


%install
install -d %{buildroot}%{_libdir}/slurm
install -m0755 spank_ebpf.so %{buildroot}%{_libdir}/slurm/
install -d %{buildroot}%{_sysconfdir}/slurm/plugstack.conf.d
install -m0644 ebpf.plugstack.conf %{buildroot}%{_sysconfdir}/slurm/plugstack.conf.d/ebpf.conf


%clean
rm -rf %{buildroot}


%files
%doc LICENSE
%defattr(-,root,root,-)
%{_sysconfdir}/slurm/plugstack.conf.d/ebpf.conf
%{_libdir}/slurm/spank_ebpf.so



%changelog
* Thu Jul 23 2026 Kwame Amedodji <kwame-externe.amedodji@edf.fr> - 0.0.1-1el8.edf
- initial el8 release
