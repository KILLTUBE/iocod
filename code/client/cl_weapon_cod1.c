/*
===========================================================================
cl_weapon_cod1.c  --  Client-side CoD1 weapon system

Auto-equips a weapon on player spawn and draws the viewmodel after the
cgame scene in a second RE_RenderScene pass (RDF_NOWORLDMODEL).

Console commands:
  give <name>      e.g. "give mp44_mp"  or  "give colt_mp"
  dropweapon       holster current weapon

Cvars:
  cl_startWeapon   weapon to auto-equip on spawn  (default "mp44_mp")
  cl_gunFov        viewmodel FOV in degrees        (default 65)
  cl_drawGun       0 = hide viewmodel              (default 1)
===========================================================================
*/
#include "client.h"
#include "../qcommon/bg_weapon_cod1.h"

/* ===========================================================================
   Cvars
   =========================================================================== */

static cvar_t *cl_startWeapon;  /* weapon name to auto-equip on spawn   */
static cvar_t *cl_gunFov;       /* viewmodel field of view (degrees)    */
static cvar_t *cl_drawGun;      /* 0 = hide viewmodel                   */
static cvar_t *cl_animDebug;    /* print current anim frame each frame  */
/* Manual position fine-tune (in view-space units, same convention as CoD cg_gun_x/y/z) */
static cvar_t *cl_gunX;         /* right (+) / left (-)                 */
static cvar_t *cl_gunY;         /* forward (+) / back (-)               */
static cvar_t *cl_gunZ;         /* up (+) / down (-)                    */

/* ===========================================================================
   Weapon state
   =========================================================================== */

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
    char        name[64];
    weaponDef_t def;

    qhandle_t   gunModel;
    qhandle_t   handModel;
    qhandle_t   worldModel;

    qhandle_t   anims[WA_COUNT];
    int         currentAnim;
    int         animStartTime;
} clWeapon_t;

static clWeapon_t cl_weapon;

/* Track player state to detect spawns */
static int  s_prevPmType  = -1;  /* -1 = never set */
static int  s_spawnSerial =  0;  /* incremented on each spawn */


/* ===========================================================================
   Helpers
   =========================================================================== */

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
    if ( Q_strncmp( shortName, "xmodel/", 7 ) == 0 )
        Q_strncpyz( path, shortName, sizeof(path) );
    else
        Com_sprintf( path, sizeof(path), "xmodel/%s", shortName );
    return re.RegisterModel( path );
}

/* ===========================================================================
   CL_GiveWeapon
   ===========================================================================
   Tries  weapons/sp/<name>  then  weapons/mp/<name>.
   If name has no  _mp  suffix, also tries  <name>_mp  automatically.
   =========================================================================== */

void CL_GiveWeapon( const char *name )
{
    weaponDef_t *d;
    char         tryName[64];

    if ( !name || !name[0] ) return;
    if ( !re.RegisterModel ) {
        Com_Printf( "give: renderer not ready\n" );
        return;
    }

    Com_Memset( &cl_weapon, 0, sizeof(cl_weapon) );
    Q_strncpyz( cl_weapon.name, name, sizeof(cl_weapon.name) );

    /* Try the name as-is */
    if ( !BG_ParseWeaponDef( name, &cl_weapon.def ) ) {
        /* Try appending _mp if not already there */
        if ( !strstr( name, "_mp" ) && !strstr( name, "_sp" ) ) {
            Com_sprintf( tryName, sizeof(tryName), "%s_mp", name );
            if ( !BG_ParseWeaponDef( tryName, &cl_weapon.def ) ) {
                Com_Printf( "give: weapon '%s' not found "
                            "(tried %s and %s)\n", name, name, tryName );
                return;
            }
            Q_strncpyz( cl_weapon.name, tryName, sizeof(cl_weapon.name) );
        } else {
            Com_Printf( "give: weapon '%s' not found\n", name );
            return;
        }
    }
    d = &cl_weapon.def;

    /* Register models */
    cl_weapon.gunModel   = LoadXModel( d->gunModel  );
    cl_weapon.handModel  = LoadXModel( d->handModel );
    if ( d->worldModel[0] )
        cl_weapon.worldModel = re.RegisterModel( d->worldModel );

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

    /* Debug: print loaded animations */
    Com_Printf( "  Animations: idle=%s fire=%s\n",
                d->anims.idleAnim, d->anims.fireAnim );

    cl_weapon.currentAnim   = cl_weapon.anims[WA_RAISE] ? WA_RAISE : WA_IDLE;
    cl_weapon.animStartTime = cls.realtime;
    cl_weapon.active        = qtrue;

    Com_Printf( "Weapon: %s  gun=%s hands=%s  "
                "(models: gun=%s hands=%s)\n",
                cl_weapon.name,
                d->gunModel[0]  ? d->gunModel  : "(none)",
                d->handModel[0] ? d->handModel : "(none)",
                cl_weapon.gunModel  ? "OK" : "MISSING",
                cl_weapon.handModel ? "OK" : "MISSING" );
}

