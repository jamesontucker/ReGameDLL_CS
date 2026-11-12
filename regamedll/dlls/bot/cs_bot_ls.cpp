/*
*
*   Loner-Strike additions to the CZ bots: each bot keeps to itself and moves like a person.
*   Kept in this file (plus small hooks marked "Loner-Strike") so upstream merges stay easy.
*
*   bot_ls_own_memory  1  each bot remembers danger (where it died, where it saw a teammate
*                         die) for itself, instead of the whole team sharing one map of it
*   bot_ls_routes    1.0  each bot has favourite parts of the map (the same ones every time
*                         it plays, from its name) that drift a little over a match, so bots
*                         take different routes; 0 is off, higher is stronger
*   bot_ls_lines     1.0  each bot cuts corners on its own line (some hug the left, some
*                         the right, some the middle) instead of all on the same one
*   bot_ls_pauses    1.0  now and then a bot stops for a moment while walking somewhere
*                         quiet, like someone checking the map or the chat
*   bot_ls_solo        1  every bot for itself, like a real pub server: it heads somewhere
*                         its teammates aren't, leaves a fight a closer teammate is
*                         already going to, and breaks off when it finds itself walking
*                         with one. In deathmatch-type modes (bot_deathmatch 1) that's
*                         every bot, and nobody follows anyone on the radio; elsewhere
*                         it's the bots with little teamwork in their profile (a team
*                         style's bots, and the team-minded ones, still play together)
*
*   bot_ls_fight       1  fights like people: never frozen while reloading in sight of an
*                         enemy, always moving when hit, backing off only with room
*                         behind and only some bots, each with its own strafe rhythm, a
*                         stop before a long-range shot with a rifle (newer players still
*                         run and gun), crouching only for the bots that crouch, and
*                         staying in a fight it's winning
*
*   bot_ls_match       0  an organised match is on (5v5 match mode sets it): disciplined
*                         habits (sneaking, careful peeks) more, careless ones less
*
*   The habits follow the game: deathmatch-type modes (bot_deathmatch 1) run and gun with
*   no sneaking; a pub game has the full mix; a match is disciplined.
*
*   bot_ls_trace       0  testing only: twice a second, each bot's state, task, goal and
*                         why it's going there, to lstrace.csv in the server's folder
*
*   All of them at 0 is the stock behaviour.
*
*/

#include "precompiled.h"
#include <vector>
#include <algorithm>
#include <utility>
#include <queue>
#include <functional>

int g_lsNavLoads = 0;	// navs loaded so far (nav_file.cpp counts them)
#include <cmath>
#include <cstdio>
#ifdef _WIN32
#include <chrono>
#else
#include <sys/time.h>
#endif

cvar_t cv_bot_ls_own_memory = { "bot_ls_own_memory", "1", FCVAR_SERVER, 0.0f, nullptr };
cvar_t cv_bot_ls_routes     = { "bot_ls_routes", "1.0", FCVAR_SERVER, 0.0f, nullptr };
cvar_t cv_bot_ls_lines      = { "bot_ls_lines", "1.0", FCVAR_SERVER, 0.0f, nullptr };
cvar_t cv_bot_ls_pauses     = { "bot_ls_pauses", "1.0", FCVAR_SERVER, 0.0f, nullptr };
cvar_t cv_bot_ls_solo       = { "bot_ls_solo", "1", FCVAR_SERVER, 0.0f, nullptr };
cvar_t cv_bot_ls_fight      = { "bot_ls_fight", "1", FCVAR_SERVER, 0.0f, nullptr };
cvar_t cv_bot_ls_match      = { "bot_ls_match", "0", FCVAR_SERVER, 0.0f, nullptr };
cvar_t cv_bot_ls_trace      = { "bot_ls_trace", "0", FCVAR_SERVER, 0.0f, nullptr };
// (each bomb site's real letter from the nav's place names, for plugins: "a x y b x y")
cvar_t cv_ls_sites          = { "ls_sites", "", 0, 0.0f, nullptr };
const char LS_PEEK[] = "Loner-Strike: peek";

extern cvar_t cv_bot_ls_surf;	// bot/cs_bot_surf.cpp

void Bot_LsRegisterCVars()
{
	CVAR_REGISTER(&cv_bot_ls_surf);
	CVAR_REGISTER(&cv_bot_ls_own_memory);
	CVAR_REGISTER(&cv_bot_ls_routes);
	CVAR_REGISTER(&cv_bot_ls_lines);
	CVAR_REGISTER(&cv_bot_ls_pauses);
	CVAR_REGISTER(&cv_bot_ls_solo);
	CVAR_REGISTER(&cv_bot_ls_fight);
	CVAR_REGISTER(&cv_bot_ls_match);
	CVAR_REGISTER(&cv_bot_ls_trace);
	CVAR_REGISTER(&cv_ls_sites);
}

// The bomb sites' letters as the nav names them (BombsiteA / BombsiteB), published once per
// nav for the plugins: on 1.6 there's no radar place message, so the chat plugin couldn't
// learn which site is which and fell back to the map's entity order - backwards on dust2
// ("ok B", then planting A)
void LsPublishSites()
{
	static int s_doneFor = -1;
	// (no nav on this map: nothing to name - the last map's letters don't carry over)
	if (TheNavAreaList.empty())
	{
		if (cv_ls_sites.string && cv_ls_sites.string[0])
			CVAR_SET_STRING("ls_sites", "");
		return;
	}
	if (s_doneFor == g_lsNavLoads || !TheCSBots())
		return;
	s_doneFor = g_lsNavLoads;
	char out[128] = "";
	for (int z = 0; z < TheCSBots()->GetZoneCount() && z < 4; z++)
	{
		const CCSBotManager::Zone *zone = TheCSBots()->GetZone(z);
		if (!zone)
			continue;
		// (the letter most of the site's areas are named with)
		int votes[3] = {};
		for (auto area : TheNavAreaList)
		{
			if ((*area->GetCenter() - zone->m_center).Make2D().IsLengthGreaterThan(450.0f))
				continue;
			const char *name = TheNavAreaGrid.IDToName(area->GetPlace());
			if (!name || Q_strnicmp(name, "Bombsite", 8) || !name[8] || name[9])
				continue;
			int c = (name[8] | 0x20) - 'a';
			if (c >= 0 && c < 3)
				votes[c]++;
		}
		int best = -1;
		for (int c = 0; c < 3; c++)
			if (votes[c] > 0 && (best < 0 || votes[c] > votes[best]))
				best = c;
		if (best < 0)
			continue;
		char one[40];
		Q_snprintf(one, sizeof(one), "%s%c %.0f %.0f", out[0] ? " " : "", 'a' + best, zone->m_center.x, zone->m_center.y);
		Q_strlcat(out, one);
	}
	CVAR_SET_STRING("ls_sites", out);
}

// An organised match (bot_ls_match, set by match mode)
bool LsMatch()
{
	return cv_bot_ls_match.value > 0.0f;
}

// Loner-Strike (bot_ls_fight): no camping on top of each other. A hiding spot is taken when a
// living teammate is close to it, or a teammate bot is hiding at or on its way to a spot close
// to it: people spread out to hold a place, they don't stand shoulder to shoulder
bool LsSpotCrowded(CBaseEntity *me, const Vector *pos)
{
	if (!LsFight() || !me || !me->IsPlayer())
		return false;
	// (a crawlspace - vents, a low tunnel - is a way through, not a place to hold: out, but for
	// the odd newer player who doesn't know better)
	{
		Vector p = *pos;
		CNavArea *area = TheNavAreaGrid.GetNavArea(&p);
		const LsAreaKnow *k = area ? LsKnow(area) : nullptr;
		if ((k && k->ceiling * 4 < 70) || (area && (area->GetAttributes() & NAV_CROUCH)))
		{
			CCSBot *bot = static_cast<CBasePlayer *>(me)->IsBot() ? static_cast<CCSBot *>(me) : nullptr;
			if (!bot || !(bot->LsSkill() < 0.25f && bot->LsTrait(76) < 0.1f))
				return true;
		}
	}
	// (free-for-all: nobody's a teammate)
	if (CSGameRules() && CSGameRules()->IsFreeForAll())
		return false;
	// (a disciplined team spreads wider: different angles onto the same way in, a crossfire)
	const float apart = LsMatch() ? 320.0f : 220.0f;
	int team = static_cast<CBasePlayer *>(me)->m_iTeam;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *mate = UTIL_PlayerByIndex(i);
		if (!mate || mate == me || !mate->IsAlive() || mate->m_iTeam != team)
			continue;
		if ((mate->pev->origin - *pos).IsLengthLessThan(apart))
			return true;
		if (mate->IsBot())
		{
			const Vector *goal = static_cast<CCSBot *>(mate)->LsHideGoal();
			if (goal && (*goal - *pos).IsLengthLessThan(apart))
				return true;
		}
	}
	return false;
}

bool LsFight()
{
	return cv_bot_ls_fight.value > 0.0f;
}

// How much who a bot is shapes how it plays: the app's personality slider (gm_personality,
// 0-200): at 0 the bot style alone, at 100 mostly the person, at 200 nearly all of it
static float LsPersonWeight()
{
	static cvar_t *s_pers = nullptr;
	if (!s_pers)
		s_pers = CVAR_GET_POINTER("gm_personality");
	float p = s_pers ? s_pers->value / 100.0f : 1.0f;
	return Q_max(0.0f, Q_min(0.9f, 0.6f * p + (p > 1.0f ? -0.3f * (p - 1.0f) : 0.0f)));
}

// A mode with teams and objectives (not a deathmatch-type one, whatever bot_ls_solo says)
bool CCSBot::LsTeamGame() const
{
	return TheCSBots() && TheCSBots()->GetScenario() != CCSBotManager::SCENARIO_DEATHMATCH;
}

// Free to head somewhere for team play: not in a fight, not on a job that mustn't be left
// (the bomb, hostages, guarding the plant), not saving its gun, not under a plugin's control
bool CCSBot::LsFree() const
{
	return !IsAttacking() && !IsBusy() && !IsCarryingBomb() && !m_lsSaving && !LsPuppetActive() && !LsOrderActive() && !IsDefusingBomb() && !m_bIsVIP
		&& GetTask() != VIP_ESCAPE && GetTask() != GUARD_VIP_ESCAPE_ZONE
		&& !IsEscapingFromBomb() && GetHostageEscortCount() == 0 && !m_isWaitingToTossGrenade
		&& GetTask() != GUARD_TICKING_BOMB && !IsUsingLadder() && m_state != &m_plantBombState && m_state != &m_defuseBombState;
}

int CCSBot::LsStyle() const
{
	return LsPersonWeight() > 0.0f ? m_lsStyle : -1;
}

// How aggressive, how much of a team player and how good this bot is: the bot style's
// profile (its range: a Newbie lobby's, Gods', Matchmaking's teamwork), pulled toward who
// the bot is (its playstyle, temper, skill...): about half way at the personality slider's
// 100, so a lurker in Gods lurks too, and a style still shows. A team style (profile teamwork of 45 and up) never drops
// below that. Stock bots (bot_ls_fight 0) and bots nobody described: the profile alone
float CCSBot::LsAggro() const
{
	float profile = GetProfile()->GetAggression();
	if (!LsFight() || m_lsAggro < 0.0f)
		return profile;
	float pull = 0.8f * LsPersonWeight();
	return Q_max(0.0f, Q_min(1.0f, profile + pull * (m_lsAggro - profile)));
}

float CCSBot::LsTeamwork() const
{
	float profile = GetProfile()->GetTeamwork();
	if (!LsFight() || m_lsTeam < 0.0f)
		return profile;
	float pull = 0.8f * LsPersonWeight();
	float v = Q_max(0.0f, Q_min(1.0f, profile + pull * (m_lsTeam - profile)));
	// (a team style, or a match: a team player whoever it is)
	return (profile >= 0.45f || LsMatch()) ? Q_max(0.45f, v) : v;
}

float CCSBot::LsSkill() const
{
	float profile = GetProfile()->GetSkill();
	if (!LsFight() || m_lsSkillTrait < 0)
		return profile;
	return Q_max(0.0f, Q_min(1.0f, profile + 0.5f * LsPersonWeight() * (float(m_lsSkillTrait) / 3.0f - 0.5f)));
}

// Every bot for itself (deathmatch-type modes, bot_ls_solo)
bool LsSolo()
{
	// (the mode says so: bot_deathmatch or free-for-all; a map with no bomb or hostages but
	// rounds and teams, an aim_ or fy_ map, isn't deathmatch)
	if (cv_bot_ls_solo.value <= 0.0f || !TheCSBots() || TheCSBots()->GetScenario() != CCSBotManager::SCENARIO_DEATHMATCH)
		return false;
	static cvar_t *s_dm = nullptr;
	if (!s_dm)
		s_dm = CVAR_GET_POINTER("bot_deathmatch");
	return (s_dm && s_dm->value > 0.0f) || (CSGameRules() && CSGameRules()->IsFreeForAll());
}

// This bot goes its own way: everyone in deathmatch, and on objective maps a bot with
// little teamwork in its profile
bool CCSBot::LsLoner() const
{
	if (cv_bot_ls_solo.value <= 0.0f)
		return false;
	// (the bomb carrier plays the objective, whoever it is)
	if (!LsSolo() && IsCarryingBomb())
		return false;
	if (LsSolo())
		return true;
	// lurkers and ninjas go their own way, objective players stay with it
	if (LsStyle() == LS_STYLE_LURKER || LsStyle() == LS_STYLE_NINJA)
		return true;
	if (LsStyle() == LS_STYLE_OBJECTIVE)
		return false;
	return GetProfile() && LsTeamwork() < 0.35f;
}

// ---- a bot's own numbers ----------------------------------------------------------------

static unsigned int LsMix(unsigned int x)
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

static unsigned int LsHash(unsigned int a, unsigned int b, unsigned int c = 0, unsigned int d = 0)
{
	return LsMix(a ^ LsMix(b ^ LsMix(c ^ LsMix(d + 0x9e3779b9U))));
}

// 0..1
static float LsUnit(unsigned int h)
{
	return (h & 0xffffff) / float(0xffffff);
}

// From the bot's name, so a regular (the same name every time it shows up) keeps the same
// habits; worked out again when the name changes
// A name without its clan tag: a bracketed tag at the start or end ([TAG], (TAG), {TAG},
// <TAG>, |TAG|, -=TAG=-) is dropped, so "[FX]Dave" and "Dave" are the same person
static void LsBareName(const char *name, char *out, size_t size)
{
	static const char *const opens = "[({<|", *const closes = "])}>|";
	const char *s = name ? name : "";
	size_t len = Q_strlen(s);
	size_t a = 0, b = len;
	for (int pass = 0; pass < 2; pass++)
	{
		// leading
		while (a < b && s[a] == ' ')
			a++;
		const char *o = a < b ? Q_strchr(opens, s[a]) : nullptr;
		if (o && *o)
		{
			char c = closes[o - opens];
			for (size_t k = a + 1; k < b; k++)
				if (s[k] == c) { a = k + 1; break; }
		}
		else if (b - a > 4 && s[a] == '-' && s[a + 1] == '=')
		{
			for (size_t k = a + 2; k + 1 < b; k++)
				if (s[k] == '=' && s[k + 1] == '-') { a = k + 2; break; }
		}
		// trailing
		while (b > a && s[b - 1] == ' ')
			b--;
		const char *c2 = b > a ? Q_strchr(closes, s[b - 1]) : nullptr;
		if (c2 && *c2)
		{
			char op = opens[c2 - closes];
			for (size_t k = b - 1; k-- > a;)
				if (s[k] == op) { b = k; break; }
		}
	}
	if (b <= a)
	{
		a = 0;
		b = len;
	}
	size_t n = Q_min(b - a, size - 1);
	Q_memcpy(out, s + a, n);
	out[n] = 0;
}

unsigned int CCSBot::LsSeed()
{
	if (!(pev->netname == m_lsSeedName))
	{
		m_lsSeedName = pev->netname;
		char bare[64];
		const char *name = STRING(pev->netname);
		LsBareName(name, bare, sizeof(bare));
		// (a regular whose name came out a little different this time, "Dave_" or "dave": gm_people
		// sends the name it really is with the persona, and that's what it's seeded from)
		if (m_lsSeedAs[0])
			LsBareName(m_lsSeedAs, bare, sizeof(bare));
		unsigned int h = 2166136261U;
		for (const char *c = bare; *c; c++)
			h = (h ^ (unsigned char)*c) * 16777619U;
		// (a second bot with the same name, "(1)Dave": a person of its own, not Dave's twin)
		if (name[0] == '(' && isdigit((unsigned char)name[1]))
		{
			for (const char *c = name; *c && *c != ')'; c++)
				h = (h ^ (unsigned char)*c) * 16777619U;
		}
		m_lsSeed = LsMix(h);
	}
	return m_lsSeed;
}

// A per-bot trait between 0 and 1 (which = what it's for)
float CCSBot::LsTrait(unsigned int which)
{
	return LsUnit(LsHash(LsSeed(), which, 0x51ab));
}

// ---- own memory of danger ---------------------------------------------------------------

// Areas are kept by id, never by pointer, and forgotten on a new map. A fixed table, not
// a container: the game frees a bot without running its destructor
void CCSBot::LsCheckMap()
{
	// (a path search asks for every area it opens: the name is compared once a frame; a new
	// map restarts the clock, so a time that went backwards checks too)
	if (gpGlobals->time == m_lsMapCheckedAt)
		return;
	m_lsMapCheckedAt = gpGlobals->time;
	if (Q_strcmp(m_lsMap, STRING(gpGlobals->mapname)) != 0)
	{
		Q_strlcpy(m_lsMap, STRING(gpGlobals->mapname));
		for (auto &d : m_lsDanger)
			d.amount = 0.0f;
		m_lsDangerUntil = -1.0f;
	}
}

// What's left of a remembered danger now (forgotten in two minutes per kill, like the team's)
static float LsDangerNow(const CCSBot::LsDanger &d)
{
	const float decayRate = 1.0f / 120.0f;
	return (d.amount > 0.0f) ? Q_max(0.0f, d.amount - decayRate * (gpGlobals->time - d.time)) : 0.0f;
}

float CCSBot::LsGetDanger(const CNavArea *area)
{
	LsCheckMap();
	if (gpGlobals->time > m_lsDangerUntil)
		return 0.0f;
	for (const auto &d : m_lsDanger)
	{
		if (d.amount > 0.0f && d.id == area->GetID())
			return LsDangerNow(d);
	}
	return 0.0f;
}

void CCSBot::LsAddDangerToArea(const CNavArea *area, float amount)
{
	LsCheckMap();
	// its slot, else a free (or forgotten) one, else the faintest memory
	LsDanger *slot = nullptr, *faintest = nullptr;
	float faint = 1e9f;
	for (auto &d : m_lsDanger)
	{
		float now = LsDangerNow(d);
		if (now > 0.0f && d.id == area->GetID())
		{
			slot = &d;
			break;
		}
		if (now < faint)
		{
			faint = now;
			faintest = &d;
		}
	}
	if (!slot)
	{
		slot = faintest;
		slot->id = area->GetID();
		slot->amount = 0.0f;
		slot->time = gpGlobals->time;
	}
	slot->amount = LsDangerNow(*slot) + amount;
	slot->time = gpGlobals->time;
	// (it fades at 1/120 a second)
	m_lsDangerUntil = Q_max(m_lsDangerUntil, slot->time + 120.0f * slot->amount);
}

// As IncreaseDangerNearby, into this bot's own memory
void CCSBot::LsAddDangerNearby(float amount, CNavArea *startArea, const Vector *pos, float maxRadius)
{
	if (!startArea)
		return;

	CNavArea::MakeNewMarker();
	CNavArea::ClearSearchLists();

	startArea->AddToOpenList();
	startArea->SetTotalCost(0.0f);
	startArea->Mark();
	LsAddDangerToArea(startArea, amount);

	while (!CNavArea::IsOpenListEmpty())
	{
		CNavArea *area = CNavArea::PopOpenList();
		for (int dir = 0; dir < NUM_DIRECTIONS; dir++)
		{
			int count = area->GetAdjacentCount((NavDirType)dir);
			for (int i = 0; i < count; i++)
			{
				CNavArea *adjArea = area->GetAdjacentArea((NavDirType)dir, i);
				if (adjArea->IsMarked())
					continue;

				float cost = (*adjArea->GetCenter() - *pos).Length();
				if (cost <= maxRadius)
				{
					adjArea->AddToOpenList();
					adjArea->SetTotalCost(cost);
					adjArea->Mark();
					LsAddDangerToArea(adjArea, amount * cost / maxRadius);
				}
			}
		}
	}
}

// Rounds so far, counting on through restarts (sv_restart at a scrim's halves, Game
// Commencing): the game's own count goes back to 0, and every once-a-round marker keyed to it
// would think round 0 after a restart was already done
int LsRoundSerial()
{
	static int s_serial = 0;
	static float s_start = -1.0f;
	if (TheCSBots() && TheCSBots()->GetRoundStartTime() != s_start)
	{
		s_start = TheCSBots()->GetRoundStartTime();
		s_serial++;
	}
	return s_serial;
}

// The Ts' site for this round: one for the whole team (most head there, the carrier too)
static int s_tSiteRound = -1;
static int s_tSiteZone = 0;
static int s_tSiteDeaths = 0;
static float s_tSiteLastDeath = -1.0f;

const CCSBotManager::Zone *LsTSite()
{
	if (!TheCSBots() || TheCSBots()->GetZoneCount() <= 0)
		return nullptr;
	if (s_tSiteRound != LsRoundSerial())
	{
		s_tSiteRound = LsRoundSerial();
		s_tSiteZone = RANDOM_LONG(0, TheCSBots()->GetZoneCount() - 1);
		s_tSiteDeaths = 0;
	}
	return TheCSBots()->GetZone(Q_min(s_tSiteZone, TheCSBots()->GetZoneCount() - 1));
}

// A person called the site for the team ("rush b", "go a"): the team's site this round, kept
// (two deaths there don't switch it); a rush goes in at once, without gathering first
static int s_callRound = -1;
static bool s_callRush = false;

void LsTeamCall(const CCSBotManager::Zone *zone, bool rush)
{
	if (!TheCSBots() || !zone)
		return;
	for (int z = 0; z < TheCSBots()->GetZoneCount(); z++)
		if (TheCSBots()->GetZone(z) == zone)
		{
			LsTSite();
			s_tSiteZone = z;
			s_tSiteDeaths = 0;
			// (a second call this round: "rush" stays a rush once said)
			s_callRush = (s_callRound == LsRoundSerial() && s_callRush) || rush;
			s_callRound = LsRoundSerial();
			return;
		}
}

void LsTeamCallOff()
{
	if (s_callRound == LsRoundSerial())
		s_callRush = false;
}

bool LsTeamCalled(bool *rush)
{
	bool on = s_callRound == LsRoundSerial();
	if (rush)
		*rush = on && s_callRush;
	return on;
}

// A T died near the round's site early on: two of those and the team reads it as the CTs
// stacking that site, and goes for the other one ("they're all at b, go a")
void LsTSiteDeath(CBasePlayer *pVictim)
{
	const CCSBotManager::Zone *site = LsTSite();
	if (!site || pVictim->m_iTeam != TERRORIST || TheCSBots()->IsBombPlanted() || TheCSBots()->GetZoneCount() < 2
		|| TheCSBots()->GetElapsedRoundTime() > 60.0f || gpGlobals->time == s_tSiteLastDeath)
		return;
	s_tSiteLastDeath = gpGlobals->time;
	// (a person's call stands: they decide when to switch)
	if (s_callRound == LsRoundSerial())
		return;
	if ((pVictim->pev->origin - site->m_center).IsLengthGreaterThan(1300.0f))
		return;
	if (++s_tSiteDeaths >= 2)
	{
		s_tSiteZone = (s_tSiteZone + 1) % TheCSBots()->GetZoneCount();
		s_tSiteDeaths = 0;
	}
}

// The round's time left; with rounds that never run out, plenty (rather than below zero,
// which would read as the clock nearly gone)
float LsTimeLeft()
{
	if (!CSGameRules())
		return 999.0f;
	if (HasRoundInfinite(SCENARIO_BLOCK_TIME_EXPRIRED))
		return 999.0f;
	// (mp_roundtime 0: no limit at all; freeze time's own count aside)
	if (CSGameRules()->m_iRoundTime <= 0 && !CSGameRules()->IsFreezePeriod())
		return 999.0f;
	return CSGameRules()->GetRoundRemainingTime();
}

bool LsOwnMemory()
{
	return cv_bot_ls_own_memory.value > 0.0f;
}

// A teammate died where this bot could see it
void CCSBot::LsSawTeammateDie(CBasePlayer *pVictim)
{
	if (!pVictim)
		return;

	// the bomb carrier: every T sees where the bomb dropped on the radar, and that someone's
	// watching that spot (so a fetcher doesn't walk into the same line of fire)
	CBaseEntity *pBomb = TheCSBots() ? TheCSBots()->GetLooseBomb() : nullptr;
	if (LsFight() && LsOwnMemory() && m_iTeam == TERRORIST && pVictim->m_iTeam == TERRORIST
		&& (pVictim->m_bHasC4 || (pBomb && (pBomb->pev->origin - pVictim->pev->origin).IsLengthLessThan(150.0f)))
		&& !IsVisible(&pVictim->pev->origin, CHECK_FOV))
	{
		LsAddDangerNearby(0.5f, TheNavAreaGrid.GetNearestNavArea(&pVictim->pev->origin), &pVictim->pev->origin, 400.0f);
		return;
	}

	// see it happen, not only hear about it (a little less than dying there yourself)
	if (!IsVisible(&pVictim->pev->origin, CHECK_FOV))
		return;

	// whatever a plugin had it doing (a look, a nod, standing still) ends
	if (m_lsPuppet.any)
		LsPuppetRelease();

	if (!LsOwnMemory())
		return;

	CNavArea *area = TheNavAreaGrid.GetNearestNavArea(&pVictim->pev->origin);
	LsAddDangerNearby(0.7f, area, &pVictim->pev->origin, 500.0f);
}

// ---- favourite routes -------------------------------------------------------------------

