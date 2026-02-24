/*
===========================================================================
g_scr.c  —  GSC scripting integration for the game module.

Architecture
------------
One gsc_Context is created per map load (G_Scr_Init) and destroyed on
map unload (G_Scr_Shutdown).  All active script threads are advanced once
per game frame (G_Scr_Frame).

Script loading (CoD1 / CoD2 style):
  maps/mp/<mapname>.gsc             — map-specific script (optional)
  maps/mp/gametypes/_callbacksetup.gsc — defines CodeCallback_* hooks

Entity objects
--------------
Each connected client gets a tagged GSC object "#entity" that shares a
common proxy object.  The proxy's __call field is an object containing
native functions registered as method names.  When a script calls
  self getName()
the GSC VM looks up "getname" on the proxy and calls the C function.
The C function retrieves the gentity_t* from the object's userdata.

Globals set in the GSC namespace:
  level   — tagged object holding level-wide state
  game    — tagged object (CoD2 compat placeholder)
  anim    — tagged object (CoD2 compat placeholder)
  self    — swapped to the relevant entity before each callback

Player object globals: "player_N" (N = clientNum, 0-based).
===========================================================================
*/

#ifdef STANDALONE

#include "g_local.h"
#include "g_scr.h"
#include "../thirdparty/gsc/include/gsc.h"

#include <stdlib.h>
#include <string.h>

/* =========================================================================
   Module-level state
   ========================================================================= */

static gsc_Context *g_scrCtx;
static qboolean     g_scrActive;

/* Stack index of the shared entity proxy object.  Stays at bottom of stack
   for the lifetime of the context.                                         */
static int          g_scrEntityProxyIdx;

/* Per-client: raw pointer to the GSC object so we can push it later.
   NULL if no object exists for this slot.                                  */
static void        *g_scrPlayerObjPtrs[ MAX_CLIENTS ];

/* The GSC file namespace (path without .gsc) where CodeCallback_* live.   */
static char         g_scrCallbackFile[ MAX_QPATH ];

/* =========================================================================
   Source-buffer tracking — freed after gsc_link completes
   ========================================================================= */

typedef struct ScrSrc_s { struct ScrSrc_s *next; } ScrSrc_t;
static ScrSrc_t *g_scrSources;

static void G_Scr_FreeSources( void )
{
    ScrSrc_t *s, *next;
    for ( s = g_scrSources; s; s = next ) {
        next = s->next;
        free( s );
    }
    g_scrSources = NULL;
}

/* =========================================================================
   GSC context callbacks
   ========================================================================= */

static void *G_Scr_AllocMem( void *ctx, int size )
{
    (void)ctx;
    return calloc( 1, (size_t)size );
}

static void G_Scr_FreeMem( void *ctx, void *ptr )
{
    (void)ctx;
    free( ptr );
}

/*
Reads a .gsc source file using the game module's file traps.
The filename received from the GSC library has no extension; we append
".gsc" automatically (same convention as cl_character_cod1.c).
Memory is malloc'd and tracked for batch-free after gsc_link().
*/
static const char *G_Scr_ReadFile( void *ctx, const char *filename, int *status )
{
    fileHandle_t fh;
    int          len;
    char         path[ MAX_QPATH ];
    char        *buf;
    ScrSrc_t    *hdr;

    (void)ctx;

    /* Append .gsc if the caller didn't already include an extension */
    Q_strncpyz( path, filename, sizeof( path ) );
    if ( !COM_GetExtension( path )[0] ) {
        Q_strcat( path, sizeof( path ), ".gsc" );
    }

    len = trap_FS_FOpenFile( path, &fh, FS_READ );
    if ( len <= 0 || !fh ) {
        if ( fh ) {
            trap_FS_FCloseFile( fh );
        }
        *status = GSC_NOT_FOUND;
        return NULL;
    }

    /* Allocate: ScrSrc_t header (for tracking) + source + NUL */
    buf = (char *)malloc( sizeof( ScrSrc_t ) + (size_t)len + 1 );
    if ( !buf ) {
        trap_FS_FCloseFile( fh );
        *status = GSC_OUT_OF_MEMORY;
        return NULL;
    }

    trap_FS_Read( buf + sizeof( ScrSrc_t ), len, fh );
    trap_FS_FCloseFile( fh );
    buf[ sizeof( ScrSrc_t ) + len ] = '\0';

    /* Track for deferred free */
    hdr       = (ScrSrc_t *)buf;
    hdr->next = g_scrSources;
    g_scrSources = hdr;

    *status = GSC_OK;
    return buf + sizeof( ScrSrc_t );
}

