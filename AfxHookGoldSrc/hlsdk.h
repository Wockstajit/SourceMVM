#pragma once

// BEGIN HLSDK includes
#pragma push_macro("ARRAYSIZE")
#ifdef ARRAYSIZE
#undef ARRAYSIZE
#endif
#pragma push_macro("offsetof")
#ifdef offsetof
#undef offsetof
#endif
#pragma push_macro("HSPRITE")
#define HSPRITE MDTHACKED_HSPRITE
//
#include <Deps/release/halflife/cl_dll/wrect.h>
#include <Deps/release/halflife/cl_dll/cl_dll.h>
#include <Deps/release/halflife/cl_dll/in_defs.h> // PITCH YAW ROLL // HL1 sdk
#include <Deps/release/halflife/engine/cdll_int.h>
#include <Deps/release/halflife/engine/progdefs.h>
#include <Deps/release/halflife/engine/eiface.h>
#include <Deps/release/halflife/engine/studio.h>
#include <Deps/release/halflife/common/cl_entity.h>
#include <Deps/release/halflife/common/com_model.h>
#include <Deps/release/halflife/common/cvardef.h>
#include <Deps/release/halflife/common/entity_state.h>
#include <Deps/release/halflife/common/entity_types.h>
#include <Deps/release/halflife/common/event_args.h>
#include <Deps/release/halflife/common/net_api.h>
//#include <halflife/common/r_studioint.h>
#include <Deps/release/halflife/pm_shared/pm_defs.h>
#include <Deps/release/halflife/common/r_efx.h>
#include <Deps/release/halflife/common/com_model.h>
//
#undef HSPRITE
#pragma pop_macro("HSPRITE")
#pragma pop_macro("offsetof")
#pragma pop_macro("ARRAYSIZE")
// END HLSDK includes
