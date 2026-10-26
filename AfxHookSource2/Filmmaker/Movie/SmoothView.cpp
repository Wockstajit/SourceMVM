#include "SmoothView.h"
#include "SmoothViewHost.h" // the pawn's real velocity + ground flag, the render frame (CS2 and Source 1)
#include "SmoothViewTiltMath.h" // strafe lean: leans INTO the strafe, sign pinned by tests
#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace Filmmaker {

namespace { constexpr double kPi = 3.14159265358979323846; }

double SmoothView::Clamp(double v,double lo,double hi){return v<lo?lo:v>hi?hi:v;}
void SmoothView::Figure8(double phase,double&x,double&y){double s=std::sin(phase);x=s;y=s*std::cos(phase);}

SmoothView& SmoothViewRef(){static SmoothView s;return s;}

// ======= settings =======
void SmoothView::SetEnabled(bool on){if(on==m_enabled)return;m_enabled=on;if(on)Reset();}
void SmoothView::SetBreathFreqX(double v){m_breathFreqX=Clamp(v,0.1,2.0);}
void SmoothView::SetBreathFreqY(double v){m_breathFreqY=Clamp(v,0.1,2.0);}
void SmoothView::SetStandBreathAmp(double v){m_standAmp=Clamp(v,0.0,2.0);}
void SmoothView::SetWalkBreathAmp(double v){m_walkAmp=Clamp(v,0.0,2.0);}
void SmoothView::SetRunBreathAmp(double v){m_runAmp=Clamp(v,0.0,2.0);}
void SmoothView::SetTiltEnabled(bool on){m_tiltEnabled=on;}
void SmoothView::SetTiltFactor(double v){m_tiltFactor=Clamp(v,0.0,60.0);}
void SmoothView::SetTiltSmoothSpeed(double v){m_tiltSmoothSpeed=Clamp(v,0.5,30.0);}
void SmoothView::SetTiltInvert(bool on){m_tiltInvert=on;}
void SmoothView::SetFallingEnabled(bool on){m_fallingEnabled=on;}
void SmoothView::SetFallingIntensity(double v){m_fallingIntensity=Clamp(v,0.0,2.0);}
void SmoothView::SetJumpingEnabled(bool on){m_jumpingEnabled=on;}
void SmoothView::SetJumpingIntensity(double v){m_jumpingIntensity=Clamp(v,0.0,2.0);}
void SmoothView::SetHealthSwayEnabled(bool on){m_healthSwayEnabled=on;}
void SmoothView::SetHealthSwayIntensity(double v){m_healthSwayIntensity=Clamp(v,0.0,1.0);}
void SmoothView::SetPlayerHealth(double hp,double maxHp){m_playerHealth=Clamp(hp,0.0,maxHp);m_playerMaxHealth=std::max(maxHp,1.0);}

void SmoothView::ResetToDefaults(){
	m_enabled=false;
	m_breathFreqX=1.0;m_breathFreqY=1.0;m_standAmp=0.2;m_walkAmp=0.4;m_runAmp=0.8;
	m_tiltEnabled=true;m_tiltFactor=40.0;m_tiltSmoothSpeed=5.0;m_tiltInvert=false;
	m_fallingEnabled=true;m_jumpingEnabled=true;m_fallingIntensity=0.5;m_jumpingIntensity=0.5;
	m_healthSwayEnabled=true;m_healthSwayIntensity=0.5;
	Reset();
}

bool SmoothView::SettingsAreDefault() const {
	auto eq=[](double a,double b){return std::fabs(a-b)<=0.000001;};
	return !m_enabled&&eq(m_breathFreqX,1.0)&&eq(m_breathFreqY,1.0)
		&&eq(m_standAmp,0.2)&&eq(m_walkAmp,0.4)&&eq(m_runAmp,0.8)
		&&m_tiltEnabled&&eq(m_tiltFactor,40.0)&&eq(m_tiltSmoothSpeed,5.0)&&!m_tiltInvert
		&&m_fallingEnabled&&m_jumpingEnabled&&eq(m_fallingIntensity,0.5)
		&&eq(m_jumpingIntensity,0.5)&&m_healthSwayEnabled&&eq(m_healthSwayIntensity,0.5);
}