// Smooth noise over the map: whole stretches of it are liked or disliked, not single areas
// (single areas average out over a long path and every route would end up the same)
static float LsNoise(unsigned int seed, float x, float y, float cell)
{
	float fx = x / cell, fy = y / cell;
	int ix = (int)floorf(fx), iy = (int)floorf(fy);
	float tx = fx - ix, ty = fy - iy;
	tx = tx * tx * (3.0f - 2.0f * tx);
	ty = ty * ty * (3.0f - 2.0f * ty);

	float a = LsUnit(LsHash(seed, ix, iy));
	float b = LsUnit(LsHash(seed, ix + 1, iy));
	float c = LsUnit(LsHash(seed, ix, iy + 1));
	float d = LsUnit(LsHash(seed, ix + 1, iy + 1));
	return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

// This bot's taste for one path search
bool CCSBot::LsRouteTaste(RouteType route, LsRoute *out)
{
	float strength = cv_bot_ls_routes.value;
	if (strength <= 0.0f)
		return false;

	// mostly its own lasting taste, with a part that changes each round (every couple of
	// minutes where there are no rounds), so the plan isn't the same every round
	const float drift = 120.0f;
	unsigned int bucket = LsSolo() ? (unsigned int)(gpGlobals->time / drift) : (unsigned int)LsRoundSerial() * 7919u + 13u;
	out->seed = LsSeed();
	out->driftSeed = LsHash(out->seed, bucket);

	// a hurry (the fastest route) leaves less room for taste
	out->most = Q_min(strength, 3.0f) * ((route == FASTEST_ROUTE) ? 0.25f : 0.6f);
	return true;
}

// How much more this bot minds walking through an area (1 = not at all)
float LsRouteFactor(const LsRoute &r, const CNavArea *area)
{
	const Vector *c = area->GetCenter();
	float taste = 0.6f * LsNoise(r.seed, c->x, c->y, 700.0f) + 0.4f * LsNoise(r.driftSeed, c->x, c->y, 500.0f);
	return 1.0f + r.most * taste;
}

// ---- own line around corners ------------------------------------------------------------

// Moves a path point along the opening between two areas, to where this bot likes to walk
// through. The point stays inside the opening (a player's width from its ends), so the
// straight line between two points still crosses only the area between them.
void CCSBot::LsShiftPortalPoint(const CNavArea *from, const CNavArea *to, NavDirType dir, Vector *pos)
{
	float strength = cv_bot_ls_lines.value;
	if (strength <= 0.0f)
		return;

	Vector center;
	float halfWidth;
	from->ComputePortal(to, dir, &center, &halfWidth);

	const float margin = 24.0f;
	float room = halfWidth - margin;
	if (room <= 4.0f)
		return;

	// 1 keeps to its left, -1 to its right (of the way it's going); mostly the bot's own
	// habit, a little different each path
	float habit = 2.0f * LsTrait(1) - 1.0f;
	float jitter = RANDOM_FLOAT(-0.35f, 0.35f);
	float side = Q_max(-1.0f, Q_min(1.0f, habit + jitter));
	// (one who knows to stay out of sight leans to the side the dangerous angles can't see:
	// the left wall of a catwalk away from the doors; in deathmatch nobody bothers)
	if (LsFight() && !LsSolo() && LsKnows(LSK_EXPOSURE))
	{
		float safe = LsSafeSide(to, dir);
		float lean = 0.5f + 0.4f * LsSkill();
		side = Q_max(-1.0f, Q_min(1.0f, side * (1.0f - lean) + safe * lean));
	}

	// its left going north (-y) is +x, south -x, east +y, west -y
	bool alongX = (dir == NORTH || dir == SOUTH);
	float leftward = (dir == NORTH || dir == EAST) ? 1.0f : -1.0f;
	float want = (alongX ? center.x : center.y) + side * leftward * room;
	float &at = alongX ? pos->x : pos->y;

	float blend = Q_min(1.0f, 0.45f * strength);
	float moved = at + (want - at) * blend;

	// stay inside the opening
	float lo = (alongX ? center.x : center.y) - room;
	float hi = (alongX ? center.x : center.y) + room;
	at = Q_max(lo, Q_min(hi, moved));
}

// ---- pauses -----------------------------------------------------------------------------

// True while the bot is standing still for a moment on its way somewhere quiet
bool CCSBot::LsUpdatePause()
{
	// (the personality slider scales them: at 0 nobody stops; deathmatch keeps few)
	float strength = LsFight() ? cv_bot_ls_pauses.value * (LsPersonWeight() / 0.6f) * (LsSolo() ? 0.3f : 1.0f) : 0.0f;
	if (strength <= 0.0f)
		return false;

	if (gpGlobals->time < m_lsPauseUntil)
	{
		// anything happening ends it
		if (IsAttacking() || GetTimeSinceLastSawEnemy() < 3.0f || IsHurrying() || IsOnLadder() || GetTimeSinceAttacked() < 3.0f || IsSurprised()
			|| TheCSBots()->IsBombPlanted() || GetHostageEscortCount() > 0)
		{
			m_lsPauseUntil = 0.0f;
			return false;
		}
		ResetStuckMonitor();
		return true;
	}

	// decide about once a second
	if (gpGlobals->time < m_lsNextPauseCheck)
		return false;
	m_lsNextPauseCheck = gpGlobals->time + RANDOM_FLOAT(0.8f, 1.2f);

	// only when nothing is going on (and not stuck: getting free comes first)
	if (IsAttacking() || GetTimeSinceLastSawEnemy() < 8.0f || IsOnLadder() || IsCarryingBomb() || LsBusy() || m_isStuck)
		return false;

	// the bomb is down, or the clock nearly out: no time for that
	if (TheCSBots()->IsBombPlanted() || (!LsSolo() && CSGameRules() && LsTimeLeft() < 30.0f))
		return false;

	// never in the air, or where it has to jump or crouch through (it would stop short)
	if (!(pev->flags & FL_ONGROUND) || !m_lastKnownArea || (m_lastKnownArea->GetAttributes() & (NAV_JUMP | NAV_CROUCH)))
		return false;

	// some bots stop often, some almost never (from 1 in 200 to 1 in 25 a second)
	float chance = (0.005f + 0.035f * LsTrait(2)) * Q_min(strength, 3.0f);
	if (RANDOM_FLOAT(0.0f, 1.0f) >= chance)
		return false;

	m_lsPauseUntil = gpGlobals->time + RANDOM_FLOAT(0.4f, 1.8f);
	ResetStuckMonitor();
	return true;
}

// ---- a plugin's short say over a bot ------------------------------------------------------
//
// ls_bot <player index> <what> [...], for plugins (gm_botfun) to have a bot do something
// human for a moment. The bot's own brain keeps running underneath, and anything that
// starts a fight (attacking, seeing an enemy) ends it straight away.
//
//   hold <secs>                      stand still
//   crouch <secs> [every]            crouch; with every > 0, down and up again every that
//                                    many seconds
//   jump <count> [every]             jump that many times, every that many seconds
//   look <x> <y> <z> <secs>          look at a point
//   nod <x> <y> <z> <secs> [every]   look at a point, aiming above and below it in turn
//   goto <x> <y> <z> [walk]          go somewhere (walk 1: walking, not running)
//   follow <player index> <secs>     tag along with someone
//   release                          stop all of it

// Doing something that mustn't be interrupted: the bomb, a grenade being lined up, hostages,
// a hurry, or just shot at or surprised. (Carrying the bomb only counts once the round is
// on: a salute in freeze time is fine.)
bool CCSBot::LsBusy() const
{
	return IsDefusingBomb() || IsEscapingFromBomb() || TheCSBots()->IsBombPlanted() || m_isWaitingToTossGrenade
		|| GetHostageEscortCount() > 0 || IsHurrying() || GetTimeSinceAttacked() < 3.0f || IsSurprised()
		|| (IsCarryingBomb() && !(CSGameRules() && CSGameRules()->IsFreezePeriod()))
		|| const_cast<CCSBot *>(this)->LsRushing();	// (the team's push onto a site, a retake)
}

// Too busy for a person's order (follow me, go b, wait): the bomb, a hostage, a grenade
// on its way. Being in a hurry, or a noise a moment ago, doesn't stop it doing as asked
bool CCSBot::LsOrderBusy() const
{
	return IsDefusingBomb() || IsEscapingFromBomb() || TheCSBots()->IsBombPlanted() || m_isWaitingToTossGrenade
		|| GetHostageEscortCount() > 0 || (IsCarryingBomb() && !(CSGameRules() && CSGameRules()->IsFreezePeriod()));
}

// The look on now is the puppet's own (still running, and nothing more urgent took over)
bool CCSBot::LsPuppetOwnsLook() const
{
	const LsPuppet &p = m_lsPuppet;
	if (gpGlobals->time >= p.lookUntil || m_lookAtSpotState == NOT_LOOKING_AT_SPOT)
		return false;
	return p.urgent ? m_lookAtSpotPriority == PRIORITY_UNINTERRUPTABLE : m_lookAtSpotPriority <= PRIORITY_LOW;
}

void CCSBot::LsPuppetRelease()
{
	if (m_lsPuppet.crouchUntil > 0.0f)
		StandUp();
	if (m_lsPuppet.followUntil > 0.0f && IsFollowing())
		StopFollowing();
	// (only its own look, still on: not the reaction - a sound, a shot - that ended the hold)
	if (LsPuppetOwnsLook())
		ClearLookAt();
	if (m_lsPuppet.walkUntil > 0.0f)
		Run();
	m_lsPuppet = LsPuppet();
}

bool CCSBot::LsPuppetActive() const
{
	float now = gpGlobals->time;
	const LsPuppet &p = m_lsPuppet;
	return now < p.holdUntil || now < p.crouchUntil || now < p.lookUntil || (p.jumpsLeft > 0 && now < p.jumpUntil) || now < p.walkUntil || now < p.followUntil;
}

// After the state machine each frame
void CCSBot::LsPuppetUpdate()
{
	// a grenade a person asked for: thrown as soon as it's in hand (a fight, or too long, and
	// it's off)
	if (m_lsThrowUntil > 0.0f)
	{
		if (gpGlobals->time > m_lsThrowUntil || IsAttacking() || IsEnemyVisible())
			m_lsThrowUntil = 0.0f;
		else if (IsUsingGrenade() && !m_isWaitingToTossGrenade)
		{
			ThrowGrenade(&m_lsThrowAt);
			m_lsThrowUntil = 0.0f;
		}
	}

	LsPuppet &p = m_lsPuppet;
	if (!p.any)
		return;

	// a fight, or anything that matters, comes first
	// (and nothing holds it on a ladder or blinded)
	// (a person's order: following them, or a chat hold, only gives way to what an order does)
	bool order = gpGlobals->time < p.followUntil || FStrEq(p.holdTag, "bc");
	bool urgent = p.urgent && gpGlobals->time < p.lookUntil && !IsDefusingBomb() && m_state != &m_plantBombState;
	// (a person's "wait" doesn't keep it standing while it's being hit)
	if (FStrEq(p.holdTag, "bc") && gpGlobals->time < p.holdUntil && (GetTimeSinceAttacked() < 1.0f || IsSurprised()))
	{
		LsPuppetRelease();
		return;
	}
	// (a plugin's hold - not a person's order - gives way once something's going on: a noise,
	// an enemy seen lately)
	if (m_isEnemyVisible || IsOnLadder() || !LsPuppetActive() || (!urgent && (IsAttacking() || (order ? LsOrderBusy() : LsBusy()) || IsBlind()))
		|| (!order && !urgent && LsFight() && !LsCalm(false)))
	{
		LsPuppetRelease();
		return;
	}

	float now = gpGlobals->time;

	// in the air (a jump in spawn): a look carries on, a hold or crouch waits for the ground
	bool grounded = (pev->flags & FL_ONGROUND) != 0;
	if (grounded && now < p.holdUntil)
	{
		ClearMovement();
		ResetStuckMonitor();
	}

	if (grounded && now < p.crouchUntil)
	{
		bool down = true;
		if (p.crouchEvery > 0.0f)
			down = (int((now - p.crouchStart) / p.crouchEvery) % 2) == 0;

		if (down)
			Crouch();
		else
			StandUp();
	}
	else if (p.crouchUntil > 0.0f)
	{
		StandUp();
		p.crouchUntil = 0.0f;
	}

	if (now < p.lookUntil)
	{
		Vector at = p.lookAt;
		if (p.nodEvery > 0.0f)
		{
			// about 25 degrees above and below it at the distance it is
			float range = (at - GetEyePosition()).Length2D();
			float rise = Q_max(12.0f, range * 0.45f);
			at.z += ((int((now - p.lookStart) / p.nodEvery) % 2) == 0) ? rise : -rise;
		}
		// (an urgent one, while aiming at someone: a quick turn that beats the aim; a hold's look
		// is only a scan, which noises and pre-aims win over)
		if (p.urgent && IsAimingAtEnemy())
		{
			if (now >= m_lsRunnerUntil)
				LsUrgentTurn(at, 0.35f, nullptr);
		}
		else
			SetLookAt("Loner-Strike", &at, p.urgent ? PRIORITY_UNINTERRUPTABLE : PRIORITY_LOW, 0.3f, false, 2.0f);
	}

	if (p.jumpsLeft > 0 && now >= p.nextJump)
	{
		if (Jump(MUST_JUMP))
		{
			p.jumpsLeft--;
			p.nextJump = now + p.jumpEvery;
		}
	}

	if (now < p.walkUntil)
		Walk();
}

// A person's order, kept up: a go reached (and held a while, if asked), a follow picked back
// up after a fight; it ends when it's done, runs out, the leader's gone, or "nvm"
void CCSBot::LsOrderTick()
{
	// handing the bomb over: walk to them, face them, drop it at their feet
	if (m_lsGiveTo.IsValid())
	{
		CBasePlayer *pTo = m_lsGiveTo;
		float now = gpGlobals->time;
		bool gun = m_lsGiveItem[0] != '\0';
		if ((gun ? !HasNamedPlayerItem(m_lsGiveItem) : !IsCarryingBomb()) || !pTo->IsAlive() || now > m_lsGiveUntil
			|| (!gun && TheCSBots()->IsBombPlanted()))
		{
			m_lsGiveTo = nullptr;
			m_lsGiveItem[0] = '\0';
		}
		else if (!IsAttacking())
		{
			Vector to = pTo->pev->origin - pev->origin;
			if (to.Make2D().IsLengthLessThan(140.0f))
			{
				Vector face = pTo->pev->origin;
				SetLookAt("Loner-Strike: handing the bomb", &face, PRIORITY_HIGH, 1.0f);
				float off = NormalizeAngle(UTIL_VecToYaw(to) - pev->v_angle.y);
				if (off > -35.0f && off < 35.0f)
				{
					const char *item = gun ? m_lsGiveItem : "weapon_c4";
					SelectItem(item);
					DropPlayerItem(item);
					m_lsGiveTo = nullptr;
					m_lsGiveItem[0] = '\0';
					// (a gun bought for them: back for its own, dropped when it bought that
					// one; with none, a rifle of its own if it still can)
					if (gun && m_lsGivePickup)
					{
						m_lsGivePickup = false;
						MoveTo(&m_lsGivePickupAt, FASTEST_ROUTE);
					}
					else
					{
						if (gun && CanPlayerBuy(false))
							BuyWeaponByWeaponID(this, m_iTeam == CT ? WEAPON_M4A1 : WEAPON_AK47);
						Idle();
					}
				}
			}
			else if (now >= m_lsGiveMoveAt)
			{
				m_lsGiveMoveAt = now + 1.0f;
				Vector at = pTo->pev->origin;
				MoveTo(&at, FASTEST_ROUTE);
			}
		}
	}

	LsOrder &o = m_lsOrder;
	if (o.kind == 0)
		return;
	float now = gpGlobals->time;
	// (done, run out, or the round's moved on: the bomb's down, it has hostages; an order
	// from before that would only keep it from the job)
	if (now >= o.until || !IsAlive() || TheCSBots()->IsBombPlanted() || GetHostageEscortCount() > 0
		|| (IsCarryingBomb() && !CSGameRules()->IsFreezePeriod()))
	{
		o = LsOrder();
		pev->vuser1.x = 0.0f;
		return;
	}
	if (o.kind == 3)
	{
		// holding the spot it was sent to: till the hide ends
		if (!IsHiding())
		{
			o = LsOrder();
			pev->vuser1.x = 0.0f;
		}
		return;
	}
	if (IsAttacking() || m_isEnemyVisible || LsOrderBusy() || IsOnLadder() || IsBuying())
		return;
	if (o.kind == 1)
	{
		if ((o.at - pev->origin).Make2D().IsLengthLessThan(120.0f))
		{
			// there: a spot with some cover close by, held a while if asked (each bot its own
			// spot, not five on one point); else back to its own business
			// (a CT sent to the hostages takes them, not a hold beside them)
			bool hostages = false;
			if (m_iTeam == CT && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_RESCUE_HOSTAGES)
			{
				CBaseEntity *pHostage = nullptr;
				while (!hostages && (pHostage = UTIL_FindEntityByClassname(pHostage, "hostage_entity")))
				{
					if (pHostage->IsAlive() && !(pHostage->pev->effects & EF_NODRAW) && (pHostage->pev->origin - pev->origin).IsLengthLessThan(300.0f))
						hostages = true;
				}
			}
			if (o.holdSecs > 0.0f && !hostages)
			{
				CNavArea *pArea = TheNavAreaGrid.GetNearestNavArea(&o.at);
				SetTask(HOLD_POSITION);	// (a plant elsewhere still calls it off: HideState)
				Hide(pArea, o.holdSecs, 350.0f, true);
				o.kind = 3;
				o.until = now + o.holdSecs + 2.0f;
				pev->vuser1.x = o.until;
				return;
			}
			o = LsOrder();
			pev->vuser1.x = 0.0f;
			return;
		}
		// (sent off it by something: a fight that's over, a state change; on its way again; but
		// a spot it can't get to is given up)
		// (in cover for a moment - from a rush, a shot - it waits: no try spent)
		if (!IsMovingTo() && !IsHiding() && now >= o.nextTry)
		{
			if (++o.tries > 2)
			{
				o = LsOrder();
				pev->vuser1.x = 0.0f;
				return;
			}
			o.nextTry = now + 2.0f;
			MoveTo(&o.at, SAFEST_ROUTE);
		}
		return;
	}
	CBasePlayer *pLeader = o.leader;
	if (!pLeader || !pLeader->IsAlive() || BotRelationship(pLeader) != BOT_TEAMMATE)
	{
		o = LsOrder();
		pev->vuser1.x = 0.0f;
		m_lsPuppet.followUntil = 0.0f;
		if (IsFollowing())
			StopFollowing();
		return;
	}
	if (!IsFollowing() && now >= o.nextTry && (pLeader->pev->origin - pev->origin).IsLengthLessThan(1500.0f))
	{
		o.nextTry = now + 2.0f;
		Follow(pLeader);
		m_lsPuppet.followUntil = o.until;
		m_lsPuppet.any = true;
	}
}

static CCSBot *LsBotByIndex(const char *arg)
{
	int index = Q_atoi(arg);
	if (index < 1 || index > gpGlobals->maxClients)
		return nullptr;

	CBasePlayer *pPlayer = UTIL_PlayerByIndex(index);
	if (!pPlayer || !pPlayer->IsBot() || !pPlayer->IsAlive())
		return nullptr;

	return static_cast<CCSBot *>(pPlayer);
}

static float LsArg(int i, float fallback = 0.0f)
{
	return (CMD_ARGC() > i) ? Q_atof(CMD_ARGV(i)) : fallback;
}

void Bot_LsServerCommand()
{
	// ls_bot <index> persona <aggression 0-1> <teamwork 0-1> <style> <temper> <humor>: who the
	// bot is (gm_people), alive or not
	if (FStrEq(CMD_ARGV(2), "persona"))
	{
		int index = Q_atoi(CMD_ARGV(1));
		CBasePlayer *pPlayer = (index >= 1 && index <= gpGlobals->maxClients) ? UTIL_PlayerByIndex(index) : nullptr;
		if (pPlayer && pPlayer->IsBot())
		{
			// (a missing value stays "not told": -1, so the profile's is used)
			float aggro = LsArg(3, -1.0f), team = LsArg(4, -1.0f);
			CCSBot *pBot = static_cast<CCSBot *>(pPlayer);
			pBot->LsSetPersona(aggro < 0.0f ? -1.0f : Q_min(1.0f, aggro), team < 0.0f ? -1.0f : Q_min(1.0f, team),
				int(LsArg(5, -1.0f)), int(LsArg(6, 1.0f)), int(LsArg(7, 1.0f)), int(LsArg(8, -1.0f)));
			// (persona ... <name>: the name it really is, when the shown one is a variant)
			pBot->LsSeedAs(CMD_ARGC() > 9 ? CMD_ARGV(9) : "");
		}
		return;
	}

	CCSBot *me = LsBotByIndex(CMD_ARGV(1));
	if (!me)
		return;

	const char *what = CMD_ARGV(2);
	float now = gpGlobals->time;

	// buyplan <0 buy | 1 save | 2 force>: a person's call ("eco", "force", "full buy") it
	// went along with (gm_botchat decides that): this buy in freeze time, else the next
	if (FStrEq(what, "buyplan"))
	{
		me->LsCallBuy(int(LsArg(3, 0.0f)));
		return;
	}
	// callout <x> <y> <z>: told where an enemy is (a teammate's call in chat): heard there
	if (FStrEq(what, "callout"))
	{
		if (!me->IsAlive() || me->IsAttacking() || !LsFight())
			return;
		me->LsTold(Vector(LsArg(3), LsArg(4), LsArg(5)));
		return;
	}
	// knife <secs>: a plugin put the knife out on purpose (showing off after a kill, a knife
	// run): left in its hand that long, unless an enemy shows up (the gun comes back then)
	if (FStrEq(what, "knife"))
	{
		me->m_lsKnifeOkUntil = now + Q_min(LsArg(3, 2.0f), 15.0f);
		return;
	}
	// throw <flash | smoke | he> <x> <y> <z>: a person asked for one there ("flash for me",
	// "smoke mid"): one it has, not in a fight, nor busy with the bomb or a ladder
	if (FStrEq(what, "throw"))
	{
		const char *kind = CMD_ARGV(3);
		const char *item = FStrEq(kind, "flash") ? "weapon_flashbang" : FStrEq(kind, "smoke") ? "weapon_smokegrenade"
			: FStrEq(kind, "he") ? "weapon_hegrenade" : nullptr;
		if (!item || !me->HasNamedPlayerItem(item) || me->IsAttacking() || me->IsEnemyVisible() || me->IsOnLadder()
			|| me->LsBusy() || me->IsThrowingGrenade())
			return;
		me->LsAskThrow(item, Vector(LsArg(4), LsArg(5), LsArg(6)));
		return;
	}

	// not while it's in a fight or busy with something that matters, nor while something
	// has startled it; a person's order (goto, follow, a chat hold) only gives way to a fight,
	// the bomb or a ladder
	// (push: a person's call for the team's site counts even if this one is busy - in a fight,
	// carrying the bomb; only its own joining in waits on that)
	// (the whole of it here, not behind the busy gate below: it's a mark, not a move - a bot in a
	// fight joins the push once the fight's over; a "no" is a "no" even mid-fight)
	if (FStrEq(what, "push"))
	{
		if (me->m_iTeam == TERRORIST && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB && !TheCSBots()->IsBombPlanted())
		{
			Vector at(LsArg(3), LsArg(4), LsArg(5));
			LsTeamCall(TheCSBots()->GetClosestZone(&at), LsArg(6) == 1.0f);
			if (LsArg(6) < 3.0f)
				me->LsPuppetCommand(what, now);
		}
		return;
	}
	bool order = FStrEq(what, "goto") || FStrEq(what, "push") || FStrEq(what, "follow") || (FStrEq(what, "hold") && CMD_ARGC() > 4 && FStrEq(CMD_ARGV(4), "bc"));
	// (look ... 2: an urgent turn, toward someone it senses behind it mid-fight: only an
	// enemy already in sight or a ladder stops it)
	bool urgent = FStrEq(what, "look") && LsArg(7) >= 2.0f;
	// (plant / dropbomb: the carrier's own orders; only a fight or a ladder stops them)
	if (FStrEq(what, "plant") || FStrEq(what, "dropbomb"))
	{
		if (!me->IsCarryingBomb() || me->IsAttacking() || me->IsEnemyVisible() || me->IsOnLadder() || TheCSBots()->IsBombPlanted())
			return;
	}
	else if (urgent)
	{
		if (me->IsEnemyVisible() || me->IsOnLadder())
			return;
	}
	else if (order)
	{
		if (me->IsAttacking() || me->IsEnemyVisible() || me->LsOrderBusy() || me->IsOnLadder())
			return;
	}
	else if (!FStrEq(what, "release") && (me->IsAttacking() || me->IsEnemyVisible() || me->LsBusy() || (LsFight() && !me->LsCalm(false))))
		return;

	me->LsPuppetCommand(what, now);
}

void CCSBot::LsAskThrow(const char *item, const Vector &at)
{
	SelectItem(item);
	m_lsThrowAt = at;
	m_lsThrowUntil = gpGlobals->time + 3.0f;
}

void CCSBot::LsPuppetCommand(const char *what, float now)
{
	LsPuppet &p = m_lsPuppet;
	const float most = 30.0f;

	// release [tag]: with a tag, only that plugin's hold goes (one plugin's habit doesn't
	// cancel another's)
	// (a person's order: chat's, tagged bc)
	bool ordered = LsOrderActive() || FStrEq(p.holdTag, "bc");
	if (FStrEq(what, "release"))
	{
		// with a tag, only that plugin's own hold goes; only chat's (bc: "nvm", "ok go") ends a
		// person's order, and nothing else ends a follow or a go
		const char *tag = CMD_ARGC() > 3 ? CMD_ARGV(3) : "";
		if (!tag[0])
		{
			LsPuppetRelease();
			return;
		}
		if (FStrEq(tag, "bc"))
		{
			// ("nvm" while holding where it was sent: it stops holding)
			if (m_lsOrder.kind == 3 && IsHiding())
				Idle();
			m_lsOrder = LsOrder();
			pev->vuser1.x = 0.0f;
			// (out of a called push too: back to its own game)
			// (out of the push, back to its own game - not sent to the other site - and no more
			// forced rush for the team)
			if (m_lsCallRound == LsRoundSerial())
			{
				m_lsCallRound = -1;
				LsTeamCallOff();
				if (LsExecuting())
					Idle();
			}
			LsPuppetRelease();
			return;
		}
		if (FStrEq(tag, p.holdTag))
		{
			if (p.crouchUntil > 0.0f)
				StandUp();
			if (!p.urgent && LsPuppetOwnsLook())
				ClearLookAt();
			p.holdUntil = p.crouchUntil = 0.0f;
			if (!p.urgent)
				p.lookUntil = 0.0f;
			p.holdTag[0] = '\0';
		}
		return;
	}

	// hold <secs> [tag]: the longest hold asked for stands; a person's (bc) always takes it
	// over, and while a person's order is on no plugin's hold gets in its way
	if (FStrEq(what, "hold"))
	{
		const char *tag = CMD_ARGC() > 4 ? CMD_ARGV(4) : "";
		bool chat = FStrEq(tag, "bc");
		if (ordered && !chat)
			return;
		float until = now + Q_min(LsArg(3, 1.0f), most);
		// (a person's hold is its own length - not stretched to a plugin hold's - and takes the tag)
		if (chat || until > p.holdUntil)
		{
			p.holdUntil = until;
			Q_strlcpy(p.holdTag, tag);
		}
		if (chat)
		{
			pev->fuser4 = now;	// (taken: gm_botchat reads it)
			m_lsOrderFields = true;
		}
	}
	else if (FStrEq(what, "crouch"))
	{
		if (ordered)
			return;
		p.crouchStart = now;
		p.crouchUntil = now + Q_min(LsArg(3, 1.0f), most);
		p.crouchEvery = Q_max(0.0f, LsArg(4));
	}
	else if (FStrEq(what, "jump"))
	{
		p.jumpsLeft = (int)Q_min(LsArg(3, 1.0f), 20.0f);
		p.jumpEvery = Q_max(0.1f, LsArg(4, 0.6f));
		p.nextJump = now;
		// jumps that can't happen (crouched, in a crawl space) are given up, not saved for later
		p.jumpUntil = now + p.jumpsLeft * p.jumpEvery + 1.5f;
	}
	else if (FStrEq(what, "look") || FStrEq(what, "nod"))
	{
		p.lookAt = Vector(LsArg(3), LsArg(4), LsArg(5));
		p.lookStart = now;
		p.lookUntil = now + Q_min(LsArg(6, 1.0f), LsArg(7) >= 2.0f ? 1.0f : most);
		p.nodEvery = FStrEq(what, "nod") ? Q_max(0.08f, LsArg(7, 0.35f)) : 0.0f;
		// (look ... 1: a quick look, a flick or a spin; a nod is always quick; 2: urgent)
		p.fast = FStrEq(what, "nod") || LsArg(7) > 0.0f;
		p.urgent = FStrEq(what, "look") && LsArg(7) >= 2.0f;
		// (an urgent turn: someone's behind it; standing still for a plugin's habit ends)
		if (p.urgent && !FStrEq(p.holdTag, "bc"))
			p.holdUntil = p.crouchUntil = 0.0f;
	}
	else if (FStrEq(what, "goto"))
	{
		// goto x y z [walk] [hold secs] [bc]: with bc a person's order (the latest wins, a hold
		// or crouch from before ends, and with hold secs it holds near the spot once there);
		// without it a plugin's walk (a gift, a step aside), which never replaces an order
		bool chat = CMD_ARGC() > 8 && FStrEq(CMD_ARGV(8), "bc");
		if (!chat && ordered)
			return;
		Vector to(LsArg(3), LsArg(4), LsArg(5));
		// (a spot on floor the nav doesn't cover: the nearest that it does)
		if (!TheNavAreaGrid.GetNavArea(&to))
		{
			CNavArea *pNear = TheNavAreaGrid.GetNearestNavArea(&to);
			if (pNear)
			{
				Vector snapped;
				pNear->GetClosestPointOnArea(&to, &snapped);
				to = snapped;
			}
		}
		if (chat)
		{
			// (and a look a plugin had it holding goes: it looks where it's going)
			p.holdUntil = p.crouchUntil = p.lookUntil = 0.0f;
			p.holdTag[0] = '\0';
		}
		// (still in the shop: it finishes buying first; the order sends it once it's done)
		bool buying = chat && IsBuying();
		if (!buying)
			MoveTo(&to, SAFEST_ROUTE);
		if (LsArg(6) > 0.0f)
			p.walkUntil = now + most;
		if (chat)
		{
			m_lsOrder = LsOrder();
			pev->vuser1.x = 0.0f;
			// (somewhere it can't get to isn't taken: gm_botchat then says it can't)
			if (!buying && !HasPath())
				return;
			m_lsOrder.kind = 1;
			m_lsOrder.at = to;
			m_lsOrder.until = now + 45.0f;
			m_lsOrder.holdSecs = Q_min(LsArg(7), most);
			m_lsOrder.nextTry = now + 2.0f;
			pev->fuser4 = now; m_lsOrderFields = true;	// (taken: gm_botchat reads it)
			pev->vuser1.x = m_lsOrder.until;	// (and on till then: gm_pubserver won't kick it)
		}
	}
	else if (FStrEq(what, "push"))
	{
		// push x y z <0 go | 1 rush | 2 heard, not going> bc: a person's "go b" / "rush b" to the
		// whole team, on a bomb map, for a T. The ones who said yes join the team's push on that
		// site, roles and all (entry, trade, flank, perch: picked from who suits them); "rush"
		// goes in at once, "go" gathers out of sight first. Not an order that takes it over: the
		// push is the order. One who heard it and didn't want to does its own thing
		int round = LsRoundSerial();
		Vector at(LsArg(3), LsArg(4), LsArg(5));
		int kind = (int)LsArg(6);
		if (m_iTeam != TERRORIST || TheCSBots()->GetScenario() != CCSBotManager::SCENARIO_DEFUSE_BOMB || TheCSBots()->IsBombPlanted())
			return;
		const CCSBotManager::Zone *zone = TheCSBots()->GetClosestZone(&at);
		if (!zone)
			return;
		if (kind == 2)
		{
			m_lsCallSkipRound = round;
			m_lsCallRound = -1;
			return;
		}
		m_lsCallRound = round;
		m_lsCallSkipRound = -1;
		m_lsOrder = LsOrder();
		// (a plugin's hold goes; one in a fight keeps fighting - the push waits for it)
		if (!IsAttacking())
		{
			p.holdUntil = p.crouchUntil = p.lookUntil = 0.0f;
			p.holdTag[0] = '\0';
		}
		m_lsExecCheckAt = 0.0f;
		pev->fuser4 = now; m_lsOrderFields = true;	// (taken: gm_botchat reads it)
		pev->vuser1.x = now + 30.0f;
	}
	else if (FStrEq(what, "plant"))
	{
		// plant -1 [x y z]: to the site nearest that point (or to the bot) and plant there
		const CCSBotManager::Zone *zone = nullptr;
		if (CMD_ARGC() > 6)
		{
			Vector at(LsArg(4), LsArg(5), LsArg(6));
			zone = TheCSBots()->GetClosestZone(&at);
		}
		else
			zone = TheCSBots()->GetClosestZone(&pev->origin);
		const Vector *pos = zone ? TheCSBots()->GetRandomPositionInZone(zone) : nullptr;
		if (!pos)
			return;
		m_lsPlantOrderZone = zone;
		m_lsPlantOrderRound = LsRoundSerial();
		// (no more tagging along or waiting for others: told to go)
		m_lsCarrierFollowRound = LsRoundSerial();
		m_lsCarrierWaits = 2;
		if (IsFollowing())
			StopFollowing();
		m_lsCarrierFollowUntil = 0.0f;
		SetTask(PLANT_BOMB);
		Vector to = *pos;
		MoveTo(&to, FASTEST_ROUTE);
		pev->fuser4 = now; m_lsOrderFields = true;	// (taken)
		LsWhy("plant-order");
		return;
	}
	else if (FStrEq(what, "buyfor"))
	{
		// buyfor <player> <weapon id>: a gun bought for a teammate and handed over ("someone
		// buy me an awp"); in the buy zone with the money, one at a time. Its own gun, if it
		// had one, drops where it stands (as buying does): it comes back for it
		int index = Q_atoi(CMD_ARGV(3));
		CBasePlayer *pTo = (index >= 1 && index <= gpGlobals->maxClients) ? UTIL_PlayerByIndex(index) : nullptr;
		if (!pTo || !pTo->IsAlive() || pTo == this || pTo->m_iTeam != m_iTeam || m_lsGiveTo.IsValid() || !CanPlayerBuy(false))
			return;
		bool had = m_bHasPrimary;
		Vector at = pev->origin;
		CBaseEntity *pGot = BuyWeaponByWeaponID(this, WeaponIdType(Q_atoi(CMD_ARGV(4))));
		if (!pGot)
			return;
		Q_strlcpy(m_lsGiveItem, STRING(pGot->pev->classname));
		m_lsGivePickup = had;
		m_lsGivePickupAt = at;
		m_lsGiveTo = pTo;
		m_lsGiveUntil = now + 25.0f;
		m_lsGiveMoveAt = 0.0f;
		if (IsFollowing())
			StopFollowing();
		pev->fuser4 = now; m_lsOrderFields = true;	// (taken)
		LsWhy("buy-for");
		return;
	}
	else if (FStrEq(what, "dropbomb"))
	{
		// dropbomb <player>: over to them and the bomb dropped at their feet
		int index = Q_atoi(CMD_ARGV(3));
		CBasePlayer *pTo = (index >= 1 && index <= gpGlobals->maxClients) ? UTIL_PlayerByIndex(index) : nullptr;
		if (!pTo || !pTo->IsAlive() || pTo == this || pTo->m_iTeam != m_iTeam)
			return;
		m_lsGiveTo = pTo;
		m_lsGiveUntil = now + 25.0f;
		m_lsGiveMoveAt = 0.0f;
		m_lsCarrierFollowRound = LsRoundSerial();
		m_lsCarrierFollowUntil = 0.0f;
		if (IsFollowing())
			StopFollowing();
		pev->fuser4 = now; m_lsOrderFields = true;	// (taken)
		LsWhy("give-bomb");
		return;
	}
	else if (FStrEq(what, "follow"))
	{
		int index = Q_atoi(CMD_ARGV(3));
		CBasePlayer *pLeader = (index >= 1 && index <= gpGlobals->maxClients) ? UTIL_PlayerByIndex(index) : nullptr;
		if (!pLeader || !pLeader->IsAlive() || pLeader == this || BotRelationship(pLeader) != BOT_TEAMMATE)
			return;
		p.holdUntil = p.crouchUntil = p.lookUntil = 0.0f;
		p.holdTag[0] = '\0';
		Follow(pLeader);
		p.followUntil = now + Q_min(LsArg(4, 20.0f), 60.0f);
		m_lsOrder = LsOrder();
		m_lsOrder.kind = 2;
		m_lsOrder.leader = pLeader;
		m_lsOrder.until = p.followUntil;
		m_lsOrder.nextTry = now + 2.0f;
		pev->fuser4 = now; m_lsOrderFields = true;	// (taken: gm_botchat reads it)
		pev->vuser1.x = m_lsOrder.until;
	}
	else
	{
		return;
	}

	p.any = true;
}

// Two players of the same clan: both names carry the same tag (what's left once LsBareName
// takes it off). Clan mates stick together: no spreading out from each other
bool LsClanMates(CBasePlayer *a, CBasePlayer *b)
{
	char bareA[64], bareB[64];
	const char *na = STRING(a->pev->netname), *nb = STRING(b->pev->netname);
	LsBareName(na, bareA, sizeof(bareA));
	LsBareName(nb, bareB, sizeof(bareB));
	// (the tag: the name with the bare part cut out)
	char tagA[64] = "", tagB[64] = "";
	auto tagOf = [](const char *name, const char *bare, char *out, size_t size) {
		const char *at = bare[0] ? Q_strstr(name, bare) : nullptr;
		if (!at)
			return;
		size_t pre = size_t(at - name);
		Q_strlcpy(out, name, Q_min(size, pre + 1));
		Q_strlcat(out, at + Q_strlen(bare), size);
	};
	tagOf(na, bareA, tagA, sizeof(tagA));
	tagOf(nb, bareB, tagB, sizeof(tagB));
	// (compared by its letters and digits, case aside, as the chat plugin does: "[CoC] " and
	// "-=[CoC]=- " are one clan; it needs a letter, so "(1)" from a duplicate name isn't one)
	auto squash = [](char *tag) {
		char *out = tag;
		bool letter = false;
		for (const char *c = tag; *c; c++)
		{
			if (isalnum((unsigned char)*c))
			{
				*out++ = (char)tolower((unsigned char)*c);
				letter = letter || isalpha((unsigned char)*c);
			}
		}
		*out = '\0';
		return letter;
	};
	bool real = squash(tagA);
	squash(tagB);
	return real && FStrEq(tagA, tagB);
}

// ---- every bot for itself (deathmatch-type modes) -----------------------------------------


// Which connected part of the nav an area is in (links either way, ladders included), worked
// out once per nav: a nav learned with spawn seeds can have parts no path joins (Base Builder's
// two sides, a Deathrun booth), and a hunt goal in another part is a walk to nowhere. -1 unknown.
static int LsNavPart(const CNavArea *area)
{
	static std::vector<int> s_part;
	static std::vector<const CNavArea *> s_areas;
	static int s_for = -1;
	static const CNavArea *s_front = nullptr;
	if (!area || TheNavAreaList.empty())
		return -1;
	// (every nav load counts, learning's included; the first area too, cheaply: a list's size
	// is a walk of the whole list on the Linux build)
	if (s_for != g_lsNavLoads || s_front != TheNavAreaList.front())
	{
		s_for = g_lsNavLoads;
		s_front = TheNavAreaList.front();
		s_areas.assign(TheNavAreaList.begin(), TheNavAreaList.end());
		std::sort(s_areas.begin(), s_areas.end());
		s_part.assign(s_areas.size(), -1);
		auto indexOf = [](const CNavArea *a) -> int {
			auto it = std::lower_bound(s_areas.begin(), s_areas.end(), a);
			return (it != s_areas.end() && *it == a) ? int(it - s_areas.begin()) : -1;
		};
		// links one way count both ways (a drop is still the same part)
		std::vector<std::vector<int>> links(s_areas.size());
		auto join = [&](int i, const CNavArea *b) {
			int j = indexOf(b);
			if (j >= 0 && j != i)
			{
				links[i].push_back(j);
				links[j].push_back(i);
			}
		};
		for (int i = 0; i < (int)s_areas.size(); i++)
		{
			const CNavArea *a = s_areas[i];
			for (int d = 0; d < NUM_DIRECTIONS; d++)
				for (int k = 0; k < a->GetAdjacentCount((NavDirType)d); k++)
					join(i, a->GetAdjacentArea((NavDirType)d, k));
			for (int l = 0; l < NUM_LADDER_DIRECTIONS; l++)
				for (auto ladder : *a->GetLadderList((LadderDirectionType)l))
				{
					join(i, ladder->m_topForwardArea);
					join(i, ladder->m_topLeftArea);
					join(i, ladder->m_topRightArea);
					join(i, ladder->m_topBehindArea);
					join(i, ladder->m_bottomArea);
				}
		}
		int part = 0;
		std::vector<int> stack;
		for (int s0 = 0; s0 < (int)s_areas.size(); s0++)
		{
			if (s_part[s0] >= 0)
				continue;
			s_part[s0] = part;
			stack.assign(1, s0);
			while (!stack.empty())
			{
				int u = stack.back();
				stack.pop_back();
				for (int v : links[u])
					if (s_part[v] < 0)
					{
						s_part[v] = part;
						stack.push_back(v);
					}
			}
			part++;
		}
	}
	auto it = std::lower_bound(s_areas.begin(), s_areas.end(), area);
	return (it != s_areas.end() && *it == area) ? s_part[it - s_areas.begin()] : -1;
}

// Whether a goal area is in the same part of the nav as the bot (unknown counts as yes).
static bool LsSamePart(const CCSBot *me, const CNavArea *goal)
{
	int mine = LsNavPart(me->GetLastKnownArea());
	int theirs = LsNavPart(goal);
	return mine < 0 || theirs < 0 || mine == theirs;
}

// Where to hunt: of a handful of random parts of the map, the one furthest from where its
// teammates are and where they're heading (with some chance in it, and not right here)
CNavArea *CCSBot::LsPickHuntArea()
{
	int count = (int)TheNavAreaList.size();
	if (count <= 0)
		return nullptr;

	// the places to keep away from: teammates, and the ends of the bots' paths
	Vector avoid[64];
	int n = 0;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		// (clan mates stick together; not in deathmatch, where a pack of them is a blob)
		if (!LsSolo() && LsClanMates(this, pPlayer))
			return;
		if (n < 64)
			avoid[n++] = pPlayer->pev->origin;
		if (n < 64 && pPlayer->IsBot())
		{
			CCSBot *pBot = static_cast<CCSBot *>(pPlayer);
			if (pBot->HasPath())
				avoid[n++] = pBot->GetPathEndpoint();
		}
	});

	// the places this side looks after: CTs on a bomb map the bomb sites until it's planted,
	// Ts on a hostage map the hostages (they hold the building, not rush the CT spawn); a
	// bold few push anyway
	Vector keep[16];
	int kept = 0;
	if (LsFight() && !LsSolo() && !(LsAggro() > 0.8f && LsTrait(31) < 0.5f))
	{
		if (m_iTeam == CT && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB && !TheCSBots()->IsBombPlanted())
		{
			for (int z = 0; z < TheCSBots()->GetZoneCount() && kept < 16; z++)
				keep[kept++] = TheCSBots()->GetZone(z)->m_center;
		}
		else if (m_iTeam == TERRORIST && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB && !TheCSBots()->IsBombPlanted())
		{
			// (Ts work toward the round's site, as a pub team drifts to "b" together, each by its
			// own way; the odd lurker takes the other one)
			const CCSBotManager::Zone *site = LsTSite();
			bool other = LsLurksOtherSite();
			for (int z = 0; z < TheCSBots()->GetZoneCount() && kept < 16; z++)
			{
				const CCSBotManager::Zone *zone = TheCSBots()->GetZone(z);
				if (!site || (zone == site) != other)
					keep[kept++] = zone->m_center;
			}
			if (!kept && site)
				keep[kept++] = site->m_center;
		}
		else if (m_iTeam == TERRORIST && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_RESCUE_HOSTAGES)
		{
			CBaseEntity *pHostage = nullptr;
			while (kept < 16 && (pHostage = UTIL_FindEntityByClassname(pHostage, "hostage_entity")))	// (kept close: the building, not the yard)
			{
				if (pHostage->IsAlive() && !(pHostage->pev->effects & EF_NODRAW))
					keep[kept++] = pHostage->pev->origin;
			}
		}
	}

	// how many teammates are already at each of those places (spread across them, not all
	// on one bomb site)
	int crowd[16] = {};
	for (int k = 0; k < kept; k++)
	{
		LsForTeammates(this, [&](CBasePlayer *pPlayer) {
			if ((pPlayer->pev->origin - keep[k]).IsLengthLessThan(1400.0f))
				crowd[k]++;
		});
	}

	const Vector *ownSpawn = LsEnemySpawn(m_iTeam == CT ? TERRORIST : CT);

	// (half the picks, when there's been gunfire in the last 10 s)
	bool pullFight = m_lsFightHeardAt > 0.0f && gpGlobals->time - m_lsFightHeardAt < 10.0f && RANDOM_FLOAT(0.0f, 1.0f) < 0.5f;

	const int tries = 12;
	int picks[tries];
	for (int t = 0; t < tries; t++)
		picks[t] = RANDOM_LONG(0, count - 1);

	CNavArea *best = nullptr;
	float bestScore = -1e9f;	// (the pulls can put every candidate below zero)
	int index = 0;
	for (auto area : TheNavAreaList)
	{
		for (int t = 0; t < tries; t++)
		{
			if (picks[t] != index)
				continue;

			// (not the small ones: corners of rooms, vents, ledges)
			const Extent *ext = area->GetExtent();
			if (ext->hi.x - ext->lo.x < 100.0f || ext->hi.y - ext->lo.y < 100.0f)
				continue;
			// (nor one no path reaches from here, nor one over a floor that broke)
			if (!LsSamePart(this, area) || LsOverBrokenFloor(area))
				continue;

			const Vector *c = area->GetCenter();
			float score = RANDOM_FLOAT(0.0f, 500.0f);
			// (not back where it's been the last half minute, unless it's holding a place
			// - Ts at the hostages - where pacing is the point)
			if (!kept && LsBeenNear(*c, 600.0f))
				score -= 2000.0f;
			// (nor turning round on the way somewhere for a new goal)
			if (!kept && LsBehindMe(*c))
				score -= 1500.0f;

			// somewhere to go, not where it is
			float fromMe = (*c - pev->origin).Length2D();
			// (somewhere to go: on a small map nothing at all beats pacing on the spot; but Ts
			// keeping to the hostages may settle near where they are)
			if (fromMe < 600.0f)
			{
				if (!kept)
					continue;
				score -= 1500.0f;
			}

			float nearest = 3000.0f;
			for (int k = 0; k < n; k++)
				nearest = Q_min(nearest, float((*c - avoid[k]).Length2D()));
			score += nearest;

			// a deathmatch: now and then toward fighting it heard lately (that's where people are;
			// gently, so a crowd doesn't all pour into one fight)
			if (LsSolo() && pullFight)
				score -= 0.15f * float((*c - m_lsFightHeard).Length2D());

			// and toward where an enemy was last seen, more so the fewer are left (with a crowd
			// there's someone everywhere; and not one who's since died)
			if (LsSolo() && GetTimeSinceLastSawEnemy() < 30.0f && m_enemy.IsValid() && m_enemy->IsAlive() && m_enemy->m_iDeaths == m_lsEnemyDeaths)
			{
				int left = GetEnemiesRemaining();
				if (left <= 6)
					score -= (left <= 1 ? 1.2f : left <= 2 ? 0.8f : 0.35f) * float((*c - GetLastKnownEnemyPosition()).Length2D());
			}

			// and in deathmatch: not where it keeps getting killed
			if (LsSolo() && LsOwnMemory())
				score -= 800.0f * Q_min(1.0f, LsGetDanger(area));

			// (a round with objectives: never back toward its own spawn - the wide-open places
			// nobody's in are mostly the side's own backyard, and a loner kept wandering home)
			if (!LsSolo() && TheCSBots()->GetScenario() != CCSBotManager::SCENARIO_DEATHMATCH && ownSpawn
				&& (*c - *ownSpawn).Length2D() < (pev->origin - *ownSpawn).Length2D() - 200.0f)
				score -= 3000.0f;

			// near what this side looks after
			if (kept)
			{
				float site = 99999.0f;
				int at = 0;
				for (int k = 0; k < kept; k++)
				{
					float d = float((*c - keep[k]).Length2D());
					if (d < site)
					{
						site = d;
						at = k;
					}
				}
				// (Ts by the hostages: closer than to a bomb site, they hold the building)
				float within = (m_iTeam == TERRORIST && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_RESCUE_HOSTAGES) ? 900.0f : 1400.0f;
				score += site < within ? 400.0f - 220.0f * float(Q_min(crowd[at], 4)) : -1500.0f;
			}

			if (score > bestScore)
			{
				bestScore = score;
				best = area;
			}
		}
		index++;
	}
	return best;
}

// A teammate closer to that spot than this bot (it'll be the one to look into it)
bool CCSBot::LsTeammateCloser(const Vector &pos)
{
	float mine = (pos - pev->origin).Length();
	bool closer = false;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		if ((pos - pPlayer->pev->origin).Length() < mine)
			closer = true;
	});
	return closer;
}

// Time to go somewhere else: it's walking right next to a teammate (checked every couple of
// seconds, and not again straight after breaking off)
bool CCSBot::LsShouldSpread()
{
	if (gpGlobals->time < m_lsNextSpreadCheck)
		return false;
	m_lsNextSpreadCheck = gpGlobals->time + RANDOM_FLOAT(1.8f, 2.6f);

	bool crowded = false;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		// (deathmatch: a pair walking together is a blob in the making; further apart, and surely)
		if ((pPlayer->pev->origin - pev->origin).IsLengthLessThan(LsSolo() ? 450.0f : 320.0f) && (LsSolo() || !LsClanMates(this, pPlayer)))
			crowded = true;
	});
	if (!crowded || RANDOM_FLOAT(0.0f, 1.0f) > (LsSolo() ? 0.9f : 0.7f))
		return false;

	m_lsNextSpreadCheck = gpGlobals->time + RANDOM_FLOAT(5.0f, 8.0f);
	return true;
}

// ---- fights (bot_ls_fight) ----------------------------------------------------------------
// Per-bot habits come from LsTrait (its name), so a bot fights the same way every time.

// Chance (0-100) this bot keeps moving in a fight: skill helps, but nobody stands like a post
float CCSBot::LsDodgeChance()
{
	return 35.0f + 45.0f * LsSkill() + 20.0f * LsTrait(10);
}

// How long it keeps one strafe or stop: some tap, some hold a direction
float CCSBot::LsDodgeDuration(bool steady)
{
	float lo = 0.15f + 0.35f * LsTrait(11);
	float hi = lo + 0.25f + 0.75f * LsTrait(12);
	float d = RANDOM_FLOAT(lo, hi);
	return steady ? d * 0.6f : d;
}

// A stop between strafes: more for the ones who stop to shoot
bool CCSBot::LsPickSteady()
{
	return RANDOM_FLOAT(0.0f, 1.0f) < 0.2f + 0.4f * LsTrait(13);
}

