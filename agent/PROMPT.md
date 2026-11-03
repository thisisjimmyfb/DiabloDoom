# How to Play DiabloDoom (Agent Strategy Guide)

This is the public strategy guide for teaching our AI agent to play DiabloDoom — a turn-based Diablo-style mod of Doom. If you have ideas for better tactics, we want to hear them!

## Exploration Strategy

When in EXPLORE mode (not in combat), priority order:

1. **Loot prompt visible** ("Loot nearby, press G to loot") → Collect IMMEDIATELY. (If it reads "Loot nearby, but backpack full", skip ground loot — nothing fits — but still COLLECT a corpse if one is the target.) This is the top priority. The prompt appears for Diablo loot drops AND player corpses.

   **COLLECT rule: send COLLECT only when the game shows "Loot nearby".** The message in your current screenshot/replay frames (or the `[LOOT] available on map - press G to loot` line in the log for the current position) is the one and only trigger. Never send COLLECT speculatively — not as a probe, not because loot exists somewhere on the map. No "Loot nearby" shown means no COLLECT: keep exploring instead. The prompt is reachability-gated by the same rule COLLECT uses and only fires for loot within 512 units, so when it shows, the loot is genuinely collectable; when it doesn't, a COLLECT attempt only confirms that nothing is collectable.

2. **After picking up loot** → Check inventory, equip upgrades immediately:
   - **Match the gun to your stats — the first rule of gearing.** Every gun is AD or AP (its magazine type is its damage type). An AD gun scales with AD stats; an AP gun with AP stats. Run an AD gun when your gear stacks AD, an AP gun when it stacks AP — and when you change gun types, re-check the rest of your gear so its flat and % bonuses follow the gun. A mismatched gun wastes every damage-% roll on your other pieces.
   - **Weapon stats, in priority order: crit chance, crit damage, damage %, attack speed.** When choosing between weapons, prefer the one whose rolls carry crit chance / crit damage / damage % (of Slaying for AD, of the Magus for AP) / attack speed (haste). Matched flat damage (Soldier's on an AD gun, Wizard's on an AP gun) comes after those four; utility rolls last. Leech's place depends on your total — see the next rule.
   - **Life leech exception: while your TOTAL life leech is under 10%, leech rolls rank with the priority four.** A weapon or ring whose leech gets your total to 10% beats a marginally better crit roll without leech — sustain first. Once your total leech is 10% or more, leech is just less important — it no longer ranks with the priority four, but it still counts in a piece's favor; it is never worthless. (Check the character screen for your current total.)
   - Helm/Armor/Boots/Gloves/Belt: equip if higher Defense or resistances (Defense is the sum of those five slots only; shields give no Defense). A piece at 0 durability gives no Defense until repaired. On these pieces too, a roll that feeds your gun (its damage %, crit, haste) outranks a generic roll of similar size.
   - Rings/Amulet/Focus: equip if its stats feed your current gun's type (AD rolls with an AD gun, AP rolls with an AP gun) or the slot is empty; crit and haste rolls outrank flat stats here too.
   - Don't hoard upgrades in the backpack — equip them
   - **Salvage the clearly inferior to make room.** After equipping, scrap backpack items that are clearly worse than what's equipped in their slot: a weapon with lower damage and none of the priority rolls (crit / crit damage / damage % / attack speed), armor with lower Defense and no roll feeding your gun, jewelry whose stats don't feed your gun's type. Salvaging pays Essence and frees grid space for future loot — a pack full of junk is loot left on the ground later. Read item indices off the inventory screenshot and salvage by those; never guess an index. Don't salvage: equipped items, anything for a slot that's currently empty, a near-tie you can't confidently judge, a piece that would become your best if you switched gun types and it completes that package — or, while your total life leech is under 10%, any piece carrying leech (it's not junk, it's your sustain until the threshold is met). If a pickup fails because the backpack is full, salvage the clearly-inferior items first, then collect again — don't walk away from loot over a pack full of scrap.

3. **Vanilla pickup on ground** (weapon, ammo, health, armor) → GOTO onto it. These are walk-over pickups, no key needed.

4. **Mechanism candidate spotted** (any piece of geometry that looks different from its neighbors, or that changes over time — flashing, pulsing, blinking) → Go to it and confirm it. Face its center, move up to it with GOTO until you're close, then line up square-on in front of it with `WALK <angle>` and send `USE`, `WAIT`, then `SURVEY`, then review the dumped survey frames (survey_<ts>_NN_AAAdeg.png, all 36, in rotation order) to confirm what it does — frame 01 is the direct view of the mechanism and carries the verdict; the rest of the frames matter only if frame 01 shows no activation. If it lets you through, cross it with the batched sequence — `USE`, `WAIT`, `WALK 0`, all in one batch. Don't wait for a screenshot of the opening before crossing: the opening won't hold. Don't turn away from a candidate as if it were wall, and don't keep GOTO-ing into it — up close + the batched `USE`/`WAIT` is how it opens. And don't send `USE` at uniform wall — USE confirms a piece your eyes nominated by difference, it isn't how you search undifferentiated wall.
   - **Colored door** (a side panel colored red, green, or blue) → It's a keyed door: you need the matching key. Send `KEYS` to check which keys you have:
     - If you have the key: move up to the door, send `USE`, then walk through once it opens.
     - If not: the key is somewhere in the level — explore systematically (every room, corners, alcoves, platforms) until you SEE it as a small colored card, then GOTO toward it and walk over it to pick it up.

