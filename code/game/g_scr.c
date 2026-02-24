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

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
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
static int          g_scrPlayerProxyIdx;
static int          g_scrHudElemProxyIdx;

/* Per-client: raw pointer to the GSC object so we can push it later.
   NULL if no object exists for this slot.                                  */
static void        *g_scrPlayerObjPtrs[ MAX_CLIENTS ];
static qboolean     g_scrEntityObjAlive[ MAX_GENTITIES ];

#define G_SCR_MAX_HUDELEMS 128

typedef enum {
    G_SCR_HUD_SCOPE_ALL,
    G_SCR_HUD_SCOPE_CLIENT,
    G_SCR_HUD_SCOPE_TEAM
} G_ScrHudScope_t;

typedef struct {
    qboolean      inuse;
    int           id;      /* 1..G_SCR_MAX_HUDELEMS */
    G_ScrHudScope_t scope;
    int           owner;   /* clientNum for CLIENT, team_t for TEAM */
    float         x;
    float         y;
    float         scale;
    float         color[ 4 ];
    int           width;
    int           height;
    char          text[ 128 ];
    char          shader[ MAX_QPATH ];
} G_ScrHudElem_t;

static G_ScrHudElem_t g_scrHudElems[ G_SCR_MAX_HUDELEMS ];

/* The GSC file namespace (path without .gsc) where CodeCallback_* live.   */
static char         g_scrCallbackFile[ MAX_QPATH ];

/* Local forward declaration from g_spawn.c (not exposed in g_local.h). */
qboolean G_CallSpawn( gentity_t *ent );
static void G_Scr_CreatePlayerObj( int clientNum );

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

static void G_Scr_NormalizePathSlashes( char *path )
{
    int i;

    for ( i = 0; path[i]; i++ ) {
        if ( path[i] == '\\' ) {
            path[i] = '/';
        }
    }
}