// Some bots back off when an enemy gets close; only with room behind (not into a wall or
// off a ledge they can't see)
bool CCSBot::LsMayBackOff(const Vector &toEnemy)
{
	if (LsTrait(14) > 0.35f)
		return false;

	Vector back = -toEnemy;
	back.z = 0.0f;
	if (back.Length2D() < 1.0f)
		return false;
	back = back.Normalize();

	// room for a body behind, not just a line
	TraceResult tr;
	Vector from = pev->origin;
	UTIL_TraceHull(from, from + back * 96.0f, ignore_monsters, human_hull, ENT(pev), &tr);
	if (tr.flFraction < 1.0f || tr.fStartSolid)
		return false;

	// ground behind (a drop would be a fall)
	Vector behind = from + back * 96.0f;
	UTIL_TraceLine(behind, behind - Vector(0, 0, 80.0f), ignore_monsters, ENT(pev), &tr);
	if (tr.flFraction < 1.0f)
		return true;
	// (a newer player in a fight, now and then, backs off an edge it didn't see: rare, a few
	// times a long session at most)
	// (once every 8 minutes at most on the whole server, whoever it is)
	static float s_edgeSlipAt = -999.0f;
	if (gpGlobals->time < s_edgeSlipAt)
		s_edgeSlipAt = -999.0f;	// (a new map: the clock restarted)
	if (LsSkill() < 0.3f && !LsMatch() && LsTrait(74) < 0.35f && gpGlobals->time - s_edgeSlipAt > 480.0f && RANDOM_FLOAT(0.0f, 1.0f) < 0.05f)
	{
		// (a drop it lives through: never into a pit or off a deadly height)
		UTIL_TraceLine(behind, behind - Vector(0, 0, 1000.0f), ignore_monsters, ENT(pev), &tr);
		float drop = float(pev->origin.z - HalfHumanHeight - tr.vecEndPos.z);
		if (tr.flFraction >= 1.0f || GetApproximateFallDamage(drop) + LsFallMargin() >= pev->health)
			return false;
		s_edgeSlipAt = gpGlobals->time;
		LsWhy("backed off an edge");
		return true;
	}
	return false;
}

// What's beside a bot 40 units one way: 0 floor (or a small step down), 1 a wall (a low wall or
// a railing's bottom bar too: checked at the knees as well as the waist), 2 a drop that would hurt
static int LsBeside(CCSBot *me, float yaw, float feet)
{
	Vector dir(BotCOS(yaw), BotSIN(yaw), 0.0f);
	Vector low(me->pev->origin.x, me->pev->origin.y, feet + 8.0f);
	Vector high(me->pev->origin.x, me->pev->origin.y, feet + 36.0f);
	TraceResult tr;
	UTIL_TraceLine(low, low + dir * 40.0f, ignore_monsters, ENT(me->pev), &tr);
	if (tr.flFraction < 1.0f || tr.fStartSolid)
		return 1;
	UTIL_TraceLine(high, high + dir * 40.0f, ignore_monsters, ENT(me->pev), &tr);
	if (tr.flFraction < 1.0f || tr.fStartSolid)
		return 1;
	Vector out = low + dir * 40.0f;
	UTIL_TraceLine(out, out - Vector(0, 0, 1000.0f), ignore_monsters, ENT(me->pev), &tr);
	if (tr.flFraction >= 1.0f || me->GetApproximateFallDamage(feet - tr.vecEndPos.z) >= 10.0f)
		return 2;
	return 0;
}

// On a beam or a ledge about a stride wide (a nav area that narrow, or none under it at all):
// a drop that would hurt on one side, and a drop or a wall on the other - not the edge of a wide
// walkway. A person walks one of those looking where they're going: no looking about, no peeks,
// no hops (cs_rats2's CT spawn beams: bots glancing at angles drifted off the side and fell)
bool CCSBot::LsNarrowFooting()
{
	float now = gpGlobals->time;
	// (checked a few times a second; a clock that went back - a new map - checks again)
	if (now < m_lsNarrowNext && now > m_lsNarrowNext - 1.0f)
		return m_lsNarrow;
	m_lsNarrowNext = now + 0.2f;
	m_lsNarrow = false;
	if (!LsFight() || !IsAlive() || IsOnLadder() || !(pev->flags & FL_ONGROUND))
		return false;
	const CNavArea *area = TheNavAreaGrid.GetNavArea(&pev->origin);
	if (area && Q_min(area->GetSizeX(), area->GetSizeY()) > 40.0f)
		return false;
	// across the way it's going (standing still: across the way it faces, and along it too)
	float moving = pev->velocity.Length2D();
	float yaw = moving > 30.0f ? UTIL_VecToYaw(pev->velocity) : pev->v_angle.y;
	float feet = pev->origin.z - ((pev->flags & FL_DUCKING) ? 18.0f : HalfHumanHeight);
	for (int pair = 0; pair < (moving > 30.0f ? 1 : 2); pair++)
	{
		float a = yaw + 90.0f * float(1 - pair);
		int one = LsBeside(this, a, feet), other = LsBeside(this, a + 180.0f, feet);
		if ((one == 2 && other != 0) || (other == 2 && one != 0))
		{
			m_lsNarrow = true;
			break;
		}
	}
	return m_lsNarrow;
}

// A spot on the wall a ladder's on (at the foot of one, the ladder in front of a bot looks just
// like a wall: the wall behind it - a ladder itself isn't solid)
bool CCSBot::LsLadderAt(const Vector &spot) const
{
	for (const CNavLadder *ladder : TheNavLadderList)
	{
		if (spot.z < ladder->m_bottom.z - 24.0f || spot.z > ladder->m_top.z + 24.0f)
			continue;
		float dx = spot.x - (ladder->m_top.x + ladder->m_bottom.x) * 0.5f;
		float dy = spot.y - (ladder->m_top.y + ladder->m_bottom.y) * 0.5f;
		if (dx * dx + dy * dy < 48.0f * 48.0f)
			return true;
	}
	return false;
}

// Holds fire while moving fast with a rifle at range, to stop and then shoot: what good
// players do. Newer ones (skill under a half) run and gun like pub players did
bool CCSBot::LsHoldFireToStop(float range)
{
	// (by its habit: the counter-strafers and crouch-stoppers hold the shot to stop; the walkers
	// slow down instead (shift), the run-and-gunners just shoot. Up close everyone just shoots)
	int stop = LsStopStyle();
	if (stop == 0 || stop == 2 || range < (stop == 3 ? 500.0f : 350.0f) || !LsStopGun())
		return false;
	// moving fast enough to spoil the shot (half its top speed: follows a mode's speeds)
	return pev->velocity.Length2D() > 0.5f * pev->maxspeed;
}

// A gun worth stopping (or slowing) for at range: the rifles and the Deagle
bool CCSBot::LsStopGun() const
{
	CBasePlayerWeapon *pWeapon = static_cast<CBasePlayerWeapon *>(m_pActiveItem);
	switch (pWeapon ? pWeapon->m_iId : 0)
	{
	case WEAPON_AK47: case WEAPON_M4A1: case WEAPON_GALIL: case WEAPON_FAMAS:
	case WEAPON_SG552: case WEAPON_AUG: case WEAPON_DEAGLE:
		return true;
	}
	return false;
}

// How it stops to shoot with a rifle on the move, its own habit: 0 runs and guns, 1 crouches to
// stop, 2 walks (shift) and shoots, 3 stops dead with a counter-strafe. The better players mostly
// counter-strafe; the middling ones walk or crouch about as often as they stop properly; the
// newer ones split between walking, crouching and running and gunning. The bold run more, the
// careful less
int CCSBot::LsStopStyle()
{
	float skill = LsSkill();
	float counter = Q_max(0.0f, Q_min(0.9f, 1.3f * skill - 0.3f));
	float walk = 0.3f * (1.0f - counter);
	float crouch = (0.15f + 0.25f * (1.0f - skill)) * (1.0f - counter);
	float run = Q_max(0.0f, 1.0f - counter - walk - crouch);
	int style = LsStyle();
	if (style == LS_STYLE_RUSHER || style == LS_STYLE_KNIFER)
		run += 0.25f;
	else if (style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER || style == LS_STYLE_AWPER)
		run *= 0.4f;
	float t = LsTrait(98) * (counter + walk + crouch + run);
	if (t < counter)
		return 3;
	t -= counter;
	if (t < walk)
		return 2;
	t -= walk;
	return t < crouch ? 1 : 0;
}

// A crouch to stop for the shot (crouching slows it at once): a short one, then up again
void CCSBot::LsCrouchStop()
{
	if (m_lsCrouchBurstUntil > gpGlobals->time || !(pev->flags & FL_ONGROUND))
		return;
	m_lsCrouchBurstUntil = gpGlobals->time + RANDOM_FLOAT(0.3f, 0.6f);
	Crouch();
}

// In a fight right on top of a teammate (the entry it came in behind, a trader on the same spot)
// or in their line of fire: two on one spot are one spray's work, so a step off to the side -
// the better players nearly always, the newer ones now and then (pubs do bunch up)
bool CCSBot::LsSpaceOut()
{
	float now = gpGlobals->time;
	if (now < m_lsSpaceUntil)
	{
		if (m_lsSpaceDir > 0)
			StrafeRight();
		else
			StrafeLeft();
		return true;
	}
	if (now < m_lsSpaceCheckAt)
		return false;
	m_lsSpaceCheckAt = now + RANDOM_FLOAT(0.4f, 0.7f);
	CBasePlayer *pMate = nullptr;
	float best = 110.0f;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		Vector d = pPlayer->pev->origin - pev->origin;
		if (Q_abs(d.z) > 60.0f)
			return;
		float len = float(d.Length2D());
		if (len < best)
		{
			best = len;
			pMate = pPlayer;
		}
	});
	if (!pMate)
		return false;
	float chance = 0.15f + 0.7f * LsSkill();
	int style = LsStyle();
	if (style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER || style == LS_STYLE_AWPER)
		chance *= 1.15f;
	else if (style == LS_STYLE_RUSHER)
		chance *= 0.8f;
	if (RANDOM_FLOAT(0.0f, 1.0f) >= chance)
	{
		// (left as it is: not asked again every moment)
		m_lsSpaceCheckAt = now + RANDOM_FLOAT(1.5f, 3.0f);
		return false;
	}
	// away from them, across its own view (a teammate on its left: a step right) - if there's
	// room that way (no wall, no drop), else the other way, else it stays
	float yaw = pev->v_angle.y;
	Vector2D left2(-BotSIN(yaw), BotCOS(yaw));
	Vector d = pMate->pev->origin - pev->origin;
	float left = d.x * left2.x + d.y * left2.y;
	int dir = left > 8.0f ? 1 : left < -8.0f ? -1 : (RANDOM_LONG(0, 1) ? 1 : -1);
	auto room = [&](int side) -> bool {
		Vector step = Vector(-left2.x, -left2.y, 0.0f) * (72.0f * side);		// (+1: to the right)
		Vector knee = pev->origin + Vector(0, 0, -HalfHumanHeight + 20.0f);
		TraceResult tr;
		UTIL_TraceLine(knee, knee + step, ignore_monsters, ENT(pev), &tr);
		if (tr.flFraction < 1.0f)
			return false;
		UTIL_TraceLine(knee + step, knee + step + Vector(0, 0, -60.0f), ignore_monsters, ENT(pev), &tr);
		return tr.flFraction < 1.0f;
	};
	if (!room(dir))
	{
		dir = -dir;
		if (!room(dir))
		{
			m_lsSpaceCheckAt = now + RANDOM_FLOAT(1.5f, 3.0f);
			return false;
		}
	}
	m_lsSpaceDir = dir;
	m_lsSpaceUntil = now + RANDOM_FLOAT(0.35f, 0.7f);
	// (and not straight into another step: a moment to settle first)
	m_lsSpaceCheckAt = m_lsSpaceUntil + 1.0f;
	LsWhy("spacing out");
	if (m_lsSpaceDir > 0)
		StrafeRight();
	else
		StrafeLeft();
	return true;
}

// How likely it gets out of sight to reload with an enemy about: a good player nearly always,
// a newer one often just stands there reloading; the careful more, the bold less
float CCSBot::LsReloadCoverChance()
{
	float chance = 0.15f + 0.75f * LsSkill();
	int style = LsStyle();
	if (style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER || style == LS_STYLE_AWPER)
		chance *= 1.2f;
	else if (style == LS_STYLE_RUSHER || style == LS_STYLE_KNIFER)
		chance *= 0.6f;
	return Q_min(0.97f, chance);
}

// Only a bit of someone showing past cover (the body points the vision checks all hidden): the
// gun sticking out, a shoulder, a knee, a foot. People notice those now and then - the better and
// the watchful more, the newer ones mostly not, a small bit less than a big one, less when it's
// far off or out at the edge of the view. Noticed, it knows someone's there: the eyes go to it
// (the edge they'd peek from), and one with the wallbang habit and a gun that goes through may
// spray the cover where they must be. A few points a look (the gun and two others), not all
void CCSBot::LsBarrelCheck(CBasePlayer *pPlayer)
{
	// (nor a bot a plugin is steering: a steered mode keeps its own view)
	if (IsBlind() || IsAttacking() || LsPuppetActive() || !pPlayer->IsAlive() || !IsEnemy(pPlayer))
		return;
	// (not over something it's already turned to look at for a reason)
	if (m_lookAtSpotState != NOT_LOOKING_AT_SPOT && m_lookAtSpotPriority >= PRIORITY_HIGH)
		return;
	float now = gpGlobals->time;
	int idx = pPlayer->entindex() - 1;
	if (idx < 0 || idx >= MAX_CLIENTS || now < m_lsBarrelCheckAt[idx])
		return;
	m_lsBarrelCheckAt[idx] = now + 0.3f;

	// the bits: (where, how easy to notice, what it's called)
	struct Bit { Vector at; float size; const char *why; const char *look; };
	Bit bits[3];
	int n = 0;
	CBasePlayerItem *pGun = pPlayer->m_pActiveItem;
	float len = 0.0f;
	switch (pGun ? WeaponIDToWeaponClass(pGun->m_iId) : WEAPONCLASS_NONE)
	{
	case WEAPONCLASS_PISTOL: len = 18.0f; break;
	// (no longer than the gun a player sees: a longer point would poke out through a thin door)
	case WEAPONCLASS_SNIPERRIFLE: case WEAPONCLASS_RIFLE: case WEAPONCLASS_MACHINEGUN: case WEAPONCLASS_SHOTGUN: len = 28.0f; break;
	case WEAPONCLASS_SUBMACHINEGUN: len = 24.0f; break;
	default: break;
	}
	Vector theirEye = pPlayer->pev->origin + pPlayer->pev->view_ofs;
	if (len > 0.0f)
	{
		Vector fwd;
		UTIL_MakeVectorsPrivate(pPlayer->pev->v_angle, fwd, nullptr, nullptr);
		bits[n++] = { theirEye + fwd * len + Vector(0, 0, -6.0f), 1.0f, "a gun sticking out", "Loner-Strike: a gun sticking out" };
	}
	// (the body's edges as the model shows them, across its line of sight: shoulders, knees, feet
	// - lower when crouched; inside the hull's edge, which is wider than the model)
	Vector2D dir = (pPlayer->pev->origin - pev->origin).Make2D();
	dir.NormalizeInPlace();
	Vector side(-dir.y, dir.x, 0.0f);
	bool ducked = (pPlayer->pev->flags & FL_DUCKING) != 0;
	const Vector &o = pPlayer->pev->origin;
	float shoulderZ = ducked ? 6.0f : 14.0f, kneeZ = ducked ? -6.0f : -16.0f, footZ = ducked ? -14.0f : -32.0f;
	for (int k = 0; k < 2; k++)
	{
		int pick = RANDOM_LONG(0, 5);
		float s = (pick & 1) ? 1.0f : -1.0f;
		switch (pick / 2)
		{
		case 0: bits[n++] = { o + side * (11.0f * s) + Vector(0, 0, shoulderZ), 1.0f, "a shoulder showing", "Loner-Strike: a shoulder showing" }; break;
		case 1: bits[n++] = { o + side * (7.0f * s) + Vector(0, 0, kneeZ), 0.6f, "a knee showing", "Loner-Strike: a knee showing" }; break;
		default: bits[n++] = { o + side * (7.0f * s) + Vector(0, 0, footZ), 0.4f, "a foot showing", "Loner-Strike: a foot showing" }; break;
		}
	}

	Vector eye = pev->origin + pev->view_ofs;
	Vector look;
	UTIL_MakeVectorsPrivate(pev->v_angle, look, nullptr, nullptr);
	float skill = LsSkill();
	int style = LsStyle();
	float lean = (style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER || style == LS_STYLE_AWPER) ? 1.3f
		: (style == LS_STYLE_RUSHER || style == LS_STYLE_KNIFER) ? 0.75f : 1.0f;
	for (int k = 0; k < n; k++)
	{
		const Bit &bit = bits[k];
		float dist = float((bit.at - eye).Length());
		if (dist > 2500.0f || !IsVisible(&bit.at, CHECK_FOV))
			continue;
		// (how far from where it's looking)
		float cosA = float(DotProduct((bit.at - eye) * (1.0f / Q_max(1.0f, dist)), look));
		float p = (0.02f + 0.3f * skill * skill) * bit.size * lean;
		p *= cosA > 0.978f ? 1.0f : cosA > 0.866f ? 0.45f : 0.15f;		// within 12 deg, 30 deg, the rest
		p *= Q_max(0.25f, 1.0f - Q_max(0.0f, dist - 600.0f) / 2000.0f);
		if (RANDOM_FLOAT(0.0f, 1.0f) >= p)
			continue;
		// noticed: not again for this one for a moment
		m_lsBarrelCheckAt[idx] = now + RANDOM_FLOAT(3.0f, 5.0f);
		// (the eyes on that spot at head-ish height, where they'd come out: not on a foot on the floor)
		Vector at(bit.at.x, bit.at.y, Q_max(bit.at.z, o.z + shoulderZ));
		SetLookAt(bit.look, &at, PRIORITY_HIGH, RANDOM_FLOAT(1.5f, 3.0f));
		LsWhy(bit.why);
		// (where they must be: a guess from what showed - closer for the better ones, and from a
		// gun or a shoulder than from a foot)
		float err = (8.0f + 24.0f * (1.0f - skill)) / Q_max(0.5f, bit.size);
		Vector body = (k == 0 && len > 0.0f ? theirEye : o + Vector(0, 0, ducked ? 4.0f : 10.0f))
			+ Vector(RANDOM_FLOAT(-err, err), RANDOM_FLOAT(-err, err), RANDOM_FLOAT(-6.0f, 6.0f));
		if (LsFree() && LsTrait(32) <= 0.175f + 0.575f * skill
			&& RANDOM_FLOAT(0.0f, 1.0f) < (0.2f + 0.35f * skill) * (0.7f + 0.6f * LsAggro()))
			LsBangThrough(body);
		return;
	}
}

// How likely this bot crouches to hold in a fight (most never do, a few always try)
float CCSBot::LsCrouchFactor()
{
	// (newer players crouch in every fight, better ones only now and then)
	float t = LsTrait(15) + 0.3f * (0.5f - LsSkill());
	return t < 0.5f ? 0.3f : t < 0.9f ? 1.0f : 2.0f;
}

// Patience before a fight counts as "pinned down": its own, and more while it's winning
float CCSBot::LsPinnedDownTime()
{
	return 5.0f + 9.0f * LsTrait(16);
}

// Two habits of real players in a fight, for the bots that have them:
//  - a crouch for a moment while shooting, then up again (not a crouch for the whole fight)
//  - a jump to look round a corner at an enemy it just lost sight of
void CCSBot::LsFightHabits(float range, bool crouchAndHold)
{
	float now = gpGlobals->time;

	if (m_lsCrouchBurstUntil > 0.0f && now >= m_lsCrouchBurstUntil)
	{
		m_lsCrouchBurstUntil = 0.0f;
		if (!crouchAndHold)
			StandUp();
	}
	else if (m_lsCrouchBurstUntil > 0.0f)
	{
		Crouch();
		return;
	}

	if (now < m_lsNextHabit || IsUsingKnife() || IsUsingSniperRifle())
		return;
	m_lsNextHabit = now + RANDOM_FLOAT(0.8f, 1.6f);

	if (IsEnemyVisible())
	{
		// crouch while shooting: close and mid range, the bots with the habit
		if (!crouchAndHold && range < 900.0f && LsCrouchFactor() >= 1.0f && RANDOM_FLOAT(0.0f, 1.0f) < 0.3f && (pev->flags & FL_ONGROUND))
		{
			m_lsCrouchBurstUntil = now + LsCrouchTapTime();	// (mostly a tap: cs_bot_ls_moves.cpp)
			Crouch();
		}
	}
	else if (GetTimeSinceLastSawEnemy() < 3.0f && !IsNotMoving() && (pev->flags & FL_ONGROUND))
	{
		// a jump to look round the corner they went behind
		if (LsTrait(18) > 0.45f && RANDOM_FLOAT(0.0f, 1.0f) < 0.4f && !LsNarrowFooting())
			Jump(MUST_JUMP);
	}
}

// A rogue who leaves the objective to others. On objective maps, going your own way
// (bot_ls_solo) doesn't mean ignoring the bomb: most still guard a planted bomb, cover a
// teammate defusing, hold the site they're at and answer a hostage; a few never do
bool CCSBot::LsShirksObjective()
{
	if (IsCarryingBomb() || (LsFight() && LsStyle() == LS_STYLE_OBJECTIVE) || !IsRogue())
		return false;
	if (cv_bot_ls_solo.value <= 0.0f || LsSolo())
		return true;
	return LsTrait(19) >= 0.7f;
}

// ---- moving like a player (bot_ls_fight) ------------------------------------------------

// A corner coming up: look into the opening past it (head height), so the bot strafes round
// it with its view on where someone would be, the way players peek with A and D. Newer
// players walk straight in looking ahead; nearly everyone else peeks, a few only now and then
bool CCSBot::LsCornerPeek()
{
	if (!LsFight() || IsSafe() || IsUsingLadder() || IsOnLadder() || IsAttacking())
		return false;

	// (decided once for each corner, not every frame near it)
	if ((m_goalPosition - m_lsPeekCorner).IsLengthGreaterThan(32.0f))
	{
		m_lsPeekCorner = m_goalPosition;
		float skill = LsSkill();
		float chance = Q_min(0.95f, 0.25f + 0.7f * skill + 0.25f * (LsTrait(20) - 0.5f));
		// deathmatch: mostly running round corners; a match: careful
		if (LsSolo())
			chance *= 0.45f;
		else if (LsMatch())
			chance = Q_min(0.98f, chance + 0.2f);
		m_lsPeekThis = RANDOM_FLOAT(0.0f, 1.0f) <= chance;
	}
	if (!m_lsPeekThis)
		return false;

	// the first path point well past the corner
	const Vector *corner = &m_goalPosition;
	for (int i = m_pathIndex + 1; i < m_pathLength; i++)
	{
		const Vector &p = m_path[i].pos;
		if ((p - *corner).IsLengthGreaterThan(220.0f))
		{
			Vector at = p;
			at.z += HalfHumanHeight + 24.0f;	// about where a head is
			SetLookAt(LS_PEEK, &at, PRIORITY_LOW, RANDOM_FLOAT(0.6f, 1.2f), true);
			return true;
		}
	}
	return false;
}

// Shift-walking where an enemy could hear it: the sneaky ones (lurkers, careful players)
// when it's gone quiet and they're somewhere they've felt danger, or late in the round on
// the way somewhere; back to running once seen, hurt, in a hurry or with the bomb down
void CCSBot::LsSneak()
{
	if (!LsFight() || gpGlobals->time < m_lsNextSneakCheck)
		return;

	// deathmatch: nobody sneaks (it would only look odd), back to running
	if (LsSolo())
	{
		if (m_lsSneaking && !IsRunning())
			Run();
		m_lsSneaking = false;
		return;
	}
	m_lsNextSneakCheck = gpGlobals->time + RANDOM_FLOAT(0.8f, 1.5f);

	float sneaky = 0.6f * (1.0f - LsAggro()) + 0.4f * LsTrait(21);
	if (LsMatch())
		sneaky += 0.25f;
	bool sneakyCarrier = IsCarryingBomb() && (LsStyle() == LS_STYLE_LURKER || LsStyle() == LS_STYLE_NINJA || LsStyle() == LS_STYLE_CAREFUL)
		&& CSGameRules() && LsTimeLeft() > 40.0f;
	// (a ninja or lurker CT walks in to a planted bomb while there's time for it)
	bool quietRetake = m_iTeam == CT && TheCSBots() && TheCSBots()->IsBombPlanted() && (LsStyle() == LS_STYLE_NINJA || LsStyle() == LS_STYLE_LURKER)
		&& TheCSBots()->GetBombTimeLeft() > 15.0f + (m_bHasDefuser ? 5.0f : 10.0f);
	bool loud = IsHurrying() || LsRushing() || GetTimeSinceAttacked() < 5.0f || GetTimeSinceLastSawEnemy() < 6.0f || IsSafe()
		|| (TheCSBots() && TheCSBots()->IsBombPlanted() && !quietRetake) || (IsCarryingBomb() && !sneakyCarrier) || GetHostageEscortCount() > 0;

	bool want = false;
	if (!loud && sneaky > 0.45f)
	{
		float danger = (LsOwnMemory() && GetLastKnownArea()) ? LsGetDanger(GetLastKnownArea()) : 0.0f;
		// (one stretch of it, 20 s to a minute: in a long round it doesn't creep for minutes)
		float elapsed = TheCSBots() ? TheCSBots()->GetElapsedRoundTime() : 0.0f;
		int round = LsRoundSerial();
		if (m_lsSneakRound != round)
		{
			m_lsSneakRound = round;
			m_lsSneakLate = RANDOM_FLOAT(0.0f, 1.0f) < sneaky;
			m_lsSneakLateEnd = 25.0f + RANDOM_FLOAT(20.0f, 60.0f);
		}
		bool lateRound = elapsed > 25.0f && elapsed < m_lsSneakLateEnd;
		// (round about when the enemy could first be here - the first contact - a careful
		// player goes quiet; one who knows the timings, not on its way in with the team)
		bool contact = false;
		if (GetLastKnownArea() && LsKnows(LSK_TIMINGS) && !LsRushing())
		{
			float arrive = LsArrival(GetLastKnownArea(), m_iTeam == CT ? TERRORIST : CT);
			// (only somewhere it takes them a while to reach: not by its own spawn)
			contact = arrive > 10.0f && elapsed > arrive - 2.0f && elapsed < arrive + 10.0f;
		}
		// (a noise close by and fresh, not gunfire across the map)
		bool noiseClose = IsNoiseHeard() && gpGlobals->time - m_noiseTimestamp < 6.0f && (m_noisePosition - pev->origin).IsLengthLessThan(1200.0f);
		want = danger > 0.1f || (lateRound && m_lsSneakLate) || noiseClose || contact;
		// (nobody creeps out of their own spawn: nothing's near yet, unless it's heard close)
		const Vector *home = LsEnemySpawn(m_iTeam == CT ? TERRORIST : CT);
		if (want && home && (*home - pev->origin).IsLengthLessThan(1000.0f) && !noiseClose)
			want = false;
	}
	// the last one alive against some of them, with time for it: quiet (a clutch is won by
	// not being heard), whoever it is, once it's not a newer player
	if (!want && !(IsHurrying() || GetTimeSinceAttacked() < 5.0f || GetTimeSinceLastSawEnemy() < 4.0f || IsSafe() || GetHostageEscortCount() > 0)
		&& LsSkill() > 0.35f && (m_iTeam == CT || m_iTeam == TERRORIST) && LsAlive(m_iTeam) == 1 && LsAlive(m_iTeam == CT ? TERRORIST : CT) >= 1
		&& LsTimeLeft() > 30.0f && !(m_iTeam == CT && TheCSBots()->IsBombPlanted() && !quietRetake))
		want = true;

	// (only its own walk is undone: a state may want walking for its own reasons)
	if (want && IsRunning())
	{
		Walk();
		m_lsSneaking = true;
	}
	else if (!want && m_lsSneaking)
	{
		if (!IsRunning())
			Run();
		m_lsSneaking = false;
	}
}

// Stopping to shoot: a tap of the opposite key (70-120 ms) stops dead, as players do; the
// careless just let go. A crouch tap on the stop for some (first-shot accuracy)
bool CCSBot::LsCounterStrafe(int fromState)
{
	if (!LsFight())
		return false;
	float skill = LsSkill();
	if (RANDOM_FLOAT(0.0f, 1.0f) > 0.2f + 0.8f * skill)
		return false;

	m_lsCounterDir = (fromState == 1) ? -1 : 1;		// slid left: tap right
	m_lsCounterUntil = gpGlobals->time + RANDOM_FLOAT(0.07f, 0.12f);
	if (LsTrait(22) > 0.5f && m_lsCrouchBurstUntil <= gpGlobals->time && (pev->flags & FL_ONGROUND))
	{
		m_lsCrouchBurstUntil = gpGlobals->time + RANDOM_FLOAT(0.2f, 0.45f);
		Crouch();
	}
	return true;
}

// A counter-strafe: the keys against its own velocity (forward/back and sideways), so it
// stops dead in about a tenth of a second instead of sliding for a third of one
void CCSBot::LsStopDead()
{
	float yaw = pev->v_angle.y;
	Vector2D dir(BotCOS(yaw), BotSIN(yaw));
	Vector2D lat(-dir.y, dir.x);
	float fwd = pev->velocity.x * dir.x + pev->velocity.y * dir.y;
	float side = pev->velocity.x * lat.x + pev->velocity.y * lat.y;

	const float slow = 40.0f;
	if (fwd > slow)
		MoveBackward();
	else if (fwd < -slow)
		MoveForward();

	if (side > slow)
		StrafeRight();
	else if (side < -slow)
		StrafeLeft();
}

// ---- habits by skill: careless ones for newer players, disciplined ones for better ------
// (each still the bot's own: some newbies are tidy, some good players have a bad habit)

// Sloppy: reloads after every kill, whatever's around; good players reload when it's low
float CCSBot::LsReloadRatio()
{
	float skill = LsSkill();
	if (skill < 0.5f && LsTrait(24) < 0.65f && !LsMatch())
		return 0.97f;
	if (skill > 0.65f || LsMatch())
		return 0.4f;
	return 0.6f;
}

float CCSBot::LsReloadWait()
{
	float skill = LsSkill();
	return (skill < 0.5f && LsTrait(24) < 0.65f && !LsMatch()) ? 1.0f : 3.0f;
}

// Aim settles with a small overshoot like a flick; newer players are twitchier
float CCSBot::LsAimDamping()
{
	float skill = LsSkill();
	return 0.6f + 0.25f * skill + 0.1f * (LsTrait(25) - 0.5f);
}

// Where the crosshair rests while moving: head height for good players, lower (chest,
// the floor) for newer ones
float CCSBot::LsLookHeight()
{
	float skill = LsSkill();
	return -14.0f + 42.0f * skill + 8.0f * (LsTrait(26) - 0.5f);
}

// The heading eases round to where it's going (people turn the mouse, they don't snap)
float CCSBot::LsSmoothYaw(float target)
{
	// (picks up from where it's actually looking after looking at something else)
	if (gpGlobals->time - m_lsYawAt > 0.2f)
		m_lsYaw = pev->v_angle.y;
	m_lsYawAt = gpGlobals->time;
	float tau = 0.12f + 0.25f * LsTrait(27);
	float dt = g_flBotCommandInterval;
	float k = 1.0f - expf(-dt / tau);
	m_lsYaw = NormalizeAngle(m_lsYaw + NormalizeAngle(target - m_lsYaw) * k);
	return m_lsYaw;
}

// Running straight at them: SMGs and shotguns full send (the eager ones), and newer
// players who just hold W
bool CCSBot::LsPushes(float range)
{
	if (range < 250.0f || IsUsingSniperRifle())
		return false;
	float skill = LsSkill();
	float aggro = LsAggro();
	CBasePlayerWeapon *pWeapon = GetActiveWeapon();
	bool closeGun = IsUsingShotgun() || (pWeapon && (pWeapon->m_iId == WEAPON_MAC10 || pWeapon->m_iId == WEAPON_TMP
		|| pWeapon->m_iId == WEAPON_P90 || pWeapon->m_iId == WEAPON_UMP45 || pWeapon->m_iId == WEAPON_MP5N));
	if (closeGun && aggro > 0.55f && range < 900.0f)
		return RANDOM_FLOAT(0.0f, 1.0f) < 0.6f;
	if (!LsMatch() && skill < 0.45f && aggro > 0.4f && LsTrait(28) < 0.6f)
		return RANDOM_FLOAT(0.0f, 1.0f) < 0.4f;
	return false;
}

// The eager ones chase a kill they lost sight of (the "chasing an eco" mistake)
float CCSBot::LsChaseExtra()
{
	float aggro = LsAggro();
	if (LsMatch())
		return 0.0f;
	return (aggro > 0.65f && LsTrait(29) < 0.6f && LsSkill() < 0.7f) ? RANDOM_FLOAT(1.0f, 2.0f) : 0.0f;
}

// Testing (bot_ls_trace): twice a second, what each bot is doing and why, so a recording
// of where the bots went can be explained (time,id,team,state,task,enemy,leader,goal x,y,
// noise x,y,why,seconds since why, then speed, view off its heading, attacking, what it's
// looking at)
// The trace file (bot_ls_trace), opened once; a line naming the map whenever it changes (the
// clock restarts with each map), and the columns once per opening
static FILE *LsTraceFile()
{
	static FILE *s_file = nullptr;
	if (!s_file)
	{
		s_file = fopen("lstrace.csv", "a");
		if (!s_file)
			return nullptr;
		fprintf(s_file, "#cols,time,id,team,state,task,enemy,leader,goal_x,goal_y,noise_x,noise_y,why,why_ago,speed,view_off,attacking,look,x,y,z,hp,weapon,crouched,pitch\n");
		fprintf(s_file, "#cols_why,time,id,team,x,y,z,why\n");
		fprintf(s_file, "#cols_attack,time,id,team,enemy,since_first_saw,range\n");
		fprintf(s_file, "#cols_seen,time,id,team,enemy,range\n");
	}
	static char s_map[64] = "";
	static float s_last = 0.0f;
	if (Q_strcmp(s_map, STRING(gpGlobals->mapname)) != 0 || gpGlobals->time < s_last)
	{
		Q_strlcpy(s_map, STRING(gpGlobals->mapname));
		fprintf(s_file, "#map,%s\n", s_map);
	}
	s_last = gpGlobals->time;
	return s_file;
}

// A decision: its label, and with bot_ls_trace on a line the moment it's made (a quick one -
// a flash turn, a runner glance - would otherwise be gone before the next half-second line)
void CCSBot::LsWhy(const char *why)
{
	bool changed = why != m_lsWhy || gpGlobals->time - m_lsWhyAt > 1.0f;
	m_lsWhy = why;
	m_lsWhyAt = gpGlobals->time;
	if (!changed || cv_bot_ls_trace.value <= 0.0f || !why || !why[0])
		return;
	FILE *f = LsTraceFile();
	if (!f)
		return;
	fprintf(f, "#why,%.2f,%d,%d,%.0f,%.0f,%.0f,%s\n", gpGlobals->time, entindex(), m_iTeam, pev->origin.x, pev->origin.y, pev->origin.z, why);
	fflush(f);
}

// A line in the trace (as a #why) for something it did on the side, without making it the
// reason for what it's doing now (the per-bot rows keep showing that)
void CCSBot::LsTraceNote(const char *note)
{
	if (cv_bot_ls_trace.value <= 0.0f || !note || !note[0])
		return;
	FILE *f = LsTraceFile();
	if (!f)
		return;
	fprintf(f, "#why,%.2f,%d,%d,%.0f,%.0f,%.0f,%s\n", gpGlobals->time, entindex(), m_iTeam, pev->origin.x, pev->origin.y, pev->origin.z, note);
	fflush(f);
}

// ls_mark [note]: a moment the person playing flagged (a key bound to it): found again later
// among the bots' lines around that time
void CCSBot::LsTraceAttack()
{
	if (cv_bot_ls_trace.value <= 0.0f)
		return;
	FILE *f = LsTraceFile();
	CBasePlayer *pEnemy = GetEnemy();
	if (!f || !pEnemy)
		return;
	fprintf(f, "#attack,%.2f,%d,%d,%d,%.2f,%.0f\n", gpGlobals->time, entindex(), m_iTeam, pEnemy->entindex(),
		gpGlobals->time - m_firstSawEnemyTimestamp, float((pEnemy->pev->origin - pev->origin).Length()));
	fflush(f);
}

void LsTraceMark()
{
	FILE *f = LsTraceFile();
	if (!f)
	{
		CONSOLE_ECHO("ls_mark: can't write lstrace.csv\n");
		return;
	}
	char note[128] = "";
	for (int i = 1; i < CMD_ARGC() && Q_strlen(note) < 100; i++)
	{
		if (i > 1)
			Q_strlcat(note, " ");
		Q_strlcat(note, CMD_ARGV(i));
	}
	for (char *c = note; *c; c++)
		if (*c == ',' || *c == '\n' || *c == '\r')
			*c = ' ';
	fprintf(f, "#mark,%.2f,%s,%s\n", gpGlobals->time, STRING(gpGlobals->mapname), note);
	fflush(f);
	CONSOLE_ECHO("Marked at %.1f%s%s\n", gpGlobals->time, note[0] ? ": " : "", note);
}

void CCSBot::LsTrace()
{
	if (cv_bot_ls_trace.value <= 0.0f || !IsAlive() || gpGlobals->time < m_lsTraceAt)
		return;
	m_lsTraceAt = gpGlobals->time + 0.5f;
	FILE *s_file = LsTraceFile();
	if (!s_file)
		return;
	CBasePlayer *pEnemy = GetEnemy();
	CBasePlayer *pLeader = IsFollowing() ? GetFollowLeader() : nullptr;
	Vector goal = HasPath() ? GetPathEndpoint() : Vector(0, 0, 0);
	const Vector *noise = IsNoiseHeard() ? &m_noisePosition : nullptr;
	float speed = float(pev->velocity.Length2D());
	float off = speed > 1.0f ? NormalizeAngle(pev->v_angle.y - UTIL_VecToYaw(pev->velocity)) : 0.0f;
	const char *look = (m_lookAtSpotState != NOT_LOOKING_AT_SPOT && m_lookAtDesc) ? m_lookAtDesc : "-";
	CBasePlayerItem *pItem = m_pActiveItem;
	fprintf(s_file, "%.2f,%d,%d,%s,%d,%d,%d,%.0f,%.0f,%.0f,%.0f,%s,%.1f,%.0f,%.0f,%d,%s,%.0f,%.0f,%.0f,%.0f,%d,%d,%.0f\n",
		gpGlobals->time, entindex(), m_iTeam, m_state ? m_state->GetName() : "-", int(GetTask()),
		pEnemy ? pEnemy->entindex() : 0, pLeader ? pLeader->entindex() : 0,
		goal.x, goal.y, noise ? noise->x : 0.0f, noise ? noise->y : 0.0f,
		m_lsWhy, m_lsWhy[0] ? gpGlobals->time - m_lsWhyAt : 0.0f,
		speed, off, IsAttacking() ? 1 : 0, look,
		pev->origin.x, pev->origin.y, pev->origin.z, pev->health, pItem ? pItem->m_iId : 0, (pev->flags & FL_DUCKING) ? 1 : 0, pev->v_angle.x);
	fflush(s_file);
}