void SmoothView::Reset(){
	m_airMotionState={};m_breathAmp=m_standAmp;
	m_breathPhase=0;m_healthTime=0;m_tilt=m_tiltTarget=0;m_breathTargetAmp=m_standAmp;m_oscFrame=-1;
	m_haveMotion=false;m_motionSpeed=m_motionLateral=m_motionVz=0;
}

void SmoothView::SetPovContext(bool active,int pawnIndex){
	const bool activeChanged=active!=m_povActive;
	const bool pawnChanged=active&&pawnIndex>=0&&pawnIndex!=m_povPawnIndex;
	if(activeChanged||pawnChanged){
		if(MvmDebugLog_Active()){
			MvmDebugLog_Linef(
				"camera.smoothview.reset",
				"reason=%s oldActive=%d newActive=%d oldPawn=%d newPawn=%d",
				activeChanged?"pov-change":"pawn-change",
				m_povActive?1:0,active?1:0,m_povPawnIndex,pawnIndex);
		}
		Reset();
	}
	m_povActive=active;
	if(!active)m_povPawnIndex=-1;
	else if(pawnIndex>=0)m_povPawnIndex=pawnIndex;
}

// ======= motion sampler =======
// Extracted verbatim from the view-setup trampoline; the only behavioural change is that the
// state is per-instance instead of function-local statics, so Reset() actually clears it.
SmoothView::MotionSample SmoothView::SampleMotion(double curTime,double x,double y,double z,double yawDegrees){
	MotionSample out;
	const double dt=curTime-m_motionLastTime;
	// curtime is the DEMO clock: several render frames can share one tick, so dt is legitimately
	// 0 on some frames. Treat those as "no new motion sample" and reuse the previous estimate
	// rather than recomputing from a zero delta.
	// A dt below this is sub-tick noise in the demo clock, not a genuine new motion sample.
	// Dividing a normal few-unit strafe step by a ~1ms dt manufactures a velocity in the
	// thousands, which used to trip the teleport guard below MID-STRAFE: tilt snapped back to
	// centre and then leaned in again, repeatedly, exactly while A/D or counter-strafing.
	constexpr double kMinSampleDt=0.002;
	const bool newTick=(dt>=kMinSampleDt&&dt<=0.5);
	if(newTick){
		if(m_haveMotion){
			const double dx=x-m_motionLastX,dy=y-m_motionLastY,dz=z-m_motionLastZ;
			const double planar=std::sqrt(dx*dx+dy*dy)/dt;
			const double vzNow=dz/dt;
			// Teleport rejection, on DISTANCE rather than velocity. A demo seek or a spectator
			// switch moves the eye hundreds of units in one step; no player covers more than a
			// couple of dozen in a tick at any speed. The old test divided by dt first and
			// compared the VELOCITY (>4000 u/s), so it fired on a perfectly legitimate step
			// whenever dt happened to come in small -- and because the response is a full
			// Reset(), that wiped the live tilt and breath phase mid-move.
			constexpr double kTeleportDist=150.0;
			const double stepDist=std::sqrt(dx*dx+dy*dy+dz*dz);
			if(stepDist>kTeleportDist){
				++m_motionTeleports;
				// Every Reset() with a motion-derived cause is logged with its trigger values:
				// a wiped tilt/breath that correlates with plain strafing is a guard misfire,
				// and without the numbers here that misfire is invisible in the log.
				if(MvmDebugLog_Active()){
					MvmDebugLog_Linef(
						"camera.smoothview.reset",
						"reason=teleport stepDist=%.1f dt=%.5f pos=(%.1f %.1f %.1f) count=%llu",
						stepDist,dt,x,y,z,m_motionTeleports);
				}
				m_motionSpeed=m_motionLateral=m_motionVz=0.0;
				Reset();
			}else{
				m_motionSpeed=planar;
				m_motionVz=vzNow;
				// Source Right vector for yaw (pitch/roll do not affect it): (sin y, -cos y, 0).
				// Lateral = velocity . Right, so forward/back is exactly zero by construction.
				const double yawRad=yawDegrees*kPi/180.0;
				const double vx=dx/dt,vy=dy/dt;
				m_motionLateral=vx*std::sin(yawRad)-vy*std::cos(yawRad);
			}
		}
		m_motionLastX=x;m_motionLastY=y;m_motionLastZ=z;m_motionLastTime=curTime;m_haveMotion=true;
	}else if(dt<-0.050||dt>0.5){
		// Backwards seek or demo change: drop the sample so the next frame starts fresh instead
		// of integrating a teleport as a sprint.
		//
		// The lower bound is -50ms, NOT 0: the demo clock ROUTINELY steps backwards by ~6-9ms
		// between render frames during plain uninterrupted playback (measured 2026-08-02 on a
		// repaired match demo: 141 regressions of -5.5..-8.5ms in ~40s, mvm_debug reason=clock-jump
		// lines). The old `dt<0.0` treated every one as a seek and answered with a FULL Reset(),
		// which zeroes the live tilt and breath phase -- THE "tilt/breathing keeps snapping back
		// while strafing" jitter. A genuine backwards seek moves the clock by many ticks; anything
		// inside 50ms is sub-tick noise and simply falls through as "same tick" below. (A 1-2 tick
		// back-step also falls through, harmlessly: the sampler holds its last estimate.)
		if(MvmDebugLog_Active()){
			MvmDebugLog_Linef(
				"camera.smoothview.reset",
				"reason=clock-jump dt=%.5f curTime=%.4f lastTime=%.4f",
				dt,curTime,m_motionLastTime);
		}
		m_haveMotion=false;m_motionSpeed=m_motionLateral=m_motionVz=0.0;
		m_motionLastTime=curTime;
		Reset();
	}
	// dt in (-0.005, kMinSampleDt) falls through as "same tick": state is held, nothing resets.
	// dt 0 means "same demo tick, another render frame". Passed through as 0 rather than a fake
	// step: ApplyCamera then re-applies its current state without advancing any integrator. That
	// keeps the effect deterministic per demo tick (a rendered movie must be reproducible) and
	// makes a PAUSED demo hold a genuinely still frame to compose against.
	// THE PAWN'S OWN MOVEMENT, when it can be read. The values derived from the camera above are
	// a position difference divided by a demo-clock step that holds on half the frames and jitters
	// by milliseconds on the rest, and it showed: measured against the pawn's real velocity on 6558
	// strafing frames, the derived lateral speed had the right SIGN on only 62% of them and moved by
	// a median 104 u/s between consecutive frames while the real value moved 0.1 -- which is the
	// lean jittering, and often leaning the wrong way. The derived values stay only as the fallback
	// for when no pawn is under the camera.
	double pv[3]={0,0,0};bool pawnGround=true;
	const bool havePawn=SmoothViewHost_PawnMotion(x,y,z,pv,&pawnGround);
	double speed=m_motionSpeed,lateral=m_motionLateral,vzOut=m_motionVz;
	bool groundOut=std::fabs(m_motionVz)<50.0; // eye Z is the only ground signal without a pawn
	if(havePawn){
		const double yr=yawDegrees*kPi/180.0;
		speed=std::sqrt(pv[0]*pv[0]+pv[1]*pv[1]);
		lateral=pv[0]*std::sin(yr)-pv[1]*std::cos(yr);
		vzOut=pv[2];
		groundOut=pawnGround;
	}
	if(MvmDebugLog_Active()){
		// EVERY frame: the strafe-lean jitter is a frame-to-frame pattern.
		MvmDebugLog_LinefAlways("camera.smoothview.lateral",
			"curTime=%.5f dt=%.5f newTick=%d derivedLat=%.1f pawnLat=%.1f havePawn=%d used=%.1f yaw=%.2f",
			curTime,dt,newTick?1:0,m_motionLateral,havePawn?lateral:0.0,havePawn?1:0,lateral,yawDegrees);
	}
	out.dt=newTick?dt:0.0;
	out.speed=speed;
	out.lateralSpeed=lateral;
	out.vz=vzOut;
	out.onGround=groundOut;
	return out;
}