/* =========================================================================
   Helper: compile one script (and all its #using dependencies)
   Returns qtrue if compilation succeeded.
   ========================================================================= */
static qboolean G_Scr_CompileScript( const char *nameSpace )
{
    int         status;
    const char *dep;

    status = gsc_compile( g_scrCtx, nameSpace, 0 );

    while ( status == GSC_OK &&
            ( dep = gsc_next_compile_dependency( g_scrCtx ) ) != NULL ) {
        status = gsc_compile( g_scrCtx, dep, 0 );
    }

    if ( status != GSC_OK ) {
        G_Printf( "GSC: compile failed for '%s' (status %d)\n",
                  nameSpace, status );
        return qfalse;
    }
    return qtrue;
}

/* =========================================================================
   Entity proxy — methods callable as  self methodName(args)
   ========================================================================= */

/* Retrieve the gentity_t* stored as userdata on the "self" entity object.
   self is at argument index -1 in the current call frame.                 */
static gentity_t *G_Scr_GetSelf( gsc_Context *ctx )
{
    int selfIdx = gsc_get_object( ctx, -1 );
    if ( selfIdx < 0 ) {
        return NULL;
    }
    return (gentity_t *)gsc_object_get_userdata( ctx, selfIdx );
}

static int GScr_Meth_GetName( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent || !ent->client ) {
        gsc_add_string( ctx, "" );
        return 1;
    }
    gsc_add_string( ctx, ent->client->pers.netname );
    return 1;
}

static int GScr_Meth_GetOrigin( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent || !ent->client ) {
        float zero[3] = { 0.0f, 0.0f, 0.0f };
        gsc_add_vec3( ctx, zero );
        return 1;
    }
    gsc_add_vec3( ctx, ent->client->ps.origin );
    return 1;
}

static int GScr_Meth_SetOrigin( gsc_Context *ctx )
{
    float     v[3];
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent || !ent->client ) {
        return 0;
    }
    gsc_get_vec3( ctx, 0, v );
    VectorCopy( v, ent->client->ps.origin );
    G_SetOrigin( ent, v );
    return 0;
}

static int GScr_Meth_GetHealth( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_int( ctx, ent ? ent->health : 0 );
    return 1;
}

static int GScr_Meth_SetHealth( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent ) {
        return 0;
    }
    ent->health = (int)gsc_get_int( ctx, 0 );
    if ( ent->client ) {
        ent->client->ps.stats[ STAT_HEALTH ] = ent->health;
    }
    return 0;
}

static int GScr_Meth_GetTeam( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent || !ent->client ) {
        gsc_add_string( ctx, "spectator" );
        return 1;
    }
    switch ( ent->client->sess.sessionTeam ) {
        case TEAM_RED:  gsc_add_string( ctx, "red" );       break;
        case TEAM_BLUE: gsc_add_string( ctx, "blue" );      break;
        case TEAM_FREE: gsc_add_string( ctx, "free" );      break;
        default:        gsc_add_string( ctx, "spectator" ); break;
    }
    return 1;
}

static int GScr_Meth_GetClientNum( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_int( ctx, ent ? ent->s.clientNum : -1 );
    return 1;
}

static int GScr_Meth_IsPlayer( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_bool( ctx, ent && ent->client &&
                  ent->client->pers.connected != CON_DISCONNECTED );
    return 1;
}

static int GScr_Meth_IsAlive( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_bool( ctx, ent && ent->health > 0 );
    return 1;
}

/* self spawn() — respawn the player at a spawn point */
static int GScr_Meth_Spawn( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent && ent->client &&
         ent->client->pers.connected == CON_CONNECTED ) {
        ClientSpawn( ent );
    }
    return 0;
}

/* self suicide() */
static int GScr_Meth_Suicide( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent && ent->client && ent->health > 0 ) {
        G_Damage( ent, ent, ent, NULL, NULL,
                  ent->health + 1, DAMAGE_NO_PROTECTION, MOD_SUICIDE );
    }
    return 0;
}

/* =========================================================================
   Global / free functions available to scripts
   ========================================================================= */

static int GScr_Fn_Print( gsc_Context *ctx )
{
    int i, n = gsc_numargs( ctx );
    for ( i = 0; i < n; i++ ) {
        G_Printf( "%s", gsc_get_string( ctx, i ) );
    }
    G_Printf( "\n" );
    return 0;
}

