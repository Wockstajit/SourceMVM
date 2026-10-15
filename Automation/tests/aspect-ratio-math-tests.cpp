#include "../../AfxHookSource2/Filmmaker/Window/AspectRatioMath.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

void Fail(const char* name, const char* detail) {
	std::cerr << name << ": " << detail << "\n";
	std::exit(1);
}

void ExpectClosest(const char* name, int width, int height, const char* expectedLabel) {
	const Filmmaker::AspectMatch match = Filmmaker::AspectRatio_Match(width, height);
	if (!match.valid || !match.entry)
		Fail(name, "expected a valid match");
	if (0 != std::strcmp(match.entry->label, expectedLabel)) {
		std::cerr << name << ": expected closest " << expectedLabel << ", got "
			<< match.entry->label << " (ratio " << match.ratio << ")\n";
		std::exit(1);
	}
}

void ExpectExact(const char* name, int width, int height, const char* expectedLabel) {
	ExpectClosest(name, width, height, expectedLabel);
	const Filmmaker::AspectMatch match = Filmmaker::AspectRatio_Match(width, height);
	if (!match.exact)
		Fail(name, "expected an exact match");
}

void ExpectInvalid(const char* name, int width, int height) {
	const Filmmaker::AspectMatch match = Filmmaker::AspectRatio_Match(width, height);
	if (match.valid)
		Fail(name, "expected an invalid match");
}

void ExpectReduced(const char* name, int width, int height, int expectedW, int expectedH) {
	const Filmmaker::AspectMatch match = Filmmaker::AspectRatio_Match(width, height);
	if (match.reducedWidth != expectedW || match.reducedHeight != expectedH) {
		std::cerr << name << ": expected reduced " << expectedW << ":" << expectedH << ", got "
			<< match.reducedWidth << ":" << match.reducedHeight << "\n";
		std::exit(1);
	}
}

void ExpectSign(const char* name, int width, int height, bool wider) {
	const Filmmaker::AspectMatch match = Filmmaker::AspectRatio_Match(width, height);
	if (wider != (match.errorPercent > 0.0)) {
		std::cerr << name << ": expected error sign " << (wider ? "+" : "-") << ", got "
			<< match.errorPercent << "\n";
		std::exit(1);
	}
}

// Every table entry must identify itself: feeding a size built from an entry's own ratio has to
// come back as that entry. This is what stops a new row from shadowing an existing one.
void ExpectTableSelfIdentifies() {
	std::size_t count = 0;
	const Filmmaker::AspectRatioEntry* table = Filmmaker::AspectRatio_Table(count);
	if (count == 0)
		Fail("table", "empty");
	for (std::size_t i = 0; i < count; ++i) {
		const int height = 1080;
		const int width = (int)(height * table[i].ratio + 0.5);
		const Filmmaker::AspectMatch match = Filmmaker::AspectRatio_Match(width, height);
		if (match.entry != &table[i]) {
			std::cerr << "table self-identify: " << table[i].label << " resolved to "
				<< (match.entry ? match.entry->label : "<none>") << "\n";
			std::exit(1);
		}
	}
	// No two rows may share a ratio -- a duplicate makes "closest" arbitrary.
	for (std::size_t i = 0; i < count; ++i) {
		for (std::size_t j = i + 1; j < count; ++j) {
			if (std::fabs(table[i].ratio - table[j].ratio) < 1e-6) {
				std::cerr << "duplicate ratio: " << table[i].label << " and " << table[j].label << "\n";
				std::exit(1);
			}
		}
	}
}

} // namespace

