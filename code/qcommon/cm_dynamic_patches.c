#include "cm_dynamic_patches.h"
#include "cm_patch.h"
struct patchCollide_s dynamicPatchCollides[MAX_DYNAMIC_PATCHES];
int numDynamicPatches = 0;
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
