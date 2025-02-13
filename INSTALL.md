# BlueALSA Installation

Given its aim of small size and minimum redundancy, BlueALSA makes many of its
features optional and only includes them when explicitly requested when
configuring the build. The number of options is therefore large, too large to
be covered fully here. For a comprehensive installation guide, please look at
the [Installation from source][] project wiki page. If you've found something
missing or incorrect, feel free to make a wiki contribution.

[Installation from source]: https://github.com/Arkq/bluez-alsa/wiki/Installation-from-source

## Configuration

Firstly, create the `configure` script. Run, in the top level project
directory:

```sh
autoreconf --install
```

then, to see a complete list of all options:

```sh
./configure --help
```

Dependencies:

- [alsa-lib](https://www.alsa-project.org/)
- [bluez](http://www.bluez.org/) >= 5.0
- [glib](https://wiki.gnome.org/Projects/GLib) with GIO support
- [sbc](https://git.kernel.org/cgit/bluetooth/sbc.git)
- [docutils](https://docutils.sourceforge.io) (when man pages build is enabled
  with `--enable-manpages`)
- [fdk-aac](https://github.com/mstorsjo/fdk-aac) (when AAC support is enabled
  with `--enable-aac`)
- [lc3plus](https://www.iis.fraunhofer.de/en/ff/amm/communication/lc3.html)
  (when LC3plus support is enabled with `--enable-lc3plus`)
- [libldac](https://github.com/EHfive/ldacBT) (when LDAC support is enabled
  with `--enable-ldac`)
- [libopenaptx](https://github.com/pali/libopenaptx) (when apt-X support is
  enabled and `--with-libopenaptx` is used)
- [mp3lame](https://lame.sourceforge.net/) (when MP3 support is enabled with
  `--enable-mp3lame`)
- [mpg123](https://www.mpg123.org/) (when MPEG decoding support is enabled with
  `--enable-mpg123`)
- [openaptx](https://github.com/Arkq/openaptx) (when apt-X support is enabled
  with `--enable-aptx` and/or `--enable-aptx-hd`)
- [spandsp](https://www.soft-switch.org) (when mSBC support is enabled with
  `--enable-msbc`)

Dependencies for client applications (e.g. `bluealsa-aplay` or `bluealsa-cli`):

- [libdbus](https://www.freedesktop.org/wiki/Software/dbus/)

Dependencies for `bluealsa-rfcomm` (when `--enable-rfcomm` is specified during
configuration):

- [readline](https://tiswww.case.edu/php/chet/readline/rltop.html)

Dependencies for `hcitop` (when `--enable-hcitop` is specified during
configuration):

- [libbsd](https://libbsd.freedesktop.org/)
- [ncurses](https://www.gnu.org/software/ncurses/)

Once the desired options have been chosen, run:

```sh
mkdir build && cd build
../configure [ OPTION ... ]
```

## Installation

When the project is configured, compile and install it by running:

```sh
make
sudo make install
```

When building from the git sources, if `git pull` is used to update the source
tree, then it is recommended to refresh the build in order to update the
version identifier embedded in the configure files. In the top-level directory
run:

```sh
autoreconf --install --force
```

then in the build directory run `make clean` before running `make`.