// ======= pump =======
// Main-thread tick. Deliberately does NOT integrate anything: every integrator lives in
// ApplyCamera, which is the only place that has a real dt AND a real speed. The old version
// advanced the breath here with a hardcoded dt of 0.016 and speed 0, so the amplitude always
// settled to the standing value no matter how fast the player was running.
void SmoothView::Pump(double,double){ ++m_frames; }

// ======= camera apply =======
void SmoothView::ApplyCamera(double& p,double& y,double& r,double& f,double dt,double speed,double lateralSpeed,double vz,bool onG,bool playbackPaused){
	if(!m_enabled)return;
	// Keep scoped compositions free of cosmetic motion.
	if(f<75.0){m_airMotionState={};return;}
	// dt == 0 is a legitimate, common case: curtime is the DEMO clock and several render frames
	// share one tick. Those frames must still APPLY the current offsets (or the view strobes
	// between corrected and uncorrected) but must not ADVANCE anything -- integrating the same
	// tick repeatedly would make the result depend on frame rate and would animate a paused demo.
	const bool integrate = (dt>0.0&&dt<=0.5);
	if(!integrate)dt=0.0;
	double renderDt=0.0;
	int responseFrame=0;
	SmoothViewHost_RenderFrame(renderDt,responseFrame);

	// Cosmetic motion advances once per render frame and freezes during demo pause.
	double oscDt = 0.0;
	if (!playbackPaused && responseFrame != m_oscFrame) {
		m_oscFrame = responseFrame;
		if (renderDt > 0.0 && renderDt <= 0.5) oscDt = renderDt;
	}
	// ---- breathing ---------------------------------------------------------------------
	// Amplitude ramps stand -> walk -> run over the mod's speed bands (0.1 / 0.4 / 0.7 of run
	// speed). CS2's run speed is 250 u/s.
	if(integrate){
		const double sn=Clamp(speed/250.0,0.0,1.0);
		m_breathTargetAmp = sn<0.1 ? m_standAmp
			: sn<0.4 ? m_standAmp+(m_walkAmp-m_standAmp)*((sn-0.1)/0.3)
			: sn<0.7 ? m_walkAmp +(m_runAmp -m_walkAmp )*((sn-0.4)/0.3)
			: m_runAmp;
	}
	if(oscDt>0.0){
		m_breathAmp+=(m_breathTargetAmp-m_breathAmp)*(1.0-std::exp(-oscDt*m_breathSpeed));
		m_breathPhase=std::fmod(m_breathPhase+((m_breathFreqX+m_breathFreqY)*0.5)*oscDt,kPi*2);
	}
	double bx,by;Figure8(m_breathPhase,bx,by);
	p+=bx*m_breathAmp;y+=by*m_breathAmp;r+=bx*m_breathAmp*0.25;m_breathX=bx*m_breathAmp;m_breathY=by*m_breathAmp;

	// ---- health sway (additional sine scaled by missing HP) ----
	if(m_healthSwayEnabled){
		if(oscDt>0.0)m_healthTime+=oscDt;
		const double hf=1.0-Clamp(m_playerHealth/m_playerMaxHealth,0.0,1.0);
		p+=std::sin(m_healthTime*1.5)*m_healthSwayIntensity*hf;
		y+=std::cos(m_healthTime*1.7)*m_healthSwayIntensity*hf;
	}

	// ---- falling/jumping pitch ----------------------------------------------------------
	// Velocity targets arrive on demo ticks; smooth cosmetic air motion on the render clock.
	const double airPitchBefore=p;
	const SmoothViewAirMotionResult airMotion=SmoothViewUpdateAirMotion(
		m_airMotionState,vz,onG,
		m_fallingEnabled,m_fallingIntensity,
		m_jumpingEnabled,m_jumpingIntensity,
		oscDt,oscDt>0.0);
	p+=airMotion.pitch;

	// ---- tilt --------------------------------------------------------------------------
	// Strafe lean. lateralSpeed is already velocity . Right (see main.cpp), so forward and
	// backward motion contribute exactly zero by construction -- only left/right does.
	//
	// Magnitude and SIGN both live in SmoothViewTiltMath.h, with the direction tests and the
	// measured roll convention (positive roll leans right). The lean goes INTO the strafe.
	if(m_tiltEnabled){
		if(integrate){
			m_tiltTarget=SmoothViewTiltTarget(lateralSpeed,m_tiltFactor,m_tiltInvert);
		}
		if(oscDt>0.0)m_tilt+=(m_tiltTarget-m_tilt)*(1.0-std::exp(-oscDt*m_tiltSmoothSpeed));
		r+=m_tilt;
	}else{
		m_tilt=m_tiltTarget=0.0;
	}

	if(m_tiltEnabled&&MvmDebugLog_Active()){
		MvmDebugLog_LinefAlways("camera.smoothview.roll",
			"frame=%d integrate=%d dt=%.5f oscDt=%.5f lateral=%.1f target=%.4f tilt=%.4f rollOut=%.4f paused=%d",
			responseFrame,integrate?1:0,dt,oscDt,lateralSpeed,m_tiltTarget,m_tilt,r,playbackPaused?1:0);
	}

	// Dedicated tilt/breath trace. This
	// carries everything the "lean resets while strafing" class of report needs: the raw motion
	// inputs, both smoother states and their targets, and the teleport-guard counter, so one
	// glance says whether a snap came from a Reset(), a target flip, or a starved oscDt.
	if(m_tiltEnabled&&MvmDebugLog_Active()){
		static unsigned s_tiltLogThrottle=0;
		if((s_tiltLogThrottle++%30)==0){
			MvmDebugLog_Linef(
				"camera.smoothview.tilt",
				"dt=%.5f oscDt=%.5f speed=%.1f lateral=%.1f tiltTarget=%.4f tilt=%.4f "
				"breathAmp=%.3f breathTarget=%.3f phase=%.3f teleports=%llu",
				dt,oscDt,speed,lateralSpeed,m_tiltTarget,m_tilt,
				m_breathAmp,m_breathTargetAmp,m_breathPhase,m_motionTeleports);
		}
	}

	if((m_fallingEnabled||m_jumpingEnabled)&&MvmDebugLog_Active()){
		static unsigned s_airLogThrottle=0;
		const bool airInteresting=!onG||std::fabs(vz)>1.0;
		if(airInteresting&&((s_airLogThrottle++%10)==0)){
			MvmDebugLog_Linef(
				"camera.smoothview.air",
				"frame=%d demoDt=%.5f renderDt=%.5f oscDt=%.5f integrate=%d onGround=%d "
				"vz=%.3f pitchBefore=%.4f target=%.4f filtered=%.4f cameraFinal=%.4f teleports=%llu",
				responseFrame,dt,renderDt,oscDt,integrate?1:0,onG?1:0,
				vz,airPitchBefore,airMotion.targetPitch,airMotion.pitch,p,m_motionTeleports);
		}
	}
}