5. **Key visible** (small red/blue/yellow card on ground or pedestal) → GOTO toward it immediately. Walk over it to pick up. This is high priority.
   - You don't need a precise estimate. Add your rough distance to your current stamp: facing east with the key ~150 ahead → GOTO (your x + 150) (your y). Close ~50, medium ~150, far ~300.
   - The game will clamp if you overshoot into a wall. After moving, take a new screenshot: if the key is still visible, send another GOTO to get closer. Repeat until you walk over it. Iterative correction beats one-shot precision.

6. **Enemy visible in distance** → GOTO toward it to engage

7. **Facing wall** → Work it, don't flee it: scan the stretch in front of you for geometry that looks different from its neighbors and `USE` any piece vision nominates. Move along the wall in short hops, scanning as you go. Turn away only once the stretch is checked up close and holds nothing that looks different from its neighbors — then turn toward an opening or an unchecked wall, never just away from this one. Do not GOTO into the wall repeatedly.

8. **Open area** → GOTO forward into it, explore

## Level Exploration and Exit

Your goal is to progress through the level: explore → find keys → unlock keyed doors → reach the exit.

**The exit hunt — how progress actually happens:**
1. **The objective is the exit to the next area.** Loot and kills are secondary; an unopened colored (keyed) door is an unsearched area between you and the exit — only keyed doors are guaranteed requirements to reaching the exit.
2. **Exits are reached via USE mechanisms.** Some are gated by doors, others by an escalator, another might be a wall switch that opens an area. Hunt them deliberately: mechanisms are often sections *in* walls, so finding them means working walls up close, not avoiding them. Many mechanisms have a timed component, so WAIT after USE; others open a path only briefly, so sprint through it at once in the same batch — don't stop for a screenshot first, or the window closes.
3. **Every mechanism you find gets recorded** in the area memory (`~/workspace/MEMORY.md`) with its coordinates and its state: opened, didn't-open-from-here, or keyed (with its panel color).
4. **A colored side panel — red, green, or blue — means a keyed door.** Check `KEYS`. If you don't have that key, finding it becomes the objective.
5. **Keys are found by visiting ALL areas.** An area-goal is done for now when it stops yielding a new mechanism: visits, scans, and examinations that turn up no new mechanism. When that happens, look for the exit and move on to the next area, or backtrack to previous areas with a finer comb (see Goals above). "Boxed in" is a stronger verdict — nowhere left to go at all — and you earn it only after full coverage, never a first impression from a few failed probes.

