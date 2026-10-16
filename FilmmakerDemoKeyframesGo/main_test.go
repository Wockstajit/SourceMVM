package main

import "testing"

// The Valve full-packet check must not fail a long match on scoreboard bookkeeping (it grows with
// every round), and must still count anything that could be a tracking bug.
func TestValveDiffExplained(t *testing.T) {
	explained := []string{
		// The breakdown of the 10-player match that failed at tick 65281 (350 of 27805 fields).
		"bits-differ CCSPlayerController.m_pActionTrackingServices.m_perRoundStats.0005.m_iKills",
		"bits-differ CCSPlayerController.m_pActionTrackingServices.m_perRoundStats.0008.m_iMoneySaved",
		"tracked-only CCSPlayerController.m_pActionTrackingServices.m_iDamage",
		"tracked-only CCSPlayerController.m_pDamageServices.m_DamageList",
		"tracked-only CCSPlayerController.m_pDamageServices.m_nSendUpdate",
		"tracked-only CWeaponGlock.m_flAnimTime",
		"bits-differ CCSPlayerPawn.m_flSimulationTime",
	}
	for _, k := range explained {
		if !valveDiffExplained(k) {
			t.Errorf("%q should be explained", k)
		}
	}
	counted := []string{
		"bits-differ CCSPlayerPawn.m_vecOrigin",
		"bits-differ CCSPlayerPawn.m_iHealth",
		"other-only CSmokeGrenadeProjectile.m_nSmokeEffectTickBegin",
		"tracked-only CCSPlayerController.m_iPawnHealth",
		"entity missing in other: CCSPlayerPawn",
	}
	for _, k := range counted {
		if valveDiffExplained(k) {
			t.Errorf("%q must count toward the 1%% bar", k)
		}
	}
}
