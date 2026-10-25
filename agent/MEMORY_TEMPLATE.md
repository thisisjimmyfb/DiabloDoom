# DiabloDoom Farm — Area Memory (short-term scratch)

Level: <level, e.g. E1M1>

LEVEL GEOMETRY ONLY — what the farm agent has SEEN of this level's
layout: areas, doors, steps, dead ends, blocked routes. Not loot,
not corpses, not enemies (transient; loot is taken greedily in the
moment). Observation only — screenshots and log stamps, never map
data. Coordinates from log stamps are exact; positions judged from
a screenshot are rough and prefixed with `~`. Verify by vision
before acting: a door that wouldn't open from one side may open
from the other.

Last updated: YYYY-MM-DD ~HH:MM PDT
Save anchor (QUIT save, latest cycle): (x,y) <area>, <level>, <HP>% HP, <kit>. No corpse pending. / Corpse pending at (x,y).

## Goals
Level goal: reach the exit.
Current: <area/room> | status: current | pass: <n> | NEXT: <next area> | ADOPTED: YYYY-MM-DD ~HH:MM PDT
Done-for-now:
- <area/room> | status: done-for-now | pass: <n> | NEXT: <next area> | ADOPTED: YYYY-MM-DD ~HH:MM PDT

One line per area goal. ADOPTED is the adoption time (not the line-write time), stamped at adoption — including re-adoption for a finer pass, with the pass number bumped — so time tried is now minus ADOPTED. QUIT ends a session, not a goal: goals persist across cycles. They do NOT persist across a level: on a level change this block resets to empty (see SYSTEM_PROMPT, Area Memory).
Bootstrap: if this block is empty, adopt your current area as the current goal at LOAD 0, pass 1, ADOPTED = now.

## Open leads
Top first. Uncrossed doors (didn't-open, keyed), mechanisms, and wall sections to work. A door you crossed is geometry, not a lead: it stays recorded with where it leads, but it leaves this list.

Cycle notes (one section per cycle, newest first):
## Cycle notes (YYYY-MM-DD ~HH:MM) — <one-line summary> (hourly farm)
- Trail: ...
- Findings: doors tested, hazards mapped, ground covered.
- Staged: ...
- Next cycle: from (x,y) <area>. Live leads: ...

Keep the file under ~60 lines: before QUIT, consolidate — fold the run's edits in, drop entries events have overtaken, keep dead-ends and no-effect spots.
Coordinate confidence: bare = log stamp (exact); ≈(x,y) (calc) = triangulated/camera-math; ~ = eyeballed from a screenshot. An arrival stamp replaces a ≈/~ entry with the bare coordinate.