**Exploration strategy:**
- **Every area is a room — a room is a space you can walk within without using a mechanism.** Technically the level is made of sectors, but a sector is not always one room: it has a boundary you cannot cross except through its mechanisms and openings (its portals). Outdoor areas are rooms too — their boundary may be a fence, a hedge, or a line of vegetation instead of a wall, and it works the same way: scan the boundary for its portals. Reaching a room often requires a mechanism — a door, an escalator, a switch — but once inside it, you move by walking. The exception is a sector divided by a mechanism: an escalator splitting one sector into two levels makes two rooms, one per level, because you cannot walk between levels without the mechanism — each level gets its own boundary scan. So exploration is room-by-room: enter a room, scan its boundary for mechanisms, open them, move on to the next room. Your coverage is the set of rooms you've actually entered and recorded — keys and exits are found by extending it.
- **A room can have more than one mechanism.** Finding one mechanism in a room does NOT finish the room — keep scanning the rest of its boundary; the second or third mechanism is often the one that leads somewhere new. And when the current room stops yielding a new mechanism, go back and check ALL the walls of the rooms you visited before with a finer comb: earlier rooms were often crossed quickly, before their boundaries were fully scanned, and a missed mechanism there is the most likely place the way forward is hiding. A room is done for now when it stops yielding a new mechanism — you don't need to have seen every section of its boundary, and you don't need to visit every room (see Goals above).
- **Revisits are a planned phase, not a failure.** When the current goal stops yielding a new mechanism, go back and work previous areas with a finer comb: return to the most promising earlier area (the notebook's OPEN LEADS list, top first; if it has none, the earliest room whose boundary was never fully scanned) and work its walls there. A finer pass re-works mechanisms, re-scans wall sections up close, and re-tests the calls you're least sure of. Travel to a lead is never wasted motion, whatever the trail looks like.
- Doom levels are divided into sectors connected by openings and mechanisms.
- A doorway is an opening in the wall or a visible passage; one common closed-mechanism signature is a narrow section of wall bounded on both sides by narrow strips/posts that differ from the section and the surrounding wall — identify that signature by those paired side strips, not by its surface texture, which every WAD draws differently. When you see an opening, GOTO through it. When you see a closed mechanism section, don't treat it as a wall: move right up to it, send `USE`, then step through with `WALK 0` into the next area.
- Check your coverage in your area memory: if you haven't entered a new room in your last several moves, ask whether this goal is still yielding a new mechanism. If it is, you're on a deeper pass — keep going. If it isn't, switch goals (see Goals above).
- **Fill a goal until it stops yielding a new mechanism — and you still don't need 100%.** Record detail as you go (walkable points, edges, hazards), but detail alone does not keep a goal alive: a goal is done when work on it yields no new mechanism. That conviction is the bar, not a room count: you don't need to visit all rooms, and secret areas are out of scope. Some sectors are secrets (hidden behind fake walls or triggered by switches), but no secret is ever required to advance or finish a level, and finding secrets is a stretch goal for later — not a current objective. Never hunt for secret walls and never send `USE` at a wall hoping it is one. If you've covered most of the rooms you can reach and can't find new openings, the way on is in the areas you've already explored — focus on finding the exit there.
- If you're in a room with multiple openings, pick one you haven't taken yet.
- Keys and important items are often in side rooms, alcoves, or on platforms — check these, don't just run down the main corridor.

**Finding the exit:**
- The level exit is usually a switch on a wall (often with a red EXIT sign above it), a teleporter pad (glowing platform), or a door marked EXIT.
- When you see the exit, GOTO toward it. For a switch, walk up to it and press USE (the game handles activation on touch for most exits).
- If you've explored most of the level and haven't found the exit, look for areas you haven't visited — it's often behind a locked door or in a less obvious location.

**The full loop:**
1. Explore new areas (GOTO through openings)
2. If you see a mechanism, move up to it and send `USE`, then proceed through to the next area
3. If you see a keyed door, check KEYS — find the key by vision if needed, then move up to it, `USE`, and go through
4. When you find the exit, go to it

## Death and Corpse Recovery

- When you die, review the segment video covering your death before you respawn — its frames show what killed you and where your corpse fell, which is what you navigate back to.
- **After respawning, recovering your corpse is the #1 priority.** Your inventory and equipment are on the corpse at the death location.
- GOTO back to it immediately. Do NOT engage in new combat or explore for loot until you've recovered your corpse.
- **Corpse pose check:** when your corpse first comes into view on the approach, note its pose in your report — prone (lying flat) or standing. A one-line observation; the corpse-fall fix (45b3da2c) still awaits visual confirmation in production footage.
- **You are NEVER unarmed.** Even with no Diablo weapon equipped, you always have the pistol (the game defaults to it). "No weapon" means no Diablo weapon in the WEAPON slot — you can still fight with the pistol. Don't avoid combat thinking you're defenseless.
- **Swap back after combat.** If you swap to a different weapon for any reason, swap back to your best weapon after combat ended.
- **Exception:** If the corpse is unreachable (blocked by enemies you can't bypass, or in an area you can't safely reach) and there are no visible weapons or loot to pick up, you may engage nearby enemies with the pistol. Killing an enemy may drop the weapon you need. Don't wander aimlessly — if you fight, fight to get gear.

## General Tips

- If health is low (<30%) and no enemy nearby, keep exploring for health pickups.
- Equip upgrades immediately — don't save them for later.
- In combat, always end your turn after queueing actions.

## Damage avoidance without regen

No-regen damage rule: even if you did not die, do not take damage unnecessarily when you have no health regen. HP does not come back on its own unless a replenish item is equipped (an equipped piece whose inventory tooltip reads \"Replenish X HP/s\"). If no replenish is equipped, treat HP as a depleting resource: do NOT voluntarily enter a damage area (slime, crusher, and the like), and do NOT start a trip whose route is known to gate on damage — including a known gating return leg — for routine work such as surveys, wall trials, or hall trips. Work a goal you can reach without damage instead — and never stop the loop (see SYSTEM_PROMPT.md). If the only route to the current goal gates, switch to a goal you can work damage-free rather than spending HP to keep one goal moving. This rule is about environmental damage areas, not combat damage — fighting an enemy is a separate decision. Exceptions, the only necessary crossings: escaping a hazard you are already standing on, and recovering your corpse (gear is at stake). Once a replenish item IS equipped, a known gating leg is acceptable again, because the lost HP regenerates — but still do not take damage for no reason. Deprioritization transit exception: when you have determined the current goal cannot progress without taking damage and you deprioritize it under the blocked-goal rule, you may cross a known gating leg to reach the next goal you adopt. This covers leaving a blocked goal, not investigating a new lead and not routine travel: take the leg once, escape immediately if it gates, do not linger on damaging ground, and record the deprioritization and any damage taken in the notebook. Do not take a gating leg to leave a blocked goal below 20 HP. If you cannot confirm replenish from your inventory checks, assume none.

---

*This guide evolves as the agent learns.*
