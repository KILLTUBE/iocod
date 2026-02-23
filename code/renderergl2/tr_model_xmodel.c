/*
===========================================================================
tr_model_xmodel.c  --  CoD1 XMODEL loader

Reads xmodel / xmodelparts / xmodelsurfs binary files and builds a
static mdvModel_t (bind-pose) for rendering through the existing
MD3 pipeline.

Animated rendering: R_RegisterXAnim (tr_model_xanim.c) loads xanim
files; R_EvalXAnimBones evaluates bone transforms per frame.
===========================================================================
*/
#include "tr_local.h"
#include "tr_xmodel.h"

#define COD1_RIGGED		65535	/* bone index meaning per-vertex skinning */

/* ===========================================================================
   xmodelparts/<lodName>  (CoD1 V14 = 0x0E)

   Layout after version u16:
     u16  bone_count       (non-root / relative bones)
     u16  root_bone_count  (root / absolute bones, identity transform)
     for each non-root bone:
       i8    parent
       f32[3] position
       i16[3] rotation  (compact quat)
     for all (root + non-root) bones in order:
       string name
       skip 24 bytes   <- CoD1 V14 only
       -> compute world transform if parent != -1
   =========================================================================== */

static int XModel_LoadParts( const char *lodName, xmBone_t *bones, int maxBones )
{
	void	*buf;
	int		 fSize, i, boneCount, rootBoneCount, totalBones;
	xmR_t	 r;
	char	 path[MAX_QPATH];
	char	 boneName[64];

	Com_sprintf( path, sizeof(path), "xmodelparts/%s", lodName );
	fSize = ri.FS_ReadFile( path, &buf );
	if ( !buf || fSize < 6 ) {
		ri.Printf( PRINT_WARNING, "XModel_LoadParts: could not load '%s'\n", path );
		if ( buf ) ri.FS_FreeFile( buf );
		return -1;
	}

	xmR_init( &r, (const byte *)buf, fSize );

	if ( xmR_u16( &r ) != COD1_XMODEL_VERSION ) {
		ri.Printf( PRINT_WARNING, "XModel_LoadParts: bad version in '%s'\n", path );
		ri.FS_FreeFile( buf );
		return -1;
	}

	boneCount     = (int)xmR_u16( &r );
	rootBoneCount = (int)xmR_u16( &r );
	totalBones    = boneCount + rootBoneCount;

	if ( totalBones <= 0 ) {
		ri.Printf( PRINT_WARNING, "XModel_LoadParts: no bones in '%s'\n", path );
		ri.FS_FreeFile( buf );
		return -1;
	}
	if ( totalBones > maxBones )
		totalBones = maxBones;

	/* Default all to identity (root bones keep this) */
	Com_Memset( bones, 0, sizeof(xmBone_t) * totalBones );
	for ( i = 0; i < totalBones; i++ ) {
		bones[i].parent  = -1;
		bones[i].lRot[0] = 1.0f;	/* identity: w=1 */
		bones[i].wRot[0] = 1.0f;
		bones[i].name[0] = '\0';
	}

	/* Non-root bone transforms at [rootBoneCount .. totalBones-1] */
	for ( i = 0; i < boneCount; i++ ) {
		int idx = rootBoneCount + i;
		if ( idx >= maxBones ) break;
		bones[idx].parent = (int)xmR_s8( &r );
		xmR_vec3( &r, bones[idx].lTrans );
		xmR_quat( &r, bones[idx].lRot   );
	}

	/* All bone names (root first) + 24-byte skip per bone + world transform */
	for ( i = 0; i < totalBones && i < maxBones; i++ ) {
		xmR_str ( &r, boneName, sizeof(boneName) );
		xmR_skip( &r, 24 );	/* CoD1 V14: 24 extra bytes after each bone name */

		Q_strncpyz( bones[i].name, boneName, sizeof(bones[i].name) );
	}

	/* Compute world transforms from local after all names are read */
	XModel_ComputeWorldBones( bones, totalBones );

	ri.FS_FreeFile( buf );
	return totalBones;
}

