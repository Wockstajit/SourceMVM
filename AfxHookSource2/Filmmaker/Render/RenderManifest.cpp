#include "RenderManifest.h"
#include "RenderPassPlan.h"
#include "RenderJob.h"
#include "RenderHost.h"

#include "../Platform/JsonBuilder.h"
#include "../Platform/TextEncoding.h"

#include <cstdio>
#include <ctime>

namespace Filmmaker {

namespace {

FILE* s_frameMap = nullptr;

const char* CameraSourceName(RenderCameraSource s) {
	switch (s) {
	case RenderCameraSource::CurrentViewer: return "currentViewer";
	case RenderCameraSource::Pov: return "pov";
	case RenderCameraSource::ThirdPerson: return "thirdPerson";
	case RenderCameraSource::FreeCamera: return "freeCamera";
	case RenderCameraSource::CameraPath: return "cameraPath";
	case RenderCameraSource::FollowCamera: return "followCamera";
	}
	return "?";
}

const char* ExtensionName(CameraPathExtension e) {
	switch (e) {
	case CameraPathExtension::Fail: return "fail";
	case CameraPathExtension::HoldFirst: return "holdFirst";
	case CameraPathExtension::HoldLast: return "holdLast";
	case CameraPathExtension::Clamp: return "clamp";
	}
	return "?";
}

const char* HudModeName(RenderHudMode m) {
	switch (m) {
	case RenderHudMode::None: return "none";
	case RenderHudMode::Game: return "game";
	case RenderHudMode::Full: return "full";
	}
	return "?";
}

std::string NowIso8601() {
	std::time_t now = std::time(nullptr);
	std::tm tm{};
	localtime_s(&tm, &now);
	char buf[40] = {0};
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	return buf;
}

bool WriteManifest(const RenderJob& job, const std::string& demoPathUtf8,
	const char* status, int framesWritten, const std::string& notes) {
	if (job.takeDir.empty())
		return false;

	JsonBuilder b;
	b.BeginObject();
	b.StringField("tool", "SOURCE:MVM render");
	b.StringField("writtenAt", NowIso8601());
	b.StringField("status", status ? status : "?");
	b.StringField("demo", demoPathUtf8);
	b.IntField("startTick", job.range.startTick);
	b.IntField("endTickExclusive", job.range.endTickExclusive);
	b.DoubleField("demoTickRate", job.tickRate);
	b.IntField("outputFps", job.out.fps);
	// IWXMVM capture model: game speed snapshotted from the timeline dial; each output frame
	// advances demo time by exactly timescale/outputFps seconds (= 1/captureFps).
	b.DoubleField("timescale", job.timescale);
	b.IntField("captureFps", job.captureFps);
	// The size frames were captured at: always the game resolution.
	b.IntField("width", job.width);
	b.IntField("height", job.height);
	b.StringField("cameraSource", CameraSourceName(job.camera));
	b.StringField("cameraPathExtension", ExtensionName(job.extension));
	b.StringField("format", RenderFormatToken(job.out.format));
	b.StringField("recorderPreset", job.presetName);
	b.StringField("hudMode", HudModeName(job.passes.hud));
	b.BoolField("cameraDataSidecar", job.passes.cameraData);
	b.BoolField("blood", job.passes.blood);
	// One output folder per pass. Every stream holds the same output frames (frame N of each was
	// rendered from the same simulation state), so a comp can stack them frame for frame.
	{
		const PassPlan plan = BuildPassPlan(job.passes.passes);
		b.Key("streams");
		b.BeginArray();
		for (const PassConfig& pass : plan.passes) {
			b.BeginObject();
			b.StringField("name", PassStreamName(pass));
			b.StringField("type", PassTypeToken(pass.type));
			b.StringField("elements", VisibleElementsToken(pass.elements));
			// Where it landed, relative to this folder: a video is <type>\<variant>.<ext>, an image
			// sequence is frames inside <type>\<variant>\.
			const bool sequence = RenderFormatIsSequence(job.out.format);
			const std::string folder = PassOutputFolder(pass, sequence);
			b.StringField("folder", folder);
			if (!sequence)
				b.StringField("file", folder + "\\" + PassFileStem(pass.elements) + "." + RenderFormatToken(job.out.format));
			b.EndObject();
		}
		b.EndArray();
		b.IntField("rendersPerFrame", RenderHost_PhysicalRendersPerFrame(plan.passes));
		b.Key("keyColor");
		b.BeginArray();
		for (int k = 0; k < 3; ++k) b.ValueDouble(job.passes.keyColor[k]);
		b.EndArray();
		b.DoubleField("depthNearUnits", job.passes.depthNear);
		b.DoubleField("depthFarUnits", job.passes.depthFar);
		b.StringField("depthEncoding", "linear gray: white at near, black at far");
		b.StringField("normalEncoding", "view space, rgb = n*0.5+0.5, +x right, +y up, +z toward camera, reconstructed from depth");
	}
	b.IntField("warmupTicks", job.warmupTicks);
	b.IntField("settleFrames", job.settleFrames);
	b.IntField("framesExpected", job.framesTotal);
	b.IntField("framesWritten", framesWritten);
	b.StringField("notes", notes);
	b.EndObject();

	std::wstring path = job.takeDir;
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path.push_back(L'\\');
	path.append(L"manifest.json");

	FILE* f = nullptr;
	if (0 != _wfopen_s(&f, path.c_str(), L"wb") || !f)
		return false;
	const std::string& s = b.Str();
	fwrite(s.data(), 1, s.size(), f);
	fclose(f);
	return true;
}

} // namespace

bool RenderManifest_WriteStart(const RenderJob& job, const std::string& demoPathUtf8,
	const std::string& notes) {
	return WriteManifest(job, demoPathUtf8, "rendering", 0, notes);
}

bool RenderManifest_WriteFinal(const RenderJob& job, const std::string& demoPathUtf8,
	const char* status, int framesWritten, const std::string& notes) {
	return WriteManifest(job, demoPathUtf8, status, framesWritten, notes);
}

bool RenderFrameMap_Open(const std::wstring& takeDir) {
	RenderFrameMap_Close();
	if (takeDir.empty())
		return false;
	std::wstring path = takeDir;
	if (path.back() != L'\\' && path.back() != L'/')
		path.push_back(L'\\');
	path.append(L"frames.csv");
	if (0 != _wfopen_s(&s_frameMap, path.c_str(), L"wb") || !s_frameMap) {
		s_frameMap = nullptr;
		return false;
	}
	fprintf(s_frameMap, "frame_index,demo_tick,demo_time,camera_x,camera_y,camera_z,pitch,yaw,roll,fov\n");
	return true;
}

void RenderFrameMap_Append(int frameIndex, int demoTick, double demoTime,
	const double origin[3], const double angles[3], double fov) {
	if (!s_frameMap)
		return;
	fprintf(s_frameMap, "%d,%d,%.6f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f\n",
		frameIndex, demoTick, demoTime,
		origin[0], origin[1], origin[2],
		angles[0], angles[1], angles[2], fov);
}

void RenderFrameMap_Close() {
	if (s_frameMap) {
		fclose(s_frameMap);
		s_frameMap = nullptr;
	}
}

} // namespace Filmmaker