int main() {
	ExpectInvalid("zero width", 0, 1080);
	ExpectInvalid("zero height", 1920, 0);
	ExpectInvalid("negative", -1920, -1080);

	// The shapes the user asked for by name.
	ExpectExact("1080p", 1920, 1080, "16:9");
	ExpectExact("1440p", 2560, 1440, "16:9");
	ExpectExact("4:3", 1024, 768, "4:3");
	ExpectExact("5:4", 1280, 1024, "5:4");
	ExpectExact("square", 1000, 1000, "1:1");
	ExpectExact("academy", 1375, 1000, "1.375:1");
	ExpectExact("flat", 1850, 1000, "1.85:1");
	ExpectExact("literal 21:9", 2520, 1080, "21:9");

	// Movie shapes.
	ExpectExact("scope", 2390, 1000, "2.39:1");
	ExpectExact("cinemascope", 2350, 1000, "2.35:1");
	ExpectExact("univisium", 2000, 1000, "2:1");
	ExpectExact("ultra panavision", 2760, 1000, "2.76:1");
	ExpectExact("imax film", 1430, 1000, "1.43:1");
	ExpectExact("imax digital", 1900, 1000, "1.90:1");
	ExpectExact("70mm", 2200, 1000, "2.20:1");
	// "1.66:1" is the rounded trade name for 5:3, and the table stores the exact ratio.
	ExpectExact("european", 5000, 3000, "1.66:1");
	ExpectClosest("rounded 1.66", 1660, 1000, "1.66:1");

	// Social shapes.
	ExpectExact("vertical", 1080, 1920, "9:16");
	ExpectExact("portrait", 1080, 1350, "4:5");

	// Real ultrawide panels are NOT 21/9; the table has to say so rather than flatter the label.
	ExpectClosest("2560x1080 ultrawide", 2560, 1080, "2.39:1");
	ExpectClosest("3440x1440 ultrawide", 3440, 1440, "2.39:1");

	// A window mid-drag: a few pixels off 16:9 still reads as 16:9, with a signed error.
	ExpectClosest("near 16:9", 2544, 1431, "16:9");
	ExpectSign("slightly wide", 1930, 1080, true);
	ExpectSign("slightly tall", 1910, 1080, false);

	// Reduced integer ratio: shown when small, suppressed when it would be noise.
	ExpectReduced("1080p reduced", 1920, 1080, 16, 9);
	ExpectReduced("7:5 reduced", 1400, 1000, 7, 5);
	ExpectReduced("prime-ish suppressed", 1919, 1081, 0, 0);

	ExpectTableSelfIdentifies();

	// Dimension helpers always return an even number (encoders reject odd yuv420p sizes).
	if (Filmmaker::AspectRatio_HeightForWidth(1920, 2.39) % 2 != 0)
		Fail("height helper", "expected an even height");
	if (Filmmaker::AspectRatio_WidthForHeight(1080, 2.39) % 2 != 0)
		Fail("width helper", "expected an even width");
	if (Filmmaker::AspectRatio_HeightForWidth(0, 1.78) != 0)
		Fail("height helper", "expected 0 for an invalid width");
	if (Filmmaker::AspectRatio_HeightForWidth(1920, 0.0) != 0)
		Fail("height helper", "expected 0 for an invalid ratio");

	// Nearest even, not round-then-bump: 3008 at 2.39:1 is 1258.58, so 1258 (0.58 away) beats
	// 1260 (1.42 away). Getting this wrong makes every grid-snapped scope size slightly too tall.
	if (Filmmaker::AspectRatio_HeightForWidth(3008, 2.39) != 1258)
		Fail("height helper", "expected 1258 -- the nearest even height, not the next one up");
	if (Filmmaker::AspectRatio_HeightForWidth(1920, 2.39) != 804)
		Fail("height helper", "expected 804 (the standard 2.39:1 HD height)");

	// The exact whole step: what makes a snapped size a real resolution rather than a near miss.
	{
		int stepWidth = 0, stepHeight = 0;
		if (!Filmmaker::AspectRatio_ExactStep(16.0 / 9.0, stepWidth, stepHeight)
			|| stepWidth != 32 || stepHeight != 18)
			Fail("exact step", "16:9 should step 32x18 (16:9 doubled so both axes are even)");
		if (!Filmmaker::AspectRatio_ExactStep(4.0 / 3.0, stepWidth, stepHeight)
			|| stepWidth != 8 || stepHeight != 6)
			Fail("exact step", "4:3 should step 8x6");
		if (!Filmmaker::AspectRatio_ExactStep(1.85, stepWidth, stepHeight)
			|| stepWidth != 74 || stepHeight != 40)
			Fail("exact step", "1.85:1 is 37:20 and should step 74x40");
		if (!Filmmaker::AspectRatio_ExactStep(1.0, stepWidth, stepHeight)
			|| stepWidth != 2 || stepHeight != 2)
			Fail("exact step", "1:1 should step 2x2");
		// 2.39 is 239/100: no whole pair small enough to be a usable step, and claiming one would
		// quantise every scope drag onto the wrong shape.
		if (Filmmaker::AspectRatio_ExactStep(2.39, stepWidth, stepHeight))
			Fail("exact step", "2.39:1 has no small whole step");
		if (Filmmaker::AspectRatio_ExactStep(1.43, stepWidth, stepHeight))
			Fail("exact step", "1.43:1 has no small whole step");
		if (Filmmaker::AspectRatio_ExactStep(0.0, stepWidth, stepHeight))
			Fail("exact step", "expected false for an invalid ratio");

		// Every step the table can produce must reproduce its ratio exactly, and every table ratio
		// that HAS a step must round-trip through it.
		std::size_t count = 0;
		const Filmmaker::AspectRatioEntry* table = Filmmaker::AspectRatio_Table(count);
		for (std::size_t i = 0; i < count; ++i) {
			if (!Filmmaker::AspectRatio_ExactStep(table[i].ratio, stepWidth, stepHeight))
				continue;
			if (stepWidth % 2 || stepHeight % 2)
				Fail(table[i].label, "step axes must both be even");
			const double stepped = (double)(stepWidth * 7) / (double)(stepHeight * 7);
			if (std::fabs(stepped - table[i].ratio) > 1e-12)
				Fail(table[i].label, "a whole multiple of the step must be exactly the ratio");
		}
	}

	return 0;
}