/* ===========================================================================
   Spawn detection
   =========================================================================== */

/*
 * Called every client frame.  Detects player-alive transitions and
 * auto-equips cl_startWeapon when the player spawns or respawns.
 */
static void CL_WeaponThink( void )
{
    int curPmType;
    const char *startWeap;

    if ( !cl.snap.valid ) {
        s_prevPmType = -1;
        return;
    }

    curPmType = cl.snap.ps.pm_type;

    /* PM_NORMAL == 0 → player is alive */
    if ( curPmType == 0 && s_prevPmType != 0 ) {
        /* Just spawned / respawned */
        s_spawnSerial++;
        startWeap = ( cl_startWeapon && cl_startWeapon->string[0] )
                    ? cl_startWeapon->string : "";
        if ( startWeap[0] ) {
            Com_Printf( "Auto-equipping '%s' on spawn...\n", startWeap );
            CL_GiveWeapon( startWeap );
        }
    }

    /* Handle reload button */
    if ( ( cl.cmds[ (cl.cmdNumber - 1) & (CMD_BACKUP - 1) ].buttons & BUTTON_RELOAD ) && cl_weapon.active ) {
        if ( cl_weapon.currentAnim == WA_IDLE || cl_weapon.currentAnim == WA_EMPTY_IDLE ) {
            if ( cl_weapon.anims[WA_RELOAD] ) {
                cl_weapon.currentAnim   = WA_RELOAD;
                cl_weapon.animStartTime = cls.realtime;
            }
        }
    }

    s_prevPmType = curPmType;
}

/* ===========================================================================
   Viewmodel rendering
   =========================================================================== */

/* Forward declaration — defined later in this file */
float CL_WeaponCurrentAnimFrame( void );

static void AddViewmodelEnt( qhandle_t hModel,
                              const vec3_t origin, vec3_t axis[3] )
{
    refEntity_t ent;
    Com_Memset( &ent, 0, sizeof(ent) );
    ent.reType   = RT_MODEL;
    ent.hModel   = hModel;
    ent.renderfx = RF_DEPTHHACK;   /* squish depth so gun stays in front */
    VectorCopy( origin, ent.origin    );
    VectorCopy( origin, ent.oldorigin );
    /*
     * The bone rotations in CoD1 xmodelparts already convert the mesh
     * from bone-local space into Quake/root-bone space where
     * +X = forward, +Y = left, +Z = up — same as ioq3 entity convention.
     * So a straight AxisCopy of the view axis is correct.
     */
    AxisCopy( axis, ent.axis );
    re.AddRefEntityToScene( &ent );
}