// An enemy's bullet landed close by, or passed close by on its way to the wall behind
// (EVENT_BULLET_IMPACT): people notice being shot at. One it can see is fought; otherwise
// it turns towards where the shots came from and treats it as a loud noise there.
void CCSBot::LsShotAt(CBasePlayer *pShooter, const Vector &impact)
{
	if (!IsAlive() || IsBlind() || !pShooter->IsPlayer() || pShooter == this || !pShooter->IsAlive() || !IsEnemy(pShooter))
		return;
	bool busy = IsAttacking() && IsEnemyVisible();
	if (busy && (pShooter == GetEnemy() || gpGlobals->time - m_lsShotAtAt < 0.8f))
		return;

	Vector eye = pev->origin + pev->view_ofs;
	Vector from = pShooter->pev->origin + pShooter->pev->view_ofs;
	// how close the bullet's line came to its head
	Vector line = impact - from;
	float len = float(line.Length());
	if (len < 1.0f)
		return;
	float t = Q_max(0.0f, Q_min(1.0f, float(DotProduct(eye - from, line)) / (len * len)));
	float miss = (from + line * t - eye).Length();
	if (miss > 90.0f && (impact - eye).Length() > 110.0f)
		return;

	// in a fight with someone else and a second one's bullets are hitting it: now and then
	// (the better ones more often) it turns to them, if it can see them; else it carries on
	if (busy)
	{
		m_lsShotAtAt = gpGlobals->time;
		m_lsShotBy = pShooter;
		m_lsShotFrom = pShooter->pev->origin;
		if (miss < 30.0f && IsVisible(pShooter, false) && RANDOM_FLOAT(0.0f, 1.0f) < 0.25f + 0.4f * LsSkill())
		{
			Attack(pShooter);
			LsWhy("second-shooter");
		}
		return;
	}

	// once a moment, not every bullet
	if (gpGlobals->time - m_lsShotAtAt < 0.8f)
		return;
	m_lsShotAtAt = gpGlobals->time;
	// (shot at: no hold keeps it standing there, a person's "wait" included)
	if (LsPuppetActive())
		LsPuppetRelease();
	m_lsShotBy = pShooter;
	{
		float away = float((pShooter->pev->origin - pev->origin).Length());
		float err = away * (0.12f - 0.08f * LsSkill());
		m_lsShotFrom = pShooter->pev->origin + Vector(RANDOM_FLOAT(-err, err), RANDOM_FLOAT(-err, err), 0.0f);
	}

	// planting or defusing: whether to stop for it is weighed there (LsObjectiveThreat); on
	// a ladder: it notes where (no fighting from a ladder)
	if (m_state == &m_plantBombState || m_state == &m_defuseBombState)
		return;
	from = m_lsShotFrom + Vector(0, 0, HalfHumanHeight);

	// running for cover or saving its gun: it notices (looks), but keeps going; and it only
	// takes the fight to a shooter in front of it (not with eyes in the back of its head)
	bool keepsGoing = GetDisposition() == IGNORE_ENEMIES || m_lsSaving || IsUsingLadder();
	if (!keepsGoing && IsVisible(pShooter, true))
	{
		Attack(pShooter);
		return;
	}
	SetLookAt("Loner-Strike: shot at", &from, PRIORITY_HIGH, RANDOM_FLOAT(1.0f, 2.0f));
	// shot at from somewhere it can't see (an AWP down a lane most of all): the better ones get
	// out of that line first, and come back by another way (the spot it was in is avoided a
	// while); newer ones turn and look about in the open. Less in deathmatch, and rushers less
	if (!keepsGoing && !IsAttacking())
	{
		CBasePlayerWeapon *pGun = static_cast<CBasePlayerWeapon *>(pShooter->m_pActiveItem);
		bool sniper = pGun && (pGun->m_iId == WEAPON_AWP || pGun->m_iId == WEAPON_SCOUT || pGun->m_iId == WEAPON_G3SG1 || pGun->m_iId == WEAPON_SG550);
		// (or the one it heard scoping lately, whatever's in their hands now)
		CBasePlayer *pSniper = m_lsSniperHeard;
		if (pSniper == pShooter && gpGlobals->time - m_lsSniperHeardAt < 20.0f)
			sniper = true;
		// (never every time: good players sometimes swing straight back to trade)
		float odds = Q_min(0.75f, 0.1f + 0.65f * LsSkill() + (sniper ? 0.15f : 0.0f)) * (!LsSolo() ? 1.0f : 0.5f) * (LsStyle() == LS_STYLE_RUSHER ? 0.6f : 1.0f);
		if (RANDOM_FLOAT(0.0f, 1.0f) < odds && LsTakeCoverFrom(m_lsShotFrom, 64.0f))
		{
			if (sniper && m_lastKnownArea)
				LsForceBadArea(m_lastKnownArea, 8.0f + 10.0f * LsSkill());
		}
	}
	CNavArea *area = TheNavAreaGrid.GetNavArea(&m_lsShotFrom);
	if (!area)
		area = TheNavAreaGrid.GetNearestNavArea(&m_lsShotFrom);
	if (area)
	{
		m_noisePosition = m_lsShotFrom;
		m_noiseArea = area;
		m_noisePriority = PRIORITY_HIGH;
		m_noiseTimestamp = gpGlobals->time;
	}
	// no sneaking while being shot at
	if (!IsRunning())
	{
		Run();
		m_lsSneaking = false;
	}
}

// Planting or defusing with a threat around: hurt, shot at, an enemy in sight or heard close
// by. Each threat is weighed once: whether to stop for it depends on how bad it is, the bot
// (bold ones risk it more) and how far along it is (nearly done: it keeps going). Stopping:
// an enemy it can see is fought; otherwise it lets go and looks that way for a moment, then
// starts over (as a player who let go of the key would). True while it's stopped.
bool CCSBot::LsObjectiveThreat(bool defusing)
{
	if (!LsFight())
		return false;
	if (gpGlobals->time < m_lsCheckUntil)
		return true;

	float now = gpGlobals->time;
	float threat = 0.0f;
	CBasePlayer *pEnemy = nullptr;
	const Vector *where = nullptr;
	if (GetTimeSinceAttacked() < 0.4f && GetAttacker() && GetAttacker()->IsAlive())
	{
		threat = 0.8f;
		pEnemy = GetAttacker();
		where = &pEnemy->pev->origin;
	}
	else if (now - m_lsShotAtAt < 0.4f)
	{
		threat = 0.6f;
		where = &m_lsShotFrom;
		CBasePlayer *pShooter = m_lsShotBy;
		if (pShooter && pShooter->IsAlive())
			pEnemy = pShooter;
	}
	else if (IsNoiseHeard() && GetNoisePriority() >= PRIORITY_MEDIUM && (m_noisePosition - pev->origin).IsLengthLessThan(700.0f))
	{
		threat = 0.35f;
		where = &m_noisePosition;
	}
	else if (IsNoiseHeard() && (m_noisePosition - pev->origin).IsLengthLessThan(500.0f))
	{
		// (footsteps close by)
		threat = 0.25f;
		where = &m_noisePosition;
	}
	if (threat <= 0.0f)
		return false;
	// a threat it already decided about (a noise: at most once every few seconds; footsteps
	// keep refreshing it)
	float stamp = Q_max(GetTimeSinceAttacked() < 0.4f ? now : -1.0f, Q_max(m_lsShotAtAt, m_noiseTimestamp));
	if (stamp <= m_lsObjRolledAt + (threat < 0.5f ? 3.0f : 0.5f))
		return false;
	m_lsObjRolledAt = stamp;

	// how far along: planting takes 3 s, defusing 5 with a kit and 10 without
	float takes = defusing ? (m_bHasDefuser ? 5.0f : 10.0f) : 3.0f;
	float done = (now - Q_max(GetStateTimestamp(), m_lsObjUseAt)) / takes;
	if (done > 0.75f)
	{
		// nearly done: it finishes, whatever turns up (the stock self-defence would stop it)
		if (GetDisposition() != IGNORE_ENEMIES)
			SetDisposition(IGNORE_ENEMIES);
		return false;
	}
	// the bomb about to go: nothing else matters (a stop costs up to 2.5 s); and twice is
	// enough for one plant or defuse
	if (defusing && TheCSBots()->GetBombTimeLeft() < takes + 3.5f)
		return false;
	// (likewise the round about to run out on a plant)
	if (!defusing && CSGameRules() && LsTimeLeft() < takes + 3.5f)
		return false;
	if (m_lsObjStops >= 2)
		return false;
	float bold = 0.5f * LsAggro() + 0.5f * LsTrait(30);
	float stop = threat * (1.2f - 0.6f * bold) * (LsMatch() ? 1.1f : 1.0f);
	if (RANDOM_FLOAT(0.0f, 1.0f) > stop)
		return false;

	if (pEnemy && IsVisible(pEnemy, false))
	{
		StandUp();
		Attack(pEnemy);
		return true;
	}
	StandUp();
	Vector look = *where + Vector(0, 0, HalfHumanHeight);
	float secs = RANDOM_FLOAT(1.0f, 2.5f);
	SetLookAt("Loner-Strike: checking", &look, PRIORITY_HIGH, secs);
	m_lsCheckUntil = now + secs;
	m_lsObjUseAt = m_lsCheckUntil;
	m_lsObjStops++;
	return true;
}

// Whether a radio message goes out (bot_ls_fight): CS 1.6 turns every bit of the bots' chatter
// into a radio command, which is far more than people use. In deathmatch nobody radios; on a
// pub a bot radios now and then (the useful calls more than "follow me" and "regroup"), and
// not straight after its last one or a teammate's same call; a match is chattier
bool CCSBot::LsRadioOk(GameEventType event)
{
	if (!LsFight())
		return true;
	if (LsSolo())
		return false;
	float now = gpGlobals->time;
	if (now - m_lastRadioSentTimestamp < (LsMatch() ? 8.0f : 20.0f))
		return false;
	if (TheCSBots()->GetRadioMessageInterval(event, m_iTeam) < 15.0f)
		return false;
	float chance;
	switch (event)
	{
	case EVENT_RADIO_ENEMY_SPOTTED:
	case EVENT_RADIO_NEED_BACKUP:
	case EVENT_RADIO_TAKING_FIRE:
	case EVENT_RADIO_ENEMY_DOWN:
		chance = LsMatch() ? 0.8f : 0.45f;
		break;
	default:
		chance = LsMatch() ? 0.6f : 0.2f;
		break;
	}
	return RANDOM_FLOAT(0.0f, 1.0f) < chance;
}

// Spraying through a wall (bot_ls_fight): while it's on (m_lsBangUntil), the view goes to the
// spot and the trigger is pulled once the gun points there. True while it's spraying.
bool CCSBot::LsBangTick()
{
	float now = gpGlobals->time;
	if (now >= m_lsBangUntil)
		return false;
	CBasePlayerWeapon *pWeapon = GetActiveWeapon();
	if (!IsAlive() || IsBlind() || !pWeapon || IsActiveWeaponClipEmpty() || !LsBangGun(pWeapon) || IsFriendInLineOfFire()
		|| (m_lsBangAt - pev->origin).IsLengthGreaterThan(1600.0f))
	{
		m_lsBangUntil = 0.0f;
		return false;
	}
	SetLookAt("Loner-Strike: wallbang", &m_lsBangAt, PRIORITY_HIGH, 0.3f);
	Vector dir = m_lsBangAt - (pev->origin + pev->view_ofs);
	dir.NormalizeInPlace();
	UTIL_MakeVectors(pev->v_angle + pev->punchangle);
	// (a semi-automatic needs the trigger let go between shots)
	bool semi = pWeapon->m_iId == WEAPON_DEAGLE || pWeapon->m_iId == WEAPON_AWP || pWeapon->m_iId == WEAPON_SCOUT;
	if (DotProduct(gpGlobals->v_forward, dir) > 0.996f && (!semi || LsTriggerTap()))
		PrimaryAttack();
	return true;
}

// A gun that goes through doors and thin walls: rifles, machine guns, snipers and the Deagle
bool CCSBot::LsBangGun(CBasePlayerWeapon *pWeapon) const
{
	switch (pWeapon ? pWeapon->m_iId : 0)
	{
	case WEAPON_AK47: case WEAPON_M4A1: case WEAPON_GALIL: case WEAPON_FAMAS: case WEAPON_SG552: case WEAPON_AUG:
	case WEAPON_M249: case WEAPON_DEAGLE: case WEAPON_AWP: case WEAPON_SCOUT: case WEAPON_G3SG1: case WEAPON_SG550:
		return true;
	}
	return false;
}

// The surface a line hits is made of (CHAR_TEX_*), as the game reads it for bullets: the
// server's own list for the map's texture names, whatever players have on their screens
static char LsSurfaceAt(const Vector &from, const Vector &to, edict_t *hit)
{
	float v1[3] = { from.x, from.y, from.z }, v2[3] = { to.x, to.y, to.z };
	const char *name = TRACE_TEXTURE(hit ? hit : ENT(0), v1, v2);
	if (!name)
		return CHAR_TEX_CONCRETE;
	if (*name == '-' || *name == '+')
		name += 2;
	if (*name == '{' || *name == '!' || *name == '~' || *name == ' ')
		name++;
	char buf[64];
	Q_strlcpy(buf, name);
	return TEXTURETYPE_Find(buf);
}

// Whether a shot from `from` at `to` with this gun reaches it through what's in between, by the
// game's penetration rule; `first` is where it first hits. Only worth it when there's damage
// left at the far side (a bullet through metal has a fifth of it)
bool LsBulletGetsThrough(CBasePlayer *shooter, int weaponId, const Vector &from, const Vector &to, TraceResult *first)
{
	float power;
	int pens;
	switch (weaponId)
	{
	case WEAPON_AWP: power = 45.0f; pens = 3; break;
	case WEAPON_SCOUT: case WEAPON_G3SG1: power = 39.0f; pens = 3; break;
	case WEAPON_AK47: power = 39.0f; pens = 2; break;
	case WEAPON_M4A1: case WEAPON_FAMAS: case WEAPON_GALIL: case WEAPON_SG552: case WEAPON_AUG: case WEAPON_SG550: case WEAPON_M249: power = 35.0f; pens = 2; break;
	case WEAPON_DEAGLE: power = 30.0f; pens = 2; break;
	default: return false;					// one penetration: it stops at the first wall
	}
	Vector dir = (to - from).Normalize();
	Vector src = from;
	float damage = 1.0f;
	edict_t *skip = ENT(shooter->pev);
	for (int wall = 0; wall < 3; wall++)
	{
		TraceResult tr;
		UTIL_TraceLine(src, to, ignore_monsters, skip, &tr);
		if (wall == 0 && first)
			*first = tr;
		if (tr.flFraction >= 1.0f && !tr.fStartSolid)
			return wall > 0 && damage >= 0.3f;
		if (tr.fStartSolid || pens <= 1)
			return false;
		pens--;
		float reach = power, mod = 0.5f;
		switch (LsSurfaceAt(src, to, tr.pHit))
		{
		case CHAR_TEX_METAL: reach *= 0.15f; mod = 0.2f; break;
		case CHAR_TEX_CONCRETE: reach *= 0.25f; break;
		case CHAR_TEX_GRATE: reach *= 0.5f; mod = 0.4f; break;
		case CHAR_TEX_VENT: reach *= 0.5f; mod = 0.45f; break;
		case CHAR_TEX_TILE: reach *= 0.65f; mod = 0.3f; break;
		case CHAR_TEX_COMPUTER: reach *= 0.4f; mod = 0.45f; break;
		case CHAR_TEX_WOOD: mod = 0.6f; break;
		default: break;
		}
		// (a door, a breakable or a box that isn't part of the map's walls: the game's 42)
		if (tr.pHit && VARS(tr.pHit)->solid != SOLID_BSP)
		{
			reach = 42.0f;
			mod = 0.75f;
		}
		Vector past = tr.vecEndPos + dir * reach;
		// (it jumps to there: still inside the wall, and that's where it stops)
		TraceResult inside;
		UTIL_TraceLine(past, past + dir * 1.0f, ignore_monsters, skip, &inside);
		if (inside.fStartSolid || inside.fAllSolid)
			return false;
		// (and not past the target)
		if (DotProduct(to - past, dir) < 0.0f)
			return damage * mod >= 0.3f;
		damage *= mod;
		src = past;
	}
	return false;
}

// Starts spraying at a spot behind one thin thing (a door, a box, a thin wall: where the
// bullet goes in and where it comes out are close) with no teammate in the way. False if
// it can't be shot through from here.
bool CCSBot::LsBangThrough(const Vector &target)
{
	CBasePlayerWeapon *pWeapon = GetActiveWeapon();
	if (!LsBangGun(pWeapon))
		return false;
	Vector eye = pev->origin + pev->view_ofs;
	if ((target - eye).IsLengthGreaterThan(1500.0f))
		return false;
	// (the game's own rule, CBaseEntity::FireBullets3: the bullet jumps on past where it hits by
	// its power, cut by what the surface is made of, and stops if that's still inside something;
	// each wall uses up one of the gun's penetrations and cuts the damage)
	TraceResult in, mates;
	if (!LsBulletGetsThrough(this, pWeapon->m_iId, eye, target, &in))
		return false;
	// (one who doesn't know the rules only tries the obvious: a door, a breakable, wood)
	if (!LsKnows(LSK_PENETRATION))
	{
		CBaseEntity *pHit = in.pHit ? CBaseEntity::Instance(in.pHit) : nullptr;
		bool obvious = (pHit && (FClassnameIs(pHit->pev, "func_door") || FClassnameIs(pHit->pev, "func_door_rotating") || FClassnameIs(pHit->pev, "func_breakable")))
			|| LsSurfaceAt(eye, target, in.pHit) == CHAR_TEX_WOOD;
		if (!obvious)
			return false;
	}
	// nobody on this side of the wall in the way, nor one of ours just past it
	UTIL_TraceLine(eye, in.vecEndPos, dont_ignore_monsters, ENT(pev), &mates);
	if (mates.flFraction < 1.0f && mates.pHit && CBaseEntity::Instance(mates.pHit) && CBaseEntity::Instance(mates.pHit)->IsPlayer())
		return false;
	bool mateBehind = false;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		if ((pPlayer->pev->origin - target).IsLengthLessThan(160.0f))
			mateBehind = true;
	});
	if (mateBehind)
		return false;

	bool slow = pWeapon->m_iId == WEAPON_AWP || pWeapon->m_iId == WEAPON_SCOUT || pWeapon->m_iId == WEAPON_DEAGLE;
	m_lsBangAt = target;
	m_lsBangUntil = gpGlobals->time + (slow ? RANDOM_FLOAT(0.3f, 0.8f) : RANDOM_FLOAT(0.6f, 1.5f));
	LsWhy("wallbang");
	return true;
}

// The enemy just went behind a door, a box or a thin wall a bullet goes through: some players
// spray it where they saw them go. Each bot either has the habit or not (more of the good
// ones); whether it does it this time is rolled once each time it loses sight of someone.
// True while it's spraying.
bool CCSBot::LsWallbang(float notSeenEnemyTime)
{
	if (!LsFight())
		return false;
	if (LsBangTick())
		return true;
	if (notSeenEnemyTime > 1.2f || GetLastSawEnemyTimestamp() == m_lsBangRolledAt)
		return false;
	m_lsBangRolledAt = GetLastSawEnemyTimestamp();
	float skill = LsSkill();
	// (the habit: most of the strong ones have it, few new players; and how often they act on it)
	if (LsTrait(32) > 0.175f + 0.575f * skill || RANDOM_FLOAT(0.0f, 1.0f) > 0.475f + 0.275f * skill)
		return false;
	// where they'd be by now, not where they were: a good player leads the spray along the way
	// they were going (a little, a newer player not at all)
	Vector at = GetLastKnownEnemyPosition();
	CBasePlayer *pEnemy = static_cast<CBasePlayer *>(GetEnemy());
	if (pEnemy && pEnemy->IsAlive())
	{
		Vector lead = pEnemy->pev->origin - at;
		lead.z = 0.0f;
		float most = 45.0f * skill;
		float len = float(lead.Length());
		if (len > most && len > 0.0f)
			lead = lead * (most / len);
		at = at + lead;
	}
	return LsBangThrough(at + Vector(0, 0, 10.0f));
}

// Something heard from an enemy it can't see (bot_ls_fight): footsteps, a reload, gunfire.
// - a reload close by: the bold go for them while they're reloading, the rest get ready
// - close footsteps: the view goes to where they'd come out (the first spot along the way
//   from the sound that it can see), at head height, like a player pre-aiming a corner
// - the good ones, knowing someone is right there behind a door or thin wall, now and then
//   spray it (with a gun that goes through)
void CCSBot::LsHeard(GameEventType event, CBasePlayer *pPlayer, float dist)
{
	if (!LsFight() || !pPlayer || !pPlayer->IsAlive() || !IsEnemy(pPlayer))
		return;
	float now = gpGlobals->time;
	float aggro = LsAggro();
	float skill = LsSkill();
	// (gunfire: where the fighting is, for deathmatch hunting; kept apart from the noise the
	// stock code forgets as soon as it looks into it)
	if (event == EVENT_WEAPON_FIRED && dist < 2000.0f)
	{
		m_lsFightHeard = pPlayer->pev->origin;
		m_lsFightHeardAt = now;
	}
	// (a sniper rifle's shot is known by its sound, from anywhere on the map: there's an
	// AWPer, roughly there - the further off, the rougher)
	if (event == EVENT_WEAPON_FIRED)
	{
		CBasePlayerWeapon *pShot = static_cast<CBasePlayerWeapon *>(pPlayer->m_pActiveItem);
		if (pShot && (pShot->m_iId == WEAPON_AWP || pShot->m_iId == WEAPON_SCOUT || pShot->m_iId == WEAPON_G3SG1 || pShot->m_iId == WEAPON_SG550)
			&& RANDOM_FLOAT(0.0f, 1.0f) < 0.4f + 0.6f * skill)
		{
			float err = dist * 0.12f;
			m_lsSniperHeard = pPlayer;
			m_lsSniperSpot = pPlayer->pev->origin + Vector(RANDOM_FLOAT(-err, err), RANDOM_FLOAT(-err, err), 0.0f);
			m_lsSniperHeardAt = now;
			if (pShot->m_iId == WEAPON_AWP || pShot->m_iId == WEAPON_SCOUT)
				m_lsSniperShotAt = now;
		}
	}
	// (a plugin's hold or look gives way to an enemy heard close by)
	// (not a person's "wait" or "follow me": those only give way to what an order does)
	if (LsPuppetActive() && dist < 700.0f && !(now < m_lsPuppet.followUntil || FStrEq(m_lsPuppet.holdTag, "bc")))
		LsPuppetRelease();
	bool busy = m_state == &m_plantBombState || m_state == &m_defuseBombState || IsUsingLadder() || m_lsSaving || LsPuppetActive() || IsBlind();

	// (someone running this way, close: counted for being rushed, LsRushed)
	if (event == EVENT_PLAYER_FOOTSTEP && dist < 1000.0f)
	{
		Vector vel = pPlayer->pev->velocity;
		Vector toMe = pev->origin - pPlayer->pev->origin;
		int idx = pPlayer->entindex() - 1;
		if (idx >= 0 && idx < MAX_CLIENTS && vel.Length2D() > 150.0f && vel.x * toMe.x + vel.y * toMe.y > 0.0f)
		{
			m_lsRushHeardAt[idx] = now;
			if (!IsAttacking())
				LsRushed(&pPlayer->pev->origin);
		}
	}

	// an enemy scoping a sniper rifle: there's an AWPer about, roughly there
	if (event == EVENT_WEAPON_ZOOMED)
	{
		CBasePlayerWeapon *pGun = static_cast<CBasePlayerWeapon *>(pPlayer->m_pActiveItem);
		if (pGun && (pGun->m_iId == WEAPON_AWP || pGun->m_iId == WEAPON_SCOUT || pGun->m_iId == WEAPON_G3SG1 || pGun->m_iId == WEAPON_SG550))
		{
			float err = dist * 0.12f;
			m_lsSniperHeard = pPlayer;
			m_lsSniperSpot = pPlayer->pev->origin + Vector(RANDOM_FLOAT(-err, err), RANDOM_FLOAT(-err, err), 0.0f);
			m_lsSniperHeardAt = now;
		}
		return;
	}

	// in a fight, someone else heard running at it close (a knife rush, a flank): the better
	// ones turn to meet it (after finishing one nearly dead in front of them), newer ones
	// tunnel on the fight they're in
	if (IsAttacking() && event == EVENT_PLAYER_FOOTSTEP && pPlayer != GetEnemy() && dist < 500.0f && now - m_lsRunnerAt > 1.2f && !IsBlind())
	{
		Vector vel = pPlayer->pev->velocity;
		Vector toMe = pev->origin - pPlayer->pev->origin;
		if (vel.Length2D() > 180.0f && vel.x * toMe.x + vel.y * toMe.y > 0.0f && RANDOM_FLOAT(0.0f, 1.0f) < 0.1f + 0.85f * skill
			&& !IsVisible(pPlayer, true))
		{
			// (only if the runner gets there first, or the one in front isn't looking its way;
			// never turning away from one nearly dead in front of it)
			CBasePlayer *pEnemy = GetEnemy();
			bool fighting = pEnemy && IsEnemyVisible();
			bool first = !fighting || dist < float((pEnemy->pev->origin - pev->origin).Length()) || !IsPlayerLookingAtMe(pEnemy);
			float knownHp = 100.0f;
			bool nearlyDead = fighting && LsKnownHurt(pEnemy, m_iTeam, &knownHp) && knownHp < 30.0f;
			if (first && !(nearlyDead && skill > 0.5f))
			{
				m_lsRunner = pPlayer;
				m_lsRunnerSpot = pPlayer->pev->origin + Vector(0, 0, HalfHumanHeight);
				m_lsRunnerUntil = now + 0.25f + 0.35f * (1.0f - skill);
				m_lsRunnerAt = now;
				LsWhy("someone running at it");
			}
		}
		return;
	}

	// in a fight with them: their reload is the moment to push
	if (IsAttacking())
	{
		if (event == EVENT_WEAPON_RELOADED && GetEnemy() == pPlayer && !LsMatch() && aggro > 0.5f && RANDOM_FLOAT(0.0f, 1.0f) < aggro)
			m_lsPushUntil = now + RANDOM_FLOAT(1.0f, 2.0f);
		return;
	}
	if (busy)
		return;
	// (a door opening, a landing from a drop: someone's coming that way)
	bool coming = event == EVENT_DOOR || event == EVENT_PLAYER_LANDED_FROM_HEIGHT;
	if (event != EVENT_WEAPON_RELOADED && event != EVENT_PLAYER_FOOTSTEP && event != EVENT_WEAPON_FIRED && !coming)
		return;
	if (dist > (coming ? 1000.0f : 900.0f))
		return;
	// (once a moment: the visibility check below is a few traces)
	if (now - m_lsHeardAt < 0.4f)
		return;
	m_lsHeardAt = now;
	if (IsVisible(pPlayer, false))
		return;

	// a floor above or below (a ceiling in between, a long way round): a glance up or down,
	// listening; no pre-aim at the ceiling, no spraying through it, no rush
	float dz = float(pPlayer->pev->origin.z - pev->origin.z);
	if (dz > 120.0f || dz < -120.0f)
	{
		// (toward where it came from: up there or down there as heard, roughly - the level they're
		// on, at head height, never more than about 25 degrees off level unless they really are
		// that steeply above; not a fixed tip that stared at the sky outdoors)
		Vector flat = pPlayer->pev->origin - pev->origin;
		flat.z = 0.0f;
		float across = float(flat.Length2D());
		flat = flat.IsZero() ? Vector(BotCOS(pev->v_angle.y), BotSIN(pev->v_angle.y), 0.0f) : flat.Normalize();
		float reach = Q_max(200.0f, Q_min(across, 600.0f));
		float rise = Q_max(-0.47f * reach, Q_min(0.47f * reach, dz * (reach / Q_max(across, 1.0f))));
		Vector glance = pev->origin + pev->view_ofs + reach * flat + Vector(0, 0, rise);
		if (now - m_lsPreAimAt > 3.0f)
		{
			SetLookAt("Loner-Strike: listening", &glance, PRIORITY_LOW, RANDOM_FLOAT(0.6f, 1.2f));
			m_lsPreAimAt = now;
		}
		return;
	}

	// where they are, as heard (less exact further away)
	float err = dist * 0.15f;
	Vector heard = pPlayer->pev->origin + Vector(RANDOM_FLOAT(-err, err), RANDOM_FLOAT(-err, err), 0.0f);

	// (only close enough to get there while they're still reloading; the very best don't
	// rush a sound blind, they hold the door instead)
	if (event == EVENT_WEAPON_RELOADED && aggro > 0.6f && skill < 0.85f && dist < 600.0f && LsFree()
		&& RANDOM_FLOAT(0.0f, 1.0f) < 0.5f + 0.4f * (aggro - 0.6f))
	{
		Run();
		Hurry(2.5f);
		MoveTo(&heard, FASTEST_ROUTE);
		LsWhy("reload-push");
		return;
	}

	// the good ones spray a thin wall they know someone's behind
	if (dist < 700.0f && skill > 0.63f && LsFree() && LsTrait(33) < skill - 0.33f && now - m_lsBangHeardAt > 5.5f && RANDOM_FLOAT(0.0f, 1.0f) < 0.32f + 0.3f * (skill - 0.63f))
	{
		m_lsBangHeardAt = now;
		if (LsBangThrough(heard + Vector(0, 0, 10.0f)))
			return;
	}

	// pre-aim where they'd come out (a door or a landing more surely: they're on their way)
	if ((event == EVENT_PLAYER_FOOTSTEP || event == EVENT_WEAPON_RELOADED || coming) && dist < (coming ? 1000.0f : 700.0f) && now - m_lsPreAimAt > 1.5f
		&& RANDOM_FLOAT(0.0f, 1.0f) < (coming ? 0.6f + 0.4f * skill : 0.4f + 0.5f * skill))
	{
		m_lsPreAimAt = now;
		if (LsPreAim(heard, RANDOM_FLOAT(1.5f, 3.0f), pPlayer))
			return;
		Vector eye = pev->origin + pev->view_ofs;
		Vector from = heard + Vector(0, 0, HalfHumanHeight);
		Vector step = eye - from;
		float len = float(step.Length());
		if (len < 1.0f)
			return;
		step = step * (48.0f / len);
		Vector at = from;
		for (int i = 0; i < 16 && (at - eye).IsLengthGreaterThan(64.0f); i++)
		{
			TraceResult tr;
			UTIL_TraceLine(eye, at, ignore_monsters, ENT(pev), &tr);
			if (tr.flFraction >= 1.0f)
			{
				SetLookAt("Loner-Strike: pre-aim", &at, PRIORITY_MEDIUM, RANDOM_FLOAT(1.5f, 3.0f));
				return;
			}
			at = at + step;
		}
	}
}

// About to defuse: most people look round the site first (corners, the way the Ts would
// come), less so with time running out or teammates around covering, and newer players
// often not at all. The defuse starts once the look is done (LsObjectiveThreat holds it)
void CCSBot::LsCheckBeforeDefuse()
{
	if (!LsFight() || !TheCSBots()->IsBombPlanted())
		return;
	float takes = m_bHasDefuser ? 5.0f : 10.0f;
	if (TheCSBots()->GetBombTimeLeft() < takes + 6.0f)
		return;
	int cover = 0;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		if ((pPlayer->pev->origin - pev->origin).IsLengthLessThan(600.0f))
			cover++;
	});
	float chance = (0.3f + 0.6f * LsSkill()) * (cover ? 0.4f : 1.0f);
	if (RANDOM_FLOAT(0.0f, 1.0f) > chance)
		return;
	Vector look;
	if (m_noiseTimestamp > 0.0f && gpGlobals->time - m_noiseTimestamp < 6.0f)
		look = m_noisePosition + Vector(0, 0, HalfHumanHeight);
	else
	{
		float yaw = pev->v_angle.y + RANDOM_FLOAT(120.0f, 240.0f);
		look = pev->origin + Vector(BotCOS(yaw) * 400.0f, BotSIN(yaw) * 400.0f, HalfHumanHeight);
	}
	float secs = RANDOM_FLOAT(0.8f, 1.8f);
	StandUp();
	SetLookAt("Loner-Strike: checking", &look, PRIORITY_HIGH, secs);
	m_lsCheckUntil = gpGlobals->time + secs;
	m_lsObjUseAt = m_lsCheckUntil;
}


// About to plant: a quick double check first - a glance at a noise just heard, or the site's
// busiest way in - then the plant, relying on what it sees and hears. A fraction of a second;
// more likely alone than with teammates covering; newer players often skip it
void CCSBot::LsCheckBeforePlant()
{
	if (!LsFight() || LsTimeLeft() < 15.0f)
		return;
	int cover = 0;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		if ((pPlayer->pev->origin - pev->origin).IsLengthLessThan(600.0f))
			cover++;
	});
	int style = LsStyle();
	float lean = (style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER) ? 1.3f : (style == LS_STYLE_RUSHER || style == LS_STYLE_KNIFER) ? 0.4f : 1.0f;
	float chance = (0.35f + 0.55f * LsSkill()) * (cover ? 0.5f : 1.0f) * lean;
	if (RANDOM_FLOAT(0.0f, 1.0f) > chance)
		return;
	Vector look;
	const LsAreaKnow *k = m_lastKnownArea ? LsKnow(m_lastKnownArea) : nullptr;
	CNavArea *way = (k && k->angleCount) ? LsKnowArea(k->angles[0].area) : nullptr;
	if (m_noiseTimestamp > 0.0f && gpGlobals->time - m_noiseTimestamp < 4.0f && (m_noisePosition - pev->origin).IsLengthLessThan(1200.0f))
		look = m_noisePosition + Vector(0, 0, HalfHumanHeight);
	else if (way)
		look = *way->GetCenter() + Vector(0, 0, HalfHumanHeight + LsLookHeight());
	else
		return;
	float secs = RANDOM_FLOAT(0.35f, 0.8f) * (lean > 1.2f ? 1.3f : 1.0f);
	// (that noise is checked: the plant's own threat roll doesn't stop to look at it again)
	m_lsObjRolledAt = m_noiseTimestamp;
	SetLookAt("Loner-Strike: checking", &look, PRIORITY_HIGH, secs);
	m_lsCheckUntil = gpGlobals->time + secs;
	m_lsObjUseAt = m_lsCheckUntil;
}


// ---- map knowledge ------------------------------------------------------------------------
//
// What a person learns about a map by playing it, worked out once per map in the background
// (a little each frame: LsSightStep) and saved next to it (maps/<map>.lsv), loaded at once
// after. Built on the nav mesh's walkable areas:
//  - sightlines: which areas can see which (standing eyes to standing or crouched heads, at
//    any height), and how much: a sliver of a doorway or wide open
//  - wallbang spots: close pairs that can't see each other through one thin layer, its
//    thickness and what it's made of
//  - for each area: cover all round (how far to something solid at crouched and standing head
//    height, and what it's made of), the ceiling, the corners someone could be just past, the
//    angles enemies can appear from (places in sight that lead to places out of it), objects
//    (low boxes and walls found by the cover scan; doors, breakables, ladders and brush boxes
//    from the map), how much of the map it sees and whether it's high ground
// Stages, each saved as it goes so a big map can finish over several visits: pairs, areas,
// angles. Until a stage is done, nothing asks for what it makes.