static int GScr_Fn_IsDefined( gsc_Context *ctx )
{
    gsc_add_bool( ctx, gsc_get_type( ctx, 0 ) != GSC_TYPE_UNDEFINED );
    return 1;
}

static int GScr_Fn_RandomInt( gsc_Context *ctx )
{
    int maxVal = (int)gsc_get_int( ctx, 0 );
    if ( maxVal <= 0 ) {
        gsc_add_int( ctx, 0 );
        return 1;
    }
    gsc_add_int( ctx, rand() % maxVal );
    return 1;
}

static int GScr_Fn_RandomFloat( gsc_Context *ctx )
{
    float maxVal = gsc_get_float( ctx, 0 );
    if ( maxVal <= 0.0f ) {
        gsc_add_float( ctx, 0.0f );
        return 1;
    }
    gsc_add_float( ctx, ( (float)rand() / (float)RAND_MAX ) * maxVal );
    return 1;
}

static int GScr_Fn_GetDvar( gsc_Context *ctx )
{
    char        buf[ 256 ];
    const char *name = gsc_get_string( ctx, 0 );
    if ( !name ) name = "";
    trap_Cvar_VariableStringBuffer( name, buf, sizeof( buf ) );
    gsc_add_string( ctx, buf );
    return 1;
}

static int GScr_Fn_GetDvarInt( gsc_Context *ctx )
{
    const char *name = gsc_get_string( ctx, 0 );
    if ( !name ) name = "";
    gsc_add_int( ctx, trap_Cvar_VariableIntegerValue( name ) );
    return 1;
}

static int GScr_Fn_SetDvar( gsc_Context *ctx )
{
    const char *name  = gsc_get_string( ctx, 0 );
    const char *value = gsc_get_string( ctx, 1 );
    if ( name && value ) {
        trap_Cvar_Set( name, value );
    }
    return 0;
}

/* gettime() — returns level time in seconds */
static int GScr_Fn_GetTime( gsc_Context *ctx )
{
    gsc_add_float( ctx, (float)level.time * 0.001f );
    return 1;
}

/* getent( clientNum ) — returns the player entity object */
static int GScr_Fn_GetEnt( gsc_Context *ctx )
{
    char globalName[ 32 ];
    int  clientNum = (int)gsc_get_int( ctx, 0 );
    int  topBefore;

    if ( clientNum < 0 || clientNum >= MAX_CLIENTS ||
         !g_scrPlayerObjPtrs[ clientNum ] ) {
        return 0; /* undefined */
    }

    topBefore = gsc_top( g_scrCtx );
    gsc_push_object( g_scrCtx, g_scrPlayerObjPtrs[ clientNum ] );
    if ( gsc_top( g_scrCtx ) <= topBefore ) {
        /* Fallback: retrieve via the named global */
        Com_sprintf( globalName, sizeof( globalName ), "player_%d", clientNum );
        gsc_get_global( g_scrCtx, globalName );
    }
    return 1;
}

/* getentarray() — CoD1 compat stub, returns empty array object */
static int GScr_Fn_GetEntArray( gsc_Context *ctx )
{
    gsc_add_object( ctx );
    return 1;
}

/* exitLevel(bool) — CoD1: end the map; we just trigger a vote_nextmap */
static int GScr_Fn_ExitLevel( gsc_Context *ctx )
{
    (void)ctx;
    trap_SendConsoleCommand( EXEC_APPEND, "map_restart 0\n" );
    return 0;
}

/* setcvar(name, value) — alias for setDvar, used in _callbacksetup */
static int GScr_Fn_SetCvar( gsc_Context *ctx )
{
    const char *name  = gsc_get_string( ctx, 0 );
    const char *value = gsc_get_string( ctx, 1 );
    if ( name && value ) {
        trap_Cvar_Set( name, value );
    }
    return 0;
}

/* getCvar(name) — alias for getDvar */
static int GScr_Fn_GetCvar( gsc_Context *ctx )
{
    return GScr_Fn_GetDvar( ctx );
}

