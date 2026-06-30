set -e

export DEVKITPRO=/opt/devkitpro
export RENPY_VER=8.5.3
export PYGAME_SDL2_VER=2.1.0

apt-get -y update
apt-get -y upgrade

apt -y install build-essential checkinstall
apt -y install libncursesw5-dev libssl-dev libsqlite3-dev tk-dev libgdbm-dev libc6-dev libbz2-dev

apt -y install python3.13 python3.13-dev python3.13-venv python3-pip curl wget unzip p7zip-full
# Установка pip для Python 3.9
curl -sS https://bootstrap.pypa.io/get-pip.py | python3.13 || true  # python3.13 already has a working pip; non-fatal
python3.13 -m pip --version || true


# Установка Cython 0.29.x (обязательно, а не 3.0+)
python3.13 -m pip install --upgrade pip || true  # debian's pip can't self-uninstall; non-fatal
python3.13 -m pip install "Cython>=3.0.11"  # Критически важно!
python3.13 -m pip install future six requests ecdsa pefile setuptools wheel
python3.13 -m pip install pycryptodomex numpy


# Системные зависимости для компиляции хост-инструментов
apt-get -y install libsdl2-dev libsdl2-image-dev libjpeg-dev \
    libpng-dev libsdl2-ttf-dev libsdl2-mixer-dev libavformat-dev \
    libswscale-dev libglew-dev libfribidi-dev libharfbuzz-dev \
    libavcodec-dev libswresample-dev libsdl2-gfx-dev libgl1 \
    libwebp-dev libopenal-dev libvorbis-dev libopusfile-dev \
    libmpg123-dev libmodplug-dev libflac-dev libvpx-dev\
    pkg-config autoconf automake libtool

python3 --version


curl -LOC - https://github.com/knautilus/Utils/releases/download/v1.0/switch-libfribidi-1.0.12-1-any.pkg.tar.xz
dkp-pacman -U --noconfirm switch-libfribidi-1.0.12-1-any.pkg.tar.xz

# Install the locally-built CPython 3.13.14 (from the PyNX sibling repo) into portlibs.
# Override NXPY if the PyNX build output lives elsewhere.
export NXPY=${NXPY:-$(pwd)/../PyNX_python3.13.0_horizonOS_switch/python_build/nxpy3.13.14}
mkdir -p $DEVKITPRO/portlibs/switch/include/python3.13
cp "$NXPY/lib/libpython3.13.a" $DEVKITPRO/portlibs/switch/lib/
cp -r "$NXPY/include/nxpy/." $DEVKITPRO/portlibs/switch/include/python3.13/


# FFmpeg: use devkitPro's prebuilt switch-ffmpeg instead of building 6.0 from
# source — devkitA64's GCC 16 ICEs (segfaults) compiling FFmpeg 6.0 (vp9recon.c
# etc.). pacman installs matched libav* + codec deps into portlibs/switch.
source $DEVKITPRO/switchvars.sh
dkp-pacman -S --needed --noconfirm switch-ffmpeg

export LD_LIBRARY_PATH=$DEVKITPRO/portlibs/switch/lib:$LD_LIBRARY_PATH
export PYTHONPATH=$DEVKITPRO/portlibs/switch/lib/python3.13:$PYTHONPATH
export C_INCLUDE_PATH=$DEVKITPRO/portlibs/switch/include:$C_INCLUDE_PATH
export CPLUS_INCLUDE_PATH=$DEVKITPRO/portlibs/switch/include:$CPLUS_INCLUDE_PATH


rm switch-libfribidi-1.0.12-1-any.pkg.tar.xz

/bin/bash -c 'sed -i'"'"'.bak'"'"' '"'"'s/set(CMAKE_EXE_LINKER_FLAGS_INIT "/set(CMAKE_EXE_LINKER_FLAGS_INIT "-fPIC /'"'"' $DEVKITPRO/cmake/Switch.cmake'


# pygame_sdl2 is merged into renpy.pygame as of Ren'Py 8.5 — no separate download.
curl -LOC - https://www.renpy.org/dl/$RENPY_VER/renpy-$RENPY_VER-sdk.zip
curl -LOC - https://www.renpy.org/dl/$RENPY_VER/renpy-$RENPY_VER-source.tar.bz2
#curl -LOC - https://dl.otorh.in/github/rawproject.zip

rm -rf renpy-$RENPY_VER-source renpy-source
tar -xf renpy-$RENPY_VER-source.tar.bz2
mv renpy-$RENPY_VER-source renpy-source
rm renpy-$RENPY_VER-source.tar.bz2

rm -rf renpy-$RENPY_VER-sdk renpy_sdk
unzip -qq renpy-$RENPY_VER-sdk.zip -d renpy_sdk
rm renpy-$RENPY_VER-sdk.zip


cp -rf switch_enc.h renpy-source/src/libhydrogen/impl/random
cp -rf encryption.patch renpy-source/src/libhydrogen/impl

#rm -rf raw
#unzip -qq rawproject.zip -d raw
#rm rawproject.zip

pushd renpy-source/src/libhydrogen/impl
patch -p1 < encryption.patch
popd

pushd renpy-source
# renpy.patch is regenerated from the hand-ported 8.5.3 tree (run make_renpy_patch.sh).
patch -p1 < ../renpy.patch
rm -rf tmp/gen3 tmp/gen3-static
popd
