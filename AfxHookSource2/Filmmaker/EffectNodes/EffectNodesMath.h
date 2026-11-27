#pragma once

// Pure engine-free DAG graph model, presets, and topological compiler for Effect Nodes.
// Connects depth ranges, sky masks, fog masks, and player mattes (all players or picked player)
// into color grades, glows, outlines, and distortions.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Filmmaker::EffectNodes {

inline constexpr int kMaxCompiledSteps = 16;
inline constexpr int kMaskSlots = 4;

enum class NodeType : int {
	// Mask producers
	DepthRange = 0,
	SkyMask = 1,
	PlayerMatte = 2,
	FogMask = 3,
	LumaKey = 4,
	MaskCombine = 5,
	// Effect processors
	ColorGrade = 6,
	Glow = 7,
	Outline = 8,
	Distortion = 9,
	// Final output
	Output = 10,
	// Masks added later. The values are what the shaders switch on, so they never move.
	HeightMask = 11,
	HueKey = 12,
	RadialMask = 13,
	GradientMask = 14,
	EdgeMask = 15,
	NoiseMask = 16,
	PlayerRadius = 17,
	MaskFeather = 18,
	// Effects added later.
	Blur = 19,
	Sharpen = 20,
	Vignette = 21,
	FilmGrain = 22,
	Posterize = 23,
	Pixelate = 24,
	Duotone = 25,
	LiftGammaGain = 26,
	HueShift = 27,
	DepthFog = 28,
	LightRays = 29,
	Scanlines = 30,
	Glitch = 31,
	Thermal = 32,
	NightVision = 33,
	InkLines = 34,
	Invert = 35,
	Halftone = 36,
	DepthBlur = 37,
	ColorFill = 38,
	BoxMask = 39,
	LensDistortion = 40,
	DirectionalBlur = 41,
	RadialBlur = 42,
	Glare = 43,
	Halation = 44,
	ChromaticAberration = 45,
	Kuwahara = 46,
	CelShade = 47,
	Refraction = 48,
	FilmDamage = 49,
	TiltShift = 50,
	Tonemap = 51,
	SplitTone = 52,
	Mirror = 53,
	AnalogDamage = 54,
	Count
};

inline bool IsMaskNode(NodeType t) {
	const int v = (int)t;
	return (v >= (int)NodeType::DepthRange && v <= (int)NodeType::MaskCombine)
		|| (v >= (int)NodeType::HeightMask && v <= (int)NodeType::MaskFeather)
		|| v == (int)NodeType::BoxMask;
}

inline bool IsEffectNode(NodeType t) {
	const int v = (int)t;
	return (v >= (int)NodeType::ColorGrade && v <= (int)NodeType::Distortion)
		|| (v >= (int)NodeType::Blur && v <= (int)NodeType::ColorFill)
		|| (v >= (int)NodeType::LensDistortion && v <= (int)NodeType::AnalogDamage);
}

// Mask nodes that read other masks: Combine (A and B) and Feather (A only).
inline bool IsMaskProcessor(NodeType t) {
	return t == NodeType::MaskCombine || t == NodeType::MaskFeather;
}

inline const char* NodeTypeToken(NodeType t) {
	switch (t) {
	case NodeType::DepthRange: return "depth_range";
	case NodeType::SkyMask: return "sky_mask";
	case NodeType::PlayerMatte: return "player_matte";
	case NodeType::FogMask: return "fog_mask";
	case NodeType::LumaKey: return "luma_key";
	case NodeType::MaskCombine: return "mask_combine";
	case NodeType::ColorGrade: return "grade";
	case NodeType::Glow: return "glow";
	case NodeType::Outline: return "outline";
	case NodeType::Distortion: return "distortion";
	case NodeType::Output: return "output";
	case NodeType::HeightMask: return "height_mask";
	case NodeType::HueKey: return "hue_key";
	case NodeType::RadialMask: return "radial_mask";
	case NodeType::GradientMask: return "gradient_mask";
	case NodeType::EdgeMask: return "edge_mask";
	case NodeType::NoiseMask: return "noise_mask";
	case NodeType::PlayerRadius: return "player_radius";
	case NodeType::MaskFeather: return "mask_feather";
	case NodeType::Blur: return "blur";
	case NodeType::Sharpen: return "sharpen";
	case NodeType::Vignette: return "vignette";
	case NodeType::FilmGrain: return "film_grain";
	case NodeType::Posterize: return "posterize";
	case NodeType::Pixelate: return "pixelate";
	case NodeType::Duotone: return "duotone";
	case NodeType::LiftGammaGain: return "lift_gamma_gain";
	case NodeType::HueShift: return "hue_shift";
	case NodeType::DepthFog: return "depth_fog";
	case NodeType::LightRays: return "light_rays";
	case NodeType::Scanlines: return "scanlines";
	case NodeType::Glitch: return "glitch";
	case NodeType::Thermal: return "thermal";
	case NodeType::NightVision: return "night_vision";
	case NodeType::InkLines: return "ink_lines";
	case NodeType::Invert: return "invert";
	case NodeType::Halftone: return "halftone";
	case NodeType::DepthBlur: return "depth_blur";
	case NodeType::ColorFill: return "color_fill";
	case NodeType::BoxMask: return "box_mask";
	case NodeType::LensDistortion: return "lens_distortion";
	case NodeType::DirectionalBlur: return "directional_blur";
	case NodeType::RadialBlur: return "radial_blur";
	case NodeType::Glare: return "glare";
	case NodeType::Halation: return "halation";
	case NodeType::ChromaticAberration: return "chromatic_aberration";
	case NodeType::Kuwahara: return "kuwahara";
	case NodeType::CelShade: return "cel_shade";
	case NodeType::Refraction: return "refraction";
	case NodeType::FilmDamage: return "film_damage";
	case NodeType::TiltShift: return "tilt_shift";
	case NodeType::Tonemap: return "tonemap";
	case NodeType::SplitTone: return "split_tone";
	case NodeType::Mirror: return "mirror";
	case NodeType::AnalogDamage: return "analog_damage";
	default: return "grade";
	}
}

