/*
===========================================================================
bg_weapon_cod1.c  --  CoD1 weapon definition parser

CoD1 weapon files live at weapons/sp/<name> or weapons/mp/<name>.
Format (text, backslash-delimited):

  WEAPONFILE\weaponType\bullet\weaponClass\rifle\...\key\value\...

Every field is separated by backslashes.  Empty values (\key\\next\) are
valid and represent the empty string / zero.
===========================================================================
*/
#include "q_shared.h"
#include "qcommon.h"
#include "bg_weapon_cod1.h"

/* ===========================================================================
   Field-setter dispatch
   =========================================================================== */

#define WFSTR(field) \
    Q_strncpyz( out->field, val, sizeof(out->field) )
#define WFINT(field) \
    out->field = atoi( val )
#define WFFLT(field) \
    out->field = (float)atof( val )

static void SetWeaponField( weaponDef_t *out, const char *key, const char *val )
{
    /* Identity */
    if      ( !Q_stricmp( key, "displayName"         ) ) WFSTR( displayName        );
    else if ( !Q_stricmp( key, "weaponType"          ) ) WFSTR( weaponType         );
    else if ( !Q_stricmp( key, "weaponClass"         ) ) WFSTR( weaponClass        );
    else if ( !Q_stricmp( key, "weaponSlot"          ) ) WFSTR( weaponSlot         );
    else if ( !Q_stricmp( key, "radiantName"         ) ) WFSTR( radiantName        );
    else if ( !Q_stricmp( key, "modeName"            ) ) WFSTR( modeName           );

    /* Models */
    else if ( !Q_stricmp( key, "gunModel"            ) ) WFSTR( gunModel           );
    else if ( !Q_stricmp( key, "handModel"           ) ) WFSTR( handModel          );
    else if ( !Q_stricmp( key, "worldModel"          ) ) WFSTR( worldModel         );

    /* Standing stance offsets */
    else if ( !Q_stricmp( key, "standMoveF"          ) ) WFFLT( standMoveF         );
    else if ( !Q_stricmp( key, "standMoveR"          ) ) WFFLT( standMoveR         );
    else if ( !Q_stricmp( key, "standMoveU"          ) ) WFFLT( standMoveU         );
    else if ( !Q_stricmp( key, "standRotP"           ) ) WFFLT( standRotP          );
    else if ( !Q_stricmp( key, "standRotY"           ) ) WFFLT( standRotY          );
    else if ( !Q_stricmp( key, "standRotR"           ) ) WFFLT( standRotR          );

    /* Crouched stance offsets */
    else if ( !Q_stricmp( key, "duckedOfsF"          ) ) WFFLT( duckedOfsF         );
    else if ( !Q_stricmp( key, "duckedOfsR"          ) ) WFFLT( duckedOfsR         );
    else if ( !Q_stricmp( key, "duckedOfsU"          ) ) WFFLT( duckedOfsU         );
    else if ( !Q_stricmp( key, "duckedMoveF"         ) ) WFFLT( duckedMoveF        );
    else if ( !Q_stricmp( key, "duckedMoveR"         ) ) WFFLT( duckedMoveR        );
    else if ( !Q_stricmp( key, "duckedMoveU"         ) ) WFFLT( duckedMoveU        );
    else if ( !Q_stricmp( key, "duckedRotP"          ) ) WFFLT( duckedRotP         );
    else if ( !Q_stricmp( key, "duckedRotY"          ) ) WFFLT( duckedRotY         );
    else if ( !Q_stricmp( key, "duckedRotR"          ) ) WFFLT( duckedRotR         );

    /* Prone stance offsets */
    else if ( !Q_stricmp( key, "proneOfsF"           ) ) WFFLT( proneOfsF          );
    else if ( !Q_stricmp( key, "proneOfsR"           ) ) WFFLT( proneOfsR          );
    else if ( !Q_stricmp( key, "proneOfsU"           ) ) WFFLT( proneOfsU          );
    else if ( !Q_stricmp( key, "proneMoveF"          ) ) WFFLT( proneMoveF         );
    else if ( !Q_stricmp( key, "proneMoveR"          ) ) WFFLT( proneMoveR         );
    else if ( !Q_stricmp( key, "proneMoveU"          ) ) WFFLT( proneMoveU         );
    else if ( !Q_stricmp( key, "proneRotP"           ) ) WFFLT( proneRotP          );
    else if ( !Q_stricmp( key, "proneRotY"           ) ) WFFLT( proneRotY          );
    else if ( !Q_stricmp( key, "proneRotR"           ) ) WFFLT( proneRotR          );

    /* Movement interpolation */
    else if ( !Q_stricmp( key, "posMoveRate"         ) ) WFFLT( posMoveRate        );
    else if ( !Q_stricmp( key, "posRotRate"          ) ) WFFLT( posRotRate         );
    else if ( !Q_stricmp( key, "posProneMoveRate"    ) ) WFFLT( posProneMoveRate   );
    else if ( !Q_stricmp( key, "posProneRotRate"     ) ) WFFLT( posProneRotRate    );
    else if ( !Q_stricmp( key, "standMoveMinSpeed"   ) ) WFFLT( standMoveMinSpeed  );
    else if ( !Q_stricmp( key, "duckedMoveMinSpeed"  ) ) WFFLT( duckedMoveMinSpeed );
    else if ( !Q_stricmp( key, "proneMoveMinSpeed"   ) ) WFFLT( proneMoveMinSpeed  );
    else if ( !Q_stricmp( key, "standRotMinSpeed"    ) ) WFFLT( standRotMinSpeed   );
    else if ( !Q_stricmp( key, "duckedRotMinSpeed"   ) ) WFFLT( duckedRotMinSpeed  );
    else if ( !Q_stricmp( key, "proneRotMinSpeed"    ) ) WFFLT( proneRotMinSpeed   );

    /* Weapon bob/sway */
    else if ( !Q_stricmp( key, "swayMaxAngle"        ) ) WFFLT( swayMaxAngle       );
    else if ( !Q_stricmp( key, "swayLerpSpeed"       ) ) WFFLT( swayLerpSpeed      );
    else if ( !Q_stricmp( key, "swayPitchScale"      ) ) WFFLT( swayPitchScale     );
    else if ( !Q_stricmp( key, "swayYawScale"        ) ) WFFLT( swayYawScale       );
    else if ( !Q_stricmp( key, "swayHorizScale"      ) ) WFFLT( swayHorizScale     );
    else if ( !Q_stricmp( key, "swayVertScale"       ) ) WFFLT( swayVertScale      );
    else if ( !Q_stricmp( key, "adsSwayMaxAngle"     ) ) WFFLT( adsSwayMaxAngle    );
    else if ( !Q_stricmp( key, "adsSwayLerpSpeed"    ) ) WFFLT( adsSwayLerpSpeed   );
    else if ( !Q_stricmp( key, "adsSwayPitchScale"   ) ) WFFLT( adsSwayPitchScale  );
    else if ( !Q_stricmp( key, "adsSwayYawScale"     ) ) WFFLT( adsSwayYawScale    );
    else if ( !Q_stricmp( key, "adsSwayHorizScale"   ) ) WFFLT( adsSwayHorizScale  );
    else if ( !Q_stricmp( key, "adsSwayVertScale"    ) ) WFFLT( adsSwayVertScale   );
    else if ( !Q_stricmp( key, "idleCrouchFactor"    ) ) WFFLT( idleCrouchFactor  );
    else if ( !Q_stricmp( key, "idleProneFactor"     ) ) WFFLT( idleProneFactor    );

    /* Animations */
    else if ( !Q_stricmp( key, "idleAnim"            ) ) WFSTR( anims.idleAnim           );
    else if ( !Q_stricmp( key, "emptyIdleAnim"       ) ) WFSTR( anims.emptyIdleAnim      );
    else if ( !Q_stricmp( key, "fireAnim"            ) ) WFSTR( anims.fireAnim           );
    else if ( !Q_stricmp( key, "lastShotAnim"        ) ) WFSTR( anims.lastShotAnim       );
    else if ( !Q_stricmp( key, "rechamberAnim"       ) ) WFSTR( anims.rechamberAnim      );
    else if ( !Q_stricmp( key, "meleeAnim"           ) ) WFSTR( anims.meleeAnim          );
    else if ( !Q_stricmp( key, "reloadAnim"          ) ) WFSTR( anims.reloadAnim         );
    else if ( !Q_stricmp( key, "reloadEmptyAnim"     ) ) WFSTR( anims.reloadEmptyAnim    );
    else if ( !Q_stricmp( key, "reloadStartAnim"     ) ) WFSTR( anims.reloadStartAnim    );
    else if ( !Q_stricmp( key, "reloadEndAnim"       ) ) WFSTR( anims.reloadEndAnim      );
    else if ( !Q_stricmp( key, "raiseAnim"           ) ) WFSTR( anims.raiseAnim          );
    else if ( !Q_stricmp( key, "dropAnim"            ) ) WFSTR( anims.dropAnim           );
    else if ( !Q_stricmp( key, "altRaiseAnim"        ) ) WFSTR( anims.altRaiseAnim       );
    else if ( !Q_stricmp( key, "altDropAnim"         ) ) WFSTR( anims.altDropAnim        );
    else if ( !Q_stricmp( key, "adsFireAnim"         ) ) WFSTR( anims.adsFireAnim        );
    else if ( !Q_stricmp( key, "adsLastShotAnim"     ) ) WFSTR( anims.adsLastShotAnim    );
    else if ( !Q_stricmp( key, "adsRechamberAnim"    ) ) WFSTR( anims.adsRechamberAnim   );
    else if ( !Q_stricmp( key, "adsUpAnim"           ) ) WFSTR( anims.adsUpAnim          );
    else if ( !Q_stricmp( key, "adsDownAnim"         ) ) WFSTR( anims.adsDownAnim        );

    /* Damage / timing */
    else if ( !Q_stricmp( key, "damage"              ) ) WFINT( damage             );
    else if ( !Q_stricmp( key, "meleeDamage"         ) ) WFINT( meleeDamage        );
    else if ( !Q_stricmp( key, "fireTime"            ) ) WFFLT( fireTime           );
    else if ( !Q_stricmp( key, "rechamberTime"       ) ) WFFLT( rechamberTime      );
    else if ( !Q_stricmp( key, "meleeTime"           ) ) WFFLT( meleeTime          );
    else if ( !Q_stricmp( key, "reloadTime"          ) ) WFFLT( reloadTime         );
    else if ( !Q_stricmp( key, "reloadEmptyTime"     ) ) WFFLT( reloadEmptyTime    );
    else if ( !Q_stricmp( key, "reloadAddTime"       ) ) WFFLT( reloadAddTime      );
    else if ( !Q_stricmp( key, "reloadStartTime"     ) ) WFFLT( reloadStartTime    );
    else if ( !Q_stricmp( key, "reloadStartAddTime"  ) ) WFFLT( reloadStartAddTime );
    else if ( !Q_stricmp( key, "reloadEndTime"       ) ) WFFLT( reloadEndTime      );
    else if ( !Q_stricmp( key, "raiseTime"           ) ) WFFLT( raiseTime          );
    else if ( !Q_stricmp( key, "dropTime"            ) ) WFFLT( dropTime           );
    else if ( !Q_stricmp( key, "altRaiseTime"        ) ) WFFLT( altRaiseTime       );
    else if ( !Q_stricmp( key, "altDropTime"         ) ) WFFLT( altDropTime        );
    else if ( !Q_stricmp( key, "fireDelay"           ) ) WFFLT( fireDelay          );
    else if ( !Q_stricmp( key, "meleeDelay"          ) ) WFFLT( meleeDelay         );
    else if ( !Q_stricmp( key, "adsTransInTime"      ) ) WFFLT( adsTransInTime     );
    else if ( !Q_stricmp( key, "adsTransOutTime"     ) ) WFFLT( adsTransOutTime    );

    /* ADS */
    else if ( !Q_stricmp( key, "adsZoomFov"          ) ) WFFLT( adsZoomFov         );
    else if ( !Q_stricmp( key, "adsZoomInFrac"       ) ) WFFLT( adsZoomInFrac      );
    else if ( !Q_stricmp( key, "adsZoomOutFrac"      ) ) WFFLT( adsZoomOutFrac     );
    else if ( !Q_stricmp( key, "adsBobFactor"        ) ) WFFLT( adsBobFactor       );
    else if ( !Q_stricmp( key, "adsViewBobMult"      ) ) WFFLT( adsViewBobMult     );
    else if ( !Q_stricmp( key, "adsSpread"           ) ) WFFLT( adsSpread          );
    else if ( !Q_stricmp( key, "adsIdleAmount"       ) ) WFFLT( adsIdleAmount      );
    else if ( !Q_stricmp( key, "adsOverlayShader"    ) ) WFSTR( adsOverlayShader   );
    else if ( !Q_stricmp( key, "adsOverlayReticle"   ) ) WFSTR( adsOverlayReticle  );

    /* Hip fire */
    else if ( !Q_stricmp( key, "hipSpreadStandMin"   ) ) WFFLT( hipSpreadStandMin  );
    else if ( !Q_stricmp( key, "hipSpreadDuckedMin"  ) ) WFFLT( hipSpreadDuckedMin );
    else if ( !Q_stricmp( key, "hipSpreadProneMin"   ) ) WFFLT( hipSpreadProneMin  );
    else if ( !Q_stricmp( key, "hipSpreadMax"        ) ) WFFLT( hipSpreadMax       );
    else if ( !Q_stricmp( key, "hipIdleAmount"       ) ) WFFLT( hipIdleAmount      );
    else if ( !Q_stricmp( key, "moveSpeedScale"      ) ) WFFLT( moveSpeedScale     );

    /* Ammo */
    else if ( !Q_stricmp( key, "maxAmmo"             ) ) WFINT( maxAmmo            );
    else if ( !Q_stricmp( key, "startAmmo"           ) ) WFINT( startAmmo          );
    else if ( !Q_stricmp( key, "clipSize"            ) ) WFINT( clipSize           );
    else if ( !Q_stricmp( key, "dropAmmoMin"         ) ) WFINT( dropAmmoMin        );
    else if ( !Q_stricmp( key, "dropAmmoMax"         ) ) WFINT( dropAmmoMax        );
    else if ( !Q_stricmp( key, "ammoName"            ) ) WFSTR( ammoName           );
    else if ( !Q_stricmp( key, "clipName"            ) ) WFSTR( clipName           );

    /* HUD */
    else if ( !Q_stricmp( key, "hudIcon"             ) ) WFSTR( hudIcon            );
    else if ( !Q_stricmp( key, "ammoIcon"            ) ) WFSTR( ammoIcon           );
    else if ( !Q_stricmp( key, "modeIcon"            ) ) WFSTR( modeIcon           );

    /* Effects */
    else if ( !Q_stricmp( key, "viewFlashEffect"     ) ) WFSTR( viewFlashEffect    );
    else if ( !Q_stricmp( key, "worldFlashEffect"    ) ) WFSTR( worldFlashEffect   );

    /* Unknown keys are silently ignored -- CoD1 has many more fields */
}