namespace
{
// A clock in milliseconds for the frame budget (on Linux gettimeofday: std::chrono's clock
// needs a newer libstdc++ than the servers this is built for have)
double LsNowMs()
{
#ifdef _WIN32
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
#else
	timeval tv;
	gettimeofday(&tv, nullptr);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
#endif
}

const unsigned int LSV_MAGIC = 0x3256534C;	// "LSV2"
const unsigned int LSV_VERSION = 6;	// (6: sky brushes block sight - inferno's rooftops)
const float LSV_RANGE = 3500.0f;			// farther than this counts as not in sight
const float LSV_BANG_RANGE = 1000.0f;		// wallbang spots looked for this close
const double LSV_BUDGET_MS = 0.4;			// per frame
const double LSV_SAVE_EVERY_MS = 20000.0;	// progress saved this often while it works
const float EYE = 60.0f, CROUCH_HEAD = 34.0f, WAIST = 24.0f;
enum { STAGE_PAIRS, STAGE_AREAS, STAGE_ANGLES, STAGE_EXPOSURE, STAGE_DONE };

#pragma pack(push, 1)
struct PairInfo
{
	unsigned short a, b;
	unsigned char quality;					// 0-255: how much of one is in view from the other
	unsigned char flags;					// LSP_*
};
struct BangInfo
{
	unsigned short a, b;
	unsigned char thick;					// units
	unsigned char mat;						// CHAR_TEX_*
};
struct FileHead
{
	unsigned int magic, version, key, count, stage, ci, cj, pairs, bangs;
	unsigned int traces;
	float spentMs;
};
#pragma pack(pop)

struct LsMapKnow
{
	bool ready = false;						// pairs done: LsSees works
	bool tried = false;
	int navLoads = -1;						// the nav it's for (a reload of the same map is a new one)
	int stage = STAGE_PAIRS;
	char map[64] = {};
	size_t count = 0, navSize = 0;
	unsigned int key = 0;
	std::vector<CNavArea *> areas;
	std::vector<int> index;					// area id -> index
	std::vector<unsigned char> bits;		// count * count, row-major
	std::vector<PairInfo> pairs;			// visible pairs, a < b
	std::vector<BangInfo> bangs;
	std::vector<LsAreaKnow> know;
	std::vector<std::vector<std::pair<unsigned short, unsigned char>>> inView;	// per area: (other, quality), both ways
	size_t i = 0, j = 1;					// next pair or area
	double spentMs = 0.0, lastSave = 0.0;
	float startedAt = 0.0f;
	unsigned int traces = 0;
};
LsMapKnow s_know;

unsigned int LsNavKey()
{
	unsigned int h = 2166136261u;
	auto mix = [&h](unsigned int v) { h ^= v; h *= 16777619u; };
	mix((unsigned int)TheNavAreaList.size());
	for (NavAreaList::iterator it = TheNavAreaList.begin(); it != TheNavAreaList.end(); it++)
	{
		const CNavArea *a = *it;
		mix(a->GetID());
		mix((unsigned int)(int)a->GetCenter()->x);
		mix((unsigned int)(int)a->GetCenter()->y);
		mix((unsigned int)(int)a->GetCenter()->z);
	}
	return h;
}

bool SeeThrough(edict_t *hit)
{
	CBaseEntity *pHit = hit ? CBaseEntity::Instance(hit) : nullptr;
	return pHit && (FClassnameIs(pHit->pev, "func_door") || FClassnameIs(pHit->pev, "func_door_rotating") || FClassnameIs(pHit->pev, "func_breakable"));
}

// Whether a person could see from one point to the other: only solid walls block (glass, a
// door or a breakable window don't)
bool Clear(const Vector &from, const Vector &to)
{
	Vector start = from;
	edict_t *skip = nullptr;
	for (int step = 0; step < 4; step++)
	{
		TraceResult tr;
		UTIL_TraceLine(start, to, ignore_monsters, ignore_glass, skip, &tr);
		s_know.traces++;
		if (tr.flFraction >= 1.0f)
			return !LsCrossesSky(from, to, 48.0f);
		if (tr.fStartSolid || !SeeThrough(tr.pHit))
			return false;
		start = tr.vecEndPos;
		skip = tr.pHit;
	}
	return !LsCrossesSky(from, to, 48.0f);
}

// How far it is to something solid that way (a door or window counts: it's in the way now)
float Open(const Vector &from, const Vector &dir, float range, TraceResult *out = nullptr)
{
	TraceResult tr;
	UTIL_TraceLine(from, from + dir * range, ignore_monsters, dont_ignore_glass, nullptr, &tr);
	s_know.traces++;
	if (out)
		*out = tr;
	return tr.fStartSolid ? 0.0f : range * tr.flFraction;
}

// The material of the surface a line hits (CHAR_TEX_*; 0 for none)
unsigned char Material(const Vector &from, const Vector &to, edict_t *hit)
{
	float v1[3] = { from.x, from.y, from.z }, v2[3] = { to.x, to.y, to.z };
	const char *name = TRACE_TEXTURE(hit ? hit : ENT(0), v1, v2);
	if (!name)
		return 0;
	if (*name == '-' || *name == '+')
		name += 2;
	if (*name == '{' || *name == '!' || *name == '~' || *name == ' ')
		name++;
	char buf[64];
	Q_strlcpy(buf, name);
	return (unsigned char)TEXTURETYPE_Find(buf);
}

Vector2D Side(const CNavArea *a, const CNavArea *b)
{
	Vector2D d(b->GetCenter()->x - a->GetCenter()->x, b->GetCenter()->y - a->GetCenter()->y);
	float len = d.Length();
	if (len < 1.0f)
		return Vector2D(1.0f, 0.0f);
	return Vector2D(-d.y / len, d.x / len);
}

float Reach(const CNavArea *n)
{
	const Extent *e = n->GetExtent();
	return 0.35f * Q_min(e->hi.x - e->lo.x, e->hi.y - e->lo.y);
}

// One pair: whether they can see each other, how much, whether a crouched head is in view, and
// (close and out of sight) whether one thin layer is all that's between them
void Pair(size_t ia, size_t ib)
{
	if (ia >= s_know.count || ib >= s_know.count)
		return;
	const CNavArea *a = s_know.areas[ia], *b = s_know.areas[ib];
	const Vector &ca = *a->GetCenter(), &cb = *b->GetCenter();
	float dist = float((ca - cb).Length());
	if (dist > LSV_RANGE)
		return;
	Vector ea(ca.x, ca.y, ca.z + EYE), eb(cb.x, cb.y, cb.z + EYE);
	bool seen = Clear(ea, eb) || Clear(ea, Vector(cb.x, cb.y, cb.z + CROUCH_HEAD)) || Clear(eb, Vector(ca.x, ca.y, ca.z + CROUCH_HEAD));
	Vector2D side = Side(a, b);
	float ra = Reach(a), rb = Reach(b);
	if (!seen && (ra > 50.0f || rb > 50.0f))
	{
		for (int k = -1; k <= 1 && !seen; k += 2)
			seen = Clear(Vector(ca.x + side.x * ra * k, ca.y + side.y * ra * k, ca.z + EYE), Vector(cb.x + side.x * rb * k, cb.y + side.y * rb * k, cb.z + EYE));
	}
	if (!seen)
	{
		// a wallbang spot: one thin layer between eyes and head, nothing after it
		if (dist > LSV_BANG_RANGE)
			return;
		TraceResult in, out;
		UTIL_TraceLine(ea, eb, ignore_monsters, ignore_glass, nullptr, &in);
		UTIL_TraceLine(eb, ea, ignore_monsters, ignore_glass, nullptr, &out);
		s_know.traces += 2;
		if (in.fStartSolid || out.fStartSolid || in.flFraction >= 1.0f || out.flFraction >= 1.0f)
			return;
		float thick = float((out.vecEndPos - in.vecEndPos).Length());
		if (thick > 64.0f)
			return;
		Vector dir = (eb - ea).Normalize();
		// (past the layer, clear the rest of the way)
		if (!Clear(out.vecEndPos + dir * 2.0f, eb))
			return;
		// (only where some gun really gets through it by the game's rule: the AWP's reach in
		// that material, 42 for anything that isn't part of the map's walls)
		unsigned char mat = Material(ea, eb, in.pHit);
		float reach = 45.0f;
		switch (mat)
		{
		case CHAR_TEX_METAL: reach *= 0.15f; break;
		case CHAR_TEX_CONCRETE: reach *= 0.25f; break;
		case CHAR_TEX_GRATE: case CHAR_TEX_VENT: reach *= 0.5f; break;
		case CHAR_TEX_TILE: reach *= 0.65f; break;
		case CHAR_TEX_COMPUTER: reach *= 0.4f; break;
		case CHAR_TEX_WOOD: break;
		default: reach *= 0.25f; break;
		}
		if (in.pHit && VARS(in.pHit)->solid != SOLID_BSP)
			reach = 42.0f;
		if (thick > reach)
			return;
		BangInfo bang;
		bang.a = (unsigned short)ia;
		bang.b = (unsigned short)ib;
		bang.thick = (unsigned char)Q_min(255.0f, thick + 0.5f);
		bang.mat = mat;
		s_know.bangs.push_back(bang);
		return;
	}
	// how much: three points in each (the middle and either side), to standing and crouched
	// heads: the share of those lines that are clear
	int clear = 0, crouched = 0;
	for (int x = -1; x <= 1; x++)
	{
		Vector pa(ca.x + side.x * ra * x, ca.y + side.y * ra * x, ca.z + EYE);
		for (int y = -1; y <= 1; y++)
		{
			Vector2D at(cb.x + side.x * rb * y, cb.y + side.y * rb * y);
			if (Clear(pa, Vector(at.x, at.y, cb.z + EYE)))
				clear++;
			if (Clear(pa, Vector(at.x, at.y, cb.z + CROUCH_HEAD)))
			{
				clear++;
				crouched++;
			}
		}
	}
	PairInfo info;
	info.a = (unsigned short)ia;
	info.b = (unsigned short)ib;
	info.quality = (unsigned char)Q_max(1, clear * 255 / 18);
	info.flags = (unsigned char)(crouched ? LSP_CROUCHED : 0);
	s_know.pairs.push_back(info);
	size_t n = s_know.count;
	s_know.bits[(ia * n + ib) >> 3] |= (unsigned char)(1 << ((ia * n + ib) & 7));
	s_know.bits[(ib * n + ia) >> 3] |= (unsigned char)(1 << ((ib * n + ia) & 7));
}

void AddObject(LsAreaKnow &k, unsigned char type, const Vector &pos, float top, unsigned char mat)
{
	for (int o = 0; o < k.objectCount; o++)
	{
		LsKnownObject &ob = k.objects[o];
		if (Q_abs(ob.x - pos.x) < 48.0f && Q_abs(ob.y - pos.y) < 48.0f && Q_abs(ob.z - pos.z) < 64.0f && ob.type == type)
			return;
	}
	if (k.objectCount >= LS_KNOW_OBJECTS)
		return;
	LsKnownObject &ob = k.objects[k.objectCount++];
	ob.type = type;
	ob.x = (short)pos.x;
	ob.y = (short)pos.y;
	ob.z = (short)pos.z;
	ob.top = (short)top;
	ob.mat = mat;
}

// Something low in front (solid at a crouched head `dist` away, open above): real low cover
// (a crate, a low wall) or just stairs, a ramp or a step? Cover stands at least 30 units
// above the floor, and past its top the floor doesn't keep rising. Its top in `top`
bool LowCover(const Vector &floor, const Vector &dir, float dist, float *top)
{
	auto surface = [&](float along) {
		Vector at = floor + dir * along;
		TraceResult down;
		UTIL_TraceLine(Vector(at.x, at.y, floor.z + EYE + 20.0f), Vector(at.x, at.y, floor.z - 8.0f), ignore_monsters, nullptr, &down);
		s_know.traces++;
		return down.fStartSolid ? floor.z + EYE : down.vecEndPos.z;
	};
	float a = surface(dist + 8.0f);
	if (a - floor.z < 30.0f)
		return false;
	// (stairs and ramps: further on it's higher again)
	if (surface(dist + 40.0f) > a + 12.0f)
		return false;
	if (top)
		*top = a;
	return true;
}

// One area: cover all round and what it's made of, low objects, the ceiling, corners
void Area(size_t idx)
{
	if (idx >= s_know.count)
		return;
	const CNavArea *area = s_know.areas[idx];
	LsAreaKnow &k = s_know.know[idx];
	const Vector &c = *area->GetCenter();
	Vector eye(c.x, c.y, c.z + EYE), low(c.x, c.y, c.z + CROUCH_HEAD);
	for (int d = 0; d < LS_KNOW_DIRS; d++)
	{
		float yaw = 360.0f / LS_KNOW_DIRS * d;
		Vector dir(BotCOS(yaw), BotSIN(yaw), 0.0f);
		TraceResult trLow, trHigh;
		float openLow = Open(low, dir, 2000.0f, &trLow), openHigh = Open(eye, dir, 2000.0f, &trHigh);
		k.coverLow[d] = (unsigned char)Q_min(255.0f, openLow / 8.0f);
		k.coverHigh[d] = (unsigned char)Q_min(255.0f, openHigh / 8.0f);
		k.mat[d] = openHigh < 300.0f ? Material(eye, eye + dir * 2000.0f, trHigh.pHit) : (openLow < 300.0f ? Material(low, low + dir * 2000.0f, trLow.pHit) : 0);
		// something low there (a box, a low wall): solid at a crouched head, open over it
		float top;
		if (openLow < 120.0f && openHigh > openLow + 48.0f && LowCover(c, dir, openLow, &top))
		{
			Vector edge = low + dir * (openLow + 8.0f);
			AddObject(k, LSO_LOW, Vector(edge.x, edge.y, c.z), top, Material(low, low + dir * 2000.0f, trLow.pHit));
		}
		// (stairs or a ramp ahead isn't low cover)
		else if (openLow < openHigh && openLow < 300.0f && !LowCover(c, dir, openLow, nullptr))
			k.coverLow[d] = k.coverHigh[d];
	}
	// the ceiling
	k.ceiling = (unsigned char)Q_min(255.0f, Open(Vector(c.x, c.y, c.z + 40.0f), Vector(0, 0, 1), 1000.0f) / 4.0f);

	// corners: all round, at eye and waist height, an edge with open space past it (someone
	// could be just round it); the nearest first. Not from a thin strip along a wall (its
	// middle is against the wall, every ray glances along it): its neighbours have those corners
	k.cornerCount = 0;
	{
		const Extent *ex = area->GetExtent();
		if (ex->hi.x - ex->lo.x < 64.0f || ex->hi.y - ex->lo.y < 64.0f)
			return;
	}
	const int N = 48;
	float dEye[N], dWaist[N];
	// (how square on each ray met what it hit: one glancing along a wall runs on much further
	// than its neighbour, which looks like a gap but isn't)
	float gEye[N], gWaist[N];
	for (int r = 0; r < N; r++)
	{
		float yaw = 360.0f / N * r;
		Vector dir(BotCOS(yaw), BotSIN(yaw), 0.0f);
		TraceResult te, tw;
		dEye[r] = Open(eye, dir, 1500.0f, &te);
		dWaist[r] = Open(Vector(c.x, c.y, c.z + WAIST), dir, 1500.0f, &tw);
		gEye[r] = te.flFraction < 1.0f ? (float)Q_abs(DotProduct(te.vecPlaneNormal, dir)) : 1.0f;
		gWaist[r] = tw.flFraction < 1.0f ? (float)Q_abs(DotProduct(tw.vecPlaneNormal, dir)) : 1.0f;
	}
	struct Cand { float dist; Vector at; float yaw; };
	Cand cand[2 * N];
	int nc = 0;
	for (int h = 0; h < 2; h++)
	{
		float *d = h ? dWaist : dEye;
		float *g = h ? gWaist : gEye;
		for (int r = 0; r < N; r++)
		{
			int r1 = (r + 1) % N;
			float n0 = d[r], n1 = d[r1];
			float nearD = Q_min(n0, n1), farD = Q_max(n0, n1);
			if (farD - nearD < 200.0f || nearD < 100.0f || nearD > 1200.0f)
				continue;
			// (the far one only glancing along a wall: the same wall, no gap; a real edge has the
			// far ray going on past it into the open, or meeting something square on)
			if ((n0 > n1 ? g[r] : g[r1]) < 0.3f)
				continue;
			float yaw = 360.0f / N * (r + 0.5f);
			Vector dir(BotCOS(yaw), BotSIN(yaw), 0.0f);
			cand[nc].dist = nearD;
			cand[nc].at = eye + dir * (nearD + 24.0f);
			// which way it opens: toward the far side
			cand[nc].yaw = n0 > n1 ? 360.0f / N * r : 360.0f / N * (r + 1);
			nc++;
		}
	}
	for (int x = 0; x < nc; x++)
		for (int y = x + 1; y < nc; y++)
			if (cand[y].dist < cand[x].dist)
				std::swap(cand[x], cand[y]);
	k.cornerCount = 0;
	for (int x = 0; x < nc && k.cornerCount < LS_KNOW_CORNERS; x++)
	{
		bool dup = false;
		for (int y = 0; y < k.cornerCount; y++)
			if (Q_abs(k.corners[y].x - cand[x].at.x) < 64.0f && Q_abs(k.corners[y].y - cand[x].at.y) < 64.0f)
				dup = true;
		// (in view from here: not past a window or gap out of the map)
		if (dup || !Clear(eye, cand[x].at))
			continue;
		LsKnownCorner &cn = k.corners[k.cornerCount++];
		cn.x = (short)cand[x].at.x;
		cn.y = (short)cand[x].at.y;
		cn.z = (short)cand[x].at.z;
		cn.yaw = (unsigned char)(cand[x].yaw / 360.0f * 256.0f);
	}
}

// The map's own objects next to the areas they're in: doors, breakables, brush boxes, ladders
void MapObjects()
{
	static const struct { const char *cls; unsigned char type; } kinds[] = {
		{ "func_door", LSO_DOOR }, { "func_door_rotating", LSO_DOOR }, { "func_breakable", LSO_BREAKABLE },
		{ "func_wall", LSO_BOX }, { "func_pushable", LSO_BOX }, { "func_illusionary", LSO_BOX },
	};
	for (auto &kind : kinds)
	{
		CBaseEntity *pEnt = nullptr;
		while ((pEnt = UTIL_FindEntityByClassname(pEnt, kind.cls)) != nullptr)
		{
			Vector mid = (pEnt->pev->absmin + pEnt->pev->absmax) * 0.5f;
			Vector feet(mid.x, mid.y, pEnt->pev->absmin.z + 1.0f);
			CNavArea *area = TheNavAreaGrid.GetNearestNavArea(&feet);
			if (!area || (*area->GetCenter() - feet).Make2D().IsLengthGreaterThan(250.0f) || area->GetID() >= s_know.index.size())
				continue;
			// (in view from that area, not decoration behind a wall or outside the map)
			{
				const Vector &ac = *area->GetCenter();
				TraceResult tr;
				UTIL_TraceLine(Vector(ac.x, ac.y, ac.z + EYE), mid, ignore_monsters, ENT(pEnt->pev), &tr);
				s_know.traces++;
				if (tr.flFraction < 0.95f || tr.fStartSolid)
					continue;
			}
			int idx = s_know.index[area->GetID()];
			if (idx < 0)
				continue;
			// (a func_illusionary is see-through scenery, not a box: only the big solid ones)
			if (kind.type == LSO_BOX && (pEnt->pev->solid == SOLID_NOT || (pEnt->pev->size.x < 16.0f && pEnt->pev->size.y < 16.0f)))
				continue;
			AddObject(s_know.know[idx], kind.type, feet, pEnt->pev->absmax.z, 0);
		}
	}
	for (CNavLadder *ladder : TheNavLadderList)
	{
		CNavArea *area = ladder->m_bottomArea;
		if (!area || area->GetID() >= s_know.index.size() || s_know.index[area->GetID()] < 0)
			continue;
		AddObject(s_know.know[s_know.index[area->GetID()]], LSO_LADDER, ladder->m_bottom, ladder->m_top.z, 0);
	}
}

void BuildNear()
{
	size_t n = s_know.count;
	s_know.inView.assign(n, {});
	for (const PairInfo &p : s_know.pairs)
	{
		s_know.inView[p.a].push_back({ p.b, p.quality });
		s_know.inView[p.b].push_back({ p.a, p.quality });
	}
	for (auto &list : s_know.inView)
		std::sort(list.begin(), list.end());
}

bool BitSees(size_t x, size_t y)
{
	size_t bit = x * s_know.count + y;
	return (s_know.bits[bit >> 3] >> (bit & 7)) & 1;
}

// One area's angles: the places in its sight that lead somewhere out of it (where someone
// appears), grouped by direction; the most-used ways first, each at its nearest point. Also
// how much of the map it sees and whether it's above what it sees
void Angles(size_t idx)
{
	if (idx >= s_know.count)
		return;
	LsAreaKnow &k = s_know.know[idx];
	const CNavArea *me = s_know.areas[idx];
	const Vector &c = *me->GetCenter();
	const int B = 16;
	int weight[B] = {};
	float nearest[B];
	int nearIdx[B];
	for (int x = 0; x < B; x++) { nearest[x] = 1e9f; nearIdx[x] = -1; }
	int lower = 0, sees = 0;
	for (auto &seen : s_know.inView[idx])
	{
		size_t v = seen.first;
		sees++;
		const CNavArea *va = s_know.areas[v];
		if (va->GetCenter()->z < c.z - 64.0f)
			lower++;
		bool reveals = false;
		for (int d = 0; d < NUM_DIRECTIONS && !reveals; d++)
		{
			for (int i = 0; i < va->GetAdjacentCount((NavDirType)d); i++)
			{
				CNavArea *w = va->GetAdjacentArea((NavDirType)d, i);
				if (!w || w->GetID() >= s_know.index.size())
					continue;
				int wi = s_know.index[w->GetID()];
				if (wi >= 0 && (size_t)wi != idx && !BitSees(idx, wi))
				{
					reveals = true;
					break;
				}
			}
		}
		if (!reveals)
			continue;
		Vector to = *va->GetCenter() - c;
		float yaw = UTIL_VecToYaw(to);
		if (yaw < 0.0f)
			yaw += 360.0f;
		int bucket = int(yaw / (360.0f / B)) % B;
		weight[bucket]++;
		float dist = float(to.Length());
		if (dist < nearest[bucket])
		{
			nearest[bucket] = dist;
			nearIdx[bucket] = (int)v;
		}
	}
	k.sees = (unsigned short)Q_min(65535, sees);
	k.high = (unsigned char)(sees ? lower * 255 / sees : 0);
	k.angleCount = 0;
	for (int pick = 0; pick < LS_KNOW_ANGLES; pick++)
	{
		int best = -1;
		for (int x = 0; x < B; x++)
			if (weight[x] > 0 && (best < 0 || weight[x] > weight[best]))
				best = x;
		if (best < 0)
			break;
		const CNavArea *va = s_know.areas[nearIdx[best]];
		Vector to = Vector(va->GetCenter()->x, va->GetCenter()->y, va->GetCenter()->z + EYE) - Vector(c.x, c.y, c.z + EYE);
		LsKnownAngle &an = k.angles[k.angleCount++];
		float yaw = UTIL_VecToYaw(to);
		if (yaw < 0.0f)
			yaw += 360.0f;
		an.yaw = (unsigned char)(yaw / 360.0f * 256.0f);
		float flat = to.Length2D();
		an.pitch = (signed char)Q_max(-90.0f, Q_min(90.0f, float(atan2(to.z, flat) * 57.29578)));
		an.dist = (unsigned short)Q_min(65535.0f, float(to.Length()));
		an.weight = (unsigned char)Q_min(255, weight[best]);
		an.area = (unsigned short)nearIdx[best];
		weight[best] = 0;
	}
}

// One area's exposure: from each of its angles (the place an enemy would appear), which of
// its 9 points can be seen, at a standing head and a crouched one
void Exposure(size_t idx)
{
	if (idx >= s_know.count)
		return;
	LsAreaKnow &k = s_know.know[idx];
	const CNavArea *area = s_know.areas[idx];
	Vector pts[9];
	for (int p = 0; p < 9; p++)
		LsExposurePoint(area, p, &pts[p]);
	// the cover right by each point, all round
	for (int p = 0; p < 9; p++)
	{
		k.pointCoverLow[p] = k.pointCoverHigh[p] = 0;
		Vector lowAt(pts[p].x, pts[p].y, pts[p].z + CROUCH_HEAD), highAt(pts[p].x, pts[p].y, pts[p].z + EYE);
		for (int d = 0; d < LS_KNOW_DIRS; d++)
		{
			float yaw = 360.0f / LS_KNOW_DIRS * d;
			Vector dir(BotCOS(yaw), BotSIN(yaw), 0.0f);
			float lowD = Open(lowAt, dir, 64.0f);
			bool high = Open(highAt, dir, 64.0f) < 64.0f;
			// (low only: real low cover, not the first steps of a staircase)
			if (lowD < 64.0f && (high || LowCover(pts[p], dir, lowD, nullptr)))
				k.pointCoverLow[p] |= (unsigned short)(1 << d);
			if (high)
				k.pointCoverHigh[p] |= (unsigned short)(1 << d);
		}
	}
	for (int a = 0; a < k.angleCount; a++)
	{
		k.exposedStand[a] = k.exposedCrouch[a] = 0;
		const CNavArea *from = s_know.areas[k.angles[a].area];
		Vector eye(from->GetCenter()->x, from->GetCenter()->y, from->GetCenter()->z + EYE);
		for (int p = 0; p < 9; p++)
		{
			if (Clear(eye, Vector(pts[p].x, pts[p].y, pts[p].z + EYE)))
				k.exposedStand[a] |= (unsigned short)(1 << p);
			if (Clear(eye, Vector(pts[p].x, pts[p].y, pts[p].z + CROUCH_HEAD)))
				k.exposedCrouch[a] |= (unsigned short)(1 << p);
		}
	}
}

void Path(char *out, size_t len)
{
	char gd[64]{};
	GET_GAME_DIR(gd);
	Q_snprintf(out, len, "%s/maps/%s.lsv", gd, s_know.map);
}

void Save()
{
	char path[MAX_OSPATH], tmp[MAX_OSPATH];
	Path(path, sizeof(path));
	Q_snprintf(tmp, sizeof(tmp), "%s.part", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return;
	FileHead head = { LSV_MAGIC, LSV_VERSION, s_know.key, (unsigned int)s_know.count, (unsigned int)s_know.stage,
		(unsigned int)s_know.i, (unsigned int)s_know.j, (unsigned int)s_know.pairs.size(), (unsigned int)s_know.bangs.size(),
		s_know.traces, (float)s_know.spentMs };
	bool ok = fwrite(&head, sizeof(head), 1, f) == 1
		&& fwrite(s_know.bits.data(), 1, s_know.bits.size(), f) == s_know.bits.size()
		&& (s_know.pairs.empty() || fwrite(s_know.pairs.data(), sizeof(PairInfo), s_know.pairs.size(), f) == s_know.pairs.size())
		&& (s_know.bangs.empty() || fwrite(s_know.bangs.data(), sizeof(BangInfo), s_know.bangs.size(), f) == s_know.bangs.size())
		&& fwrite(s_know.know.data(), sizeof(LsAreaKnow), s_know.know.size(), f) == s_know.know.size();
	fclose(f);
	if (!ok)
	{
		remove(tmp);
		return;
	}
	remove(path);
	if (rename(tmp, path) != 0)
		remove(tmp);
}

// What's saved for this map, if it's for this nav: done, or how far it got
bool Load()
{
	char rel[128];
	Q_snprintf(rel, sizeof(rel), "maps/%s.lsv", s_know.map);
	int len = 0;
	byte *data = LOAD_FILE_FOR_ME(rel, &len);
	if (!data)
		return false;
	bool ok = false;
	FileHead head;
	size_t n = s_know.count;
	if ((size_t)len >= sizeof(head))
	{
		Q_memcpy(&head, data, sizeof(head));
		// (counts from the file are checked against what fits before anything's read by them)
		size_t want = sizeof(head) + s_know.bits.size() + (size_t)head.pairs * sizeof(PairInfo) + (size_t)head.bangs * sizeof(BangInfo) + n * sizeof(LsAreaKnow);
		if (head.magic == LSV_MAGIC && head.version == LSV_VERSION && head.key == s_know.key && head.count == n && head.stage <= STAGE_DONE
			&& head.pairs <= n * n && head.bangs <= n * n && (size_t)len == want
			// (where it carries on from has to be a real place for its stage: a pair i < j < n,
			// or finished with them; an area up to n)
			&& (head.stage == STAGE_PAIRS ? (head.ci + 1 >= n || (head.ci < head.cj && head.cj < n)) : head.ci <= n))
		{
			const byte *p = data + sizeof(head);
			Q_memcpy(s_know.bits.data(), p, s_know.bits.size());
			p += s_know.bits.size();
			s_know.pairs.resize(head.pairs);
			if (head.pairs)
				Q_memcpy(s_know.pairs.data(), p, head.pairs * sizeof(PairInfo));
			p += head.pairs * sizeof(PairInfo);
			s_know.bangs.resize(head.bangs);
			if (head.bangs)
				Q_memcpy(s_know.bangs.data(), p, head.bangs * sizeof(BangInfo));
			p += head.bangs * sizeof(BangInfo);
			Q_memcpy(s_know.know.data(), p, n * sizeof(LsAreaKnow));
			// (indices in it are checked too: a damaged file isn't trusted)
			ok = true;
			for (const PairInfo &pi : s_know.pairs)
				if (pi.a >= n || pi.b >= n)
					ok = false;
			for (const BangInfo &bi : s_know.bangs)
				if (bi.a >= n || bi.b >= n)
					ok = false;
			for (LsAreaKnow &k : s_know.know)
			{
				k.cornerCount = Q_min(k.cornerCount, (unsigned char)LS_KNOW_CORNERS);
				k.angleCount = Q_min(k.angleCount, (unsigned char)LS_KNOW_ANGLES);
				k.objectCount = Q_min(k.objectCount, (unsigned char)LS_KNOW_OBJECTS);
				for (int a = 0; a < k.angleCount; a++)
					if (k.angles[a].area >= n)
						k.angleCount = (unsigned char)a;
			}
			if (ok)
			{
				s_know.stage = (int)head.stage;
				s_know.i = head.ci;
				s_know.j = head.cj;
				s_know.traces = head.traces;
				s_know.spentMs = head.spentMs;
			}
		}
	}
	FREE_FILE(data);
	if (!ok)
	{
		s_know.pairs.clear();
		s_know.bangs.clear();
		std::fill(s_know.bits.begin(), s_know.bits.end(), 0);
		s_know.know.assign(n, LsAreaKnow());
	}
	return ok;
}

void KnowLog(const char *what)
{
	char gd[64]{}, path[MAX_OSPATH];
	GET_GAME_DIR(gd);
	Q_snprintf(path, sizeof(path), "%s/lonerstrike_mapknow.log", gd);
	FILE *f = fopen(path, "a");
	if (!f)
		return;
	fprintf(f, "%s\n", what);
	fclose(f);
}

void Finish()
{
	size_t lows = 0, mapObjects = 0, corners = 0, angles = 0;
	for (const LsAreaKnow &k : s_know.know)
	{
		corners += k.cornerCount;
		angles += k.angleCount;
		for (int o = 0; o < k.objectCount; o++)
			(k.objects[o].type == LSO_LOW ? lows : mapObjects)++;
	}
	char line[512];
	Q_snprintf(line, sizeof(line), "%s: %d areas, %d pairs in sight, %d wallbang spots, %d corners, %d angles, %d low objects, %d map objects; %u traces, %.1f s of work, done %.0f s after it started on this visit",
		s_know.map, (int)s_know.count, (int)s_know.pairs.size(), (int)s_know.bangs.size(), (int)corners, (int)angles, (int)lows, (int)mapObjects,
		s_know.traces, s_know.spentMs / 1000.0, gpGlobals->time - s_know.startedAt);
	CONSOLE_ECHO("Loner-Strike: map knowledge for %s\n", line);
	KnowLog(line);
}

// A new map, or its nav changed: start again, or carry on from the file
void Reset()
{
	s_know.navLoads = g_lsNavLoads;
	s_know.ready = false;
	s_know.tried = true;
	s_know.stage = STAGE_PAIRS;
	Q_strlcpy(s_know.map, STRING(gpGlobals->mapname));
	s_know.count = TheNavAreaList.size();
	s_know.navSize = s_know.count;
	s_know.key = LsNavKey();
	s_know.areas.assign(TheNavAreaList.begin(), TheNavAreaList.end());
	s_know.pairs.clear();
	s_know.bangs.clear();
	s_know.inView.clear();
	// (area ids come from the map's .nav, which can come with a downloaded map: one far out of
	// range, or too many areas, and the map gets no knowledge rather than a huge table)
	size_t top = 0;
	for (CNavArea *a : s_know.areas)
		top = Q_max(top, (size_t)a->GetID());
	const size_t MAX_AREAS = 6000;				// (about 4.5 MB of sightlines; bigger maps go without)
	if (s_know.count > MAX_AREAS || top > 16 * s_know.count + 1024)
	{
		s_know.count = 0;
		s_know.areas.clear();
		s_know.index.clear();
		s_know.bits.clear();
		s_know.know.clear();
		return;
	}
	s_know.index.assign(top + 1, -1);
	for (size_t k = 0; k < s_know.areas.size(); k++)
		s_know.index[s_know.areas[k]->GetID()] = (int)k;
	s_know.bits.assign((s_know.count * s_know.count + 7) / 8, 0);
	s_know.know.assign(s_know.count, LsAreaKnow());
	s_know.i = 0;
	s_know.j = 1;
	s_know.spentMs = 0.0;
	s_know.traces = 0;
	s_know.startedAt = gpGlobals->time;
	s_know.lastSave = LsNowMs();
	if (s_know.count < 2)
		return;
	if (Load())
	{
		if (s_know.stage > STAGE_PAIRS)
		{
			BuildNear();
			s_know.ready = true;
		}
		if (s_know.stage == STAGE_DONE)
			CONSOLE_ECHO("Loner-Strike: map knowledge for %s loaded (%d areas).\n", s_know.map, (int)s_know.count);
	}
}
} // namespace

// A line through sky: traces go straight through sky brushes, but people see the sky there - a
// wall. Some maps cap rooftops and gaps with them (inferno: two lines in three over its roofs),
// so a "clear" trace through one isn't seen through. Checked every few units along the line
bool LsCrossesSky(const Vector &from, const Vector &to, float step)
{
	Vector d = to - from;
	float len = float(d.Length());
	if (len < step * 2.0f)
		return false;
	int steps = int(len / step);
	for (int i = 1; i < steps; i++)
	{
		Vector p = from + d * (float(i) / float(steps));
		if (POINT_CONTENTS(p) == CONTENTS_SKY)
			return true;
	}
	return false;
}

// Room to stand in an area: open at a crouched head's height above it. Nav learning sometimes
// leaves areas wedged under sky brushes or ceilings (inferno's rooftops: sky a few units above
// the floor) - nobody can be there, so they're never cover, a perch, or a way through
bool LsHeadroom(const CNavArea *area)
{
	if (!area)
		return false;
	Vector head = *area->GetCenter() + Vector(0, 0, 40.0f);
	int c = POINT_CONTENTS(head);
	return c != CONTENTS_SOLID && c != CONTENTS_SKY;
}

// How lit a spot is, from the map's own lighting: a hidden point entity moved there and the
// engine asked for its light (as it works out a player's). 128 if it can't tell
int LsLightAt(const Vector &pos)
{
	static EntityHandle<CBaseEntity> s_probe;
	CBaseEntity *pProbe = s_probe;
	if (!pProbe)
	{
		edict_t *e = CREATE_NAMED_ENTITY(MAKE_STRING("info_target"));
		if (FNullEnt(e))
			return 128;
		pProbe = CBaseEntity::Instance(e);
		if (!pProbe)
			return 128;
		pProbe->pev->effects |= EF_NODRAW;
		pProbe->pev->solid = SOLID_NOT;
		pProbe->pev->movetype = MOVETYPE_NONE;
		pProbe->pev->classname = MAKE_STRING("ls_light_probe");
		s_probe = pProbe;
	}
	UTIL_SetOrigin(pProbe->pev, pos + Vector(0, 0, 36.0f));
	return GETENTITYILLUM(pProbe->edict());
}

// Whether this map has sky brushes inside the playing space (between places people stand): one
// look per map at a few hundred lines between nav areas; most maps don't, and then the bots'
// sight checks skip the sky test
bool LsSkyInPlay()
{
	static int s_serial = -1;
	static bool s_sky = false;
	if (s_serial == LsMapSerial())
		return s_sky;
	s_serial = LsMapSerial();
	s_sky = false;
	int n = (int)TheNavAreaList.size();
	if (n < 2 || TheCSBots()->IsLearningMap())
	{
		s_serial = -1;	// (no nav yet: looked at again once there is one)
		return false;
	}
	std::vector<CNavArea *> list(TheNavAreaList.begin(), TheNavAreaList.end());
	unsigned int seed = 12345u;
	auto next = [&seed]() { seed = seed * 1103515245u + 12345u; return (seed >> 8) & 0xffffff; };
	for (int t = 0; t < 400 && !s_sky; t++)
	{
		const CNavArea *a = list[next() % n], *b = list[next() % n];
		Vector ea = *a->GetCenter() + Vector(0, 0, 64.0f), eb = *b->GetCenter() + Vector(0, 0, 64.0f);
		if ((ea - eb).IsLengthGreaterThan(3000.0f) || (ea - eb).IsLengthLessThan(300.0f))
			continue;
		TraceResult tr;
		UTIL_TraceLine(ea, eb, ignore_monsters, ignore_glass, nullptr, &tr);
		if (tr.flFraction >= 1.0f && LsCrossesSky(ea, eb, 48.0f))
			s_sky = true;
	}
	return s_sky;
}


// A little more of the map's knowledge, within this frame's budget (CCSBotManager::StartFrame)
void LsSightStep()
{
	if (!LsFight() || TheNavAreaList.empty() || !gpGlobals->mapname)
		return;
	// (bots learning the map: its nav is still being built; the knowledge waits for the finished one)
	if (TheCSBots() && (TheCSBots()->IsLearningMap() || TheCSBots()->IsAnalysisRequested()))
		return;
	// (the nav replaced, even with the same number of areas: its first area is a new one)
	if (!s_know.tried || Q_strcmp(s_know.map, STRING(gpGlobals->mapname)) != 0 || (s_know.navLoads != g_lsNavLoads && !TheNavAreaList.empty())
		|| (!s_know.areas.empty() && (s_know.count != TheNavAreaList.size() || s_know.areas.front() != TheNavAreaList.front()))
		|| (s_know.areas.empty() && s_know.navSize != TheNavAreaList.size()))
		Reset();
	if (s_know.stage == STAGE_DONE || s_know.count < 2)
		return;

	double start = LsNowMs();
	size_t n = s_know.count;
	int sinceCheck = 0;
	// (more while nobody's playing: freeze time, everyone dead, or no people at all)
	double budget = LSV_BUDGET_MS;
	{
		int alive = 0, humans = 0;
		for (int p = 1; p <= gpGlobals->maxClients; p++)
		{
			CBasePlayer *pl = UTIL_PlayerByIndex(p);
			if (!pl || pl->IsBot() || FNullEnt(pl->edict()) || pl->has_disconnected)
				continue;
			humans++;
			if (pl->IsAlive())
				alive++;
		}
		if (humans == 0)
			budget = 4.0;
		else if (alive == 0 || (CSGameRules() && CSGameRules()->IsFreezePeriod()))
			budget = 2.0;
	}
	auto out = [&]() {
		if (++sinceCheck < 4)
			return false;
		sinceCheck = 0;
		return LsNowMs() - start >= budget;
	};
	while (s_know.stage != STAGE_DONE)
	{
		if (s_know.stage == STAGE_PAIRS)
		{
			if (s_know.i + 1 >= n)
			{
				s_know.stage = STAGE_AREAS;
				s_know.i = 0;
				BuildNear();
				s_know.ready = true;
				Save();
				continue;
			}
			Pair(s_know.i, s_know.j);
			if (++s_know.j >= n)
			{
				s_know.i++;
				s_know.j = s_know.i + 1;
			}
		}
		else if (s_know.stage == STAGE_AREAS)
		{
			if (s_know.i >= n)
			{
				MapObjects();
				s_know.stage = STAGE_ANGLES;
				s_know.i = 0;
				Save();
				continue;
			}
			Area(s_know.i++);
		}
		else if (s_know.stage == STAGE_ANGLES)
		{
			if (s_know.i >= n)
			{
				s_know.stage = STAGE_EXPOSURE;
				s_know.i = 0;
				Save();
				continue;
			}
			Angles(s_know.i++);
		}
		else
		{
			if (s_know.i >= n)
			{
				s_know.stage = STAGE_DONE;
				s_know.spentMs += LsNowMs() - start;
				Save();
				Finish();
				return;
			}
			Exposure(s_know.i++);
		}
		if (out())
			break;
	}
	double now = LsNowMs();
	s_know.spentMs += now - start;
	if (now - s_know.lastSave >= LSV_SAVE_EVERY_MS)
	{
		s_know.lastSave = now;
		Save();
	}
}

bool LsSightReady()
{
	return s_know.ready;
}

static int LsKnowIndex(const CNavArea *a)
{
	if (!a || s_know.index.empty() || a->GetID() >= s_know.index.size())
		return -1;
	return s_know.index[a->GetID()];
}

// How long a team needs, running from its spawn, to reach each area: a shortest walk over the
// nav (ladders too), at a knife's running speed so it's the earliest anyone could be there. Built
// once a map per team, the first time it's asked (a few milliseconds), once the map knowledge
// (which numbers the areas) is in
static std::vector<float> s_arrive[2];
static int s_arriveSerial[2] = { -1, -1 };

float LsArrival(const CNavArea *area, int team)
{
	int t = team == TERRORIST ? 0 : team == CT ? 1 : -1;
	int idx = LsKnowIndex(area);
	if (t < 0 || idx < 0 || !s_know.ready)
		return -1.0f;
	if (s_arriveSerial[t] != LsMapSerial() || s_arrive[t].size() != (size_t)s_know.count)
	{
		s_arriveSerial[t] = LsMapSerial();
		std::vector<float> &d = s_arrive[t];
		d.assign((size_t)s_know.count, -1.0f);
		// (that team's own spawn: LsEnemySpawn gives the other side's)
		const Vector *home = LsEnemySpawn(team == TERRORIST ? CT : TERRORIST);
		if (!home)
			return -1.0f;
		Vector h = *home;
		int si = LsKnowIndex(TheNavAreaGrid.GetNearestNavArea(&h));
		if (si < 0)
			return -1.0f;
		typedef std::pair<float, int> Item;
		std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
		d[si] = 0.0f;
		open.push(Item(0.0f, si));
		const float speed = 250.0f;
		auto relax = [&](float at, const CNavArea *to, float extra) {
			int j = LsKnowIndex(to);
			if (j < 0 || !LsHeadroom(to))
				return;
			if (d[j] < 0.0f || at + extra < d[j])
			{
				d[j] = at + extra;
				open.push(Item(d[j], j));
			}
		};
		while (!open.empty())
		{
			Item it = open.top();
			open.pop();
			if (it.first > d[it.second])
				continue;
			const CNavArea *a = s_know.areas[it.second];
			const Vector &ac = *a->GetCenter();
			for (int dir = 0; dir < NUM_DIRECTIONS; dir++)
			{
				int n = a->GetAdjacentCount((NavDirType)dir);
				for (int i = 0; i < n; i++)
				{
					const CNavArea *b = a->GetAdjacentArea((NavDirType)dir, i);
					if (!b)
						continue;
					const Vector &bc = *b->GetCenter();
					// (a jump up costs a moment)
					float extra = float((bc - ac).Length()) / speed + (bc.z - ac.z > 20.0f ? 0.3f : 0.0f);
					relax(it.first, b, extra);
				}
			}
			for (int ld = 0; ld < 2; ld++)
			{
				const NavLadderList *list = a->GetLadderList((LadderDirectionType)ld);
				for (NavLadderList::const_iterator li = list->begin(); li != list->end(); ++li)
				{
					const CNavLadder *ladder = *li;
					// (climbing is slow: about half running speed, plus getting on and off)
					float climb = ladder->m_length / 130.0f + 0.5f;
					const CNavArea *ends[5] = { ladder->m_topForwardArea, ladder->m_topLeftArea, ladder->m_topRightArea, ladder->m_topBehindArea, ladder->m_bottomArea };
					for (int e = 0; e < 5; e++)
						if (ends[e] && ends[e] != a)
							relax(it.first, ends[e], climb);
				}
			}
		}
	}
	return s_arrive[t][(size_t)idx];
}

// Whether someone in one area could see into the other (false until the map's are ready)
bool LsSees(const CNavArea *a, const CNavArea *b)
{
	if (!s_know.ready)
		return false;
	int x = LsKnowIndex(a), y = LsKnowIndex(b);
	if (x < 0 || y < 0)
		return false;
	return x == y || BitSees((size_t)x, (size_t)y);
}

// How much of one area is in view from the other: 0 (none) to 255 (wide open)
int LsSightQuality(const CNavArea *a, const CNavArea *b)
{
	if (!s_know.ready)
		return 0;
	int x = LsKnowIndex(a), y = LsKnowIndex(b);
	if (x < 0 || y < 0)
		return 0;
	if (x == y)
		return 255;
	auto &list = s_know.inView[x];
	auto it = std::lower_bound(list.begin(), list.end(), std::make_pair((unsigned short)y, (unsigned char)0));
	return (it != list.end() && it->first == y) ? it->second : 0;
}

// What's known about an area (cover, corners, angles, objects); null until that's worked out
const LsAreaKnow *LsKnow(const CNavArea *a)
{
	if (s_know.stage <= STAGE_ANGLES)
		return nullptr;
	int x = LsKnowIndex(a);
	return x < 0 ? nullptr : &s_know.know[x];
}

// The area an angle looks at
CNavArea *LsKnowArea(unsigned short index)
{
	return index < s_know.areas.size() ? s_know.areas[index] : nullptr;
}

// A wallbang spot between the two (one thin layer): its thickness and material
bool LsWallbangKnown(const CNavArea *a, const CNavArea *b, float *thick, char *mat)
{
	if (s_know.stage < STAGE_AREAS)
		return false;
	int x = LsKnowIndex(a), y = LsKnowIndex(b);
	if (x < 0 || y < 0)
		return false;
	unsigned short lo = (unsigned short)Q_min(x, y), hi = (unsigned short)Q_max(x, y);
	for (const BangInfo &bi : s_know.bangs)
	{
		if (bi.a == lo && bi.b == hi)
		{
			if (thick) *thick = bi.thick;
			if (mat) *mat = (char)bi.mat;
			return true;
		}
	}
	return false;
}

bool LsExposureReady()
{
	return s_know.stage == STAGE_DONE;
}

// One of an area's 9 exposure points: a 3 x 3 grid across it, a fifth in from its edges, on its floor
void LsExposurePoint(const CNavArea *area, int point, Vector *out)
{
	// (the outer ones 20 units in from its edges: where someone hugging a wall or a box stands;
	// a side too narrow for that keeps to the middle)
	const Extent *e = area->GetExtent();
	auto at = [](float lo, float hi, int k) {
		float mid = 0.5f * (lo + hi);
		if (k == 1 || hi - lo < 60.0f)
			return mid;
		return k == 0 ? lo + 20.0f : hi - 20.0f;
	};
	Vector p(at(e->lo.x, e->hi.x, point % 3), at(e->lo.y, e->hi.y, point / 3), 0.0f);
	p.z = area->GetZ(&p);
	*out = p;
}

// Whether this bot knows a part of the game beyond the basics (see LSK_*): the better it is the
// more it knows, a newer player some of it, and its personality leans it (an AWPer knows the
// angles, a nader the lineups, a rusher the timings, a lurker how to keep quiet and out of
// sight). Decided once per bot (from its name), so it knows the same things every game
bool CCSBot::LsKnows(int fact)
{
	if (!LsFight() || fact < 0 || fact >= LSK_COUNT)
		return false;
	float s = LsSkill();
	static const float FLOOR[LSK_COUNT] = { 0.15f, 0.35f, 0.25f, 0.4f, 0.1f, 0.05f, 0.3f, 0.35f, 0.2f, 0.2f };
	float p = FLOOR[fact] + (1.0f - FLOOR[fact]) * s;
	int style = LsStyle();
	switch (fact)
	{
	case LSK_TIMINGS: if (style == LS_STYLE_RUSHER) p += 0.25f; break;
	case LSK_LINEUPS: if (style == LS_STYLE_NADER) p += 0.4f; break;
	case LSK_QUIET: case LSK_EXPOSURE:
		if (style == LS_STYLE_LURKER || style == LS_STYLE_NINJA || style == LS_STYLE_CAREFUL) p += 0.2f;
		else if (style == LS_STYLE_RUSHER) p -= 0.2f;
		break;
	case LSK_ECONOMY: case LSK_DEFUSE_MATH: if (style == LS_STYLE_OBJECTIVE || style == LS_STYLE_CAREFUL) p += 0.15f; break;
	case LSK_CROSSHAIR: if (style == LS_STYLE_AWPER || style == LS_STYLE_CAMPER) p += 0.15f; break;
	case LSK_PENETRATION: case LSK_WALLBANG_SPOTS: if (style == LS_STYLE_AWPER) p += 0.15f; break;
	}
	return LsTrait(200 + fact) < p;
}


// ---- map kind and spawns ------------------------------------------------------------------

namespace
{
char s_kindMap[64] = {};
int s_kind = LSMAP_OBJECTIVE;
int s_mapSerial = 0;
static int s_kindNavLoads = -1;
Vector s_spawn[2];
bool s_hasSpawn[2] = {};

void LsMapKindUpdate()
{
	if (!gpGlobals->mapname || (Q_strcmp(s_kindMap, STRING(gpGlobals->mapname)) == 0 && s_kindNavLoads == g_lsNavLoads))
		return;
	Q_strlcpy(s_kindMap, STRING(gpGlobals->mapname));
	s_kindNavLoads = g_lsNavLoads;
	s_mapSerial++;
	auto starts = [](const char *prefix) { return Q_strnicmp(s_kindMap, prefix, Q_strlen(prefix)) == 0; };
	if (starts("aim_") || starts("35hp_") || starts("1v1") || starts("aim-"))
		s_kind = LSMAP_AIM;
	else if (starts("fy_"))
		s_kind = LSMAP_FY;
	else if (starts("awp_") || starts("awp-"))
		s_kind = LSMAP_AWP;
	else if (starts("ka_") || starts("knife"))
		s_kind = LSMAP_KNIFE;
	else if (TheCSBots() && TheCSBots()->GetScenario() != CCSBotManager::SCENARIO_DEATHMATCH)
		s_kind = LSMAP_OBJECTIVE;
	else
		s_kind = LSMAP_OTHER;
	// the middle of each team's spawn (Ts: info_player_deathmatch, CTs: info_player_start)
	const char *cls[2] = { "info_player_deathmatch", "info_player_start" };
	for (int t = 0; t < 2; t++)
	{
		Vector sum(0, 0, 0);
		int n = 0;
		CBaseEntity *pEnt = nullptr;
		while ((pEnt = UTIL_FindEntityByClassname(pEnt, cls[t])) != nullptr && n < 64)
		{
			sum = sum + pEnt->pev->origin;
			n++;
		}
		s_hasSpawn[t] = n > 0;
		if (n)
			s_spawn[t] = sum / float(n);
	}
}
} // namespace

int LsMapKind()
{
	LsMapKindUpdate();
	return s_kind;
}

const Vector *LsEnemySpawn(int team)
{
	LsMapKindUpdate();
	int other = team == TERRORIST ? 1 : 0;
	return s_hasSpawn[other] ? &s_spawn[other] : nullptr;
}

// ---- scanning the angles -------------------------------------------------------------------
//
// Where a person looks moving through (or holding) a place: the ways an enemy could come in
// (the area's angles from the map knowledge: places in sight that lead out of sight), at the
// height they're at, the most likely first: busy ways, ways from the enemy's side of the map,
// near a noise or where an enemy was seen; less the ones a teammate is already watching or
// it checked a moment ago, and (moving) fewer behind it. A good player checks them in a
// tidy order, one after another; a newer one checks fewer, later, and a bit at random.
// Everyone for themselves in deathmatch: every way counts the same. True if it looked.
bool CCSBot::LsScanAngles(bool holding)
{
	// (not on a ladder: there the view decides the way it climbs - a glance aside mid-climb has
	// it go up and down at the top and step off the side)
	if (!LsFight() || !m_lastKnownArea || IsOnLadder())
		return false;
	float now = gpGlobals->time;
	// (the stock hide state's approach-point look has no end: it doesn't keep the angles from
	// being checked in turn while holding)
	bool approachLook = m_lookAtDesc && !Q_strcmp(m_lookAtDesc, "Approach Point (Hiding)");
	if (now < m_lsScanNext || (IsLookingAtSpot(PRIORITY_LOW) && !(holding && approachLook)))
		return false;
	const LsAreaKnow *k = LsKnow(m_lastKnownArea);
	if (!k || k->angleCount == 0)
		return false;
	float skill = LsSkill();
	int style = LsStyle();
	// (a newer player doesn't always check: it just looks where it's going for a bit)
	if (!holding && RANDOM_FLOAT(0.0f, 1.0f) > 0.45f + 0.55f * skill)
	{
		m_lsScanNext = now + RANDOM_FLOAT(0.8f, 1.6f);
		return false;
	}
	bool dm = LsSolo();
	const Vector *enemyHome = dm ? nullptr : LsEnemySpawn(m_iTeam);
	bool fresh = now - m_lsScanInAt < 2.0f || LsAboutToEnter();
	Vector eye = GetEyePosition();
	float heading = pev->velocity.Length2D() > 60.0f ? UTIL_VecToYaw(pev->velocity) : pev->v_angle.y;
	bool noiseFresh = IsNoiseHeard() && now - m_noiseTimestamp < 5.0f;
	bool sawFresh = GetTimeSinceLastSawEnemy() < 10.0f;

	float bestScore = -1.0f, scores[LS_KNOW_ANGLES];
	bool behind[LS_KNOW_ANGLES] = {};
	bool smoked[LS_KNOW_ANGLES] = {};
	int bestA = -1;
	Vector at[LS_KNOW_ANGLES];
	for (int a = 0; a < k->angleCount; a++)
	{
		scores[a] = -1.0f;
		const LsKnownAngle &an = k->angles[a];
		CNavArea *target = LsKnowArea(an.area);
		if (!target)
			continue;
		const Vector &tc = *target->GetCenter();
		at[a] = Vector(tc.x, tc.y, tc.z + HalfHumanHeight + LsLookHeight());
		// (the angles were worked out for the area as a whole; from where the bot actually is, a
		// big area's angle can be behind the wall right beside it - a check that way would be a
		// stare at bare wall: only angles it can actually see down from here)
		{
			TraceResult tr;
			UTIL_TraceLine(eye, at[a], ignore_monsters, ENT(pev), &tr);
			float len = float((at[a] - eye).Length());
			// (it has to actually see the place: the line gets there, or to within a step or so
			// of it - a canyon wall 400 units off on a 1500-unit line is still a wall stare)
			if (tr.flFraction < 1.0f && len - tr.flFraction * len > Q_max(120.0f, 0.12f * len))
				continue;
		}
		float score = 1.0f + float(an.weight);
		// (the enemy's side of the map first)
		if (enemyHome)
			score *= (tc - *enemyHome).Length2D() < (pev->origin - *enemyHome).Length2D() ? 1.5f : 0.7f;
		// (somewhere the enemy can't have got to yet this round matters less; the kill feed's
		// spot - a teammate just died there - more; a person's favourite spots, to a veteran)
		score *= LsTimingWeight(target);
		// (behind a smoke: nothing to see there - not held through it)
		if (TheCSBots()->IsLineBlockedBySmoke(&eye, &at[a]))
		{
			smoked[a] = true;
			score *= 0.25f;
		}
		if (now - m_lsFeedAt < 12.0f && (tc - m_lsFeedSpot).IsLengthLessThan(700.0f))
			score *= m_lsFeedSniper ? 2.0f : 1.6f;
		if (skill > 0.55f && LsHabitNear(tc, 450.0f))
			score *= 1.5f;
		// (a CT going for a planted bomb watches the places that see it: where the Ts guard it
		// from)
		if (m_iTeam == CT && skill > 0.35f && TheCSBots()->IsBombPlanted() && GetGameState()->GetBombPosition())
		{
			Vector bomb = *GetGameState()->GetBombPosition();
			CNavArea *bombArea = TheNavAreaGrid.GetNearestNavArea(&bomb);
			if (bombArea && target != bombArea && LsSees(target, bombArea) && (tc - bomb).IsLengthLessThan(1000.0f))
				score *= 1.8f;
		}
		// (where an AWPer was heard scoping lately: the better ones keep an eye on it)
		if (skill > 0.4f && now - m_lsSniperHeardAt < 15.0f && (tc - m_lsSniperSpot).IsLengthLessThan(600.0f))
			score *= 1.8f;
		// (near something heard or seen lately)
		if ((noiseFresh && (tc - m_noisePosition).IsLengthLessThan(600.0f)) || (sawFresh && (tc - GetLastKnownEnemyPosition()).IsLengthLessThan(600.0f)))
			score *= 2.5f;
		// (someone it backed off from, still remembered: that way is watched - the better ones
		// and the careful keep coming back to it)
		else if (LsThreatActive() && (tc - m_lsThreatPos).IsLengthLessThan(600.0f))
			score *= 1.5f + 1.2f * skill + ((style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER) ? 0.5f : 0.0f);
		// (moving: ahead first, nobody walks backwards into somewhere unchecked; behind only
		// once what's ahead is checked, and then just a quick look over the shoulder)
		float off = float(Q_abs(NormalizeAngle(UTIL_VecToYaw(tc - pev->origin) - heading)));
		behind[a] = !holding && off > 100.0f;
		if (behind[a])
			score *= fresh ? 0.03f : (style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER) ? 0.25f : 0.1f;
		// (checked a moment ago)
		// (the same place, by where it is: walking along, each step's areas list their own
		// neighbouring bits of the same far spot - those count as the one it just checked)
		for (int r = 0; r < 8; r++)
		{
			if (!m_lsScanArea[r] || now - m_lsScanAt[r] >= (holding ? 6.0f : 4.0f) || m_lsScanArea[r] == an.area + 1)
				continue;
			CNavArea *was = LsKnowArea(m_lsScanArea[r] - 1);
			if (was && (*was->GetCenter() - tc).IsLengthLessThan(250.0f))
				score *= 0.15f;
		}
		for (int r = 0; r < 8; r++)
			if (m_lsScanArea[r] == an.area + 1 && now - m_lsScanAt[r] < (holding ? 6.0f : 4.0f))
				score *= 0.2f;
		// (holding a spot: one not looked at for a good while gets its turn - every way in,
		// checked in rotation, not one stared at)
		if (holding)
		{
			float lastAt = -999.0f;
			for (int r = 0; r < 8; r++)
				if (m_lsScanArea[r] == an.area + 1)
					lastAt = Q_max(lastAt, m_lsScanAt[r]);
			// (a good player sweeps every way in, in turn; a newer one's checks are patchier -
			// one stared at longer, another forgotten)
			if (now - lastAt > 10.0f)
				score *= 1.0f + 0.8f * skill;
		}
		// (a teammate close by already watching it: on the radar, looking that way)
		bool covered = false;
		for (int m = 1; m <= gpGlobals->maxClients && !covered; m++)
		{
			CBasePlayer *mate = UTIL_PlayerByIndex(m);
			if (!mate || mate == this || !mate->IsAlive() || BotRelationship(mate) != BOT_TEAMMATE || (mate->pev->origin - pev->origin).IsLengthGreaterThan(1200.0f))
				continue;
			// (the radar shows where they are, not where they look: one holding still where it
			// can see that spot is taken to be watching it)
			if (mate->pev->velocity.Length2D() > 60.0f)
				continue;
			Vector mp = mate->pev->origin;
			CNavArea *mateArea = mate->IsBot() ? static_cast<CCSBot *>(mate)->GetLastKnownArea() : TheNavAreaGrid.GetNearestNavArea(&mp);
			if (mateArea && LsSees(mateArea, target))
				covered = true;
		}
		if (covered)
			score *= 0.25f;
		scores[a] = score;
		if (score > bestScore)
		{
			bestScore = score;
			bestA = a;
		}
	}
	// (nothing it can see down from here: not again every think - the sight traces wait a moment)
	if (bestA < 0)
	{
		m_lsScanNext = now + RANDOM_FLOAT(0.4f, 0.8f);
		m_lsScanBlindUntil = m_lsScanNext + 0.2f;
		return false;
	}
	// (an angle ahead not checked yet goes before any behind)
	if (behind[bestA])
	{
		int ahead = -1;
		for (int a = 0; a < k->angleCount; a++)
			if (scores[a] >= 0.0f && !behind[a] && (ahead < 0 || scores[a] > scores[ahead]))
				ahead = a;
		bool aheadChecked = true;
		if (ahead >= 0)
		{
			aheadChecked = false;
			for (int r = 0; r < 8; r++)
				if (m_lsScanArea[r] == k->angles[ahead].area + 1 && now - m_lsScanAt[r] < 4.0f)
					aheadChecked = true;
		}
		if (ahead >= 0 && (!aheadChecked || fresh))
			bestA = ahead;
	}
	// a newer player's pick is looser: one of the top few, not always the best (but not one
	// behind it while walking in fresh)
	if (RANDOM_FLOAT(0.0f, 1.0f) > 0.4f + 0.6f * skill)
	{
		int pick = RANDOM_LONG(0, k->angleCount - 1);
		if (scores[pick] >= 0.0f && !(fresh && behind[pick]))
			bestA = pick;
	}
	// (all it's got is behind it, just walked in: nothing now, and nothing marked as checked)
	if (fresh && behind[bestA] && !holding)
	{
		m_lsScanNext = now + 0.3f;
		return false;
	}
	Vector look = at[bestA];
	// (a busy angle smoked off: the better ones watch the smoke's near edge, where a push
	// comes out of it)
	if (skill > 0.45f)
	{
		int top = -1;
		for (int a = 0; a < k->angleCount; a++)
			if (smoked[a] && (top < 0 || k->angles[a].weight > k->angles[top].weight))
				top = a;
		if (top >= 0 && RANDOM_FLOAT(0.0f, 1.0f) < 0.5f)
		{
			Vector step = at[top] - eye;
			float len = float(step.Length());
			if (len > 1.0f)
			{
				step = step * (48.0f / len);
				Vector p = eye;
				for (int i = 0; i < 40 && (p - eye).IsLengthLessThan(len); i++)
				{
					Vector next = p + step;
					if (TheCSBots()->IsInsideSmokeCloud(&next))
						break;
					p = next;
				}
				if ((p - eye).IsLengthGreaterThan(100.0f))
				{
					look = p;
					bestA = top;
				}
			}
		}
	}
	LsLookableSpot(&look);
	// (the map knowledge says this area sees it, from the area's middle: from where it actually
	// stands, at an edge of a big area, a wall can be in the way - walking along glancing at a
	// wall. Blocked close by: not this one, and counted as checked so it isn't picked again)
	{
		float span = float((look - eye).Length());
		float clear = span > 1.0f ? span * LsClearTo(eye, look, ENT(pev)) : span;
		if (clear < Q_min(250.0f, 0.5f * span))
		{
			m_lsScanArea[m_lsScanNextSlot] = (unsigned short)(k->angles[bestA].area + 1);
			m_lsScanAt[m_lsScanNextSlot] = now;
			m_lsScanNextSlot = (m_lsScanNextSlot + 1) % 8;
			m_lsScanNext = now + 0.15f;
			return false;
		}
	}
	// how long on it: a good player a quick, sure check; careful ones and campers longer; holding
	// a spot, longer still
	// (moving: quick checks, a flick over the shoulder for one behind; holding: longer looks)
	float hold = holding ? RANDOM_FLOAT(0.9f, 1.8f) + 0.5f * (1.0f - skill) : RANDOM_FLOAT(0.3f, 0.6f) + 0.3f * (1.0f - skill);
	if (style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER)
		hold *= 1.25f;
	if (!holding && behind[bestA])
		hold = RANDOM_FLOAT(0.2f, 0.35f);
	SetLookAt("Loner-Strike: angle", &look, PRIORITY_LOW, hold, true, 8.0f);
	m_lsScanArea[m_lsScanNextSlot] = (unsigned short)(k->angles[bestA].area + 1);
	m_lsScanAt[m_lsScanNextSlot] = now;
	m_lsScanNextSlot = (m_lsScanNextSlot + 1) % 8;
	m_lsScanNext = now + hold * 0.8f + RANDOM_FLOAT(0.1f, 0.5f) * (1.0f - skill);
	return true;
}

// An enemy bumping into it or brushing past (closer than a body's width, any direction): a
// person feels that and turns round. A good player at once, a newer one after a moment
void CCSBot::LsBumpCheck()
{
	if (!LsFight() || !IsAlive())
		return;
	float now = gpGlobals->time;
	if (m_lsBumpAt > 0.0f)
	{
		if (now < m_lsBumpAt)
			return;
		m_lsBumpAt = 0.0f;
		CBasePlayer *pFoe = UTIL_PlayerByIndex(m_lsBumpBy);
		if (!pFoe || !pFoe->IsAlive() || !IsEnemy(pFoe) || (pFoe->pev->origin - pev->origin).IsLengthGreaterThan(110.0f))
			return;
		Vector at = pFoe->pev->origin + pFoe->pev->view_ofs * 0.8f;
		SetLookAt("Loner-Strike: bumped", &at, PRIORITY_HIGH, 0.8f, false);
		if (IsVisible(pFoe, false))
			Attack(pFoe);
		LsWhy("bumped");
		return;
	}
	if (now < m_lsBumpCheckAt || IsAttacking())
		return;
	m_lsBumpCheckAt = now + 0.1f;
	for (int m = 1; m <= gpGlobals->maxClients; m++)
	{
		CBasePlayer *pFoe = UTIL_PlayerByIndex(m);
		if (!pFoe || pFoe == this || !pFoe->IsAlive() || !IsEnemy(pFoe))
			continue;
		Vector d = pFoe->pev->origin - pev->origin;
		if (d.Length2D() > 44.0f || Q_abs(d.z) > 72.0f)
			continue;
		// (really touching: not the other side of a thin wall or floor)
		TraceResult tr;
		UTIL_TraceLine(pev->origin, pFoe->pev->origin, ignore_monsters, ENT(pev), &tr);
		if (tr.flFraction < 1.0f)
			continue;
		m_lsBumpBy = m;
		m_lsBumpAt = now + 0.12f + 0.5f * (1.0f - LsSkill());
		return;
	}
}


// ---- moving out of sight ---------------------------------------------------------------------

// Teammates' areas now (for the path cost: not following one down the same corridor), and
// what a teammate there adds to the way (every bot for itself: more; team play: a little)
int CCSBot::LsCrowdAreas(CNavArea **out, int max, float *cost)
{
	*cost = LsSolo() ? 400.0f : 120.0f;
	if (!LsFight())
		return 0;
	int n = 0;
	for (int m = 1; m <= gpGlobals->maxClients && n < max; m++)
	{
		CBasePlayer *mate = UTIL_PlayerByIndex(m);
		if (!mate || mate == this || !mate->IsAlive() || BotRelationship(mate) != BOT_TEAMMATE)
			continue;
		CNavArea *area = mate->IsBot() ? static_cast<CCSBot *>(mate)->GetLastKnownArea() : TheNavAreaGrid.GetNavArea(&mate->pev->origin);
		if (area)
			out[n++] = area;
	}
	return n;
}

// Walking into `area` across its `dir` side: which side (1 its left, -1 its right) is less
// seen from the area's angles on the enemy's side of the map (the exposure grid); 0 when it's
// the same or not known yet
float CCSBot::LsSafeSide(const CNavArea *area, NavDirType dir)
{
	if (!LsExposureReady() || !area)
		return 0.0f;
	const LsAreaKnow *k = LsKnow(area);
	if (!k || k->angleCount == 0)
		return 0.0f;
	const Vector *enemyHome = LsEnemySpawn(m_iTeam);
	// its left: going north (-y) +x, east +y, south -x, west -y (as LsShiftPortalPoint)
	Vector2D left = dir == NORTH ? Vector2D(1, 0) : dir == EAST ? Vector2D(0, 1) : dir == SOUTH ? Vector2D(-1, 0) : Vector2D(0, -1);
	const Vector &c = *area->GetCenter();
	float onLeft = 0.0f, onRight = 0.0f;
	for (int p = 0; p < 9; p++)
	{
		Vector pt;
		LsExposurePoint(area, p, &pt);
		float lateral = (pt.x - c.x) * left.x + (pt.y - c.y) * left.y;
		if (Q_abs(lateral) < 8.0f)
			continue;
		float seen = 0.0f;
		for (int a = 0; a < k->angleCount; a++)
		{
			if (!(k->exposedStand[a] >> p & 1))
				continue;
			CNavArea *from = LsKnowArea(k->angles[a].area);
			float w = 1.0f;
			if (enemyHome && from)
				w = (*from->GetCenter() - *enemyHome).Length2D() < (c - *enemyHome).Length2D() ? 1.5f : 0.5f;
			seen += w;
		}
		(lateral > 0.0f ? onLeft : onRight) += seen;
	}
	float total = onLeft + onRight;
	if (total < 0.5f)
		return 0.0f;
	return Q_max(-1.0f, Q_min(1.0f, (onRight - onLeft) / total));
}

// Walking past a low wall that hides a crouched player from an angle on the enemy's side (and
// not a standing one): the ones who know crouch past it, a few steps; up again after, or as
// soon as anything happens. Never in deathmatch, never in a hurry
void CCSBot::LsCrouchWalk()
{
	auto stop = [this]() {
		if (m_lsCrouchWalking)
		{
			m_lsCrouchWalking = false;
			StandUp();
		}
	};
	if (!LsFight() || LsSolo() || !LsExposureReady() || IsAttacking() || IsHurrying() || LsRushing() || IsNotMoving() || IsSafe() || GetTimeSinceLastSawEnemy() < 3.0f
		|| TheCSBots()->IsBombPlanted() || IsCarryingBomb() || IsUsingLadder() || !m_lastKnownArea || LsStyle() == LS_STYLE_RUSHER || !LsKnows(LSK_EXPOSURE))
	{
		stop();
		return;
	}
	float now = gpGlobals->time;
	// (a few steps at most: then up and on, and not again for a bit; never when it's stuck or a
	// jump is coming up on its path: a crouching bot can't jump)
	if (m_lsCrouchWalking && now - m_lsCrouchWalkSince > 3.5f)
	{
		stop();
		m_lsCrouchWalkNext = now + 4.0f;
		return;
	}
	if (now < m_lsCrouchWalkNext || m_isStuck || IsNearJump() || IsJumping())
	{
		stop();
		return;
	}
	const LsAreaKnow *k = LsKnow(m_lastKnownArea);
	if (!k || k->angleCount == 0)
	{
		stop();
		return;
	}
	// the nearest of the area's points
	int nearest = 0;
	float bestD = 1e9f;
	for (int p = 0; p < 9; p++)
	{
		Vector pt;
		LsExposurePoint(m_lastKnownArea, p, &pt);
		float d = (pt - pev->origin).Length2D();
		if (d < bestD)
		{
			bestD = d;
			nearest = p;
		}
	}
	const Vector *enemyHome = LsEnemySpawn(m_iTeam);
	bool hides = false;
	for (int a = 0; a < k->angleCount && !hides; a++)
	{
		if ((k->exposedStand[a] >> nearest & 1) && !(k->exposedCrouch[a] >> nearest & 1))
		{
			CNavArea *from = LsKnowArea(k->angles[a].area);
			if (!enemyHome || !from || (*from->GetCenter() - *enemyHome).Length2D() < (pev->origin - *enemyHome).Length2D())
				hides = true;
		}
	}
	if (hides)
	{
		if (!m_lsCrouchWalking)
		{
			m_lsCrouchWalking = true;
			m_lsCrouchWalkSince = now;
		}
		Crouch();
	}
	else
		stop();
}


// ---- doors worked by a button ----------------------------------------------------------------
//
// A door that opens from a button on the wall beside it (the map links them: the button's
// target is the door's name) doesn't open by walking into it: the game plays the "locked"
// buzzer. A bot held up at one like that finds the button wired to it, walks over, looks at it
// and presses it. One press at a time per button (a toggle pressed twice closes the door again).

namespace
{
float s_buttonPressedAt[MAX_EDICTS] = {};		// by entity index, when a bot last pressed it

float &ButtonPressed(int index)
{
	static float none = 0.0f;
	if (!(index > 0 && index < int(sizeof(s_buttonPressedAt) / sizeof(s_buttonPressedAt[0]))))
		return none;
	// (a time from the last map - the clock restarts on a new one - is forgotten)
	if (s_buttonPressedAt[index] > gpGlobals->time)
		s_buttonPressedAt[index] = -999.0f;
	return s_buttonPressedAt[index];
}

Vector BrushCenter(CBaseEntity *pEnt)
{
	return (pEnt->pev->absmin + pEnt->pev->absmax) * 0.5f;
}
} // namespace

// when each breakable was last given up on (it doesn't break: round it, not again for a while)
static float s_breakTriedAt[MAX_EDICTS];

void CCSBot::LsDoorButton()
{
	if (!LsFight() || !IsAlive() || IsAttacking() || IsUsingLadder() || LsPuppetActive())
		return;
	float now = gpGlobals->time;
	auto giveUp = [&]() {
		// (a door it couldn't open: round it for a minute)
		if (m_lsButtonBehind)
		{
			LsForceBadArea(m_lsButtonBehind, 60.0f);
			m_lsButtonBehind = nullptr;
			DestroyPath();
			Idle();
		}
	};

	// stepping back out of a door's swing
	if (now < m_lsDoorBackUntil)
	{
		MoveBackward();
		return;
	}

	// a door it pressed for: did it open? (a delay on the button: a couple of seconds' grace)
	if (m_lsDoorCheckAt > 0.0f && now >= m_lsDoorCheckAt)
	{
		m_lsDoorCheckAt = 0.0f;
		CBaseToggle *pDoor = static_cast<CBaseToggle *>((CBaseEntity *)m_lsDoor);
		if (pDoor && pDoor->m_toggle_state == TS_AT_BOTTOM)
		{
			giveUp();
			LsWhy("door didn't open: another way");
		}
		m_lsDoor = nullptr;
		m_lsButtonBehind = nullptr;
	}

	// breaking something in the way (a grate, a board, a window on its path)
	CBaseEntity *pBreak = m_lsBreak;
	if (pBreak)
	{
		bool broken = pBreak->pev->health <= 0.0f || pBreak->pev->solid == SOLID_NOT;
		if (broken || now > m_lsBreakUntil || GetTimeSinceLastSawEnemy() < 2.0f)
		{
			// (not broken in time: round it, and not tried again for a while)
			if (!broken && now > m_lsBreakUntil)
			{
				s_breakTriedAt[pBreak->entindex()] = now;
				giveUp();
			}
			m_lsBreak = nullptr;
			m_lsButtonBehind = nullptr;
			ResetStuckMonitor();
			return;
		}
		// (a newer player stands there a moment before it clicks, and its shots are sloppier)
		if (now < m_lsBreakStartAt)
			return;
		// (a knife-run bot gets its gun out for it; never with a teammate in the way)
		if (IsUsingKnife() && !IsUsingGrenade())
			EquipBestWeapon(MUST_EQUIP);
		Vector at = BrushCenter(pBreak);
		SetLookAt("Loner-Strike: breaking it", &at, PRIORITY_HIGH, 0.4f);
		if (IsLookingAtPosition(&at, 6.0f + 14.0f * (1.0f - LsSkill())) && !IsActiveWeaponReloading() && !IsActiveWeaponClipEmpty() && !IsFriendInLineOfFire())
		{
			// (a pistol or a pump needs the trigger let go between shots)
			bool semi = IsUsingPistol() || IsUsingShotgun();
			if (!semi || LsTriggerTap())
				PrimaryAttack();
		}
		return;
	}

	// on its way to a button (or pressing a use-only door)
	CBaseEntity *pButton = m_lsButton;
	if (pButton)
	{
		if (now > m_lsButtonUntil || GetTimeSinceLastSawEnemy() < 2.0f)
		{
			m_lsButton = nullptr;
			if (now > m_lsButtonUntil)
				giveUp();
			return;
		}
		Vector at = BrushCenter(pButton);
		Vector eye = GetEyePosition();
		SetLookAt("Loner-Strike: button", &at, PRIORITY_HIGH, 0.5f);
		if ((at - eye).IsLengthLessThan(80.0f))
		{
			if (IsLookingAtPosition(&at, 20.0f))
			{
				UseEnvironment();
				ButtonPressed(pButton->entindex()) = now;
				CBaseEntity *pDoor = m_lsDoor;
				if (pDoor)
				{
					ButtonPressed(pDoor->entindex()) = now;
					m_lsDoorCheckAt = now + 2.0f;
				}
				m_lsButton = nullptr;
				LsWhy("opened the door");
				ResetStuckMonitor();
			}
		}
		else if (!IsMovingTo())
		{
			CNavArea *area = TheNavAreaGrid.GetNearestNavArea(&at);
			if (area)
			{
				Vector stand = *area->GetCenter();
				MoveTo(&stand, FASTEST_ROUTE);
			}
		}
		return;
	}

	// held up? (stuck, or barely moving on a path) - a few times a second at most
	if (now < m_lsButtonCheckAt || m_lsDoorCheckAt > 0.0f)
		return;
	m_lsButtonCheckAt = now + 0.3f;
	if (!m_isStuck && !(HasPath() && pev->velocity.Length2D() < 40.0f))
		return;

	// what's just in front (the way it's looking, or the way its path goes)
	Vector eye = GetEyePosition();
	Vector dirs[2];
	int nd = 0;
	// (the way its path goes; the way it's facing only when it's stuck with no path - not a
	// window it happens to be looking at while it pauses)
	if (HasPath() && m_pathIndex >= 0 && m_pathIndex < m_pathLength)
		dirs[nd++] = m_path[m_pathIndex].pos - pev->origin;
	else if (m_isStuck)
	{
		UTIL_MakeVectors(pev->v_angle);
		dirs[nd++] = gpGlobals->v_forward;
	}
	CBaseEntity *pHit = nullptr;
	Vector ahead;
	for (int i = 0; i < nd && !pHit; i++)
	{
		ahead = dirs[i];
		ahead.z = 0.0f;
		if (ahead.Length2D() < 1.0f)
			continue;
		ahead.NormalizeInPlace();
		TraceResult tr;
		UTIL_TraceLine(pev->origin, pev->origin + ahead * 72.0f, ignore_monsters, dont_ignore_glass, ENT(pev), &tr);
		if (tr.flFraction < 1.0f && tr.pHit && !FNullEnt(tr.pHit))
			pHit = CBaseEntity::Instance(tr.pHit);
	}
	if (!pHit)
		return;

	// something breakable on its way (not one only the map can break)
	if (FClassnameIs(pHit->pev, "func_breakable"))
	{
		// (one that really breaks - not unbreakable glass, not a "wall" with a mountain of health
		// - and not one it gave up on lately)
		CBreakable *pB = static_cast<CBreakable *>(pHit);
		if (pHit->pev->takedamage != DAMAGE_NO && !(pHit->pev->spawnflags & SF_BREAK_TRIGGER_ONLY) && pHit->pev->health > 0.0f
			&& pHit->pev->health <= 300.0f && pB->IsBreakable()
			&& (s_breakTriedAt[pHit->entindex()] <= 0.0f || s_breakTriedAt[pHit->entindex()] > now || now - s_breakTriedAt[pHit->entindex()] > 20.0f))
		{
			Vector beyond = BrushCenter(pHit) + ahead * 64.0f;
			m_lsButtonBehind = TheNavAreaGrid.GetNearestNavArea(&beyond);
			float hesitate = RANDOM_FLOAT(0.3f, 1.4f) * (1.0f - LsSkill());
			m_lsBreak = pHit;
			m_lsBreakStartAt = now + hesitate;
			m_lsBreakUntil = now + hesitate + 4.0f;
			LsWhy("breaking what's in the way");
		}
		return;
	}

	bool rotating = FClassnameIs(pHit->pev, "func_door_rotating");
	if (!rotating && !FClassnameIs(pHit->pev, "func_door"))
		return;
	CBaseToggle *pDoor = static_cast<CBaseToggle *>(pHit);

	// a swinging door it's standing in the way of: a step back so it can open
	if (rotating && (pDoor->m_toggle_state == TS_GOING_UP || pDoor->m_toggle_state == TS_GOING_DOWN))
	{
		m_lsDoorBackUntil = now + 0.6f;
		LsWhy("out of the door's way");
		return;
	}
	// an open swinging door it's walking into (a leaf standing open across the way): a step back
	// and to the side, round its edge - a few goes, then another way round
	if (rotating && pDoor->m_toggle_state == TS_AT_TOP)
	{
		if (now - m_lsDoorSideAt > 4.0f)
			m_lsDoorSideTries = 0;
		m_lsDoorSideAt = now;
		if (++m_lsDoorSideTries > 3)
		{
			m_lsDoorSideTries = 0;
			// (the area past the door: round it for a minute)
			Vector past = BrushCenter(pHit) + ahead * 64.0f;
			m_lsButtonBehind = TheNavAreaGrid.GetNearestNavArea(&past);
			giveUp();
			LsWhy("door in the way: another way");
			return;
		}
		// (across the way it's going, away from the leaf's middle; as strafe keys, by its view)
		Vector toDoor = BrushCenter(pHit) - pev->origin;
		Vector2D side(-ahead.y, ahead.x);
		if (side.x * toDoor.x + side.y * toDoor.y > 0.0f)
			side = side * -1.0f;
		float yaw = pev->v_angle.y;
		float viewLeft = side.x * -BotSIN(yaw) + side.y * BotCOS(yaw);
		m_lsDoorSide = viewLeft > 0.0f ? -1 : 1;
		m_lsDoorBackUntil = now + 0.3f;
		m_lsDoorSideUntil = now + 0.7f;
		LsWhy("round the open door");
		return;
	}
	if (pDoor->m_toggle_state != TS_AT_BOTTOM)
		return;
	Vector doorAt = BrushCenter(pHit);
	Vector behind = doorAt + ahead * 64.0f;
	m_lsButtonBehind = TheNavAreaGrid.GetNearestNavArea(&behind);

	// a door opened by pressing use on it
	// (one it pressed for a moment ago: still opening)
	if (now - ButtonPressed(pHit->entindex()) < 3.0f)
		return;
	m_lsDoor = pHit;
	if (pHit->pev->spawnflags & SF_DOOR_USE_ONLY)
	{
		m_lsButton = pHit;
		m_lsButtonUntil = now + 4.0f;
		LsWhy("opening the door");
		return;
	}

	// a door opened by something else (it has a name): the button wired to it, close by
	if (FStringNull(pHit->pev->targetname))
	{
		// (no button for it anywhere: locked, or opened by the map - another way round)
		giveUp();
		LsWhy("door won't open: another way");
		return;
	}
	const char *name = STRING(pHit->pev->targetname);
	static const char *cls[] = { "func_button", "func_rot_button", "button_target" };
	CBaseEntity *pBest = nullptr;
	float bestD = 600.0f;
	for (const char *c : cls)
	{
		CBaseEntity *pEnt = nullptr;
		while ((pEnt = UTIL_FindEntityByClassname(pEnt, c)) != nullptr)
		{
			if (FStringNull(pEnt->pev->target) || Q_strcmp(STRING(pEnt->pev->target), name) != 0)
				continue;
			if (now - ButtonPressed(pEnt->entindex()) < 3.0f)
				continue;
			float d = float((BrushCenter(pEnt) - eye).Length());
			if (d < bestD)
			{
				bestD = d;
				pBest = pEnt;
			}
		}
	}
	if (!pBest)
	{
		// (locked for good, or opened from somewhere it can't get to: another way round)
		giveUp();
		LsWhy("door won't open: another way");
		return;
	}
	m_lsButton = pBest;
	m_lsButtonUntil = now + 8.0f;
	LsWhy("going to the door's button");
}

int LsMapSerial()
{
	LsMapKindUpdate();
	return s_mapSerial;
}

// The "safe" early part of the round (knife out, no worries) on this map: until a couple of
// seconds before the two teams could first meet (half the way between the spawns, running), and
// none on maps where the fight starts at once. Stock had 15-20 s on every map: still knife-running
// when de_dust2's mid fights start
float LsMapSafeTime()
{
	int kind = LsMapKind();
	if (kind == LSMAP_AIM || kind == LSMAP_FY || kind == LSMAP_AWP || kind == LSMAP_KNIFE)
		return 0.0f;
	const Vector *ct = LsEnemySpawn(TERRORIST), *t = LsEnemySpawn(CT);
	if (!ct || !t)
		return 20.0f;
	return Q_max(0.0f, float((*ct - *t).Length2D()) * 0.5f / 250.0f - 2.0f);
}

// ---- sounds and being shot at (batch 2) --------------------------------------------------

// Where a sound will come out: of the angles enemies appear from here (map knowledge), the
// one leading to where it was heard. False if none fits (the old walk-back trace is used)
bool CCSBot::LsPreAim(const Vector &heard, float secs, CBasePlayer *who)
{
	if (!m_lastKnownArea)
		return false;
	// someone on a ladder (its clank): they'll come off the end on this side of it - the top
	// if it's above, the bottom if below - when that end's in view from here
	if (who && who->IsOnLadder())
	{
		const CNavLadder *best = nullptr;
		float bestD = 250.0f;
		for (NavLadderList::const_iterator li = TheNavLadderList.begin(); li != TheNavLadderList.end(); ++li)
		{
			const CNavLadder *ladder = *li;
			Vector mid = (ladder->m_top + ladder->m_bottom) * 0.5f;
			float d = float((Vector(mid.x, mid.y, who->pev->origin.z) - who->pev->origin).Length2D());
			if (d < bestD)
			{
				bestD = d;
				best = ladder;
			}
		}
		if (best)
		{
			bool above = pev->origin.z > (best->m_top.z + best->m_bottom.z) * 0.5f;
			Vector end = (above ? best->m_top : best->m_bottom) + Vector(0, 0, above ? 20.0f : HalfHumanHeight);
			TraceResult tr;
			UTIL_TraceLine(GetEyePosition(), end, ignore_monsters, ENT(pev), &tr);
			if (tr.flFraction >= 1.0f)
			{
				SetLookAt("Loner-Strike: ladder", &end, PRIORITY_MEDIUM, secs);
				return true;
			}
		}
	}
	const LsAreaKnow *k = LsKnow(m_lastKnownArea);
	if (!k || !k->angleCount)
		return false;
	CNavArea *ha = TheNavAreaGrid.GetNearestNavArea(&heard);
	int best = -1;
	float bestD = 900.0f;
	for (int a = 0; a < k->angleCount; a++)
	{
		CNavArea *t = LsKnowArea(k->angles[a].area);
		if (!t)
			continue;
		float d = (t == ha) ? 0.0f : float((*t->GetCenter() - heard).Length());
		// (an angle the sound's area can see into is the way it'd come)
		if (ha && t != ha && LsSightReady() && LsSees(ha, t))
			d *= 0.6f;
		// (footsteps in a vent or a crawlspace - a low ceiling over them - come out where the
		// low ceiling ends: an angle onto a low-ceilinged spot is the vent's mouth)
		const LsAreaKnow *hk = ha ? LsKnow(ha) : nullptr;
		const LsAreaKnow *tk = LsKnow(t);
		if (hk && tk && hk->ceiling * 4 < 60 && tk->ceiling * 4 < 90)
			d *= 0.5f;
		if (d < bestD)
		{
			bestD = d;
			best = a;
		}
	}
	if (best < 0)
		return false;
	CNavArea *t = LsKnowArea(k->angles[best].area);
	Vector at = *t->GetCenter() + Vector(0, 0, HalfHumanHeight + LsLookHeight());
	SetLookAt("Loner-Strike: pre-aim", &at, PRIORITY_MEDIUM, secs);
	return true;
}

// Glass, a vent cover, a board breaking close by, with none of its own team there: someone's
// coming through. Newer players don't always register what it means
// ---- floors that broke ------------------------------------------------------------------
// A glass or wooden floor that broke (IsFloorBreakable: a glass arena's pane, a plank bridge):
// its box until the round restarts. The nav keeps the areas over it (the floor comes back next
// round); routes go round them (PathCost), and a bot already on its way across notices or not.
static struct { int ent; Vector lo, hi; float at; } s_lsBroken[64];
static int s_lsBrokenCount = 0;

bool LsNoteBrokenFloor(CBaseEntity *pEntity)
{
	if (!pEntity || !IsFloorBreakable(pEntity))
		return false;
	int ent = ENTINDEX(pEntity->edict());
	for (int i = 0; i < s_lsBrokenCount; i++)
	{
		if (s_lsBroken[i].ent == ent)
			return true;		// (every bot hears it: noted once)
	}
	if (s_lsBrokenCount < (int)(sizeof(s_lsBroken) / sizeof(s_lsBroken[0])))
	{
		s_lsBroken[s_lsBrokenCount].ent = ent;
		s_lsBroken[s_lsBrokenCount].lo = pEntity->pev->absmin;
		s_lsBroken[s_lsBrokenCount].hi = pEntity->pev->absmax;
		s_lsBroken[s_lsBrokenCount].at = gpGlobals->time;
		s_lsBrokenCount++;
	}
	return true;
}

void LsClearBrokenFloors()
{
	s_lsBrokenCount = 0;
}

// Whether an area lies over a floor that broke: at its height, and mostly over the hole (an
// area that's mostly solid floor beside it isn't blocked). One whole again (a plugin put it
// back mid-round) doesn't count.
bool LsOverBrokenFloor(const CNavArea *area)
{
	if (!s_lsBrokenCount || !area)
		return false;
	const Extent *e = area->GetExtent();
	float areaSize = Q_max(1.0f, (e->hi.x - e->lo.x) * (e->hi.y - e->lo.y));
	float z = area->GetCenter()->z;
	for (int i = 0; i < s_lsBrokenCount; i++)
	{
		const Vector &lo = s_lsBroken[i].lo, &hi = s_lsBroken[i].hi;
		if (Q_abs(z - hi.z) >= 24.0f)
			continue;
		float ox = Q_min(e->hi.x, hi.x) - Q_max(e->lo.x, lo.x);
		float oy = Q_min(e->hi.y, hi.y) - Q_max(e->lo.y, lo.y);
		if (ox <= 0.0f || oy <= 0.0f || ox * oy < 0.5f * areaSize)
			continue;
		// (the game says it broke a moment before it stops being solid: noted this moment, it's broken)
		edict_t *pEdict = INDEXENT(s_lsBroken[i].ent);
		if (s_lsBroken[i].at != gpGlobals->time && (!pEdict || pEdict->free || pEdict->v.solid != SOLID_NOT))
			continue;
		return true;
	}
	return false;
}

// A floor just broke: if the way it's going crosses it, a new route, if it noticed. Most do;
// a newer player in a fight sometimes doesn't, and walks into the hole as people do.
void CCSBot::LsHoleCheck()
{
	if (!HasPath())
		return;
	bool crosses = false;
	for (int i = Q_max(0, m_pathIndex); i < m_pathLength && !crosses; i++)
		crosses = LsOverBrokenFloor(m_path[i].area);
	if (!crosses)
		return;
	float skill = LsSkill();
	float notice = 0.55f + 0.45f * skill;
	if (IsEnemyVisible() || IsAttacking())
		notice *= 0.6f + 0.4f * skill;
	if (RANDOM_FLOAT(0.0f, 1.0f) >= notice)
	{
		LsTraceNote("hole ahead: didn't notice");
		return;
	}
	LsTraceNote("hole ahead: new route");
	// (the same goal, a way round: a path cleared reads as "arrived" to the states)
	Vector goal = m_path[m_pathLength - 1].pos;
	if (!ComputePath(TheNavAreaGrid.GetNavArea(&goal), &goal, SAFEST_ROUTE))
		DestroyPath();
}

void CCSBot::LsHeardBreak(CBaseEntity *pBreak)
{
	if (!LsFight() || !pBreak || IsBlind() || (CBaseEntity *)m_lsBreak == pBreak)
		return;
	Vector at = BrushCenter(pBreak);
	float dist = float((at - pev->origin).Length());
	if (dist > 1100.0f)
		return;
	bool ours = false;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		if ((pPlayer->pev->origin - at).IsLengthLessThan(250.0f))
			ours = true;
	});
	if (ours || RANDOM_FLOAT(0.0f, 1.0f) > 0.5f + 0.5f * LsSkill())
		return;
	// (its own shots broke it; or it's already on a louder, fresher noise)
	if (gpGlobals->time - m_fireWeaponTimestamp < 0.4f)
		return;
	if (IsNoiseHeard() && GetNoisePriority() > PRIORITY_MEDIUM && gpGlobals->time - m_noiseTimestamp < 3.0f)
		return;
	CNavArea *area = TheNavAreaGrid.GetNearestNavArea(&at);
	if (!area)
		return;
	m_noisePosition = at;
	m_noiseArea = area;
	m_noisePriority = PRIORITY_MEDIUM;
	m_noiseTimestamp = gpGlobals->time;
	if (!IsAttacking() && !LsPreAim(at, RANDOM_FLOAT(1.5f, 3.0f)))
	{
		Vector look = at;
		SetLookAt("Loner-Strike: something broke", &look, PRIORITY_MEDIUM, RANDOM_FLOAT(1.0f, 2.0f));
	}
	LsWhy("heard something break");
}