inline const char* NodeTypeLabel(NodeType t) {
	switch (t) {
	case NodeType::DepthRange: return "Depth Range";
	case NodeType::SkyMask: return "Sky Mask";
	case NodeType::PlayerMatte: return "Player Matte";
	case NodeType::FogMask: return "Fog Mask";
	case NodeType::LumaKey: return "Luma Key";
	case NodeType::MaskCombine: return "Combine Mask";
	case NodeType::ColorGrade: return "Color Grade";
	case NodeType::Glow: return "Glow";
	case NodeType::Outline: return "Outline";
	case NodeType::Distortion: return "Distortion";
	case NodeType::Output: return "Output";
	case NodeType::HeightMask: return "Height Mask";
	case NodeType::HueKey: return "Hue Key";
	case NodeType::RadialMask: return "Radial Mask";
	case NodeType::GradientMask: return "Gradient Mask";
	case NodeType::EdgeMask: return "Edge Mask";
	case NodeType::NoiseMask: return "Noise Mask";
	case NodeType::PlayerRadius: return "Around Player";
	case NodeType::MaskFeather: return "Mask Feather";
	case NodeType::Blur: return "Blur";
	case NodeType::Sharpen: return "Sharpen";
	case NodeType::Vignette: return "Vignette";
	case NodeType::FilmGrain: return "Film Grain";
	case NodeType::Posterize: return "Posterize";
	case NodeType::Pixelate: return "Pixelate";
	case NodeType::Duotone: return "Duotone";
	case NodeType::LiftGammaGain: return "Lift Gamma Gain";
	case NodeType::HueShift: return "Hue Shift";
	case NodeType::DepthFog: return "Depth Fog";
	case NodeType::LightRays: return "Light Rays";
	case NodeType::Scanlines: return "Scanlines";
	case NodeType::Glitch: return "Glitch";
	case NodeType::Thermal: return "Thermal";
	case NodeType::NightVision: return "Night Vision";
	case NodeType::InkLines: return "Ink Lines";
	case NodeType::Invert: return "Invert";
	case NodeType::Halftone: return "Halftone";
	case NodeType::DepthBlur: return "Depth Blur";
	case NodeType::ColorFill: return "Color Fill";
	case NodeType::BoxMask: return "Box Mask";
	case NodeType::LensDistortion: return "Lens Distortion";
	case NodeType::DirectionalBlur: return "Directional Blur";
	case NodeType::RadialBlur: return "Radial Blur";
	case NodeType::Glare: return "Glare";
	case NodeType::Halation: return "Halation";
	case NodeType::ChromaticAberration: return "Chromatic Aberration";
	case NodeType::Kuwahara: return "Kuwahara";
	case NodeType::CelShade: return "Cel Shade";
	case NodeType::Refraction: return "Refraction";
	case NodeType::FilmDamage: return "Film Damage";
	case NodeType::TiltShift: return "Tilt Shift";
	case NodeType::Tonemap: return "Tonemap";
	case NodeType::SplitTone: return "Split Tone";
	case NodeType::Mirror: return "Mirror";
	case NodeType::AnalogDamage: return "Analog Damage";
	default: return "Node";
	}
}

inline bool NodeTypeFromToken(const char* s, NodeType& out) {
	if (!s) return false;
	for (int i = 0; i < (int)NodeType::Count; ++i)
		if (0 == _stricmp(s, NodeTypeToken((NodeType)i))) { out = (NodeType)i; return true; }
	return false;
}

