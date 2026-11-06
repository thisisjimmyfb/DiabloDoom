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
   - Rings/Amulet/Clips: equip if its stats feed your current gun's type (AD rolls with an AD gun, AP rolls with an AP gun) or the slot is empty; crit and haste rolls outrank flat stats here too.
   - Don't hoard upgrades in the backpack — equip them
   - **Salvage the clearly inferior to make room.** After equipping, scrap backpack items that are clearly worse than what's equipped in their slot: a weapon with lower damage and none of the priority rolls (crit / crit damage / damage % / attack speed), armor with lower Defense and no roll feeding your gun, jewelry whose stats don't feed your gun's type. Salvaging pays Essence and frees grid space for future loot — a pack full of junk is loot left on the ground later. Read item indices off the inventory screenshot; INSPECT an item to read its exact stats, then salvage by its index; never guess an index. Always INSPECT, then WAIT, before equipping or salvaging an item. Don't salvage: equipped items, anything for a slot that's currently empty, a near-tie you can't confidently judge, a piece that would become your best if you switched gun types and it completes that package — or, while your total life leech is under 10%, any piece carrying leech (it's not junk, it's your sustain until the threshold is met). If a pickup fails because the backpack is full, salvage the clearly-inferior items first, then collect again — don't walk away from loot over a pack full of scrap.

3. **Pickup on ground** → GOTO onto it to pick it up. Every item on the ground can be picked up, whatever it is — don't leave one behind because of its type. Exception: health items can be conserved — leave them on the ground until health is below maximum, rather than picking them up at full health.

4. **Key visible** (small red/blue/yellow card on ground or pedestal) → GOTO toward it immediately. Walk over it to pick up. This is high priority.
   - You don't need a precise estimate. Add your rough distance to your current stamp: facing east with the key ~150 ahead → GOTO (your x + 150) (your y). Close ~50, medium ~150, far ~300.
   - The game will clamp if you overshoot into a wall. After moving, take a new screenshot: if the key is still visible, send another GOTO to get closer. Repeat until you walk over it. Iterative correction beats one-shot precision.

5. **Enemy visible in distance** → GOTO toward it to engage

   - **Fists and the chainsaw are melee — they only hit a foe right next to you.** Close to melee range before attacking with them; an attack from farther away can't land.
   - **Don't attack barrels with melee — they explode.** At melee range you take the full splash, so leave them alone up close.

6. **Facing wall** → Work it, don't flee it: scan the stretch in front of you for geometry that looks different from its neighbors and `USE` any piece vision nominates. Move along the wall in short hops, scanning as you go. Turn away only once the stretch is checked up close and holds nothing that looks different from its neighbors — then turn toward an opening or an unchecked wall, never just away from this one. Do not GOTO into the wall repeatedly.

7. **Open area** → GOTO forward into it, explore

## Weapons

How each weapon type works, and what it is good against. Guns are AD or AP by magazine type (see the gearing rules above) — match the gun to your stats. Base damage is low on purpose; your gear's rolls are the real damage. Strength requirements (gear STR total): fists, pistol, plasma 0 (free); shotgun 2; double shotgun 4; chainsaw 5; rocket launcher 6; chaingun 8; BFG 10. A gun you can't lift falls back to fists.

