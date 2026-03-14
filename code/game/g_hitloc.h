/*
===========================================================================
g_hitloc.h — CoD1 joint-based hit location damage system

19 hit locations (0–18) mapped to body joints. Damage multipliers are
loaded from info/mp_lochit_dmgtable inside the pak files.

Format: LOCDMGTABLE\name\mult\name\mult\...
===========================================================================
*/
#pragma once

#ifdef STANDALONE

typedef enum {
	HITLOC_NONE,
	HITLOC_HELMET,
	HITLOC_HEAD,
	HITLOC_NECK,
	HITLOC_TORSO_UPPER,
	HITLOC_TORSO_LOWER,
	HITLOC_RIGHT_ARM_UPPER,
	HITLOC_LEFT_ARM_UPPER,
	HITLOC_RIGHT_ARM_LOWER,
	HITLOC_LEFT_ARM_LOWER,
	HITLOC_RIGHT_HAND,
	HITLOC_LEFT_HAND,
	HITLOC_RIGHT_LEG_UPPER,
	HITLOC_LEFT_LEG_UPPER,
	HITLOC_RIGHT_LEG_LOWER,
	HITLOC_LEFT_LEG_LOWER,
	HITLOC_RIGHT_FOOT,
	HITLOC_LEFT_FOOT,
	HITLOC_GUN,			/* hitting the held weapon — 0 damage */

	HITLOC_NUM
} hitLocation_t;

extern float	g_fHitLocDamageMult[HITLOC_NUM];

void			G_ParseHitLocDmgTable( void );
const char		*G_GetHitLocationString( int index );
int				G_GetHitLocationIndexFromString( const char *name );
int				G_CalcHitLocFromPoint( vec3_t point, gentity_t *target );

#else /* !STANDALONE */

#define G_ParseHitLocDmgTable()        ((void)0)

#endif /* STANDALONE */