/* ambientPlay / ambientStop — sound stubs (not yet implemented) */
static int GScr_Fn_AmbientPlay( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_AmbientStop( gsc_Context *ctx ) { (void)ctx; return 0; }

/* setCullFog — client-side, no-op on server */
static int GScr_Fn_SetCullFog( gsc_Context *ctx ) { (void)ctx; return 0; }

/* maps_mp_gametypes_callbacksetup compatibility stubs */
static int GScr_Fn_GetMaxPlayers( gsc_Context *ctx )
{
    gsc_add_int( ctx, level.maxclients );
    return 1;
}

static int GScr_Fn_GetNumPlayers( gsc_Context *ctx )
{
    gsc_add_int( ctx, level.numConnectedClients );
    return 1;
}

/* =========================================================================
   Register all built-in functions.
   The GSC hash trie is case-insensitive, so one registration per function
   covers all capitalisation variants.
   ========================================================================= */
static void G_Scr_RegisterFunctions( void )
{
    /* Output */
    gsc_register_function( g_scrCtx, NULL, "print",        GScr_Fn_Print );
    gsc_register_function( g_scrCtx, NULL, "println",      GScr_Fn_Print );
    gsc_register_function( g_scrCtx, NULL, "iprintln",     GScr_Fn_Print );
    gsc_register_function( g_scrCtx, NULL, "iprintlnbold", GScr_Fn_Print );

    /* Type checking */
    gsc_register_function( g_scrCtx, NULL, "isdefined",    GScr_Fn_IsDefined );

    /* Math / random */
    gsc_register_function( g_scrCtx, NULL, "randomint",    GScr_Fn_RandomInt );
    gsc_register_function( g_scrCtx, NULL, "randomfloat",  GScr_Fn_RandomFloat );

    /* Cvar / dvar — register both spellings since they're different names */
    gsc_register_function( g_scrCtx, NULL, "getdvar",      GScr_Fn_GetDvar );
    gsc_register_function( g_scrCtx, NULL, "getdvarint",   GScr_Fn_GetDvarInt );
    gsc_register_function( g_scrCtx, NULL, "setdvar",      GScr_Fn_SetDvar );
    gsc_register_function( g_scrCtx, NULL, "getcvar",      GScr_Fn_GetCvar );   /* CoD1 alias */
    gsc_register_function( g_scrCtx, NULL, "setcvar",      GScr_Fn_SetCvar );   /* CoD1 alias */

    /* Time */
    gsc_register_function( g_scrCtx, NULL, "gettime",      GScr_Fn_GetTime );

    /* Entity access */
    gsc_register_function( g_scrCtx, NULL, "getent",       GScr_Fn_GetEnt );
    gsc_register_function( g_scrCtx, NULL, "getentarray",  GScr_Fn_GetEntArray );

    /* Player counts */
    gsc_register_function( g_scrCtx, NULL, "getmaxplayers", GScr_Fn_GetMaxPlayers );
    gsc_register_function( g_scrCtx, NULL, "getnumplayers", GScr_Fn_GetNumPlayers );

    /* Level control */
    gsc_register_function( g_scrCtx, NULL, "exitlevel",    GScr_Fn_ExitLevel );

    /* Client-side / audio stubs (no-ops on the server) */
    gsc_register_function( g_scrCtx, NULL, "ambientplay",  GScr_Fn_AmbientPlay );
    gsc_register_function( g_scrCtx, NULL, "ambientstop",  GScr_Fn_AmbientStop );
    gsc_register_function( g_scrCtx, NULL, "setcullfog",   GScr_Fn_SetCullFog );
}

/* =========================================================================
   Create the shared entity proxy and the level/game/anim globals.

   Stack layout after this function (permanent — never popped):
     [0] = shared entity proxy object  (g_scrEntityProxyIdx)

   Globals set:
     level, game, anim  (tagged empty objects, CoD compat)
     self               (initially = level object)
   ========================================================================= */
static void G_Scr_CreateGlobals( void )
{
    int proxyObj;
    int methodsObj;
    int entObj;

    /* ---- shared entity proxy ---- */
    proxyObj = gsc_add_tagged_object( g_scrCtx, "#entity_proxy" );
    g_scrEntityProxyIdx = proxyObj;

    methodsObj = gsc_add_object( g_scrCtx );

    /* Register entity methods on the methods object */
    gsc_add_function( g_scrCtx, GScr_Meth_GetName );
    gsc_object_set_field( g_scrCtx, methodsObj, "getname" );

    gsc_add_function( g_scrCtx, GScr_Meth_GetOrigin );
    gsc_object_set_field( g_scrCtx, methodsObj, "getorigin" );

    gsc_add_function( g_scrCtx, GScr_Meth_SetOrigin );
    gsc_object_set_field( g_scrCtx, methodsObj, "setorigin" );

    gsc_add_function( g_scrCtx, GScr_Meth_GetHealth );
    gsc_object_set_field( g_scrCtx, methodsObj, "gethealth" );

    gsc_add_function( g_scrCtx, GScr_Meth_SetHealth );
    gsc_object_set_field( g_scrCtx, methodsObj, "sethealth" );

    gsc_add_function( g_scrCtx, GScr_Meth_GetTeam );
    gsc_object_set_field( g_scrCtx, methodsObj, "getteam" );

    gsc_add_function( g_scrCtx, GScr_Meth_GetClientNum );
    gsc_object_set_field( g_scrCtx, methodsObj, "getclientnum" );

    gsc_add_function( g_scrCtx, GScr_Meth_IsPlayer );
    gsc_object_set_field( g_scrCtx, methodsObj, "isplayer" );

    gsc_add_function( g_scrCtx, GScr_Meth_IsAlive );
    gsc_object_set_field( g_scrCtx, methodsObj, "isalive" );

    gsc_add_function( g_scrCtx, GScr_Meth_Spawn );
    gsc_object_set_field( g_scrCtx, methodsObj, "spawn" );

    gsc_add_function( g_scrCtx, GScr_Meth_Suicide );
    gsc_object_set_field( g_scrCtx, methodsObj, "suicide" );

    /* methods object becomes the __call handler on the proxy */
    gsc_object_set_field( g_scrCtx, proxyObj, "__call" );
    /* Stack: [proxyObj] — methodsObj was consumed */

    /* ---- level object (CoD compat global) ---- */
    entObj = gsc_add_tagged_object( g_scrCtx, "#level" );
    gsc_object_set_proxy( g_scrCtx, entObj, proxyObj );
    gsc_set_global( g_scrCtx, "level" );

    /* ---- game object ---- */
    gsc_add_tagged_object( g_scrCtx, "#game" );
    gsc_set_global( g_scrCtx, "game" );

    /* ---- anim object ---- */
    gsc_add_tagged_object( g_scrCtx, "#anim" );
    gsc_set_global( g_scrCtx, "anim" );

    /* ---- default self = level ---- */
    gsc_get_global( g_scrCtx, "level" );
    gsc_set_global( g_scrCtx, "self" );

    /* Stack is now just [proxyObj] which lives here forever */
}

/* =========================================================================
   Create and register a GSC entity object for a connected client.
   ========================================================================= */
static void G_Scr_CreatePlayerObj( int clientNum )
{
    char       globalName[ 32 ];
    gentity_t *ent;
    int        entObj;

    ent = &g_entities[ clientNum ];

    entObj = gsc_add_tagged_object( g_scrCtx, "#entity" );
    gsc_object_set_proxy( g_scrCtx, entObj, g_scrEntityProxyIdx );
    gsc_object_set_userdata( g_scrCtx, entObj, ent );

    /* Store raw ptr so we can push it back later for callbacks */
    g_scrPlayerObjPtrs[ clientNum ] = gsc_get_ptr( g_scrCtx, entObj );

    /* Register as a named global to keep it alive */
    Com_sprintf( globalName, sizeof( globalName ), "player_%d", clientNum );
    gsc_set_global( g_scrCtx, globalName ); /* pops entObj */
}

/* =========================================================================
   Swap the GSC "self" global to the given player entity object.
   Must be called before any callback that needs self = player.
   ========================================================================= */
static void G_Scr_SetSelfToPlayer( int clientNum )
{
    char globalName[ 32 ];
    int  topBefore;

    if ( !g_scrPlayerObjPtrs[ clientNum ] ) {
        return;
    }

    Com_sprintf( globalName, sizeof( globalName ), "player_%d", clientNum );

    topBefore = gsc_top( g_scrCtx );
    gsc_get_global( g_scrCtx, globalName );

    if ( gsc_top( g_scrCtx ) > topBefore ) {
        gsc_set_global( g_scrCtx, "self" );
    }
}

/* =========================================================================
   Try to call a function in g_scrCallbackFile.
   Silently ignores GSC_NOT_FOUND (callback not in this script).
   ========================================================================= */
static void G_Scr_ExecCallback( const char *fnName )
{
    int status;
    if ( !g_scrActive || !g_scrCtx || !g_scrCallbackFile[0] ) {
        return;
    }
    status = gsc_call( g_scrCtx, g_scrCallbackFile, fnName, 0 );
    if ( status != GSC_OK && status != GSC_NOT_FOUND ) {
        G_Printf( "GSC: error in %s::%s (status %d)\n",
                  g_scrCallbackFile, fnName, status );
    }
}

/* Same but sets self = player before calling */
static void G_Scr_ExecPlayerCallback( int clientNum, const char *fnName )
{
    if ( !g_scrActive || !g_scrCtx || !g_scrCallbackFile[0] ) {
        return;
    }
    G_Scr_SetSelfToPlayer( clientNum );
    G_Scr_ExecCallback( fnName );
}

/* =========================================================================
   G_Scr_GetGametypeString
   Returns the CoD1 MP gametype name ("dm", "tdm", "hq", "sd", "re", "bel").
   g_gametype in Q3 is an integer (0=FFA, 3=TDM, 4=CTF…).
   In CoD1 MP it is a string cvar set by the server: +set g_gametype dm
   We read the raw string; if it looks like a CoD1 name we use it directly,
   otherwise we map Q3 integers to CoD1 names.
   ========================================================================= */
static void G_Scr_GetGametypeString( char *out, int outSize )
{
    char buf[ 64 ];
    trap_Cvar_VariableStringBuffer( "g_gametype", buf, sizeof( buf ) );

    /* If already a known CoD1 MP gametype name, use it */
    if ( Q_stricmp( buf, "dm"  ) == 0 || Q_stricmp( buf, "tdm" ) == 0 ||
         Q_stricmp( buf, "hq"  ) == 0 || Q_stricmp( buf, "sd"  ) == 0 ||
         Q_stricmp( buf, "re"  ) == 0 || Q_stricmp( buf, "bel" ) == 0 ) {
        Q_strncpyz( out, buf, outSize );
        return;
    }

    /* Q3 integer → CoD1 name fallback */
    switch ( atoi( buf ) ) {
        case 3: /* GT_TEAM */ Q_strncpyz( out, "tdm", outSize ); break;
        case 4: /* GT_CTF  */ Q_strncpyz( out, "sd",  outSize ); break;
        default:              Q_strncpyz( out, "dm",  outSize ); break;
    }
}

/* =========================================================================
   G_Scr_Init  —  called at the end of G_InitGame
   ========================================================================= */
void G_Scr_Init( void )
{
    gsc_CreateOptions opts;
    char              serverinfo[ MAX_INFO_STRING ];
    const char       *mapname;
    char              gametype[ 16 ];
    char              gametypeScript[ MAX_QPATH ];  /* maps/MP/gametypes/<gt>    */
    char              callbackScript[ MAX_QPATH ];  /* maps/MP/gametypes/_cbsetup */
    char              mapScript[ MAX_QPATH ];       /* maps/MP/<mapname>          */
    char              mapScriptSP[ MAX_QPATH ];     /* maps/<mapname> (SP fallback) */
    int               status;
    qboolean          gametypeOk;
    qboolean          callbackOk;
    qboolean          mapOk;

    /* Reset state */
    g_scrCtx     = NULL;
    g_scrActive  = qfalse;
    g_scrSources = NULL;
    Com_Memset( g_scrPlayerObjPtrs, 0, sizeof( g_scrPlayerObjPtrs ) );
    g_scrCallbackFile[0] = '\0';

    /* Determine map name and gametype */
    trap_GetServerinfo( serverinfo, sizeof( serverinfo ) );
    mapname = Info_ValueForKey( serverinfo, "mapname" );
    if ( !mapname || !mapname[0] ) {
        G_Printf( "GSC: no mapname in serverinfo, scripting disabled\n" );
        return;
    }

    G_Scr_GetGametypeString( gametype, sizeof( gametype ) );

    /*
     Script namespaces (no .gsc extension — the read_file callback adds it).
     CoD1 MP maps live under  maps/MP/<mapname>.gsc
     CoD1 SP maps live under  maps/<mapname>.gsc
     Gametypes live under     maps/MP/gametypes/<gametype>.gsc
     _callbacksetup is at     maps/MP/gametypes/_callbacksetup.gsc
                                                                             */
    Com_sprintf( gametypeScript, sizeof( gametypeScript ),
                 "maps/MP/gametypes/%s", gametype );
    Com_sprintf( callbackScript, sizeof( callbackScript ),
                 "maps/MP/gametypes/_callbacksetup" );
    Com_sprintf( mapScript,      sizeof( mapScript ),
                 "maps/MP/%s",   mapname );
    Com_sprintf( mapScriptSP,    sizeof( mapScriptSP ),
                 "maps/%s",      mapname );

    G_Printf( "GSC: map '%s'  gametype '%s'\n", mapname, gametype );

    /* Create context */
    Com_Memset( &opts, 0, sizeof( opts ) );
    opts.allocate_memory          = G_Scr_AllocMem;
    opts.free_memory              = G_Scr_FreeMem;
    opts.read_file                = G_Scr_ReadFile;
    opts.verbose                  = 0;
    opts.main_memory_size         = 32 * 1024 * 1024; /* 32 MB */
    opts.temp_memory_size         = 4  * 1024 * 1024; /*  4 MB */
    opts.string_table_memory_size = 2  * 1024 * 1024; /*  2 MB */
    opts.default_self             = "self";
    opts.max_threads              = 128;

    g_scrCtx = gsc_create( opts );
    if ( !g_scrCtx ) {
        G_Printf( "GSC: failed to create context\n" );
        return;
    }

    G_Scr_RegisterFunctions();
    G_Scr_CreateGlobals();

    /*
     CoD1 MP loading order (matches GScr_LoadGameTypeScript in CoD2):
       1. Gametype script  — sets level.callback* and registers game logic
          (auto-loads _callbacksetup and _teams/_spawnlogic as dependencies
           via maps\mp\gametypes\... :: cross-file call syntax)
       2. _callbacksetup   — always compile explicitly too, as the GSC library
          may not yet resolve the CoD-style :: cross-file dependency notation
       3. Map script       — sets game["allies"]/["axis"], loads _load/_fx etc.
          Try maps/MP/<name>.gsc first (MP), fall back to maps/<name>.gsc (SP)
    */
    gametypeOk = G_Scr_CompileScript( gametypeScript );
    callbackOk = G_Scr_CompileScript( callbackScript );  /* explicit, safe to dup */
    mapOk      = G_Scr_CompileScript( mapScript );
    if ( !mapOk ) {
        mapOk = G_Scr_CompileScript( mapScriptSP );      /* SP fallback */
        if ( mapOk ) {
            Q_strncpyz( mapScript, mapScriptSP, sizeof( mapScript ) );
        }
    }

    if ( !gametypeOk && !callbackOk && !mapOk ) {
        G_Printf( "GSC: no scripts found for map '%s' gametype '%s', "
                  "scripting disabled\n", mapname, gametype );
        gsc_destroy( g_scrCtx );
        G_Scr_FreeSources();
        g_scrCtx = NULL;
        return;
    }

    /* Link all compiled files together */
    status = gsc_link( g_scrCtx );
    G_Scr_FreeSources(); /* source text no longer needed after link */

    if ( status != GSC_OK ) {
        G_Printf( "GSC: link failed (status %d)\n", status );
        gsc_destroy( g_scrCtx );
        g_scrCtx = NULL;
        return;
    }

    /*
     Choose which file holds the CodeCallback_* entry points.
     _callbacksetup is the canonical location; fall back to gametype or map.
    */
    if ( callbackOk ) {
        Q_strncpyz( g_scrCallbackFile, callbackScript,
                    sizeof( g_scrCallbackFile ) );
    } else if ( gametypeOk ) {
        Q_strncpyz( g_scrCallbackFile, gametypeScript,
                    sizeof( g_scrCallbackFile ) );
    } else {
        Q_strncpyz( g_scrCallbackFile, mapScript,
                    sizeof( g_scrCallbackFile ) );
    }

    g_scrActive = qtrue;
    G_Printf( "GSC: scripting active  callbacks in '%s'\n", g_scrCallbackFile );

    /*
     Execution order:
       1. Gametype main()  — registers level.callbackStartGameType etc.
       2. Map main()       — sets game["allies"], game["axis"], loads FX etc.
       3. CodeCallback_StartGameType() — calls level.callbackStartGameType()
    */
    if ( gametypeOk ) {
        status = gsc_call( g_scrCtx, gametypeScript, "main", 0 );
        if ( status != GSC_OK && status != GSC_NOT_FOUND ) {
            G_Printf( "GSC: error in %s::main (status %d)\n",
                      gametypeScript, status );
        }
        while ( gsc_update( g_scrCtx, 0.0f ) == GSC_YIELD ) {}
    }

    if ( mapOk ) {
        status = gsc_call( g_scrCtx, mapScript, "main", 0 );
        if ( status != GSC_OK && status != GSC_NOT_FOUND ) {
            G_Printf( "GSC: error in %s::main (status %d)\n",
                      mapScript, status );
        }
        while ( gsc_update( g_scrCtx, 0.0f ) == GSC_YIELD ) {}
    }

    G_Scr_ExecCallback( "CodeCallback_StartGameType" );
    while ( gsc_update( g_scrCtx, 0.0f ) == GSC_YIELD ) {}
}

/* =========================================================================
   G_Scr_Shutdown  —  called at the start of G_ShutdownGame
   ========================================================================= */
void G_Scr_Shutdown( void )
{
    if ( g_scrCtx ) {
        gsc_destroy( g_scrCtx );
        g_scrCtx = NULL;
    }
    G_Scr_FreeSources();
    Com_Memset( g_scrPlayerObjPtrs, 0, sizeof( g_scrPlayerObjPtrs ) );
    g_scrCallbackFile[0] = '\0';
    g_scrActive = qfalse;
}

/* =========================================================================
   G_Scr_Frame  —  called once per game frame from G_RunFrame
   ========================================================================= */
void G_Scr_Frame( float dt )
{
    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }
    gsc_update( g_scrCtx, dt );
}

