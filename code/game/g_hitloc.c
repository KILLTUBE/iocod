/*
===========================================================================
g_hitloc.c — CoD1 joint-based hit location damage system

Parses info/mp_lochit_dmgtable from the pak files to populate the
g_fHitLocDamageMult[] array.  The table format is:

    LOCDMGTABLE\name\multiplier\name\multiplier\...

CoD1 default multipliers (from pak5.pk3):
    head/helmet/neck  1.5
    torso_upper       0.9
    torso_lower       0.8
    arms/legs upper   0.6
    arms/legs lower   0.5
    hands/feet        0.4
    gun               0.0
===========================================================================
*/

#ifdef STANDALONE

#include "g_local.h"
#include "g_hitloc.h"

/* -----------------------------------------------------------------------
   Hit location name table — order must match hitLocation_t enum.
   ----------------------------------------------------------------------- */
static const char *hitLocNames[HITLOC_NUM] = {
	"none",
	"helmet",
	"head",
	"neck",
	"torso_upper",
	"torso_lower",
	"right_arm_upper",
	"left_arm_upper",
	"right_arm_lower",
	"left_arm_lower",
	"right_hand",
	"left_hand",
	"right_leg_upper",
	"left_leg_upper",
	"right_leg_lower",
	"left_leg_lower",
	"right_foot",
	"left_foot",
	"gun"
};

/* Damage multiplier per hit location — defaults to 1.0 for all, 0.0 for gun */
float g_fHitLocDamageMult[HITLOC_NUM];

/* -----------------------------------------------------------------------
   G_ParseHitLocDmgTable — load info/mp_lochit_dmgtable
   ----------------------------------------------------------------------- */
void G_ParseHitLocDmgTable( void )
{
	fileHandle_t	f;
	int				len;
	char			buf[8192];
	char			*p, *token;
	int				i;

	/* Set defaults: 1.0 for all, 0.0 for gun */
	for ( i = 0; i < HITLOC_NUM; i++ ) {
		g_fHitLocDamageMult[i] = 1.0f;
	}
	g_fHitLocDamageMult[HITLOC_GUN] = 0.0f;

	len = trap_FS_FOpenFile( "info/mp_lochit_dmgtable", &f, FS_READ );
	if ( len <= 0 ) {
		G_Printf( "G_ParseHitLocDmgTable: file not found, using defaults\n" );
		return;
	}
	if ( len >= (int)sizeof(buf) - 1 ) {
		G_Printf( "G_ParseHitLocDmgTable: file too large (%d bytes)\n", len );
		trap_FS_FCloseFile( f );
		return;
	}

	trap_FS_Read( buf, len, f );
	buf[len] = '\0';
	trap_FS_FCloseFile( f );

	/* Validate header */
	p = buf;
	if ( Q_stricmpn( p, "LOCDMGTABLE", 11 ) != 0 ) {
		G_Printf( "G_ParseHitLocDmgTable: invalid header\n" );
		return;
	}
	p += 11;

	/* Parse \name\value pairs */
	while ( *p == '\\' ) {
		p++;
		/* read name */
		token = p;
		while ( *p && *p != '\\' ) p++;
		if ( *p == '\\' ) *p++ = '\0';

		/* find hit location index */
		i = G_GetHitLocationIndexFromString( token );

		/* read value */
		token = p;
		while ( *p && *p != '\\' ) p++;
		if ( *p == '\\' ) *p++ = '\0';

		if ( i >= 0 && i < HITLOC_NUM ) {
			g_fHitLocDamageMult[i] = atof( token );
		}
	}

	G_Printf( "G_ParseHitLocDmgTable: loaded %d hit locations\n", HITLOC_NUM );
	for ( i = 0; i < HITLOC_NUM; i++ ) {
		G_Printf( "  %s: %.2f\n", hitLocNames[i], g_fHitLocDamageMult[i] );
	}
}

/* -----------------------------------------------------------------------
   G_GetHitLocationString
   ----------------------------------------------------------------------- */
const char *G_GetHitLocationString( int index )
{
	if ( index < 0 || index >= HITLOC_NUM ) {
		return "none";
	}
	return hitLocNames[index];
}

/* -----------------------------------------------------------------------
   G_GetHitLocationIndexFromString
   ----------------------------------------------------------------------- */
int G_GetHitLocationIndexFromString( const char *name )
{
	int i;
	for ( i = 0; i < HITLOC_NUM; i++ ) {
		if ( !Q_stricmp( name, hitLocNames[i] ) ) {
			return i;
		}
	}
	return HITLOC_NONE;
}

/* -----------------------------------------------------------------------
   G_CalcHitLocFromPoint — approximate hit location from impact point
   relative to the target entity.

   CoD1 uses trap_LocationalTrace for bone-level hit testing, which
   requires the engine to do ray-skeleton intersection. Until that's
   implemented, we approximate by comparing the impact Z height against
   the target's bounding box to determine body region.
   ----------------------------------------------------------------------- */
int G_CalcHitLocFromPoint( vec3_t point, gentity_t *target )
{
	float	targetZ, hitZ, frac;
	float	dx, dy;

	if ( !target || !point ) {
		return HITLOC_NONE;
	}

	targetZ = target->r.currentOrigin[2];
	hitZ = point[2] - targetZ;

	/* Approximate standing player height = 72 units (CoD1 standing bbox) */
	frac = hitZ / 72.0f;
	if ( frac < 0.0f ) frac = 0.0f;
	if ( frac > 1.0f ) frac = 1.0f;

	/* Determine left/right from impact relative to facing */
	dx = point[0] - target->r.currentOrigin[0];
	dy = point[1] - target->r.currentOrigin[1];

	if ( frac > 0.85f ) {
		return HITLOC_HEAD;			/* head region */
	} else if ( frac > 0.75f ) {
		return HITLOC_NECK;
	} else if ( frac > 0.55f ) {
		return HITLOC_TORSO_UPPER;
	} else if ( frac > 0.40f ) {
		return HITLOC_TORSO_LOWER;
	} else if ( frac > 0.20f ) {
		return HITLOC_RIGHT_LEG_UPPER;	/* approximate */
	} else if ( frac > 0.05f ) {
		return HITLOC_RIGHT_LEG_LOWER;
	} else {
		return HITLOC_RIGHT_FOOT;
	}
}

#endif /* STANDALONE */
