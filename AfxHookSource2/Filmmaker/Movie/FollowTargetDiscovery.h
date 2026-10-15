#pragma once

#include "FollowCamera.h"

// Candidate discovery for the Follow Camera: enumerates followable players/weapons/grenades
// (FollowCamera::Candidates), the live grenade cache, target selection (SelectCandidate/
// SelectNearest/SelectEntity/SelectHandle), grenade throw-tracking, and the on-death retarget
// fallback. Split out of FollowCamera.cpp; declarations live on the FollowCamera class itself
// in FollowCamera.h, so this header just pulls that in for FollowTargetDiscovery.cpp.
