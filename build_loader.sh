

rm -rf ./raw/switch/romfs/Contents/game
/opt/devkitpro/tools/bin/nacptool --create "RenPy Loader" "It's Rhew!" "8.5.3" ./raw/app.nacp
/opt/devkitpro/tools/bin/elf2nro ./build/renpy-switch.elf ./raw/renpy_loader.nro \
    --nacp=./raw/app.nacp --icon=./logo.jpg \
    --romfsdir=./raw/switch/romfs