- **Fists.** Unarmed default. Melee only — a foe right next to you, nothing farther. Very fast (fastest attack in the game) and costs 1 point, no ammo. Strength scales fist damage. Good for: finishing a weak enemy cheaply, fighting when you have no weapon. Bad against: anything at a distance — fists can't reach it, so don't stand there punching air; switch to a ranged gun.
- **Pistol (Sidearm).** AD. Single shot, moderate cost (4 points), reliable at range. Good for: a general-purpose AD ranged weapon when you have nothing better; picking off weak enemies (zombies, imps) at a distance without spending much.
- **Shotgun.** AD. Close-range burst — 4 pellets, each rolling separately, so all four have to connect. Good for: single tough enemies up close (all pellets hit = heavy burst), or a tight group at close range. Bad at a distance — the pellets spread and most miss.
- **Chaingun.** AD. 3-round burst, fast, cheap ammo (1 per shot). Good for: sustained AD fire at range — the reliable workhorse against anything at a distance when your gear stacks AD. Good against imps, zombies, and demons at medium range.
- **Rocket launcher.** AP. Fires a rocket with splash (128 radius, half damage to anything nearby). Slow, expensive (6 points), and charge-gated (3 charges). Good for: groups clustered together, and tough enemies at medium range — the splash punishes anything standing near the blast. Bad up close: the splash hits you too, so never fire it at point-blank range.
- **Plasma rifle (Pulse).** AP. Fast (same speed as the chaingun) and the fast AP weapon. Heat-based: each shot adds heat, and the weapon stops firing when it overheats — firing pauses the heat drain for a round, so longer bursts want more Energy (capacity). Good for: sustained AP fire at range against anything when your gear stacks AP; the go-to AP gun for ordinary fights.
- **BFG.** AP. The ultimate: highest damage (10–18) with the biggest splash (192 radius). Expensive (8 points), slow, and a long cooldown (3 rounds) that gates its reload. Good for: the toughest enemies (Barons, Hell Knights, Cyberdemon, Spider Mastermind) and big groups — save it for the fights that matter, not for a lone zombie. With the BFG10K item, its BF Storm hits everything in view.
- **Chainsaw.** AD. Melee only, but it has inherent lifesteal (30% of damage returns as health). Good for: close-quarters sustain — when you're low on health and surrounded, the saw heals you as you fight. Bad against: anything at a distance, same as fists.
- **Double shotgun (SSG).** AP. 8 pellets — the biggest close-range burst in the game — with knockback that shoves the target back a tile. Cooldown-gated reload (breach). Good for: single tough enemies up close (all 8 pellets landing is devastating) and for keeping a dangerous enemy at arm's length with the knockback. Bad at a distance — most pellets miss.

When you change gun types, re-check your gear (see the gearing rules above): the flat and % bonuses have to follow the gun. And if the enemy is far away, use a ranged gun — don't walk up to it with fists or the chainsaw when a pistol, chaingun, or plasma rifle is in your pack.

## Death and Corpse Recovery

- When you die, review the segment video covering your death before you respawn — its frames show what killed you and where your corpse fell, which is what you navigate back to.
- **After respawning, recovering your corpse is the #1 priority.** Your inventory and equipment are on the corpse at the death location.
- GOTO back to it immediately. Do NOT engage in new combat or explore for loot until you've recovered your corpse.
- **Corpse pose check:** when your corpse first comes into view on the approach, note its pose in your report — prone (lying flat) or standing. A one-line observation; the corpse-fall fix (45b3da2c) still awaits visual confirmation in production footage.
- **Swap back after combat.** If you swap to a different weapon for any reason, swap back to your best weapon after combat ended.
- **Exception:** If the corpse is unreachable (blocked by enemies you can't bypass, or in an area you can't safely reach) and there are no visible weapons or loot to pick up, you may engage nearby enemies with your fists. Killing an enemy may drop the weapon you need. Don't wander aimlessly — if you fight, fight to get gear.

## General Tips

- If health is low (<30%) and no enemy nearby, keep exploring for health pickups.
- Equip upgrades immediately — don't save them for later.
- In combat, always end your turn after queueing actions.

## Damage avoidance without regen

No-regen damage rule: even if you did not die, do not take damage unnecessarily when you have no health regen. HP does not come back on its own unless a replenish item is equipped (an equipped piece whose inventory tooltip reads \"Replenish X HP/s\"). If no replenish is equipped, treat HP as a depleting resource: do NOT voluntarily enter a damage area (slime, crusher, and the like), and do NOT start a trip whose route is known to gate on damage — including a known gating return leg — for routine work such as surveys, wall trials, or hall trips. Work a goal you can reach without damage instead — and never stop the loop (see SYSTEM_PROMPT.md). If the only route to the current goal gates, switch to a goal you can work damage-free rather than spending HP to keep one goal moving. This rule is about environmental damage areas, not combat damage — fighting an enemy is a separate decision. Exceptions, the only necessary crossings: escaping a hazard you are already standing on, and recovering your corpse (gear is at stake). Once a replenish item IS equipped, a known gating leg is acceptable again, because the lost HP regenerates — but still do not take damage for no reason. Deprioritization transit exception: when you have determined the current goal cannot progress without taking damage and you deprioritize it under the blocked-goal rule, you may cross a known gating leg to reach the next goal you adopt. This covers leaving a blocked goal, not investigating a new lead and not routine travel: take the leg once, escape immediately if it gates, do not linger on damaging ground, and record the deprioritization and any damage taken in the notebook. Do not take a gating leg to leave a blocked goal below 20 HP. If you cannot confirm replenish from your inventory checks, assume none.

---

*This guide evolves as the agent learns.*