struct Node {
	int id = 0;
	NodeType type = NodeType::ColorGrade;
	std::string label;
	float x = 120.0f, y = 120.0f; // canvas coordinates for the interactive graph editor
	bool enabled = true;
	// Up to 8 numeric parameters per node (meaning defined by NodeType):
	// DepthRange:  [0]=near(100), [1]=far(1400), [2]=softness(150), [3]=invert(0)
	// SkyMask:     [0]=cutoff(18000), [1]=softness(500), [2]=invert(0)
	// PlayerMatte: [0]=selectedOnly(0=all,1=selected), [1]=feather(1.0), [2]=invert(0)
	// FogMask:     [0]=start(300), [1]=end(2600), [2]=density(1.0), [3]=invert(0)
	// LumaKey:     [0]=threshold(0.65), [1]=softness(0.15), [2]=invert(0)
	// MaskCombine: [0]=op(0=union,1=intersect,2=subtract,3=invert)
	// ColorGrade:  [0]=saturation(1), [1]=contrast(1), [2]=exposure(0), [3]=temp(0),
	//              [4]=tintR(1), [5]=tintG(1), [6]=tintB(1), [7]=mix(1)
	// Glow:        [0]=threshold(0.55), [1]=intensity(1.0), [2]=radius(12), [3]=tintR(1),
	//              [4]=tintG(0.9), [5]=tintB(0.75), [6]=mix(1)
	// Outline:     [0]=thickness(2.5), [1]=threshold(0.25), [2]=colorR(1), [3]=colorG(0.78),
	//              [4]=colorB(0.22), [5]=opacity(0.95)
	// Distortion:  [0]=mode(0=ripple,1=heathaze,2=radial,3=chromatic), [1]=strength(0.35),
	//              [2]=scale(14), [3]=speed(2.0), [4]=chromatic(0.35), [5]=mix(1)
	// Output:      [0]=masterMix(1.0)
	// HeightMask:  [0]=min z(-64), [1]=max z(160), [2]=softness(40), [3]=invert(0)
	// HueKey:      [0]=hue deg(120), [1]=range deg(30), [2]=softness deg(15), [3]=min sat(0.15), [4]=invert(0)
	// RadialMask:  [0]=center x(0.5), [1]=center y(0.5), [2]=radius(0.3), [3]=softness(0.2), [4]=aspect(1), [5]=invert(0)
	// GradientMask:[0]=angle deg(90), [1]=position(0.5), [2]=softness(0.3), [3]=invert(0)
	// EdgeMask:    [0]=threshold(0.08), [1]=thickness px(1.5), [2]=invert(0)
	// NoiseMask:   [0]=scale(6), [1]=speed(0.5), [2]=threshold(0.5), [3]=softness(0.2), [4]=invert(0)
	// PlayerRadius:[0]=radius(260), [1]=softness(140), [2]=invert(0)
	// MaskFeather: [0]=radius px(6), [1]=grow(-1..1, 0), [2]=invert(0)
	// Blur:        [0]=radius px(10), [1]=mix(1)
	// Sharpen:     [0]=amount(0.6), [1]=radius px(1), [2]=mix(1)
	// Vignette:    [0]=strength(0.75), [1]=radius(0.55), [2]=softness(0.5), [3..5]=color(0,0,0), [6]=mix(1)
	// FilmGrain:   [0]=amount(0.08), [1]=size(1.5), [2]=color(0), [3]=mix(1)
	// Posterize:   [0]=levels(6), [1]=mix(1)
	// Pixelate:    [0]=cell px(8), [1]=mix(1)
	// Duotone:     [0..2]=shadow rgb, [3..5]=highlight rgb, [6]=mix(1)
	// LiftGammaGain:[0]=lift(0), [1]=gamma(1), [2]=gain(1), [3]=saturation(1), [4]=mix(1)
	// HueShift:    [0]=degrees(0), [1]=saturation(1), [2]=vibrance(0), [3]=mix(1)
	// DepthFog:    [0]=start(150), [1]=end(2000), [2]=density(1.6), [3..5]=color, [6]=mix(1)
	// LightRays:   [0]=threshold(0.75), [1]=strength(1), [2]=length(0.45), [3]=center x(0.5), [4]=center y(0.3), [5]=mix(1)
	// Scanlines:   [0]=lines(360), [1]=strength(0.3), [2]=curvature(0.1), [3]=roll speed(0), [4]=mix(1)
	// Glitch:      [0]=strength(0.5), [1]=block px(24), [2]=speed(4), [3]=rgb split(0.5), [4]=mix(1)
	// Thermal:     [0]=palette(0 white hot,1 black hot,2 ironbow,3 sepia; 2), [1]=contrast(1.2),
	//              [2]=people heat(1), [3]=sensor noise(0.2), [4]=blur px(1.5),
	//              [5]=scope(0 none,1 circle,2 circle+reticle), [6]=sway(0.5), [7]=mix(1)
	// NightVision: [0]=gain(1.6), [1]=scintillation(0.35), [2]=halo(0.8),
	//              [3]=tube(0 full frame,1 one tube,2 two tubes; 1), [4]=phosphor(0 green,1 white),
	//              [5]=sway(0.5), [6]=mix(1)
	// InkLines:    [0]=threshold(0.3), [1]=strength(0.8), [2]=paper(0), [3]=mix(1)
	// Invert:      [0]=mix(1)
	// Halftone:    [0]=dot px(8), [1]=angle deg(45), [2]=mix(1)
	// DepthBlur:   [0]=focus(400), [1]=range(220), [2]=max radius px(8), [3]=mix(1)
	// ColorFill:   [0..2]=rgb, [3]=blend(0 normal,1 multiply,2 screen,3 overlay,4 add), [4]=mix(1)
	// BoxMask:     [0]=center x(0.5), [1]=center y(0.5), [2]=width(0.4), [3]=height(0.3), [4]=angle deg(0),
	//              [5]=softness(0.05), [6]=invert(0)
	// LensDistortion:[0]=distortion(0.2, <0 pincushion), [1]=dispersion(0.03), [2]=fit(1), [3]=mix(1)
	// DirectionalBlur:[0]=angle deg(0), [1]=length px(24), [2]=mix(1)
	// RadialBlur:  [0]=center x(0.5), [1]=center y(0.5), [2]=amount(0.15), [3]=mode(0 zoom,1 spin), [4]=mix(1)
	// Glare:       [0]=type(0 streaks,1 star 4,2 star 6,3 ghosts,4 fog glow), [1]=threshold(0.85),
	//              [2]=length(0.2), [3]=intensity(1), [4]=warmth(-1..1, 0), [5]=mix(1)
	// Halation:    [0]=threshold(0.6), [1]=radius px(18), [2]=intensity(0.9), [3..5]=color(1,0.25,0.08), [6]=mix(1)
	// ChromaticAberration:[0]=amount px(4), [1]=mode(0 radial,1 lateral), [2]=clear centre(1), [3]=mix(1)
	// Kuwahara:    [0]=radius px(4), [1]=mix(1)
	// CelShade:    [0]=bands(4), [1]=outline px(2), [2]=outline strength(1), [3]=depth edges(0.04),
	//              [4]=normal edges(0.35), [5]=hatching(0.35), [6]=saturation(1.25), [7]=mix(1)
	// Refraction:  [0]=strength(0.6), [1]=scale(8), [2]=speed(0.6), [3]=chromatic(0.3),
	//              [4]=source(0 noise,1 mask edges), [5]=mix(1)
	// FilmDamage:  [0]=scratches(0.5), [1]=dust(0.5), [2]=flicker(0.3), [3]=gate weave(0.3), [4]=age(0.2), [5]=mix(1)
	// TiltShift:   [0]=focus y(0.55), [1]=band(0.15), [2]=blur px(10), [3]=saturation(1.2), [4]=mix(1)
	// Tonemap:     [0]=exposure EV(0), [1]=contrast(1), [2]=curve(0 filmic,1 soft,2 punchy), [3]=mix(1)
	// SplitTone:   [0]=shadow hue(210), [1]=shadow amount(0.35), [2]=highlight hue(35),
	//              [3]=highlight amount(0.3), [4]=balance(-1..1, 0), [5]=mix(1)
	// Mirror:      [0]=mode(0 left to right,1 top to bottom,2 kaleidoscope), [1]=segments(6), [2]=angle(0), [3]=mix(1)
	// AnalogDamage:[0]=tracking(0.4), [1]=noise(0.3), [2]=color bleed(0.5), [3]=wobble(0.4), [4]=mix(1)
	float p[8] = {};
};

