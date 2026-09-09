# chatterino-obs

This is a WIP for embedding Chatterino into OBS.

For build documentation see https://github.com/obsproject/obs-plugintemplate.

Building/testing on Windows:

```
git submodule update --init --recursive
cmake --preset windows-x64
cd build_x64
cmake --build . --config RelWithDebInfo && cmake --install . --config RelWithDebInfo
```

On Windows, this includes the SChannel and cert-only TLS backends for Qt.
This will probably cause issues on some systems, because OBS' build doesn't include the OpenSSL backend and the default SChannel backend has caused some issues in the past.

Then you can start OBS. In `Tools`, you should see a new item.

As you can tell, this is not an ideal workflow.

## Linux / Fedora 44

The Linux preset also works on Fedora. The important part is to let CMake pick the distro-default
library directory so the plugin installs into the OBS plugin path that Fedora expects (`/usr/lib64`
on x86_64 Fedora instead of a Debian-style multiarch path).

Install the Chatterino and OBS build dependencies first. For Fedora 44 this includes at least Qt 6,
OpenSSL, Boost, Hunspell, libnotify, CMake/Ninja, and the OBS Studio development files.

Example package install:

```sh
sudo dnf install cmake ninja-build gcc-c++ git \
  qt6-qtbase-devel qt6-qtsvg-devel qt6-qtimageformats \
  openssl-devel boost-devel hunspell-devel libnotify-devel \
  obs-studio-devel
```

If you run OBS on Wayland, also install:

```sh
sudo dnf install qt6-qtwayland
```

Then build and install:

```sh
git submodule update --init --recursive
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
cmake --install build_x86_64 --prefix /tmp/chatterino-obs-install
```

On Fedora x86_64 the plugin library should end up under:

```text
/tmp/chatterino-obs-install/lib64/obs-plugins/
```

and the data files under:

```text
/tmp/chatterino-obs-install/share/obs/obs-plugins/chatterino-obs/
```

## Windows and clangd

To get compile commands on Windows, you need to open the `build_x64/chatterino-obs.slnx` in Visual Studio and use the Clang Power Tools extension to export compile commands.
In the Solution Explorer, right click on the top solution item and select `Clang Power Tools > Export Compile Commands`.
You may need to tell clangd about the build directory:

```yaml
# .clangd
CompileFlags:
  CompilationDatabase: build_x64
Completion:
  HeaderInsertion: Never
```