// The nearest spot (within 500 units, at least minMove away, not toward the threat) a
// standing head there is out of the threat's sight: it runs there
bool CCSBot::LsTakeCoverFrom(const Vector &threat, float minMove, bool fromFight, float holdSecs)
{
	if (!m_lastKnownArea || IsUsingLadder() || (IsAttacking() && !fromFight))
		return false;
	// (it picked a spot a moment ago and is on its way: that stands - two reasons for cover at
	// once, a reload and a fight, picked one after the other each think)
	if (gpGlobals->time - m_lsCoverPickAt < 0.8f)
		return true;
	// (never off a plant, a defuse, a run from the bomb, or with hostages following)
	if (m_state == &m_plantBombState || m_state == &m_defuseBombState || IsEscapingFromBomb() || m_hostageEscortCount > 0)
		return false;
	Vector from = threat + Vector(0, 0, 17.0f);
	float threatD = float((pev->origin - threat).Length());
	CNavArea *queue[48];
	int head = 0, tail = 0;
	queue[tail++] = m_lastKnownArea;
	CNavArea *best = nullptr, *bestTaken = nullptr;
	float bestD = 500.0f, bestTakenD = 500.0f;
	while (head < tail)
	{
		CNavArea *area = queue[head++];
		Vector c = *area->GetCenter();
		float d = float((c - pev->origin).Length2D());
		if (d > 500.0f)
			continue;
		if (d >= minMove && d < bestD && (c - threat).Length() > threatD - 100.0f && !LsBadArea(area) && LsHeadroom(area))
		{
			// (not a spot a teammate is standing on or already heading for: two in one bit of
			// cover end up stepping round each other, and one spray gets both)
			bool taken = false;
			LsForTeammates(this, [&](CBasePlayer *pPlayer) {
				if ((pPlayer->pev->origin - c).IsLengthLessThan(90.0f))
					taken = true;
				// (heading there: only one close by it can see going that way - not a teammate's
				// plan it couldn't know)
				else if (pPlayer->IsBot() && static_cast<CCSBot *>(pPlayer)->HasPath()
					&& (pPlayer->pev->origin - pev->origin).IsLengthLessThan(600.0f)
					&& (static_cast<CCSBot *>(pPlayer)->GetPathEndpoint() - c).IsLengthLessThan(90.0f))
					taken = true;
			});
			TraceResult tr;
			tr.flFraction = 1.0f;
			// (a taken one is kept as a last resort: cover shared beats none)
			if (taken && d < bestTakenD)
			{
				UTIL_TraceLine(from, c + Vector(0, 0, 62.0f), ignore_monsters, ignore_glass, ENT(pev), &tr);
				if (tr.flFraction < 1.0f)
				{
					bestTaken = area;
					bestTakenD = d;
				}
				tr.flFraction = 1.0f;
			}
			else if (!taken)
				UTIL_TraceLine(from, c + Vector(0, 0, 62.0f), ignore_monsters, ignore_glass, ENT(pev), &tr);
			if (tr.flFraction < 1.0f)
			{
				best = area;
				bestD = d;
			}
		}
		for (int dir = 0; dir < NUM_DIRECTIONS; dir++)
		{
			int n = area->GetAdjacentCount((NavDirType)dir);
			for (int i = 0; i < n && tail < 48; i++)
			{
				CNavArea *adj = area->GetAdjacentArea((NavDirType)dir, i);
				if (!adj)
					continue;
				// (not down a drop that would kill it, or leave it nearly dead)
				float drop = float(area->GetCenter()->z - adj->GetCenter()->z);
				if (drop > 60.0f && GetApproximateFallDamage(drop) + LsFallMargin() >= pev->health)
					continue;
				bool seen = false;
				for (int j = 0; j < tail && !seen; j++)
					seen = queue[j] == adj;
				if (!seen)
					queue[tail++] = adj;
			}
		}
	}
	if (!best)
		best = bestTaken;
	if (!best)
		return false;
	m_lsCoverStay = best == m_lastKnownArea;
	Vector spot = *best->GetCenter();
	// (on the way there it doesn't stop to fight - until it's hit, which ends that at once)
	float travel = float((spot - pev->origin).Length2D()) / 230.0f;
	if (fromFight)
	{
		// (backing out of a fight: it remembers who it backed off from)
		LsThreatNote();
		StopAttacking();
		IgnoreEnemies(Q_min(2.5f, travel + (!LsSolo() ? 0.3f : 0.1f)));
	}
	StandUp();
	Run();
	// (a short hold there, then on: back out by another way, the spot it left avoided; the
	// better ones are quicker back out, careful ones and campers wait longer, rushers least)
	float hold = RANDOM_FLOAT(1.0f, 2.5f) + 1.5f * (1.0f - LsSkill());
	int style = LsStyle();
	if (style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER)
		hold *= 1.3f;
	else if (style == LS_STYLE_RUSHER)
		hold *= 0.6f;
	// (ahead on numbers: back out sooner; behind: warier)
	if (LsSkill() > 0.3f)
	{
		float tempo = LsTempo();
		hold *= tempo >= 2.0f ? 0.7f : tempo <= -2.0f ? 1.3f : 1.0f;
	}
	if (holdSecs >= 0.0f)
		hold = holdSecs;
	// (a dash: no sneaking, crouch-walking or pausing on the way; a slow route gets more time)
	travel *= 1.4f;
	Hurry(travel + 0.5f);
	Hide(&spot, hold + travel);
	m_lsCoverPickAt = gpGlobals->time;
	// (on the way it shoots what it meets but doesn't go off after noises - the shot that sent
	// it there is one; set directly: SetDisposition would cancel the "don't stop to fight on
	// the way" window just started above, and every move to cover from a fight undid itself)
	m_disposition = OPPORTUNITY_FIRE;
	LsFallBack();
	m_lsBackFrom = threat + Vector(0, 0, HalfHumanHeight + LsLookHeight());
	LsWhy("to cover");
	return true;
}

