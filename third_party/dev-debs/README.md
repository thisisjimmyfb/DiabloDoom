# Dev Headers (.debs)

Committed SDL2 and build dependency .deb files for VM rebuilds.

After a VM recycle wipes /usr, run:

```bash
./third_party/dev-debs/install.sh
cd build && cmake . && make -j2 chocolate-doom
```

This restores the -dev headers needed to compile. The runtime libs are
handled separately by `~/workspace/goals/diablodoom-bfg-hunt/hidden_files/restore_game_libs.sh`.

Files: 45 .debs (~7.9MB) covering libsdl2-dev, libsdl2-mixer-dev,
libsdl2-net-dev, libfluidsynth-dev, and their transitive build dependencies.
