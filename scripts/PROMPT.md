# DiabloDoom Farm Prompt

You play DiabloDoom by looking at screenshots and deciding actions. Latency is not a concern — take your time to make smart decisions.

You are playing DiabloDoom, a turn-based Diablo-style Doom mod. Follow these instructions to play well.

## Objective

Play the game well — explore the level, engage enemies in turn-based combat, pick up loot. Play like a competent player, not a bot grinding a specific drop.

## Setup

1. No X display exists on this VM — launch the game with the dummy video driver (no Xvfb — never start it).
2. Kill any stale game: `pkill -9 -f "chocolate-doo[m]"` (bracket trick avoids suicide).
3. Launch from ~/workspace/diablodoom: `cd ~/workspace/diablodoom && SDL_VIDEODRIVER=dummy nohup ~/workspace/DiabloDoom/build/src/chocolate-doom -iwad ~/workspace/doom/freedoom1.wad -width 960 -height 600 -warp 1 1 > /tmp/doom_hourly.log 2>&1 &` then sleep 15. Verify with `pgrep -a -f "chocolate-doo[m]"` (bracket trick avoids suicide).
   - NEVER pull from git, NEVER rebuild. Use the existing binary only.
   - Stale binary check: send `VERSION\n` to /tmp/doom_cmd.txt, then check /tmp/doom_hourly.log for `[VERSION] <hash>`. Compare with `git -C ~/workspace/DiabloDoom rev-parse --short HEAD`. If they differ, the binary is stale — report it to the main agent, do not try to rebuild.
4. Screenshot: use the game's built-in SCREENSHOT driver command. Write `SCREENSHOT\n` to /tmp/doom_cmd.txt, sleep 4, then find the newest `diablodoom_*.png` in `~/workspace/diablodoom/doomshots/` and copy it to /tmp/farm_view.png. Move the diablodoom_*.png files out afterward so the game's timestamped name cycle never fills up.

## Game Loop

Play for ~4 minutes (~20 iterations). Keep it tight — you have a 10-minute total budget including setup and reporting. The game persists between runs — you are continuing a continuous play session, not starting fresh each time.

