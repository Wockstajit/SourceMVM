#pragma once
#include "EnvironmentCasterBounds.h"

namespace Filmmaker::Environment {
// Retains the native sprite pose, sheet frames and fade for shadow coverage only.
// Additive light never becomes a dark caster. Native rendering is left intact.
void NoteParticleShadowShader(ID3D11VertexShader*, const void*, size_t);
void BeginParticleShadows(ID3D11Device*);
void EndParticleShadows();
void SetParticleShadowsEnabled(bool);
bool ParticleShadowsEnabled();
void NextParticleShadowFrame();
bool BeginParticleShadowDraw(ID3D11DeviceContext*, UINT vertices, UINT instances);
bool EndParticleShadowDraw(ID3D11DeviceContext*);
CasterVertexStream ParticleShadowStream();
void DrawParticleShadows(ID3D11DeviceContext*, const Matrix&);
}
