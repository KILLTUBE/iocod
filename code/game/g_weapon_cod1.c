/*
===========================================================================
g_weapon_cod1.c  --  Server-side CoD1 weapon entities and melee

Handles:
  - Spawning weapon_* entities from CoD1 maps as pickupable world models
  - Sending pickup notification to the client (reliable "weapon" command)
  - Server-side melee damage trace (G_MeleeDamage)

CoD1 map entities look like:
  "classname" "weapon_mp44_mp"     or "weapon_mp44"
  "classname" "mpweapon_kar98k_mp" or "mpweapon_colt"
  "origin"    "512 -128 64"
  "angles"    "0 90 0"

The weapon name is derived from the classname by stripping the leading
"weapon_" or "mpweapon_" prefix.
===========================================================================
*/
#ifdef STANDALONE

#include "g_local.h"
#include "../qcommon/bg_weapon_cod1.h"

/* Respawn time in seconds; 0 = do not respawn */
#define WEAPON_RESPAWN_TIME   20

/* Weapon name is re-derived from classname each time (classname pointer persists) */
static const char *WeaponNameFromClass( const char *classname )
{
    if ( !Q_stricmpn( classname, "mpweapon_", 9 ) ) return classname + 9;
    if ( !Q_stricmpn( classname, "weapon_",   7 ) ) return classname + 7;
    return classname;
}

/*
 * G_WeaponRespawn -- re-enable a weapon entity after respawn delay
 */
static void G_WeaponRespawn( gentity_t *ent )
{
    ent->r.svFlags &= ~SVF_NOCLIENT;
    ent->r.contents = CONTENTS_TRIGGER;
    ent->nextthink  = 0;
    ent->think      = NULL;
    trap_LinkEntity( ent );
}

/*
 * Touch_WeaponCod1 -- player walks into a weapon pickup trigger
 */
static void Touch_WeaponCod1( gentity_t *ent, gentity_t *other, trace_t *trace )
{
    gclient_t  *client;
    const char *weapName;
    int         clipAmmo, reserveAmmo;

    if ( !other || !other->client ) return;
    client = other->client;

    /* Only alive players can pick up weapons */
    if ( client->ps.pm_type != PM_NORMAL ) return;

    weapName    = WeaponNameFromClass( ent->classname );
    clipAmmo    = ent->count;    /* clip size stored during spawn   */
    reserveAmmo = ent->damage;   /* reserve ammo stored during spawn */

    /* Store in player's weapon slots */
    {
        weaponDef_t wd;
        int slotIdx = 0, i;

        if ( BG_ParseWeaponDef( weapName, &wd ) ) {
            slotIdx = !Q_stricmp(wd.weaponSlot,"pistol") ? 2 :
                      !Q_stricmp(wd.weaponSlot,"grenade") ? 3 :
                      !Q_stricmp(wd.weaponSlot,"smokegrenade") ? 4 : 0;
        }
        /* if slot occupied, try primaryb or any empty */
        if ( client->weaponSlots[slotIdx].name[0] ) {
            if ( slotIdx == 0 && !client->weaponSlots[1].name[0] ) {
                slotIdx = 1;
            } else {
                for ( i = 5; i < COD1_WEAPON_SLOT_NUM; i++ ) {
                    if ( !client->weaponSlots[i].name[0] ) { slotIdx = i; break; }
                }
            }
        }
        Q_strncpyz( client->weaponSlots[slotIdx].name, weapName,
                    sizeof(client->weaponSlots[slotIdx].name) );
        client->weaponSlots[slotIdx].clipAmmo = clipAmmo;
        client->weaponSlots[slotIdx].reserveAmmo = reserveAmmo;
        client->currentWeaponSlot = slotIdx;
    }

    /* Send weapon pickup notification to the client */
    trap_SendServerCommand( other->s.number,
        va( "weapon %s %d %d", weapName, clipAmmo, reserveAmmo ) );

    /* Temporarily hide the entity and disable its trigger */
    ent->r.contents = 0;
    ent->r.svFlags |= SVF_NOCLIENT;
    trap_UnlinkEntity( ent );

    if ( WEAPON_RESPAWN_TIME > 0 ) {
        ent->think     = G_WeaponRespawn;
        ent->nextthink = level.time + WEAPON_RESPAWN_TIME * 1000;
    }
}

