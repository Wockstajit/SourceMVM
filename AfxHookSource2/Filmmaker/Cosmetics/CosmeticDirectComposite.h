#pragma once

#include <cstddef>
#include <cstdint>

namespace Filmmaker {

struct DirectCompositeResult {
	bool resolved = false;
	bool called = false;
	bool faulted = false;
	uint32_t completedStages = 0; // only stages whose native call returned successfully
};

enum DirectCompositeStage : uint32_t {
	DirectCompositeStage_NamedAttributes = 1u << 0,
	DirectCompositeStage_OwnerMaterial = 1u << 1,
	DirectCompositeStage_MaterialSet = 1u << 2,
	DirectCompositeStage_UpdateSkin = 1u << 3,
	DirectCompositeStage_All = (1u << 4) - 1,
};

void SetDirectCompositeStageMask(uint32_t mask);
uint32_t GetDirectCompositeStageMask();

// Lazily resolves Andromeda-style direct composite refresh functions from client.dll.
bool DirectCompositeResolved();

// Composite-material owner subobject offset found by RTTI on the first composite (-1 until then).
ptrdiff_t DirectCompositeOwnerOffset();

// Drop every composite material an entity owns (CCompositeMaterialOwner found by RTTI), so it renders
// its base materials again. Used on the first-person HUD weapon when a gun goes back to its vanilla
// finish: the HUD model keeps its own copy of the last composite, which a new skin replaces but a
// vanilla pick (no new composite) never clears.
bool ReleaseEntityCompositeMaterials(unsigned char* entity);

// Diagnostic read of an entity's composite-material counts (built, in flight). False when unresolvable.
bool ReadCompositeOwnerCounts(unsigned char* entity, int* built, int* pending);

// Engine named-setter fallback for items with no networked paint attribute (viewmodel entities).
bool FireNamedSkinAttributes(unsigned char* itemView, int paintKit, float wear, int seed, int statTrak);

// Nonmutating signature/schema check before a temporary theme adds local attributes.
bool CanRemoveCosmeticLocalAttributes();

// Remove a local skin attribute through the engine (including its network notifications).
bool RemoveCosmeticLocalAttribute(unsigned char* itemView, unsigned definition);

// Direct composite refresh on a weapon entity + its embedded C_EconItemView.
DirectCompositeResult FireDirectCompositeRefresh(
	unsigned char* weapon,
	unsigned char* itemView,
	ptrdiff_t offRestoreMaterial,
	ptrdiff_t compositeOwnerOffset,
	int32_t paintKit,
	float wear,
	int32_t seed,
	bool writeNamedAttributes = true);

} // namespace Filmmaker
