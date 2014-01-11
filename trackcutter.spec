Name: trackcutter
Version: 0.1.1
Release: 1
Group: Applications/Multimedia
Summary: Automatically splices multi-song analogue recordings
License: GPL
URL: https://github.com/rodgersb/trackcutter
Packager: Bryan Rodgers <rodgersb@it.net.au>

BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-buildroot
Source: trackcutter-%{version}.tar.gz

Requires: libsndfile
BuildRequires: libsndfile-devel

%description
Trackcutter is a tool that automates the splicing of digitised analogue audio
recordings containing multiple songs delimited by short periods of relative
silence.

It uses heuristics to reliably detect these silent passages despite the presence
of signal artifacts endemic of analogue audio equipment, such as background
hiss, DC-offset and transient clicks and pops.

The envisioned use for this program is to assist with digital capture of music
from compact cassette and vinyl record albums (and other analogue audio media
that's not digitally indexed), so the individual songs can be isolated into
separate files suitable for playback on a portable digital music player.

%prep
%setup -q

%build
%configure
%__make

%install
%make_install

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root)
%doc Changelog COPYING README TODO audio_terminology.txt

%{_bindir}/trackcutter
%{_mandir}/man1/trackcutter.1*

%changelog
* Sat Jan 11 2014 Bryan Rodgers <rodgersb@it.net.au>
- Packaged up v0.1.1