/* ===========================================================================
   xmodel/<name>  (CoD1 V14 = 0x0E)

   Layout after version u16:
     skip 24  (mins + maxs)
     3 LODs:  f32 distance + null string
     skip 4   (collision LOD enum)
     u32 padding_count
     for each: u32 sub_count, skip( sub_count*48 + 36 )
     for each LOD: u16 mat_count, then mat_count strings
   =========================================================================== */

#define XMODEL_MAX_LODS	3
#define XMODEL_MAX_MATS	32

typedef struct {
	char	name[MAX_QPATH];
	char	matNames[XMODEL_MAX_MATS][MAX_QPATH];
	int		numMats;
} xmLod_t;

static qboolean XModel_LoadHeader(
	xmR_t  *r,
	xmLod_t lods[XMODEL_MAX_LODS],
	int    *numLods )
{
	int          i, j;
	unsigned int paddingCount, subCount;

	if ( xmR_u16( r ) != COD1_XMODEL_VERSION ) {
		ri.Printf( PRINT_WARNING, "XModel_LoadHeader: bad version\n" );
		return qfalse;
	}

	xmR_skip( r, 24 );		/* mins(12) + maxs(12) */

	*numLods = 0;
	for ( i = 0; i < XMODEL_MAX_LODS; i++ ) {
		char name[MAX_QPATH];
		xmR_float( r );		/* lod switch distance */
		xmR_str( r, name, sizeof(name) );
		if ( name[0] && *numLods < XMODEL_MAX_LODS ) {
			Q_strncpyz( lods[ *numLods ].name, name, MAX_QPATH );
			lods[ *numLods ].numMats = 0;
			(*numLods)++;
		}
	}

	if ( *numLods == 0 ) {
		ri.Printf( PRINT_WARNING, "XModel_LoadHeader: no valid LODs\n" );
		return qfalse;
	}

	xmR_skip( r, 4 );		/* collision LOD enum (i32) */

	paddingCount = xmR_u32( r );
	for ( i = 0; i < (int)paddingCount; i++ ) {
		subCount = xmR_u32( r );
		xmR_skip( r, (int)( subCount * 48 + 36 ) );
	}

	/* Materials per LOD */
	for ( i = 0; i < *numLods; i++ ) {
		int matCount = (int)xmR_u16( r );
		lods[i].numMats = 0;
		for ( j = 0; j < matCount; j++ ) {
			if ( j < XMODEL_MAX_MATS ) {
				xmR_str( r, lods[i].matNames[j], MAX_QPATH );
				lods[i].numMats++;
			} else {
				char tmp[MAX_QPATH];
				xmR_str( r, tmp, sizeof(tmp) );
			}
		}
	}

	return qtrue;
}

/* ===========================================================================
   xmodelsurfs/<lodName>  (CoD1 V14 = 0x0E)

   Surface layout:
     skip 1
     u16 vertex_count
     u16 triangle_count
     skip 2
     u16 og_default_bone_idx  (65535 = RIGGED = per-vertex bones)
     if RIGGED: skip 4

     -- Triangles FIRST (fan-encoded loop) --
     loop until triangle_count triangles decoded:
       u8 idx_count
       u16 idx1, idx2, idx3   (first tri, reversed winding)
       for extra indices:  alternating fan pattern

     -- Vertices --
     for each vertex:
       f32[3] normal
       f32[2] uv
       if RIGGED: u16 weight_count, u16 bone_idx
       f32[3] position
       if weight_count != 0: skip 4

     -- Bone weights (after ALL vertices) --
     for each vertex, for j in 0..weight_count[i]:
       u16 blend_bone_idx
       skip 12  (vec3 blend offset)
       f32 weight_influence  (/ 65535.0)
   =========================================================================== */