static void G_Scr_NormalizeMapName( const char *rawMapName, char *out, int outSize )
{
    char        cleaned[ MAX_QPATH ];
    char        noExt[ MAX_QPATH ];
    const char *name;

    if ( !rawMapName ) {
        out[0] = '\0';
        return;
    }

    Q_strncpyz( cleaned, rawMapName, sizeof( cleaned ) );
    G_Scr_NormalizePathSlashes( cleaned );

    name = cleaned;
    if ( !Q_stricmpn( name, "maps/", 5 ) ) {
        name += 5;
    }
    if ( !Q_stricmpn( name, "mp/", 3 ) ) {
        name += 3;
    }

    Q_strncpyz( out, name, outSize );

    if ( !Q_stricmp( COM_GetExtension( out ), "bsp" ) ) {
        COM_StripExtension( out, noExt, sizeof( noExt ) );
        Q_strncpyz( out, noExt, outSize );
    }
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
    char         altPath[ MAX_QPATH ];
    char        *buf;
    ScrSrc_t    *hdr;

    (void)ctx;

    /* Append .gsc if the caller didn't already include an extension */
    Q_strncpyz( path, filename, sizeof( path ) );
    G_Scr_NormalizePathSlashes( path );
    if ( !COM_GetExtension( path )[0] ) {
        Q_strcat( path, sizeof( path ), ".gsc" );
    }

    len = trap_FS_FOpenFile( path, &fh, FS_READ );

    if ( ( len <= 0 || !fh ) && !strncmp( path, "maps/mp/", 8 ) ) {
        Com_sprintf( altPath, sizeof( altPath ), "maps/MP/%s", path + 8 );
        len = trap_FS_FOpenFile( altPath, &fh, FS_READ );
        if ( len > 0 && fh ) {
            Q_strncpyz( path, altPath, sizeof( path ) );
        }
    } else if ( ( len <= 0 || !fh ) && !strncmp( path, "maps/MP/", 8 ) ) {
        Com_sprintf( altPath, sizeof( altPath ), "maps/mp/%s", path + 8 );
        len = trap_FS_FOpenFile( altPath, &fh, FS_READ );
        if ( len > 0 && fh ) {
            Q_strncpyz( path, altPath, sizeof( path ) );
        }
    }

    if ( len <= 0 || !fh ) {
        int i;
        Q_strncpyz( altPath, path, sizeof( altPath ) );
        for ( i = 0; altPath[i]; i++ ) {
            altPath[i] = (char)tolower( (unsigned char)altPath[i] );
        }
        len = trap_FS_FOpenFile( altPath, &fh, FS_READ );
        if ( len > 0 && fh ) {
            Q_strncpyz( path, altPath, sizeof( path ) );
        }
    }

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
    const char *failedName;

    failedName = nameSpace;
    status = gsc_compile( g_scrCtx, nameSpace, 0 );

    while ( status == GSC_OK &&
            ( dep = gsc_next_compile_dependency( g_scrCtx ) ) != NULL ) {
        failedName = dep;
        status = gsc_compile( g_scrCtx, dep, 0 );
    }

    if ( status != GSC_OK ) {
        G_Printf( "GSC: compile failed for '%s' (status %d)\n",
                  failedName, status );
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

static qboolean G_Scr_GetClientNumForEntity( const gentity_t *ent, int *outClientNum )
{
    int clientNum;

    if ( !ent || !ent->client ) {
        return qfalse;
    }

    clientNum = (int)( ent - g_entities );
    if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
        return qfalse;
    }

    if ( outClientNum ) {
        *outClientNum = clientNum;
    }
    return qtrue;
}

static const char *G_Scr_TeamToString( team_t team )
{
    switch ( team ) {
        case TEAM_RED:       return "allies";
        case TEAM_BLUE:      return "axis";
        case TEAM_FREE:      return "none";
        case TEAM_SPECTATOR: return "spectator";
        default:             return "spectator";
    }
}

static gentity_t *G_Scr_GetEntityFromArg( gsc_Context *ctx, int arg )
{
    int type = gsc_get_type( ctx, arg );

    if ( type == GSC_TYPE_OBJECT ) {
        int obj = gsc_get_object( ctx, arg );
        if ( obj >= 0 ) {
            return (gentity_t *)gsc_object_get_userdata( ctx, obj );
        }
        return NULL;
    }

    if ( type == GSC_TYPE_INTEGER ) {
        int entNum = (int)gsc_get_int( ctx, arg );
        if ( entNum >= 0 && entNum < level.num_entities ) {
            return &g_entities[ entNum ];
        }
    }

    return NULL;
}

static char *G_Scr_CopyString( const char *src )
{
    size_t len;
    char  *dst;

    if ( !src ) {
        src = "";
    }

    len = strlen( src );
    dst = (char *)G_Alloc( (int)len + 1 );
    if ( !dst ) {
        return NULL;
    }

    memcpy( dst, src, len + 1 );
    return dst;
}

static qboolean G_Scr_GetEntityNum( const gentity_t *ent, int *outEntNum )
{
    int entNum;

    if ( !ent ) {
        return qfalse;
    }

    entNum = (int)( ent - g_entities );
    if ( entNum < 0 || entNum >= MAX_GENTITIES ) {
        return qfalse;
    }

    if ( outEntNum ) {
        *outEntNum = entNum;
    }
    return qtrue;
}

static void G_Scr_GetEntityGlobalName( int entNum, char *name, int nameSize )
{
    if ( entNum >= 0 && entNum < MAX_CLIENTS ) {
        Com_sprintf( name, nameSize, "player_%d", entNum );
    } else {
        Com_sprintf( name, nameSize, "entity_%d", entNum );
    }
}

static void G_Scr_ClearGlobal( const char *name )
{
    gsc_add_int( g_scrCtx, 0 ); /* undefined-equivalent in this VM */
    gsc_set_global( g_scrCtx, name );
}

static void G_Scr_SetEntityObjectFields( gsc_Context *ctx, int entObj, gentity_t *ent )
{
    int entNum = -1;

    if ( !ent ) {
        return;
    }

    G_Scr_GetEntityNum( ent, &entNum );

    gsc_add_string( ctx, ent->classname ? ent->classname : "" );
    gsc_object_set_field( ctx, entObj, "classname" );
    gsc_add_string( ctx, ent->targetname ? ent->targetname : "" );
    gsc_object_set_field( ctx, entObj, "targetname" );
    gsc_add_string( ctx, ent->target ? ent->target : "" );
    gsc_object_set_field( ctx, entObj, "target" );
    gsc_add_string( ctx, ent->model ? ent->model : "" );
    gsc_object_set_field( ctx, entObj, "model" );

    gsc_add_vec3( ctx, ent->r.currentOrigin );
    gsc_object_set_field( ctx, entObj, "origin" );
    gsc_add_vec3( ctx, ent->s.angles );
    gsc_object_set_field( ctx, entObj, "angles" );

    gsc_add_int( ctx, entNum );
    gsc_object_set_field( ctx, entObj, "entitynum" );
}

static void G_Scr_CreateEntityObject( gentity_t *ent, qboolean keepOnStack )
{
    char globalName[ 32 ];
    int  entObj;
    int  entNum;

    if ( !g_scrCtx || !ent || !G_Scr_GetEntityNum( ent, &entNum ) ) {
        return;
    }

    entObj = gsc_add_tagged_object( g_scrCtx, "#entity" );
    if ( entNum < MAX_CLIENTS ) {
        gsc_object_set_proxy( g_scrCtx, entObj, g_scrPlayerProxyIdx );
    } else {
        gsc_object_set_proxy( g_scrCtx, entObj, g_scrEntityProxyIdx );
    }
    gsc_object_set_userdata( g_scrCtx, entObj, ent );
    G_Scr_SetEntityObjectFields( g_scrCtx, entObj, ent );

    if ( entNum < MAX_CLIENTS ) {
        g_scrPlayerObjPtrs[ entNum ] = gsc_get_ptr( g_scrCtx, entObj );
    } else {
        g_scrEntityObjAlive[ entNum ] = qtrue;
    }

    G_Scr_GetEntityGlobalName( entNum, globalName, sizeof( globalName ) );
    gsc_set_global( g_scrCtx, globalName ); /* pops entObj */

    if ( keepOnStack ) {
        if ( entNum < MAX_CLIENTS && g_scrPlayerObjPtrs[ entNum ] ) {
            gsc_push_object( g_scrCtx, g_scrPlayerObjPtrs[ entNum ] );
        } else {
            gsc_get_global( g_scrCtx, globalName );
        }
    }
}

static void G_Scr_PushEntityObject( gsc_Context *ctx, gentity_t *ent )
{
    char globalName[ 32 ];
    int  entObj;
    int  entNum;
    int clientNum;
    int topBefore;

    if ( !ent || !G_Scr_GetEntityNum( ent, &entNum ) ) {
        return;
    }

    if ( G_Scr_GetClientNumForEntity( ent, &clientNum ) &&
         g_scrPlayerObjPtrs[ clientNum ] ) {
        topBefore = gsc_top( ctx );
        gsc_push_object( ctx, g_scrPlayerObjPtrs[ clientNum ] );
        if ( gsc_top( ctx ) > topBefore && gsc_type( ctx, -1 ) == GSC_TYPE_OBJECT ) {
            entObj = gsc_top( ctx ) - 1;
            gsc_object_set_userdata( ctx, entObj, ent );
            G_Scr_SetEntityObjectFields( ctx, entObj, ent );
            return;
        }
    }

    if ( entNum >= MAX_CLIENTS && g_scrEntityObjAlive[ entNum ] ) {
        topBefore = gsc_top( ctx );
        G_Scr_GetEntityGlobalName( entNum, globalName, sizeof( globalName ) );
        gsc_get_global( ctx, globalName );
        if ( gsc_top( ctx ) > topBefore && gsc_type( ctx, -1 ) == GSC_TYPE_OBJECT ) {
            entObj = gsc_top( ctx ) - 1;
            gsc_object_set_userdata( ctx, entObj, ent );
            G_Scr_SetEntityObjectFields( ctx, entObj, ent );
            return;
        }
        if ( gsc_top( ctx ) > topBefore ) {
            gsc_pop( ctx, 1 );
        }
        g_scrEntityObjAlive[ entNum ] = qfalse;
    }

    G_Scr_CreateEntityObject( ent, qtrue );
}

static G_ScrHudElem_t *G_Scr_Hud_Alloc( void )
{
    int i;

    for ( i = 0; i < G_SCR_MAX_HUDELEMS; i++ ) {
        G_ScrHudElem_t *hud = &g_scrHudElems[ i ];
        if ( hud->inuse ) {
            continue;
        }

        Com_Memset( hud, 0, sizeof( *hud ) );
        hud->inuse = qtrue;
        hud->id = i + 1;
        hud->scope = G_SCR_HUD_SCOPE_ALL;
        hud->owner = -1;
        hud->scale = 0.35f;
        hud->color[0] = 1.0f;
        hud->color[1] = 1.0f;
        hud->color[2] = 1.0f;
        hud->color[3] = 1.0f;
        return hud;
    }

    return NULL;
}

static void G_Scr_PushHudElemObject( gsc_Context *ctx, G_ScrHudScope_t scope, int owner )
{
    float          white[3] = { 1.0f, 1.0f, 1.0f };
    G_ScrHudElem_t *hud;
    int            obj;

    hud = G_Scr_Hud_Alloc();

    obj = gsc_add_tagged_object( ctx, "#hudelem" );
    gsc_object_set_proxy( ctx, obj, g_scrHudElemProxyIdx );

    if ( hud ) {
        hud->scope = scope;
        hud->owner = owner;
        gsc_object_set_userdata( ctx, obj, (void *)(intptr_t)hud->id );
    } else {
        /* Safe fallback: still return an object so scripts don't crash on field writes. */
        gsc_object_set_userdata( ctx, obj, NULL );
    }

    gsc_add_float( ctx, hud ? hud->x : 0.0f );
    gsc_object_set_field( ctx, obj, "x" );
    gsc_add_float( ctx, hud ? hud->y : 0.0f );
    gsc_object_set_field( ctx, obj, "y" );
    gsc_add_float( ctx, hud ? hud->scale : 0.35f );
    gsc_object_set_field( ctx, obj, "fontscale" );
    gsc_add_vec3( ctx, white );
    gsc_object_set_field( ctx, obj, "color" );
    gsc_add_float( ctx, hud ? hud->color[3] : 1.0f );
    gsc_object_set_field( ctx, obj, "alpha" );
}

static int G_Scr_NoopReturn0( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

static int G_Scr_NoopReturn1( gsc_Context *ctx )
{
    (void)ctx;
    gsc_add_int( ctx, 1 );
    return 1;
}

static int G_Scr_NoopFalse( gsc_Context *ctx )
{
    (void)ctx;
    gsc_add_bool( ctx, qfalse );
    return 1;
}

static int G_Scr_NoopNoneString( gsc_Context *ctx )
{
    (void)ctx;
    gsc_add_string( ctx, "none" );
    return 1;
}

static int G_Scr_NoopZero( gsc_Context *ctx )
{
    (void)ctx;
    gsc_add_int( ctx, 0 );
    return 1;
}

static qboolean G_Scr_ClassnameMatches( const char *wanted, const char *actual )
{
    if ( !wanted || !wanted[0] || !actual || !actual[0] ) {
        return qfalse;
    }

    if ( !Q_stricmp( wanted, actual ) ) {
        return qtrue;
    }

    /*
     * CoD MP spawn entities are remapped to Q3 spawn classes by the gamecode.
     * Keep script lookups working by aliasing CoD class requests.
     */
    if ( !Q_stricmp( actual, "info_player_deathmatch" ) &&
         !Q_stricmpn( wanted, "mp_", 3 ) &&
         Q_stristr( wanted, "_spawn" ) ) {
        return qtrue;
    }

    if ( !Q_stricmp( actual, "info_player_intermission" ) &&
         !Q_stricmpn( wanted, "mp_", 3 ) &&
         Q_stristr( wanted, "intermission" ) ) {
        return qtrue;
    }

    return qfalse;
}

static qboolean G_Scr_EntityMatchesFilter( gentity_t *ent, const char *value, const char *key )
{
    if ( !ent || !ent->inuse || !value || !value[0] ) {
        return qfalse;
    }

    if ( !key || !key[0] || !Q_stricmp( key, "classname" ) ) {
        return G_Scr_ClassnameMatches( value, ent->classname );
    }

    if ( !Q_stricmp( key, "targetname" ) ) {
        return ent->targetname && !Q_stricmp( ent->targetname, value );
    }

    if ( !Q_stricmp( key, "target" ) ) {
        return ent->target && !Q_stricmp( ent->target, value );
    }

    if ( !Q_stricmp( key, "model" ) ) {
        return ent->model && !Q_stricmp( ent->model, value );
    }

    return qfalse;
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
    if ( !ent ) {
        float zero[3] = { 0.0f, 0.0f, 0.0f };
        gsc_add_vec3( ctx, zero );
        return 1;
    }
    gsc_add_vec3( ctx, ent->r.currentOrigin );
    return 1;
}

static int GScr_Meth_SetOrigin( gsc_Context *ctx )
{
    float     v[3];
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent ) {
        return 0;
    }
    gsc_get_vec3( ctx, 0, v );
    if ( ent->client ) {
        VectorCopy( v, ent->client->ps.origin );
    }
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
    gsc_add_string( ctx, G_Scr_TeamToString( ent->client->sess.sessionTeam ) );
    return 1;
}

static int GScr_Meth_GetClientNum( gsc_Context *ctx )
{
    int clientNum;
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( G_Scr_GetClientNumForEntity( ent, &clientNum ) ) {
        gsc_add_int( ctx, clientNum );
    } else {
        gsc_add_int( ctx, ent ? ent->s.number : -1 );
    }
    return 1;
}

static int GScr_Meth_GetEntityNumber( gsc_Context *ctx )
{
    return GScr_Meth_GetClientNum( ctx );
}

static int GScr_Meth_GetGuid( gsc_Context *ctx )
{
    int clientNum;
    gentity_t *ent = G_Scr_GetSelf( ctx );

    if ( !G_Scr_GetClientNumForEntity( ent, &clientNum ) ) {
        gsc_add_int( ctx, -1 );
        return 1;
    }

    /*
     * ioq3 does not expose CoD-style integer GUIDs in gamecode.
     * Return stable client slot as best-effort compatibility value.
     */
    gsc_add_int( ctx, clientNum );
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
    vec3_t spawnOrigin;
    vec3_t spawnAngles;
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent && ent->client &&
         ent->client->pers.connected == CON_CONNECTED ) {
        ClientSpawn( ent );
        if ( gsc_numargs( ctx ) >= 2 &&
             gsc_get_type( ctx, 0 ) == GSC_TYPE_VECTOR &&
             gsc_get_type( ctx, 1 ) == GSC_TYPE_VECTOR ) {
            gsc_get_vec3( ctx, 0, spawnOrigin );
            gsc_get_vec3( ctx, 1, spawnAngles );
            TeleportPlayer( ent, spawnOrigin, spawnAngles );
        }
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

/* CoD map setup helper used by gametype scripts on spawnpoint entities. */
static int GScr_Meth_PlaceSpawnpoint( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent ) {
        return 0;
    }

    VectorCopy( ent->s.origin, ent->s.pos.trBase );
    VectorCopy( ent->s.origin, ent->r.currentOrigin );
    trap_LinkEntity( ent );
    return 0;
}

static int GScr_Meth_Delete( gsc_Context *ctx )
{
    int entNum;
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent ) {
        return 0;
    }

    entNum = (int)( ent - g_entities );
    if ( entNum >= MAX_CLIENTS && ent->inuse ) {
        G_FreeEntity( ent );
    }
    return 0;
}

