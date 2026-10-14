# Driver Command System

DiabloDoom on Muse VMs (`D_OnMuseVM()` — `JARVIS_FQDN` contains `metaaivm.com`) polls `/tmp/doom_cmd.txt` every tick. Write one command per line; the game executes them and deletes the file.

## Commands

### Explore Mode Only
These work only in explore mode (`T_InExplore()`). Ignored in combat.

| Command | Action |
|---------|--------|
| `WALK` | Teleport 64 units forward. Triggers `P_TouchSpecialThing` for overlapping pickups (walk-over loot). |
| `TURNL` | Turn left 45 degrees. |
| `TURNR` | Turn right 45 degrees. |

### Combat Mode Only
These work in turn-based combat (when `T_Active()`). They queue actions (1 TP each for moves, variable for attacks).

| Command | Action | TP Cost |
|---------|--------|---------|
| `ATTACK` | Queue attack on selected target. | Varies by weapon |
| `MOVE_FWD` | Step forward (facing-relative). Kiting: use to close distance. | 1 TP |
| `MOVE_BACK` | Step back (facing-relative). Kiting: back away from melee attackers. | 1 TP |
| `STRAFE_L` | Strafe left (facing-relative, no camera turn). | 1 TP |
| `STRAFE_R` | Strafe right (facing-relative, no camera turn). | 1 TP |
| `END` | End turn. Executes queued actions, then enemy phase. | — |

### Both Modes

| Command | Action |
|---------|--------|
| `COLLECT` | Auto-loot nearest Diablo loot (pathfind + pick up). |
| `SCREENSHOT` | Capture `DOOM*.png` from game's 320x200 framebuffer (no tearing). |
| `INVENTORY` | Toggle character/inventory screen (C key equivalent). |
| `RESPAWN` | Confirm respawn after death (manual corpse-run). Player stays dead at 0% until this is sent. |

## Combat Movement (Kiting)

In turn-based combat, use movement commands to kite melee attackers:

- **Guns only**: Keep distance from melee enemies. `MOVE_BACK` to create space, `ATTACK` to shoot, repeat. Never let a Demon reach melee range.
- **Fists**: Do not fight. `MOVE_BACK` / `STRAFE_*` to disengage, then explore for a weapon.
- Movement costs 1 TP per step. Queue moves + attacks, then `END` to execute.

## Examples

```
# Explore: walk forward, turn, walk
WALK
WALK
TURNL
WALK

# Combat: kite a Demon with a pistol
MOVE_BACK
ATTACK
ATTACK
END

# Combat: disengage with fists (run away)
MOVE_BACK
MOVE_BACK
END

# Death: screenshot, then respawn
SCREENSHOT
RESPAWN
```

## Implementation

- Driver polling: `src/doom/t_turn.c` — `T_Ticker()` reads `/tmp/doom_cmd.txt`.
- Explore movement: `WALK` teleports + manual `P_TouchSpecialThing` check (see `t_turn.c` WALK handler).
- Combat movement: `T_DoMove(dir)` in `src/doom/t_action.c` — queues `TA_MOVE_*` actions.
- Combat attacks: `T_DoAttack()` in `src/doom/t_action.c`.
- Loot: `T_DoCollect()` in `src/doom/t_action.c`.
- Respawn: `P_DoCorpseRespawn()` in `src/doom/p_inter.c`.
