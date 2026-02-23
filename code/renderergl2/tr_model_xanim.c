/*
===========================================================================
tr_model_xanim.c  --  CoD1 XANIM loader and bone evaluator

Loads xanim binary files (CoD1 V14 / 0x0E format) and provides:
  R_RegisterXAnim  -- load from VFS, return handle
  R_EvalXAnimBones -- evaluate bones at a given frame

Also owns:
  XModel_ComputeWorldBones  -- recompute world transforms from local
  R_StoreXModelBindPose     -- cache bind-pose bones by model handle

Binary format (little-endian, version 0x0E):
  u16 version
  u16 numFrames
  u16 numParts
  u8  flags          (0x01 = loop, 0x02 = delta)
  u16 framerate

  [if delta]:
    read_rotations("tag_origin", flipquat=false, simplequat=true)
    read_translations("tag_origin")

  [if loop]: numFrames++

  u8[ceil(numParts/8)]  flipFlags    (per-part bitmask)
  u8[ceil(numParts/8)]  simpleFlags
  null-string * numParts              (part names)

  for each part:
    read_rotations(partName, flipquat, simplequat)
    read_translations(partName)

  u8 notifyCount
  for each notify:
    null-string  (event name)
    u16          (frame index)

Per-track encoding:
  u16 numKeys
  if (numKeys == 0)              nothing
  if (numKeys == 1 || == total frames):  implicit indices 0,1,2,...
  elif (numFrames > 255):        numKeys × u16 frame indices
  else:                          numKeys × u8  frame indices
  numKeys × rotation-or-translation data

Rotation data:
  if simplequat: 1 × i16 (z component only; w = sqrt(1-z^2))
  else:          3 × i16 (x,y,z; w = sqrt(1-x^2-y^2-z^2))
  Divisor: SHRT_MAX (32767)

Translation data: numKeys × vec3 (3 × float)

===========================================================================
*/
#include "tr_local.h"
#include "tr_xmodel.h"

/* ===========================================================================
   Rotation keyframe track
   =========================================================================== */

typedef struct {
    int     numKeys;
    int    *frames;    /* frame indices, [numKeys] */
    vec4_t *rots;      /* quaternion [w,x,y,z],  [numKeys] */
} xaRotTrack_t;

/* ===========================================================================
   Translation keyframe track
   =========================================================================== */

typedef struct {
    int     numKeys;
    int    *frames;    /* frame indices, [numKeys] */
    vec3_t *trans;     /* translation,   [numKeys] */
} xaTransTrack_t;

/* ===========================================================================
   Per-bone animation tracks
   =========================================================================== */

typedef struct {
    char           partName[64];
    xaRotTrack_t   rotTrack;
    xaTransTrack_t transTrack;
} xaPartAnim_t;

/* ===========================================================================
   Animation object
   =========================================================================== */

typedef struct {
    int          numFrames;   /* total frames (may include looping extra frame) */
    int          numParts;
    int          framerate;
    int          flags;       /* 0x01 = loop, 0x02 = delta */
    xaPartAnim_t *parts;      /* [numParts], ri.Malloc'd */
} xaAnim_t;

/* ===========================================================================
   Global tables
   =========================================================================== */

#define XANIM_TABLE_SIZE    1024

static xaAnim_t *s_animTable[ XANIM_TABLE_SIZE ];
static char      s_animNames[ XANIM_TABLE_SIZE ][ MAX_QPATH ];
static int       s_numAnims = 0;

/* Bind-pose table: indexed by (model->index), same size as MAX_MOD_KNOWN */
#define BIND_TABLE_SIZE     1024

static xmBone_t    *s_bindPose[  BIND_TABLE_SIZE ];
static int          s_bindCount[ BIND_TABLE_SIZE ];

/* CPU skinning data (local vertex positions) and current animated pose */
static xmSkinData_t *s_skinData[  BIND_TABLE_SIZE ];
static xmBone_t     *s_curPose[   BIND_TABLE_SIZE ];
static int           s_curCount[  BIND_TABLE_SIZE ];

/* ===========================================================================
   XModel_ComputeWorldBones
   =========================================================================== */