static int GScr_Meth_Hide( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent ) {
        ent->r.svFlags |= SVF_NOCLIENT;
        trap_LinkEntity( ent );
    }
    return 0;
}

static int GScr_Meth_Show( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent ) {
        ent->r.svFlags &= ~SVF_NOCLIENT;
        trap_LinkEntity( ent );
    }
    return 0;
}

static int GScr_Meth_NotSolid( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent ) {
        ent->r.contents = 0;
        ent->clipmask = 0;
        trap_LinkEntity( ent );
    }
    return 0;
}

static int GScr_Meth_Solid( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent ) {
        ent->r.contents = CONTENTS_SOLID;
        ent->clipmask = MASK_SOLID;
        trap_LinkEntity( ent );
    }
    return 0;
}

static int GScr_Meth_SetModel( gsc_Context *ctx )
{
    const char *model = gsc_get_string( ctx, 0 );
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent || !model || !model[0] ) {
        return 0;
    }

    ent->model = G_Scr_CopyString( model );
    if ( model[0] == '*' ) {
        trap_SetBrushModel( ent, model );
    }
    trap_LinkEntity( ent );
    return 0;
}

static int GScr_Meth_IsTouching( gsc_Context *ctx )
{
    gentity_t *self = G_Scr_GetSelf( ctx );
    gentity_t *other = G_Scr_GetEntityFromArg( ctx, 0 );
    gsc_add_bool( ctx,
                  self && other &&
                  BoundsIntersect( self->r.absmin, self->r.absmax,
                                   other->r.absmin, other->r.absmax ) );
    return 1;
}

