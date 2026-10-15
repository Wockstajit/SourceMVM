#pragma once

// Engine plumbing for METHOD ACTING (see MethodActing.h): finding a player model's eye flex
// controllers and writing their weights.
//
// SPLIT OUT from MethodActing.cpp because it is the only version-coupled part of the feature.
// Everything in MethodActing.cpp is arithmetic and state that survives a CS2 update untouched;
// everything here is a byte-pattern scan that one game update can invalidate. Keeping them apart
// means "the faces stopped moving" is answerable by looking at ONE file and one status readout.
//
// The flex controllers animate eyelids. Actual pupil directions are handled separately by
// MethodActingGaze through the named eye bones; the vertical flex pair follows their pitch.
// A model without matching controllers is skipped.
//
// THE ORDER IS NOT FIXED: ctm_sas lists them down/up/blink while tm_phoenix lists blink/up/down,
// so indices MUST be resolved by name per model. A hardcoded index would silently roll half the
// roster's eyes upward instead of squinting them.
//
// RESOLUTION STRATEGY: two byte patterns, three functions, and none of them needs a guess about
// a struct layout. The first anchors on the CS2 HOSTAGE's own "find the eye_blink flex
// controller by name" loop, which resolves the engine's flex-controller count and name
// accessors together -- the hostage blinks natively, so this is literally the code Valve wrote
// to do what this feature does. The second is the bounds-checked flex weight setter that the
// same hostage code calls. Anchoring on a call site rather than on each function's prologue is
// what keeps the accessors honest: two functions in client.dll share the name accessor's exact
// prologue and differ only in their tail jump, so a prologue scan is genuinely ambiguous.
//
// Every native call is SEH-guarded. A CS2 update that moves a pattern turns this feature into a
// no-op with a status string, never a crash.

namespace Filmmaker {
namespace MethodActingFlex {

// Resolved per model. -1 means the model has no such controller.
struct EyeIndices {
	int blink = -1;
	int lookUp = -1;
	int lookDown = -1;
	int count = 0;   // the model's flex controller count, straight from the engine
};

struct Status {
	bool attempted = false;      // Resolve() has run
	bool resolved = false;       // every pattern found
	bool sawName = false;        // at least one controller name read back as a sane string
	int  lastCount = 0;          // flex controller count of the last model inspected
	int  lookups = 0;            // name walks actually performed (cache misses)
	EyeIndices lastEyes;         // what the last walk found
	char lastNames[192] = { 0 }; // the names we read, comma separated -- the whole diagnosis
	unsigned long long writes = 0;
	unsigned long long faults = 0;
};

// Idempotent; safe to call every frame. Returns true once the patterns resolved.
bool Resolve();

const Status& GetStatus();

// Fills `out` with this model's eye controller indices. Cached per model, so ten players wearing
// three agents cost three walks rather than ten per frame. Returns false when nothing resolved.
bool FindEyeIndices(void* entity, EyeIndices* out);

// Writes one flex weight through the engine's own SetFlexWeightByIndex, which range-checks the
// index against THIS model's controller count and applies the controller's min/max remap and its
// non-finite guard. Returns false on a fault or an unresolved pattern.
bool SetWeight(void* entity, int index, float value);

// Escape hatch for a CS2 update that renames the blink controller: forces its index instead of
// looking it up by name. Negative clears it. Diagnostics only.
void SetIndexOverride(int index);
int  IndexOverride();

} // namespace MethodActingFlex
} // namespace Filmmaker
