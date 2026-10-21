# How to Play DiabloDoom (Agent Strategy Guide)

This is the public strategy guide for teaching our AI agent to play DiabloDoom — a turn-based Diablo-style mod of Doom. If you have ideas for better tactics, we want to hear them!

## Observation Strategy

**The loop is: ACT → REPLAY → THINK → ACT → REPLAY → THINK...**

The replay always comes *after* your commands play out, never before. This guarantees the video shows real gameplay, not idle thinking time.

**Explore mode: use REPLAY videos.**
- Videos give temporal context — you can see movement, enemy behavior, and what's changing.
- To move: send `WALK`. The game keeps walking forward until it spots an enemy, loot, or hits a wall — then it stops and logs `[WALK] <reason>`. You don't need to estimate distances or send repeated commands.
- A loot/pickup stop fires only ONCE per item: if you send `WALK` again, the game walks past that item. Use a second WALK when you've decided to leave an item behind — and if WALK reports `blocked` immediately in several directions, you're probably in a pit or trench: follow the low ground along its length instead of pushing at the high walls.
- Turn to face a doorway or direction you want to explore (TURNL/TURNR), then send `WALK`. The game handles the rest.
- Send WALK, wait ~5 seconds, then REPLAY. The 15s buffer captures your movement in the moment.
- After the replay, view the frames: if WALK stopped for loot, COLLECT it. If for an enemy, prepare for combat. If blocked, turn and WALK again.
- If WALK stops `blocked` while you are facing a door, send `USE` to open it, then `WALK` again.
- For precise positioning (e.g. moving toward a specific key you see), use `GOTO fwd strafe` (facing-relative, e.g. `GOTO 150 0` = 150 forward). The game pathfinds there.
- Keep vision fast: extract only 2 frames at half resolution (`-vf fps=1/8,scale=480:300` as JPEG). You don't need 4 full-res PNGs — 2 small frames show position, enemies, and loot just fine.

**Turn-based combat: use screenshots, refreshed after every single movement command.**
- Combat is discrete — the game state only changes when you queue and execute.
- Movement especially: queue ONE move, take a new SCREENSHOT, see where you landed, then queue the next. Never chain multiple moves blind — you can't see walls, enemies, or hazards between steps without a fresh frame.
- After attacks, refresh the screenshot to check TP remaining, queue contents, target selection, enemy positions.
- Don't rely on a stale screenshot from before you queued — the queue panel, TP, and target markers all change.

## The Game

DiabloDoom combines Doom's first-person shooting with Diablo's loot system and turn-based combat. The agent explores levels, fights monsters in turn-based mode, picks up loot, equips gear, and tries to survive.

## Objective

Play well — explore the level, engage enemies in turn-based combat, pick up loot. The goal is loot progression: collect better and better gear over time and equip upgrades. Play like a competent player, not a bot grinding a specific drop.

## Combat Strategy

When the turn-based UI is visible (TURN MODE banner):

### Targeting
- Enemies are marked with numbers. The agent selects a target and queues attacks.
- Always END the turn after queueing attacks, or the game stalls.

### Melee vs Ranged Enemies

**Melee enemies** (Flesh worm, Demon, Spectre, etc.):
- With only the vanilla pistol: DO NOT fight up close. You will die.
- ONLY back up (kite) if a melee enemy is almost in melee range (very close, about to hit you).
- If the melee enemy is far away: do NOT back up — just attack from range.
- Queue: `MOVE_BACK` then `ATTACK` x2 (only when the enemy is close).

**Ranged enemies** (Serpentipede, Shotgun zombie, etc.):
- Do NOT kite — backing up doesn't dodge projectiles.
- Just ATTACK, or strafe sideways to dodge.

### Staying Alive