void XModel_ComputeWorldBones( xmBone_t *bones, int numBones )
{
    int i;
    for ( i = 0; i < numBones; i++ ) {
        int p = bones[i].parent;
        if ( p >= 0 && p < i ) {           /* parent must already be processed */
            vec3_t rotTrans;
            Quat_Mul   ( bones[p].wRot, bones[i].lRot,   bones[i].wRot  );
            Quat_RotVec( bones[p].wRot, bones[i].lTrans, rotTrans        );
            VectorAdd  ( bones[p].wTrans, rotTrans,       bones[i].wTrans );
        } else {
            VectorCopy ( bones[i].lTrans, bones[i].wTrans );
            Vector4Copy( bones[i].lRot,   bones[i].wRot   );
        }
    }
}

/* ===========================================================================
   Bind-pose storage
   =========================================================================== */

void R_StoreXModelBindPose( qhandle_t modelHandle,
                             const xmBone_t *bones, int numBones )
{
    int idx = (int)modelHandle;
    if ( idx < 0 || idx >= BIND_TABLE_SIZE || numBones <= 0 ) return;

    if ( s_bindPose[idx] ) {
        ri.Free( s_bindPose[idx] );
        s_bindPose[idx]  = NULL;
        s_bindCount[idx] = 0;
    }

    s_bindPose[idx]  = (xmBone_t *)ri.Malloc( sizeof(xmBone_t) * numBones );
    Com_Memcpy( s_bindPose[idx], bones, sizeof(xmBone_t) * numBones );
    s_bindCount[idx] = numBones;
}

/* ===========================================================================
   CPU skinning data storage
   =========================================================================== */

void R_StoreXModelSkinData( qhandle_t modelHandle, xmSkinData_t *data )
{
    int idx = (int)modelHandle;
    if ( idx < 0 || idx >= BIND_TABLE_SIZE ) return;
    s_skinData[idx] = data;
}

/* ===========================================================================
   R_UpdateXModelPose
   Evaluates the animation at 'frame', re-skins all vertices in-place, and
   updates the mdvTag array so re.LerpTag returns animated positions.
   =========================================================================== */

void R_UpdateXModelPose( qhandle_t modelHandle, qhandle_t animHandle, float frame )
{
    int          idx = (int)modelHandle;
    int          i, j, numBones;
    xmBone_t     workBones[XMODEL_MAX_BONES];
    xmSkinData_t *sd;
    mdvModel_t   *mdvModel;

    if ( idx < 0 || idx >= BIND_TABLE_SIZE ) return;
    if ( !s_bindPose[idx] || s_bindCount[idx] <= 0 ) return;

    numBones = s_bindCount[idx];

    /* Start from bind pose */
    Com_Memcpy( workBones, s_bindPose[idx], sizeof(xmBone_t) * numBones );

    /* Overlay animation keyframes (by bone name matching) */
    if ( animHandle > 0 )
        R_EvalXAnimBones( animHandle, frame, workBones, numBones );
    else
        XModel_ComputeWorldBones( workBones, numBones );

    /* Cache the evaluated pose for R_XModelLerpTag */
    if ( !s_curPose[idx] || s_curCount[idx] < numBones ) {
        if ( s_curPose[idx] ) ri.Free( s_curPose[idx] );
        s_curPose[idx]  = (xmBone_t *)ri.Malloc( sizeof(xmBone_t) * numBones );
        s_curCount[idx] = numBones;
    }
    Com_Memcpy( s_curPose[idx], workBones, sizeof(xmBone_t) * numBones );

    /* Re-skin all vertices in-place */
    sd = s_skinData[idx];
    if ( sd ) {
        for ( i = 0; i < sd->numSurfaces; i++ ) {
            xmSkinSurf_t *surf = &sd->surfs[i];
            for ( j = 0; j < surf->numVerts; j++ ) {
                vec3_t worldPos, worldNormal;
                int    bi = surf->verts[j].boneIdx;

                if ( bi >= 0 && bi < numBones ) {
                    const xmBone_t *b = &workBones[bi];
                    Quat_RotVec( b->wRot, surf->verts[j].localPos,    worldPos    );
                    VectorAdd  ( b->wTrans, worldPos,                  worldPos    );
                    Quat_RotVec( b->wRot, surf->verts[j].localNormal,  worldNormal );
                } else {
                    VectorCopy( surf->verts[j].localPos,    worldPos    );
                    VectorCopy( surf->verts[j].localNormal, worldNormal );
                }

                VectorNormalize( worldNormal );
                VectorCopy( worldPos, surf->mdvVerts[j].xyz );
                R_VaoPackNormal( surf->mdvVerts[j].normal, worldNormal );
            }
        }

        /* Update mdvTag entries so re.LerpTag returns animated positions */
        mdvModel = sd->mdvModel;
        if ( mdvModel && mdvModel->numTags > 0 ) {
            int tagIdx = 0;
            for ( i = 0; i < numBones && tagIdx < mdvModel->numTags; i++ ) {
                if ( Q_strncmp( workBones[i].name, "tag_", 4 ) == 0 ) {
                    VectorCopy( workBones[i].wTrans, mdvModel->tags[tagIdx].origin );
                    Quat_ToAxis( workBones[i].wRot,  mdvModel->tags[tagIdx].axis   );
                    tagIdx++;
                }
            }
        }
    }
}