struct Edge {
	int id = 0;
	int fromNode = 0;
	int fromPort = 0; // 0 = primary output (color or mask)
	int toNode = 0;
	int toPort = 0;   // for Effect: 0 = color in, 1 = mask in; for MaskCombine: 0 = mask A, 1 = mask B
};

struct NodeGraph {
	bool enabled = false;
	std::string presetName;
	int selectedNodeId = -1;
	int nextNodeId = 1;
	int nextEdgeId = 1;
	std::vector<Node> nodes;
	std::vector<Edge> edges;
};

inline Node MakeDefaultNode(int id, NodeType type, float x, float y) {
	Node n;
	n.id = id;
	n.type = type;
	n.label = NodeTypeLabel(type);
	n.x = x;
	n.y = y;
	switch (type) {
	case NodeType::DepthRange:
		n.p[0] = 100.0f; n.p[1] = 1400.0f; n.p[2] = 150.0f; n.p[3] = 0.0f;
		break;
	case NodeType::SkyMask:
		n.p[0] = 18000.0f; n.p[1] = 500.0f; n.p[2] = 0.0f;
		break;
	case NodeType::PlayerMatte:
		n.p[0] = 0.0f; n.p[1] = 1.0f; n.p[2] = 0.0f;
		break;
	case NodeType::FogMask:
		n.p[0] = 320.0f; n.p[1] = 2600.0f; n.p[2] = 1.0f; n.p[3] = 0.0f;
		break;
	case NodeType::LumaKey:
		n.p[0] = 0.65f; n.p[1] = 0.15f; n.p[2] = 0.0f;
		break;
	case NodeType::MaskCombine:
		n.p[0] = 0.0f;
		break;
	case NodeType::ColorGrade:
		n.p[0] = 1.0f; n.p[1] = 1.0f; n.p[2] = 0.0f; n.p[3] = 0.0f;
		n.p[4] = 1.0f; n.p[5] = 1.0f; n.p[6] = 1.0f; n.p[7] = 1.0f;
		break;
	case NodeType::Glow:
		n.p[0] = 0.55f; n.p[1] = 1.0f; n.p[2] = 12.0f;
		n.p[3] = 1.0f; n.p[4] = 0.90f; n.p[5] = 0.72f; n.p[6] = 1.0f;
		break;
	case NodeType::Outline:
		n.p[0] = 2.5f; n.p[1] = 0.25f;
		n.p[2] = 1.0f; n.p[3] = 0.78f; n.p[4] = 0.22f; n.p[5] = 0.95f;
		break;
	case NodeType::Distortion:
		n.p[0] = 1.0f; n.p[1] = 0.35f; n.p[2] = 14.0f; n.p[3] = 2.0f; n.p[4] = 0.35f; n.p[5] = 1.0f;
		break;
	case NodeType::Output:
		n.p[0] = 1.0f;
		break;
	case NodeType::HeightMask: n.p[0] = -64.0f; n.p[1] = 160.0f; n.p[2] = 40.0f; break;
	case NodeType::HueKey: n.p[0] = 120.0f; n.p[1] = 30.0f; n.p[2] = 15.0f; n.p[3] = 0.15f; break;
	case NodeType::RadialMask: n.p[0] = 0.5f; n.p[1] = 0.5f; n.p[2] = 0.3f; n.p[3] = 0.2f; n.p[4] = 1.0f; break;
	case NodeType::GradientMask: n.p[0] = 90.0f; n.p[1] = 0.5f; n.p[2] = 0.3f; break;
	case NodeType::EdgeMask: n.p[0] = 0.08f; n.p[1] = 1.5f; break;
	case NodeType::NoiseMask: n.p[0] = 6.0f; n.p[1] = 0.5f; n.p[2] = 0.5f; n.p[3] = 0.2f; break;
	case NodeType::PlayerRadius: n.p[0] = 260.0f; n.p[1] = 140.0f; break;
	case NodeType::MaskFeather: n.p[0] = 6.0f; break;
	case NodeType::Blur: n.p[0] = 10.0f; n.p[1] = 1.0f; break;
	case NodeType::Sharpen: n.p[0] = 0.6f; n.p[1] = 1.0f; n.p[2] = 1.0f; break;
	case NodeType::Vignette: n.p[0] = 0.75f; n.p[1] = 0.55f; n.p[2] = 0.5f; n.p[6] = 1.0f; break;
	case NodeType::FilmGrain: n.p[0] = 0.08f; n.p[1] = 1.5f; n.p[3] = 1.0f; break;
	case NodeType::Posterize: n.p[0] = 6.0f; n.p[1] = 1.0f; break;
	case NodeType::Pixelate: n.p[0] = 8.0f; n.p[1] = 1.0f; break;
	case NodeType::Duotone:
		n.p[0] = 0.05f; n.p[1] = 0.08f; n.p[2] = 0.22f; n.p[3] = 1.0f; n.p[4] = 0.85f; n.p[5] = 0.55f; n.p[6] = 1.0f;
		break;
	case NodeType::LiftGammaGain: n.p[1] = 1.0f; n.p[2] = 1.0f; n.p[3] = 1.0f; n.p[4] = 1.0f; break;
	case NodeType::HueShift: n.p[1] = 1.0f; n.p[3] = 1.0f; break;
	case NodeType::DepthFog:
		n.p[0] = 150.0f; n.p[1] = 2000.0f; n.p[2] = 1.6f; n.p[3] = 0.6f; n.p[4] = 0.65f; n.p[5] = 0.7f; n.p[6] = 1.0f;
		break;
	case NodeType::LightRays: n.p[0] = 0.75f; n.p[1] = 1.0f; n.p[2] = 0.45f; n.p[3] = 0.5f; n.p[4] = 0.3f; n.p[5] = 1.0f; break;
	case NodeType::Scanlines: n.p[0] = 360.0f; n.p[1] = 0.3f; n.p[2] = 0.1f; n.p[4] = 1.0f; break;
	case NodeType::Glitch: n.p[0] = 0.5f; n.p[1] = 24.0f; n.p[2] = 4.0f; n.p[3] = 0.5f; n.p[4] = 1.0f; break;
	case NodeType::Thermal:
		n.p[0] = 2.0f; n.p[1] = 1.2f; n.p[2] = 1.0f; n.p[3] = 0.2f; n.p[4] = 1.5f; n.p[5] = 0.0f; n.p[6] = 0.5f; n.p[7] = 1.0f;
		break;
	case NodeType::NightVision:
		n.p[0] = 1.6f; n.p[1] = 0.35f; n.p[2] = 0.8f; n.p[3] = 1.0f; n.p[4] = 0.0f; n.p[5] = 0.5f; n.p[6] = 1.0f;
		break;
	case NodeType::InkLines: n.p[0] = 0.3f; n.p[1] = 0.8f; n.p[3] = 1.0f; break;
	case NodeType::Invert: n.p[0] = 1.0f; break;
	case NodeType::Halftone: n.p[0] = 8.0f; n.p[1] = 45.0f; n.p[2] = 1.0f; break;
	case NodeType::DepthBlur: n.p[0] = 400.0f; n.p[1] = 220.0f; n.p[2] = 8.0f; n.p[3] = 1.0f; break;
	case NodeType::ColorFill: n.p[0] = 1.0f; n.p[1] = 0.45f; n.p[2] = 0.2f; n.p[4] = 0.6f; break;
	case NodeType::BoxMask: n.p[0] = 0.5f; n.p[1] = 0.5f; n.p[2] = 0.4f; n.p[3] = 0.3f; n.p[5] = 0.05f; break;
	case NodeType::LensDistortion: n.p[0] = 0.2f; n.p[1] = 0.03f; n.p[2] = 1.0f; n.p[3] = 1.0f; break;
	case NodeType::DirectionalBlur: n.p[1] = 24.0f; n.p[2] = 1.0f; break;
	case NodeType::RadialBlur: n.p[0] = 0.5f; n.p[1] = 0.5f; n.p[2] = 0.15f; n.p[4] = 1.0f; break;
	case NodeType::Glare: n.p[1] = 0.85f; n.p[2] = 0.2f; n.p[3] = 1.0f; n.p[5] = 1.0f; break;
	case NodeType::Halation:
		n.p[0] = 0.6f; n.p[1] = 18.0f; n.p[2] = 0.9f; n.p[3] = 1.0f; n.p[4] = 0.25f; n.p[5] = 0.08f; n.p[6] = 1.0f;
		break;
	case NodeType::ChromaticAberration: n.p[0] = 4.0f; n.p[2] = 1.0f; n.p[3] = 1.0f; break;
	case NodeType::Kuwahara: n.p[0] = 4.0f; n.p[1] = 1.0f; break;
	case NodeType::CelShade:
		n.p[0] = 4.0f; n.p[1] = 2.0f; n.p[2] = 1.0f; n.p[3] = 0.04f; n.p[4] = 0.35f; n.p[5] = 0.35f; n.p[6] = 1.25f; n.p[7] = 1.0f;
		break;
	case NodeType::Refraction: n.p[0] = 0.6f; n.p[1] = 8.0f; n.p[2] = 0.6f; n.p[3] = 0.3f; n.p[5] = 1.0f; break;
	case NodeType::FilmDamage: n.p[0] = 0.5f; n.p[1] = 0.5f; n.p[2] = 0.3f; n.p[3] = 0.3f; n.p[4] = 0.2f; n.p[5] = 1.0f; break;
	case NodeType::TiltShift: n.p[0] = 0.55f; n.p[1] = 0.15f; n.p[2] = 10.0f; n.p[3] = 1.2f; n.p[4] = 1.0f; break;
	case NodeType::Tonemap: n.p[1] = 1.0f; n.p[3] = 1.0f; break;
	case NodeType::SplitTone: n.p[0] = 210.0f; n.p[1] = 0.35f; n.p[2] = 35.0f; n.p[3] = 0.3f; n.p[5] = 1.0f; break;
	case NodeType::Mirror: n.p[1] = 6.0f; n.p[3] = 1.0f; break;
	case NodeType::AnalogDamage: n.p[0] = 0.4f; n.p[1] = 0.3f; n.p[2] = 0.5f; n.p[3] = 0.4f; n.p[4] = 1.0f; break;
	default: break;
	}
	return n;
}