- **Retreat when hurt:** If HP drops below 40% and you're fighting a ranged enemy, STOP attacking. Move behind a wall, door, or corner to break line of sight. Let your HP recover or reposition — don't trade shots you'll lose.
- **Queue aggressively:** Fill the turn queue every round — batch 6-10 actions (moves + attacks) per iteration, then END turn. A half-empty queue wastes the round. The old "2-3 attacks" caution is retired: more actions per loop means more progress per minute.
- **Check ammo first:** Never queue more attacks than your current ammo. If you see "NO AMMO" skips in the log, you're queuing too many.
- **Kill ranged first:** Serpentipedes and Imps out-DPS the pistol at range. If multiple enemies are visible, target the ranged ones before melee — or break LOS and force them to come to you.
- **Use cover:** Peek around corners and doors. Move out, queue a full batch of attacks, move back behind cover, END turn. Repeat. Don't stand in the open against ranged enemies.
- **Captures are automatic:** The game auto-saves a replay when your HP drops below 50%. Use CAPTURE (not SCREENSHOT) when you want both a 15s video and a screenshot — it's one command that does both cleanly. After you see the screenshot, decide whether the paired video is worth sharing.

### Gear Check
- You are NEVER unarmed — you always have the pistol, even with no Diablo weapon equipped. "No weapon" means no Diablo weapon in the WEAPON slot. Don't avoid combat thinking you're defenseless.
- If you have no Diablo weapon equipped (vanilla pistol only): prioritize finding a Diablo weapon over fighting tough enemies. But you CAN fight — just be cautious.

### Movement History, Traps, Edges and Corners

The game tracks where you have actually been. Your screenshot shows one moment — HISTORY shows the pattern, and the pattern is what gets you trapped.

- **Check HISTORY often** (diagnostic, works any time). It lists your recent positions oldest-first, and every WALK/GOTO result is stamped with start and end positions (`moved N from (x,y)`), so you always know whether a command actually took you somewhere.
- **You are trapped when any of these is true:**
  - HISTORY ends with the "likely trapped" note (your last points huddle in a small box), or
  - your last 3 movement commands each moved you less than ~32 units, or
  - HISTORY shows you revisiting the same ±48-unit area 3 or more times.
- **When trapped, name it and run the escape ladder — don't keep poking:**
  1. Strafe out sideways: `GOTO 0 120` or `GOTO 0 -120` (a perpendicular step away from the wall), then WALK.
  2. Turn 180° and WALK back out the way you came in.
  3. Wall-follow: turn so a wall is on your left, WALK; at each stop, turn 45–90° toward the open side and WALK again, keeping the wall on the same side. A corner ends when the wall ends — follow it around instead of bouncing off it.
  4. In a trench or pit: move ALONG the low ground, parallel to the high wall, never straight at it. Look for the step up or the ramp at the trench's ends.
- **Edges and corners:**
  - Approach openings square-on: face the CENTER of the doorway or gap before walking through. Clipping a corner post and stopping is the #1 way this farm gets wedged.
  - If you stop against a door frame or a corner edge, don't WALK again at the same angle — you will re-wedge in the same spot and HISTORY will show the cluster. Back off with `GOTO -60 0`, strafe 30–60 units toward the opening's center, then WALK through.
  - In tight or ledgy geometry, prefer WALK + wall-following over GOTO: GOTO's stepper gives up at waypoint 0 in spots that blind WALK slides through.
  - Never issue repeated forward WALKs at slightly different angles hoping one slips through. Change something structural instead: strafe, back out, or follow the wall.

## Exploration Strategy

**Prefer new ground over old ground.** You find it with your own eyes, not a map readout. Watch `SECTORS`: if the visited count hasn't grown over your last several moves, you are looping. Then do a **survey spin**: turn in steps all the way around (a full 360°), pausing briefly at each facing, then send `CAPTURE` — the replay's rolling buffer now holds the whole panorama, so review its frames to catalog every opening, doorway, and corridor around you. Cross off the directions HISTORY says you've already taken, and head for an opening you haven't tried, even if it means crossing known rooms to reach it. Backtracking is a means to reach new ground, never the activity itself. When two routes look equal, always take the one you haven't been down.

When in EXPLORE mode (not in combat), priority order:

1. **Loot prompt visible** ("press G to loot") → Collect IMMEDIATELY. This is the top priority. The prompt appears for Diablo loot drops AND player corpses.

2. **After picking up loot** → Check inventory, equip upgrades immediately:
   - Weapon: equip if higher damage than current
   - Armor/Helm/Boots/Gloves/Shield: equip if higher armor or resistances
   - Rings/Amulet/Belt: equip if stats are better or slot is empty
   - Don't hoard upgrades in the backpack — equip them