/*
 * SP_weapon_cod1 -- generic CoD1 weapon entity spawner.
 *
 * Called for any "weapon_*" or "mpweapon_*" classname.
 */
void SP_weapon_cod1( gentity_t *ent )
{
    const char *weapName;
    weaponDef_t wd;
    char        worldModel[MAX_QPATH];

    weapName = WeaponNameFromClass( ent->classname );

    /* Load weapon definition to get ammo and world model */
    Com_Memset( &wd, 0, sizeof(wd) );
    if ( !BG_ParseWeaponDef( weapName, &wd ) ) {
        G_Printf( "SP_weapon_cod1: no def for '%s'\n", weapName );
        ent->count  = 30;   /* default clip */
        ent->damage = 90;   /* default reserve */
    } else {
        ent->count  = wd.clipSize;
        ent->damage = wd.startAmmo;
    }

    /* Determine world model */
    worldModel[0] = '\0';
    if ( ent->model && ent->model[0] ) {
        Q_strncpyz( worldModel, ent->model, sizeof(worldModel) );
    } else if ( wd.worldModel[0] ) {
        Q_strncpyz( worldModel, wd.worldModel, sizeof(worldModel) );
    } else if ( wd.gunModel[0] ) {
        if ( Q_strncmp( wd.gunModel, "xmodel/", 7 ) == 0 )
            Q_strncpyz( worldModel, wd.gunModel, sizeof(worldModel) );
        else
            Com_sprintf( worldModel, sizeof(worldModel), "xmodel/%s", wd.gunModel );
    }

    if ( worldModel[0] )
        ent->s.modelindex = G_ModelIndex( worldModel );

    /* Pickup trigger bounds */
    VectorSet( ent->r.mins, -16, -16, -16 );
    VectorSet( ent->r.maxs,  16,  16,  16 );
    ent->r.contents = CONTENTS_TRIGGER;
    ent->touch      = Touch_WeaponCod1;

    G_SetOrigin( ent, ent->s.origin );
    VectorCopy( ent->s.angles, ent->s.apos.trBase );

    trap_LinkEntity( ent );

    G_Printf( "SP_weapon_cod1: '%s' -> weapon='%s' clip=%d res=%d model=%s\n",
                 ent->classname, weapName, ent->count, ent->damage,
                 worldModel[0] ? worldModel : "(none)" );
}

/* ===========================================================================
   G_MeleeDamage -- server-side melee trace from player position
   ===========================================================================
   Called from ClientThink_real when BUTTON_MELEE is pressed.
   Traces a short box forward from the player's eye, applies damage to the
   first entity hit.
   =========================================================================== */

#define MELEE_RANGE     64.0f
#define MELEE_DAMAGE   150

void G_MeleeDamage( gentity_t *attacker )
{
    vec3_t    forward, right, up;
    vec3_t    muzzle, end;
    trace_t   tr;
    gentity_t *traceEnt;
    static const vec3_t meleeHalf = { 8, 8, 8 };

    if ( !attacker || !attacker->client ) return;

    AngleVectors( attacker->client->ps.viewangles, forward, right, up );

    VectorCopy( attacker->client->ps.origin, muzzle );
    muzzle[2] += attacker->client->ps.viewheight;

    VectorMA( muzzle, MELEE_RANGE, forward, end );

    trap_Trace( &tr, muzzle, (float*)meleeHalf, (float*)meleeHalf, end,
                attacker->s.number, MASK_SHOT );

    if ( tr.fraction >= 1.0f ) return;

    traceEnt = &g_entities[ tr.entityNum ];
    if ( !traceEnt->takedamage ) return;

    G_Damage( traceEnt, attacker, attacker, forward, tr.endpos,
              MELEE_DAMAGE, 0, MOD_MELEE );
}

#endif /* STANDALONE */
