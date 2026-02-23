/*
===========================================================================
tr_xmodel.h  --  Shared types and inline utilities for CoD1 xmodel/xanim

Included by tr_model_xmodel.c and tr_model_xanim.c.
===========================================================================
*/
#pragma once
#include <math.h>

/* ===========================================================================
   Constants
   =========================================================================== */

#define XMODEL_MAX_BONES    256
#define COD1_XMODEL_VERSION 0x0E

/* ===========================================================================
   Bone type (bind pose or evaluated animation pose)
   =========================================================================== */

typedef struct {
    int    parent;      /* -1 = root */
    char   name[64];    /* bone name */
    vec3_t lTrans;      /* local translation */
    vec4_t lRot;        /* local rotation [w,x,y,z] (Hamilton) */
    vec3_t wTrans;      /* world translation (computed) */
    vec4_t wRot;        /* world rotation (computed) */
} xmBone_t;

/* ===========================================================================
   Binary reader  (little-endian, sequential)
   =========================================================================== */

typedef struct {
    const byte *buf;
    int         pos;
    int         size;
} xmR_t;

static inline void xmR_init( xmR_t *r, const byte *buf, int size )
{
    r->buf = buf; r->pos = 0; r->size = size;
}

static inline byte xmR_u8( xmR_t *r )
{
    return ( r->pos < r->size ) ? r->buf[ r->pos++ ] : 0;
}

static inline signed char xmR_s8( xmR_t *r )
{
    return (signed char)xmR_u8( r );
}

static inline unsigned short xmR_u16( xmR_t *r )
{
    unsigned short v;
    if ( r->pos + 2 > r->size ) { r->pos = r->size; return 0; }
    v = (unsigned short)( r->buf[r->pos] | ( r->buf[r->pos+1] << 8 ) );
    r->pos += 2;
    return v;
}

static inline short xmR_s16( xmR_t *r )
{
    return (short)xmR_u16( r );
}

static inline unsigned int xmR_u32( xmR_t *r )
{
    unsigned int v;
    if ( r->pos + 4 > r->size ) { r->pos = r->size; return 0; }
    v = (unsigned int)( r->buf[r->pos]
        | ( r->buf[r->pos+1] << 8  )
        | ( r->buf[r->pos+2] << 16 )
        | ( r->buf[r->pos+3] << 24 ) );
    r->pos += 4;
    return v;
}

static inline float xmR_float( xmR_t *r )
{
    union { unsigned int i; float f; } u;
    u.i = xmR_u32( r );
    return u.f;
}

static inline void xmR_vec3( xmR_t *r, vec3_t out )
{
    out[0] = xmR_float( r );
    out[1] = xmR_float( r );
    out[2] = xmR_float( r );
}

static inline void xmR_skip( xmR_t *r, int n )
{
    r->pos += n;
    if ( r->pos > r->size ) r->pos = r->size;
}

/* Read null-terminated string. Returns char count (0 = empty). */
static inline int xmR_str( xmR_t *r, char *buf, int maxlen )
{
    int  i = 0;
    byte c;
    while ( ( c = xmR_u8( r ) ) != 0 ) {
        if ( i < maxlen - 1 ) buf[ i++ ] = (char)c;
    }
    buf[i] = '\0';
    return i;
}

/*
 * CoD1 compact quaternion (3 × int16 / 32768.0 → x,y,z; w = sqrt(1-xx-yy-zz))
 * Result: [w, x, y, z]  (Hamilton convention)
 */
static inline void xmR_quat( xmR_t *r, vec4_t q )
{
    float x, y, z, ww;
    x  = xmR_s16( r ) / 32768.0f;
    y  = xmR_s16( r ) / 32768.0f;
    z  = xmR_s16( r ) / 32768.0f;
    ww = 1.0f - x*x - y*y - z*z;
    q[0] = ( ww > 0.0f ) ? (float)sqrt( (double)ww ) : 0.0f;
    q[1] = x; q[2] = y; q[3] = z;
}

/* ===========================================================================
   Quaternion math  (Hamilton: q = [w, x, y, z])
   =========================================================================== */

/* out = a * b */
static inline void Quat_Mul( const vec4_t a, const vec4_t b, vec4_t out )
{
    out[0] = a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3];
    out[1] = a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2];
    out[2] = a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1];
    out[3] = a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0];
}

/* out = rotate v by unit quaternion q */
static inline void Quat_RotVec( const vec4_t q, const vec3_t v, vec3_t out )
{
    float qw = q[0], qx = q[1], qy = q[2], qz = q[3];
    float vx = v[0], vy = v[1], vz = v[2];
    float tx = 2.0f * ( qy*vz - qz*vy );
    float ty = 2.0f * ( qz*vx - qx*vz );
    float tz = 2.0f * ( qx*vy - qy*vx );
    out[0] = vx + qw*tx + qy*tz - qz*ty;
    out[1] = vy + qw*ty + qz*tx - qx*tz;
    out[2] = vz + qw*tz + qx*ty - qy*tx;
}