#undef WFSTR
#undef WFINT
#undef WFFLT

/* ===========================================================================
   BG_ParseWeaponDef
   =========================================================================== */

qboolean BG_ParseWeaponDef( const char *name, weaponDef_t *out )
{
    char  path[MAX_QPATH];
    char *raw = NULL;
    const char *p;
    char  key[64], val[256];
    int   ki, vi;

    if ( !name || !name[0] || !out ) return qfalse;

    /* Try sp first, then mp */
    Com_sprintf( path, sizeof(path), "weapons/sp/%s", name );
    FS_ReadFile( path, (void **)&raw );
    if ( !raw ) {
        Com_sprintf( path, sizeof(path), "weapons/mp/%s", name );
        FS_ReadFile( path, (void **)&raw );
    }
    if ( !raw ) {
        Com_Printf( "BG_ParseWeaponDef: '%s' not found\n", name );
        return qfalse;
    }

    /* Verify WEAPONFILE header */
    if ( Q_strncmp( raw, "WEAPONFILE", 10 ) != 0 ) {
        Com_Printf( "BG_ParseWeaponDef: '%s' missing WEAPONFILE header\n", name );
        FS_FreeFile( raw );
        return qfalse;
    }

    Com_Memset( out, 0, sizeof(*out) );

    /* Walk \key\value pairs */
    p = raw + 10;   /* skip "WEAPONFILE" */
    while ( *p == '\\' ) {
        /* key */
        p++;
        ki = 0;
        while ( *p && *p != '\\' ) {
            if ( ki < (int)sizeof(key) - 1 ) key[ki++] = *p;
            p++;
        }
        key[ki] = '\0';

        /* value */
        vi = 0;
        if ( *p == '\\' ) p++;
        while ( *p && *p != '\\' ) {
            if ( vi < (int)sizeof(val) - 1 ) val[vi++] = *p;
            p++;
        }
        val[vi] = '\0';

        if ( key[0] ) SetWeaponField( out, key, val );
    }

    FS_FreeFile( raw );
    return qtrue;
}