// Whether a wire from `fromId`'s output into (`toId`, `toPort`) makes sense. The graph editor asks
// before it connects, so a wire that the compiler would silently ignore or that would loop is
// refused with a reason instead of leaving the Output dark ("Final Output sometimes doesn't work").
//   Effect   port 0 = image (from an effect), port 1 = mask (from a mask)
//   Combine  ports 0 and 1 = masks
//   Output   port 0 = image only
//   Masks other than Combine take no input; Output has no output.
enum class ConnectCheck { Ok, SameNode, MissingNode, NoSuchPort, MaskIntoImage, ImageIntoMask, Cycle };

inline const char* ConnectCheckText(ConnectCheck c) {
	switch (c) {
	case ConnectCheck::Ok: return "ok";
	case ConnectCheck::SameNode: return "a node cannot feed itself";
	case ConnectCheck::MissingNode: return "no such node";
	case ConnectCheck::NoSuchPort: return "that node has no such input";
	case ConnectCheck::MaskIntoImage: return "a mask cannot be the image input";
	case ConnectCheck::ImageIntoMask: return "an image cannot be a mask input";
	case ConnectCheck::Cycle: return "that wire would make a loop";
	}
	return "?";
}

inline ConnectCheck CheckConnect(const NodeGraph& g, int fromId, int toId, int toPort) {
	if (fromId == toId) return ConnectCheck::SameNode;
	const Node* from = nullptr;
	const Node* to = nullptr;
	for (const Node& n : g.nodes) {
		if (n.id == fromId) from = &n;
		if (n.id == toId) to = &n;
	}
	if (!from || !to) return ConnectCheck::MissingNode;
	if (from->type == NodeType::Output || toPort < 0 || toPort > 1) return ConnectCheck::NoSuchPort;
	const bool fromMask = IsMaskNode(from->type);
	if (to->type == NodeType::Output) {
		if (toPort != 0) return ConnectCheck::NoSuchPort;
		if (fromMask) return ConnectCheck::MaskIntoImage;
	} else if (to->type == NodeType::MaskCombine) {
		if (!fromMask) return ConnectCheck::ImageIntoMask;
	} else if (to->type == NodeType::MaskFeather) {
		if (toPort != 0) return ConnectCheck::NoSuchPort;
		if (!fromMask) return ConnectCheck::ImageIntoMask;
	} else if (IsMaskNode(to->type)) {
		return ConnectCheck::NoSuchPort;
	} else if (toPort == 0) {
		if (fromMask) return ConnectCheck::MaskIntoImage;
	} else if (!fromMask) {
		return ConnectCheck::ImageIntoMask;
	}
	// A loop: `to` already feeds `from`, directly or further up.
	std::vector<int> stack{ fromId };
	std::set<int> seen;
	while (!stack.empty()) {
		const int id = stack.back(); stack.pop_back();
		if (id == toId) return ConnectCheck::Cycle;
		if (!seen.insert(id).second) continue;
		for (const Edge& e : g.edges)
			if (e.toNode == id) stack.push_back(e.fromNode);
	}
	return ConnectCheck::Ok;
}