1. Take screenshot: write `SCREENSHOT\n` to /tmp/doom_cmd.txt, sleep 4, then copy the newest `diablodoom_*.png` from `~/workspace/diablodoom/doomshots/` to /tmp/farm_view.png (and clear the diablodoom_*.png files so the game's name cycle doesn't fill up).
2. Read `/tmp/farm_view.png` with your vision. Assess:
   - HUD: health %, ammo, current state (EXPLORE banner vs COMBAT/planning UI)
   - 3D view: are you facing a wall? Open area? Doorway? Enemy visible?
   - If COMBAT (turn-based UI visible): 
     * If you have no Diablo weapon equipped (vanilla pistol only) and the enemy is a Flesh worm or other tanky melee attacker: DO NOT fight it up close. You will die. Instead, use `MOVE_BACK` / `STRAFE_L` / `STRAFE_R` to disengage. Find a Diablo weapon first.
     * KITING: With the pistol, do not stand toe-to-toe with melee attackers. ONLY kite (MOVE_BACK) if the enemy is almost in melee range (very close, about to hit you). If the enemy is far away, do NOT back up — just ATTACK from range, or WALK toward them if you want to close distance. Backing up when the enemy is far just wastes moves and risks getting stuck against walls. Queue: `MOVE_BACK\nATTACK\nATTACK\nEND\n` (only when enemy is close).
     * STUCK RECOVERY: If your position (x,y) does not change after a movement command, you are against a wall. Do NOT repeat the same move. Try `STRAFE_L` or `STRAFE_R` to sidestep, or turn with arrow keys then `WALK` forward. Alternate until you move.
     * Queue attacks — write `ATTACK\nATTACK\nATTACK\nEND\n` to /tmp/doom_cmd.txt, sleep 8, continue. (ATTACK uses your equipped weapon; with no Diablo weapon, it's the vanilla pistol.)
   - If EXPLORE: decide movement (PRIORITY ORDER):
     * Loot prompt "press G to loot" visible → write `COLLECT\n` IMMEDIATELY. Do not walk past loot. This is your top priority. The prompt appears for Diablo loot drops AND player corpses — COLLECT walks to the nearest one.
     * After picking up loot: use `INVENTORY` to check what you got, then `EQUIP <backpack_idx> <slot>` to equip upgrades. Slots: 0=HELM,1=ARMOR,2=WEAPON,3=SHIELD,4=RING1,5=RING2,6=AMULET,7=BOOTS,8=GLOVES,9=BELT,10=FOCUS. Use `SALVAGE <backpack_idx>` to turn junk into essence, `UNEQUIP <slot>` to remove gear.
     * Vanilla pickup visible on ground (weapon, ammo box, health pack, armor) → WALK toward it to walk over and pick up. These are walk-over pickups, no key needed. Do not ignore pickups.
     * Enemy visible in distance → WALK toward it
     * Facing wall/door up close → TURNL or TURNR (alternate to avoid spinning)
     * Open area → WALK
   - Write your chosen command(s) to /tmp/doom_cmd.txt (one per line) using e.g. `printf "WALK\n" > /tmp/doom_cmd.txt`. IMPORTANT: /tmp/doom_cmd.txt is a FIFO (named pipe) created by the game — DO NOT delete it. If you see it as a pipe (not a regular file), that is correct. Just write to it; the game drains it every tick.
   - If you pick up loot: press C to open the character/inventory screen. EQUIP upgrades immediately to maximize survival and kill speed:
     * Weapon: equip if it has higher damage than current (check the AD/AP damage range). Prioritize kill speed.
     * Armor/Helm/Boots/Gloves/Shield: equip if it has higher armor or resistances. Prioritize survival.
     * Rings/Amulet/Belt: equip if the stats (dexterity, energy, resistances) are better than what's equipped or if the slot is empty.
     * Compare the item stats shown — if it's strictly better than what's equipped in that slot, equip it. Don't hoard upgrades in the backpack.
     * Screenshot the inventory screen after equipping, then close with Esc. Upload the inventory screenshot alongside the regular one.
3. Sleep 5-8 seconds between iterations to let the game act.
4. Check /tmp/doom_hourly.log periodically for: kills, PLAYER_DIED, errors.
   - If you see PLAYER_DIED: the player stays dead at 0% until you confirm. Capture a REPLAY video (write `REPLAY\n` to /tmp/doom_cmd.txt, sleep 20, grab the newest diablodoom_*.mp4) for the death moment, upload it with your report. Mention what killed you and where your corpse is. Then send RESPAWN to confirm respawn: write `RESPAWN\n` to /tmp/doom_cmd.txt. Do not send RESPAWN until you've uploaded the death video.
   - CORPSE RUN PRIORITY: After RESPAWN, your #1 priority is recovering your corpse. Your inventory and equipment are on the corpse at the death location. Walk back to it immediately (use WALK/TURNL/TURNR to navigate, SCREENSHOT to verify). Do NOT engage in new combat or explore for loot until you've recovered your corpse. A naked respawn fighting with no gear is how you die twice.

## Decision Guidance

- In combat, always END your turn after queueing attacks (else you stall).
- Don't waste time on empty rooms; head toward doorways and corridors.
- If health is low (<30%) and no enemy nearby, keep exploring.
- GEAR FIRST: If you have no Diablo weapon equipped (vanilla pistol only), prioritize finding a Diablo weapon over fighting. Do not engage Flesh worms or tough enemies in melee — you'll die. Explore, loot, equip, then fight.

## Freedoom Monster Names

This game uses Freedoom sprites, NOT classic Doom. Use these OFFICIAL Freedoom names in reports (from doomwiki.org/wiki/Freedoom):

- **Serpentipede** (engine: Imp) — dark insectoid alien, near-black chitin, red head-horns, yellow eyes, segmented abdomen. NEVER call it an "imp."
- **Flesh worm** (engine: Demon) — pinkish-red fleshy blob with gaping toothed maw.
- **Stealth worm** (engine: Spectre) — same as Flesh worm but translucent.
- **Zombie** (engine: Zombieman) — humanoid soldier, gray-olive skin, blue visor, red shoulder pads.
- **Shotgun zombie** (engine: Shotgun guy) — dark-armored trooper, red-orange head, blue chest strip.
- **Minigun zombie** (engine: Heavy weapon dude)
- **Trilobite** (engine: Cacodemon) — giant floating armored head, tan dome, blue patches, red tentacles.
- **Hatchling** (engine: Lost soul) — flaming horned skull.
- **Pain bringer** (engine: Hell knight)
- **Pain lord** (engine: Baron of Hell) — armored dark knight, not a minotaur.
- **Matribite** (engine: Pain elemental)
- **Octaminator** (engine: Revenant)
- **Necromancer** (engine: Arch-vile)
- **Combat slug** (engine: Mancubus)
- **Technospider** (engine: Arachnotron)
- **Large technospider** (engine: Spiderdemon/Spider Mastermind)
- **Assault tripod** (engine: Cyberdemon)

## Report

After ~4 min of play:
1. Kill count: grep the log for kill messages, or count from game output.
2. Capture a replay video: write `REPLAY\n` to /tmp/doom_cmd.txt, sleep 20 (encoding 525 frames takes a while), then find the newest `diablodoom_*.mp4` in `~/workspace/diablodoom/doomshots/`. Copy to workspace (`cp <newest.mp4> ~/workspace/farm_replay_latest.mp4`), then `/opt/hatch/bin/remote-storage upload-file --path ~/workspace/farm_replay_latest.mp4 --content-type video/mp4`
3. Report to user: kills this run and replay video link.
4. Leave the game running — the next session continues from where you left off.

## Failure Handling

If the game won't start (crash on boot), report the last 20 lines of /tmp/doom_hourly.log plainly. Do not retry more than once per run. If libs are broken after a VM recycle, restore via `~/workspace/goals/diablodoom-bfg-hunt/hidden_files/restore_game_libs.sh` before giving up.
