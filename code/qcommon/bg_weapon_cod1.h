/*
===========================================================================
bg_weapon_cod1.h  --  CoD1 weapon definition types

Parsed from weapons/sp/<name> or weapons/mp/<name>  (WEAPONFILE format).
Format: WEAPONFILE\key\value\key\value...  (backslash-delimited)
===========================================================================
*/
#pragma once

/* Animation slot names -- all reference xanim/<animName> files */
typedef struct {
	char idleAnim[MAX_QPATH];
	char emptyIdleAnim[MAX_QPATH];
	char fireAnim[MAX_QPATH];
	char lastShotAnim[MAX_QPATH];
	char rechamberAnim[MAX_QPATH];
	char meleeAnim[MAX_QPATH];
	char reloadAnim[MAX_QPATH];
	char reloadEmptyAnim[MAX_QPATH];
	char reloadStartAnim[MAX_QPATH];
	char reloadEndAnim[MAX_QPATH];
	char raiseAnim[MAX_QPATH];        /* pullout */
	char dropAnim[MAX_QPATH];         /* putaway */
	char altRaiseAnim[MAX_QPATH];
	char altDropAnim[MAX_QPATH];
	char adsFireAnim[MAX_QPATH];
	char adsLastShotAnim[MAX_QPATH];
	char adsRechamberAnim[MAX_QPATH];
	char adsUpAnim[MAX_QPATH];
	char adsDownAnim[MAX_QPATH];
} weaponAnims_t;

typedef struct {
	/* Identity */
	char  displayName[64];
	char  weaponType[32];    /* bullet, grenade, rocket, ... */
	char  weaponClass[32];   /* rifle, pistol, smg, mg, shotgun, ... */
	char  weaponSlot[16];    /* primary, secondary, grenade, ... */
	char  radiantName[64];   /* editor name, also used as weapon ID */

	/* Models */
	char  gunModel[MAX_QPATH];    /* viewmodel xmodel (without "xmodel/") */
	char  handModel[MAX_QPATH];   /* hands xmodel (without "xmodel/") */
	char  worldModel[MAX_QPATH];  /* world/pickup xmodel (full path, e.g. "xmodel/weapon_colt45") */

	/* Viewmodel positioning (offset from camera eye) */
	float  handOffset[3];         /* right, forward, up offset for viewmodel */

	/* Animations */
	weaponAnims_t anims;

	/* Gameplay stats */
	int   damage;
	int   meleeDamage;
	float fireTime;           /* seconds between shots */
	float rechamberTime;
	float meleeTime;
	float reloadTime;
	float reloadEmptyTime;
	float reloadAddTime;      /* time when ammo is added during reload */
	float reloadStartTime;
	float reloadStartAddTime;
	float reloadEndTime;
	float raiseTime;
	float dropTime;
	float altRaiseTime;
	float altDropTime;
	float fireDelay;
	float meleeDelay;
	float adsTransInTime;
	float adsTransOutTime;

	/* ADS (Aim Down Sights) */
	float adsZoomFov;
	float adsZoomInFrac;
	float adsZoomOutFrac;
	float adsBobFactor;
	float adsViewBobMult;
	float adsSpread;
	float adsIdleAmount;
	char  adsOverlayShader[MAX_QPATH];
	char  adsOverlayReticle[32];

	/* Hip fire */
	float hipSpreadStandMin;
	float hipSpreadDuckedMin;
	float hipSpreadProneMin;
	float hipSpreadMax;
	float hipIdleAmount;
	float moveSpeedScale;

	/* Ammo */
	int   maxAmmo;
	int   startAmmo;
	int   clipSize;
	int   dropAmmoMin;
	int   dropAmmoMax;
	char  ammoName[32];
	char  clipName[32];

	/* HUD */
	char  hudIcon[MAX_QPATH];
	char  ammoIcon[MAX_QPATH];
	char  modeIcon[MAX_QPATH];
	char  modeName[64];

	/* Effects */
	char  viewFlashEffect[MAX_QPATH];
	char  worldFlashEffect[MAX_QPATH];
} weaponDef_t;

/*
 * Parse a CoD1 weapon definition file.
 * name: weapon name without directory (e.g. "colt_mp", "30cal")
 *       The function tries weapons/sp/<name> then weapons/mp/<name>.
 * out:  filled on success.
 * Returns qtrue on success, qfalse if the file was not found / invalid.
 */
qboolean BG_ParseWeaponDef( const char *name, weaponDef_t *out );
