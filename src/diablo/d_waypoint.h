//
// d_waypoint.h — the WAYPOINT marker (2026-11-18): one point on the map,
// set by the driver's WAYPOINT command (the VLM in production, the
// training env in training), drawn in the scene as a see-through pillar
// that shows through walls, until the player comes within reach of it.
//
// Part of the DiabloDoom mod (GPL-2.0-or-later, like Chocolate Doom).

#ifndef __D_WAYPOINT__
#define __D_WAYPOINT__

#include "doomtype.h"

void D_WaypointSet(int x, int y);       // map units
void D_WaypointClear(void);
boolean D_WaypointGet(int *x, int *y);  // false when none is set

// Every tic: clears the waypoint once the player is within reach, with a
// "waypoint_reached" event.
void D_WaypointTick(void);

// From HU_Drawer: the pillar.
void D_WaypointDraw(void);

#endif