/* =========================================================================
   Player lifecycle callbacks
   ========================================================================= */

void G_Scr_PlayerConnect( int clientNum )
{
    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }
    G_Scr_CreatePlayerObj( clientNum );
    G_Scr_ExecPlayerCallback( clientNum, "CodeCallback_PlayerConnect" );
}

void G_Scr_PlayerDisconnect( int clientNum )
{
    char globalName[ 32 ];

    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }

    G_Scr_ExecPlayerCallback( clientNum, "CodeCallback_PlayerDisconnect" );

    /* Advance any disconnect-triggered threads */
    gsc_update( g_scrCtx, 0.0f );

    /* Clear the player global so the object can be GC'd */
    Com_sprintf( globalName, sizeof( globalName ), "player_%d", clientNum );
    gsc_add_int( g_scrCtx, 0 );  /* push undefined-equivalent */
    gsc_set_global( g_scrCtx, globalName );

    g_scrPlayerObjPtrs[ clientNum ] = NULL;
}

void G_Scr_PlayerSpawn( int clientNum )
{
    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }

    /* Update the userdata pointer in case the entity was re-initialised */
    if ( g_scrPlayerObjPtrs[ clientNum ] ) {
        char globalName[ 32 ];
        int  topBefore, entObjIdx;

        Com_sprintf( globalName, sizeof( globalName ), "player_%d", clientNum );
        topBefore = gsc_top( g_scrCtx );
        gsc_get_global( g_scrCtx, globalName );
        if ( gsc_top( g_scrCtx ) > topBefore ) {
            entObjIdx = gsc_top( g_scrCtx ) - 1;
            gsc_object_set_userdata( g_scrCtx, entObjIdx,
                                     &g_entities[ clientNum ] );
            gsc_pop( g_scrCtx, 1 );
        }
    }

    G_Scr_ExecPlayerCallback( clientNum, "CodeCallback_PlayerSpawn" );
}

