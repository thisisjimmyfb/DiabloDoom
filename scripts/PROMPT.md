# How to Play DiabloDoom (Agent Strategy Guide)

This is the public strategy guide for teaching our AI agent to play DiabloDoom — a turn-based Diablo-style mod of Doom. The agent plays by looking at screenshots and deciding actions. If you have ideas for better tactics, we want to hear them!

## The Game

DiabloDoom combines Doom's first-person shooting with Diablo's loot system and turn-based combat. The agent explores levels, fights monsters in turn-based mode, picks up loot, equips gear, and tries to survive.

## Objective

Play well — explore the level, engage enemies in turn-based combat, pick up loot. Play like a competent player, not a bot grinding a specific drop.

## How the Agent Sees the Game

Each decision starts with a fresh screenshot. The agent assesses:
- **HUD**: health %, ammo, current state (EXPLORE banner vs turn-based COMBAT UI)
- **3D view**: facing a wall? Open area? Doorway? Enemy visible?
- **Loot**: is there a "press G to loot" prompt? Visible pickups on the ground?

**Decision cadence**: A fresh screenshot every ~5 seconds. The game state changes fast in combat, so stale visual input leads to bad decisions.

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

### Gear Check
- If you have no Diablo weapon equipped (vanilla pistol only): prioritize finding a Diablo weapon over fighting. Don't engage Flesh worms or tough enemies — explore, loot, equip first, then fight.

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
- Walk back to it immediately. Do NOT engage in new combat or explore for loot until you've recovered your corpse. A naked respawn fighting with no gear is how you die twice.

## Monster Guide

This game uses Freedoom sprites (not classic Doom). Use these names:

| Freedoom Name | What It Looks Like | Threat |
|---|---|---|
| **Serpentipede** | Dark insectoid alien, red head-horns, yellow eyes | Ranged — don't kite, just attack |
| **Flesh worm** | Pinkish-red fleshy blob with gaping toothed maw | Melee, tanky — don't fight up close with pistol |
| **Stealth worm** | Same as Flesh worm but translucent | Melee — same as above |
| **Zombie** | Humanoid soldier, gray-olive skin, blue visor | Weak ranged |
| **Shotgun zombie** | Dark-armored trooper, red-orange head | Ranged, dangerous up close |
| **Minigun zombie** | Heavy weapons trooper | Ranged, very dangerous |
| **Trilobite** | Giant floating armored head | Tough |
| **Hatchling** | Flaming horned skull | Fast, weak |
| **Pain bringer** | Knight-like demon | Tough melee |
| **Pain lord** | Armored dark knight | Very tough |
| **Combat slug** | Bulky cybernetic demon | Tanky ranged |
| **Necromancer** | Robed figure | Resurrects enemies — kill first! |

## General Tips

- Don't waste time on empty rooms; head toward doorways and corridors.
- If health is low (<30%) and no enemy nearby, keep exploring for health pickups.
- Equip upgrades immediately — don't save them for later.
- In combat, always end your turn after queueing actions.

---

*This guide evolves as the agent learns. Have a better strategy? Let us know on Threads @diablo.doom!*
