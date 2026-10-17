# How to Play DiabloDoom (Agent Strategy Guide)

This is the public strategy guide for teaching our AI agent to play DiabloDoom — a turn-based Diablo-style mod of Doom. The agent plays by looking at screenshots and deciding actions. If you have ideas for better tactics, we want to hear them!

## KITING DEMO (temporary - for the next run only)
**PRIORITY: Find a melee enemy (Flesh worm, Demon, Spectre) and KITE it for the camera.**
- Kiting = back up (MOVE_BACK) when the melee enemy gets close, then ATTACK, repeat.
- Keep your distance — don't let it touch you.
- Capture a REPLAY while kiting.
- This is a demo for the user. Prioritize this over other objectives.

## The Game

DiabloDoom combines Doom's first-person shooting with Diablo's loot system and turn-based combat. The agent explores levels, fights monsters in turn-based mode, picks up loot, equips gear, and tries to survive.

## Objective

Play well — explore the level, engage enemies in turn-based combat, pick up loot. Play like a competent player, not a bot grinding a specific drop.

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
- **Don't over-queue:** Queue 2-3 attacks per round, END turn, assess. Dumping 10 attacks at once wastes ammo on misses and leaves you exposed if the enemy closes in.
- **Check ammo first:** Never queue more attacks than your current ammo. If you see "NO AMMO" skips in the log, you're queuing too many.
- **Kill ranged first:** Serpentipedes and Imps out-DPS the pistol at range. If multiple enemies are visible, target the ranged ones before melee — or break LOS and force them to come to you.
- **Use cover:** Peek around corners and doors. Move out, queue 2-3 attacks, move back behind cover, END turn. Repeat. Don't stand in the open against ranged enemies.
- **Capture the fight:** If your HP drops below 50% during combat, capture a REPLAY during the PROGRESS phase (after you END TURN, while your queued commands are executing) — NOT during planning. The replay buffer holds the last N seconds; capturing during planning just records the frozen planning screen. Wait for the progress phase, then send REPLAY.

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
