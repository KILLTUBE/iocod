/*
===========================================================================
cl_weapon_cod1.c  --  Client-side CoD1 weapon system

Loads weapon definitions and renders the viewmodel independently of the
Q3 cgame module.  The viewmodel is drawn after cgame's scene in a
separate RE_RenderScene pass with RDF_NOWORLDMODEL + RF_DEPTHHACK.

Usage (console):
  give <weaponname>     -- equip weapon (e.g. "give colt_mp")
  dropweapon            -- holster current weapon
===========================================================================
*/
#include "client.h"
#include "../qcommon/bg_weapon_cod1.h"

/* ===========================================================================
   Cvars
   =========================================================================== */

static cvar_t *cl_gunFov;       /* viewmodel field of view (degrees) */
static cvar_t *cl_drawGun;      /* 0 = hide viewmodel */

/* ===========================================================================
   Weapon state
   =========================================================================== */

/* Animation slot indices */
#define WA_IDLE         0
#define WA_FIRE         1
#define WA_RELOAD       2
#define WA_RELOAD_EMPTY 3
#define WA_RAISE        4
#define WA_DROP         5
#define WA_MELEE        6
#define WA_ADS_FIRE     7
#define WA_ADS_UP       8
#define WA_ADS_DOWN     9
#define WA_LAST_SHOT    10
#define WA_EMPTY_IDLE   11
#define WA_COUNT        12

typedef struct {
    qboolean    active;
    char        name[64];           /* weapon name, e.g. "colt_mp" */
    weaponDef_t def;

    /* Registered renderer handles */
    qhandle_t   gunModel;           /* viewmodel (xmodel/gunModel) */
    qhandle_t   handModel;          /* hands (xmodel/handModel) */
    qhandle_t   worldModel;         /* pickup / world model */

    /* Animation handles (0 if not loaded) */
    qhandle_t   anims[WA_COUNT];
    int         currentAnim;        /* WA_* index of active anim */
    int         animStartTime;      /* cls.realtime when anim began */
} clWeapon_t;

static clWeapon_t cl_weapon;

/* ===========================================================================
   Helpers
   =========================================================================== */

/*
 * Register an xanim by name.  Returns 0 if name is empty or not found.
 * This calls back into the renderer's internal table via the render DLL.
 * For now we call the symbol directly; a proper refexport hook is a TODO.
 */
static qhandle_t LoadAnim( const char *animName )
{
    if ( !animName || !animName[0] ) return 0;
    if ( !re.RegisterXAnim ) return 0;
    return re.RegisterXAnim( animName );
}

static qhandle_t LoadXModel( const char *shortName )
{
    char path[MAX_QPATH];
    if ( !shortName || !shortName[0] ) return 0;
    /* If it already starts with "xmodel/", use as-is */
    if ( Q_strncmp( shortName, "xmodel/", 7 ) == 0 )
        Q_strncpyz( path, shortName, sizeof(path) );
    else
        Com_sprintf( path, sizeof(path), "xmodel/%s", shortName );
    return re.RegisterModel( path );
}

/* ===========================================================================
   CL_GiveWeapon
   =========================================================================== */

