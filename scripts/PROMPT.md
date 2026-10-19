# How to Play DiabloDoom (Agent Strategy Guide)

This is the public strategy guide for teaching our AI agent to play DiabloDoom — a turn-based Diablo-style mod of Doom. If you have ideas for better tactics, we want to hear them!

## Observation Strategy

**The loop is: ACT → REPLAY → THINK → ACT → REPLAY → THINK...**

The replay always comes *after* your commands play out, never before. This guarantees the video shows real gameplay, not idle thinking time.

**Explore mode: use REPLAY videos.**
- Videos give temporal context — you can see movement, enemy behavior, and what's changing.
- Send your WALK commands, wait ~5 seconds for them to start playing out, then REPLAY. The 15s buffer captures your movement in the moment.
- Batch movement: queue 8-12 WALK commands per cycle. One replay covers a lot of ground.
- After the replay, view the frames, decide your next batch of moves, and send them. Then replay again.
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

### Getting Unstuck
- If your position doesn't change after a movement command, you're against a wall.
- Don't repeat the same move. Strafe left/right or turn 90° instead.

## Exploration Strategy

When in EXPLORE mode (not in combat), priority order:

1. **Loot prompt visible** ("press G to loot") → Collect IMMEDIATELY. This is the top priority. The prompt appears for Diablo loot drops AND player corpses.

2. **After picking up loot** → Check inventory, equip upgrades immediately:
   - Weapon: equip if higher damage than current
   - Armor/Helm/Boots/Gloves/Shield: equip if higher armor or resistances
   - Rings/Amulet/Belt: equip if stats are better or slot is empty
   - Don't hoard upgrades in the backpack — equip them

3. **Vanilla pickup on ground** (weapon, ammo, health, armor) → Walk toward it. These are walk-over pickups, no key needed.

4. **Enemy visible in distance** → Walk toward it to engage

5. **Facing wall** → Turn left or right (alternate to avoid spinning)

6. **Open area** → Walk forward, explore

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
