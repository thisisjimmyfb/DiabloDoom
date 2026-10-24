# How to Play DiabloDoom (Agent Strategy Guide)

This is the public strategy guide for teaching our AI agent to play DiabloDoom — a turn-based Diablo-style mod of Doom. If you have ideas for better tactics, we want to hear them!

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

- **Passive damage (damaging floors, crushers) — two gears.** Bad ground kills by latency, not force: in explore the world runs in real time while you think, and the floor ticks the whole time. Passive damage does NOT stop a `GOTO` — it rides out the ticks at full run speed until it arrives.
  - **Sprint out (first reaction):** if the way out is known and short — you just stepped in, or safety is one straight line back — immediately `GOTO` your last safe position. `GOTO` moves at run speed (the Shift-held speed). No screenshot first; the command is the reaction. And never `GOTO` *across* a hazard toward somewhere beyond it: the walk will not stop on its own when the ground turns bad, it will cross the whole pool if the target is past it.
  - **Freeze-escape (when the exit isn't obvious):** send `ATTACK`. From explore it enters turn-based combat and freezes the world, with a target-less attack queued (if your weapon cannot fire, planning still holds with an empty queue — the freeze works either way). `UNDO` deletes the queued attack and refunds its TP. Read the frozen screenshot, decide the way out, queue movement (`MOVE_FWD`/`MOVE_BACK`/`STRAFE_L`/`STRAFE_R`, `TURNL`/`TURNR` to face) until TP is spent, then `END` to execute. Damage accrues only while the queue executes. If you are still on bad ground when explore resumes, `ATTACK` again and repeat. Never plan an escape from a live screenshot with the ticks running.
- **Retreat when hurt:** If HP drops below 40% and you're fighting a ranged enemy, STOP attacking. Move behind a wall, door, or corner to break line of sight. Let your HP recover or reposition — don't trade shots you'll lose.
- **Queue aggressively:** Fill the turn queue every round — batch 6-10 actions (moves + attacks) per iteration, then END turn — when enemies are far and no enemy projectiles are in flight toward you. When you are very close to an enemy or projectiles are inbound, drop to one move at a time with a fresh SCREENSHOT between moves (see the combat movement rule). A half-empty queue wastes the round. The old "2-3 attacks" caution is retired: more actions per loop means more progress per minute.
- **Check ammo first:** Never queue more attacks than your current ammo. If you see "NO AMMO" skips in the log, you're queuing too many.
- **Kill ranged first:** Serpentipedes and Imps out-DPS the pistol at range. If multiple enemies are visible, target the ranged ones before melee — or break LOS and force them to come to you.
- **Use cover:** Peek around corners and doors. Move out, queue a full batch of attacks, move back behind cover, END turn. Repeat. Don't stand in the open against ranged enemies.
- **See with the segment videos:** the game continuously saves 15-second segment videos — find the newest one in `/tmp/doomshots/` and read its frames to see what just happened: enemy movement, your own path, the moment a fight turned. For a single still frame, use SCREENSHOT.

### Gear Check
- You are NEVER unarmed — you always have the pistol, even with no Diablo weapon equipped. "No weapon" means no Diablo weapon in the WEAPON slot. Don't avoid combat thinking you're defenseless.
- If you have no Diablo weapon equipped (vanilla pistol only): prioritize finding a Diablo weapon over fighting tough enemies. But you CAN fight — just be cautious.

### Your Trail, Traps, Edges and Corners

The game tracks where you have actually been. Your screenshot shows one moment — the stamps in the log show the pattern, and the pattern is what gets you trapped.

- **Read your trail in the log.** Every GOTO result is stamped with start and end positions (`moved N from (x,y)`), so you always know whether a command actually took you somewhere — keep the recent stamps in mind and compare each new one against them.
- **Your area memory carries your trail across cycles** (`~/workspace/MEMORY.md`, mechanics in SYSTEM_PROMPT.md): it records what you've seen — areas visited, doors found and whether they opened, wall sections where `USE` already did nothing, targets that returned `no path`. Read it at the start of the run; navigate by its doors, never re-probe its no-path targets, and never re-`USE` a plain-wall spot it records as no-effect — but a door it records as *didn't open* is a live lead, not a retirement: it stays on the hunt until it opens, or is proven keyed with the key still unfound after full coverage of every reachable area. And write this run's observations into it as you go, so the next cycle starts knowing what you learned.
- **Revisit check — read your stamps in the live log:** The log updates during play now. After every GOTO, read its stamped end position and compare it with the end positions of your earlier moves this session (the log's stamps are the trail; all positions are world coordinates). If you land within ~128 units of a point already in your trail, you are revisiting — name it, and pick an opening or heading that is not in the trail instead of continuing the loop.
- **Walls are where doors live — work them, don't avoid them:** A frame full of wall is not a dead end; it's an unscanned wall. The trap is facing a wall and doing nothing with it (or GOTO-ing into it repeatedly), not facing one. Work the wall: move along it in short hops and scan it section by section for the paired side strips, a colored side panel, or a seam-bounded section; when vision nominates a section, face its center square-on, move right up to it, and `USE` it. A wall stretch counts as *checked* only once you've seen it up close along its length — a glance from across the room doesn't count, because up close is where the side strips and panels become visible. Turn away only from wall you have actually scanned and found no bounded section in. Never test plain, unnominated wall at random with `USE` — but never skip a nominated section either.
- **You are trapped when any of these is true:**
  - your last stamped points huddle in a small box (read them back in the log), or
  - your last 3 movement commands each moved you less than ~32 units, or
  - your stamps show you revisiting the same ±48-unit area 3 or more times.
- **When trapped, name it and run the escape ladder — don't keep poking:**
  1. Step out sideways: GOTO to a point ~120 units perpendicular to your facing from your current stamp (left or right), then GOTO onward.
  2. GOTO back out the way you came in — target an earlier stamp from your trail in the log.
  3. Wall-follow: turn so a wall is on your left, then GOTO in hops along it — each hop a point ~150 units along the wall from your current stamp; at each stop (arrived or blocked), turn 45–90° toward the open side and GOTO again, keeping the wall on the same side. A corner ends when the wall ends — follow it around instead of bouncing off it.
  4. In a trench or pit: move ALONG the low ground, parallel to the high wall, never straight at it. Look for the step up or the ramp at the trench's ends.
- **Height changes and escalators:** A level change is not always a door. An escalator or lift is a moving platform between floor heights, and it is one of the mechanisms above: work it with the USE, WAIT, MOVE pattern and adapt to what it does — if it responds to `USE`, the batch brings it to you and steps you aboard; if it moves when you walk onto it, it is automatic and the walk itself is the activation. Either way, once aboard it carries you to the other level. If a long GOTO clamps or ends `blocked` on approach, use short hops instead. You can tell two floors apart by vision even though log stamps are (x,y) only: the camera sits at your eye height, so when you change levels the whole view's height relationship shifts, and the feet ruler breaks across a step (a figure's feet sit wrong for its size when it's on a different floor). A sheer ledge is different from a lift: no ramp, no step down, and a step up of more than 24 units doesn't exist to the pathfinder, so it will refuse targets across it — that is not a failed lift, it's a wall at that spot, so record it and look for the stepped or moving route, usually at the structure's ends.
- **Edges and corners:**
  - Approach openings square-on: face the CENTER of the doorway or gap before walking through. Clipping a corner post and stopping is the #1 way this farm gets wedged.
  - If you stop against a door frame or a corner edge, don't GOTO again at the same angle — you will re-wedge in the same spot and your stamps will show the cluster. Back off: GOTO to a point ~60 units behind your current stamp (away from the opening), then a short hop 30–60 units toward the opening's center, then GOTO through.
  - In tight or ledgy geometry, use SHORT hops — destinations 50–100 units from your current stamp — and wall-following: distant destinations get clamped or end `blocked`, so hop along the opening a little at a time.
  - Never issue repeated forward GOTOs at slightly different angles hoping one slips through. Change something structural instead: strafe, back out, or follow the wall.

## Exploration Strategy

**Prefer new ground over old ground.** You find it with your own eyes, not a map readout. Watch your own coverage: if your last several moves haven't taken you into a room you haven't recorded in your area memory, you are looping. Then do a **survey spin**: turn in steps all the way around (a full 360°), pausing briefly at each facing, then find the newest segment video — it now holds the whole panorama, so review its frames to catalog every opening, doorway, and corridor around you. Cross off the directions your stamped trail says you've already taken, and head for an opening you haven't tried, even if it means crossing known rooms to reach it. Backtracking is a means to reach new ground, never the activity itself. When two routes look equal, always take the one you haven't been down. Ground truth for where you've been is the live log: every GOTO result is stamped with world-coordinate start and end positions, and those stamps are your trail — read them before you pick a direction.

When in EXPLORE mode (not in combat), priority order:

1. **Loot prompt visible** ("Loot nearby, press G to loot") → Collect IMMEDIATELY. (If it reads "Loot nearby, but backpack full", skip ground loot — nothing fits — but still COLLECT a corpse if one is the target.) This is the top priority. The prompt appears for Diablo loot drops AND player corpses.

   **COLLECT rule: send COLLECT only when the game shows "Loot nearby".** The message in your current screenshot/replay frames (or the `[LOOT] available on map - press G to loot` line in the log for the current position) is the one and only trigger. Never send COLLECT speculatively — not as a probe, not because loot exists somewhere on the map. No "Loot nearby" shown means no COLLECT: keep exploring instead. The prompt is reachability-gated by the same rule COLLECT uses and only fires for loot within 512 units, so when it shows, the loot is genuinely collectable; when it doesn't, a COLLECT attempt only burns the run's budget confirming that.

2. **After picking up loot** → Check inventory, equip upgrades immediately:
   - **Match the gun to your stats — the first rule of gearing.** Every gun is AD or AP (its magazine type is its damage type). An AD gun scales with AD stats; an AP gun with AP stats. Run an AD gun when your gear stacks AD, an AP gun when it stacks AP — and when you change gun types, re-check the rest of your gear so its flat and % bonuses follow the gun. A mismatched gun wastes every damage-% roll on your other pieces.
   - **Weapon stats, in priority order: crit chance, crit damage, damage %, attack speed.** When choosing between weapons, prefer the one whose rolls carry crit chance / crit damage / damage % (of Slaying for AD, of the Magus for AP) / attack speed (haste). Matched flat damage (Soldier's on an AD gun, Wizard's on an AP gun) comes after those four; utility rolls last. Leech's place depends on your total — see the next rule.
   - **Life leech exception: while your TOTAL life leech is under 10%, leech rolls rank with the priority four.** A weapon or ring whose leech gets your total to 10% beats a marginally better crit roll without leech — sustain first. Once your total leech is 10% or more, leech is just less important — it no longer ranks with the priority four, but it still counts in a piece's favor; it is never worthless. (Check the character screen for your current total.)
   - Helm/Armor/Boots/Gloves/Belt: equip if higher Defense or resistances (Defense is the sum of those five slots only; shields give no Defense). A piece at 0 durability gives no Defense until repaired. On these pieces too, a roll that feeds your gun (its damage %, crit, haste) outranks a generic roll of similar size.
   - Rings/Amulet/Focus: equip if its stats feed your current gun's type (AD rolls with an AD gun, AP rolls with an AP gun) or the slot is empty; crit and haste rolls outrank flat stats here too.
   - Don't hoard upgrades in the backpack — equip them
   - **Salvage the clearly inferior to make room.** After equipping, scrap backpack items that are clearly worse than what's equipped in their slot: a weapon with lower damage and none of the priority rolls (crit / crit damage / damage % / attack speed), armor with lower Defense and no roll feeding your gun, jewelry whose stats don't feed your gun's type. Salvaging pays Essence and frees grid space for future loot — a pack full of junk is loot left on the ground later. Read item indices off the inventory screenshot and salvage by those; never guess an index. Don't salvage: equipped items, anything for a slot that's currently empty, a near-tie you can't confidently judge, a piece that would become your best if you switched gun types and it completes that package — or, while your total life leech is under 10%, any piece carrying leech (it's not junk, it's your sustain until the threshold is met). If a pickup fails because the backpack is full, salvage the clearly-inferior items first, then collect again — don't walk away from loot over a pack full of scrap.

3. **Vanilla pickup on ground** (weapon, ammo, health, armor) → GOTO onto it. These are walk-over pickups, no key needed.

4. **Door section spotted** (any door — plain or colored; identified by the paired narrow side strips bounding a narrow section of wall, NOT by its texture, which differs in every WAD) → Go to it and open it. Doors lead to the next area, so look for them and treat one as your destination: face its center, move up to it with GOTO until you're close, then line up square-on in front of it with `WALK <angle>` and cross it with the batched sequence — `USE`, `WAIT`, `WALK 0`, all in one batch (first time: `USE`, `WAIT`, `SCREENSHOT` to confirm it opens, then `WALK 0` through). Don't wait for a screenshot of the opening before crossing: the door won't hold it. Don't turn away from a closed door as if it were a wall, and don't keep GOTO-ing into it — up close + the batched `USE`/`WAIT`/`WALK 0` is how it opens. And don't send `USE` speculatively at wall sections vision hasn't nominated — USE confirms a door your eyes picked out, it isn't how you search for one.
   - **Colored door** (a side panel colored red, green, or blue) → It's a keyed door: you need the matching key. Send `KEYS` to check which keys you have:
     - If you have the key: move up to the door, send `USE`, then walk through once it opens.
     - If not: the key is somewhere in the level — explore systematically (every room, corners, alcoves, platforms) until you SEE it as a small colored card, then GOTO toward it and walk over it to pick it up.

5. **Key visible** (small red/blue/yellow card on ground or pedestal) → GOTO toward it immediately. Walk over it to pick up. This is high priority.
   - You don't need a precise estimate. Add your rough distance to your current stamp: facing east with the key ~150 ahead → GOTO (your x + 150) (your y). Close ~50, medium ~150, far ~300.
   - The game will clamp if you overshoot into a wall. After moving, take a new screenshot: if the key is still visible, send another GOTO to get closer. Repeat until you walk over it. Iterative correction beats one-shot precision.

6. **Enemy visible in distance** → GOTO toward it to engage

7. **Facing wall** → Work it, don't flee it: scan the stretch in front of you for a door section (paired side strips, a colored side panel, a seam-bounded panel) and `USE` any section vision nominates. Move along the wall in short hops, scanning as you go. Turn away only once the stretch is checked up close and holds no bounded section — then turn toward an opening or an unchecked wall, never just away from this one. Do not GOTO into the wall repeatedly.

8. **Open area** → GOTO forward into it, explore

## Level Exploration and Exit

Your goal is to progress through the level: explore → find keys → unlock doors → reach the exit.

**The exit hunt — how progress actually happens:**
1. **The objective is the exit to the next area.** Loot and kills are secondary; an unopened door is an unsearched area between you and it.
2. **Exits are reached through doors.** Find doors and open them with `USE`. Hunt doors deliberately: they are sections *in* walls, so finding them means working walls up close, not avoiding them.
3. **Every door you find gets recorded** in the area memory (`~/workspace/MEMORY.md`) with its coordinates and its state: opened, didn't-open-from-here, or keyed (with its panel color). A door that didn't open stays in the file **as a door** — never reclassify one as "not a door" or as plain wall, and never let a failed `USE` end the search at it.
4. **A colored side panel — red, green, or blue — means a keyed door.** Check `KEYS`. If you don't have that key, finding it becomes the objective.
5. **Keys are found by visiting ALL areas.** Cover the current area completely — every room entered, every wall scanned up close for door sections, every alcove checked — before concluding it is exhausted. "Boxed in" is a verdict you earn only after full coverage, never a first impression from a few failed probes.

**Exploration strategy:**
- **Every area is a room.** Technically the level is made of sectors, and a sector is a room: it has a boundary you cannot cross except through its doors, doorways, and openings (its portals). Outdoor areas are rooms too — their boundary may be a fence, a hedge, or a line of vegetation instead of a wall, and it works the same way: scan the boundary for its portals. So exploration is room-by-room: enter a room, scan its *whole* boundary for doors, open them, move on to the next room. Your coverage is the set of rooms you've actually entered and recorded — keys and exits are found by extending it.
- **A room can have more than one door.** Finding one door in a room does NOT finish the room — keep scanning the rest of its boundary; the second or third door is often the one that leads somewhere new. And when the current room's walls are exhausted, go back and check ALL the walls of the rooms you visited before: earlier rooms were often crossed quickly, before their boundaries were fully scanned, and a missed door there is the most likely place the way forward is hiding. No room counts as finished until its entire boundary has been seen up close, section by section — including rooms you visited long ago.
- **Backtracking is a tool, not a failure.** Sometimes you need to go back: when you are lost, backtracking is how you get found. If you have worked the current area for a long stretch — many moves, no new room, no unopened door or open lead left in it — the area is done for now, and pushing further into new ground is the mistake. Go back: return to the most promising earlier area (the notebook's OPEN LEADS list, top first; if it has none, the earliest room whose boundary was never fully scanned) and work its walls there. Travel to a lead is never wasted motion, whatever the trail looks like.
- Doom levels are divided into sectors connected by doorways and doors. To explore the full level, go through every doorway and open every door you see.
- A doorway is an opening in the wall or a visible passage; a closed door is a narrow section of wall bounded on both sides by narrow strips/posts that differ from the section and the surrounding wall — identify it by those paired side strips, not by its surface texture, which every WAD draws differently. When you see an opening, GOTO through it. When you see a closed door section, don't treat it as a wall: move right up to it, send `USE` to open it, then step through with `WALK 0` into the next area.
- Check your coverage in your area memory: if you haven't entered a new room in your last several moves, one of two things is true — you're looping (pick a doorway you haven't taken), or this area is exhausted and it's time to backtrack (see below).
- **You don't need 100%, and secret areas are out of scope.** Some sectors are secrets (hidden behind fake walls or triggered by switches), but no secret is ever required to advance or finish a level, and finding secrets is a stretch goal for later — not a current objective. Never hunt for secret walls and never send `USE` at a wall hoping it is one. If you've covered most of the rooms you can reach and can't find new doorways, the way on is in the areas you've already explored — focus on finding the exit there.
- **If stuck**: If you haven't entered a new room in several minutes, the exit is in an area you've already visited, or you walked past a doorway or door section without noticing it. Re-check the areas you've been in for a missed doorway, a door section (paired side strips), or an exit switch you might have missed — and re-scan the FULL boundaries of earlier rooms, not just the current one: a room can hold more than one door, and the one you missed is usually in a room you crossed early and never fully scanned. Do not assume a secret is what you're missing, and never call yourself boxed in until every visited room's boundary has been checked up close.
- If you're in a room with multiple doorways, pick one you haven't taken yet.
- Keys and important items are often in side rooms, alcoves, or on platforms — check these, don't just run down the main corridor.

**Finding the exit:**
- The level exit is usually a switch on a wall (often with a red EXIT sign above it), a teleporter pad (glowing platform), or a door marked EXIT.
- When you see the exit, GOTO toward it. For a switch, walk up to it and press USE (the game handles activation on touch for most exits).
- If you've explored most of the level and haven't found the exit, look for areas you haven't visited — it's often behind a locked door or in a less obvious location.

**The full loop:**
1. Explore new areas (GOTO through doorways)
2. If you see a door, move up to it and send `USE` to open it, then proceed through to the next area
3. If you see a colored door, check KEYS — find the key by vision if needed, then move up to it, `USE`, and go through
4. When you find the exit, go to it

## Death and Corpse Recovery

- When you die, review the segment video covering your death before you respawn — its frames show what killed you and where your corpse fell, which is what you navigate back to.
- **After respawning, recovering your corpse is the #1 priority.** Your inventory and equipment are on the corpse at the death location.
- GOTO back to it immediately. Do NOT engage in new combat or explore for loot until you've recovered your corpse.
- **Corpse pose check:** when your corpse first comes into view on the approach, note its pose in your report — prone (lying flat) or standing. A one-line observation; the corpse-fall fix (45b3da2c) still awaits visual confirmation in production footage.
- **You are NEVER unarmed.** Even with no Diablo weapon equipped, you always have the pistol (the game defaults to it). "No weapon" means no Diablo weapon in the WEAPON slot — you can still fight with the pistol. Don't avoid combat thinking you're defenseless.
- **Exception:** If the corpse is unreachable (blocked by enemies you can't bypass, or in an area you can't safely reach) and there are no visible weapons or loot to pick up, you may engage nearby enemies with the pistol. Killing an enemy may drop the weapon you need. Don't wander aimlessly — if you fight, fight to get gear.

## General Tips

- Don't waste time on empty rooms; head toward doorways and corridors.
- If health is low (<30%) and no enemy nearby, keep exploring for health pickups.
- Equip upgrades immediately — don't save them for later.
- In combat, always end your turn after queueing actions.

---

*This guide evolves as the agent learns.*