void G_Scr_PlayerKilled( int clientNum, int attackerNum, int mod )
{
    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }
    G_Scr_SetSelfToPlayer( clientNum );
    /* Push attacker and MOD as arguments */
    gsc_add_int( g_scrCtx, attackerNum );
    gsc_add_int( g_scrCtx, mod );
    if ( g_scrCallbackFile[0] ) {
        int status = gsc_call( g_scrCtx, g_scrCallbackFile,
                               "CodeCallback_PlayerKilled", 2 );
        if ( status != GSC_OK && status != GSC_NOT_FOUND ) {
            G_Printf( "GSC: error in CodeCallback_PlayerKilled (status %d)\n",
                      status );
        }
    }
}

void G_Scr_PlayerDamage( int clientNum, int attackerNum, int damage, int mod )
{
    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }
    G_Scr_SetSelfToPlayer( clientNum );
    gsc_add_int( g_scrCtx, attackerNum );
    gsc_add_int( g_scrCtx, damage );
    gsc_add_int( g_scrCtx, mod );
    if ( g_scrCallbackFile[0] ) {
        int status = gsc_call( g_scrCtx, g_scrCallbackFile,
                               "CodeCallback_PlayerDamage", 3 );
        if ( status != GSC_OK && status != GSC_NOT_FOUND ) {
            G_Printf( "GSC: error in CodeCallback_PlayerDamage (status %d)\n",
                      status );
        }
    }
}

#endif /* STANDALONE */