// The input a new node takes when a wire dragged from `fromType`'s output is dropped on empty space
// and the node is created there (Blender / Nuke): a mask lands on the mask input, an image on the
// image input. -1 when the new node takes nothing from it.
inline int DefaultLinkPort(NodeType fromType, NodeType newType) {
	const bool fromMask = IsMaskNode(fromType);
	if (IsMaskProcessor(newType)) return fromMask ? 0 : -1;
	if (IsMaskNode(newType) || newType == NodeType::Output) return -1;
	return fromMask ? 1 : 0;
}

struct CompiledStep {
	int nodeId = 0;
	NodeType type = NodeType::ColorGrade;
	int outMaskSlot = -1; // 0..3 when IsMaskNode
	int maskSlotA = -1;   // bound mask slot for Effect or MaskCombine A
	int maskSlotB = -1;   // bound mask slot for MaskCombine B
	float p[8] = {};
	// The mask nodes this step reads (MaskCombine A/B, an Effect's mask), before slots are assigned.
	int maskNodeA = -1;
	int maskNodeB = -1;
};

struct CompiledPlan {
	bool enabled = false;
	bool needsWorldDepthRef = false;
	float masterMix = 1.0f;
	// Steps dropped because more than kMaskSlots masks would have to be alive at once.
	int droppedForMaskSlots = 0;
	std::vector<CompiledStep> steps;
};

// Gives every mask step a slot that no mask still waiting for a reader holds. A slot is free again
// only after the last step that reads its mask has run, so a mask feeding several effects, or the
// first branch of a deep MaskCombine tree, is never overwritten before it is used. A step whose
// mask would need a fifth live slot is dropped (with everything that reads it) instead of
// overwriting a live mask, and counted in droppedForMaskSlots.
inline void AssignMaskSlots(CompiledPlan& plan) {
	std::map<int, size_t> lastRead; // mask node id -> index of its last reader
	for (size_t i = 0; i < plan.steps.size(); ++i) {
		const CompiledStep& st = plan.steps[i];
		if (st.maskNodeA > 0) lastRead[st.maskNodeA] = i;
		if (st.maskNodeB > 0) lastRead[st.maskNodeB] = i;
	}
	std::map<int, int> slotOf; // mask node id -> slot
	int owner[kMaskSlots];
	for (int& o : owner) o = -1;
	std::vector<CompiledStep> kept;
	for (size_t i = 0; i < plan.steps.size(); ++i) {
		CompiledStep st = plan.steps[i];
		const auto bound = [&](int node) { auto it = slotOf.find(node); return it == slotOf.end() ? -1 : it->second; };
		const bool needsA = st.maskNodeA > 0, needsB = st.maskNodeB > 0;
		st.maskSlotA = needsA ? bound(st.maskNodeA) : -1;
		st.maskSlotB = needsB ? bound(st.maskNodeB) : -1;
		const bool inputsLost = (needsA && st.maskSlotA < 0) || (needsB && st.maskSlotB < 0);
		if (IsMaskNode(st.type)) {
			int slot = -1;
			if (!inputsLost)
				for (int s = 0; s < kMaskSlots && slot < 0; ++s) if (owner[s] < 0) slot = s;
			if (slot < 0) { ++plan.droppedForMaskSlots; }
			else {
				st.outMaskSlot = slot;
				owner[slot] = st.nodeId;
				slotOf[st.nodeId] = slot;
			}
		} else if (inputsLost && IsEffectNode(st.type)) {
			// Its mask was dropped: apply the effect unmasked would change the look, so skip it.
			++plan.droppedForMaskSlots;
		}
		const bool keep = IsMaskNode(st.type) ? st.outMaskSlot >= 0 : !(inputsLost && IsEffectNode(st.type));
		if (keep) kept.push_back(st);
		// Free the slots of masks this step was the last reader of (after the step, so a MaskCombine
		// never writes into a slot it is still reading).
		for (int node : { st.maskNodeA, st.maskNodeB }) {
			auto lr = lastRead.find(node);
			if (node > 0 && lr != lastRead.end() && lr->second == i) {
				const int s = bound(node);
				if (s >= 0 && owner[s] == node) owner[s] = -1;
			}
		}
		// A mask nothing reads frees its slot at once.
		if (IsMaskNode(st.type) && st.outMaskSlot >= 0 && !lastRead.count(st.nodeId)) owner[st.outMaskSlot] = -1;
	}
	plan.steps.swap(kept);
}

