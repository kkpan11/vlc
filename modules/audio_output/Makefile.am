aoutdir = $(pluginsdir)/audio_output
aout_LTLIBRARIES =
# Install the pkglib/pkglibexec first before relinking the modules.
# This ensures libtool re-linking is not racy.
installaoutLTLIBRARIES = install-aoutLTLIBRARIES
$(installaoutLTLIBRARIES): install-pkglibLTLIBRARIES

libvlc_android_audioformat_jni_la_SOURCES = \
	audio_output/android/audioformat_jni.c \
	audio_output/android/audioformat_jni.h

libvlc_android_dynamicsprocessing_jni_la_SOURCES = \
	audio_output/android/dynamicsprocessing_jni.c \
	audio_output/android/dynamicsprocessing_jni.h

if HAVE_ANDROID
noinst_LTLIBRARIES += \
	libvlc_android_audioformat_jni.la \
	libvlc_android_dynamicsprocessing_jni.la
endif

libopensles_android_plugin_la_SOURCES = audio_output/android/opensles.c
libopensles_android_plugin_la_LIBADD = $(LIBDL) $(LIBM)

libandroid_audiotrack_plugin_la_SOURCES = audio_output/android/audiotrack.c audio_output/android/device.h
libandroid_audiotrack_plugin_la_LIBADD = libandroid_env.la \
	libvlc_android_audioformat_jni.la \
	libvlc_android_dynamicsprocessing_jni.la

libandroid_aaudio_plugin_la_SOURCES = audio_output/android/aaudio.c audio_output/android/device.h
libandroid_aaudio_plugin_la_LIBADD = libandroid_env.la libvlc_android_dynamicsprocessing_jni.la $(LIBDL)

libandroid_audiodevice_plugin_la_SOURCES = audio_output/android/device.c audio_output/android/device.h
libandroid_audiodevice_plugin_la_LIBADD = libandroid_env.la \
	libvlc_android_audioformat_jni.la \
	libvlc_android_dynamicsprocessing_jni.la

if HAVE_ANDROID
aout_LTLIBRARIES += libandroid_audiodevice_plugin.la libopensles_android_plugin.la \
	libandroid_audiotrack_plugin.la libandroid_aaudio_plugin.la
endif

libadummy_plugin_la_SOURCES = audio_output/adummy.c

libafile_plugin_la_SOURCES = audio_output/file.c
libafile_plugin_la_CPPFLAGS = $(AM_CPPFLAGS)

libamem_plugin_la_SOURCES = audio_output/amem.c

aout_LTLIBRARIES += \
	libadummy_plugin.la \
	libafile_plugin.la \
	libamem_plugin.la

liboss_plugin_la_SOURCES = audio_output/oss.c audio_output/volume.h
liboss_plugin_la_LIBADD = $(OSS_LIBS) $(LIBM)
if HAVE_OSS
aout_LTLIBRARIES += liboss_plugin.la
endif

libalsa_plugin_la_SOURCES = audio_output/alsa.c audio_output/volume.h
libalsa_plugin_la_CFLAGS = $(AM_CFLAGS) $(ALSA_CFLAGS)
libalsa_plugin_la_LIBADD = $(ALSA_LIBS) $(LIBM)
if HAVE_ALSA
aout_LTLIBRARIES += libalsa_plugin.la
endif

libvlc_pipewire_la_SOURCES = audio_output/vlc_pipewire.c audio_output/vlc_pipewire.h
libvlc_pipewire_la_CFLAGS = $(AM_CFLAGS) $(PIPEWIRE_CFLAGS)
libvlc_pipewire_la_LIBADD = $(PIPEWIRE_LIBS) $(LTLIBVLCCORE)
libvlc_pipewire_la_LDFLAGS = \
	-no-undefined \
	-export-symbols-regex ^vlc_pw_ \
	-version-info 0:0:0
libaout_pipewire_plugin_la_SOURCES = audio_output/pipewire.c
libaout_pipewire_plugin_la_CFLAGS = $(AM_CFLAGS) $(PIPEWIRE_CFLAGS)
libaout_pipewire_plugin_la_LIBADD = libvlc_pipewire.la $(PIPEWIRE_LIBS) $(LIBM)
if HAVE_PIPEWIRE
pkglib_LTLIBRARIES += libvlc_pipewire.la
aout_LTLIBRARIES += libaout_pipewire_plugin.la
endif

libvlc_pulse_la_SOURCES = audio_output/vlcpulse.c audio_output/vlcpulse.h
libvlc_pulse_la_CFLAGS = $(AM_CFLAGS) $(PULSE_CFLAGS)
libvlc_pulse_la_LIBADD = $(PULSE_LIBS) $(LTLIBVLCCORE)
libvlc_pulse_la_LDFLAGS = \
	-no-undefined \
	-export-symbols-regex ^vlc_pa_ \
	-version-info 0:0:0
