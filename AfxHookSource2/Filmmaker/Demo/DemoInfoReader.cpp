#include "DemoInfoReader.h"

#include "../Platform/ProtobufWire.h"

#include <fstream>
#include <filesystem>
#include <vector>

namespace Filmmaker {

namespace {

// Field numbers inside CMsgGCCStrike15_v2_MatchmakingServerRoundStats.
constexpr uint32_t k_RsReservation = 2;
constexpr uint32_t k_RsKills = 5;
constexpr uint32_t k_RsAssists = 6;
constexpr uint32_t k_RsDeaths = 7;
constexpr uint32_t k_RsScores = 8;
constexpr uint32_t k_RsTeamScores = 12;

// Field number inside CMsgGCCStrike15_v2_MatchmakingGG2GServerReserve.
constexpr uint32_t k_ReservationAccountIds = 1;

// Field numbers inside CDataGCCStrike15_v2_MatchInfo.
constexpr uint32_t k_MatchInfoMatchTime = 2;     // uint32 unix seconds (when the match was played)
constexpr uint32_t k_MatchInfoRoundStatsAll = 5; // repeated; the LAST entry holds final totals

// Collects all repeated varint values for a given field number, plus (optionally)
// the last length-delimited bytes for a sub-message field.
struct RoundStats {
	std::vector<uint32_t> accountIds;
	std::vector<int> kills, assists, deaths, scores, teamScores;
};

std::vector<uint32_t> ReadRepeatedVarint(const uint8_t* data, size_t len, uint32_t wantField) {
	std::vector<uint32_t> out;
	ProtobufReader r(data, len);
	uint32_t field; PbWire wire;
	while (r.ReadTag(field, wire)) {
		if (field == wantField && wire == PbWire::Varint) {
			out.push_back((uint32_t)r.ReadVarint());
		} else {
			r.SkipField(wire);
		}
	}
	return out;
}

RoundStats ParseRoundStats(const uint8_t* data, size_t len) {
	RoundStats rs;
	ProtobufReader r(data, len);
	uint32_t field; PbWire wire;
	while (r.ReadTag(field, wire)) {
		if (field == k_RsReservation && wire == PbWire::LengthDelimited) {
			const uint8_t* p; size_t l;
			if (r.ReadBytes(p, l)) {
				auto ids = ReadRepeatedVarint(p, l, k_ReservationAccountIds);
				rs.accountIds = std::move(ids);
			}
			continue;
		}
		if (wire == PbWire::Varint) {
			const int v = (int)(int64_t)r.ReadVarint();
			switch (field) {
			case k_RsKills:      rs.kills.push_back(v); break;
			case k_RsAssists:    rs.assists.push_back(v); break;
			case k_RsDeaths:     rs.deaths.push_back(v); break;
			case k_RsScores:     rs.scores.push_back(v); break;
			case k_RsTeamScores: rs.teamScores.push_back(v); break;
			default: break;
			}
			continue;
		}
		r.SkipField(wire);
	}
	return rs;
}

int At(const std::vector<int>& v, size_t i) { return i < v.size() ? v[i] : 0; }

} // namespace

DemoInfoResult ReadDemoInfo(const std::wstring& infoPath) {
	DemoInfoResult result;
	if (infoPath.empty())
		return result;

	std::error_code ec;
	if (!std::filesystem::exists(std::filesystem::path(infoPath), ec))
		return result;

	std::ifstream f(std::filesystem::path(infoPath), std::ios::binary);
	if (!f.is_open())
		return result;
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (data.empty())
		return result;

	// Find the last roundstatsall entry (cumulative final totals) and the match
	// time (the wall-clock date the match was actually played).
	const uint8_t* lastRoundPtr = nullptr;
	size_t lastRoundLen = 0;
	int64_t matchTime = 0;
	{
		ProtobufReader r(data.data(), data.size());
		uint32_t field; PbWire wire;
		while (r.ReadTag(field, wire)) {
			if (field == k_MatchInfoMatchTime && wire == PbWire::Varint) {
				matchTime = (int64_t)r.ReadVarint();
				continue;
			}
			if (field == k_MatchInfoRoundStatsAll && wire == PbWire::LengthDelimited) {
				const uint8_t* p; size_t l;
				if (r.ReadBytes(p, l)) {
					lastRoundPtr = p;
					lastRoundLen = l;
				}
				continue;
			}
			r.SkipField(wire);
		}
	}

	result.matchTime = matchTime;

	if (!lastRoundPtr)
		return result;

	RoundStats rs = ParseRoundStats(lastRoundPtr, lastRoundLen);
	if (rs.accountIds.empty())
		return result;

	const size_t n = rs.accountIds.size();
	const size_t halfway = n / 2;
	for (size_t i = 0; i < n; ++i) {
		ScoreboardPlayer p;
		p.accountId = rs.accountIds[i];
		p.kills = At(rs.kills, i);
		p.assists = At(rs.assists, i);
		p.deaths = At(rs.deaths, i);
		p.score = At(rs.scores, i);
		p.teamIndex = (i < halfway) ? 0 : 1;
		result.players.push_back(p);
	}

	if (rs.teamScores.size() >= 2) {
		result.teamScore0 = rs.teamScores[0];
		result.teamScore1 = rs.teamScores[1];
	}

	result.ok = true;
	return result;
}

} // namespace Filmmaker