/* Spherical linear interpolation between two unit quaternions, t in [0,1] */
static inline void Quat_Slerp( const vec4_t a, const vec4_t b, float t, vec4_t out )
{
    float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    float s0, s1;
    /* Use b2 = -b if dot < 0 to take shortest path */
    float bw = b[0], bx = b[1], by = b[2], bz = b[3];
    if ( dot < 0.0f ) { bw=-bw; bx=-bx; by=-by; bz=-bz; dot=-dot; }
    if ( dot > 0.9995f ) {
        /* Quaternions nearly identical: linear interpolate */
        out[0] = a[0] + t*(bw-a[0]);
        out[1] = a[1] + t*(bx-a[1]);
        out[2] = a[2] + t*(by-a[2]);
        out[3] = a[3] + t*(bz-a[3]);
    } else {
        float theta_0 = (float)acos( (double)dot );
        float theta   = theta_0 * t;
        float sin_t   = (float)sin( (double)theta );
        float sin_t0  = (float)sin( (double)theta_0 );
        s0 = (float)cos( (double)theta ) - dot * sin_t / sin_t0;
        s1 = sin_t / sin_t0;
        out[0] = a[0]*s0 + bw*s1;
        out[1] = a[1]*s0 + bx*s1;
        out[2] = a[2]*s0 + by*s1;
        out[3] = a[3]*s0 + bz*s1;
    }
    /* Normalize */
    {
        float len = out[0]*out[0]+out[1]*out[1]+out[2]*out[2]+out[3]*out[3];
        if ( len > 0.0f ) {
            float inv = 1.0f / (float)sqrt( (double)len );
            out[0] *= inv; out[1] *= inv; out[2] *= inv; out[3] *= inv;
        }
    }
}

/* Hamiltonian quaternion to 3x3 axis matrix */
static inline void Quat_ToAxis( const vec4_t q, vec3_t axis[3] )
{
    float w = q[0], x = q[1], y = q[2], z = q[3];
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2;
    float yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;

    axis[0][0] = 1.0f - (yy + zz);
    axis[0][1] = xy + wz;
    axis[0][2] = xz - wy;

    axis[1][0] = xy - wz;
    axis[1][1] = 1.0f - (xx + zz);
    axis[1][2] = yz + wx;

    axis[2][0] = xz + wy;
    axis[2][1] = yz - wx;
    axis[2][2] = 1.0f - (xx + yy);
}

/* ===========================================================================
   CPU skinning data  (per-vertex local position for re-skinning each frame)
   =========================================================================== */

typedef struct {
    vec3_t  localPos;      /* vertex position in bone-local space */
    vec3_t  localNormal;   /* normal in bone-local space */
    int     boneIdx;       /* which bone this vertex is weighted to */
} xmSkinVert_t;

typedef struct {
    int          numVerts;
    xmSkinVert_t *verts;      /* ri.Malloc'd */
    mdvVertex_t  *mdvVerts;   /* into mdvModel Hunk -- updated in-place each frame */
} xmSkinSurf_t;

typedef struct {
    int           numSurfaces;
    xmSkinSurf_t *surfs;      /* ri.Malloc'd */
    mdvModel_t   *mdvModel;   /* pointer into Hunk -- tags updated in-place */
} xmSkinData_t;

/* ===========================================================================
   Shared function declarations (defined in tr_model_xanim.c)
   =========================================================================== */

/* Re-compute world transforms for all bones from their local transforms.
 * Traverses the parent chain; parent indices must be < i for correct results. */
void XModel_ComputeWorldBones( xmBone_t *bones, int numBones );

/* Store a copy of the bind-pose bones for a loaded xmodel.
 * Called by R_RegisterXModel after xmodelparts is parsed. */
void R_StoreXModelBindPose( qhandle_t modelHandle,
                             const xmBone_t *bones, int numBones );

/* Store per-vertex local skinning data for CPU re-skinning.
 * Called by R_RegisterXModel after XModel_LoadSurfs. */
void R_StoreXModelSkinData( qhandle_t modelHandle, xmSkinData_t *data );

/* Evaluate animation at 'frame', CPU-skin all vertices and update mdvTags.
 * Safe to call every frame before AddRefEntityToScene. */
void R_UpdateXModelPose( qhandle_t modelHandle, qhandle_t animHandle, float frame );

/* Look up a bone by name in the current animated pose (or bind-pose fallback)
 * and return its world-space orientation.  Returns 1 if found, 0 otherwise. */
int R_XModelLerpTag( orientation_t *tag, qhandle_t handle,
                      int startFrame, int endFrame,
                      float frac, const char *tagName );

/* Load an xanim binary file.  Path may omit the "xanim/" prefix.
 * Returns a non-zero handle on success, 0 on failure. */
qhandle_t R_RegisterXAnim( const char *name );

/* Evaluate an animation at the given (possibly fractional) frame.
 * inOutBones must be pre-filled with the bind-pose local transforms (lTrans, lRot).
 * Animation data overrides only bones whose names match part names in the anim.
 * World transforms (wTrans, wRot) are recomputed on return.
 * numBones: number of entries in inOutBones. */
void R_EvalXAnimBones( qhandle_t animHandle, float frame,
                       xmBone_t *inOutBones, int numBones );

/* Return total frame count for the animation (0 if handle invalid). */
int  R_XAnimNumFrames( qhandle_t animHandle );

/* Return frames-per-second for the animation (30 if handle invalid). */
int  R_XAnimFramerate( qhandle_t animHandle );