// ======= debug =======
std::string SmoothView::DebugStateJson() const{
	std::ostringstream o;
	o<<"{\"on\":"<<(m_enabled?"true":"false")
		<<",\"breathAmp\":"<<m_breathAmp<<",\"breathX\":"<<m_breathX<<",\"breathY\":"<<m_breathY
		// tiltDeg is the roll actually added this frame (the mod's radians-as-degrees value),
		// NOT tiltFactor. The default factor 40 produces about 1.4 degrees.
		<<",\"tiltOn\":"<<(m_tiltEnabled?"true":"false")
		<<",\"motionTeleports\":"<<m_motionTeleports
		<<",\"tiltDeg\":"<<m_tilt<<",\"tiltTarget\":"<<m_tiltTarget<<",\"tiltInv\":"<<(m_tiltInvert?"true":"false")
		<<",\"healthSway\":"<<(m_healthSwayEnabled?"true":"false")<<",\"healthPct\":"<<(m_playerHealth/m_playerMaxHealth)
		<<",\"frames\":"<<m_frames<<"}";
	return o.str();
}

// ======= console =======
void SmoothView_RunCommand(int argc,advancedfx::ICommandArgs* args,const char* cmd){
	SmoothView& sv=SmoothViewRef();
	const char* a=(argc>=3)?args->ArgV(2):"";
	if(0==_stricmp(a,"on")||0==_stricmp(a,"off")||0==_stricmp(a,"toggle")){bool on=0==_stricmp(a,"toggle")?!sv.Enabled():0==_stricmp(a,"on")||0==_stricmp(a,"1");sv.SetEnabled(on);advancedfx::Message("mirv_filmmaker: smooth view %s.\n",on?"ON":"OFF");return;}
	if(0==_stricmp(a,"state")){advancedfx::Message("[smoothview][state] %s\n",sv.DebugStateJson().c_str());return;}
	if(0==_stricmp(a,"reset")){sv.ResetToDefaults();advancedfx::Message("smooth view reset to defaults.\n");return;}

	// The Config sliders append "quiet" and fire on every drag notch; echoing each one floods the
	// console (and, via the render->sync->command loop, costs a line per frame). Same contract as
	// ViewFx / HandheldCam / WeaponRecoil.
	const bool quiet = (argc>=5 && 0==_stricmp(args->ArgV(4),"quiet"));

	auto onoff=[&](const char* n,bool(SmoothView::*g)()const,void(SmoothView::*s)(bool)){
		if(argc>=4){const char*v=args->ArgV(3);bool on=0==_stricmp(v,"toggle")?!(sv.*g)():(0!=std::atoi(v)||0==_stricmp(v,"on")||0==_stricmp(v,"1"));(sv.*s)(on);}
		if(quiet)return;
		advancedfx::Message("mirv_filmmaker: smooth view %s = %s.\n",n,(sv.*g)()?"ON":"OFF");
	};
	auto num=[&](const char*n,double(SmoothView::*g)()const,void(SmoothView::*s)(double)){
		if(argc>=4)(sv.*s)(std::atof(args->ArgV(3)));
		if(quiet)return;
		advancedfx::Message("mirv_filmmaker: smooth view %s = %.2f.\n",n,(sv.*g)());
	};

	if(0==_stricmp(a,"breathX")){num("breath freq X",&SmoothView::BreathFreqX,&SmoothView::SetBreathFreqX);return;}
	if(0==_stricmp(a,"breathY")){num("breath freq Y",&SmoothView::BreathFreqY,&SmoothView::SetBreathFreqY);return;}
	if(0==_stricmp(a,"standAmp")){num("stand breath amp",&SmoothView::StandBreathAmp,&SmoothView::SetStandBreathAmp);return;}
	if(0==_stricmp(a,"walkAmp")){num("walk breath amp",&SmoothView::WalkBreathAmp,&SmoothView::SetWalkBreathAmp);return;}
	if(0==_stricmp(a,"runAmp")){num("run breath amp",&SmoothView::RunBreathAmp,&SmoothView::SetRunBreathAmp);return;}
	if(0==_stricmp(a,"tiltEnabled")){onoff("tilt",&SmoothView::TiltEnabled,&SmoothView::SetTiltEnabled);return;}
	if(0==_stricmp(a,"tilt")){num("tilt factor",&SmoothView::TiltFactor,&SmoothView::SetTiltFactor);return;}
	if(0==_stricmp(a,"tiltSpeed")){num("tilt smooth speed",&SmoothView::TiltSmoothSpeed,&SmoothView::SetTiltSmoothSpeed);return;}
	if(0==_stricmp(a,"tiltInv")){onoff("tilt invert",&SmoothView::TiltInvert,&SmoothView::SetTiltInvert);return;}
	if(0==_stricmp(a,"falling")){onoff("falling",&SmoothView::FallingEnabled,&SmoothView::SetFallingEnabled);return;}
	if(0==_stricmp(a,"jumping")){onoff("jumping",&SmoothView::JumpingEnabled,&SmoothView::SetJumpingEnabled);return;}
	if(0==_stricmp(a,"healthSway")){onoff("health sway",&SmoothView::HealthSwayEnabled,&SmoothView::SetHealthSwayEnabled);return;}
	advancedfx::Message("%s smoothview on|off|toggle\n"
		"  breathX|breathY [0.1-2] / standAmp|walkAmp|runAmp [0-2]\n"
		"  tiltEnabled [0|1] / tilt [0-60, default 40] / tiltSpeed [0.5-30] / tiltInv [0|1]\n"
		"  falling|jumping|healthSway [0|1] / reset / state\n",cmd);
}
}