/*
====================
R_XModelLerpTag
====================
*/
int R_XModelLerpTag( orientation_t *tag, qhandle_t handle,
                      int startFrame, int endFrame,
                      float frac, const char *tagName )
{
    int      idx = (int)handle;
    int      i;
    xmBone_t *pose;
    int      count;

    if ( idx < 0 || idx >= BIND_TABLE_SIZE ) return 0;

    /* Prefer the current animated pose if available */
    if ( s_curPose[idx] && s_curCount[idx] > 0 ) {
        pose  = s_curPose[idx];
        count = s_curCount[idx];
    } else if ( s_bindPose[idx] && s_bindCount[idx] > 0 ) {
        pose  = s_bindPose[idx];
        count = s_bindCount[idx];
    } else {
        return 0;
    }

    for ( i = 0; i < count; i++ ) {
        if ( !Q_stricmp( pose[i].name, tagName ) ) {
            VectorCopy( pose[i].wTrans, tag->origin );
            Quat_ToAxis( pose[i].wRot,  tag->axis   );
            return 1;
        }
    }

    return 0;
}

/* ===========================================================================
   Track helpers
   =========================================================================== */

static void XAnim_FreeRotTrack( xaRotTrack_t *t )
{
    if ( t->frames ) { ri.Free( t->frames ); t->frames = NULL; }
    if ( t->rots   ) { ri.Free( t->rots   ); t->rots   = NULL; }
    t->numKeys = 0;
}

static void XAnim_FreeTransTrack( xaTransTrack_t *t )
{
    if ( t->frames ) { ri.Free( t->frames ); t->frames = NULL; }
    if ( t->trans  ) { ri.Free( t->trans  ); t->trans  = NULL; }
    t->numKeys = 0;
}

/* Read a rotation track.
 * flipquat: when set, negate all stored quaternion components.
 *           (q and -q represent the same rotation; the flag picks the
 *            hemisphere for best interpolation.  Our Quat_Slerp already
 *            handles the sign-flip via its dot<0 branch, so either sign
 *            produces correct results, but we honour the flag anyway.)
 * simplequat: when set, only the Z component is stored in the file
 *             (W and XY are derived).  Otherwise X, Y, Z are stored.
 */