static int XModel_DecodeFanStrip(
	xmR_t		*r,
	glIndex_t	*outIdx,
	int			 maxTris,
	int			 alreadyWritten )
{
	int		 written = 0;
	unsigned idx_count;
	glIndex_t idx1, idx2, idx3, idx4, idx5;
	unsigned i;

	idx_count = (unsigned)xmR_u8( r );
	idx1 = (glIndex_t)xmR_u16( r );
	idx2 = (glIndex_t)xmR_u16( r );
	idx3 = (glIndex_t)xmR_u16( r );

	/* First triangle — reversed winding to match CoD1 (D3D CW → GL CCW) */
	if ( idx1 != idx2 && idx1 != idx3 && idx2 != idx3
		&& alreadyWritten + written < maxTris ) {
		outIdx[ written*3+0 ] = idx3;
		outIdx[ written*3+1 ] = idx2;
		outIdx[ written*3+2 ] = idx1;
		written++;
	}

	i = 3;
	while ( i < idx_count ) {
		idx4 = idx3;
		idx5 = (glIndex_t)xmR_u16( r );

		if ( idx4 != idx2 && idx4 != idx5 && idx2 != idx5
			&& alreadyWritten + written < maxTris ) {
			outIdx[ written*3+0 ] = idx5;
			outIdx[ written*3+1 ] = idx2;
			outIdx[ written*3+2 ] = idx4;
			written++;
		}

		i++;
		if ( i >= idx_count ) break;

		idx2 = idx5;
		idx3 = (glIndex_t)xmR_u16( r );

		if ( idx4 != idx2 && idx4 != idx3 && idx2 != idx3
			&& alreadyWritten + written < maxTris ) {
			outIdx[ written*3+0 ] = idx3;
			outIdx[ written*3+1 ] = idx2;
			outIdx[ written*3+2 ] = idx4;
			written++;
		}

		i++;
	}

	return written;
}