// What kind of gun someone has out
int LsGunClass(CBasePlayer *pPlayer)
{
	CBasePlayerWeapon *pGun = static_cast<CBasePlayerWeapon *>(pPlayer->m_pActiveItem);
	if (!pGun)
		return LSG_NONE;
	switch (pGun->m_iId)
	{
	case WEAPON_GLOCK18: case WEAPON_USP: case WEAPON_P228: case WEAPON_FIVESEVEN: case WEAPON_ELITE: return LSG_PISTOL;
	case WEAPON_DEAGLE: return LSG_DEAGLE;
	case WEAPON_MAC10: case WEAPON_TMP: case WEAPON_MP5N: case WEAPON_UMP45: case WEAPON_P90: return LSG_SMG;
	case WEAPON_M3: case WEAPON_XM1014: return LSG_SHOTGUN;
	case WEAPON_AK47: case WEAPON_M4A1: case WEAPON_GALIL: case WEAPON_FAMAS: case WEAPON_SG552: case WEAPON_AUG: return LSG_RIFLE;
	case WEAPON_AWP: case WEAPON_SCOUT: case WEAPON_G3SG1: case WEAPON_SG550: return LSG_SNIPER;
	case WEAPON_M249: return LSG_MG;
	}
	return LSG_NONE;
}

// Low cover right by where it stands, toward a spot (map knowledge: something solid within
// 64 units that way at a crouched head, nothing at standing eyes)
bool CCSBot::LsLowCoverToward(const Vector &at)
{
	if (!m_lastKnownArea || !LsExposureReady())
		return false;
	const LsAreaKnow *k = LsKnow(m_lastKnownArea);
	if (!k)
		return false;
	int bestP = -1;
	float bestD = 40.0f;
	for (int p = 0; p < 9; p++)
	{
		Vector v;
		LsExposurePoint(m_lastKnownArea, p, &v);
		float d = float((v - pev->origin).Length2D());
		if (d < bestD)
		{
			bestD = d;
			bestP = p;
		}
	}
	if (bestP < 0)
		return false;
	float yaw = UTIL_VecToYaw(at - pev->origin);
	while (yaw < 0.0f)
		yaw += 360.0f;
	int d = int(yaw / 22.5f + 0.5f) & 15;
	return (k->pointCoverLow[bestP] & (1 << d)) && !(k->pointCoverHigh[bestP] & (1 << d));
}

// How close to dying a drop may bring it: the better ones keep well clear, newer ones now and
// then misjudge it (never off a lethal one: some margin's always there)
float CCSBot::LsFallMargin()
{
	if (!LsFight())
		return 10.0f;
	float skill = LsSkill();
	if (skill > 0.6f)
		return 25.0f;
	if (skill < 0.3f && !LsMatch() && LsTrait(73) < 0.25f)
		return 3.0f;
	return 10.0f;
}

// Mid-fight, a couple of times a second: a flashed enemy is pushed (the better ones see it);
// reloading behind low cover, it crouches; and once a fight, the matchup: the wrong gun for
// this range (a rifle against an AWP down a long lane, an SMG or a pistol against a rifle far
// off, a shotgun past its reach) - the better ones get out of that line and come by another
// way, unless the enemy's hurt or not looking its way. Newer ones just fight it out; less of
// it in deathmatch, from rushers, and on aim maps (nowhere to go round)
bool CCSBot::LsMatchup(CBasePlayer *pEnemy, bool crouchAndHold)
{
	float now = gpGlobals->time;
	// (reloaded behind low cover: back up, whether or not the enemy's in view from down there)
	if (m_lsReloadCrouched && !IsReloading())
	{
		m_lsReloadCrouched = false;
		if (!crouchAndHold && m_lsDuckUntil <= 0.0f)
			StandUp();
	}
	// hit with low cover in front: down behind it a moment, then back up to shoot (the better
	// ones; on aim maps it's how the whole fight goes: from the box, not out in the open)
	// (not a bot holding crouched there anyway: that's its own habit)
	if (crouchAndHold)
		m_lsDuckUntil = 0.0f;
	else if (m_lsDuckUntil > 0.0f && now >= m_lsDuckUntil)
	{
		m_lsDuckUntil = 0.0f;
		StandUp();
	}
	else if (m_lsDuckUntil > 0.0f)
		Crouch();
	else if (GetTimeSinceAttacked() < 0.3f && now > m_lsDuckAt && !IsReloading() && pEnemy->IsAlive())
	{
		// (not like clockwork: a while before the next, and sometimes it just stays up)
		m_lsDuckAt = now + RANDOM_FLOAT(1.5f, 4.0f);
		int kind = LsMapKind();
		bool aim = kind == LSMAP_AIM || kind == LSMAP_FY || kind == LSMAP_AWP;
		float odds = aim ? 0.3f + 0.5f * LsSkill() : 0.4f * LsSkill();
		if (LsSkill() > 0.35f && RANDOM_FLOAT(0.0f, 1.0f) < odds && LsLowCoverToward(pEnemy->pev->origin))
		{
			// (a quick dip and straight back up for the better ones; newer ones stay down longer)
			m_lsDuckUntil = now + RANDOM_FLOAT(0.3f, 0.6f) + 0.4f * (1.0f - LsSkill());
			Crouch();
			LsWhy("down behind cover");
		}
	}
	if (LsRushed(&pEnemy->pev->origin))
		return true;
	if (now < m_lsMatchupAt || !IsEnemyVisible() || !pEnemy->IsAlive())
		return false;
	m_lsMatchupAt = now + 0.5f;
	float skill = LsSkill();
	float range = float((pEnemy->pev->origin - pev->origin).Length());

	// A flashed enemy, read the way a person reads it (1.6 shows nothing on a blind player):
	// a flash it saw pop by them, or a teammate's, pushed for as long as it reckons it lasts; or,
	// a good player, a bang heard over there and that one in view with its back turned for half a
	// second. Never the game's own flag: a wrong read runs it into someone who can see again
	{
		Vector toMe = pev->origin - pEnemy->pev->origin;
		toMe.z = 0.0f;
		Vector efwd;
		UTIL_MakeVectorsPrivate(pEnemy->pev->v_angle, efwd, nullptr, nullptr);
		efwd.z = 0.0f;
		float tl = float(toMe.Length()), fl = float(efwd.Length());
		bool facingMe = tl > 1.0f && fl > 0.01f && float(DotProduct(toMe, efwd)) / (tl * fl) > 0.75f;
		int who = pEnemy->entindex();
		if (facingMe || m_lsAwayWho != who)
		{
			m_lsAwayWho = facingMe ? 0 : who;
			m_lsAwaySince = now;
		}
		float until = 0.0f;
		bool sawPop = now < m_lsFlashPopAt + m_lsFlashGuess && (m_lsFlashPopPos - pEnemy->pev->origin).IsLengthLessThan(900.0f);
		// (a good player sees one already turned on it isn't blind; a newer one goes anyway)
		if (sawPop && (!facingMe || skill < 0.5f))
			until = m_lsFlashPopAt + m_lsFlashGuess;
		else if (skill >= 0.5f && !facingMe && now - m_lsFlashHeardAt < 2.0f && now - m_lsAwaySince >= 0.5f
			&& (m_lsFlashHeardPos - pEnemy->pev->origin).IsLengthLessThan(800.0f))
			until = now + RANDOM_FLOAT(0.8f, 1.4f);
		// (the bold go for it more, the careful and campers less)
		float lean = (LsStyle() == LS_STYLE_CAREFUL || LsStyle() == LS_STYLE_CAMPER) ? 0.7f : (LsStyle() == LS_STYLE_RUSHER || LsStyle() == LS_STYLE_KNIFER) ? 1.25f : 1.0f;
		if (until > now && range < 1200.0f && !IsUsingSniperRifle() && now > m_lsPushUntil && !IsActiveWeaponReloading()
			&& RANDOM_FLOAT(0.0f, 1.0f) < (0.3f + 0.6f * skill) * lean * (0.8f + 0.4f * LsAggro()))
		{
			m_lsPushUntil = Q_min(until, now + 1.8f);
			LsWhy(sawPop ? "pushing a flashed one" : "pushing one acting flashed");
		}
	}
	// (most of the time, more often the better it is: not every reload like a script; decided
	// once per reload)
	// one called hurt, in view: the ones who listen to callouts push it (it's one shot)
	float hurtHp = 100.0f;
	// (with health to spare: pushing a one-shot enemy on 10 hp is trading, not winning)
	// (never with the gun mid-reload: a push with nothing to shoot is a walk into them)
	if (LsKnownHurt(pEnemy, m_iTeam, &hurtHp) && hurtHp < 40.0f && now > m_lsPushUntil && !IsUsingSniperRifle() && !LsSolo() && !IsActiveWeaponReloading()
		&& pev->health >= 55.0f - 30.0f * LsAggro()
		&& range < 1000.0f && RANDOM_FLOAT(0.0f, 1.0f) < 0.25f + 0.5f * skill * LsTeamwork() + 0.25f * LsAggro())
	{
		m_lsPushUntil = now + RANDOM_FLOAT(0.8f, 1.5f);
		LsWhy("pushing a hurt one");
	}
		if (IsReloading() && !crouchAndHold && skill > 0.4f && now > m_lsReloadCrouchAt)
	{
		m_lsReloadCrouchAt = now + 3.0f;
		if (RANDOM_FLOAT(0.0f, 1.0f) < 0.45f + 0.5f * skill && LsLowCoverToward(pEnemy->pev->origin))
		{
			Crouch();
			m_lsReloadCrouched = true;
		}
	}

	CBasePlayer *pDone = m_lsMatchupEnemy;
	if (pDone == pEnemy)
		return false;
	m_lsMatchupEnemy = pEnemy;
	m_lsMatchupUntil = now + 15.0f;
	int mine = LsGunClass(this), theirs = LsGunClass(pEnemy);
	bool bad = false;
	// (an AWPer that just fired: the bolt's coming back - the better ones swing on it)
	CBasePlayer *pHeardSniper = m_lsSniperHeard;
	if (theirs == LSG_SNIPER && pHeardSniper == pEnemy && now - m_lsSniperShotAt < 1.2f && skill > 0.5f && mine != LSG_SNIPER)
	{
		m_lsMatchupEnemy = nullptr;	// (weighed again after)
		m_lsPushUntil = now + RANDOM_FLOAT(0.6f, 1.0f);
		LsWhy("swinging on the bolt");
		return false;
	}
	if (theirs == LSG_SNIPER && mine != LSG_SNIPER && range > 900.0f && IsPlayerLookingAtMe(pEnemy))
		bad = true;
	// (a shotgun up close beats a rifle: get some distance)
	if (theirs == LSG_SHOTGUN && (mine == LSG_RIFLE || mine == LSG_SNIPER) && range < 300.0f)
		bad = true;
	bool farGun = theirs == LSG_RIFLE || theirs == LSG_SNIPER || theirs == LSG_MG;
	if (farGun && ((mine == LSG_SMG && range > 1300.0f) || (mine == LSG_SHOTGUN && range > 800.0f) || (mine == LSG_PISTOL && range > 1100.0f)))
		bad = true;
	// (an eco pistol up close against a rifle: that's the fight it wanted - an ambush - no backing off)
	if (mine == LSG_PISTOL && farGun && range < 700.0f)
		bad = false;
	// (hurt as far as it knows: called out, or its own hits)
	if (!bad || LsKnownHurt(pEnemy, m_iTeam) || skill < 0.35f)
		return false;
	if (m_state == &m_plantBombState || m_state == &m_defuseBombState || IsEscapingFromBomb())
		return false;
	float odds = 0.3f + 0.6f * skill;
	if (LsSolo())
		odds *= 0.4f;
	if (LsStyle() == LS_STYLE_RUSHER)
		odds *= 0.5f;
	int kind = LsMapKind();
	if (kind == LSMAP_AIM || kind == LSMAP_FY || kind == LSMAP_AWP)
		odds *= 0.3f;
	if (RANDOM_FLOAT(0.0f, 1.0f) > odds)
		return false;
	const CNavArea *here = m_lastKnownArea;
	if (!LsTakeCoverFrom(pEnemy->pev->origin, 64.0f, true))
		return false;
	m_lsCoverRunUntil = now + 2.5f;
	if (here)
		LsForceBadArea(here, 6.0f + 8.0f * LsSkill());
	// (an AWP's lane: from cover, a smoke across it - then the lane, smoked, is crossed)
	if (theirs == LSG_SNIPER && skill > 0.5f && !LsSolo() && LsHasNade(WEAPON_SMOKEGRENADE))
	{
		Vector to = pEnemy->pev->origin - pev->origin;
		float len = float(to.Length());
		m_lsSmokeLaneSpot = pev->origin + to * (Q_min(700.0f, len * 0.5f) / Q_max(len, 1.0f));
		m_lsSmokeLaneAt = now + RANDOM_FLOAT(1.5f, 2.5f);
	}
	LsWhy(theirs == LSG_SNIPER ? "out of the AWP's lane" : "wrong gun for this range");
	return true;
}

// Hit by someone it can't see, with a wall between: being wallbanged. That spot isn't safe
// this round: out of it (to somewhere else out of their line), and paths avoid it a while.
// Newer players often stay put
void CCSBot::LsHurtBy(CBasePlayer *pAttacker)
{
	float now = gpGlobals->time;
	if (!LsFight() || !IsAlive() || !pAttacker->IsAlive() || !IsEnemy(pAttacker) || now < m_lsHurtCheckAt)
		return;
	m_lsHurtCheckAt = now + 1.0f;
	if (IsVisible(pAttacker, false))
	{
		// (in plain view, being picked off from further than its gun reaches - an SMG or a
		// pistol at 900 against a rifle tapping away: no sitting there taking it. The better
		// ones get out of the line, by who it is; the rest at least fire back, below)
		int mine = LsGunClass(this), theirs = LsGunClass(pAttacker);
		float apart = float((pAttacker->pev->origin - pev->origin).Length());
		bool shortGun = (mine == LSG_SMG || mine == LSG_SHOTGUN || mine == LSG_PISTOL) && (theirs == LSG_RIFLE || theirs == LSG_SNIPER || theirs == LSG_DEAGLE || theirs == LSG_MG);
		if (shortGun && apart > 750.0f && m_state != &m_plantBombState && m_state != &m_defuseBombState && !IsEscapingFromBomb()
			&& RANDOM_FLOAT(0.0f, 1.0f) < 0.3f + 0.6f * LsSkill() && LsTakeCoverFrom(pAttacker->pev->origin, 120.0f, true))
		{
			// (a deliberate run out of the line: the rifle aiming at it doesn't pull it back)
			m_lsCoverRunUntil = now + 2.5f;
			LsWhy("outgunned at range: cover");
		}
		return;
	}
	TraceResult tr;
	UTIL_TraceLine(pAttacker->pev->origin + pAttacker->pev->view_ofs, GetEyePosition(), ignore_monsters, ignore_glass, ENT(pAttacker->pev), &tr);
	if (tr.flFraction >= 1.0f)
		return;
	if (RANDOM_FLOAT(0.0f, 1.0f) > 0.25f + 0.7f * LsSkill())
		return;
	if (m_lastKnownArea)
		LsForceBadArea(m_lastKnownArea, 8.0f + 7.0f * LsSkill());
	// (out of sight: only roughly where the shots came from)
	float err = 120.0f + 200.0f * (1.0f - LsSkill());
	Vector roughly = pAttacker->pev->origin + Vector(RANDOM_FLOAT(-err, err), RANDOM_FLOAT(-err, err), 0.0f);
	if (LsTakeCoverFrom(roughly, 150.0f))
		LsWhy("wallbanged: moving");
}

// Mid-fight, turning to someone heard running at it (the aim leaves its enemy a moment): seen
// now, it's the one fought
void CCSBot::LsUrgentTurn(const Vector &at, float secs, CBasePlayer *who)
{
	m_lsRunner = who;
	m_lsRunnerSpot = at;
	m_lsRunnerUntil = gpGlobals->time + secs;
}

bool CCSBot::LsRunnerGlance()
{
	if (!LsFight() || gpGlobals->time >= m_lsRunnerUntil)
		return false;
	CBasePlayer *pRunner = m_lsRunner;
	if ((pRunner && !pRunner->IsAlive()) || IsBlind())
	{
		m_lsRunnerUntil = 0.0f;
		return false;
	}
	if (pRunner)
	{
		if (IsVisible(pRunner, true))
		{
			m_lsRunnerUntil = 0.0f;
			Attack(pRunner);
			return false;
		}
		m_lsRunnerSpot = pRunner->pev->origin + Vector(0, 0, HalfHumanHeight);
	}
	Vector ang = UTIL_VecToAngles(m_lsRunnerSpot - GetEyePosition());
	ang.x = 360.0f - ang.x;
	SetLookAngles(ang.y, ang.x);
	return true;
}

// Being rushed: walking onto a site into three of them, or hearing a pack running at it, with
// none or few of its own team there. Counted from what it can fairly know: enemies seen in
// the last second or so, enemies heard running its way, teammates close by. Clearly
// outnumbered (two more of them than of its side, three at least), the better players see it
// at once and get back into cover, eyes on them, and don't walk straight back out (that spot's
// avoided a while: it waits for its team); newer ones often stand and take the fight, rushers
// more often go down shooting. Not in deathmatch (everyone's an enemy there), not planting or
// defusing, not a CT with the bomb about to go off (no time to wait)
bool CCSBot::LsRushed(const Vector *threat)
{
	float now = gpGlobals->time;
	if (!LsFight() || LsSolo() || now < m_lsRushCheckAt || now - m_lsRushedAt < 6.0f || IsUsingLadder() || IsBlind() || LsRushing())
		return false;
	m_lsRushCheckAt = now + 0.4f;
	if (m_state == &m_plantBombState || m_state == &m_defuseBombState || IsEscapingFromBomb())
		return false;
	if (m_iTeam == CT && TheCSBots()->IsBombPlanted() && TheCSBots()->GetBombTimeLeft() < 15.0f)
		return false;
	int seen = 0, heard = 0;
	Vector sum(0, 0, 0);
	for (int i = 1; i <= gpGlobals->maxClients && i <= MAX_CLIENTS; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!IsEntityValid(pPlayer) || !pPlayer->IsAlive() || !IsEnemy(pPlayer))
			continue;
		const WatchInfo &w = m_watchInfo[i - 1];
		if (w.isEnemy && w.timestamp > 0.0f && now - w.timestamp < 1.5f)
			seen++;
		else if (now - m_lsRushHeardAt[i - 1] < 2.0f)
			heard++;
		else
			continue;
		sum = sum + pPlayer->pev->origin;
	}
	int count = seen + heard;
	if (count < 3)
		return false;
	int side = 1;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		if ((pPlayer->pev->origin - pev->origin).IsLengthLessThan(700.0f))
			side++;
	});
	// (heard ones weigh less than ones in sight: footsteps might be two people or five)
	if (float(seen) + 0.6f * float(heard) < float(side) + 2.0f)
		return false;
	m_lsRushedAt = now;
	float skill = LsSkill();
	float odds = 0.15f + 0.8f * skill;
	if (LsStyle() == LS_STYLE_RUSHER)
		odds *= 0.6f;
	// (the scoreboard: well ahead on numbers it holds its ground more, well behind it's warier)
	if (skill > 0.3f)
	{
		float tempo = LsTempo();
		odds *= tempo >= 2.0f ? 0.6f : tempo <= -2.0f ? 1.2f : 1.0f;
	}
	if (RANDOM_FLOAT(0.0f, 1.0f) > odds)
		return false;
	Vector from = sum * (1.0f / float(count));
	const CNavArea *here = m_lastKnownArea;
	// (the better ones wait for the team in there - until one turns up, or they've gone quiet -
	// not peek straight back into the stack; never past what the round's clock allows)
	float wait = Q_min(5.0f + 7.0f * skill, Q_max(1.5f, LsTimeLeft() - 25.0f));
	if (LsStyle() == LS_STYLE_RUSHER)
		wait *= 0.5f;
	// (nobody left to wait for: just out of their line)
	bool team = GetFriendsRemaining() > 0;
	// (already somewhere they can't see: it stays put, crouched if it was)
	if (IsAtHidingSpot())
	{
		TraceResult tr;
		UTIL_TraceLine(from + Vector(0, 0, 17.0f), GetEyePosition(), ignore_monsters, ignore_glass, ENT(pev), &tr);
		if (tr.flFraction < 1.0f)
		{
			LsWhy("rushed: staying in cover");
			return false;
		}
	}
	if (!LsTakeCoverFrom(from, 0.0f, IsAttacking(), team ? wait : -1.0f))
		return false;
	m_lsCoverRunUntil = now + 2.5f;
	if (m_lsCoverStay)
		here = nullptr;
	if (team)
		m_lsRushHoldUntil = now + wait + 3.0f;
	if (here)
		LsForceBadArea(here, 8.0f + 6.0f * skill);
	GetChatter()->Scared();
	LsWhy("rushed: back to cover");
	return true;
}

// In cover from a rush, waiting for its team: it comes out when a teammate's close by, or it's
// been quiet a few seconds (none of them seen or heard running at it)
void CCSBot::LsRushHoldTick()
{
	float now = gpGlobals->time;
	if (m_lsRushHoldUntil <= 0.0f || now < m_lsRushHoldCheckAt)
		return;
	m_lsRushHoldCheckAt = now + 0.5f;
	if (!IsHiding() || now > m_lsRushHoldUntil || IsAttacking())
	{
		m_lsRushHoldUntil = 0.0f;
		return;
	}
	bool mate = false;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		if ((pPlayer->pev->origin - pev->origin).IsLengthLessThan(500.0f))
			mate = true;
	});
	bool quiet = now - m_lsRushedAt > 3.0f && GetTimeSinceLastSawEnemy() > 3.0f;
	for (int i = 0; i < MAX_CLIENTS && quiet; i++)
		if (now - m_lsRushHeardAt[i] < 2.0f)
			quiet = false;
	if (mate || quiet)
	{
		m_lsRushHoldUntil = 0.0f;
		Idle();
		LsWhy(mate ? "team's here: out of cover" : "gone quiet: out of cover");
	}
}

// ---- round knowledge (batch 3) ---------------------------------------------------------

// A person's spots this map: where they stood when they killed a bot (a veteran expects them
// there again). The last dozen
struct LsHabit { Vector at; int map; int who; };
static LsHabit s_habits[12];
static int s_habitNext = 0;

bool LsHabitNear(const Vector &at, float range)
{
	int map = LsMapSerial();
	for (int i = 0; i < 12; i++)
	{
		if (s_habits[i].map != map || !(s_habits[i].at - at).IsLengthLessThan(range))
			continue;
		CBasePlayer *pWho = UTIL_PlayerByIndex(s_habits[i].who);
		if (pWho && pWho->IsAlive() && !pWho->IsBot())
			return true;
	}
	return false;
}

int LsAlive(int team)
{
	int n = 0;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (pPlayer && !FNullEnt(pPlayer->pev) && pPlayer->IsAlive() && pPlayer->m_iTeam == team)
			n++;
	}
	return n;
}

