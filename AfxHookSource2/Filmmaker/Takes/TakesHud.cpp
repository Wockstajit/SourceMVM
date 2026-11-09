#include "TakesHud.h"

#include "Takes.h"

#include "../Panorama/PanoramaBridge.h"
#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel

#include <string>

namespace Filmmaker::Takes {
namespace {

const char *const kHudJs = R"TKJS(
(function () {
  try {
    var old = $('#TakesStatusLine'); if (old) old.DeleteAsync(0);
    var line = $.CreatePanel('Label', $.GetContextPanel(), 'TakesStatusLine');
    line.hittest = false;
    line.style.horizontalAlign = 'center';
    line.style.verticalAlign = 'top';
    line.style.marginTop = '72px';
    line.style.paddingLeft = '12px'; line.style.paddingRight = '12px';
    line.style.paddingTop = '4px'; line.style.paddingBottom = '4px';
    line.style.fontSize = '18px'; line.style.fontWeight = 'bold';
    line.style.borderRadius = '4px';
    line.style.zIndex = '420';
    line.visible = false;
    $.TakesStatus = function (text, recording) {
      line.visible = !!text;
      line.text = text || '';
      // Warmup ending restarts the round once, and CS2 answers that with a big MATCH START banner.
      // Its own animation drives the opacity, so the panel is switched off instead.
      var alerts = $.GetContextPanel().FindChildTraverse('HudAlerts');
      if (alerts) alerts.visible = !text;
      line.style.color = '#ffffffff';
      line.style.backgroundColor = recording ? '#c23030e6' : '#14181ccc';
    };
  } catch (e) { $.Msg('TakesStatus build failed: ' + e); }
})();
)TKJS";

PanoramaBridge g_bridge;
void *g_hud = nullptr;
bool g_built = false;
std::string g_shown;

void Show(const std::string &text, bool recording) {
	std::string script = "$.TakesStatus && $.TakesStatus('";
	script += text;
	script += recording ? "',true);" : "',false);";
	g_bridge.RunScript(script.c_str());
}

} // namespace

void RunHudFrame() {
	bool recording = false;
	const std::string text = HudLine(recording);
	if (text.empty() && g_shown.empty()) return;
	g_bridge.Init();
	unsigned char *hud = AfxHookSource2_GetPanoramaHudPanel();
	if (!hud) {
		g_hud = nullptr;
		g_built = false;
		g_shown.clear();
		return;
	}
	if (hud != g_hud) {
		g_hud = hud;
		g_built = false;
		g_shown.clear();
	}
	g_bridge.SetContextPanel(hud);
	if (!g_built) {
		if (!g_bridge.CanRunScript() || !g_bridge.ContextPanel()) return;
		if (!g_bridge.RunScript(kHudJs)) return;
		g_built = true;
	}
	if (text == g_shown) return;
	g_shown = text;
	Show(text, recording);
}

} // namespace Filmmaker::Takes