// Cycle-safe topological compiler: walks backward from the Output node, emits mask evaluations
// before the effects that consume them, then gives masks slots 0..3 by lifetime (AssignMaskSlots).
inline CompiledPlan CompileGraph(const NodeGraph& g) {
	CompiledPlan plan;
	plan.enabled = g.enabled;
	if (!g.enabled) return plan;

	std::map<int, const Node*> byId;
	const Node* outputNode = nullptr;
	for (const Node& n : g.nodes) {
		byId[n.id] = &n;
		if (n.type == NodeType::Output) outputNode = &n;
	}
	if (!outputNode) return plan;
	plan.masterMix = std::clamp(outputNode->p[0], 0.0f, 1.0f);

	auto findInput = [&](int toNode, int toPort) -> int {
		for (const Edge& e : g.edges)
			if (e.toNode == toNode && e.toPort == toPort) return e.fromNode;
		return -1;
	};

	std::set<int> visiting, visited;
	std::set<int> maskNodes; // mask nodes already emitted

	auto visit = [&](auto&& self, int id) -> void {
		if (id <= 0 || visited.count(id) || visiting.count(id)) return;
		auto it = byId.find(id);
		if (it == byId.end()) return;
		const Node* n = it->second;
		visiting.insert(id);

		// Visit upstream color/mask dependencies first.
		const int in0 = findInput(id, 0);
		const int in1 = findInput(id, 1);
		if (in0 > 0) self(self, in0);
		if (in1 > 0) self(self, in1);

		visiting.erase(id);
		visited.insert(id);

		if (n->type == NodeType::Output || !n->enabled) return;
		if ((int)plan.steps.size() >= kMaxCompiledSteps) return;

		CompiledStep step;
		step.nodeId = n->id;
		step.type = n->type;
		std::memcpy(step.p, n->p, sizeof(step.p));

		if (IsMaskNode(n->type)) {
			maskNodes.insert(n->id);
			if (n->type == NodeType::MaskCombine) {
				if (maskNodes.count(in0)) step.maskNodeA = in0;
				if (maskNodes.count(in1)) step.maskNodeB = in1;
			} else if (n->type == NodeType::MaskFeather) {
				if (maskNodes.count(in0)) step.maskNodeA = in0;
			}
			if (n->type == NodeType::PlayerMatte) plan.needsWorldDepthRef = true;
		} else if (IsEffectNode(n->type)) {
			// Thermal finds people the way a Player Matte does, so it needs the same reference render.
			if (n->type == NodeType::Thermal && n->p[2] > 0.001f) plan.needsWorldDepthRef = true;
			// Port 1 is the mask input on every Effect node.
			if (maskNodes.count(in1)) step.maskNodeA = in1;
			else if (maskNodes.count(in0)) step.maskNodeA = in0;
		}
		plan.steps.push_back(step);
	};

	visit(visit, outputNode->id);
	AssignMaskSlots(plan);
	return plan;
}

