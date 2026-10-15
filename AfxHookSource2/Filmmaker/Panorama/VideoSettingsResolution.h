#pragma once

namespace Filmmaker {

// Extends CS2's own Video > Resolution dropdown with a Custom entry and saved filmmaker sizes.
// Main/UI thread only: this searches and modifies Panorama's native settings panels.
void VideoSettingsResolution_RunMainThreadFrame();

} // namespace Filmmaker
