/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
//
// bg_slidemove.c -- part of bg_pmove functionality

#include "../qcommon/q_shared.h"
#include "bg_public.h"
#include "bg_local.h"

/*

input: origin, velocity, bounds, groundPlane, trace function

output: origin, velocity, impacts, stairup boolean

*/

/*
==================
PM_SlideMove

Returns qtrue if the velocity was clipped in some way
==================
*/
#ifdef STANDALONE
#define	MAX_CLIP_PLANES	8	// CoD1: 8 (Q3: 5)
#else
#define	MAX_CLIP_PLANES	5
#endif
qboolean	PM_SlideMove( qboolean gravity ) {
	int			bumpcount, numbumps;
	vec3_t		dir;
	float		d;
	int			numplanes;
	vec3_t		planes[MAX_CLIP_PLANES];
	vec3_t		primal_velocity;
	vec3_t		clipVelocity;
	int			i, j, k;
	trace_t	trace;
	vec3_t		end;
	float		time_left;
	float		into;
	vec3_t		endVelocity;
	vec3_t		endClipVelocity;
	
	numbumps = 4;

	VectorCopy (pm->ps->velocity, primal_velocity);

	if ( gravity ) {
		VectorCopy( pm->ps->velocity, endVelocity );
		endVelocity[2] -= pm->ps->gravity * pml.frametime;
		pm->ps->velocity[2] = ( pm->ps->velocity[2] + endVelocity[2] ) * 0.5;
		primal_velocity[2] = endVelocity[2];
		if ( pml.groundPlane ) {
			// slide along the ground plane
			PM_ClipVelocity (pm->ps->velocity, pml.groundTrace.plane.normal, 
				pm->ps->velocity, OVERCLIP );
		}
	}

	time_left = pml.frametime;

	// never turn against the ground plane
	if ( pml.groundPlane ) {
		numplanes = 1;
		VectorCopy( pml.groundTrace.plane.normal, planes[0] );
	} else {
		numplanes = 0;
	}

	// never turn against original velocity
	VectorNormalize2( pm->ps->velocity, planes[numplanes] );
	numplanes++;

	for ( bumpcount=0 ; bumpcount < numbumps ; bumpcount++ ) {

		// calculate position we are trying to move to
		VectorMA( pm->ps->origin, time_left, pm->ps->velocity, end );

		// see if we can make it there
		pm->trace ( &trace, pm->ps->origin, pm->mins, pm->maxs, end, pm->ps->clientNum, pm->tracemask);

		if (trace.allsolid) {
			// entity is completely trapped in another solid
			pm->ps->velocity[2] = 0;	// don't build up falling damage, but allow sideways acceleration
			return qtrue;
		}

		if (trace.fraction > 0) {
			// actually covered some distance
			VectorCopy (trace.endpos, pm->ps->origin);
		}

		if (trace.fraction == 1) {
			 break;		// moved the entire distance
		}

		// save entity for contact
		PM_AddTouchEnt( trace.entityNum );

		time_left -= time_left * trace.fraction;

		if (numplanes >= MAX_CLIP_PLANES) {
			// this shouldn't really happen
			VectorClear( pm->ps->velocity );
			return qtrue;
		}

		//
		// if this is the same plane we hit before, nudge velocity
		// out along it, which fixes some epsilon issues with
		// non-axial planes
		//
		for ( i = 0 ; i < numplanes ; i++ ) {
	#ifdef STANDALONE
		if ( DotProduct( trace.plane.normal, planes[i] ) > 0.999 ) {	// CoD1: 0.999 (Q3: 0.99)
#else
		if ( DotProduct( trace.plane.normal, planes[i] ) > 0.99 ) {
#endif
				VectorAdd( trace.plane.normal, pm->ps->velocity, pm->ps->velocity );
				break;
			}
		}
		if ( i < numplanes ) {
			continue;
		}
		VectorCopy (trace.plane.normal, planes[numplanes]);
		numplanes++;

		//
		// modify velocity so it parallels all of the clip planes
		//

		// find a plane that it enters
		for ( i = 0 ; i < numplanes ; i++ ) {
			into = DotProduct( pm->ps->velocity, planes[i] );
			if ( into >= 0.1 ) {
				continue;		// move doesn't interact with the plane
			}

			// see how hard we are hitting things
			if ( -into > pml.impactSpeed ) {
				pml.impactSpeed = -into;
			}

			// slide along the plane
			PM_ClipVelocity (pm->ps->velocity, planes[i], clipVelocity, OVERCLIP );

			if ( gravity ) {
				// slide along the plane
				PM_ClipVelocity (endVelocity, planes[i], endClipVelocity, OVERCLIP );
			}

			// see if there is a second plane that the new move enters
			for ( j = 0 ; j < numplanes ; j++ ) {
				if ( j == i ) {
					continue;
				}
				if ( DotProduct( clipVelocity, planes[j] ) >= 0.1 ) {
					continue;		// move doesn't interact with the plane
				}

				// try clipping the move to the plane
				PM_ClipVelocity( clipVelocity, planes[j], clipVelocity, OVERCLIP );

				if ( gravity ) {
					PM_ClipVelocity( endClipVelocity, planes[j], endClipVelocity, OVERCLIP );
				}

				// see if it goes back into the first clip plane
				if ( DotProduct( clipVelocity, planes[i] ) >= 0 ) {
					continue;
				}

				// slide the original velocity along the crease
				CrossProduct (planes[i], planes[j], dir);
				VectorNormalize( dir );
				d = DotProduct( dir, pm->ps->velocity );
				VectorScale( dir, d, clipVelocity );

				if ( gravity ) {
					CrossProduct (planes[i], planes[j], dir);
					VectorNormalize( dir );
					d = DotProduct( dir, endVelocity );
					VectorScale( dir, d, endClipVelocity );
				}

				// see if there is a third plane the the new move enters
				for ( k = 0 ; k < numplanes ; k++ ) {
					if ( k == i || k == j ) {
						continue;
					}
					if ( DotProduct( clipVelocity, planes[k] ) >= 0.1 ) {
						continue;		// move doesn't interact with the plane
					}

					// stop dead at a tripple plane interaction
					VectorClear( pm->ps->velocity );
					return qtrue;
				}
			}

			// if we have fixed all interactions, try another move
			VectorCopy( clipVelocity, pm->ps->velocity );

			if ( gravity ) {
				VectorCopy( endClipVelocity, endVelocity );
			}

			break;
		}
	}

	if ( gravity ) {
		VectorCopy( endVelocity, pm->ps->velocity );
	}

	// don't change velocity if in a timer (FIXME: is this correct?)
	if ( pm->ps->pm_time ) {
		VectorCopy( primal_velocity, pm->ps->velocity );
	}

	return ( bumpcount != 0 );
}

/*
==================
PM_StepSlideMove

==================
*/
#ifdef STANDALONE
/*
CoD1-style PM_StepSlideMove: allows step-up during jumps by tracking the
expected jump peak (ps->jumpOriginZ).  When stepping up mid-jump the step
height is clamped so the player never rises above the peak, and the upward
velocity is recalculated to preserve the correct arc.
*/
void PM_StepSlideMove( qboolean gravity ) {
	vec3_t		start_o, start_v;
	vec3_t		slide_o, slide_v;		// result of initial SlideMove
	trace_t		trace;
	vec3_t		up, down;
	float		stepSize;
	float		stepHeight;				// actual height gained from step-up trace
	qboolean	isJumpStep;				// stepping while in a jump arc
	qboolean	doProne;				// prone down-step behaviour
	float		slideXYdot, stepXYdot;	// for "did step help?" comparison

	VectorCopy( pm->ps->origin, start_o );
	VectorCopy( pm->ps->velocity, start_v );

	// determine step size: 18 normally, 10 when prone
	stepSize = STEPSIZE;
#ifdef STEPSIZE_PRONE
	if ( pm->ps->pm_flags & PMF_PRONE )
		stepSize = STEPSIZE_PRONE;
#endif
	doProne = ( pml.walking && !(pm->ps->pm_flags & PMF_JUMP_HELD) );

	if ( PM_SlideMove( gravity ) == 0 ) {
		// moved without hitting anything — but still check for down-step
		// if on ground, so the player sticks to slopes/stairs going down
		if ( pm->ps->groundEntityNum == ENTITYNUM_NONE ) {
			// in air with no collision → check if we should still do ground snap
			if ( !(pm->ps->pm_flags & PMF_JUMP_HELD) && pm->ps->velocity[2] > 0 ) {
				// walked off an edge with upward velocity — allow step logic to continue
			} else {
				return;
			}
		}
		// on ground, no collision — fall through to step-down logic below
	}

	// save the SlideMove result
	VectorCopy( pm->ps->origin, slide_o );
	VectorCopy( pm->ps->velocity, slide_v );

	isJumpStep = qfalse;

	// --- decide whether to attempt step-up ---
	if ( pm->ps->jumpOriginZ > 0.001f ) {
		// player is in a jump arc — clamp step size to remaining room below jump peak
		if ( pm->ps->groundEntityNum != ENTITYNUM_NONE ) {
			// landed on something — normal step
		} else {
			float room = pm->ps->jumpOriginZ - start_o[2];
			if ( room < 1.0f )
				goto downstep;	// already at/above jump peak, don't step up

			if ( stepSize > room )
				stepSize = room;

			isJumpStep = qtrue;
		}
	} else if ( pm->ps->jumpOriginZ < -0.001f ) {
		// shouldn't happen, but be safe
		goto downstep;
	} else {
		// jumpOriginZ == 0: not in a jump
		// Q3-compatible check: don't step up when flying upward with no ground
		if ( pm->ps->groundEntityNum == ENTITYNUM_NONE
			&& !(pm->ps->pm_flags & PMF_JUMP_HELD)
			&& pm->ps->velocity[2] > 0 ) {
			// walked off an edge going up — allow step (CoD1 does)
		} else if ( pm->ps->groundEntityNum == ENTITYNUM_NONE ) {
			goto downstep;
		}
	}

	// --- step-up: trace upward from start ---
	VectorCopy( start_o, up );
	up[2] += stepSize + 1.0f;	// CoD1 traces stepSize+1, then subtracts 1

	pm->trace( &trace, start_o, pm->mins, pm->maxs, up, pm->ps->clientNum, pm->tracemask );
	stepHeight = (stepSize + 1.0f) * trace.fraction - 1.0f;

	if ( stepHeight < 1.0f ) {
		if ( pm->debugLevel )
			Com_Printf( "%i:not enough step room\n", c_pmove );
		goto downstep;
	}

	// place player at stepped-up position, restore original velocity, re-slide
	VectorCopy( start_o, pm->ps->origin );
	pm->ps->origin[2] += stepHeight;
	VectorCopy( start_v, pm->ps->velocity );

	if ( pm->debugLevel && isJumpStep )
		Com_Printf( "%i:jump step to:%.2f jump peak at:%.2f\n",
			c_pmove, pm->ps->origin[2], pm->ps->jumpOriginZ );

	PM_SlideMove( gravity );

	// --- step-down: push back down to ground ---
	if ( doProne || stepHeight > 0 ) {
		VectorCopy( pm->ps->origin, down );
		if ( doProne )
			down[2] = pm->ps->origin[2] - stepHeight - stepSize * 0.5f;
		else
			down[2] = pm->ps->origin[2] - stepHeight;

		pm->trace( &trace, pm->ps->origin, pm->mins, pm->maxs, down,
			pm->ps->clientNum, pm->tracemask );

		if ( trace.fraction < 0.0625f ) {
			// trace hit almost immediately — step result is worse, revert
			VectorCopy( slide_o, pm->ps->origin );
			VectorCopy( slide_v, pm->ps->velocity );
			goto jumpfix;
		}

		if ( trace.fraction < 1.0f ) {
			VectorCopy( trace.endpos, pm->ps->origin );
			PM_ClipVelocity( pm->ps->velocity, trace.plane.normal,
				pm->ps->velocity, OVERCLIP );
		} else if ( stepHeight > 0 ) {
			// didn't find ground below — undo step height
			pm->ps->origin[2] -= stepHeight;
		}
	}

	// --- verify step result is an improvement ---
	// compare XY progress: dot(newMove, slideMove) with original velocity direction
	{
		float sx = slide_o[2] - start_o[2];	// just used as temp
		(void)sx;
		// XY dot of stepped displacement with original velocity
		stepXYdot = (pm->ps->origin[0] - start_o[0]) * start_v[0]
				  + (pm->ps->origin[1] - start_o[1]) * start_v[1];
		slideXYdot = (slide_o[0] - start_o[0]) * start_v[0]
				   + (slide_o[1] - start_o[1]) * start_v[1];
	}

	if ( stepXYdot + 0.001f < slideXYdot ) {
		// step result went backwards — revert
		VectorCopy( slide_o, pm->ps->origin );
		VectorCopy( slide_v, pm->ps->velocity );
		if ( pm->debugLevel ) {
			if ( isJumpStep )
				Com_Printf( "%i:didn't use jump step results because it went too high\n", c_pmove );
			else
				Com_Printf( "%i:didn't use step results\n", c_pmove );
		}
		// even though we reverted, still try prone down-step
		if ( doProne ) {
			VectorCopy( pm->ps->origin, down );
			down[2] -= stepSize * 0.5f;
			pm->trace( &trace, pm->ps->origin, pm->mins, pm->maxs, down,
				pm->ps->clientNum, pm->tracemask );
			if ( trace.fraction < 1.0f ) {
				VectorCopy( trace.endpos, pm->ps->origin );
				PM_ClipVelocity( pm->ps->velocity, trace.plane.normal,
					pm->ps->velocity, OVERCLIP );
				if ( pm->debugLevel )
					Com_Printf( "%i:did down step after not using step results\n", c_pmove );
			}
		}
	}

jumpfix:
	// --- CoD1 jump velocity adjustment ---
	// after stepping up during a jump, recalculate velocity[2] so the player
	// still reaches the same peak height (not higher from the step-up boost)
	if ( isJumpStep && pm->ps->origin[2] > start_o[2] ) {
		float remaining = pm->ps->jumpOriginZ - pm->ps->origin[2];
		if ( remaining < 0.1f ) {
			pm->ps->velocity[2] = 0;
		} else {
			float newVel = (float)sqrt( 2.0f * remaining * (float)pm->ps->gravity );
			if ( newVel < pm->ps->velocity[2] ) {
				if ( pm->debugLevel )
					Com_Printf( "%i:adjusted jump vel: %.1f -> %.1f\n",
						c_pmove, pm->ps->velocity[2], newVel );
				pm->ps->velocity[2] = newVel;
			}
		}
	}

	// step event
	{
		float delta = pm->ps->origin[2] - start_o[2];
		if ( delta > 2 ) {
			if ( delta < 7 )
				PM_AddEvent( EV_STEP_4 );
			else if ( delta < 11 )
				PM_AddEvent( EV_STEP_8 );
			else if ( delta < 15 )
				PM_AddEvent( EV_STEP_12 );
			else
				PM_AddEvent( EV_STEP_16 );
		}
		if ( pm->debugLevel && delta > 0 )
			Com_Printf( "%i:jump step %2i\n", c_pmove, (int)delta );
	}
	return;

downstep:
	// no step-up was performed — just use the slide result and check for
	// down-step if the player is walking (keeps them glued to slopes)
	VectorCopy( slide_o, pm->ps->origin );
	VectorCopy( slide_v, pm->ps->velocity );

	if ( pm->ps->groundEntityNum != ENTITYNUM_NONE ) {
		VectorCopy( pm->ps->origin, down );
		down[2] -= stepSize;
		pm->trace( &trace, pm->ps->origin, pm->mins, pm->maxs, down,
			pm->ps->clientNum, pm->tracemask );
		if ( trace.fraction < 1.0f && trace.plane.normal[2] >= MIN_WALK_NORMAL ) {
			VectorCopy( trace.endpos, pm->ps->origin );
			PM_ClipVelocity( pm->ps->velocity, trace.plane.normal,
				pm->ps->velocity, OVERCLIP );
		}
	}
}

#else /* !STANDALONE — original Q3 PM_StepSlideMove */

void PM_StepSlideMove( qboolean gravity ) {
	vec3_t		start_o, start_v;
	trace_t		trace;
	vec3_t		up, down;
	float		stepSize;

	VectorCopy (pm->ps->origin, start_o);
	VectorCopy (pm->ps->velocity, start_v);

	if ( PM_SlideMove( gravity ) == 0 ) {
		return;		// we got exactly where we wanted to go first try
	}

	VectorCopy(start_o, down);
	down[2] -= STEPSIZE;
	pm->trace (&trace, start_o, pm->mins, pm->maxs, down, pm->ps->clientNum, pm->tracemask);
	VectorSet(up, 0, 0, 1);
	// never step up when you still have up velocity
	if ( pm->ps->velocity[2] > 0 && (trace.fraction == 1.0 ||
										DotProduct(trace.plane.normal, up) < 0.7)) {
		return;
	}

	VectorCopy (start_o, up);
	up[2] += STEPSIZE;

	// test the player position if they were a stepheight higher
	pm->trace (&trace, start_o, pm->mins, pm->maxs, up, pm->ps->clientNum, pm->tracemask);
	if ( trace.allsolid ) {
		if ( pm->debugLevel ) {
			Com_Printf("%i:bend can't step\n", c_pmove);
		}
		return;		// can't step up
	}

	stepSize = trace.endpos[2] - start_o[2];
	// try slidemove from this position
	VectorCopy (trace.endpos, pm->ps->origin);
	VectorCopy (start_v, pm->ps->velocity);

	PM_SlideMove( gravity );

	// push down the final amount
	VectorCopy (pm->ps->origin, down);
	down[2] -= stepSize;
	pm->trace (&trace, pm->ps->origin, pm->mins, pm->maxs, down, pm->ps->clientNum, pm->tracemask);
	if ( !trace.allsolid ) {
		VectorCopy (trace.endpos, pm->ps->origin);
	}
	if ( trace.fraction < 1.0 ) {
		PM_ClipVelocity( pm->ps->velocity, trace.plane.normal, pm->ps->velocity, OVERCLIP );
	}

	{
		// use the step move
		float	delta;

		delta = pm->ps->origin[2] - start_o[2];
		if ( delta > 2 ) {
			if ( delta < 7 ) {
				PM_AddEvent( EV_STEP_4 );
			} else if ( delta < 11 ) {
				PM_AddEvent( EV_STEP_8 );
			} else if ( delta < 15 ) {
				PM_AddEvent( EV_STEP_12 );
			} else {
				PM_AddEvent( EV_STEP_16 );
			}
		}
		if ( pm->debugLevel ) {
			Com_Printf("%i:stepped\n", c_pmove);
		}
	}
}
#endif /* STANDALONE */

