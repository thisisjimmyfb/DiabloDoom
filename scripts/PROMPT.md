# DiabloDoom Farm Prompt

You play DiabloDoom by looking at screenshots and deciding actions. Latency is not a concern — take your time to make smart decisions.

You are playing DiabloDoom, a turn-based Diablo-style Doom mod. Follow these instructions to play well.

## Objective

Play the game well — explore the level, engage enemies in turn-based combat, pick up loot. Play like a competent player, not a bot grinding a specific drop.

## Setup

1. Use whatever DISPLAY is already set in the environment (no Xvfb — never start it).
2. Kill any stale game: `pkill -9 -f "chocolate-doo[m]"` (bracket trick avoids suicide).
3. Launch: `SDL_VIDEODRIVER=x11 nohup ~/workspace/DiabloDoom/build/src/chocolate-doom -iwad ~/workspace/doom/freedoom1.wad -width 960 -height 600 -warp 1 1 > /tmp/doom_hourly.log 2>&1 &` then sleep 15.
   - NEVER pull from git, NEVER rebuild. Use the existing binary only.
4. Screenshot: use the game's built-in SCREENSHOT driver command. Write `SCREENSHOT\n` to /tmp/doom_cmd.txt, sleep 4, then find the newest `DOOM*.png` in the game's working dir (`~/workspace/doomshots/`) and copy it to /tmp/farm_view.png. Move the DOOM*.png files out afterward so the game's DOOM00..DOOM99 cycle never fills up.

## Game Loop

Play for ~4 minutes (~20 iterations). Keep it tight — you have a 10-minute total budget including setup and reporting. The game persists between runs — you are continuing a continuous play session, not starting fresh each time.

1. Take screenshot: write `SCREENSHOT\n` to /tmp/doom_cmd.txt, sleep 4, then copy the newest `DOOM*.png` from `~/workspace/doomshots/` to /tmp/farm_view.png (and clear the DOOM*.png files so the game's name cycle doesn't fill up).
2. Read `/tmp/farm_view.png` with your vision. Assess:
   - HUD: health %, ammo, current state (EXPLORE banner vs COMBAT/planning UI)
   - 3D view: are you facing a wall? Open area? Doorway? Enemy visible?
   - If COMBAT (turn-based UI visible): 
     * If you have FISTS and the enemy is a Demon or other tanky melee attacker: DO NOT fight it. You will die. Instead, use `MOVE_BACK` / `STRAFE_L` / `STRAFE_R` to disengage. Find a weapon first.
     * KITING (guns only): With a ranged weapon, do not stand toe-to-toe with melee attackers. Use `MOVE_BACK` to create distance, then `ATTACK`. Melee enemies must close the gap to hit you — use that. Keep shooting from range, keep moving away. Never let a Demon get into melee range. Queue: `MOVE_BACK\nATTACK\nATTACK\nEND\n`.
     * Queue attacks — write `ATTACK\nATTACK\nATTACK\nEND\n` to /tmp/doom_cmd.txt, sleep 8, continue.
   - If EXPLORE: decide movement (PRIORITY ORDER):
     * Loot prompt "press G to loot" visible → write `COLLECT\n` IMMEDIATELY. Do not walk past loot. This is your top priority.
     * Vanilla pickup visible on ground (weapon, ammo box, health pack, armor) → WALK toward it to walk over and pick up. These are walk-over pickups, no key needed. Do not ignore pickups.
     * Enemy visible in distance → WALK toward it
     * Facing wall/door up close → TURNL or TURNR (alternate to avoid spinning)
     * Open area → WALK
   - Write your chosen command(s) to /tmp/doom_cmd.txt (one per line). The game's driver consumes the file each tick.
   - If you pick up loot: press C to open the character/inventory screen. EQUIP upgrades immediately to maximize survival and kill speed:
     * Weapon: equip if it has higher damage than current (check the AD/AP damage range). Prioritize kill speed.
     * Armor/Helm/Boots/Gloves/Shield: equip if it has higher armor or resistances. Prioritize survival.
     * Rings/Amulet/Belt: equip if the stats (dexterity, energy, resistances) are better than what's equipped or if the slot is empty.
     * Compare the item stats shown — if it's strictly better than what's equipped in that slot, equip it. Don't hoard upgrades in the backpack.
     * Screenshot the inventory screen after equipping, then close with Esc. Upload the inventory screenshot alongside the regular one.
3. Sleep 5-8 seconds between iterations to let the game act.
4. Check /tmp/doom_hourly.log periodically for: kills, PLAYER_DIED, errors.
   - If you see PLAYER_DIED: the player stays dead at 0% until you confirm. Take a regular SCREENSHOT (write `SCREENSHOT\n` to /tmp/doom_cmd.txt, grab the newest DOOM*.png) for the death moment, upload it with your report. Mention what killed you and where your corpse is. Then send RESPAWN to confirm respawn: write `RESPAWN\n` to /tmp/doom_cmd.txt. Do not send RESPAWN until you've uploaded the death screenshot.
   - CORPSE RUN PRIORITY: After RESPAWN, your #1 priority is recovering your corpse. Your inventory and equipment are on the corpse at the death location. Walk back to it immediately (use WALK/TURNL/TURNR to navigate, SCREENSHOT to verify). Do NOT engage in new combat or explore for loot until you've recovered your corpse. A naked respawn fighting with no gear is how you die twice.

## Decision Guidance

- In combat, always END your turn after queueing attacks (else you stall).
- Don't waste time on empty rooms; head toward doorways and corridors.
- If health is low (<30%) and no enemy nearby, keep exploring.
- GEAR FIRST: If you have FISTS and no weapon, prioritize finding a weapon over fighting. Do not engage Demons or tough enemies with fists — you'll die. Explore, loot, equip, then fight.

## Report

After ~4 min of play:
1. Kill count: grep the log for kill messages, or count from game output.
2. Upload the latest screenshot: copy to workspace first (`cp /tmp/farm_view.png ~/workspace/farm_view_latest.png`), then `/opt/hatch/bin/remote-storage upload-file --path ~/workspace/farm_view_latest.png`
3. Report to user: kills this run and screenshot link.
4. Leave the game running — the next session continues from where you left off.

## Failure Handling

If the game won't start (crash on boot), report the last 20 lines of /tmp/doom_hourly.log plainly. Do not retry more than once per run. If libs are broken after a VM recycle, restore via `~/workspace/goals/diablodoom-bfg-hunt/hidden_files/restore_game_libs.sh` before giving up.
