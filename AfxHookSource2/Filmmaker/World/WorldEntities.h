#pragma once

// Shared entity-class matching for the World/Effects features.
//
// WHY THIS EXISTS (measured 2026-08-04 on Mirage, via `mirv_listentities`):
// map entities like the post-processing volume, env_sky and the fog controllers do NOT report a
// leaf client class. Their columns come back as:
//
//   107 / ... / post_processing_volume / post_processing_volume / C_BaseModelEntity / [...]
//   380 / ... / mirage_sky             / env_sky                / C_BaseModelEntity / [...]
//   381 / ... / env_cubemap_fog        / env_cubemap_fog        / C_BaseEntity      / [...]
//                                        ^^ GetClassName()       ^^ GetClientClassName()
//
// So GetClientClassName() yields the generic networked base (C_BaseModelEntity / C_BaseEntity),
// and the useful identity is GetClassName() -- the ENTITY class ("post_processing_volume").
// Matching on the schema-style "C_PostProcessingVolume" name finds nothing at all, which is
// exactly the silent-no-op failure this codebase treats as a bug rather than a quirk.
//
// Every matcher here therefore tests BOTH names against a caller-supplied list, so a feature
// keeps working whether Valve reports the entity class, the client class, or (after some future
// update) swaps which one is populated.

#include <cstddef>

class CEntityInstance;

namespace Filmmaker {

// True if the entity's GetClassName() OR GetClientClassName() case-insensitively equals any
// entry in `names`. Exception-safe: an entity that faults while being read simply does not match.
bool WorldEntity_Matches(CEntityInstance* ent, const char* const* names, std::size_t count);

// Number of live entities matching any name in `names`. Returns -1 when there is no entity list
// (not in a demo/map), which is a different answer from 0 ("in a map, none present").
int WorldEntity_Count(const char* const* names, std::size_t count);

} // namespace Filmmaker
