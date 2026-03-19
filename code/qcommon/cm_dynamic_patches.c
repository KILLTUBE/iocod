#include "cm_dynamic_patches.h"
#include "cm_patch.h"
struct patchCollide_s dynamicPatchCollides[MAX_DYNAMIC_PATCHES];
int numDynamicPatches = 0;
dynamicPatchInstance_t dynamicPatchInstances[MAX_DYNAMIC_PATCH_INSTANCES];
int numDynamicPatchInstances = 0;
// Fixed at 0,0,0 but requires less trace work math
int CM_AddDynamicPatch(patchCollide_t *pc) {
  if (!pc) {
    Com_Printf("Cannot add dynamic patch: pc == NULL\n");
    return -1;
  }
  if (numDynamicPatches >= MAX_DYNAMIC_PATCHES) {
    Com_Printf("Cannot add dynamic patch: numDynamicPatches=%d >= MAX_DYNAMIC_PATCHES=%d\n", numDynamicPatches, MAX_DYNAMIC_PATCHES);
    return -1;
  }
  int id = numDynamicPatches;
  dynamicPatchCollides[id] = *pc;
  numDynamicPatches++;
  return id;
}
// Dynamic but requires more trace work math
dynamicPatchInstance_t *CM_AddDynamicPatchInstance(patchCollide_t *pc, const vec3_t origin) {
  if (!pc || numDynamicPatchInstances >= MAX_DYNAMIC_PATCH_INSTANCES) {
    Com_Printf("Cannot add dynamic patch instance\n");
    return NULL;
  }
  dynamicPatchInstance_t *dpi = &dynamicPatchInstances[numDynamicPatchInstances++];
  dpi->pc = pc;
  VectorCopy(origin, dpi->origin);
  return dpi;
}