void CL_GiveWeapon( const char *name )
{
    weaponDef_t *d;

    if ( !name || !name[0] ) return;

    Com_Memset( &cl_weapon, 0, sizeof(cl_weapon) );
    Q_strncpyz( cl_weapon.name, name, sizeof(cl_weapon.name) );

    if ( !BG_ParseWeaponDef( name, &cl_weapon.def ) ) {
        Com_Printf( "CL_GiveWeapon: could not load weapon '%s'\n", name );
        return;
    }
    d = &cl_weapon.def;

    /* Register models */
    cl_weapon.gunModel   = LoadXModel( d->gunModel  );
    cl_weapon.handModel  = LoadXModel( d->handModel );
    cl_weapon.worldModel = d->worldModel[0]
                           ? re.RegisterModel( d->worldModel )
                           : 0;

    /* Register animations */
    cl_weapon.anims[WA_IDLE]         = LoadAnim( d->anims.idleAnim          );
    cl_weapon.anims[WA_FIRE]         = LoadAnim( d->anims.fireAnim          );
    cl_weapon.anims[WA_RELOAD]       = LoadAnim( d->anims.reloadAnim        );
    cl_weapon.anims[WA_RELOAD_EMPTY] = LoadAnim( d->anims.reloadEmptyAnim   );
    cl_weapon.anims[WA_RAISE]        = LoadAnim( d->anims.raiseAnim         );
    cl_weapon.anims[WA_DROP]         = LoadAnim( d->anims.dropAnim          );
    cl_weapon.anims[WA_MELEE]        = LoadAnim( d->anims.meleeAnim         );
    cl_weapon.anims[WA_ADS_FIRE]     = LoadAnim( d->anims.adsFireAnim       );
    cl_weapon.anims[WA_ADS_UP]       = LoadAnim( d->anims.adsUpAnim         );
    cl_weapon.anims[WA_ADS_DOWN]     = LoadAnim( d->anims.adsDownAnim       );
    cl_weapon.anims[WA_LAST_SHOT]    = LoadAnim( d->anims.lastShotAnim      );
    cl_weapon.anims[WA_EMPTY_IDLE]   = LoadAnim( d->anims.emptyIdleAnim     );

    /* Start with raise anim, fall back to idle */
    cl_weapon.currentAnim    = cl_weapon.anims[WA_RAISE] ? WA_RAISE : WA_IDLE;
    cl_weapon.animStartTime  = cls.realtime;
    cl_weapon.active         = qtrue;

    Com_Printf( "Weapon equipped: %s  (gun=%s hands=%s)\n",
                name,
                d->gunModel[0]  ? d->gunModel  : "(none)",
                d->handModel[0] ? d->handModel : "(none)" );
}

/* ===========================================================================
   Animation state machine
   =========================================================================== */

/*
 * Returns the current animation frame (fractional) for the active anim slot.
 * When the raise anim finishes it transitions to idle automatically.
 */
static float GetCurrentAnimFrame( void )
{
    qhandle_t h    = cl_weapon.anims[cl_weapon.currentAnim];
    int       fps  = 30;
    int       nf   = 1;
    float     elapsed, frame;

    if ( h && re.XAnimFramerate ) {
        fps = re.XAnimFramerate( h );
        nf  = re.XAnimNumFrames( h );
        if ( fps <= 0 ) fps = 30;
        if ( nf  <= 0 ) nf  = 1;
    }

    elapsed = (float)( cls.realtime - cl_weapon.animStartTime ) / 1000.0f;
    frame   = elapsed * (float)fps;

    /* Loop idle; finish-then-idle for one-shot anims */
    if ( cl_weapon.currentAnim == WA_IDLE ||
         cl_weapon.currentAnim == WA_EMPTY_IDLE ) {
        if ( nf > 1 ) frame = (float)fmod( (double)frame, (double)nf );
        else          frame = 0.0f;
    } else {
        if ( frame >= (float)nf ) {
            /* One-shot anim finished: switch to idle */
            cl_weapon.currentAnim   = WA_IDLE;
            cl_weapon.animStartTime = cls.realtime;
            frame = 0.0f;
        }
    }
    return frame;
}

/* ===========================================================================
   CL_DrawViewModel
   =========================================================================== */

/*
 * Add a single xmodel entity to the current scene.
 * 'renderfx' should include at least RF_DEPTHHACK | RF_FIRST_PERSON.
 */
static void AddViewmodelEnt( qhandle_t hModel, int renderfx,
                              const vec3_t origin, vec3_t axis[3] )
{
    refEntity_t ent;
    Com_Memset( &ent, 0, sizeof(ent) );
    ent.reType    = RT_MODEL;
    ent.hModel    = hModel;
    ent.renderfx  = renderfx;
    VectorCopy ( origin, ent.origin      );
    VectorCopy ( origin, ent.oldorigin   );
    AxisCopy   ( axis,   ent.axis        );
    re.AddRefEntityToScene( &ent );
}