static qboolean XModel_LoadSurfs(
	const char		*lodName,
	const xmBone_t	*bones,
	int				 numBones,
	mdvModel_t		*mdvModel,
	const xmLod_t	*lod,
	xmSkinData_t	*skinOut )
{
	void			*buf;
	int				 fSize;
	xmR_t			 r;
	char			 path[MAX_QPATH];
	int				 i, j, k;
	int				 numSurfs;
	mdvSurface_t	*surf;
	mdvFrame_t		*frame;
	vec3_t			 modelMins, modelMaxs;

	Com_sprintf( path, sizeof(path), "xmodelsurfs/%s", lodName );
	fSize = ri.FS_ReadFile( path, &buf );
	if ( !buf || fSize < 4 ) {
		ri.Printf( PRINT_WARNING, "XModel_LoadSurfs: could not load '%s'\n", path );
		if ( buf ) ri.FS_FreeFile( buf );
		return qfalse;
	}

	xmR_init( &r, (const byte *)buf, fSize );

	if ( xmR_u16( &r ) != COD1_XMODEL_VERSION ) {
		ri.Printf( PRINT_WARNING, "XModel_LoadSurfs: bad version in '%s'\n", path );
		ri.FS_FreeFile( buf );
		return qfalse;
	}

	numSurfs = (int)xmR_u16( &r );
	if ( numSurfs <= 0 ) {
		ri.Printf( PRINT_WARNING, "XModel_LoadSurfs: no surfaces in '%s'\n", path );
		ri.FS_FreeFile( buf );
		return qfalse;
	}

	mdvModel->numFrames      = 1;
	mdvModel->frames         = frame = (mdvFrame_t *)ri.Hunk_Alloc( sizeof(mdvFrame_t), h_low );
	mdvModel->numTags        = 0;
	mdvModel->tags           = NULL;
	mdvModel->tagNames       = NULL;
	mdvModel->numSurfaces    = numSurfs;
	mdvModel->surfaces       = surf = (mdvSurface_t *)ri.Hunk_Alloc( sizeof(mdvSurface_t) * numSurfs, h_low );
	mdvModel->numVaoSurfaces = 0;
	mdvModel->vaoSurfaces    = NULL;
	mdvModel->numSkins       = 0;

	/* Initialise the skin output structure */
	skinOut->numSurfaces = numSurfs;
	skinOut->surfs       = (xmSkinSurf_t *)ri.Malloc( sizeof(xmSkinSurf_t) * numSurfs );
	skinOut->mdvModel    = mdvModel;
	Com_Memset( skinOut->surfs, 0, sizeof(xmSkinSurf_t) * numSurfs );

	ClearBounds( modelMins, modelMaxs );

	for ( i = 0; i < numSurfs; i++, surf++ ) {
		int				 matIdx;
		int				 vertCount, triCount;
		int				 ogBoneIdx;
		qboolean		 rigged;
		int			   *shaderIdx;
		glIndex_t		*indexes;
		mdvVertex_t		*verts;
		mdvSt_t			*st;
		unsigned short	*boneWeightCounts;
		unsigned short	*boneIdxPerVert;

		surf->surfaceType = SF_MDV;
		surf->model       = mdvModel;
		Com_sprintf( surf->name, sizeof(surf->name), "surf%d", i );

		matIdx = ( lod->numMats > 0 ) ? ( i < lod->numMats ? i : lod->numMats - 1 ) : 0;
		surf->numShaderIndexes = 1;
		surf->shaderIndexes = shaderIdx = (int *)ri.Hunk_Alloc( sizeof(int), h_low );
		if ( lod->numMats > 0 ) {
			/*
			 * CoD1 material names include the extension, e.g. "viewhands@default.jpg".
			 * Prefix with "skins/" to form the full VFS path.
			 *
			 * "@default" is a 4×4 white placeholder used as a material template.
			 * CoD1 substitutes the correct faction skin at runtime (e.g. @hand, @vsleeve_waffen).
			 * We try "@hand" first as the base skin texture, falling back to "@default".
			 *
			 * Use LIGHTMAP_WHITEIMAGE so surfaces render fullbright (unlit) in the
			 * NOWORLDMODEL pass — CGEN_IDENTITY_LIGHTING in GL2 does not apply
			 * entity ambient, so the texture colour is preserved without over-brightening.
			 */
			const char *matName = lod->matNames[matIdx];
			const char *defAt   = strstr( matName, "@default" );
			shader_t   *sh      = NULL;

			if ( defAt ) {
				/* Try replacing "@default[.ext]" with "@hand" */
				char altPath[MAX_QPATH];
				Com_sprintf( altPath, sizeof(altPath), "skins/%.*s@hand",
				             (int)(defAt - matName), matName );
				sh = R_FindShader( altPath, LIGHTMAP_WHITEIMAGE, qtrue );
				if ( sh->defaultShader ) sh = NULL;
			}
			if ( !sh ) {
				char skinPath[MAX_QPATH];
				Com_sprintf( skinPath, sizeof(skinPath), "skins/%s", matName );
				sh = R_FindShader( skinPath, LIGHTMAP_WHITEIMAGE, qtrue );
			}
			/* CoD1 viewmodel geometry wraps around the camera; always use
			 * two-sided rendering so inner surfaces are visible. */
			sh->cullType = CT_TWO_SIDED;
			shaderIdx[0] = sh->defaultShader ? 0 : sh->index;
		} else {
			shaderIdx[0] = 0;
		}

		/* Surface header */
		xmR_skip( &r, 1 );
		vertCount  = (int)xmR_u16( &r );
		triCount   = (int)xmR_u16( &r );
		xmR_skip( &r, 2 );
		ogBoneIdx  = (int)xmR_u16( &r );
		rigged     = ( ogBoneIdx == COD1_RIGGED ) ? qtrue : qfalse;
		if ( rigged )
			xmR_skip( &r, 4 );

		surf->numVerts   = vertCount;
		surf->numIndexes = triCount * 3;

		surf->indexes = indexes = (glIndex_t  *)ri.Hunk_Alloc( sizeof(glIndex_t)   * triCount * 3, h_low );
		surf->verts   = verts   = (mdvVertex_t *)ri.Hunk_Alloc( sizeof(mdvVertex_t) * vertCount,    h_low );
		surf->st      = st      = (mdvSt_t     *)ri.Hunk_Alloc( sizeof(mdvSt_t)     * vertCount,    h_low );

		/* Initialise skinning surface entry */
		skinOut->surfs[i].numVerts  = vertCount;
		skinOut->surfs[i].verts     = (xmSkinVert_t *)ri.Malloc( sizeof(xmSkinVert_t) * vertCount );
		skinOut->surfs[i].mdvVerts  = verts;   /* points into Hunk — updated in-place each frame */

		/* Triangles (fan-encoded, come BEFORE vertices in V14) */
		{
			int written = 0;
			while ( written < triCount ) {
				int got = XModel_DecodeFanStrip( &r, indexes + written*3,
				                                 triCount, written );
				if ( got <= 0 ) break;
				written += got;
			}
		}

		/* Per-vertex bone tracking for weight pass */
		boneWeightCounts = (unsigned short *)ri.Malloc( sizeof(unsigned short) * vertCount );
		boneIdxPerVert   = (unsigned short *)ri.Malloc( sizeof(unsigned short) * vertCount );

		/* Vertices */
		for ( j = 0; j < vertCount; j++ ) {
			vec3_t		 localNormal, localPos, worldPos, worldNormal;
			unsigned short weightCount;
			int			 boneIdx;

			weightCount = 0;

			xmR_vec3( &r, localNormal );	/* normal (no negation in V14) */
			st[j].st[0] = xmR_float( &r );
			st[j].st[1] = 1.0f - xmR_float( &r );	/* V14: flip V (D3D→GL convention) */

			if ( rigged ) {
				weightCount = xmR_u16( &r );	/* u16 in V14 */
				boneIdx     = (int)xmR_u16( &r );
			} else {
				boneIdx = ogBoneIdx;
			}

			xmR_vec3( &r, localPos );

			if ( weightCount != 0 )
				xmR_skip( &r, 4 );

			boneWeightCounts[j] = weightCount;
			boneIdxPerVert[j]   = (unsigned short)boneIdx;

			/* Save bone-local data for CPU re-skinning */
			VectorCopy( localPos,    skinOut->surfs[i].verts[j].localPos    );
			VectorCopy( localNormal, skinOut->surfs[i].verts[j].localNormal );
			skinOut->surfs[i].verts[j].boneIdx = boneIdx;

			if ( boneIdx >= 0 && boneIdx < numBones ) {
				const xmBone_t *b = &bones[boneIdx];
				Quat_RotVec( b->wRot, localPos,    worldPos    );
				VectorAdd  ( b->wTrans, worldPos,  worldPos    );
				Quat_RotVec( b->wRot, localNormal, worldNormal );
			} else {
				VectorCopy( localPos,    worldPos    );
				VectorCopy( localNormal, worldNormal );
			}

			VectorNormalize( worldNormal );
			VectorCopy( worldPos, verts[j].xyz );
			R_VaoPackNormal( verts[j].normal, worldNormal );

			/* No tangent data in V14 — zero out */
			verts[j].tangent[0] = 0;
			verts[j].tangent[1] = 0;
			verts[j].tangent[2] = 0;
			verts[j].tangent[3] = 0;

			AddPointToBounds( worldPos, modelMins, modelMaxs );
		}

		/* Bone weights (after ALL vertices in V14) */
		for ( j = 0; j < vertCount; j++ ) {
			for ( k = 0; k < (int)boneWeightCounts[j]; k++ ) {
				xmR_u16  ( &r );		/* blend bone index */
				xmR_skip ( &r, 12 );	/* blend offset (vec3) */
				xmR_float( &r );		/* weight influence f32 / 65535.0 */
			}
		}

		ri.Free( boneWeightCounts );
		ri.Free( boneIdxPerVert   );
	}

	/* Frame bounds */
	VectorCopy ( modelMins, frame->bounds[0] );
	VectorCopy ( modelMaxs, frame->bounds[1] );
	VectorAdd  ( modelMins, modelMaxs, frame->localOrigin );
	VectorScale( frame->localOrigin, 0.5f, frame->localOrigin );
	frame->radius  = RadiusFromBounds( modelMins, modelMaxs );

	ri.FS_FreeFile( buf );
	return qtrue;
}

