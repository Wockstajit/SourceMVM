// verify-fx-weapons-go is a per-weapon FX verifier for mirv_filmmaker's particle-swap
// modes (see Docs/features/filmmaker_effects_modifiers.md). It is the per-weapon-class companion to
// Automation/verify/verify-fx-allweapons.ps1 (which asserts coarse effect-group coverage);
// this tool additionally buckets every observed particle system by WEAPON CLASS (assault
// rifle, awp, autosniper, pistol, ...) using the exact match strings from
// AfxHookSource2/Filmmaker/Movie/ParticleFxRules.cpp's kVariantWeaponFx/kVariantTracers tables,
// and takes an event-triggered screenshot the first time each (mode, class, effect) combo
// is confirmed swapped, instead of blind timer-based captures.
//
// Usage (CS2 must already be running with netcon -- Automation/launch/launch-cs2-netcon.ps1):
//
//	go run . -demo "all weapon test .dem" -profiles classic,modern
//
// Output lands under Automation/runs/fx-weapons-go/<timestamp>/ (git-ignored):
// report.json (full machine-readable results), summary.txt (human table), screenshots,
// fx-names-<profile>.txt / fx-recent-<profile>.txt (raw dumps for manual review), and
// unmapped-<profile>.txt (weapon-path systems nothing acted on -- candidates for new
// FXRULE/FXRULE_MODERN entries; this is where bug-2 tracer gaps show up in practice).
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

// ---------------------------------------------------------------------------------------
// Weapon/effect classification -- mirrors kVariantWeaponFx / kVariantTracers in
// AfxHookSource2/Filmmaker/Movie/ParticleFxRules.cpp (transcribed 2026-07-03). Keyed by the
// exact lowercase resource path CS2 passes into the create hook, i.e. exactly what
// `mirv_filmmaker fx names`/`fx recent` print. Any weapon-path name NOT in this table is
// surfaced separately as "unmapped" -- that is the ground-truth audit for bug 2 (tracer
// coverage gaps), not a guess against an external CS2 weapon list.
// ---------------------------------------------------------------------------------------

type effectKind string

const (
	effMuzzleFlash effectKind = "muzzleflash"
	effTracer      effectKind = "tracer"
	effSmoke       effectKind = "barrelsmoke"
	effWisp        effectKind = "wisp"
	effShell       effectKind = "shellcasing"
	effGroundDust  effectKind = "grounddust"
)

type weaponEntry struct {
	class  string
	effect effectKind
}

