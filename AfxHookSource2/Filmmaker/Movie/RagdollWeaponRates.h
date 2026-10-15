#pragma once
#include <cstring>

namespace Filmmaker {
// Generated from installed CS2 weapon data. Regenerate after weapon balance updates.
// weapons.vdata SHA256: fbd0d6f754c234efb24ec5c6b8c335f190e67f17ac7ec98dc2cd2ea2ddb4037e
struct RagdollWeaponFireProfile { unsigned item; const char* name; double cycle; bool singleShot; };
inline const RagdollWeaponFireProfile* RagdollWeaponProfile(unsigned item, const char* name) {
	static constexpr RagdollWeaponFireProfile profiles[] = {
		{1,"weapon_deagle",0.225,false},
		{2,"weapon_elite",0.12,false},
		{3,"weapon_fiveseven",0.15,false},
		{4,"weapon_glock",0.15,false},
		{7,"weapon_ak47",0.1,false},
		{8,"weapon_aug",0.1,false},
		{9,"weapon_awp",1.455,true},
		{10,"weapon_famas",0.09,false},
		{11,"weapon_g3sg1",0.25,true},
		{13,"weapon_galilar",0.09,false},
		{14,"weapon_m249",0.08,false},
		{16,"weapon_m4a1",0.09,false},
		{17,"weapon_mac10",0.075,false},
		{19,"weapon_p90",0.07,false},
		{23,"weapon_mp5sd",0.08,false},
		{24,"weapon_ump45",0.09,false},
		{25,"weapon_xm1014",0.35,false},
		{26,"weapon_bizon",0.08,false},
		{27,"weapon_mag7",0.85,false},
		{28,"weapon_negev",0.075,false},
		{29,"weapon_sawedoff",0.85,false},
		{30,"weapon_tec9",0.12,false},
		{32,"weapon_hkp2000",0.17,false},
		{33,"weapon_mp7",0.08,false},
		{34,"weapon_mp9",0.07,false},
		{35,"weapon_nova",0.88,false},
		{36,"weapon_p250",0.15,false},
		{38,"weapon_scar20",0.25,true},
		{39,"weapon_sg556",0.11,false},
		{40,"weapon_ssg08",1.25,true},
		{60,"weapon_m4a1_silencer",0.1,false},
		{61,"weapon_usp_silencer",0.17,false},
		{63,"weapon_cz75a",0.1,false},
		{64,"weapon_revolver",0.5,false},
	};
	for(const auto& p:profiles) if(item && p.item==item) return &p;
	for(const auto& p:profiles) if(name && !std::strcmp(name,p.name)) return &p;
	return nullptr;
}
}