static void XAnim_ReadRotTrack( xmR_t *r, int numFrames,
                                  xaRotTrack_t *track,
                                  qboolean flipquat, qboolean simplequat )
{
    int i, numKeys;

    track->numKeys = 0;
    track->frames  = NULL;
    track->rots    = NULL;

    numKeys = (int)xmR_u16( r );
    if ( numKeys == 0 ) return;

    /* Frame indices */
    track->frames = (int *)ri.Malloc( sizeof(int) * numKeys );
    if ( numKeys == 1 || numKeys == numFrames ) {
        for ( i = 0; i < numKeys; i++ ) track->frames[i] = i;
    } else if ( numFrames > 0xFF ) {
        for ( i = 0; i < numKeys; i++ ) track->frames[i] = (int)xmR_u16( r );
    } else {
        for ( i = 0; i < numKeys; i++ ) track->frames[i] = (int)xmR_u8( r );
    }

    /* Quaternion data */
    track->rots = (vec4_t *)ri.Malloc( sizeof(vec4_t) * numKeys );
    for ( i = 0; i < numKeys; i++ ) {
        float x = 0.0f, y = 0.0f, z, ww;
        if ( simplequat ) {
            z = xmR_s16( r ) / 32767.0f;
        } else {
            x = xmR_s16( r ) / 32767.0f;
            y = xmR_s16( r ) / 32767.0f;
            z = xmR_s16( r ) / 32767.0f;
        }
        ww = 1.0f - x*x - y*y - z*z;
        track->rots[i][0] = ( ww > 0.0f ) ? (float)sqrt( (double)ww ) : 0.0f;
        track->rots[i][1] = x;
        track->rots[i][2] = y;
        track->rots[i][3] = z;

        /* Honour the per-part flip flag: negate all components.
         * Previously the track was discarded when flipquat was set, which
         * caused those bone rotation tracks to be silently dropped,
         * leaving affected bones frozen in their bind-pose orientation. */
        if ( flipquat ) {
            track->rots[i][0] = -track->rots[i][0];
            track->rots[i][1] = -track->rots[i][1];
            track->rots[i][2] = -track->rots[i][2];
            track->rots[i][3] = -track->rots[i][3];
        }
    }

    track->numKeys = numKeys;
}

static void XAnim_ReadTransTrack( xmR_t *r, int numFrames, xaTransTrack_t *track )
{
    int i, numKeys;

    track->numKeys = 0;
    track->frames  = NULL;
    track->trans   = NULL;

    numKeys = (int)xmR_u16( r );
    if ( numKeys == 0 ) return;

    track->frames = (int *)ri.Malloc( sizeof(int) * numKeys );
    if ( numKeys == 1 || numKeys == numFrames ) {
        for ( i = 0; i < numKeys; i++ ) track->frames[i] = i;
    } else if ( numFrames > 0xFF ) {
        for ( i = 0; i < numKeys; i++ ) track->frames[i] = (int)xmR_u16( r );
    } else {
        for ( i = 0; i < numKeys; i++ ) track->frames[i] = (int)xmR_u8( r );
    }

    track->trans = (vec3_t *)ri.Malloc( sizeof(vec3_t) * numKeys );
    for ( i = 0; i < numKeys; i++ ) xmR_vec3( r, track->trans[i] );

    track->numKeys = numKeys;
}

/* ===========================================================================
   R_RegisterXAnim
   =========================================================================== */

