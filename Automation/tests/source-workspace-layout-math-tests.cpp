// The CS:S workspace layout against CS2's numbers: stage scaling, the inspector column's clamp, the
// dock measure, and the centred, aspect-fitted preview.

#include "../../AfxHookSource/Filmmaker/Ui/WorkspaceLayoutMath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace SourceFilmmaker::Ui;

static int g_failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++g_failed; } } while (0)

static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

static void TestStage() {
	const Stage s = StageFor(1920, 1080);
	CHECK(Near(s.scale, 1) && Near(s.width, 1920) && Near(s.height, 1080));
	const Stage small = StageFor(854, 480); // the routine test window
	CHECK(Near(small.scale, 480.0f / 1080.0f) && Near(small.width, 854.0f * 1080.0f / 480.0f));
	const Stage wide = StageFor(2560, 1080);
	CHECK(Near(wide.width, 2560)); // wider windows get a wider stage, not a smaller UI
	CHECK(StageFor(0, 1080).width == 0);
}

static void TestDockMeasure() {
	const DockMeasure bar = MeasureDock(1080, kDemoBarHeight);
	CHECK(Near(bar.bottomH, 96 + 4 + 36) && Near(bar.dockTopY, 1080 - 136) && Near(bar.tabY, 1080 - 136));
	const DockMeasure timeline = MeasureDock(1080, kTimelineEditorHeight);
	CHECK(Near(kTimelineEditorHeight, 440) && Near(timeline.bottomH, 480));
	// A dock taller than the screen allows is capped so 160 px stay above it.
	const DockMeasure huge = MeasureDock(500, 440);
	CHECK(Near(huge.bottomH, 340) && Near(huge.dockTopY, 160));
}

static void TestCameraEditor1080() {
	const WorkspaceLayout l = CameraEditorLayout(1920, 1080, 640, kDemoBarHeight);
	CHECK(Near(l.inspectorW, 640));
	CHECK(Near(l.inspector.x0, 1280) && Near(l.inspector.x1, 1920) && Near(l.inspector.y1, 1080));
	CHECK(Near(l.area.x1, 1280) && Near(l.area.y1, 944));
	CHECK(Near(l.ribbon.y0, 944) && Near(l.ribbon.y1, 980) && Near(l.ribbon.x1, 1280));
	CHECK(Near(l.dock.y0, 984) && Near(l.dock.y1, 1080));
	// 16:9 preview in a 1280 x 944 area: full width, letterboxed evenly top and bottom.
	CHECK(Near(l.preview.x0, 0) && Near(l.preview.x1, 1280));
	CHECK(Near(l.preview.y0, 112) && Near(l.preview.y1, 832));
}

static void TestCameraEditorTimeline() {
	const WorkspaceLayout l = CameraEditorLayout(1920, 1080, 640, kTimelineEditorHeight);
	// 1280 x 600 area: height-limited, so the preview is 1066 wide and centred horizontally.
	CHECK(Near(l.area.y1, 600));
	CHECK(Near(l.preview.y0, 0) && Near(l.preview.y1, 600));
	CHECK(Near(l.preview.W(), 1066) && Near(l.preview.x0, 107));
}

static void TestInspectorClamp() {
	CHECK(Near(CameraEditorLayout(1920, 1080, 200, kDemoBarHeight).inspectorW, 600));  // floor
	CHECK(Near(CameraEditorLayout(1920, 1080, 5000, kDemoBarHeight).inspectorW, 1100)); // stored cap
	// A narrow 4:3 stage (1440 wide) caps the column at 62 % of it.
	CHECK(Near(CameraEditorLayout(1440, 1080, 1100, kDemoBarHeight).inspectorW, std::floor(1440 * 0.62f)));
	// Narrower still, the floor wins over the fraction.
	CHECK(Near(CameraEditorLayout(800, 1080, 900, kDemoBarHeight).inspectorW, 600));
}

static void TestSmallWindow() {
	// The 854x480 test window lays out the same stage as 1080p, scaled by 480/1080.
	const WorkspaceLayout l = CameraEditorLayout(854, 480, 640, kDemoBarHeight);
	const Rect inspector = ToClient(l.inspector, l.stage);
	CHECK(Near(inspector.x1, 854) && Near(inspector.y1, 480));
	CHECK(Near(inspector.W(), 640 * 480.0f / 1080.0f));
	CHECK(l.preview.Valid() && l.preview.x1 <= l.area.x1 && l.preview.y1 <= l.area.y1);
}

static void TestFitPreview() {
	CHECK(!FitPreview(Rect(), 1.777f).Valid());
	CHECK(!FitPreview(MakeRect(0, 0, 100, 100), 0).Valid());
	const Rect r = FitPreview(MakeRect(10, 20, 410, 320), 1.0f); // 400 x 300 area, square content
	CHECK(Near(r.W(), 300) && Near(r.H(), 300) && Near(r.x0, 60) && Near(r.y0, 20));
}

int main() {
	TestStage();
	TestDockMeasure();
	TestCameraEditor1080();
	TestCameraEditorTimeline();
	TestInspectorClamp();
	TestSmallWindow();
	TestFitPreview();
	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("SourceWorkspaceLayoutMathTests passed\n");
	return 0;
}