float CCSBot::LsTempo()
{
	float now = gpGlobals->time;
	if (now - m_lsTempoAt > 0.5f || now < m_lsTempoAt)
	{
		m_lsTempoAt = now;
		int other = m_iTeam == CT ? TERRORIST : CT;
		m_lsTempo = (m_iTeam == CT || m_iTeam == TERRORIST) && !LsSolo() ? float(LsAlive(m_iTeam) - LsAlive(other)) : 0.0f;
	}
	return m_lsTempo;
}

// Somewhere the enemy can't have reached yet this round (the earliest they could, from their
// spawn, is still a few seconds off): a good player doesn't watch it yet. Newer players don't
// time it, and it means nothing in deathmatch or once the round's well on
float CCSBot::LsTimingWeight(const CNavArea *area)
{
	if (!area || LsSolo() || !LsKnows(LSK_TIMINGS) || !TheCSBots())
		return 1.0f;
	float elapsed = TheCSBots()->GetElapsedRoundTime();
	if (elapsed > 60.0f)
		return 1.0f;
	float arrive = LsArrival(area, m_iTeam == CT ? TERRORIST : CT);
	if (arrive < 0.0f)
		return 1.0f;
	// (a margin: a newer player's sense of it is rougher)
	float slack = 2.0f + 3.0f * (1.0f - LsSkill());
	return elapsed + slack < arrive ? 0.25f : 1.0f;
}

void CCSBot::LsFeed(CBasePlayer *pVictim, CBasePlayer *pKiller)
{
	if (LsSolo() || BotRelationship(pVictim) != BOT_TEAMMATE)
		return;
	// (newer players hardly look at the radar and the feed)
	float skill = LsSkill();
	if (RANDOM_FLOAT(0.0f, 1.0f) > 0.25f + 0.75f * skill)
		return;
	m_lsFeedSpot = pVictim->pev->origin;
	m_lsFeedAt = gpGlobals->time;
	m_lsFeedSniper = pKiller && pKiller != pVictim && BotRelationship(pKiller) == BOT_ENEMY && LsGunClass(pKiller) == LSG_SNIPER;
	// (the teammate's last words: "1 low" - the killer's hurt)
	if (pKiller && pKiller != pVictim && pVictim->IsBot() && pKiller->IsAlive() && BotRelationship(pKiller) == BOT_ENEMY && pKiller->pev->health < 40.0f)
		LsCallHurt(pKiller, m_iTeam, pKiller->pev->health, pKiller->pev->origin);
	// (killed by an AWP there: that lane's covered - the better ones keep off it a while)
	if (m_lsFeedSniper && skill > 0.45f)
	{
		Vector at = pVictim->pev->origin;
		CNavArea *area = TheNavAreaGrid.GetNearestNavArea(&at);
		if (area && area != m_lastKnownArea)
			LsForceBadArea(area, 10.0f + 12.0f * skill);
	}
}

// Where each side's players died lately (the radar shows a teammate's death): the last few
struct LsTeamDeath { Vector at; float when; int team; int map; };
static LsTeamDeath s_teamDeaths[16];
static int s_teamDeathNext = 0;

bool LsRecentDeathNear(int team, const Vector &at, float range, float secs)
{
	float now = gpGlobals->time;
	int map = LsMapSerial();
	for (int i = 0; i < 16; i++)
	{
		const LsTeamDeath &d = s_teamDeaths[i];
		if (d.map == map && d.team == team && d.when <= now && now - d.when < secs && (d.at - at).IsLengthLessThan(range))
			return true;
	}
	return false;
}

// A player's death for the "teammate died there" memory: bots note their own; a person's is
// noted from the bots' events, once (every bot hears the same death)
void LsNoteAnyDeath(CBasePlayer *pVictim)
{
	if (!pVictim)
		return;
	float now = gpGlobals->time;
	int map = LsMapSerial();
	for (int i = 0; i < 16; i++)
	{
		const LsTeamDeath &d = s_teamDeaths[i];
		if (d.map == map && d.team == pVictim->m_iTeam && now - d.when < 0.5f && (d.at - pVictim->pev->origin).IsLengthLessThan(64.0f))
			return;
	}
	s_teamDeaths[s_teamDeathNext].at = pVictim->pev->origin;
	s_teamDeaths[s_teamDeathNext].when = now;
	s_teamDeaths[s_teamDeathNext].team = pVictim->m_iTeam;
	s_teamDeaths[s_teamDeathNext].map = map;
	s_teamDeathNext = (s_teamDeathNext + 1) % 16;
}

void CCSBot::LsNoteDeath(CBasePlayer *pKiller)
{
	s_teamDeaths[s_teamDeathNext].at = pev->origin;
	s_teamDeaths[s_teamDeathNext].when = gpGlobals->time;
	s_teamDeaths[s_teamDeathNext].team = m_iTeam;
	s_teamDeaths[s_teamDeathNext].map = LsMapSerial();
	s_teamDeathNext = (s_teamDeathNext + 1) % 16;
	m_lsDiedArea = m_lastKnownArea;
	m_lsDiedMap = LsMapSerial();
	m_lsDiedRound = LsRoundSerial();
	// (a person who killed it: where they were - a spot they like)
	if (pKiller && pKiller != this && !pKiller->IsBot() && BotRelationship(pKiller) == BOT_ENEMY)
	{
		s_habits[s_habitNext].at = pKiller->pev->origin;
		s_habits[s_habitNext].map = LsMapSerial();
		s_habits[s_habitNext].who = pKiller->entindex();
		s_habitNext = (s_habitNext + 1) % 12;
	}
}

// An enemy's bullets landing by a teammate it can't see shooting: a fight's going on there.
// Free and not too far, it turns that way and treats it as gunfire there (to help or keep
// clear, as the stock noise handling and the trade decide); newer players often miss it
void CCSBot::LsImpactNearMate(CBasePlayer *pShooter, const Vector &impact)
{
	float now = gpGlobals->time;
	if (now - m_lsImpactAt < 6.0f || !pShooter->IsPlayer() || !pShooter->IsAlive() || !IsEnemy(pShooter) || IsAttacking() || LsSolo())
		return;
	// (already on a fresh noise: that comes first)
	if (IsNoiseHeard() && now - m_noiseTimestamp < 4.0f)
		return;
	if ((impact - pev->origin).IsLengthLessThan(150.0f) || (impact - pev->origin).IsLengthGreaterThan(1300.0f))
		return;
	bool mate = false;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		if ((pPlayer->pev->origin - impact).IsLengthLessThan(120.0f))
			mate = true;
	});
	if (!mate)
		return;
	m_lsImpactAt = now;
	if (RANDOM_FLOAT(0.0f, 1.0f) > 0.2f + 0.4f * LsSkill())
		return;
	// (the shooter as heard, roughly)
	float err = float((pShooter->pev->origin - pev->origin).Length()) * 0.12f;
	Vector heard = pShooter->pev->origin + Vector(RANDOM_FLOAT(-err, err), RANDOM_FLOAT(-err, err), 0.0f);
	CNavArea *area = TheNavAreaGrid.GetNearestNavArea(&heard);
	if (!area)
		return;
	if (!IsNoiseHeard() || GetNoisePriority() <= PRIORITY_MEDIUM)
	{
		m_noisePosition = heard;
		m_noiseArea = area;
		m_noisePriority = PRIORITY_MEDIUM;
		m_noiseTimestamp = now;
	}
	if (!LsPreAim(heard, RANDOM_FLOAT(1.0f, 2.0f)))
	{
		Vector look = impact;
		SetLookAt("Loner-Strike: teammate under fire", &look, PRIORITY_MEDIUM, RANDOM_FLOAT(0.8f, 1.5f));
	}
	LsWhy("teammate under fire");
}

// Where to defuse from: beside the bomb on the side away from the threat (the last place an
// enemy was seen, or the kill feed's spot), so the bomb and what's around it is between them;
// the bomb itself if nothing's known or no side is hidden. The better players think of it
Vector CCSBot::LsDefuseSpot(const Vector &bomb)
{
	if (!LsFight() || LsSkill() < 0.35f)
		return bomb;
	Vector threat;
	if (GetTimeSinceLastSawEnemy() < 20.0f)
		threat = GetLastKnownEnemyPosition();
	else if (gpGlobals->time - m_lsFeedAt < 20.0f)
		threat = m_lsFeedSpot;
	else
		return bomb;
	Vector from = threat + Vector(0, 0, 17.0f);
	Vector best = bomb;
	float bestScore = -1e9f;
	for (int i = 0; i < 8; i++)
	{
		float yaw = float(i) * 45.0f;
		Vector spot = bomb + Vector(BotCOS(yaw) * 40.0f, BotSIN(yaw) * 40.0f, 0.0f);
		// (standing room, and ground)
		TraceResult tr;
		UTIL_TraceHull(bomb + Vector(0, 0, 36.0f), spot + Vector(0, 0, 36.0f), ignore_monsters, human_hull, ENT(pev), &tr);
		if (tr.flFraction < 1.0f || tr.fStartSolid)
			continue;
		// (a crouched defuser's head out of the threat's line is the point)
		UTIL_TraceLine(from, spot + Vector(0, 0, 20.0f), ignore_monsters, ignore_glass, ENT(pev), &tr);
		float score = (tr.flFraction < 1.0f ? 500.0f : 0.0f) + float((spot - threat).Length2D()) * 0.2f;
		if (score > bestScore)
		{
			bestScore = score;
			best = spot;
		}
	}
	return best;
}

// What a team's been told about hurt enemies: by whom, how low, when. CS has no healing, so it
// stays true the rest of their life (a new life, a new round: forgotten)
static float s_hurtAt[2][MAX_CLIENTS + 1];
static float s_hurtHp[2][MAX_CLIENTS + 1];
static int s_hurtRound[2][MAX_CLIENTS + 1];
static Vector s_hurtWhere[2][MAX_CLIENTS + 1];

void LsCallHurt(CBasePlayer *pEnemy, int team, float hp, const Vector &where)
{
	int t = team == TERRORIST ? 0 : team == CT ? 1 : -1;
	int i = pEnemy ? pEnemy->entindex() : 0;
	if (t < 0 || i < 1 || i > MAX_CLIENTS)
		return;
	s_hurtAt[t][i] = gpGlobals->time;
	// (as a player calls it: "10", "20", ... - "1" under ten)
	s_hurtHp[t][i] = hp < 10.0f ? 5.0f : float(int((hp + 9.0f) / 10.0f) * 10);
	s_hurtRound[t][i] = LsRoundSerial();
	s_hurtWhere[t][i] = where;
}

bool LsKnownHurt(CBasePlayer *pEnemy, int team, float *hp, Vector *where, float *when)
{
	int t = team == TERRORIST ? 0 : team == CT ? 1 : -1;
	int i = pEnemy ? pEnemy->entindex() : 0;
	if (t < 0 || i < 1 || i > MAX_CLIENTS || s_hurtRound[t][i] != LsRoundSerial() || s_hurtAt[t][i] > gpGlobals->time
		|| gpGlobals->time - s_hurtAt[t][i] > 40.0f || !pEnemy->IsAlive())
		return false;
	if (hp)
		*hp = s_hurtHp[t][i];
	if (where)
		*where = s_hurtWhere[t][i];
	if (when)
		*when = s_hurtAt[t][i];
	return true;
}

// An enemy called hurt somewhere it isn't (out of its sight): a free bot not far off, bold or a
// team player, goes after them - to where they were, quickly (newer players don't listen)
void CCSBot::LsHurtCallTick()
{
	float now = gpGlobals->time;
	if (!LsFight() || LsSolo() || now - m_lsHurtPushAt < 6.0f || !LsFree() || IsHiding() || LsRushing() || TheCSBots()->IsBombPlanted()
		|| pev->health < 65.0f - 30.0f * LsAggro())
		return;
	m_lsHurtPushAt = now - 4.0f;	// (looked at once every two seconds or so)
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pEnemy = UTIL_PlayerByIndex(i);
		float hp = 100.0f, when = 0.0f;
		Vector at;
		if (!pEnemy || !pEnemy->IsAlive() || !IsEnemy(pEnemy) || !LsKnownHurt(pEnemy, m_iTeam, &hp, &at, &when) || hp > 35.0f)
			continue;
		if (now - when > 6.0f)
			continue;
		// (not running into where teammates just died chasing them - the better ones hold for it)
		// (more the better and the more patient it is; the bold still go)
		if (LsRecentDeathNear(m_iTeam, at, 600.0f, 10.0f)
			&& RANDOM_FLOAT(0.0f, 1.0f) < Q_min(0.95f, (0.2f + 0.7f * LsSkill()) * (1.0f - 0.5f * LsAggro())))
			continue;
		// (where the callout said: where they were hit, not where they are now)
		float d = float((at - pev->origin).Length());
		if (d > 1300.0f || d < 200.0f)
			continue;
		if (RANDOM_FLOAT(0.0f, 1.0f) > (0.15f + 0.45f * LsSkill()) * (0.5f + 0.5f * LsTeamwork() + 0.5f * LsAggro()))
		{
			m_lsHurtPushAt = now;
			return;
		}
		m_lsHurtPushAt = now;
		Run();
		Hurry(3.0f);
		MoveTo(&at, FASTEST_ROUTE);
		LsWhy("going after a hurt one");
		return;
	}
}

// ---- flashes, ladders, AWPers known ------------------------------------------------------

// The flashbangs in the air about to go off, looked up once a tenth of a second for all the bots
struct LsFlashSeen { Vector at, vel; float pop; edict_t *owner; bool he; };
static LsFlashSeen s_flashes[8];
static int s_flashCount = 0;
static float s_flashScanAt = -1.0f;

static void LsScanFlashes()
{
	float now = gpGlobals->time;
	if (now - s_flashScanAt < 0.1f && now >= s_flashScanAt)
		return;
	s_flashScanAt = now;
	s_flashCount = 0;
	CBaseEntity *pEnt = nullptr;
	while ((pEnt = UTIL_FindEntityByClassname(pEnt, "grenade")) != nullptr && s_flashCount < 8)
	{
		if (static_cast<CGrenade *>(pEnt)->m_bIsC4 || FStringNull(pEnt->pev->model))
			continue;
		bool he = FStrEq(STRING(pEnt->pev->model), "models/w_hegrenade.mdl");
		if (!he && !FStrEq(STRING(pEnt->pev->model), "models/w_flashbang.mdl"))
			continue;
		float pop = pEnt->pev->dmgtime;
		if (pop < now)
			continue;
		s_flashes[s_flashCount].at = pEnt->pev->origin;
		s_flashes[s_flashCount].vel = pEnt->pev->velocity;
		s_flashes[s_flashCount].pop = pop;
		s_flashes[s_flashCount].owner = pEnt->pev->owner;
		s_flashes[s_flashCount].he = he;
		s_flashCount++;
	}
}

// A flashbang in front of it, in view and close, about to pop (anyone's but its own, which it
// handles when it throws): the better players turn their back to it; newer ones eat it
void CCSBot::LsFlashWatch()
{
	float now = gpGlobals->time;
	// back round once it has popped, to where it came from: whoever threw it comes in behind it
	if (m_lsFlashBackAt > 0.0f && now >= m_lsFlashBackAt)
	{
		m_lsFlashBackAt = 0.0f;
		if (LsFight() && !IsBlind() && !IsEnemyVisible())
		{
			// (the turn away was only for the pop itself)
			if (m_lookAtDesc && !Q_strncmp(m_lookAtDesc, "Loner-Strike: turned", 20))
				ClearLookAt();
			SetLookAt("Loner-Strike: back to the flash", &m_lsFlashBackPos, PRIORITY_HIGH, 1.2f);
			LsWhy("back to where the flash came from");
		}
	}
	if (!LsFight() || now < m_lsFlashCheckAt || now < m_lsFlashAwayUntil || IsBlind())
		return;
	m_lsFlashCheckAt = now + 0.1f;
	LsScanFlashes();
	if (!s_flashCount)
		return;
	Vector eye = GetEyePosition();
	UTIL_MakeVectors(pev->v_angle);
	Vector fwd = gpGlobals->v_forward;
	for (int i = 0; i < s_flashCount; i++)
	{
		const LsFlashSeen &f = s_flashes[i];
		// an HE landing close (heard bouncing, seen): out of its reach - behind something from
		// it - the better ones; newer players often stand in it
		if (f.he)
		{
			if (f.owner == edict() || f.pop - now > 1.6f || (f.at - pev->origin).IsLengthGreaterThan(350.0f))
				continue;
			m_lsFlashAwayUntil = now + 1.5f;
			if (RANDOM_FLOAT(0.0f, 1.0f) < 0.15f + 0.7f * LsSkill() && LsTakeCoverFrom(f.at, 120.0f, IsAttacking(), 0.5f))
				LsWhy("away from the grenade");
			return;
		}
		if (f.owner == edict() || f.pop - now > 0.7f)
			continue;
		Vector to = f.at - eye;
		float d = float(to.Length());
		if (d < 1.0f || d > 1000.0f || float(DotProduct(to, fwd)) / d < 0.3f)
			continue;
		TraceResult tr;
		UTIL_TraceLine(eye, f.at, ignore_monsters, ignore_glass, ENT(pev), &tr);
		if (tr.flFraction < 1.0f)
			continue;
		m_lsFlashAwayUntil = now + 1.2f;
		if (RANDOM_FLOAT(0.0f, 1.0f) > 0.15f + 0.75f * LsSkill())
			return;
		Vector away = eye - to * (200.0f / d);
		away.z = eye.z;
		if (IsAimingAtEnemy())
			LsUrgentTurn(away, 0.9f, nullptr);
		else
			SetLookAt("Loner-Strike: turned from a flash", &away, PRIORITY_UNINTERRUPTABLE, 0.9f);
		LsWhy("turned from a flash");
		// (an enemy's: then straight back round to where it was thrown from, the way it flew in,
		// a little after the pop - the better it is, the quicker; a teammate's needs no look)
		CBaseEntity *pThrower = f.owner ? CBaseEntity::Instance(f.owner) : nullptr;
		if (pThrower && pThrower->IsPlayer() && IsEnemy(pThrower))
		{
			Vector back = f.vel;
			back.z = 0.0f;
			Vector src = f.at;
			if (back.Length() > 50.0f)
			{
				TraceResult tb;
				Vector far2 = f.at - back.Normalize() * 500.0f;
				UTIL_TraceLine(f.at, far2, ignore_monsters, ignore_glass, ENT(pev), &tb);
				src = f.at + (tb.vecEndPos - f.at) * 0.9f;
			}
			m_lsFlashBackPos = src;
			m_lsFlashBackPos.z = eye.z;
			m_lsFlashBackAt = Q_max(f.pop, now) + RANDOM_FLOAT(0.05f, 0.2f) + 0.5f * (1.0f - LsSkill());
		}
		return;
	}
}

bool CCSBot::LsLadderExposed() const
{
	if (!LsFight() || LsSkill() < 0.35f)
		return false;
	return GetTimeSinceLastSawEnemy() < 8.0f || (IsNoiseHeard() && GetNoisePriority() >= PRIORITY_MEDIUM && gpGlobals->time - m_noiseTimestamp < 6.0f);
}

// An AWPer known to be out that way (heard scoping or firing lately, or a teammate killed by one
// in the feed), roughly in the given direction
bool CCSBot::LsAwpKnownToward(const Vector &dir2d) const
{
	float now = gpGlobals->time;
	Vector spot;
	if (now - m_lsSniperHeardAt < 20.0f)
		spot = m_lsSniperSpot;
	else if (m_lsFeedSniper && now - m_lsFeedAt < 20.0f)
		spot = m_lsFeedSpot;
	else
		return false;
	Vector to = spot - pev->origin;
	to.z = 0.0f;
	float len = float(to.Length2D());
	return len > 300.0f && float(DotProduct(to, dir2d)) / len > 0.3f;
}

// A custom map's feel from its map knowledge: from the contested places (both teams can get
// there within a few seconds of each other: where fights happen), how far the longest clear view
// goes - the upper quarter of them, as players judge a map by its long lanes. Measured on the
// stock maps: cbble 2180, awp_india 2120 (long); italy 1270, scoutzknivez 1255, vertigo 1130
// (close); dust2, office, nuke and train sit between and get no lean. Only a clear end leans
static int LsMapOpenness()
{
	static int s_serial = -1, s_lean = 0;
	if (!s_know.ready || s_know.stage < STAGE_AREAS)
		return 0;
	if (s_serial == LsMapSerial())
		return s_lean;
	s_serial = LsMapSerial();
	s_lean = 0;
	std::vector<float> views;
	for (size_t i = 0; i < s_know.count && i < s_know.inView.size(); i++)
	{
		const CNavArea *a = s_know.areas[i];
		float t = LsArrival(a, TERRORIST), c = LsArrival(a, CT);
		if (t < 0.0f || c < 0.0f || fabs(t - c) > 6.0f)
			continue;
		float best = 0.0f;
		for (const auto &v : s_know.inView[i])
		{
			if (v.second < 170 || v.first >= s_know.count)
				continue;
			best = Q_max(best, float((*s_know.areas[v.first]->GetCenter() - *a->GetCenter()).Length()));
		}
		views.push_back(best);
	}
	if (views.size() < 20)
		return 0;
	std::sort(views.begin(), views.end());
	float q = views[(size_t)(views.size() * 0.75f)];
	s_lean = q > 2050.0f ? 1 : q < 1300.0f ? -1 : 0;
	return s_lean;
}

// The stock maps people know as long (rifles and AWPs) or close (SMGs and shotguns); awp_ maps
// are all AWP. A custom map is neither: its feel isn't guessed
int LsMapLean()
{
	const char *map = STRING(gpGlobals->mapname);
	if (!Q_strnicmp(map, "awp_", 4))
		return 2;
	static const char *longMaps[] = { "de_dust", "de_dust2", "de_aztec", "de_train", "de_cbble", "de_prodigy", "de_inferno" };
	static const char *closeMaps[] = { "cs_office", "cs_italy", "cs_militia", "cs_747", "cs_estate", "cs_backalley", "cs_siege", "de_vertigo", "de_piranesi" };
	for (const char *m : longMaps)
		if (!Q_stricmp(map, m))
			return 1;
	for (const char *m : closeMaps)
		if (!Q_stricmp(map, m))
			return -1;
	return LsMapOpenness();
}

// Told where an enemy is (a teammate's callout): as if it heard them there - unless it's on
// something louder and fresher already
void CCSBot::LsTold(const Vector &at)
{
	if (!LsFight() || LsMatch())
		return;
	Vector spot = at;
	CNavArea *area = TheNavAreaGrid.GetNearestNavArea(&spot);
	if (!area || (IsNoiseHeard() && GetNoisePriority() > PRIORITY_MEDIUM && gpGlobals->time - m_noiseTimestamp < 3.0f))
		return;
	m_noisePosition = spot;
	m_noiseArea = area;
	m_noisePriority = PRIORITY_MEDIUM;
	m_noiseTimestamp = gpGlobals->time;
}

// ---- state for the plugins, peeks over cover, aim maps ------------------------------------

// What it's doing, for the plugins to respect (pev->vuser1.y, bits): 1 defusing, 2 planting,
// 4 running from the bomb, 8 a plugin hold on, 16 hiding
void CCSBot::LsPublishState()
{
	LsPublishSites();
	int flags = 0;
	if (LsFight())
	{
		if (IsDefusingBomb())
			flags |= 1;
		if (m_state == &m_plantBombState)
			flags |= 2;
		if (IsEscapingFromBomb())
			flags |= 4;
		if (gpGlobals->time < m_lsPuppet.holdUntil)
			flags |= 8;
		if (IsHiding())
			flags |= 16;
	}
	pev->vuser1.y = float(flags);
	LsPressureTick();
}

// Pressure: what's coming at it right now - damage taken in the last few seconds, teammates
// going down near it, more than one enemy on it, and being low. It builds and fades (about 3 s).
// How a bot takes it is who it is: the weaker, the bolder and the hot-headed crack - faster
// clicks, longer sprays, shakier aim - while a strong player tightens up and picks its shots,
// as good players do. Published for the aim plugin in vuser1.z: above 0 the strain toward
// panic, below 0 composure (0 none). Not for stock bots
void CCSBot::LsPressureTick()
{
	float now = gpGlobals->time;
	float dt = Q_min(1.0f, Q_max(0.0f, now - m_lsPressureAt));
	m_lsPressureAt = now;
	if (!LsFight() || !IsAlive())
	{
		m_lsPressure = 0.0f;
		m_lsPressureHp = pev->health;
		pev->vuser1.z = 0.0f;
		return;
	}
	// (fading: about a third left after 3 s)
	m_lsPressure *= Q_max(0.0f, 1.0f - dt / 3.0f);
	// (hits: a full health bar's worth is a lot of pressure)
	float lost = m_lsPressureHp - pev->health;
	if (lost > 0.0f)
		m_lsPressure += lost / 70.0f;
	m_lsPressureHp = pev->health;
	// (a teammate dropping close by)
	if (now - m_lsPressureDeathAt > 1.5f && LsRecentDeathNear(m_iTeam, pev->origin, 900.0f, 1.0f))
	{
		m_lsPressure += 0.35f;
		m_lsPressureDeathAt = now;
	}
	// (more than one on it)
	if (GetNearbyEnemyCount() >= 2 && GetTimeSinceLastSawEnemy() < 1.0f)
		m_lsPressure += 0.25f * dt * float(Q_min(GetNearbyEnemyCount(), 4) - 1);
	m_lsPressure = Q_min(m_lsPressure, 1.5f);
	float felt = Q_max(m_lsPressure, pev->health < 35.0f ? 0.35f : 0.0f);
	// (how it takes it: skill most, then temper - careful players and campers keep their heads,
	// rushers and the hot-headed lose it - and the bot's own nerves)
	int style = LsStyle();
	float temper = (style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER) ? 0.7f : (style == LS_STYLE_RUSHER || style == LS_STYLE_KNIFER) ? 1.25f : 1.0f;
	temper *= (0.75f + 0.5f * LsAggro()) * (0.7f + 0.6f * LsTrait(53));
	float skill = LsSkill();
	float crack = powf(Q_max(0.0f, 1.0f - skill), 1.3f) * temper;
	float strain = 0.0f;
	if (crack > 0.12f)
		strain = felt * crack * 2.0f;
	else if (skill > 0.7f)
		strain = -felt * Q_min(1.0f, (skill - 0.7f) / 0.25f);
	pev->vuser1.z = strain;
}

// Holding a spot behind low cover (stock hides crouched): now and then it comes up for a look
// over it and goes back down - the better ones, more on aim maps. After the state machine (which
// crouches it every frame there)
void CCSBot::LsCoverPeekTick()
{
	float now = gpGlobals->time;
	if (!LsFight() || !IsHiding() || !IsAtHidingSpot() || IsAttacking() || LsSkill() < 0.35f || LsPuppetActive())
	{
		m_lsCoverPeekUp = false;
		return;
	}
	if (now >= m_lsCoverPeekUntil)
	{
		if (m_lsCoverPeekUp)
		{
			m_lsCoverPeekUp = false;
			m_lsCoverPeekUntil = now + RANDOM_FLOAT(2.0f, 4.5f);
		}
		else
		{
			UTIL_MakeVectors(pev->v_angle);
			Vector at = GetEyePosition() + gpGlobals->v_forward * 400.0f;
			if (LsLowCoverToward(at))
			{
				m_lsCoverPeekUp = true;
				m_lsCoverPeekUntil = now + RANDOM_FLOAT(0.7f, 1.4f);
			}
			else
				m_lsCoverPeekUntil = now + 1.5f;
		}
	}
	if (m_lsCoverPeekUp)
		StandUp();
}

// aim_, fy_ and awp_ maps with teams: no rounds of rushing across - from cover to cover toward
// them, a hold at each (a box to peek from), and a push only with an edge (ahead on numbers,
// one called hurt); rushers hold least, careful players most. Nothing once it's near their spawn
void CCSBot::LsAimMapTick()
{
	float now = gpGlobals->time;
	int kind = LsMapKind();
	if (!LsFight() || LsSolo() || now < m_lsAimMapAt || (kind != LSMAP_AIM && kind != LSMAP_FY && kind != LSMAP_AWP))
		return;
	m_lsAimMapAt = now + 1.0f;
	if (CSGameRules()->IsFreezePeriod() || IsAttacking() || IsHiding() || !LsFree() || GetTimeSinceLastSawEnemy() < 2.0f || !m_lastKnownArea)
		return;
	const Vector *theirs = LsEnemySpawn(m_iTeam);
	if (!theirs || (*theirs - pev->origin).IsLengthLessThan(500.0f))
		return;
	// (an edge: go)
	float hp = 100.0f;
	bool edge = LsTempo() >= 1.0f;
	for (int i = 1; i <= gpGlobals->maxClients && !edge; i++)
	{
		CBasePlayer *p = UTIL_PlayerByIndex(i);
		edge = p && p->IsAlive() && IsEnemy(p) && LsKnownHurt(p, m_iTeam, &hp) && hp < 50.0f;
	}
	if (edge && RANDOM_FLOAT(0.0f, 1.0f) < 0.5f + 0.4f * LsAggro())
		return;
	Vector dir = *theirs - pev->origin;
	dir.z = 0.0f;
	dir.NormalizeInPlace();
	Vector ahead = pev->origin + dir * 350.0f;
	const Vector *spot = FindNearbyHidingSpot(this, &ahead, m_lastKnownArea, 500.0f, false, true);
	// (a spot that's forward, not back the way it came)
	if (!spot || float((*theirs - *spot).Length2D()) > float((*theirs - pev->origin).Length2D()) - 120.0f)
		return;
	int style = LsStyle();
	float hold = style == LS_STYLE_RUSHER ? RANDOM_FLOAT(1.5f, 3.0f) : (style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER) ? RANDOM_FLOAT(6.0f, 10.0f) : RANDOM_FLOAT(3.5f, 7.0f);
	Hide(spot, hold, true);
	LsWhy("aim map: to the next cover");
}

// The smoke for an AWP's lane, thrown once it's in cover (if it still has one, and isn't fighting)
void CCSBot::LsSmokeLaneTick()
{
	if (m_lsSmokeLaneAt <= 0.0f || gpGlobals->time < m_lsSmokeLaneAt)
		return;
	m_lsSmokeLaneAt = 0.0f;
	if (!LsFight() || IsAttacking() || IsEnemyVisible() || !LsHasNade(WEAPON_SMOKEGRENADE) || m_isWaitingToTossGrenade || IsUsingLadder())
		return;
	LsNadeStart(WEAPON_SMOKEGRENADE, m_lsSmokeLaneSpot, RANDOM_FLOAT(4.0f, 8.0f));
	LsWhy("smoking the AWP's lane");
}

bool CCSBot::LsOrderActive() const
{
	return m_lsOrder.kind != 0 && gpGlobals->time < m_lsOrder.until;
}

// ---- objective hunting, range, shooting through cover --------------------------------------

// Somewhere worth going on an objective map, out of a few dozen areas: not one nearer its own
// spawn than it is now (no wandering home), on the way to the other side; a T leans to the
// round's site and the bomb sites, a CT to the ways the Ts come; ones its team hasn't looked
// at lately first
CNavArea *CCSBot::LsObjectiveHuntArea()
{
	int n = (int)TheNavAreaList.size();
	if (n < 2)
		return nullptr;
	const Vector *own = LsEnemySpawn(m_iTeam == CT ? TERRORIST : CT);
	const Vector *theirs = LsEnemySpawn(m_iTeam);
	if (!own || !theirs)
		return nullptr;
	float myHome = float((*own - pev->origin).Length2D());
	const CCSBotManager::Zone *site = (m_iTeam == TERRORIST) ? LsTSite() : nullptr;
	static std::vector<CNavArea *> s_list;
	static int s_serial = -1;
	if (s_serial != g_lsNavLoads || (int)s_list.size() != n)
	{
		s_serial = g_lsNavLoads;
		s_list.assign(TheNavAreaList.begin(), TheNavAreaList.end());
	}
	CNavArea *best = nullptr;
	float bestScore = -1e9f;
	for (int t = 0; t < 40; t++)
	{
		CNavArea *a = s_list[RANDOM_LONG(0, n - 1)];
		const Extent *e = a->GetExtent();
		if (e->hi.x - e->lo.x < 100.0f || e->hi.y - e->lo.y < 100.0f || !LsHeadroom(a) || !LsSamePart(this, a) || LsOverBrokenFloor(a))
			continue;
		const Vector &c = *a->GetCenter();
		float home = float((*own - c).Length2D());
		if (home < myHome - 150.0f)
			continue;
		// (not back where it's been the last half minute: no circling round)
		if (LsBeenNear(c, 600.0f))
			continue;
		// (nor one behind the way it's going: no turning round on the way in for a new goal)
		if (LsBehindMe(c))
			continue;
		float score = Q_min(60.0f, gpGlobals->time - a->GetClearedTimestamp(m_iTeam - 1)) * 10.0f;
		score += 0.3f * (home - myHome);
		if (site)
			score -= 0.4f * float((c - site->m_center).Length2D());
		else if (m_iTeam == TERRORIST)
		{
			float nearest = 1e9f;
			for (int z = 0; z < TheCSBots()->GetZoneCount(); z++)
				nearest = Q_min(nearest, float((c - TheCSBots()->GetZone(z)->m_center).Length2D()));
			score -= 0.3f * nearest;
		}
		score += RANDOM_FLOAT(0.0f, 150.0f);
		if (score > bestScore)
		{
			bestScore = score;
			best = a;
		}
	}
	return best;
}

// Its gun can't do anything from here (a shotgun across a room, an SMG or a pistol down a long
// lane) and the enemy isn't on it: a player who knows closes in instead of spraying; a newer one
// sprays anyway. One called nearly dead is worth a go from anywhere
bool CCSBot::LsOutOfRange(CBasePlayer *pEnemy, float range)
{
	if (!LsFight() || LsSkill() < 0.3f || !pEnemy)
		return false;
	int mine = LsGunClass(this);
	bool tooFar = (mine == LSG_SHOTGUN && range > 900.0f) || (mine == LSG_SMG && range > 1600.0f) || (mine == LSG_PISTOL && range > 1500.0f);
	if (!tooFar)
		return false;
	float hp = 100.0f;
	if (LsKnownHurt(pEnemy, m_iTeam, &hp) && hp < 30.0f)
		return false;
	// (it's shooting at it: no standing there taking it - fire back)
	// (a slow tapper hits every couple of seconds: the window covers it, not just the instant
	// after a hit)
	if (GetAttacker() == pEnemy && GetTimeSinceAttacked() < 4.0f)
		return false;
	// (the patient close in; rushers and the impatient spray from there now and then - a steady
	// lean per bot, not a coin each think)
	int style = LsStyle();
	float sprays = (style == LS_STYLE_RUSHER || style == LS_STYLE_KNIFER) ? 0.5f : 0.35f * LsAggro() * (1.0f - LsSkill());
	return LsTrait(50) >= sprays;
}

// Only the enemy's head showing over something (a box, a thin wall) and the bullet would go
// through it at the chest: the better players who know what's bangable aim there. Looked at a
// few times a second
bool CCSBot::LsBangChest()
{
	float now = gpGlobals->time;
	if (now < m_lsBangChestAt)
		return m_lsBangChest;
	m_lsBangChestAt = now + 0.3f;
	m_lsBangChest = false;
	CBasePlayer *pEnemy = m_enemy;
	CBasePlayerWeapon *pGun = GetActiveWeapon();
	if (!LsFight() || !pEnemy || !pGun || LsSkill() < 0.55f || !LsKnows(LSK_PENETRATION))
		return false;
	int cls = LsGunClass(this);
	if (cls != LSG_RIFLE && cls != LSG_SNIPER && cls != LSG_DEAGLE && cls != LSG_MG)
		return false;
	Vector chest = pEnemy->pev->origin + Vector(0, 0, 10.0f);
	TraceResult first;
	m_lsBangChest = LsBulletGetsThrough(this, pGun->m_iId, GetEyePosition(), chest, &first);
	return m_lsBangChest;
}

// A reason to have the gun out right now, in the early safe time too: an enemy seen or fought,
// shot at, a noise close and fresh, or standing somewhere the other side could already have
// reached (their earliest arrival, with a few seconds to spare)
bool CCSBot::LsKnifeDanger()
{
	float now = gpGlobals->time;
	if (IsAttacking() || IsEnemyVisible() || GetTimeSinceLastSawEnemy() < 4.0f || GetTimeSinceAttacked() < 4.0f || now - m_lsShotAtAt < 4.0f)
		return true;
	if (IsNoiseHeard() && now - m_noiseTimestamp < 4.0f && (m_noisePosition - pev->origin).IsLengthLessThan(1500.0f))
		return true;
	if (LsSolo() || !m_lastKnownArea || !TheCSBots())
		return false;
	float arrive = LsArrival(m_lastKnownArea, m_iTeam == CT ? TERRORIST : CT);
	return arrive >= 0.0f && TheCSBots()->GetElapsedRoundTime() + 4.0f > arrive;
}

// Right at a doorway or round a corner into somewhere it can't see from here: the next area on
// its path, close, and out of this one's sight (people face where they're going in)
bool CCSBot::LsAboutToEnter()
{
	if (!HasPath() || m_pathIndex < 0 || m_pathIndex >= m_pathLength || !m_lastKnownArea || !LsSightReady())
		return false;
	const CNavArea *next = m_path[m_pathIndex].area;
	if (!next || next == m_lastKnownArea || (m_path[m_pathIndex].pos - pev->origin).IsLengthGreaterThan(250.0f))
		return false;
	return !LsSees(m_lastKnownArea, next);
}

// Where it's been: a spot every few seconds, the last eight (about half a minute)
void CCSBot::LsNoteWhereItIs()
{
	float now = gpGlobals->time;
	if (now < m_lsRecentAt && now >= m_lsRecentAt - 10.0f)
		return;
	m_lsRecentAt = now + 4.0f;
	m_lsRecent[m_lsRecentNext] = pev->origin;
	m_lsRecentNext = (m_lsRecentNext + 1) % 8;
}

bool CCSBot::LsBeenNear(const Vector &at, float range) const
{
	for (int i = 0; i < 8; i++)
		if ((m_lsRecent[i] - at).Make2D().IsLengthLessThan(range))
			return true;
	return false;
}

// Behind it, the way it's been going (moving, or the way its path ran): a new goal there means
// turning round - fine for a reason (a call, a fight, a switch of site), not for a fresh hunt pick
bool CCSBot::LsBehindMe(const Vector &at) const
{
	Vector2D way;
	if (pev->velocity.Length2D() > 60.0f)
		way = pev->velocity.Make2D();
	else
		return false;
	way.NormalizeInPlace();
	Vector2D to = (at - pev->origin).Make2D();
	float len = float(to.Length());
	if (len < 400.0f)
		return false;
	return float(DotProduct(to, way)) / len < -0.3f;
}

// The dropped bomb lies where a teammate just died: someone's holding that angle
bool CCSBot::LsBombHot(const Vector &bomb)
{
	return LsRecentDeathNear(m_iTeam, bomb, 500.0f, 15.0f);
}

// bot_ls_trace: an enemy first in view (after not having been for a few seconds): with the
// #attack line, the true time from seeing to the fight
void CCSBot::LsTraceSeen(CBasePlayer *pEnemy)
{
	if (cv_bot_ls_trace.value <= 0.0f || !pEnemy)
		return;
	FILE *f = LsTraceFile();
	if (!f)
		return;
	fprintf(f, "#seen,%.2f,%d,%d,%d,%.0f\n", gpGlobals->time, entindex(), m_iTeam, pEnemy->entindex(), float((pEnemy->pev->origin - pev->origin).Length()));
	fflush(f);
}