var knownNames = map[string]weaponEntry{
	// --- muzzle flash (particles/unified_weapon_fx/*) ---
	"particles/unified_weapon_fx/uweapon_muzflsh_ak47.vpcf":              {"assaultrifle", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzflsh_ak47_fps.vpcf":          {"assaultrifle", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzflsh_riffle.vpcf":            {"assaultrifle", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzflsh_riffle_fps.vpcf":        {"assaultrifle", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzflsh_aug.vpcf":               {"assaultrifle", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzflsh_aug_fps.vpcf":           {"assaultrifle", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzflsh_aug_fps_ironsight.vpcf": {"assaultrifle", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzflsh_sg_fps_ironsight.vpcf":  {"assaultrifle", effMuzzleFlash},
	"particles/unified_weapon_fx/weapon_muzzleflash_basic.vpcf":          {"assaultrifle", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzsilenced_rif.vpcf":           {"rifle_silenced", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzsilenced_rif_fps.vpcf":       {"rifle_silenced", effMuzzleFlash},

	"particles/unified_weapon_fx/uweapon_muzflsh_shot.vpcf":     {"shotgun", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzflsh_shot_fps.vpcf": {"shotgun", effMuzzleFlash},

	"particles/unified_weapon_fx/weapon_muzzleflash_snip.vpcf":     {"awp", effMuzzleFlash},
	"particles/unified_weapon_fx/weapon_muzzleflash_snip_fps.vpcf": {"awp", effMuzzleFlash},

	"particles/unified_weapon_fx/uweapon_muzflsh_riffle_lrg.vpcf":     {"autosniper", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzflsh_riffle_lrg_fps.vpcf": {"autosniper", effMuzzleFlash},
	"particles/unified_weapon_fx/weapon_muzzleflash_snip_ar.vpcf":     {"autosniper", effMuzzleFlash},
	"particles/unified_weapon_fx/weapon_muzzleflash_snip_ar_fps.vpcf": {"autosniper", effMuzzleFlash},

	"particles/unified_weapon_fx/uweapon_muzzleflash_subm.vpcf":     {"smg", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzzleflash_subm_fps.vpcf": {"smg", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzsilenced_subm.vpcf":     {"smg_silenced", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzsilenced_subm_fps.vpcf": {"smg_silenced", effMuzzleFlash},

	"particles/unified_weapon_fx/uweapon_muzzleflash_pist.vpcf":               {"pistol", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzzleflash_pist_fps.vpcf":           {"pistol", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzflsh_deagle.vpcf":                 {"deagle", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzflsh_deagle_fps.vpcf":             {"deagle", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzzleflash_pist_revolver.vpcf":      {"revolver", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzzleflash_pist_revolver_fps.vpcf":  {"revolver", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzzleflash_pist_fire_revolver.vpcf": {"revolver", effMuzzleFlash},

	"particles/unified_weapon_fx/uweapon_muzflsh_mach.vpcf":     {"lmg", effMuzzleFlash},
	"particles/unified_weapon_fx/uweapon_muzflsh_mach_fps.vpcf": {"lmg", effMuzzleFlash},

	"particles/unified_weapon_fx/uweapon_muzflsh_ground_smoke.vpcf": {"sniper_grounddust", effGroundDust},

	// --- tracers (particles/weapons/cs_weapon_fx/*) ---
	"particles/weapons/cs_weapon_fx/weapon_tracers.vpcf":              {"generic", effTracer},
	"particles/weapons/cs_weapon_fx/weapon_tracers_pistol.vpcf":       {"pistol", effTracer},
	"particles/weapons/cs_weapon_fx/weapon_tracers_smg.vpcf":          {"smg", effTracer},
	"particles/weapons/cs_weapon_fx/weapon_tracers_rifle.vpcf":        {"rifle", effTracer},
	"particles/weapons/cs_weapon_fx/weapon_tracers_rifle_scar.vpcf":   {"autosniper", effTracer},
	"particles/weapons/cs_weapon_fx/weapon_tracers_rifle_ssg.vpcf":    {"awp", effTracer},
	"particles/weapons/cs_weapon_fx/weapon_tracers_assrifle.vpcf":     {"assaultrifle", effTracer},
	"particles/weapons/cs_weapon_fx/weapon_tracers_assrifle_aug.vpcf": {"assaultrifle", effTracer},
	"particles/weapons/cs_weapon_fx/weapon_tracers_mach.vpcf":         {"lmg", effTracer},
	"particles/weapons/cs_weapon_fx/weapon_tracers_shot.vpcf":         {"shotgun", effTracer},

	// --- sustained barrel smoke (weapon-class-agnostic top-level systems) ---
	"particles/weapons/cs_weapon_fx/weapon_muzzle_smoke.vpcf":             {"sustained", effSmoke},
	"particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_b.vpcf":           {"sustained", effSmoke},
	"particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_b_version_2.vpcf": {"sustained", effSmoke},
	"particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_long.vpcf":        {"sustained", effSmoke},
	"particles/weapons/cs_weapon_fx/weapon_muzzle_smoke_long_b.vpcf":      {"sustained", effSmoke},

	// --- shell casings (coverage-only -- no swap rule exists for these today) ---
	"particles/weapons/cs_weapon_fx/weapon_shell_casing_9mm.vpcf":          {"pistol", effShell},
	"particles/weapons/cs_weapon_fx/weapon_shell_casing_45acp.vpcf":        {"pistol", effShell},
	"particles/weapons/cs_weapon_fx/weapon_shell_casing_57.vpcf":           {"pistol", effShell},
	"particles/weapons/cs_weapon_fx/weapon_shell_casing_rifle.vpcf":        {"rifle", effShell},
	"particles/weapons/cs_weapon_fx/weapon_shell_casing_awp.vpcf":          {"awp", effShell},
	"particles/weapons/cs_weapon_fx/weapon_shell_casing_deagle.vpcf":       {"deagle", effShell},
	"particles/weapons/cs_weapon_fx/weapon_shell_casing_shotgun.vpcf":      {"shotgun", effShell},
	"particles/weapons/cs_weapon_fx/weapon_shell_casing_shotgun_mag7.vpcf": {"shotgun", effShell},
	"particles/weapons/cs_weapon_fx/weapon_shell_casing_shotgun_nova.vpcf": {"shotgun", effShell},
}

// Modern-only sniper composition targets (arc9_fas_muzzleflashes/mvm_muzzleflash_sniper_*)
// bundle ground-dust + heatwave/distortion + barrel-smoke-plume as CHILD particle systems.
// Children bypass Hook_CreateBody entirely (see ParticleFx.h's documented architecture), so
// they can never appear as their own top-level creation in `fx names`/`fx recent`. Seeing
// the PARENT composition resolved in `fx recent`'s "-> target" column is therefore the only
// netcon-observable signal that the heatwave/ground-dust bundle was used; whether the
// children actually rendered correctly (position, visibility) still requires eyeballing the
// screenshot this tool captures at that moment.
var sniperCompositionHint = map[string]string{
	"arc9_fas_muzzleflashes/mvm_muzzleflash_sniper_awp":  "awp",
	"arc9_fas_muzzleflashes/mvm_muzzleflash_sniper_auto": "autosniper",
}

// Modern per-shot class flashes (barrel_smoke + rope wisp are PCF children; world twin only).
var modernFlashWispClass = map[string]string{
	"muzzleflash_ar.vpcf":            "assaultrifle",
	"muzzleflash_smg.vpcf":           "smg",
	"muzzleflash_shotgun.vpcf":       "shotgun",
	"muzzleflash_pistol.vpcf":        "pistol",
	"muzzleflash_pistol_deagle.vpcf": "deagle",
	"muzzleflash_lmg.vpcf":           "lmg",
	"muzzleflash_dmr.vpcf":           "autosniper",
}

// weaponPathPrefixes: used to decide whether an unrecognized name belongs in the
// "unmapped" report (a weapon-fx system nothing acted on) vs. being irrelevant noise.
var weaponPathPrefixes = []string{
	"particles/weapons/cs_weapon_fx/",
	"particles/unified_weapon_fx/",
}

// ---------------------------------------------------------------------------------------
// netcon client
// ---------------------------------------------------------------------------------------

type NetCon struct {
	conn net.Conn
}

func dialNetcon(port int) (*NetCon, error) {
	conn, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), 5*time.Second)
	if err != nil {
		return nil, err
	}
	if tc, ok := conn.(*net.TCPConn); ok {
		tc.SetNoDelay(true)
	}
	return &NetCon{conn: conn}, nil
}

func (c *NetCon) drain(d time.Duration) string {
	var sb strings.Builder
	buf := make([]byte, 16384)
	deadline := time.Now().Add(d)
	for {
		remain := time.Until(deadline)
		if remain <= 0 {
			break
		}
		step := remain
		if step > 100*time.Millisecond {
			step = 100 * time.Millisecond
		}
		c.conn.SetReadDeadline(time.Now().Add(step))
		n, err := c.conn.Read(buf)
		if n > 0 {
			sb.Write(buf[:n])
		}
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			break
		}
	}
	return sb.String()
}

func (c *NetCon) send(cmd string, wait time.Duration) string {
	c.drain(60 * time.Millisecond)
	c.conn.Write([]byte(cmd + "\n"))
	return c.drain(wait)
}

func (c *NetCon) Close() { c.conn.Close() }

// ---------------------------------------------------------------------------------------
// fx state / fx names / fx recent parsing
// ---------------------------------------------------------------------------------------

type FxState struct {
	Installed bool              `json:"installed"`
	Seen      int64             `json:"seen"`
	Modes     map[string]string `json:"modes"`
}

// dewrapConsoleLine removes hard line-wraps the game console/netcon relay injects into a
// single long printed line once it exceeds the console's display width (observed
// 2026-07-03: a >230-char "fx state" JSON blob came back with a raw CRLF spliced into the
// middle of a token, e.g. "of\r\nf" instead of "off" -- NOT a real newline, just word-wrap
// baked into the byte stream). Safe here because this response is always one logical line;
// stripping CR/LF reassembles the original text exactly (no whitespace was actually there).
func dewrapConsoleLine(text string) string {
	return strings.NewReplacer("\r\n", "", "\r", "", "\n", "").Replace(text)
}

func readFxState(nc *NetCon) *FxState {
	text := nc.send("mirv_filmmaker fx state", time.Second)
	idx := strings.Index(text, "[fx][state]")
	if idx < 0 {
		return nil
	}
	braceStart := strings.Index(text[idx:], "{")
	if braceStart < 0 {
		return nil
	}
	braceStart += idx
	depth := 0
	for i := braceStart; i < len(text); i++ {
		switch text[i] {
		case '{':
			depth++
		case '}':
			depth--
			if depth == 0 {
				payload := dewrapConsoleLine(text[braceStart : i+1])
				var st FxState
				if err := json.Unmarshal([]byte(payload), &st); err != nil {
					return nil
				}
				return &st
			}
		}
	}
	return nil
}

type NameStat struct {
	Name  string
	Seen  int64
	Acted int64
}

var fxNamesRe = regexp.MustCompile(`(?m)^\s+(\d+)\s+(\d+)\s+(particles/\S+)\s*$`)
var fxNamesRowStartRe = regexp.MustCompile(`^\s*\d+\s+\d+\s+particles/`)

// dewrapConsoleTable undoes the same console hard-wrap dewrapConsoleLine handles (see its
// doc comment), but for multi-row table output where a real newline per row must be kept:
// a wrapped continuation line (one that does NOT look like the start of a new row) is
// spliced directly onto the end of the previous row instead of being treated as its own
// line -- long Modern resource paths (e.g. .../muzzleflash_pistol_deagle.vpcf)
// are exactly the kind of long single line that triggers this.
func dewrapConsoleTable(text string, rowStart *regexp.Regexp) string {
	normalized := strings.ReplaceAll(text, "\r\n", "\n")
	normalized = strings.ReplaceAll(normalized, "\r", "\n")
	var out []string
	for _, line := range strings.Split(normalized, "\n") {
		if len(out) == 0 || rowStart.MatchString(line) {
			out = append(out, line)
		} else {
			out[len(out)-1] += line
		}
	}
	return strings.Join(out, "\n")
}

func readFxNames(nc *NetCon, filter string) map[string]NameStat {
	text := dewrapConsoleTable(nc.send("mirv_filmmaker fx names "+filter, 1200*time.Millisecond), fxNamesRowStartRe)
	out := map[string]NameStat{}
	for _, m := range fxNamesRe.FindAllStringSubmatch(text, -1) {
		seen, _ := strconv.ParseInt(m[1], 10, 64)
		acted, _ := strconv.ParseInt(m[2], 10, 64)
		out[m[3]] = NameStat{Name: m[3], Seen: seen, Acted: acted}
	}
	return out
}

var fxRecentRe = regexp.MustCompile(`(?m)^\s*([=X>])\s+(particles/\S+)(?:\s+->\s+(\S+))?\s*$`)
var fxRecentRowStartRe = regexp.MustCompile(`^\s*[=X>]\s+particles/`)

type recentEvent struct {
	action string
	raw    string
	target string
}

func readFxRecent(nc *NetCon, n int) []recentEvent {
	text := dewrapConsoleTable(nc.send(fmt.Sprintf("mirv_filmmaker fx recent %d", n), 1200*time.Millisecond), fxRecentRowStartRe)
	var out []recentEvent
	for _, m := range fxRecentRe.FindAllStringSubmatch(text, -1) {
		out = append(out, recentEvent{action: m[1], raw: m[2], target: m[3]})
	}
	return out
}

// ---------------------------------------------------------------------------------------
// profiles (mirrors verify-fx-allweapons.ps1's $profileModes exactly)
// ---------------------------------------------------------------------------------------

var profileModes = map[string]map[string]string{
	"classic": {"impacts": "on", "tracers": "on", "weaponfx": "on", "blood": "on",
		"explosions": "on", "bombfx": "on", "molotov": "on", "mapfx": "on"},
	"modern": {"impacts": "less", "tracers": "modern", "weaponfx": "modern", "blood": "on",
		"explosions": "modern", "bombfx": "on", "molotov": "on", "mapfx": "on"},
	"less": {"impacts": "less", "tracers": "on", "weaponfx": "on", "blood": "on",
		"explosions": "less", "bombfx": "less", "molotov": "on", "mapfx": "on"},
}

var sweepFilters = []string{"muz", "tracer", "smoke", "muzzle_smoke", "ground_smoke", "shocksmoke",
	"shell", "impact", "explosion", "inferno", "molotov", "incend", "thrown", "trail",
	"blood", "weapons/cs_weapon_fx", "unified_weapon_fx"}

// ---------------------------------------------------------------------------------------
// report types
// ---------------------------------------------------------------------------------------

type weaponRow struct {
	Class      string   `json:"class"`
	Effect     string   `json:"effect"`
	Seen       int64    `json:"seen"`
	Acted      int64    `json:"acted"`
	Status     string   `json:"status"` // PASS / FAIL / NOT-OBSERVED / NA
	Screenshot string   `json:"screenshot,omitempty"`
	Names      []string `json:"names"`
}

type profileReport struct {
	Profile      string      `json:"profile"`
	Rows         []weaponRow `json:"rows"`
	Unmapped     []string    `json:"unmapped"`
	FxNamesFile  string      `json:"fxNamesFile"`
	FxRecentFile string      `json:"fxRecentFile"`
}

type fullReport struct {
	Demo      string          `json:"demo"`
	StartedAt string          `json:"startedAt"`
	Profiles  []profileReport `json:"profiles"`
}

// ---------------------------------------------------------------------------------------
// screenshot capture (shells out to the existing capture-game-window.ps1, per repo
// convention -- automation is testing-only and this keeps a single window-finding
// implementation instead of reimplementing it in Go).
// ---------------------------------------------------------------------------------------

// snap shells out per screenshot rather than reusing one long-lived process, so a single
// stuck capture (observed 2026-07-03: a screen-capture call can hang indefinitely, e.g. on
// a graphics-driver hiccup) must not be allowed to wedge the whole harness for the rest of
// the profile -- bound it with a hard timeout and keep going, logging instead of hanging.
const snapTimeout = 15 * time.Second

func snap(automationRoot, outDir, label string) string {
	script := filepath.Join(automationRoot, "capture", "capture-game-window.ps1")
	if _, err := os.Stat(script); err != nil {
		return ""
	}
	outPath := filepath.Join(outDir, label+".png")
	ctx, cancel := context.WithTimeout(context.Background(), snapTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script,
		"-Process", "cs2", "-Mode", "client", "-Out", outPath)
	if err := cmd.Run(); err != nil {
		if ctx.Err() == context.DeadlineExceeded {
			fmt.Fprintf(os.Stderr, "[WARN] screenshot %q timed out after %s, skipping\n", label, snapTimeout)
		}
		return ""
	}
	if _, err := os.Stat(outPath); err != nil {
		return ""
	}
	return outPath
}

func recentEventKey(ev recentEvent) string {
	return ev.action + "|" + ev.raw + "|" + ev.target
}

// seedRecentDedup marks everything already in fx recent as seen so we only screenshot
// live events after demo playback starts (backlog entries are minutes old by the time
// the first poll runs and would capture the wrong weapon on screen).
func seedRecentDedup(nc *NetCon, seenRecentKey map[string]bool) {
	for _, ev := range readFxRecent(nc, 120) {
		seenRecentKey[recentEventKey(ev)] = true
	}
	fmt.Println("  [seed] primed fx-recent dedup (skipping backlog)")
}

type shotTracker struct {
	profile           string
	automationRoot    string
	outDir            string
	captureEnabled    bool
	screenshotted     map[string]string
	heatwaveConfirmed map[string]bool
}

func captureNeedsPause(comboKey string) bool {
	return strings.HasSuffix(comboKey, "|wisp") || strings.HasSuffix(comboKey, "|barrelsmoke")
}

func (t *shotTracker) maybeSnap(nc *NetCon, comboKey, label string, paused bool) {
	if !t.captureEnabled {
		return
	}
	if _, done := t.screenshotted[comboKey]; done {
		return
	}
	if !paused && captureNeedsPause(comboKey) {
		return
	}
	if paused && !captureNeedsPause(comboKey) {
		return
	}
	var path string
	if paused {
		path = snap(t.automationRoot, t.outDir, label)
	} else {
		path = snap(t.automationRoot, t.outDir, label)
	}
	if path == "" {
		return
	}
	t.screenshotted[comboKey] = path
	fmt.Printf("  [shot] %s -> %s\n", comboKey, path)
}

func (t *shotTracker) processEvent(nc *NetCon, ev recentEvent, paused bool) {
	if entry, ok := knownNames[ev.raw]; ok {
		comboKey := entry.class + "|" + string(entry.effect)
		label := fmt.Sprintf("%s-%s-%s", t.profile, entry.class, entry.effect)
		t.maybeSnap(nc, comboKey, label, paused)
	}
	if ev.target == "" {
		return
	}
	targetLower := strings.ToLower(ev.target)
	for hint, class := range sniperCompositionHint {
		if strings.Contains(targetLower, hint) {
			t.heatwaveConfirmed[class] = true
			comboKey := class + "|heatwave_grounddust"
			label := fmt.Sprintf("%s-%s-heatwave", t.profile, class)
			t.maybeSnap(nc, comboKey, label, paused)
		}
	}
	if t.profile != "modern" || ev.action != ">" {
		return
	}
	for hint, class := range modernFlashWispClass {
		if strings.Contains(targetLower, hint) && !strings.Contains(targetLower, "_fp") {
			comboKey := class + "|wisp"
			label := fmt.Sprintf("%s-%s-wisp", t.profile, class)
			t.maybeSnap(nc, comboKey, label, paused)
			break
		}
	}
	if strings.Contains(targetLower, "/modern/") &&
		(strings.Contains(targetLower, "muzzleflash_") || strings.Contains(targetLower, "barrel_smoke")) {
		if entry, ok := knownNames[ev.raw]; ok && entry.effect == effMuzzleFlash {
			comboKey := entry.class + "|barrelsmoke"
			label := fmt.Sprintf("%s-%s-barrelsmoke", t.profile, entry.class)
			t.maybeSnap(nc, comboKey, label, paused)
		}
	}
}

// ---------------------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------------------

func main() {
	port := flag.Int("port", 29010, "CS2 netcon TCP port")
	demoName := flag.String("demo", "all weapon test .dem", `playdemo argument, relative to game/csgo`)
	profilesFlag := flag.String("profiles", "classic,modern", "comma-separated profiles to verify (classic, modern, less)")
	timescale := flag.Float64("timescale", 2.0, "demo_timescale during observation")
	stallSeconds := flag.Int("stall-seconds", 25, "stop a profile once the seen-counter stalls this long")
	maxMinutes := flag.Float64("max-minutes", 20, "hard cap per profile")
	pollSeconds := flag.Float64("poll-seconds", 2.0, "how often to poll fx recent for new per-weapon events")
	skipScreenshots := flag.Bool("skip-screenshots", false, "do not pause for mid-fire screenshots (use only when capture is unavailable)")
	skipDemoLoad := flag.Bool("skip-demo-load", false, "demo already playing; just apply profile + observe")
	outDirFlag := flag.String("out-dir", "", "override output directory")
	flag.Parse()

	repoRoot, err := findRepoRoot()
	if err != nil {
		fmt.Fprintln(os.Stderr, "could not locate repo root (looked for CLAUDE.md):", err)
		os.Exit(1)
	}
	automationRoot := filepath.Join(repoRoot, "Automation")

	outDir := *outDirFlag
	if outDir == "" {
		ts := time.Now().Format("2006-01-02_150405")
		outDir = filepath.Join(automationRoot, "runs", "fx-weapons-go", ts)
	}
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		fmt.Fprintln(os.Stderr, "failed to create output dir:", err)
		os.Exit(1)
	}
	fmt.Println("=== verify-fx-weapons-go ===")
	fmt.Println("output dir:", outDir)

	profiles := strings.Split(*profilesFlag, ",")
	for i := range profiles {
		profiles[i] = strings.TrimSpace(profiles[i])
	}

	nc, err := dialNetcon(*port)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[FAIL] no CS2 netcon on 127.0.0.1:%d: %v\n", *port, err)
		os.Exit(1)
	}
	defer nc.Close()

	state := readFxState(nc)
	if state == nil {
		fmt.Fprintln(os.Stderr, "[FAIL] mirv_filmmaker fx state returned nothing (hook DLL not loaded?)")
		os.Exit(1)
	}
	savedModes := map[string]string{}
	for k, v := range state.Modes {
		savedModes[k] = v
	}
	nc.send("mirv_filmmaker fx on quiet", 300*time.Millisecond)
	nc.send("mirv_filmmaker fx debughud on", 300*time.Millisecond)
	nc.send("mvm_debug start", 400*time.Millisecond)

	report := fullReport{Demo: *demoName, StartedAt: time.Now().Format(time.RFC3339)}
	anyFail := false

	for _, profile := range profiles {
		modes, ok := profileModes[profile]
		if !ok {
			fmt.Printf("[SKIP] unknown profile %q\n", profile)
			continue
		}
		fmt.Printf("--- Profile %q ---\n", profile)
		for cat, mode := range modes {
			nc.send(fmt.Sprintf("mirv_filmmaker fx set %s %s quiet", cat, mode), 300*time.Millisecond)
		}
		nc.send("mirv_filmmaker fx log on", 400*time.Millisecond)

		if !*skipDemoLoad {
			fmt.Println("Loading demo:", *demoName)
			preLoad := readFxState(nc)
			var preSeen int64
			if preLoad != nil {
				preSeen = preLoad.Seen
			}
			nc.send(fmt.Sprintf(`playdemo "%s"`, *demoName), 2*time.Second)
			loadDeadline := time.Now().Add(120 * time.Second)
			armed := false
			for time.Now().Before(loadDeadline) {
				time.Sleep(3 * time.Second)
				st := readFxState(nc)
				if st != nil && st.Installed && st.Seen > preSeen {
					armed = true
					break
				}
			}
			if !armed {
				fmt.Printf("[FAIL] [%s] demo did not load / hook did not arm within 120s\n", profile)
				anyFail = true
				continue
			}
			fmt.Printf("[PASS] [%s] demo loaded, hook armed, particles flowing\n", profile)
		} else {
			nc.send("demo_gototick 0", time.Second)
		}
		nc.send(fmt.Sprintf("demo_timescale %g", *timescale), 300*time.Millisecond)
		nc.send("demo_resume", 300*time.Millisecond)

		// Per-weapon event-triggered screenshot tracking: on each live fx-state growth,
		// pause demo and snap only brand-new tail entries from fx recent (never the startup
		// backlog — those events are stale and would capture the wrong weapon on screen).
		tracker := &shotTracker{
			profile:           profile,
			automationRoot:    automationRoot,
			outDir:            outDir,
			captureEnabled:    !*skipScreenshots,
			screenshotted:     map[string]string{},
			heatwaveConfirmed: map[string]bool{},
		}
		seenRecentKey := map[string]bool{}
		seedRecentDedup(nc, seenRecentKey)

		baseline := readFxState(nc)
		var lastSeen int64
		if baseline != nil {
			lastSeen = baseline.Seen
		}
		lastGrowth := time.Now()
		started := time.Now()
		pollInterval := time.Duration(*pollSeconds * float64(time.Second))
		if pollInterval < 200*time.Millisecond {
			pollInterval = 200 * time.Millisecond
		}

		for {
			time.Sleep(pollInterval)
			now := time.Now()
			st := readFxState(nc)
			if st != nil && st.Seen > lastSeen {
				var fresh []recentEvent
				for _, ev := range readFxRecent(nc, 24) {
					key := recentEventKey(ev)
					if seenRecentKey[key] {
						continue
					}
					seenRecentKey[key] = true
					fresh = append(fresh, ev)
				}
				// Fast effects (muzzle/tracer/shell): snap while demo still running so flash is visible.
				for _, ev := range fresh {
					tracker.processEvent(nc, ev, false)
				}
				needPause := false
				for _, ev := range fresh {
					if entry, ok := knownNames[ev.raw]; ok {
						comboKey := entry.class + "|barrelsmoke"
						if captureNeedsPause(comboKey) {
							if _, done := tracker.screenshotted[comboKey]; !done {
								needPause = true
								break
							}
						}
					}
					if ev.target == "" || profile != "modern" || ev.action != ">" {
						continue
					}
					targetLower := strings.ToLower(ev.target)
					for hint, class := range modernFlashWispClass {
						if strings.Contains(targetLower, hint) && !strings.Contains(targetLower, "_fp") {
							comboKey := class + "|wisp"
							if _, done := tracker.screenshotted[comboKey]; !done {
								needPause = true
							}
							break
						}
					}
					if needPause {
						break
					}
					if strings.Contains(targetLower, "/modern/") &&
						(strings.Contains(targetLower, "muzzleflash_") || strings.Contains(targetLower, "barrel_smoke")) {
						if entry, ok := knownNames[ev.raw]; ok && entry.effect == effMuzzleFlash {
							comboKey := entry.class + "|barrelsmoke"
							if _, done := tracker.screenshotted[comboKey]; !done {
								needPause = true
								break
							}
						}
					}
				}
				if needPause && tracker.captureEnabled {
					nc.send("demo_pause", 250*time.Millisecond)
					time.Sleep(150 * time.Millisecond)
					for _, ev := range fresh {
						tracker.processEvent(nc, ev, true)
					}
					nc.send("demo_resume", 250*time.Millisecond)
				}
				lastSeen = st.Seen
				lastGrowth = now
			}

			if now.Sub(lastGrowth).Seconds() >= float64(*stallSeconds) {
				break
			}
			if now.Sub(started).Minutes() >= *maxMinutes {
				fmt.Println("max-minutes reached; collecting results anyway")
				break
			}
		}
		screenshotted := tracker.screenshotted
		heatwaveConfirmed := tracker.heatwaveConfirmed
		nc.send("demo_pause", 300*time.Millisecond)
		nc.send("demo_timescale 1", 300*time.Millisecond)
		fmt.Printf("Playback observed for %.0fs, %d creations.\n", time.Since(started).Seconds(), lastSeen)

		// Aggregate fx names across all sweep filters.
		names := map[string]NameStat{}
		for _, f := range sweepFilters {
			for k, v := range readFxNames(nc, f) {
				names[k] = v
			}
		}
		namesFile := filepath.Join(outDir, fmt.Sprintf("fx-names-%s.txt", profile))
		writeSortedNames(namesFile, names)

		recentFile := filepath.Join(outDir, fmt.Sprintf("fx-recent-%s.txt", profile))
		os.WriteFile(recentFile, []byte(nc.send("mirv_filmmaker fx recent 120", 1500*time.Millisecond)), 0o644)

		// Build per (class, effect) rows from the classification table.
		agg := map[string]*weaponRow{}
		for name, stat := range names {
			entry, ok := knownNames[name]
			if !ok {
				continue
			}
			key := entry.class + "|" + string(entry.effect)
			row, ok := agg[key]
			if !ok {
				row = &weaponRow{Class: entry.class, Effect: string(entry.effect)}
				agg[key] = row
			}
			row.Seen += stat.Seen
			row.Acted += stat.Acted
			row.Names = append(row.Names, name)
		}
		var rows []weaponRow
		for key, row := range agg {
			switch {
			case row.Seen == 0:
				row.Status = "NOT-OBSERVED"
			case row.Acted > 0:
				row.Status = "PASS"
			default:
				row.Status = "FAIL"
				anyFail = true
			}
			if path, ok := screenshotted[key]; ok {
				row.Screenshot = path
			}
			rows = append(rows, *row)
		}
		// Modern-only heatwave/ground-dust rows for the three sniper classes called out by
		// the user (AWP, autosniper/G3SG1+SCAR-20; the composition covers both bolt and
		// auto snipers under the two hint keys above).
		for _, class := range []string{"awp", "autosniper"} {
			key := class + "|heatwave_grounddust"
			row := weaponRow{Class: class, Effect: "heatwave_grounddust"}
			if profile != "modern" {
				row.Status = "NA"
			} else if heatwaveConfirmed[class] {
				row.Status = "PASS"
			} else {
				row.Status = "NOT-OBSERVED"
			}
			if path, ok := screenshotted[key]; ok {
				row.Screenshot = path
			}
			rows = append(rows, row)
		}
		// Alignment-audit captures (screenshot-only rows; not in fx names table).
		if profile == "modern" {
			for _, effect := range []string{"wisp", "barrelsmoke"} {
				prefix := "|" + effect
				for key, path := range screenshotted {
					if !strings.HasSuffix(key, prefix) || path == "" {
						continue
					}
					class := strings.TrimSuffix(key, prefix)
					row := weaponRow{
						Class:      class,
						Effect:     effect,
						Status:     "CAPTURED",
						Screenshot: path,
					}
					rows = append(rows, row)
				}
			}
		}
		sort.Slice(rows, func(i, j int) bool {
			if rows[i].Class != rows[j].Class {
				return rows[i].Class < rows[j].Class
			}
			return rows[i].Effect < rows[j].Effect
		})

		// Unmapped: weapon-path systems nothing acted on and that this tool doesn't
		// classify -- the live audit trail for missing FXRULE/FXRULE_MODERN entries.
		var unmapped []string
		for name, stat := range names {
			if stat.Acted != 0 {
				continue
			}
			if _, known := knownNames[name]; known {
				continue
			}
			for _, prefix := range weaponPathPrefixes {
				if strings.HasPrefix(name, prefix) {
					unmapped = append(unmapped, name)
					break
				}
			}
		}
		sort.Strings(unmapped)
		os.WriteFile(filepath.Join(outDir, fmt.Sprintf("unmapped-%s.txt", profile)),
			[]byte(strings.Join(unmapped, "\n")), 0o644)

		fmt.Printf("Profile %q results:\n", profile)
		for _, row := range rows {
			fmt.Printf("  [%-12s] %-18s %-20s seen=%-6d acted=%-6d %s\n",
				row.Status, row.Class, row.Effect, row.Seen, row.Acted, row.Screenshot)
		}
		fmt.Printf("Unmapped weapon-path systems this profile: %d (see unmapped-%s.txt)\n", len(unmapped), profile)

		report.Profiles = append(report.Profiles, profileReport{
			Profile: profile, Rows: rows, Unmapped: unmapped,
			FxNamesFile: namesFile, FxRecentFile: recentFile,
		})

		nc.send("mirv_filmmaker fx log off", 300*time.Millisecond)
		if !*skipDemoLoad {
			nc.send("disconnect", 1500*time.Millisecond)
			time.Sleep(4 * time.Second)
		}
	}

	for cat, mode := range savedModes {
		nc.send(fmt.Sprintf("mirv_filmmaker fx set %s %s quiet", cat, mode), 250*time.Millisecond)
	}

	reportPath := filepath.Join(outDir, "report.json")
	if b, err := json.MarshalIndent(report, "", "  "); err == nil {
		os.WriteFile(reportPath, b, 0o644)
	}
	writeSummary(filepath.Join(outDir, "summary.txt"), report)

	fmt.Println("\nArtifacts:", outDir)
	if anyFail {
		fmt.Println("SOME CHECKS FAILED (see summary.txt / report.json)")
		os.Exit(1)
	}
	fmt.Println("ALL PER-WEAPON FX CHECKS PASSED (review screenshots for visual correctness)")
}

func writeSortedNames(path string, names map[string]NameStat) {
	var list []NameStat
	for _, v := range names {
		list = append(list, v)
	}
	sort.Slice(list, func(i, j int) bool { return list[i].Seen > list[j].Seen })
	var sb strings.Builder
	for _, n := range list {
		fmt.Fprintf(&sb, "%8d %6d  %s\n", n.Seen, n.Acted, n.Name)
	}
	os.WriteFile(path, []byte(sb.String()), 0o644)
}

func writeSummary(path string, report fullReport) {
	var sb strings.Builder
	fmt.Fprintf(&sb, "verify-fx-weapons-go summary\ndemo: %s\nstarted: %s\n\n", report.Demo, report.StartedAt)
	for _, p := range report.Profiles {
		fmt.Fprintf(&sb, "=== profile: %s ===\n", p.Profile)
		for _, row := range p.Rows {
			fmt.Fprintf(&sb, "  [%-12s] %-18s %-20s seen=%-6d acted=%-6d %s\n",
				row.Status, row.Class, row.Effect, row.Seen, row.Acted, row.Screenshot)
		}
		fmt.Fprintf(&sb, "  unmapped: %d (see unmapped-%s.txt)\n\n", len(p.Unmapped), p.Profile)
	}
	os.WriteFile(path, []byte(sb.String()), 0o644)
}

func findRepoRoot() (string, error) {
	dir, err := os.Getwd()
	if err != nil {
		return "", err
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "CLAUDE.md")); err == nil {
			return dir, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", fmt.Errorf("CLAUDE.md not found above %s", dir)
		}
		dir = parent
	}
}
