#pragma once

namespace Filmmaker {
// Private renderer allocations must not become engine scene-target candidates or
// receive engine-only supersampling transforms. Creation and its detour are synchronous.
inline thread_local unsigned privateRenderTextureDepth = 0;
struct RenderPrivateTextureScope {
    RenderPrivateTextureScope() { ++privateRenderTextureDepth; }
    ~RenderPrivateTextureScope() { --privateRenderTextureDepth; }
    RenderPrivateTextureScope(const RenderPrivateTextureScope&) = delete;
    RenderPrivateTextureScope& operator=(const RenderPrivateTextureScope&) = delete;
};
inline bool CreatingPrivateRenderTexture() { return privateRenderTextureDepth != 0; }
}