qhandle_t R_RegisterXAnim( const char *name )
{
    void     *buf;
    int       fSize, i;
    char      path[MAX_QPATH];
    xmR_t     r;
    xaAnim_t *anim;
    int       numFrames, numParts, framerate, flags;
    int       boneFlagsSize;
    byte     *flipFlags, *simpleFlags;
    char    **partNames;
    qboolean  looping, delta;

    if ( !name || !name[0] ) return 0;

    /* ---- cache lookup ---- */
    for ( i = 0; i < s_numAnims; i++ ) {
        if ( !Q_stricmp( s_animNames[i], name ) )
            return (qhandle_t)( i + 1 );
    }

    if ( s_numAnims >= XANIM_TABLE_SIZE ) {
        ri.Printf( PRINT_WARNING, "R_RegisterXAnim: table full, cannot load '%s'\n", name );
        return 0;
    }

    /* ---- build path ---- */
    if ( Q_strncmp( name, "xanim/", 6 ) == 0 )
        Q_strncpyz( path, name, sizeof(path) );
    else
        Com_sprintf( path, sizeof(path), "xanim/%s", name );

    fSize = ri.FS_ReadFile( path, &buf );
    if ( !buf || fSize < 9 ) {
        ri.Printf( PRINT_DEVELOPER, "R_RegisterXAnim: could not load '%s'\n", path );
        if ( buf ) ri.FS_FreeFile( buf );
        return 0;
    }

    xmR_init( &r, (const byte *)buf, fSize );

    if ( xmR_u16( &r ) != COD1_XMODEL_VERSION ) {
        ri.Printf( PRINT_WARNING, "R_RegisterXAnim: bad version in '%s'\n", path );
        ri.FS_FreeFile( buf );
        return 0;
    }

    numFrames = (int)xmR_u16( &r );
    numParts  = (int)xmR_u16( &r );
    flags     = (int)xmR_u8 ( &r );
    framerate = (int)xmR_u16( &r );

    looping = ( flags & 0x1 ) != 0;
    delta   = ( flags & 0x2 ) != 0;

    /* ---- delta tag_origin (read and discard) ---- */
    if ( delta ) {
        xaRotTrack_t   deltaRot   = { 0, NULL, NULL };
        xaTransTrack_t deltaTrans = { 0, NULL, NULL };
        /* tag_origin uses simplequat=true per reference */
        XAnim_ReadRotTrack  ( &r, numFrames, &deltaRot,   qfalse, qtrue );
        XAnim_ReadTransTrack( &r, numFrames, &deltaTrans );
        XAnim_FreeRotTrack  ( &deltaRot   );
        XAnim_FreeTransTrack( &deltaTrans );
    }

    if ( looping ) numFrames++;   /* extra wrap frame */

    if ( numParts <= 0 ) {
        ri.Printf( PRINT_WARNING, "R_RegisterXAnim: no parts in '%s'\n", path );
        ri.FS_FreeFile( buf );
        return 0;
    }

    /* ---- flip/simple bitmasks ---- */
    boneFlagsSize = ( ( numParts - 1 ) >> 3 ) + 1;
    flipFlags   = (byte *)ri.Malloc( boneFlagsSize );
    simpleFlags = (byte *)ri.Malloc( boneFlagsSize );
    for ( i = 0; i < boneFlagsSize; i++ ) flipFlags[i]   = xmR_u8( &r );
    for ( i = 0; i < boneFlagsSize; i++ ) simpleFlags[i] = xmR_u8( &r );

    /* ---- part names ---- */
    partNames = (char **)ri.Malloc( sizeof(char *) * numParts );
    for ( i = 0; i < numParts; i++ ) {
        partNames[i] = (char *)ri.Malloc( 64 );
        xmR_str( &r, partNames[i], 64 );
    }

    /* ---- allocate animation ---- */
    anim            = (xaAnim_t *)ri.Malloc( sizeof(xaAnim_t) );
    anim->numFrames = numFrames;
    anim->numParts  = numParts;
    anim->framerate = framerate;
    anim->flags     = flags;
    anim->parts     = (xaPartAnim_t *)ri.Malloc( sizeof(xaPartAnim_t) * numParts );
    Com_Memset( anim->parts, 0, sizeof(xaPartAnim_t) * numParts );

    /* ---- per-part tracks ---- */
    for ( i = 0; i < numParts; i++ ) {
        qboolean fq = ( ( 1 << ( i & 7 ) ) & flipFlags[i >> 3]   ) != 0;
        qboolean sq = ( ( 1 << ( i & 7 ) ) & simpleFlags[i >> 3] ) != 0;

        Q_strncpyz( anim->parts[i].partName, partNames[i], 64 );
        XAnim_ReadRotTrack  ( &r, numFrames, &anim->parts[i].rotTrack,   fq, sq );
        XAnim_ReadTransTrack( &r, numFrames, &anim->parts[i].transTrack );
    }

    /* ---- notifies (consume, print for now) ---- */
    {
        int notifyCount = (int)xmR_u8( &r );
        for ( i = 0; i < notifyCount; i++ ) {
            char evtName[64];
            xmR_str( &r, evtName, sizeof(evtName) );
            xmR_u16( &r );  /* frame index */
        }
    }

    /* ---- cleanup temporaries ---- */
    for ( i = 0; i < numParts; i++ ) ri.Free( partNames[i] );
    ri.Free( partNames );
    ri.Free( flipFlags );
    ri.Free( simpleFlags );
    ri.FS_FreeFile( buf );

    /* ---- store in table ---- */
    Q_strncpyz( s_animNames[s_numAnims], name, MAX_QPATH );
    s_animTable[s_numAnims] = anim;
    s_numAnims++;

    ri.Printf( PRINT_DEVELOPER,
               "R_RegisterXAnim: '%s'  frames=%d parts=%d fps=%d%s\n",
               name, numFrames, numParts, framerate,
               looping ? " [loop]" : "" );

    return (qhandle_t)s_numAnims;  /* 1-based */
}

/* ===========================================================================
   R_XAnimNumFrames / R_XAnimFramerate
   =========================================================================== */