/* ===========================================================================
   Public entry point
   =========================================================================== */

qhandle_t R_RegisterXModel( const char *name, model_t *mod )
{
	void		*buf;
	int			 fSize;
	xmR_t		 r;
	xmLod_t		 lods[XMODEL_MAX_LODS];
	int			 numLods = 0;
	xmBone_t	*bones;
	int			 numBones;
	mdvModel_t	*mdvModel;

	fSize = ri.FS_ReadFile( name, &buf );
	if ( !buf || fSize < 4 ) {
		ri.Printf( PRINT_DEVELOPER, "R_RegisterXModel: could not load '%s'\n", name );
		if ( buf ) ri.FS_FreeFile( buf );
		mod->type = MOD_BAD;
		return 0;
	}

	xmR_init( &r, (const byte *)buf, fSize );
	if ( !XModel_LoadHeader( &r, lods, &numLods ) ) {
		ri.FS_FreeFile( buf );
		mod->type = MOD_BAD;
		return 0;
	}
	ri.FS_FreeFile( buf );

	bones = (xmBone_t *)ri.Malloc( sizeof(xmBone_t) * XMODEL_MAX_BONES );
	Com_Memset( bones, 0, sizeof(xmBone_t) * XMODEL_MAX_BONES );

	numBones = XModel_LoadParts( lods[0].name, bones, XMODEL_MAX_BONES );
	if ( numBones <= 0 ) {
		ri.Free( bones );
		mod->type = MOD_BAD;
		return 0;
	}

	mod->type    = MOD_MESH;
	mod->numLods = 1;
	mdvModel     = mod->mdv[0] = (mdvModel_t *)ri.Hunk_Alloc( sizeof(mdvModel_t), h_low );

	{
		xmSkinData_t *skinData = (xmSkinData_t *)ri.Malloc( sizeof(xmSkinData_t) );
		Com_Memset( skinData, 0, sizeof(xmSkinData_t) );

		if ( !XModel_LoadSurfs( lods[0].name, bones, numBones, mdvModel, &lods[0], skinData ) ) {
			ri.Free( skinData );
			ri.Free( bones );
			mod->type = MOD_BAD;
			return 0;
		}

		/* Allocate mdvTag entries for every bone whose name begins with "tag_"
		 * so that re.LerpTag works on xmodel handles (R_GetTag → mdvTag lookup).
		 * R_UpdateXModelPose updates these in-place each frame. */
		{
			int          numTagBones = 0, tagIdx = 0, bi;
			mdvTag_t     *tags;
			mdvTagName_t *tagNames;

			for ( bi = 0; bi < numBones; bi++ )
				if ( Q_strncmp( bones[bi].name, "tag_", 4 ) == 0 )
					numTagBones++;

			if ( numTagBones > 0 ) {
				tags     = (mdvTag_t     *)ri.Hunk_Alloc( sizeof(mdvTag_t)     * numTagBones, h_low );
				tagNames = (mdvTagName_t *)ri.Hunk_Alloc( sizeof(mdvTagName_t) * numTagBones, h_low );

				for ( bi = 0; bi < numBones; bi++ ) {
					if ( Q_strncmp( bones[bi].name, "tag_", 4 ) == 0 ) {
						VectorCopy( bones[bi].wTrans, tags[tagIdx].origin );
						Quat_ToAxis( bones[bi].wRot,  tags[tagIdx].axis   );
						Q_strncpyz( tagNames[tagIdx].name, bones[bi].name, MAX_QPATH );
						tagIdx++;
					}
				}

				mdvModel->numTags  = numTagBones;
				mdvModel->tags     = tags;
				mdvModel->tagNames = tagNames;
				/* Also point skinData so R_UpdateXModelPose can update tags */
				skinData->mdvModel = mdvModel;
			}
		}

		/* Cache bind pose and skin data for per-frame evaluation */
		R_StoreXModelBindPose( (qhandle_t)mod->index, bones, numBones );
		R_StoreXModelSkinData( (qhandle_t)mod->index, skinData );
	}

	ri.Free( bones );

	ri.Printf( PRINT_DEVELOPER,
	           "R_RegisterXModel: '%s'  surfs=%d bones=%d tags=%d\n",
	           name, mdvModel->numSurfaces, numBones, mdvModel->numTags );

	return mod->index;
}