static int GScr_Meth_SetClientCvar( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

static int GScr_Meth_OpenMenu( gsc_Context *ctx )
{
    (void)ctx;
    gsc_add_int( ctx, 1 );
    return 1;
}

static int GScr_Meth_CloseMenu( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

static int GScr_Meth_CloseInGameMenu( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

static int GScr_Meth_AttackButtonPressed( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_bool( ctx, ent && ent->client && ( ent->client->buttons & BUTTON_ATTACK ) );
    return 1;
}

static int GScr_Meth_UseButtonPressed( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_bool( ctx, ent && ent->client && ( ent->client->buttons & BUTTON_USE_HOLDABLE ) );
    return 1;
}

static int GScr_Meth_MeleeButtonPressed( gsc_Context *ctx )
{
    return GScr_Meth_AttackButtonPressed( ctx );
}

static int GScr_Meth_PlayerADS( gsc_Context *ctx )
{
    (void)ctx;
    gsc_add_bool( ctx, qfalse );
    return 1;
}

static int GScr_Meth_IsOnGround( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_bool( ctx, ent && ent->client &&
                  ent->client->ps.groundEntityNum != ENTITYNUM_NONE );
    return 1;
}

static int GScr_Meth_GiveWeapon( gsc_Context *ctx )          { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_TakeWeapon( gsc_Context *ctx )          { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_GiveMaxAmmo( gsc_Context *ctx )         { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_GiveStartAmmo( gsc_Context *ctx )       { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_SetSpawnWeapon( gsc_Context *ctx )      { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_SetWeaponSlotWeapon( gsc_Context *ctx ) { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_SetWeaponSlotAmmo( gsc_Context *ctx )   { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_SetWeaponSlotClipAmmo( gsc_Context *ctx ){ return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_SetWeaponClipAmmo( gsc_Context *ctx )   { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_GetWeaponSlotAmmo( gsc_Context *ctx )   { return G_Scr_NoopZero( ctx ); }
static int GScr_Meth_GetWeaponSlotClipAmmo( gsc_Context *ctx ){ return G_Scr_NoopZero( ctx ); }
static int GScr_Meth_GetWeaponSlotWeapon( gsc_Context *ctx ) { return G_Scr_NoopNoneString( ctx ); }
static int GScr_Meth_GetCurrentWeapon( gsc_Context *ctx )    { return G_Scr_NoopNoneString( ctx ); }
static int GScr_Meth_GetCurrentOffhand( gsc_Context *ctx )   { return G_Scr_NoopNoneString( ctx ); }
static int GScr_Meth_HasWeapon( gsc_Context *ctx )           { return G_Scr_NoopFalse( ctx ); }
static int GScr_Meth_SwitchToWeapon( gsc_Context *ctx )      { return G_Scr_NoopReturn1( ctx ); }
static int GScr_Meth_SwitchToOffhand( gsc_Context *ctx )     { return G_Scr_NoopReturn1( ctx ); }
static int GScr_Meth_SetClientDvar( gsc_Context *ctx )       { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_FreezeControls( gsc_Context *ctx )      { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_DisableWeapon( gsc_Context *ctx )       { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_EnableWeapon( gsc_Context *ctx )        { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_SetEnterTime( gsc_Context *ctx )        { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_PingPlayer( gsc_Context *ctx )          { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_SayAll( gsc_Context *ctx )              { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_SayTeam( gsc_Context *ctx )             { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_SetViewModel( gsc_Context *ctx )        { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_GetViewModel( gsc_Context *ctx )        { return G_Scr_NoopNoneString( ctx ); }
static int GScr_Meth_Attach( gsc_Context *ctx )              { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_Detach( gsc_Context *ctx )              { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_DetachAll( gsc_Context *ctx )           { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_PlaySound( gsc_Context *ctx )           { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_PlayLocalSound( gsc_Context *ctx )      { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_PlayLoopSound( gsc_Context *ctx )       { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_StopLoopSound( gsc_Context *ctx )       { return G_Scr_NoopReturn0( ctx ); }

static int GScr_Meth_ClonePlayer( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent && ent->client ) {
        CopyToBodyQue( ent );
    }
    return 0;
}

static int GScr_Meth_DropItem( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

/*
 * CoD callback scripts call finishPlayerDamage(), but ioq3 already applies
 * damage before G_Scr_PlayerDamage is fired. Keep this as a no-op so damage
 * does not get applied twice.
 */
static int GScr_Meth_FinishPlayerDamage( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

static int GScr_Meth_AllowSpectateTeam( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

static void G_Scr_Hud_SanitizeServerString( const char *src, char *dst, int dstSize )
{
    int i, j;

    if ( !dst || dstSize <= 0 ) {
        return;
    }

    if ( !src ) {
        dst[0] = '\0';
        return;
    }

    for ( i = 0, j = 0; src[i] && j < dstSize - 1; i++ ) {
        unsigned char ch = (unsigned char)src[i];
        if ( ch == '"' || ch == '\\' || ch == '\n' || ch == '\r' ) {
            dst[j++] = ' ';
            continue;
        }
        if ( ch < 32 ) {
            continue;
        }
        dst[j++] = (char)ch;
    }
    dst[j] = '\0';
}

static qboolean G_Scr_Hud_ClientMatches( const G_ScrHudElem_t *hud, int clientNum )
{
    if ( !hud || !hud->inuse ) {
        return qfalse;
    }

    if ( clientNum < 0 || clientNum >= level.maxclients ) {
        return qfalse;
    }

    if ( level.clients[ clientNum ].pers.connected != CON_CONNECTED ) {
        return qfalse;
    }

    switch ( hud->scope ) {
        case G_SCR_HUD_SCOPE_ALL:
            return qtrue;
        case G_SCR_HUD_SCOPE_CLIENT:
            return hud->owner == clientNum;
        case G_SCR_HUD_SCOPE_TEAM:
            return (int)level.clients[ clientNum ].sess.sessionTeam == hud->owner;
        default:
            return qfalse;
    }
}

static void G_Scr_Hud_SendUpdateToClient( const G_ScrHudElem_t *hud, int clientNum )
{
    char safeText[ sizeof( hud->text ) ];
    char safeShader[ sizeof( hud->shader ) ];

    if ( !hud ) {
        return;
    }

    G_Scr_Hud_SanitizeServerString( hud->text, safeText, sizeof( safeText ) );
    G_Scr_Hud_SanitizeServerString( hud->shader, safeShader, sizeof( safeShader ) );

    trap_SendServerCommand(
        clientNum,
        va( "scr_hud set %d %.3f %.3f %.3f %.3f %.3f %.3f %.3f %d %d \"%s\" \"%s\"",
            hud->id,
            hud->x,
            hud->y,
            hud->scale,
            hud->color[0], hud->color[1], hud->color[2], hud->color[3],
            hud->width, hud->height,
            safeText, safeShader ) );
}

static void G_Scr_Hud_SendUpdate( const G_ScrHudElem_t *hud )
{
    int i;

    if ( !hud || !hud->inuse ) {
        return;
    }

    if ( hud->scope == G_SCR_HUD_SCOPE_ALL ) {
        G_Scr_Hud_SendUpdateToClient( hud, -1 );
        return;
    }

    for ( i = 0; i < level.maxclients; i++ ) {
        if ( G_Scr_Hud_ClientMatches( hud, i ) ) {
            G_Scr_Hud_SendUpdateToClient( hud, i );
        }
    }
}

static void G_Scr_Hud_SendDelete( const G_ScrHudElem_t *hud )
{
    int i;

    if ( !hud ) {
        return;
    }

    if ( hud->scope == G_SCR_HUD_SCOPE_ALL ) {
        trap_SendServerCommand( -1, va( "scr_hud del %d", hud->id ) );
        return;
    }

    for ( i = 0; i < level.maxclients; i++ ) {
        if ( G_Scr_Hud_ClientMatches( hud, i ) ) {
            trap_SendServerCommand( i, va( "scr_hud del %d", hud->id ) );
        }
    }
}

static void G_Scr_Hud_SendAllToClient( int clientNum )
{
    int i;

    if ( clientNum < 0 || clientNum >= level.maxclients ) {
        return;
    }

    for ( i = 0; i < G_SCR_MAX_HUDELEMS; i++ ) {
        G_ScrHudElem_t *hud = &g_scrHudElems[ i ];
        if ( G_Scr_Hud_ClientMatches( hud, clientNum ) ) {
            G_Scr_Hud_SendUpdateToClient( hud, clientNum );
        }
    }
}

static void G_Scr_Hud_ResetAll( qboolean broadcast )
{
    Com_Memset( g_scrHudElems, 0, sizeof( g_scrHudElems ) );
    if ( broadcast ) {
        trap_SendServerCommand( -1, "scr_hud reset" );
    }
}

static G_ScrHudElem_t *G_Scr_Hud_FindById( int id )
{
    if ( id < 1 || id > G_SCR_MAX_HUDELEMS ) {
        return NULL;
    }

    if ( !g_scrHudElems[ id - 1 ].inuse ) {
        return NULL;
    }

    return &g_scrHudElems[ id - 1 ];
}

static G_ScrHudElem_t *G_Scr_Hud_FromSelf( gsc_Context *ctx, int *outSelfObjIdx )
{
    int             selfObjIdx;
    intptr_t        rawId;
    G_ScrHudElem_t *hud;

    selfObjIdx = gsc_get_object( ctx, -1 );
    if ( selfObjIdx < 0 ) {
        return NULL;
    }

    rawId = (intptr_t)gsc_object_get_userdata( ctx, selfObjIdx );
    if ( rawId <= 0 ) {
        return NULL;
    }

    hud = G_Scr_Hud_FindById( (int)rawId );
    if ( !hud ) {
        return NULL;
    }

    if ( outSelfObjIdx ) {
        *outSelfObjIdx = selfObjIdx;
    }

    return hud;
}

static void G_Scr_Hud_ReadFloatField( gsc_Context *ctx, int objIdx, const char *name, float *value )
{
    gsc_object_get_field( ctx, objIdx, name );
    if ( gsc_type( ctx, -1 ) != GSC_TYPE_UNDEFINED ) {
        *value = gsc_to_float( ctx, -1 );
    }
    gsc_pop( ctx, 1 );
}

static void G_Scr_Hud_ReadVec3Field( gsc_Context *ctx, int objIdx, const char *name, float out[3] )
{
    gsc_object_get_field( ctx, objIdx, name );
    if ( gsc_type( ctx, -1 ) == GSC_TYPE_VECTOR ) {
        float       x, y, z;
        const char *vecStr = gsc_to_string( ctx, -1 );
        if ( vecStr &&
             ( sscanf( vecStr, " ( %f , %f , %f ) ", &x, &y, &z ) == 3 ||
               sscanf( vecStr, "(%f,%f,%f)", &x, &y, &z ) == 3 ) ) {
            out[0] = x;
            out[1] = y;
            out[2] = z;
        }
    }
    gsc_pop( ctx, 1 );
}

static void G_Scr_Hud_ReadCommonFields( gsc_Context *ctx, int selfObjIdx, G_ScrHudElem_t *hud )
{
    float dim;

    if ( !hud ) {
        return;
    }

    G_Scr_Hud_ReadFloatField( ctx, selfObjIdx, "x", &hud->x );
    G_Scr_Hud_ReadFloatField( ctx, selfObjIdx, "y", &hud->y );
    G_Scr_Hud_ReadFloatField( ctx, selfObjIdx, "fontscale", &hud->scale );
    G_Scr_Hud_ReadFloatField( ctx, selfObjIdx, "scale", &hud->scale );
    G_Scr_Hud_ReadFloatField( ctx, selfObjIdx, "alpha", &hud->color[3] );
    G_Scr_Hud_ReadVec3Field( ctx, selfObjIdx, "color", hud->color );

    dim = (float)hud->width;
    G_Scr_Hud_ReadFloatField( ctx, selfObjIdx, "width", &dim );
    hud->width = (int)dim;
    dim = (float)hud->height;
    G_Scr_Hud_ReadFloatField( ctx, selfObjIdx, "height", &dim );
    hud->height = (int)dim;

    if ( hud->scale <= 0.0f ) {
        hud->scale = 0.35f;
    }
    if ( hud->color[3] < 0.0f ) {
        hud->color[3] = 0.0f;
    } else if ( hud->color[3] > 1.0f ) {
        hud->color[3] = 1.0f;
    }
}

static int GScr_Meth_Hud_SetValue( gsc_Context *ctx );

static int GScr_Meth_Hud_SetText( gsc_Context *ctx )
{
    int             selfObjIdx;
    const char     *text;
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, &selfObjIdx );

    if ( !hud ) {
        return 0;
    }

    text = ( gsc_numargs( ctx ) > 0 ) ? gsc_get_string( ctx, 0 ) : "";
    Q_strncpyz( hud->text, text ? text : "", sizeof( hud->text ) );
    G_Scr_Hud_ReadCommonFields( ctx, selfObjIdx, hud );
    G_Scr_Hud_SendUpdate( hud );
    return 0;
}

static int GScr_Meth_Hud_SetShader( gsc_Context *ctx )
{
    int             selfObjIdx;
    const char     *shader;
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, &selfObjIdx );

    if ( !hud ) {
        return 0;
    }

    shader = ( gsc_numargs( ctx ) > 0 ) ? gsc_get_string( ctx, 0 ) : "";
    Q_strncpyz( hud->shader, shader ? shader : "", sizeof( hud->shader ) );

    if ( gsc_numargs( ctx ) > 1 ) {
        hud->width = (int)gsc_get_float( ctx, 1 );
    }
    if ( gsc_numargs( ctx ) > 2 ) {
        hud->height = (int)gsc_get_float( ctx, 2 );
    }

    G_Scr_Hud_ReadCommonFields( ctx, selfObjIdx, hud );
    G_Scr_Hud_SendUpdate( hud );
    return 0;
}

static int GScr_Meth_Hud_SetTimer( gsc_Context *ctx )
{
    return GScr_Meth_Hud_SetValue( ctx );
}

static int GScr_Meth_Hud_SetTimerUp( gsc_Context *ctx )
{
    return GScr_Meth_Hud_SetValue( ctx );
}

static int GScr_Meth_Hud_SetTenthsTimer( gsc_Context *ctx )
{
    return GScr_Meth_Hud_SetValue( ctx );
}

static int GScr_Meth_Hud_SetValue( gsc_Context *ctx )
{
    int             selfObjIdx;
    int             value = 0;
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, &selfObjIdx );

    if ( !hud ) {
        return 0;
    }

    if ( gsc_numargs( ctx ) > 0 ) {
        if ( gsc_get_type( ctx, 0 ) == GSC_TYPE_FLOAT ) {
            value = (int)gsc_get_float( ctx, 0 );
        } else {
            value = (int)gsc_get_int( ctx, 0 );
        }
        Com_sprintf( hud->text, sizeof( hud->text ), "%d", value );
    } else {
        hud->text[0] = '\0';
    }

    G_Scr_Hud_ReadCommonFields( ctx, selfObjIdx, hud );
    G_Scr_Hud_SendUpdate( hud );
    return 0;
}

static int GScr_Meth_Hud_FadeOverTime( gsc_Context *ctx )
{
    int             selfObjIdx;
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, &selfObjIdx );

    if ( !hud ) {
        return 0;
    }

    G_Scr_Hud_ReadCommonFields( ctx, selfObjIdx, hud );
    G_Scr_Hud_SendUpdate( hud );
    return 0;
}

static int GScr_Meth_Hud_MoveOverTime( gsc_Context *ctx )
{
    int             selfObjIdx;
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, &selfObjIdx );

    if ( !hud ) {
        return 0;
    }

    G_Scr_Hud_ReadCommonFields( ctx, selfObjIdx, hud );
    G_Scr_Hud_SendUpdate( hud );
    return 0;
}

static int GScr_Meth_Hud_Destroy( gsc_Context *ctx )
{
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, NULL );

    if ( !hud ) {
        return 0;
    }

    G_Scr_Hud_SendDelete( hud );
    Com_Memset( hud, 0, sizeof( *hud ) );
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

static int GScr_Fn_GetDvarFloat( gsc_Context *ctx )
{
    const char *name = gsc_get_string( ctx, 0 );
    if ( !name ) name = "";
    gsc_add_float( ctx, trap_Cvar_VariableValue( name ) );
    return 1;
}

static int GScr_Fn_MakeCvarServerInfo( gsc_Context *ctx )
{
    const char *name = gsc_get_string( ctx, 0 );
    const char *defaultValue = ( gsc_numargs( ctx ) > 1 ) ? gsc_get_string( ctx, 1 ) : "";
    char        cur[ 256 ];

    if ( name && name[0] ) {
        trap_Cvar_VariableStringBuffer( name, cur, sizeof( cur ) );
        if ( !cur[0] ) {
            trap_Cvar_Set( name, defaultValue ? defaultValue : "" );
        }
    }
    return 0;
}

static int GScr_Fn_SetArchive( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

static int GScr_Fn_LogPrint( gsc_Context *ctx )
{
    int i, n = gsc_numargs( ctx );
    for ( i = 0; i < n; i++ ) {
        G_LogPrintf( "%s", gsc_get_string( ctx, i ) );
    }
    return 0;
}

static int GScr_Fn_IsPlayerGlobal( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetEntityFromArg( ctx, 0 );
    gsc_add_bool( ctx, ent && ent->client &&
                  ent->client->pers.connected != CON_DISCONNECTED );
    return 1;
}

static int GScr_Fn_IsAliveGlobal( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetEntityFromArg( ctx, 0 );
    gsc_add_bool( ctx, ent && ent->health > 0 );
    return 1;
}

static int G_Scr_Hud_ParseTeamArg( gsc_Context *ctx, int arg )
{
    int         type;
    const char *team;

    if ( gsc_numargs( ctx ) <= arg ) {
        return TEAM_FREE;
    }

    type = gsc_get_type( ctx, arg );
    if ( type == GSC_TYPE_INTEGER ) {
        int teamNum = (int)gsc_get_int( ctx, arg );
        if ( teamNum >= TEAM_FREE && teamNum <= TEAM_SPECTATOR ) {
            return teamNum;
        }
        return TEAM_FREE;
    }

    team = gsc_get_string( ctx, arg );
    if ( !team || !team[0] ) {
        return TEAM_FREE;
    }

    if ( !Q_stricmp( team, "allies" ) || !Q_stricmp( team, "red" ) ) {
        return TEAM_RED;
    }
    if ( !Q_stricmp( team, "axis" ) || !Q_stricmp( team, "blue" ) ) {
        return TEAM_BLUE;
    }
    if ( !Q_stricmp( team, "spectator" ) ) {
        return TEAM_SPECTATOR;
    }
    return TEAM_FREE;
}

static int GScr_Fn_NewHudElem( gsc_Context *ctx )
{
    int topBefore = gsc_top( ctx );
    G_Scr_PushHudElemObject( ctx, G_SCR_HUD_SCOPE_ALL, -1 );
    return ( gsc_top( ctx ) > topBefore ) ? 1 : 0;
}

static int GScr_Fn_NewClientHudElem( gsc_Context *ctx )
{
    int       topBefore = gsc_top( ctx );
    int       clientNum = -1;
    gentity_t *ownerEnt = NULL;

    if ( gsc_numargs( ctx ) > 0 ) {
        ownerEnt = G_Scr_GetEntityFromArg( ctx, 0 );
    }
    if ( !ownerEnt ) {
        ownerEnt = G_Scr_GetSelf( ctx );
    }

    if ( G_Scr_GetClientNumForEntity( ownerEnt, &clientNum ) ) {
        G_Scr_PushHudElemObject( ctx, G_SCR_HUD_SCOPE_CLIENT, clientNum );
    } else {
        /* CoD scripts may call this without owner during setup; keep it non-fatal. */
        G_Scr_PushHudElemObject( ctx, G_SCR_HUD_SCOPE_ALL, -1 );
    }
    return ( gsc_top( ctx ) > topBefore ) ? 1 : 0;
}

static int GScr_Fn_NewTeamHudElem( gsc_Context *ctx )
{
    int topBefore = gsc_top( ctx );
    int team = G_Scr_Hud_ParseTeamArg( ctx, 0 );
    G_Scr_PushHudElemObject( ctx, G_SCR_HUD_SCOPE_TEAM, team );
    return ( gsc_top( ctx ) > topBefore ) ? 1 : 0;
}

static int GScr_Fn_PositionWouldTelefrag( gsc_Context *ctx )
{
    trace_t tr;
    vec3_t  origin;
    vec3_t  mins = { -15.0f, -15.0f, -24.0f };
    vec3_t  maxs = {  15.0f,  15.0f,  32.0f };

    gsc_get_vec3( ctx, 0, origin );
    trap_Trace( &tr, origin, mins, maxs, origin, ENTITYNUM_NONE, MASK_PLAYERSOLID );
    gsc_add_bool( ctx, tr.startsolid || tr.allsolid );
    return 1;
}

static int GScr_Fn_Distance( gsc_Context *ctx )
{
    vec3_t a, b, d;
    gsc_get_vec3( ctx, 0, a );
    gsc_get_vec3( ctx, 1, b );
    VectorSubtract( a, b, d );
    gsc_add_float( ctx, VectorLength( d ) );
    return 1;
}

static int GScr_Fn_Length( gsc_Context *ctx )
{
    vec3_t v;
    gsc_get_vec3( ctx, 0, v );
    gsc_add_float( ctx, VectorLength( v ) );
    return 1;
}

static int GScr_Fn_LengthSquared( gsc_Context *ctx )
{
    vec3_t v;
    gsc_get_vec3( ctx, 0, v );
    gsc_add_float( ctx, DotProduct( v, v ) );
    return 1;
}

static int GScr_Fn_VectorNormalize( gsc_Context *ctx )
{
    vec3_t v;
    gsc_get_vec3( ctx, 0, v );
    VectorNormalize( v );
    gsc_add_vec3( ctx, v );
    return 1;
}

static int GScr_Fn_VectorScale( gsc_Context *ctx )
{
    vec3_t v, out;
    float  s = gsc_get_float( ctx, 1 );
    gsc_get_vec3( ctx, 0, v );
    VectorScale( v, s, out );
    gsc_add_vec3( ctx, out );
    return 1;
}

static int GScr_Fn_VectorToAngles( gsc_Context *ctx )
{
    vec3_t v, a;
    gsc_get_vec3( ctx, 0, v );
    vectoangles( v, a );
    gsc_add_vec3( ctx, a );
    return 1;
}

static int GScr_Fn_AnglesToForward( gsc_Context *ctx )
{
    vec3_t a, fwd;
    gsc_get_vec3( ctx, 0, a );
    AngleVectors( a, fwd, NULL, NULL );
    gsc_add_vec3( ctx, fwd );
    return 1;
}

static int GScr_Fn_SpawnStruct( gsc_Context *ctx )
{
    (void)ctx;
    gsc_add_object( ctx );
    return 1;
}

static int GScr_Fn_Spawn( gsc_Context *ctx )
{
    const char *classname = gsc_get_string( ctx, 0 );
    vec3_t      origin = { 0.0f, 0.0f, 0.0f };
    gentity_t  *ent = G_Spawn();

    if ( !ent ) {
        return 0;
    }

    if ( gsc_numargs( ctx ) > 1 && gsc_get_type( ctx, 1 ) == GSC_TYPE_VECTOR ) {
        gsc_get_vec3( ctx, 1, origin );
    }

    if ( classname && classname[0] ) {
        ent->classname = G_Scr_CopyString( classname );
    } else {
        ent->classname = G_Scr_CopyString( "script_model" );
    }

    VectorCopy( origin, ent->s.origin );
    VectorCopy( origin, ent->s.pos.trBase );
    VectorCopy( origin, ent->r.currentOrigin );

    if ( !G_CallSpawn( ent ) ) {
        trap_LinkEntity( ent );
    }

    if ( !ent->inuse ) {
        return 0;
    }

    G_Scr_PushEntityObject( ctx, ent );
    return 1;
}

static int GScr_Fn_MapRestart( gsc_Context *ctx )
{
    (void)ctx;
    trap_SendConsoleCommand( EXEC_APPEND, "map_restart 0\n" );
    return 0;
}

static int GScr_Fn_SetClientNameMode( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

static int GScr_Fn_Obituary( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

static int GScr_Fn_MapExists( gsc_Context *ctx )
{
    char        path[ MAX_QPATH ];
    char        mapName[ MAX_QPATH ];
    fileHandle_t fh;
    int          len;
    const char  *name = gsc_get_string( ctx, 0 );

    if ( !name || !name[0] ) {
        gsc_add_bool( ctx, qfalse );
        return 1;
    }

    G_Scr_NormalizeMapName( name, mapName, sizeof( mapName ) );
    Com_sprintf( path, sizeof( path ), "maps/%s.bsp", mapName );

    len = trap_FS_FOpenFile( path, &fh, FS_READ );
    if ( fh ) {
        trap_FS_FCloseFile( fh );
    }

    if ( len <= 0 ) {
        Com_sprintf( path, sizeof( path ), "maps/mp/%s.bsp", mapName );
        len = trap_FS_FOpenFile( path, &fh, FS_READ );
        if ( fh ) {
            trap_FS_FCloseFile( fh );
        }
    }

    gsc_add_bool( ctx, len > 0 );
    return 1;
}

static int GScr_Fn_Announcement( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_ClientAnnouncement( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_UpdateScores( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_AddTestClient( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_PrecacheMenu( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_PrecacheStatusIcon( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_PrecacheHeadIcon( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_PrecacheItem( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_PrecacheModel( gsc_Context *ctx )
{
    const char *model = gsc_get_string( ctx, 0 );

    if ( model && model[0] ) {
        G_ModelIndex( (char *)model );
    }
    return 0;
}
static int GScr_Fn_PrecacheShader( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_PrecacheString( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_PrecacheTurret( gsc_Context *ctx ) { (void)ctx; return 0; }

/* gettime() — returns level time in seconds */
static int GScr_Fn_GetTime( gsc_Context *ctx )
{
    gsc_add_float( ctx, (float)level.time * 0.001f );
    return 1;
}

/* getent() — supports getent(clientNum) and getent(value, key) */
static int GScr_Fn_GetEnt( gsc_Context *ctx )
{
    char globalName[ 32 ];
    int  clientNum;
    int  topBefore;
    int  i;
    const char *value;
    const char *key;

    if ( gsc_get_type( ctx, 0 ) == GSC_TYPE_INTEGER ) {
        clientNum = (int)gsc_get_int( ctx, 0 );
        if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
            return 0; /* undefined */
        }

        if ( !g_scrPlayerObjPtrs[ clientNum ] ) {
            G_Scr_CreatePlayerObj( clientNum );
        }

        topBefore = gsc_top( g_scrCtx );
        if ( g_scrPlayerObjPtrs[ clientNum ] ) {
            gsc_push_object( g_scrCtx, g_scrPlayerObjPtrs[ clientNum ] );
        }
        if ( gsc_top( g_scrCtx ) <= topBefore || gsc_type( g_scrCtx, -1 ) != GSC_TYPE_OBJECT ) {
            /* Fallback: retrieve via the named global */
            Com_sprintf( globalName, sizeof( globalName ), "player_%d", clientNum );
            gsc_get_global( g_scrCtx, globalName );
            if ( gsc_type( g_scrCtx, -1 ) != GSC_TYPE_OBJECT ) {
                gsc_pop( g_scrCtx, 1 );
                return 0;
            }
        }
        return 1;
    }

    if ( gsc_numargs( ctx ) < 1 ) {
        return 0;
    }

    value = gsc_get_string( ctx, 0 );
    key = ( gsc_numargs( ctx ) > 1 ) ? gsc_get_string( ctx, 1 ) : "targetname";

    for ( i = 0; i < level.num_entities; i++ ) {
        gentity_t *ent = &g_entities[ i ];
        if ( G_Scr_EntityMatchesFilter( ent, value, key ) ) {
            G_Scr_PushEntityObject( ctx, ent );
            return 1;
        }
    }

    return 0; /* undefined */
}

/* getentarray(value, key) — returns array-like object of matching entities */
static int GScr_Fn_GetEntArray( gsc_Context *ctx )
{
    int arrObj;
    int i;
    int count = 0;
    char idxKey[ 32 ];
    const char *value = NULL;
    const char *key = "classname";
    qboolean filter = qfalse;

    arrObj = gsc_add_object( ctx );

    if ( gsc_numargs( ctx ) > 0 ) {
        value = gsc_get_string( ctx, 0 );
        key = ( gsc_numargs( ctx ) > 1 ) ? gsc_get_string( ctx, 1 ) : "classname";
        filter = ( value && value[0] );
    }

    for ( i = 0; i < level.num_entities; i++ ) {
        gentity_t *ent = &g_entities[ i ];
        if ( !ent->inuse ) {
            continue;
        }
        if ( filter && !G_Scr_EntityMatchesFilter( ent, value, key ) ) {
            continue;
        }

        G_Scr_PushEntityObject( ctx, ent );
        Com_sprintf( idxKey, sizeof( idxKey ), "%d", count );
        gsc_object_set_field( ctx, arrObj, idxKey );
        count++;
    }
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
    gsc_register_function( g_scrCtx, NULL, "distance",     GScr_Fn_Distance );
    gsc_register_function( g_scrCtx, NULL, "length",       GScr_Fn_Length );
    gsc_register_function( g_scrCtx, NULL, "lengthsquared", GScr_Fn_LengthSquared );
    gsc_register_function( g_scrCtx, NULL, "vectornormalize", GScr_Fn_VectorNormalize );
    gsc_register_function( g_scrCtx, NULL, "vectorscale",  GScr_Fn_VectorScale );
    gsc_register_function( g_scrCtx, NULL, "vectortoangles", GScr_Fn_VectorToAngles );
    gsc_register_function( g_scrCtx, NULL, "anglestoforward", GScr_Fn_AnglesToForward );

    /* Cvar / dvar — register both spellings since they're different names */
    gsc_register_function( g_scrCtx, NULL, "getdvar",      GScr_Fn_GetDvar );
    gsc_register_function( g_scrCtx, NULL, "getdvarint",   GScr_Fn_GetDvarInt );
    gsc_register_function( g_scrCtx, NULL, "getdvarfloat", GScr_Fn_GetDvarFloat );
    gsc_register_function( g_scrCtx, NULL, "setdvar",      GScr_Fn_SetDvar );
    gsc_register_function( g_scrCtx, NULL, "getcvar",      GScr_Fn_GetCvar );   /* CoD1 alias */
    gsc_register_function( g_scrCtx, NULL, "getcvarint",   GScr_Fn_GetDvarInt );
    gsc_register_function( g_scrCtx, NULL, "getcvarfloat", GScr_Fn_GetDvarFloat );
    gsc_register_function( g_scrCtx, NULL, "setcvar",      GScr_Fn_SetCvar );   /* CoD1 alias */
    gsc_register_function( g_scrCtx, NULL, "makecvarserverinfo", GScr_Fn_MakeCvarServerInfo );

    /* Time */
    gsc_register_function( g_scrCtx, NULL, "gettime",      GScr_Fn_GetTime );

    /* Entity access */
    gsc_register_function( g_scrCtx, NULL, "getent",       GScr_Fn_GetEnt );
    gsc_register_function( g_scrCtx, NULL, "getentarray",  GScr_Fn_GetEntArray );
    gsc_register_function( g_scrCtx, NULL, "spawn",        GScr_Fn_Spawn );
    gsc_register_function( g_scrCtx, NULL, "spawnstruct",  GScr_Fn_SpawnStruct );
    gsc_register_function( g_scrCtx, NULL, "positionwouldtelefrag", GScr_Fn_PositionWouldTelefrag );

    /* Player counts */
    gsc_register_function( g_scrCtx, NULL, "getmaxplayers", GScr_Fn_GetMaxPlayers );
    gsc_register_function( g_scrCtx, NULL, "getnumplayers", GScr_Fn_GetNumPlayers );
    gsc_register_function( g_scrCtx, NULL, "isplayer",     GScr_Fn_IsPlayerGlobal );
    gsc_register_function( g_scrCtx, NULL, "isalive",      GScr_Fn_IsAliveGlobal );

    /* Level control */
    gsc_register_function( g_scrCtx, NULL, "exitlevel",    GScr_Fn_ExitLevel );
    gsc_register_function( g_scrCtx, NULL, "maprestart",   GScr_Fn_MapRestart );
    gsc_register_function( g_scrCtx, NULL, "mapexists",    GScr_Fn_MapExists );
    gsc_register_function( g_scrCtx, NULL, "setarchive",   GScr_Fn_SetArchive );
    gsc_register_function( g_scrCtx, NULL, "setclientnamemode", GScr_Fn_SetClientNameMode );
    gsc_register_function( g_scrCtx, NULL, "obituary",     GScr_Fn_Obituary );
    gsc_register_function( g_scrCtx, NULL, "logprint",     GScr_Fn_LogPrint );
    gsc_register_function( g_scrCtx, NULL, "announcement", GScr_Fn_Announcement );
    gsc_register_function( g_scrCtx, NULL, "clientannouncement", GScr_Fn_ClientAnnouncement );
    gsc_register_function( g_scrCtx, NULL, "updatescores", GScr_Fn_UpdateScores );
    gsc_register_function( g_scrCtx, NULL, "addtestclient", GScr_Fn_AddTestClient );

    /* Client-side / audio stubs (no-ops on the server) */
    gsc_register_function( g_scrCtx, NULL, "ambientplay",  GScr_Fn_AmbientPlay );
    gsc_register_function( g_scrCtx, NULL, "ambientstop",  GScr_Fn_AmbientStop );
    gsc_register_function( g_scrCtx, NULL, "setcullfog",   GScr_Fn_SetCullFog );
    gsc_register_function( g_scrCtx, NULL, "precachemenu", GScr_Fn_PrecacheMenu );
    gsc_register_function( g_scrCtx, NULL, "precachestatusicon", GScr_Fn_PrecacheStatusIcon );
    gsc_register_function( g_scrCtx, NULL, "precacheheadicon", GScr_Fn_PrecacheHeadIcon );
    gsc_register_function( g_scrCtx, NULL, "precacheitem", GScr_Fn_PrecacheItem );
    gsc_register_function( g_scrCtx, NULL, "precachemodel", GScr_Fn_PrecacheModel );
    gsc_register_function( g_scrCtx, NULL, "precacheshader", GScr_Fn_PrecacheShader );
    gsc_register_function( g_scrCtx, NULL, "precachestring", GScr_Fn_PrecacheString );
    gsc_register_function( g_scrCtx, NULL, "precacheturret", GScr_Fn_PrecacheTurret );

    /* HUD element constructors */
    gsc_register_function( g_scrCtx, NULL, "newhudelem",        GScr_Fn_NewHudElem );
    gsc_register_function( g_scrCtx, NULL, "newclienthudelem",  GScr_Fn_NewClientHudElem );
    gsc_register_function( g_scrCtx, NULL, "newteamhudelem",    GScr_Fn_NewTeamHudElem );
}

/* =========================================================================
   Create the shared entity proxy and the level/game/anim globals.

   Stack layout after this function (permanent — never popped):
     [0] = shared entity proxy object  (g_scrEntityProxyIdx)

   Globals set:
     level, game, anim  (tagged empty objects, CoD compat)
     self               (initially = level object)
   ========================================================================= */
static void G_Scr_AddMethod( int methodsObj, const char *name, gsc_Function fn )
{
    gsc_add_function( g_scrCtx, fn );
    gsc_object_set_field( g_scrCtx, methodsObj, name );
}

static void G_Scr_CreateGlobals( void )
{
    int entProxyObj;
    int entMethodsObj;
    int playerProxyObj;
    int playerMethodsObj;
    int hudProxyObj;
    int hudMethodsObj;
    int levelObj;

    /* ---- base entity proxy ---- */
    entProxyObj = gsc_add_tagged_object( g_scrCtx, "#ent_proxy" );
    g_scrEntityProxyIdx = entProxyObj;

    entMethodsObj = gsc_add_object( g_scrCtx );
    G_Scr_AddMethod( entMethodsObj, "getname",         GScr_Meth_GetName );
    G_Scr_AddMethod( entMethodsObj, "getorigin",       GScr_Meth_GetOrigin );
    G_Scr_AddMethod( entMethodsObj, "setorigin",       GScr_Meth_SetOrigin );
    G_Scr_AddMethod( entMethodsObj, "gethealth",       GScr_Meth_GetHealth );
    G_Scr_AddMethod( entMethodsObj, "sethealth",       GScr_Meth_SetHealth );
    G_Scr_AddMethod( entMethodsObj, "getteam",         GScr_Meth_GetTeam );
    G_Scr_AddMethod( entMethodsObj, "getclientnum",    GScr_Meth_GetClientNum );
    G_Scr_AddMethod( entMethodsObj, "getentitynumber", GScr_Meth_GetEntityNumber );
    G_Scr_AddMethod( entMethodsObj, "isplayer",        GScr_Meth_IsPlayer );
    G_Scr_AddMethod( entMethodsObj, "isalive",         GScr_Meth_IsAlive );
    G_Scr_AddMethod( entMethodsObj, "spawn",           GScr_Meth_Spawn );
    G_Scr_AddMethod( entMethodsObj, "suicide",         GScr_Meth_Suicide );
    G_Scr_AddMethod( entMethodsObj, "placespawnpoint", GScr_Meth_PlaceSpawnpoint );
    G_Scr_AddMethod( entMethodsObj, "delete",          GScr_Meth_Delete );
    G_Scr_AddMethod( entMethodsObj, "hide",            GScr_Meth_Hide );
    G_Scr_AddMethod( entMethodsObj, "show",            GScr_Meth_Show );
    G_Scr_AddMethod( entMethodsObj, "notsolid",        GScr_Meth_NotSolid );
    G_Scr_AddMethod( entMethodsObj, "solid",           GScr_Meth_Solid );
    G_Scr_AddMethod( entMethodsObj, "setmodel",        GScr_Meth_SetModel );
    G_Scr_AddMethod( entMethodsObj, "istouching",      GScr_Meth_IsTouching );
    G_Scr_AddMethod( entMethodsObj, "attach",          GScr_Meth_Attach );
    G_Scr_AddMethod( entMethodsObj, "detach",          GScr_Meth_Detach );
    G_Scr_AddMethod( entMethodsObj, "detachall",       GScr_Meth_DetachAll );
    G_Scr_AddMethod( entMethodsObj, "playsound",       GScr_Meth_PlaySound );
    G_Scr_AddMethod( entMethodsObj, "playlocalsound",  GScr_Meth_PlayLocalSound );
    G_Scr_AddMethod( entMethodsObj, "playloopsound",   GScr_Meth_PlayLoopSound );
    G_Scr_AddMethod( entMethodsObj, "stoploopsound",   GScr_Meth_StopLoopSound );
    gsc_object_set_field( g_scrCtx, entProxyObj, "__call" );

    /* ---- player proxy (inherits from ent proxy) ---- */
    playerProxyObj = gsc_add_tagged_object( g_scrCtx, "#player_proxy" );
    g_scrPlayerProxyIdx = playerProxyObj;
    gsc_object_set_proxy( g_scrCtx, playerProxyObj, entProxyObj );

    playerMethodsObj = gsc_add_object( g_scrCtx );
    G_Scr_AddMethod( playerMethodsObj, "getguid",               GScr_Meth_GetGuid );
    G_Scr_AddMethod( playerMethodsObj, "setclientcvar",         GScr_Meth_SetClientCvar );
    G_Scr_AddMethod( playerMethodsObj, "openmenu",              GScr_Meth_OpenMenu );
    G_Scr_AddMethod( playerMethodsObj, "closemenu",             GScr_Meth_CloseMenu );
    G_Scr_AddMethod( playerMethodsObj, "closeingamemenu",       GScr_Meth_CloseInGameMenu );
    G_Scr_AddMethod( playerMethodsObj, "attackbuttonpressed",   GScr_Meth_AttackButtonPressed );
    G_Scr_AddMethod( playerMethodsObj, "usebuttonpressed",      GScr_Meth_UseButtonPressed );
    G_Scr_AddMethod( playerMethodsObj, "meleebuttonpressed",    GScr_Meth_MeleeButtonPressed );
    G_Scr_AddMethod( playerMethodsObj, "playerads",             GScr_Meth_PlayerADS );
    G_Scr_AddMethod( playerMethodsObj, "isonground",            GScr_Meth_IsOnGround );
    G_Scr_AddMethod( playerMethodsObj, "giveweapon",            GScr_Meth_GiveWeapon );
    G_Scr_AddMethod( playerMethodsObj, "takeweapon",            GScr_Meth_TakeWeapon );
    G_Scr_AddMethod( playerMethodsObj, "givemaxammo",           GScr_Meth_GiveMaxAmmo );
    G_Scr_AddMethod( playerMethodsObj, "givestartammo",         GScr_Meth_GiveStartAmmo );
    G_Scr_AddMethod( playerMethodsObj, "setspawnweapon",        GScr_Meth_SetSpawnWeapon );
    G_Scr_AddMethod( playerMethodsObj, "setweaponslotweapon",   GScr_Meth_SetWeaponSlotWeapon );
    G_Scr_AddMethod( playerMethodsObj, "setweaponslotammo",     GScr_Meth_SetWeaponSlotAmmo );
    G_Scr_AddMethod( playerMethodsObj, "setweaponslotclipammo", GScr_Meth_SetWeaponSlotClipAmmo );
    G_Scr_AddMethod( playerMethodsObj, "setweaponclipammo",     GScr_Meth_SetWeaponClipAmmo );
    G_Scr_AddMethod( playerMethodsObj, "getweaponslotweapon",   GScr_Meth_GetWeaponSlotWeapon );
    G_Scr_AddMethod( playerMethodsObj, "getweaponslotammo",     GScr_Meth_GetWeaponSlotAmmo );
    G_Scr_AddMethod( playerMethodsObj, "getweaponslotclipammo", GScr_Meth_GetWeaponSlotClipAmmo );
    G_Scr_AddMethod( playerMethodsObj, "getcurrentweapon",      GScr_Meth_GetCurrentWeapon );
    G_Scr_AddMethod( playerMethodsObj, "getcurrentoffhand",     GScr_Meth_GetCurrentOffhand );
    G_Scr_AddMethod( playerMethodsObj, "hasweapon",             GScr_Meth_HasWeapon );
    G_Scr_AddMethod( playerMethodsObj, "switchtoweapon",        GScr_Meth_SwitchToWeapon );
    G_Scr_AddMethod( playerMethodsObj, "switchtooffhand",       GScr_Meth_SwitchToOffhand );
    G_Scr_AddMethod( playerMethodsObj, "setclientdvar",         GScr_Meth_SetClientDvar );
    G_Scr_AddMethod( playerMethodsObj, "freezecontrols",        GScr_Meth_FreezeControls );
    G_Scr_AddMethod( playerMethodsObj, "disableweapon",         GScr_Meth_DisableWeapon );
    G_Scr_AddMethod( playerMethodsObj, "enableweapon",          GScr_Meth_EnableWeapon );
    G_Scr_AddMethod( playerMethodsObj, "setentertime",          GScr_Meth_SetEnterTime );
    G_Scr_AddMethod( playerMethodsObj, "pingplayer",            GScr_Meth_PingPlayer );
    G_Scr_AddMethod( playerMethodsObj, "sayall",                GScr_Meth_SayAll );
    G_Scr_AddMethod( playerMethodsObj, "sayteam",               GScr_Meth_SayTeam );
    G_Scr_AddMethod( playerMethodsObj, "setviewmodel",          GScr_Meth_SetViewModel );
    G_Scr_AddMethod( playerMethodsObj, "getviewmodel",          GScr_Meth_GetViewModel );
    G_Scr_AddMethod( playerMethodsObj, "cloneplayer",           GScr_Meth_ClonePlayer );
    G_Scr_AddMethod( playerMethodsObj, "dropitem",              GScr_Meth_DropItem );
    G_Scr_AddMethod( playerMethodsObj, "finishplayerdamage",    GScr_Meth_FinishPlayerDamage );
    G_Scr_AddMethod( playerMethodsObj, "allowspectateteam",     GScr_Meth_AllowSpectateTeam );
    gsc_object_set_field( g_scrCtx, playerProxyObj, "__call" );

    /* ---- HUD element proxy ---- */
    hudProxyObj = gsc_add_tagged_object( g_scrCtx, "#hudelem_proxy" );
    g_scrHudElemProxyIdx = hudProxyObj;

    hudMethodsObj = gsc_add_object( g_scrCtx );
    G_Scr_AddMethod( hudMethodsObj, "settext",       GScr_Meth_Hud_SetText );
    G_Scr_AddMethod( hudMethodsObj, "setshader",     GScr_Meth_Hud_SetShader );
    G_Scr_AddMethod( hudMethodsObj, "settimer",      GScr_Meth_Hud_SetTimer );
    G_Scr_AddMethod( hudMethodsObj, "settimerup",    GScr_Meth_Hud_SetTimerUp );
    G_Scr_AddMethod( hudMethodsObj, "settenthstimer", GScr_Meth_Hud_SetTenthsTimer );
    G_Scr_AddMethod( hudMethodsObj, "setvalue",      GScr_Meth_Hud_SetValue );
    G_Scr_AddMethod( hudMethodsObj, "fadeovertime",  GScr_Meth_Hud_FadeOverTime );
    G_Scr_AddMethod( hudMethodsObj, "moveovertime",  GScr_Meth_Hud_MoveOverTime );
    G_Scr_AddMethod( hudMethodsObj, "destroy",       GScr_Meth_Hud_Destroy );
    G_Scr_AddMethod( hudMethodsObj, "delete",        GScr_Meth_Hud_Destroy );
    gsc_object_set_field( g_scrCtx, hudProxyObj, "__call" );

    /* ---- level object (CoD compat global) ---- */
    levelObj = gsc_add_tagged_object( g_scrCtx, "#level" );
    gsc_object_set_proxy( g_scrCtx, levelObj, entProxyObj );
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
}

/* =========================================================================
   Create and register a GSC entity object for a connected client.
   ========================================================================= */
static void G_Scr_CreatePlayerObj( int clientNum )
{
    if ( !g_scrCtx || clientNum < 0 || clientNum >= MAX_CLIENTS ) {
        return;
    }
    G_Scr_CreateEntityObject( &g_entities[ clientNum ], qfalse );
}

/* =========================================================================
   Swap the GSC "self" global to the given player entity object.
   Must be called before any callback that needs self = player.
   ========================================================================= */
static void G_Scr_SetSelfToPlayer( int clientNum )
{
    char globalName[ 32 ];
    int  topBefore;

    if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
        return;
    }

    if ( !g_scrPlayerObjPtrs[ clientNum ] ) {
        G_Scr_CreatePlayerObj( clientNum );
    }
    if ( !g_scrPlayerObjPtrs[ clientNum ] ) {
        return;
    }

    Com_sprintf( globalName, sizeof( globalName ), "player_%d", clientNum );

    topBefore = gsc_top( g_scrCtx );
    gsc_get_global( g_scrCtx, globalName );

    if ( gsc_top( g_scrCtx ) > topBefore ) {
        if ( gsc_type( g_scrCtx, -1 ) == GSC_TYPE_OBJECT ) {
            gsc_set_global( g_scrCtx, "self" );
        } else {
            gsc_pop( g_scrCtx, 1 );
        }
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
    const char       *mapnameRaw;
    char              mapname[ MAX_QPATH ];
    char              gametype[ 16 ];
    char              gametypeScript[ MAX_QPATH ];  /* maps/mp/gametypes/<gt>    */
    char              callbackScript[ MAX_QPATH ];  /* maps/mp/gametypes/_cbsetup */
    char              mapScript[ MAX_QPATH ];       /* maps/mp/<mapname>          */
    char              mapScriptSP[ MAX_QPATH ];     /* maps/<mapname> (SP fallback) */
    int               i;
    int               status;
    qboolean          gametypeOk;
    qboolean          callbackOk;
    qboolean          mapOk;

    /* Reset state */
    g_scrCtx     = NULL;
    g_scrActive  = qfalse;
    g_scrSources = NULL;
    Com_Memset( g_scrPlayerObjPtrs, 0, sizeof( g_scrPlayerObjPtrs ) );
    Com_Memset( g_scrEntityObjAlive, 0, sizeof( g_scrEntityObjAlive ) );
    G_Scr_Hud_ResetAll( qfalse );
    g_scrCallbackFile[0] = '\0';

    /* Determine map name and gametype */
    trap_GetServerinfo( serverinfo, sizeof( serverinfo ) );
    mapnameRaw = Info_ValueForKey( serverinfo, "mapname" );
    G_Scr_NormalizeMapName( mapnameRaw, mapname, sizeof( mapname ) );
    if ( !mapname[0] ) {
        G_Printf( "GSC: no mapname in serverinfo, scripting disabled\n" );
        return;
    }

    G_Scr_GetGametypeString( gametype, sizeof( gametype ) );

    /*
     Script namespaces (no .gsc extension — the read_file callback adds it).
     CoD1 MP maps live under  maps/mp/<mapname>.gsc
     CoD1 SP maps live under  maps/<mapname>.gsc
     Gametypes live under     maps/mp/gametypes/<gametype>.gsc
     _callbacksetup is at     maps/mp/gametypes/_callbacksetup.gsc
                                                                             */
    Com_sprintf( gametypeScript, sizeof( gametypeScript ),
                 "maps/mp/gametypes/%s", gametype );
    Com_sprintf( callbackScript, sizeof( callbackScript ),
                 "maps/mp/gametypes/_callbacksetup" );
    Com_sprintf( mapScript,      sizeof( mapScript ),
                 "maps/mp/%s",   mapname );
    Com_sprintf( mapScriptSP,    sizeof( mapScriptSP ),
                 "maps/%s",      mapname );

    G_Printf( "GSC: map '%s' (raw '%s')  gametype '%s'\n",
              mapname, mapnameRaw ? mapnameRaw : "", gametype );

    /* Create context */
    Com_Memset( &opts, 0, sizeof( opts ) );
    opts.allocate_memory          = G_Scr_AllocMem;
    opts.free_memory              = G_Scr_FreeMem;
    opts.read_file                = G_Scr_ReadFile;
    opts.verbose                  = 0;
    opts.main_memory_size         = 96 * 1024 * 1024; /* 96 MB */
    opts.temp_memory_size         = 16 * 1024 * 1024; /* 16 MB */
    opts.string_table_memory_size = 8  * 1024 * 1024; /*  8 MB */
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
    if ( gametypeOk && callbackOk ) {
        Q_strncpyz( g_scrCallbackFile, callbackScript,
                    sizeof( g_scrCallbackFile ) );
    } else if ( gametypeOk ) {
        Q_strncpyz( g_scrCallbackFile, gametypeScript,
                    sizeof( g_scrCallbackFile ) );
    } else if ( mapOk ) {
        Q_strncpyz( g_scrCallbackFile, mapScript,
                    sizeof( g_scrCallbackFile ) );
    } else {
        g_scrCallbackFile[0] = '\0';
    }

    if ( !gametypeOk ) {
        G_Printf( "GSC: gametype '%s' unavailable; CodeCallback_* hooks disabled\n",
                  gametypeScript );
        g_scrCallbackFile[0] = '\0';
    }

    g_scrActive = qtrue;
    if ( g_scrCallbackFile[0] ) {
        G_Printf( "GSC: scripting active  callbacks in '%s'\n", g_scrCallbackFile );
    } else {
        G_Printf( "GSC: scripting active  callbacks disabled\n" );
    }

    /* Fresh HUD state per map and stable object handles for all live entities. */
    G_Scr_Hud_ResetAll( qtrue );
    for ( i = 0; i < level.num_entities; i++ ) {
        if ( g_entities[ i ].inuse ) {
            G_Scr_CreateEntityObject( &g_entities[ i ], qfalse );
        }
    }

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

    if ( g_scrCallbackFile[0] ) {
        G_Scr_ExecCallback( "CodeCallback_StartGameType" );
        while ( gsc_update( g_scrCtx, 0.0f ) == GSC_YIELD ) {}
    }
}

/* =========================================================================
   G_Scr_Shutdown  —  called at the start of G_ShutdownGame
   ========================================================================= */
void G_Scr_Shutdown( void )
{
    if ( g_scrActive ) {
        G_Scr_Hud_ResetAll( qtrue );
    }

    if ( g_scrCtx ) {
        gsc_destroy( g_scrCtx );
        g_scrCtx = NULL;
    }
    G_Scr_FreeSources();
    Com_Memset( g_scrPlayerObjPtrs, 0, sizeof( g_scrPlayerObjPtrs ) );
    Com_Memset( g_scrEntityObjAlive, 0, sizeof( g_scrEntityObjAlive ) );
    Com_Memset( g_scrHudElems, 0, sizeof( g_scrHudElems ) );
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
   Entity lifecycle callbacks
   ========================================================================= */

void G_Scr_EntitySpawned( gentity_t *ent )
{
    int entNum;

    if ( !g_scrActive || !g_scrCtx || !ent ) {
        return;
    }

    if ( !G_Scr_GetEntityNum( ent, &entNum ) || entNum < MAX_CLIENTS ) {
        return;
    }

    G_Scr_CreateEntityObject( ent, qfalse );
}

void G_Scr_EntityFreed( gentity_t *ent )
{
    char globalName[ 32 ];
    int  entNum;

    if ( !g_scrCtx || !ent ) {
        return;
    }

    if ( !G_Scr_GetEntityNum( ent, &entNum ) || entNum < MAX_CLIENTS ) {
        return;
    }

    if ( !g_scrEntityObjAlive[ entNum ] ) {
        return;
    }

    G_Scr_GetEntityGlobalName( entNum, globalName, sizeof( globalName ) );
    G_Scr_ClearGlobal( globalName );
    g_scrEntityObjAlive[ entNum ] = qfalse;
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
    G_Scr_Hud_SendAllToClient( clientNum );
    G_Scr_ExecPlayerCallback( clientNum, "CodeCallback_PlayerConnect" );
}

void G_Scr_PlayerDisconnect( int clientNum )
{
    char globalName[ 32 ];
    int  i;

    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }

    G_Scr_ExecPlayerCallback( clientNum, "CodeCallback_PlayerDisconnect" );

    /* Advance any disconnect-triggered threads */
    gsc_update( g_scrCtx, 0.0f );

    /* Clear the player global so the object can be GC'd */
    Com_sprintf( globalName, sizeof( globalName ), "player_%d", clientNum );
    G_Scr_ClearGlobal( globalName );

    g_scrPlayerObjPtrs[ clientNum ] = NULL;

    for ( i = 0; i < G_SCR_MAX_HUDELEMS; i++ ) {
        G_ScrHudElem_t *hud = &g_scrHudElems[ i ];
        if ( hud->inuse &&
             hud->scope == G_SCR_HUD_SCOPE_CLIENT &&
             hud->owner == clientNum ) {
            Com_Memset( hud, 0, sizeof( *hud ) );
        }
    }
}

void G_Scr_PlayerSpawn( int clientNum )
{
    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }

    if ( !g_scrPlayerObjPtrs[ clientNum ] ) {
        G_Scr_CreatePlayerObj( clientNum );
    }

    /* Update the userdata pointer/fields in case the entity was re-initialised. */
    if ( g_scrPlayerObjPtrs[ clientNum ] ) {
        char globalName[ 32 ];
        int  topBefore, entObjIdx;

        Com_sprintf( globalName, sizeof( globalName ), "player_%d", clientNum );
        topBefore = gsc_top( g_scrCtx );
        gsc_get_global( g_scrCtx, globalName );
        if ( gsc_top( g_scrCtx ) > topBefore ) {
            if ( gsc_type( g_scrCtx, -1 ) == GSC_TYPE_OBJECT ) {
                entObjIdx = gsc_top( g_scrCtx ) - 1;
                gsc_object_set_userdata( g_scrCtx, entObjIdx,
                                         &g_entities[ clientNum ] );
                G_Scr_SetEntityObjectFields( g_scrCtx, entObjIdx, &g_entities[ clientNum ] );
            }
            gsc_pop( g_scrCtx, 1 );
        }
    }

    G_Scr_Hud_SendAllToClient( clientNum );
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