void CL_DrawViewModel( stereoFrame_t stereo )
{
    refdef_t      refdef;
    vec3_t        viewAngles;
    vec3_t        axis[3];
    float         gunFov;
    float         animFrame;
    qhandle_t     curAnim;

    /* Run per-frame logic (spawn detection, anim state machine) */
    CL_WeaponThink();

    if ( !cl_drawGun || !cl_drawGun->integer ) return;
    if ( !cl_weapon.active ) return;
    if ( !cl_weapon.gunModel && !cl_weapon.handModel ) return;
    if ( !cl.snap.valid ) return;
    /* Only draw when player is alive (PM_NORMAL == 0) */
    if ( cl.snap.ps.pm_type != 0 ) return;

    /* Compute current animation frame */
    animFrame = CL_WeaponCurrentAnimFrame();
    curAnim   = cl_weapon.anims[ cl_weapon.currentAnim ];

    /* DObj-style combined pose evaluation.
     *
     * Both hand and gun bones are evaluated together in one skeleton:
     *   combined[0..N]   = hand model bones
     *   combined[N..N+M] = gun model bones, gun root parented to tag_weapon
     *
     * The animation drives all bones by name-matching across both models.
     * Gun vertices end up in correct world-space positions relative to the
     * shared entity origin (tag_weapon offset is baked in by the skeleton).
     *
     * IMPORTANT: both entities MUST be rendered at the SAME origin/axis. */
    if ( re.UpdateDObjPose && ( cl_weapon.handModel || cl_weapon.gunModel ) ) {
        if ( !curAnim && cl_animDebug && cl_animDebug->integer )
            Com_Printf( "WARNING: No animation loaded for viewmodel!\n" );
        re.UpdateDObjPose( cl_weapon.handModel, cl_weapon.gunModel,
                            curAnim, animFrame );
    }

    /* Debug output */
    if ( cl_animDebug && cl_animDebug->integer )
        Com_Printf( "anim=%d frame=%.1f\n", cl_weapon.currentAnim, animFrame );

    /* ---- Camera / view setup ---- */
    vec3_t cameraOrigin, modelOrigin;
    vec3_t entityAxis[3];
    int    stance;
    float  ofsF = 0.0f, ofsR = 0.0f, ofsU = 0.0f;

    VectorCopy( cl.snap.ps.viewangles, viewAngles );
    AnglesToAxis( viewAngles, axis );

    /* Player eye position */
    VectorCopy( cl.snap.ps.origin, cameraOrigin );
    cameraOrigin[2] += cl.snap.ps.viewheight;

    /* PMF_DUCKED = 1 (bg_public.h) - same in CoD1 */
    stance = cl.snap.ps.pm_flags & PMF_DUCKED;

    /* Apply weapon file offsets */
    if ( cl_weapon.active ) {
        weaponDef_t *def = &cl_weapon.def;
        if ( stance == PMF_DUCKED ) {   /* Ducked */
            ofsF = def->duckedOfsF;
            ofsR = def->duckedOfsR;
            ofsU = def->duckedOfsU;
        } else {               /* Standing */
            ofsF = def->standMoveF;
            ofsR = def->standMoveR;
            ofsU = def->standMoveU;
        }
    }

    /* Manual tuning cvars */
    ofsR += cl_gunX ? cl_gunX->value : 0.0f;
    ofsF += cl_gunY ? cl_gunY->value : 0.0f;
    ofsU += cl_gunZ ? cl_gunZ->value : 0.0f;

    /* Model origin = camera + offsets (axis[0]=F, axis[1]=L, axis[2]=U) */
    VectorCopy( cameraOrigin, modelOrigin );
    VectorMA( modelOrigin,  ofsF, axis[0], modelOrigin );   /* forward  */
    VectorMA( modelOrigin, -ofsR, axis[1], modelOrigin );   /* right (R = -L) */
    VectorMA( modelOrigin,  ofsU, axis[2], modelOrigin );   /* up       */

    AnglesToAxis( viewAngles, entityAxis );

    /* Full-screen refdef with no world (just our weapon entities) */
    Com_Memset( &refdef, 0, sizeof(refdef) );
    refdef.x      = 0;
    refdef.y      = 0;
    refdef.width  = cls.glconfig.vidWidth;
    refdef.height = cls.glconfig.vidHeight;
    refdef.time   = cl.serverTime;
    refdef.rdflags = RDF_NOWORLDMODEL;

    gunFov = ( cl_gunFov && cl_gunFov->value > 1.0f ) ? cl_gunFov->value : 65.0f;
    refdef.fov_x = gunFov;
    refdef.fov_y = 2.0f * (float)atan(
                        tan( gunFov * 0.5f * (float)(M_PI / 180.0) )
                        * (float)refdef.height / (float)refdef.width )
                   * (float)(180.0 / M_PI);

    /* Camera at tag_camera position but follows player view angles */
    VectorCopy( cameraOrigin, refdef.vieworg );
    AnglesToAxis( viewAngles, refdef.viewaxis );

    /* Start a new entity list after cgame's committed scene */
    re.ClearScene();

    /* Both entities at the same origin/axis — DObj bone evaluation has
     * already baked the tag_weapon offset into gun vertex positions. */
    if ( cl_weapon.handModel )
        AddViewmodelEnt( cl_weapon.handModel, modelOrigin, entityAxis );
    if ( cl_weapon.gunModel )
        AddViewmodelEnt( cl_weapon.gunModel,  modelOrigin, entityAxis );

    re.RenderScene( &refdef );
}

/* ===========================================================================
   Console commands
   =========================================================================== */