int R_XAnimNumFrames( qhandle_t h )
{
    int idx = (int)h - 1;
    if ( idx < 0 || idx >= s_numAnims || !s_animTable[idx] ) return 0;
    return s_animTable[idx]->numFrames;
}

int R_XAnimFramerate( qhandle_t h )
{
    int idx = (int)h - 1;
    if ( idx < 0 || idx >= s_numAnims || !s_animTable[idx] ) return 30;
    return s_animTable[idx]->framerate;
}

/* ===========================================================================
   R_EvalXAnimBones
   =========================================================================== */

/* Binary search: return index of last keyframe with frame# <= targetFrame, or -1 */
static int FindKeyBefore( const int *frames, int numKeys, int targetFrame )
{
    int lo = 0, hi = numKeys - 1, best = -1;
    while ( lo <= hi ) {
        int mid = ( lo + hi ) / 2;
        if ( frames[mid] <= targetFrame ) { best = mid; lo = mid + 1; }
        else                              { hi = mid - 1; }
    }
    return best;
}

void R_EvalXAnimBones( qhandle_t animHandle, float fframe,
                       xmBone_t *inOutBones, int numBones )
{
    xaAnim_t *anim;
    int       i, j, frame;
    float     t;

    if ( animHandle <= 0 || animHandle > s_numAnims ) return;
    anim = s_animTable[ (int)animHandle - 1 ];
    if ( !anim ) return;

    frame = (int)fframe;
    t     = fframe - (float)frame;    /* fractional part for interpolation */

    if ( frame < 0 )                    frame = 0;
    if ( frame >= anim->numFrames - 1 ) frame = anim->numFrames - 1;

    for ( i = 0; i < anim->numParts; i++ ) {
        const xaPartAnim_t *part = &anim->parts[i];

        /* Find bone by name */
        int boneIdx = -1;
        for ( j = 0; j < numBones; j++ ) {
            if ( !Q_stricmp( inOutBones[j].name, part->partName ) ) {
                boneIdx = j;
                break;
            }
        }
        if ( boneIdx < 0 ) continue;

        /* ---- Rotation ---- */
        if ( part->rotTrack.numKeys > 0 ) {
            int ki = FindKeyBefore( part->rotTrack.frames,
                                    part->rotTrack.numKeys, frame );
            if ( ki >= 0 ) {
                /* Interpolate with next keyframe when possible */
                int next = ki + 1;
                if ( t > 0.0f && next < part->rotTrack.numKeys ) {
                    int   fn = part->rotTrack.frames[next];
                    int   f0 = part->rotTrack.frames[ki];
                    float dt = ( fn > f0 )
                        ? (float)( frame - f0 ) / (float)( fn - f0 ) + t / (float)( fn - f0 )
                        : t;
                    if ( dt > 1.0f ) dt = 1.0f;
                    Quat_Slerp( part->rotTrack.rots[ki],
                                part->rotTrack.rots[next],
                                dt, inOutBones[boneIdx].lRot );
                } else {
                    Vector4Copy( part->rotTrack.rots[ki], inOutBones[boneIdx].lRot );
                }
            }
        }

        /* ---- Translation ---- */
        if ( part->transTrack.numKeys > 0 ) {
            int ki = FindKeyBefore( part->transTrack.frames,
                                    part->transTrack.numKeys, frame );
            if ( ki >= 0 ) {
                int next = ki + 1;
                if ( t > 0.0f && next < part->transTrack.numKeys ) {
                    int   fn = part->transTrack.frames[next];
                    int   f0 = part->transTrack.frames[ki];
                    float dt = ( fn > f0 )
                        ? (float)( frame - f0 ) / (float)( fn - f0 ) + t / (float)( fn - f0 )
                        : t;
                    if ( dt > 1.0f ) dt = 1.0f;
                    VectorLerp( part->transTrack.trans[ki],
                                part->transTrack.trans[next],
                                dt, inOutBones[boneIdx].lTrans );
                } else {
                    VectorCopy( part->transTrack.trans[ki],
                                inOutBones[boneIdx].lTrans );
                }
            }
        }
    }

    /* Recompute world transforms */
    XModel_ComputeWorldBones( inOutBones, numBones );
}