libpulse_plugin_la_SOURCES = audio_output/pulse.c
libpulse_plugin_la_CFLAGS = $(AM_CFLAGS) $(PULSE_CFLAGS)
libpulse_plugin_la_LIBADD = libvlc_pulse.la $(PULSE_LIBS) $(LIBM)
if HAVE_PULSE
pkglib_LTLIBRARIES += libvlc_pulse.la
aout_LTLIBRARIES += libpulse_plugin.la
endif

libjack_plugin_la_SOURCES = audio_output/jack.c audio_output/volume.h
libjack_plugin_la_CFLAGS = $(AM_CFLAGS) $(JACK_CFLAGS)
libjack_plugin_la_LIBADD = $(JACK_LIBS) $(LIBM)
if HAVE_JACK
aout_LTLIBRARIES += libjack_plugin.la
endif

libmmdevice_plugin_la_SOURCES = audio_output/mmdevice.c audio_output/mmdevice.h
libmmdevice_plugin_la_LIBADD = $(LIBCOM) $(LIBM)
libwinstore_plugin_la_SOURCES = audio_output/winstore.c audio_output/mmdevice.h
libwinstore_plugin_la_LIBADD = $(LIBCOM) -lmmdevapi
libwasapi_plugin_la_SOURCES = audio_output/wasapi.c
libwasapi_plugin_la_LIBADD = $(LIBCOM) -lksuser
if HAVE_WASAPI
aout_LTLIBRARIES += libwasapi_plugin.la
if !HAVE_WINSTORE
aout_LTLIBRARIES += libmmdevice_plugin.la
else
aout_LTLIBRARIES += libwinstore_plugin.la
endif
endif

libkai_plugin_la_SOURCES = audio_output/kai.c
libkai_plugin_la_LIBADD = $(KAI_LIBS)
if HAVE_KAI
aout_LTLIBRARIES += libkai_plugin.la
endif

libsndio_plugin_la_SOURCES = audio_output/sndio.c
libsndio_plugin_la_LIBADD = -lsndio $(LIBM)
if HAVE_SNDIO
aout_LTLIBRARIES += libsndio_plugin.la
endif

libwaveout_plugin_la_SOURCES = audio_output/waveout.c \
	audio_output/windows_audio_common.h
libwaveout_plugin_la_LIBADD = -lwinmm
if HAVE_WIN32_DESKTOP
aout_LTLIBRARIES += libwaveout_plugin.la
endif

libavsamplebuffer_plugin_la_SOURCES = audio_output/apple/avsamplebuffer.m \
	audio_output/apple/channel_layout.c
if HAVE_IOS
libavsamplebuffer_plugin_la_SOURCES += audio_output/apple/avaudiosession_common.m
endif
if HAVE_TVOS
libavsamplebuffer_plugin_la_SOURCES += audio_output/apple/avaudiosession_common.m
endif
if HAVE_XROS
libavsamplebuffer_plugin_la_SOURCES += audio_output/apple/avaudiosession_common.m
endif
if HAVE_WATCHOS
libavsamplebuffer_plugin_la_SOURCES += audio_output/apple/avaudiosession_common.m
endif
libavsamplebuffer_plugin_la_LDFLAGS = $(AM_LDFLAGS) -rpath '$(aoutdir)' \
	-Wl,-framework,CoreMedia,-framework,Foundation,-framework,AVFoundation
libavsamplebuffer_plugin_la_OBJCFLAGS = $(AM_OBJCFLAGS) -fobjc-arc

libauhal_plugin_la_SOURCES = audio_output/apple/auhal.c \
	audio_output/apple/coreaudio_common.c audio_output/apple/coreaudio_common.h \
	audio_output/apple/channel_layout.c

libauhal_plugin_la_LDFLAGS = $(AM_LDFLAGS) -rpath '$(aoutdir)' \
	-Wl,-framework,CoreAudio,-framework,AudioUnit,-framework,AudioToolbox,-framework,CoreServices
if HAVE_OSX
aout_LTLIBRARIES += libauhal_plugin.la libavsamplebuffer_plugin.la
endif
libaudiounit_ios_plugin_la_SOURCES = audio_output/apple/audiounit_ios.m \
	audio_output/apple/coreaudio_common.c audio_output/apple/coreaudio_common.h \
	audio_output/apple/channel_layout.c audio_output/apple/avaudiosession_common.m
libaudiounit_ios_plugin_la_LDFLAGS = $(AM_LDFLAGS) -rpath '$(aoutdir)' \
	-Wl,-framework,Foundation,-framework,CoreAudio,-framework,AudioToolbox,-framework,UIKit,-framework,AVFoundation
if HAVE_IOS_OR_TVOS
aout_LTLIBRARIES += libaudiounit_ios_plugin.la libavsamplebuffer_plugin.la
endif
if HAVE_XROS
aout_LTLIBRARIES += libaudiounit_ios_plugin.la libavsamplebuffer_plugin.la
endif
if HAVE_WATCHOS
aout_LTLIBRARIES += libavsamplebuffer_plugin.la
endif