// Built-in starter presets matching the user's examples and ready to tweak in the graph editor!
inline NodeGraph BuildPreset(const std::string& name, bool enableNow = true) {
	NodeGraph g;
	g.enabled = enableNow;
	g.presetName = name;

	auto addNode = [&](NodeType t, float x, float y, const char* customLabel = nullptr) -> Node& {
		Node n = MakeDefaultNode(g.nextNodeId++, t, x, y);
		if (customLabel) n.label = customLabel;
		g.nodes.push_back(n);
		return g.nodes.back();
	};
	auto link = [&](int from, int to, int toPort = 0) {
		g.edges.push_back(Edge{ g.nextEdgeId++, from, 0, to, toPort });
	};

	if (name == "color_subject_desat_world") {
		Node& pmWorld = addNode(NodeType::PlayerMatte, 40, 60, "World (Inv Player)");
		// The picked player (everyone while nobody is picked), inverted -> selects world
		pmWorld.p[0] = 1.0f; pmWorld.p[2] = 1.0f;
		const int pmWorldId = pmWorld.id;

		Node& pmSubj = addNode(NodeType::PlayerMatte, 40, 210, "Subject Matte");
		pmSubj.p[0] = 1.0f; pmSubj.p[2] = 0.0f;
		const int pmSubjId = pmSubj.id;

		Node& desat = addNode(NodeType::ColorGrade, 270, 60, "Desaturate World");
		desat.p[0] = 0.05f; desat.p[1] = 1.15f; desat.p[2] = -0.18f; desat.p[7] = 1.0f;
		const int desatId = desat.id;

		Node& pop = addNode(NodeType::ColorGrade, 500, 140, "Color Subject");
		pop.p[0] = 1.38f; pop.p[1] = 1.08f; pop.p[2] = 0.08f; pop.p[3] = 0.12f; pop.p[7] = 1.0f;
		const int popId = pop.id;

		Node& out = addNode(NodeType::Output, 730, 140, "Final Output");
		link(pmWorldId, desatId, 1);
		link(desatId, popId, 0);
		link(pmSubjId, popId, 1);
		link(popId, out.id, 0);
		g.selectedNodeId = desatId;
	} else if (name == "glow_through_fog") {
		Node& fog = addNode(NodeType::FogMask, 40, 80, "Fog Distance Mask");
		fog.p[0] = 280.0f; fog.p[1] = 2400.0f; fog.p[2] = 1.15f;
		const int fogId = fog.id;

		Node& glow = addNode(NodeType::Glow, 280, 80, "Fog Bloom");
		glow.p[0] = 0.42f; glow.p[1] = 1.45f; glow.p[2] = 16.0f;
		glow.p[3] = 1.0f; glow.p[4] = 0.86f; glow.p[5] = 0.65f; glow.p[6] = 1.0f;
		const int glowId = glow.id;

		Node& grade = addNode(NodeType::ColorGrade, 510, 120, "Atmospheric Grade");
		grade.p[0] = 1.10f; grade.p[1] = 1.08f; grade.p[2] = 0.05f; grade.p[3] = 0.18f; grade.p[7] = 0.85f;
		const int gradeId = grade.id;

		Node& out = addNode(NodeType::Output, 740, 120, "Final Output");
		link(fogId, glowId, 1);
		link(glowId, gradeId, 0);
		link(fogId, gradeId, 1);
		link(gradeId, out.id, 0);
		g.selectedNodeId = glowId;
	} else if (name == "player_neon_outline") {
		Node& pm = addNode(NodeType::PlayerMatte, 40, 110, "Player Matte");
		pm.p[0] = 1.0f; pm.p[2] = 0.0f; // the picked player, or everyone while nobody is picked
		const int pmId = pm.id;

		Node& outline = addNode(NodeType::Outline, 280, 110, "Neon Silhouette");
		outline.p[0] = 3.0f; outline.p[1] = 0.2f;
		outline.p[2] = 1.0f; outline.p[3] = 0.72f; outline.p[4] = 0.18f; outline.p[5] = 1.0f;
		const int outlId = outline.id;

		Node& glow = addNode(NodeType::Glow, 510, 110, "Edge Glow");
		glow.p[0] = 0.60f; glow.p[1] = 1.25f; glow.p[2] = 10.0f;
		glow.p[3] = 1.0f; glow.p[4] = 0.76f; glow.p[5] = 0.28f; glow.p[6] = 0.9f;
		const int glowId = glow.id;

		Node& out = addNode(NodeType::Output, 740, 110, "Final Output");
		link(pmId, outlId, 1);
		link(outlId, glowId, 0);
		// Only the player's side of the outline glows; unmasked, the bloom whites out a bright scene.
		link(pmId, glowId, 1);
		link(glowId, out.id, 0);
		g.selectedNodeId = outlId;
	} else if (name == "sky_grade_heat_haze") {
		Node& sky = addNode(NodeType::SkyMask, 40, 60, "Sky Mask");
		const int skyId = sky.id;

		Node& depth = addNode(NodeType::DepthRange, 40, 210, "Far Distance");
		depth.p[0] = 550.0f; depth.p[1] = 3200.0f; depth.p[2] = 260.0f;
		const int depthId = depth.id;

		Node& skyGrade = addNode(NodeType::ColorGrade, 280, 60, "Dramatic Sky");
		skyGrade.p[0] = 1.45f; skyGrade.p[1] = 1.18f; skyGrade.p[2] = -0.10f;
		skyGrade.p[4] = 1.15f; skyGrade.p[5] = 0.82f; skyGrade.p[6] = 0.65f; skyGrade.p[7] = 1.0f;
		const int skyGradeId = skyGrade.id;

		Node& haze = addNode(NodeType::Distortion, 520, 140, "Heat Haze");
		haze.p[0] = 1.0f; haze.p[1] = 0.35f; haze.p[2] = 16.0f; haze.p[3] = 2.4f; haze.p[4] = 0.25f; haze.p[5] = 0.85f;
		const int hazeId = haze.id;

		Node& out = addNode(NodeType::Output, 750, 140, "Final Output");
		link(skyId, skyGradeId, 1);
		link(skyGradeId, hazeId, 0);
		link(depthId, hazeId, 1);
		link(hazeId, out.id, 0);
		g.selectedNodeId = skyGradeId;
	} else {
		// depth_spotlight (default)
		g.presetName = "depth_spotlight";
		Node& depth = addNode(NodeType::DepthRange, 40, 100, "Outside Subject Band");
		depth.p[0] = 120.0f; depth.p[1] = 750.0f; depth.p[2] = 160.0f; depth.p[3] = 1.0f;
		const int depthId = depth.id;

		Node& bgGrade = addNode(NodeType::ColorGrade, 290, 100, "Darken Surroundings");
		bgGrade.p[0] = 0.35f; bgGrade.p[1] = 1.15f; bgGrade.p[2] = -0.55f; bgGrade.p[7] = 0.9f;
		const int bgId = bgGrade.id;

		Node& out = addNode(NodeType::Output, 560, 100, "Final Output");
		link(depthId, bgId, 1);
		link(bgId, out.id, 0);
		g.selectedNodeId = bgId;
	}
	return g;
}

} // namespace Filmmaker::EffectNodes
