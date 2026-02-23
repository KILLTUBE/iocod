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
static cvar_t *cl_gunPosScale;  /* scale multiplier for weapon offsets  */
static cvar_t *cl_animDebug;    /* print current anim frame each frame  */

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

/* Weapon bob/sway state - tracks interpolation between stances */
typedef struct {
    float   lastMoveAng[3];      /* interpolated movement angles */
    float   lastIdleFactor;      /* idle sway amount */
    int     weapIdleTime;        /* time accumulator for idle sway */
    float   swayAngles[3];       /* current sway angles */
} weaponSwayState_t;

static weaponSwayState_t s_sway = {0};

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
    vec3_t        viewOrigin;
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

    /* CPU-skin hands and gun using the current animation.
     * This evaluates bones at 'animFrame', re-skins all vertices in-place,
     * and updates mdvTag entries (so re.LerpTag returns animated positions).
     * Must be done before AddRefEntityToScene / RenderScene. */
    if ( re.UpdateXModelPose ) {
        if ( cl_weapon.handModel )
            re.UpdateXModelPose( cl_weapon.handModel, curAnim, animFrame );
        if ( cl_weapon.gunModel )
            re.UpdateXModelPose( cl_weapon.gunModel, curAnim, animFrame );
    }

    /* Debug output */
    if ( cl_animDebug && cl_animDebug->integer )
        Com_Printf( "anim=%d frame=%.1f\n", cl_weapon.currentAnim, animFrame );

    /* Get player stance and build view axis */
    vec3_t cameraOrigin, modelOrigin, modelAngles;
    vec3_t gunOffset[3];  /* [0]=forward, [1]=right, [2]=up */
    vec3_t gunRot[3];     /* rotation angles */
    int stance;
    float xyspeed, adsFrac;
    weaponDef_t *def = &cl_weapon.def;
    int i;

    VectorCopy( cl.snap.ps.origin, cameraOrigin );
    cameraOrigin[2] += cl.snap.ps.viewheight;
    VectorCopy( cl.snap.ps.viewangles, viewAngles );
    AnglesToAxis( viewAngles, axis );

    /* Determine stance: 0=stand, 4=duck, 5=prone (pm_type values) */
    stance = cl.snap.ps.pm_type;
    xyspeed = sqrtf( cl.snap.ps.velocity[0] * cl.snap.ps.velocity[0] +
                     cl.snap.ps.velocity[1] * cl.snap.ps.velocity[1] );

    /* ADS fraction (0 = hip, 1 = fully ADS) - simplified for now */
    adsFrac = 0.0f;  /* TODO: get from player state when ADS is implemented */

    /* Select stance-specific offsets */
    if ( stance == 5 ) {  /* Prone */
        gunOffset[0][0] = def->proneOfsF;  gunOffset[1][0] = def->proneOfsR;  gunOffset[2][0] = def->proneOfsU;
        gunRot[0][0] = def->proneRotP;    gunRot[1][0] = def->proneRotY;    gunRot[2][0] = def->proneRotR;
    } else if ( stance == 4 ) {  /* Ducked */
        gunOffset[0][0] = def->duckedOfsF; gunOffset[1][0] = def->duckedOfsR; gunOffset[2][0] = def->duckedOfsU;
        gunRot[0][0] = def->duckedRotP;   gunRot[1][0] = def->duckedRotY;   gunRot[2][0] = def->duckedRotR;
    } else {  /* Standing */
        gunOffset[0][0] = def->standMoveF; gunOffset[1][0] = def->standMoveR; gunOffset[2][0] = def->standMoveU;
        gunRot[0][0] = def->standRotP;    gunRot[1][0] = def->standRotY;    gunRot[2][0] = def->standRotR;
    }

    /* Calculate weapon bob/sway - simplified version */
    /* Time-based idle sway - use smaller scale factor */
    float idleAmount = def->hipIdleAmount;
    if ( stance == 4 ) idleAmount *= def->idleCrouchFactor;
    else if ( stance == 5 ) idleAmount *= def->idleProneFactor;

    /* Clamp idle amount to reasonable values */
    if ( idleAmount > 30.0f ) idleAmount = 30.0f;

    s_sway.weapIdleTime += cls.frametime * 1000.0f;
    /* Much smaller sway - CoD values are degrees, we need small radians */
    float swayScale = 0.0001f;  /* very subtle */
    gunRot[0][0] += sinf( s_sway.weapIdleTime * 0.001f ) * idleAmount * swayScale;
    gunRot[1][0] += sinf( s_sway.weapIdleTime * 0.0007f ) * idleAmount * swayScale;
    gunRot[2][0] += sinf( s_sway.weapIdleTime * 0.0005f ) * idleAmount * swayScale;

    /* Movement bob - only when actually moving on ground */
    if ( xyspeed > 50.0f && stance != 5 ) {
        float bob = sinf( cls.realtime * 0.01f ) * 0.1f;
        gunRot[2][0] += bob;  /* Roll bob */
    }

    /* Build model position: start at camera, apply offsets along view axes */
    VectorCopy( cameraOrigin, modelOrigin );

    /* Get scale factor for tuning */
    float scale = ( cl_gunPosScale && cl_gunPosScale->value > 0.0f ) ? cl_gunPosScale->value : 1.0f;

    /* axis[0] = forward, axis[1] = left, axis[2] = up in Quake */
    /* But CoD offsets are: F = forward, R = right, U = up */
    VectorMA( modelOrigin, gunOffset[0][0] * scale, axis[0], modelOrigin );  /* Forward */
    VectorMA( modelOrigin, -gunOffset[1][0] * scale, axis[1], modelOrigin ); /* Right (negate left axis) */
    VectorMA( modelOrigin, gunOffset[2][0] * scale, axis[2], modelOrigin );  /* Up */

    /* Build model angles: view angles + weapon rotation */
    VectorCopy( viewAngles, modelAngles );
    modelAngles[0] += gunRot[0][0];  /* Pitch */
    modelAngles[1] += gunRot[1][0];  /* Yaw */
    modelAngles[2] += gunRot[2][0];  /* Roll */

    /* Build entity rotation axis from model angles */
    vec3_t entityAxis[3];
    AnglesToAxis( modelAngles, entityAxis );

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

    /* Camera stays at eye position with raw view angles */
    VectorCopy( cameraOrigin, refdef.vieworg );
    AnglesToAxis( viewAngles, refdef.viewaxis );

    /* Start a new entity list after cgame's committed scene */
    re.ClearScene();

    if ( cl_weapon.handModel ) {
        AddViewmodelEnt( cl_weapon.handModel, modelOrigin, entityAxis );
    }
    if ( cl_weapon.gunModel ) {
        /* Place gun at the same root origin as the hands.  In CoD, the hand
         * and gun models share a skeleton via DObj — both are rendered from
         * viewOrigin and the shared animation drives all bones (including
         * tag_weapon) relative to that root.  Placing the gun at the hand's
         * tag_weapon would double-count the tag_weapon bone offset. */
        AddViewmodelEnt( cl_weapon.gunModel, modelOrigin, entityAxis );
    }

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
    cl_gunPosScale = Cvar_Get( "cl_gunPosScale", "1.0",     CVAR_ARCHIVE );
    cl_animDebug   = Cvar_Get( "cl_animDebug",   "0",       CVAR_TEMP  );

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