static void CL_Give_f( void )
{
    if ( Cmd_Argc() < 2 ) {
        Com_Printf( "Usage: give <weaponname>\n"
                    "  e.g.  give mp44_mp\n"
                    "  e.g.  give colt_mp\n"
                    "  e.g.  give mp40_mp\n" );
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

static void CL_WeaponInfo_f( void )
{
    weaponDef_t wd;
    const char *name;
    if ( Cmd_Argc() < 2 ) {
        Com_Printf( "Usage: weaponinfo <weaponname>\n" );
        return;
    }
    name = Cmd_Argv(1);
    if ( !BG_ParseWeaponDef( name, &wd ) ) {
        Com_Printf( "weaponinfo: '%s' not found\n", name );
        return;
    }
    Com_Printf( "=== %s ===\n", name );
    Com_Printf( "  type/class : %s / %s\n", wd.weaponType, wd.weaponClass );
    Com_Printf( "  gunModel   : %s\n", wd.gunModel  );
    Com_Printf( "  handModel  : %s\n", wd.handModel );
    Com_Printf( "  worldModel : %s\n", wd.worldModel);
    Com_Printf( "  damage     : %d  (melee %d)\n", wd.damage, wd.meleeDamage );
    Com_Printf( "  clip/max   : %d / %d\n", wd.clipSize, wd.maxAmmo );
    Com_Printf( "  fireTime   : %.3f s\n", wd.fireTime   );
    Com_Printf( "  reloadTime : %.3f s\n", wd.reloadTime );
    if ( wd.anims.idleAnim[0]   ) Com_Printf( "  idle    : %s\n", wd.anims.idleAnim   );
    if ( wd.anims.fireAnim[0]   ) Com_Printf( "  fire    : %s\n", wd.anims.fireAnim   );
    if ( wd.anims.reloadAnim[0] ) Com_Printf( "  reload  : %s\n", wd.anims.reloadAnim );
    if ( wd.anims.raiseAnim[0]  ) Com_Printf( "  raise   : %s\n", wd.anims.raiseAnim  );
}

/* ===========================================================================
   Init / Shutdown
   =========================================================================== */

void CL_WeaponCod1_Init( void )
{
    cl_startWeapon = Cvar_Get( "cl_startWeapon", "mp44_mp", CVAR_ARCHIVE );
    cl_gunFov      = Cvar_Get( "cl_gunFov",      "65",      CVAR_ARCHIVE );
    cl_drawGun     = Cvar_Get( "cl_drawGun",     "1",       CVAR_ARCHIVE );
    cl_animDebug   = Cvar_Get( "cl_animDebug",   "0",       CVAR_TEMP   );
    cl_gunX        = Cvar_Get( "cl_gunX",        "0",       CVAR_ARCHIVE );
    cl_gunY        = Cvar_Get( "cl_gunY",        "0",       CVAR_ARCHIVE );
    cl_gunZ        = Cvar_Get( "cl_gunZ",        "0",       CVAR_ARCHIVE );

    Cmd_AddCommand( "give",        CL_Give_f        );
    Cmd_AddCommand( "dropweapon",  CL_DropWeapon_f  );
    Cmd_AddCommand( "weaponinfo",  CL_WeaponInfo_f  );

    Com_Memset( &cl_weapon, 0, sizeof(cl_weapon) );
    s_prevPmType  = -1;
    s_spawnSerial =  0;
}

void CL_WeaponCod1_Shutdown( void )
{
    Cmd_RemoveCommand( "give"       );
    Cmd_RemoveCommand( "dropweapon" );
    Cmd_RemoveCommand( "weaponinfo" );
    Com_Memset( &cl_weapon, 0, sizeof(cl_weapon) );
    s_prevPmType = -1;
}

/*
 * CL_WeaponCurrentAnimFrame -- exposes the animation evaluator for future
 * per-frame bone skinning integration (unused until runtime skinning is added).
 */
float CL_WeaponCurrentAnimFrame( void )
{
    qhandle_t h   = cl_weapon.anims[cl_weapon.currentAnim];
    int       fps = re.XAnimFramerate ? re.XAnimFramerate(h) : 30;
    int       nf  = re.XAnimNumFrames ? re.XAnimNumFrames(h) : 1;
    float     elapsed, frame;

    if ( fps <= 0 ) fps = 30;
    if ( nf  <= 0 ) nf  = 1;

    elapsed = (float)( cls.realtime - cl_weapon.animStartTime ) / 1000.0f;
    frame   = elapsed * (float)fps;

    if ( cl_weapon.currentAnim == WA_IDLE || cl_weapon.currentAnim == WA_EMPTY_IDLE ) {
        if ( nf > 1 ) frame = (float)fmod( (double)frame, (double)nf );
        else          frame = 0.0f;
    } else {
        if ( frame >= (float)nf ) {
            cl_weapon.currentAnim   = WA_IDLE;
            cl_weapon.animStartTime = cls.realtime;
            frame = 0.0f;
        }
    }
    return frame;
}
