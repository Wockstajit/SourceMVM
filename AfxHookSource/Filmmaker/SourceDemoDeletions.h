#pragma once

// Entities a SourceTV demo deletes while it still sends them.
//
// A delta packet's entity updates end with a list of entities to delete, and the engine deletes each
// one (CClientState::ReadDeletions, then CL_DeleteDLLEntity). The SDK only asserts that a listed
// entity is not one the packet's new frame still sends. CS:S's SourceTV breaks that at a round
// restart. Measured in a 10-bot SourceTV demo on 2026-10-27: the packet at the second round_start
// lists four CWeaponGlocks lying on the ground for deletion while the same frame keeps them. The
// engine deletes them, and the next packet, which keeps them unchanged, stops the game with
// "Host_Error: CL_PreserveExistingEntity: missing client entity 71". The game without the hook stops
// the same way.
//
// The hook wraps ReadDeletions: the CClientState virtual that passes "ReadDeletions" to
// CL_DeleteDLLEntity, so a game update that moves the slot is still found. Before the list is read,
// it reads the list ahead. For an entity the new frame still sends, the client entity list's
// GetClientNetworkable answers null on that thread while the list is read, and CL_DeleteDLLEntity
// leaves an entity it cannot find alone. Every other deletion goes through unchanged.

namespace SourceFilmmaker {

// Installs both wraps. Safe to call every frame; it retries until engine.dll and the client entity
// list are there.
void DemoDeletions_Init();

// One line for mirv_filmmaker health.
void DemoDeletions_PrintHealth();

} // namespace SourceFilmmaker
