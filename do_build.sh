cd /mnt/d/Source/renpy_switch/RenpySwitch
cp sources/main.c source/main.c
cd build && make 2>&1 | tail -5 && cd ..

# 1. Did the rebuild include the new code?
strings build/renpy-switch.elf | grep -q "calling Py_InitializeFromConfig" \
    && echo "MARKER OK" || echo "MARKER MISSING — make did not relink"

# 2. Does lib.zip actually carry the stdlib bootstrap?
unzip -l raw/switch/romfs/Contents/lib.zip | grep -E "encodings/|os\.py" \
    || echo "STDLIB MISSING FROM lib.zip"

# 3. Rebuild the NRO
$DEVKITPRO/tools/bin/elf2nro build/renpy-switch.elf the_question.nro \
    --nacp=the_question.nacp --icon=../PyNX_python3.13.0_horizonOS_switch/icon.jpg --romfsdir=raw/switch/romfs