void CL_DrawViewModel( stereoFrame_t stereo )
{
    refdef_t refdef;
    vec3_t   viewOrigin;
    vec3_t   viewAngles;
    vec3_t   axis[3];
    float    gunFov;
    int      rfx = RF_DEPTHHACK | RF_FIRST_PERSON;

    if ( !cl_drawGun || !cl_drawGun->integer ) return;
    if ( !cl_weapon.active ) return;
    if ( !cl_weapon.gunModel && !cl_weapon.handModel ) return;

    /* Need a valid snapshot for player position */
    if ( !cl.snap.valid ) return;

    /* ---- Build eye position from player state ---- */
    VectorCopy( cl.snap.ps.origin, viewOrigin );
    viewOrigin[2] += cl.snap.ps.viewheight;
    VectorCopy( cl.snap.ps.viewangles, viewAngles );
    AnglesToAxis( viewAngles, axis );

    /* ---- Set up a refdef covering the full viewport ---- */
    Com_Memset( &refdef, 0, sizeof(refdef) );
    refdef.x      = 0;
    refdef.y      = 0;
    refdef.width  = cls.glconfig.vidWidth;
    refdef.height = cls.glconfig.vidHeight;
    refdef.time   = cl.serverTime;
    /* Skip BSP / sky; only render entities */
    refdef.rdflags = RDF_NOWORLDMODEL;

    gunFov = ( cl_gunFov && cl_gunFov->value > 1.0f ) ? cl_gunFov->value : 65.0f;
    refdef.fov_x = gunFov;
    /* fov_y derived from aspect ratio */
    refdef.fov_y = 2.0f * (float)atan( tan( gunFov * 0.5f * ( M_PI / 180.0f ) )
                                        * (float)refdef.height / (float)refdef.width )
                   * ( 180.0f / (float)M_PI );

    VectorCopy( viewOrigin, refdef.vieworg );
    AxisCopy( axis, refdef.viewaxis );

    /* ---- Build fresh entity list ---- */
    re.ClearScene();

    if ( cl_weapon.handModel ) AddViewmodelEnt( cl_weapon.handModel, rfx, viewOrigin, axis );
    if ( cl_weapon.gunModel  ) AddViewmodelEnt( cl_weapon.gunModel,  rfx, viewOrigin, axis );

    re.RenderScene( &refdef );
}

/*
 * Returns the fractional animation frame for the current anim slot.
 * Used by future animated rendering (bone evaluation + vertex skinning).
 * Currently unused until runtime skinning is implemented.
 */
float CL_WeaponCurrentAnimFrame( void )
{
    return GetCurrentAnimFrame();
}

/* ===========================================================================
   Console commands
   =========================================================================== */

static void CL_Give_f( void )
{
    if ( Cmd_Argc() < 2 ) {
        Com_Printf( "Usage: give <weaponname>\n"
                    "  e.g.  give colt_mp\n"
                    "  e.g.  give 30cal\n" );
        return;
    }
    CL_GiveWeapon( Cmd_Argv(1) );
}

static void CL_DropWeapon_f( void )
{
    if ( cl_weapon.active )
        Com_Printf( "Holstered '%s'\n", cl_weapon.name );
    Com_Memset( &cl_weapon, 0, sizeof(cl_weapon) );
}

/* ===========================================================================
   Init / shutdown
   =========================================================================== */

void CL_WeaponCod1_Init( void )
{
    cl_gunFov  = Cvar_Get( "cl_gunFov",  "65",  CVAR_ARCHIVE );
    cl_drawGun = Cvar_Get( "cl_drawGun", "1",   CVAR_ARCHIVE );

    Cmd_AddCommand( "give",        CL_Give_f        );
    Cmd_AddCommand( "dropweapon",  CL_DropWeapon_f  );

    Com_Memset( &cl_weapon, 0, sizeof(cl_weapon) );
}

void CL_WeaponCod1_Shutdown( void )
{
    Cmd_RemoveCommand( "give"       );
    Cmd_RemoveCommand( "dropweapon" );
    Com_Memset( &cl_weapon, 0, sizeof(cl_weapon) );
}