3. **Vanilla pickup on ground** (weapon, ammo, health, armor) → Walk toward it. These are walk-over pickups, no key needed.

4. **Colored door** (red/blue/yellow markings on a door) → You need the matching key. Send `KEYS` to check which keys you have:
   - If you have the key: walk through the door (GOTO toward it)
   - If not: send `SCAN` to get the direction/distance to nearby keys (e.g. `[SCAN] red_key: 150 fwd, 30 right`). Then use GOTO to move toward it. If SCAN finds no keys, explore systematically — visit every room, check corners, alcoves, platforms.

5. **Key visible** (small red/blue/yellow card on ground or pedestal) → GOTO toward it immediately. Walk over it to pick up. This is high priority.
   - You don't need precise coordinates. Estimate the direction (ahead/left/right) and a rough distance: close (~50), medium (~150), far (~300). Send e.g. `GOTO 150 0` for a key that looks medium-distance ahead.
   - The game will clamp if you overshoot into a wall. After moving, take a new screenshot: if the key is still visible, send another GOTO to get closer. Repeat until you walk over it. Iterative correction beats one-shot precision.

6. **Enemy visible in distance** → Walk toward it to engage

7. **Facing wall** → Turn left or right (alternate to avoid spinning)

8. **Open area** → Walk forward, explore

## Level Exploration and Exit

Your goal is to progress through the level: explore → find keys → unlock doors → reach the exit.

**Exploration strategy:**
- Doom levels are divided into sectors connected by doorways. To explore the full level, go through every doorway you see.
- A doorway is an opening in the wall, often with a door texture or a visible passage. When you see one, GOTO through it.
- Send `SECTORS` to check your progress (e.g. `[SECTORS] current=5 visited=12 total=48`). If visited isn't increasing, you're backtracking — find a doorway you haven't been through.
- **You don't need 100%.** Some sectors are secrets (hidden behind fake walls or triggered by switches). If you've visited most sectors (>80%) and can't find new doorways, stop trying — focus on finding the exit in the areas you've already explored.
- **If stuck**: If SECTORS visited hasn't increased in several minutes, you're likely missing a secret or the exit is in an area you've already visited. Try pressing USE on suspicious-looking walls (different texture, slightly misaligned), or re-check areas for an exit switch you might have missed.
- If you're in a room with multiple doorways, pick one you haven't taken yet.
- Keys and important items are often in side rooms, alcoves, or on platforms — check these, don't just run down the main corridor.

**Finding the exit:**
- The level exit is usually a switch on a wall (often with a red EXIT sign above it), a teleporter pad (glowing platform), or a door marked EXIT.
- When you see the exit, GOTO toward it. For a switch, walk up to it and press USE (the game handles activation on touch for most exits).
- If you've explored most of the level and haven't found the exit, look for areas you haven't visited — it's often behind a locked door or in a less obvious location.

**The full loop:**
1. Explore new areas (GOTO through doorways)
2. If you see a colored door, check KEYS — find the key if needed (SCAN or vision)
3. Unlock doors and continue exploring
4. When you find the exit, go to it

## Death and Corpse Recovery

- When you die, capture the moment (it's interesting!), then respawn.
- **After respawning, recovering your corpse is the #1 priority.** Your inventory and equipment are on the corpse at the death location.
- Walk back to it immediately. Do NOT engage in new combat or explore for loot until you've recovered your corpse.
- **You are NEVER unarmed.** Even with no Diablo weapon equipped, you always have the pistol (the game defaults to it). "No weapon" means no Diablo weapon in the WEAPON slot — you can still fight with the pistol. Don't avoid combat thinking you're defenseless.
- **Exception:** If the corpse is unreachable (blocked by enemies you can't bypass, or in an area you can't safely reach) and there are no visible weapons or loot to pick up, you may engage nearby enemies with the pistol. Killing an enemy may drop the weapon you need. Don't wander aimlessly — if you fight, fight to get gear.

## General Tips

- Don't waste time on empty rooms; head toward doorways and corridors.
- If health is low (<30%) and no enemy nearby, keep exploring for health pickups.
- Equip upgrades immediately — don't save them for later.
- In combat, always end your turn after queueing actions.

---

*This guide evolves as the agent learns.*
