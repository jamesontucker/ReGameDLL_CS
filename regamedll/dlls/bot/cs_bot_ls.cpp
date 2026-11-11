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
const char LS_PEEK[] = "Loner-Strike: peek";

void Bot_LsRegisterCVars()
{
	CVAR_REGISTER(&cv_bot_ls_own_memory);
	CVAR_REGISTER(&cv_bot_ls_routes);
	CVAR_REGISTER(&cv_bot_ls_lines);
	CVAR_REGISTER(&cv_bot_ls_pauses);
	CVAR_REGISTER(&cv_bot_ls_solo);
	CVAR_REGISTER(&cv_bot_ls_fight);
	CVAR_REGISTER(&cv_bot_ls_match);
	CVAR_REGISTER(&cv_bot_ls_trace);
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
	const float apart = 220.0f;
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
	return !IsAttacking() && !IsBusy() && !IsCarryingBomb() && !m_lsSaving && !LsPuppetActive() && !LsOrderActive() && !IsDefusingBomb()
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

// A T died near the round's site early on: two of those and the team reads it as the CTs
// stacking that site, and goes for the other one ("they're all at b, go a")
void LsTSiteDeath(CBasePlayer *pVictim)
{
	const CCSBotManager::Zone *site = LsTSite();
	if (!site || pVictim->m_iTeam != TERRORIST || TheCSBots()->IsBombPlanted() || TheCSBots()->GetZoneCount() < 2
		|| TheCSBots()->GetElapsedRoundTime() > 60.0f || gpGlobals->time == s_tSiteLastDeath)
		return;
	s_tSiteLastDeath = gpGlobals->time;
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
	if (LsFight() && LsTeamGame() && LsKnows(LSK_EXPOSURE))
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
		|| (IsCarryingBomb() && !(CSGameRules() && CSGameRules()->IsFreezePeriod()));
}

// Too busy for a person's order (follow me, go b, wait): the bomb, a hostage, a grenade
// on its way. Being in a hurry, or a noise a moment ago, doesn't stop it doing as asked
bool CCSBot::LsOrderBusy() const
{
	return IsDefusingBomb() || IsEscapingFromBomb() || TheCSBots()->IsBombPlanted() || m_isWaitingToTossGrenade
		|| GetHostageEscortCount() > 0 || (IsCarryingBomb() && !(CSGameRules() && CSGameRules()->IsFreezePeriod()));
}

void CCSBot::LsPuppetRelease()
{
	if (m_lsPuppet.crouchUntil > 0.0f)
		StandUp();
	if (m_lsPuppet.followUntil > 0.0f && IsFollowing())
		StopFollowing();
	if (m_lsPuppet.lookUntil > 0.0f)
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
	bool urgent = p.urgent && gpGlobals->time < p.lookUntil;
	// (a person's "wait" doesn't keep it standing while it's being hit)
	if (FStrEq(p.holdTag, "bc") && gpGlobals->time < p.holdUntil && (GetTimeSinceAttacked() < 1.0f || IsSurprised()))
	{
		LsPuppetRelease();
		return;
	}
	if (m_isEnemyVisible || IsOnLadder() || !LsPuppetActive() || (!urgent && (IsAttacking() || (order ? LsOrderBusy() : LsBusy()) || IsBlind())))
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
		SetLookAt("Loner-Strike", &at, p.urgent ? PRIORITY_UNINTERRUPTABLE : PRIORITY_HIGH, 0.3f, false, 2.0f);
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
		if (!IsMovingTo() && now >= o.nextTry)
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
	bool order = FStrEq(what, "goto") || FStrEq(what, "follow") || (FStrEq(what, "hold") && CMD_ARGC() > 4 && FStrEq(CMD_ARGV(4), "bc"));
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
			LsPuppetRelease();
			return;
		}
		if (FStrEq(tag, p.holdTag))
		{
			p.holdUntil = p.crouchUntil = 0.0f;
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
		if (chat || until > p.holdUntil)
		{
			p.holdUntil = chat ? Q_max(until, p.holdUntil) : until;
			Q_strlcpy(p.holdTag, tag);
		}
		if (chat)
			pev->fuser4 = now; m_lsOrderFields = true;	// (taken: gm_botchat reads it)
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

			const Vector *c = area->GetCenter();
			float score = RANDOM_FLOAT(0.0f, 500.0f);

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
		s_edgeSlipAt = gpGlobals->time;
		LsWhy("backed off an edge");
		return true;
	}
	return false;
}

// Holds fire while moving fast with a rifle at range, to stop and then shoot: what good
// players do. Newer ones (skill under a half) run and gun like pub players did
bool CCSBot::LsHoldFireToStop(float range)
{
	if (LsSkill() < 0.5f || range < 500.0f)
		return false;

	CBasePlayerWeapon *pWeapon = GetActiveWeapon();
	if (!pWeapon)
		return false;

	switch (pWeapon->m_iId)
	{
	case WEAPON_AK47: case WEAPON_M4A1: case WEAPON_GALIL: case WEAPON_FAMAS:
	case WEAPON_SG552: case WEAPON_AUG: case WEAPON_DEAGLE:
		break;
	default:
		return false;
	}
	// moving fast enough to spoil the shot (half its top speed: follows a mode's speeds)
	return pev->velocity.Length2D() > 0.5f * pev->maxspeed;
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
		if (LsTrait(18) > 0.45f && RANDOM_FLOAT(0.0f, 1.0f) < 0.4f)
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
	if (!LsFight() || IsSafe() || IsUsingLadder() || IsAttacking())
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
	bool loud = IsHurrying() || GetTimeSinceAttacked() < 5.0f || GetTimeSinceLastSawEnemy() < 6.0f || IsSafe()
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
		want = danger > 0.1f || (lateRound && m_lsSneakLate) || IsNoiseHeard();
	}

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
void CCSBot::LsTrace()
{
	if (cv_bot_ls_trace.value <= 0.0f || !IsAlive() || gpGlobals->time < m_lsTraceAt)
		return;
	m_lsTraceAt = gpGlobals->time + 0.5f;

	static FILE *s_file = nullptr;
	if (!s_file)
	{
		s_file = fopen("lstrace.csv", "a");
		if (!s_file)
			return;
	}
	// (a line naming the map whenever it changes: the clock restarts with each map)
	static char s_map[64] = "";
	static float s_last = 0.0f;
	if (Q_strcmp(s_map, STRING(gpGlobals->mapname)) != 0 || gpGlobals->time < s_last)
	{
		Q_strlcpy(s_map, STRING(gpGlobals->mapname));
		fprintf(s_file, "#map,%s\n", s_map);
	}
	s_last = gpGlobals->time;
	CBasePlayer *pEnemy = GetEnemy();
	CBasePlayer *pLeader = IsFollowing() ? GetFollowLeader() : nullptr;
	Vector goal = HasPath() ? GetPathEndpoint() : Vector(0, 0, 0);
	const Vector *noise = IsNoiseHeard() ? &m_noisePosition : nullptr;
	float speed = float(pev->velocity.Length2D());
	float off = speed > 1.0f ? NormalizeAngle(pev->v_angle.y - UTIL_VecToYaw(pev->velocity)) : 0.0f;
	const char *look = (m_lookAtSpotState != NOT_LOOKING_AT_SPOT && m_lookAtDesc) ? m_lookAtDesc : "-";
	fprintf(s_file, "%.2f,%d,%d,%s,%d,%d,%d,%.0f,%.0f,%.0f,%.0f,%s,%.1f,%.0f,%.0f,%d,%s\n",
		gpGlobals->time, entindex(), m_iTeam, m_state ? m_state->GetName() : "-", int(GetTask()),
		pEnemy ? pEnemy->entindex() : 0, pLeader ? pLeader->entindex() : 0,
		goal.x, goal.y, noise ? noise->x : 0.0f, noise ? noise->y : 0.0f,
		m_lsWhy, m_lsWhy[0] ? gpGlobals->time - m_lsWhyAt : 0.0f,
		speed, off, IsAttacking() ? 1 : 0, look);
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
		// (never every time: good players sometimes swing straight back to trade)
		float odds = Q_min(0.75f, 0.1f + 0.65f * LsSkill() + (sniper ? 0.15f : 0.0f)) * (LsTeamGame() ? 1.0f : 0.5f) * (LsStyle() == LS_STYLE_RUSHER ? 0.6f : 1.0f);
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
	if (DotProduct(gpGlobals->v_forward, dir) > 0.996f && (!semi || int(now * 8.0f) % 2 == 0))
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
	// (a plugin's hold or look gives way to an enemy heard close by)
	if (LsPuppetActive() && dist < 700.0f)
		LsPuppetRelease();
	bool busy = m_state == &m_plantBombState || m_state == &m_defuseBombState || IsUsingLadder() || m_lsSaving || LsPuppetActive() || IsBlind();

	// an enemy scoping a sniper rifle: there's an AWPer about, roughly there
	if (event == EVENT_WEAPON_ZOOMED)
	{
		CBasePlayerWeapon *pGun = static_cast<CBasePlayerWeapon *>(pPlayer->m_pActiveItem);
		if (pGun && (pGun->m_iId == WEAPON_AWP || pGun->m_iId == WEAPON_SCOUT || pGun->m_iId == WEAPON_G3SG1 || pGun->m_iId == WEAPON_SG550))
		{
			m_lsSniperHeard = pPlayer;
			m_lsSniperSpot = pPlayer->pev->origin;
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
			if (first && !(fighting && pEnemy->pev->health < 30.0f && skill > 0.5f))
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
	if (dist > 900.0f)
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
		// (toward where it came from, tipped 20-35 degrees up or down, not at the ceiling or the floor)
		Vector flat = pPlayer->pev->origin - pev->origin;
		flat.z = 0.0f;
		flat = flat.IsZero() ? Vector(BotCOS(pev->v_angle.y), BotSIN(pev->v_angle.y), 0.0f) : flat.Normalize();
		Vector glance = pev->origin + pev->view_ofs + 200.0f * flat + Vector(0, 0, (dz > 0.0f ? 1.0f : -1.0f) * RANDOM_FLOAT(75.0f, 140.0f));
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
	if (dist < 700.0f && skill > 0.63f && LsTrait(33) < skill - 0.33f && now - m_lsBangHeardAt > 5.5f && RANDOM_FLOAT(0.0f, 1.0f) < 0.32f + 0.3f * (skill - 0.63f))
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
		if (LsPreAim(heard, RANDOM_FLOAT(1.5f, 3.0f)))
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
const unsigned int LSV_VERSION = 5;
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
			return true;
		if (tr.fStartSolid || !SeeThrough(tr.pHit))
			return false;
		start = tr.vecEndPos;
		skip = tr.pHit;
	}
	return true;
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

// A little more of the map's knowledge, within this frame's budget (CCSBotManager::StartFrame)
void LsSightStep()
{
	if (!LsFight() || TheNavAreaList.empty() || !gpGlobals->mapname)
		return;
	// (bots learning the map: its nav is still being built; the knowledge waits for the finished one)
	if (TheCSBots() && (TheCSBots()->IsLearningMap() || TheCSBots()->IsAnalysisRequested()))
		return;
	// (the nav replaced, even with the same number of areas: its first area is a new one)
	if (!s_know.tried || Q_strcmp(s_know.map, STRING(gpGlobals->mapname)) != 0
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
Vector s_spawn[2];
bool s_hasSpawn[2] = {};

void LsMapKindUpdate()
{
	if (!gpGlobals->mapname || Q_strcmp(s_kindMap, STRING(gpGlobals->mapname)) == 0)
		return;
	Q_strlcpy(s_kindMap, STRING(gpGlobals->mapname));
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
	if (!LsFight() || !m_lastKnownArea)
		return false;
	float now = gpGlobals->time;
	if (now < m_lsScanNext || IsLookingAtSpot(PRIORITY_LOW))
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
	bool dm = !LsTeamGame();
	const Vector *enemyHome = dm ? nullptr : LsEnemySpawn(m_iTeam);
	bool fresh = now - m_lsScanInAt < 2.0f;
	Vector eye = GetEyePosition();
	float heading = pev->velocity.Length2D() > 60.0f ? UTIL_VecToYaw(pev->velocity) : pev->v_angle.y;
	bool noiseFresh = IsNoiseHeard() && now - m_noiseTimestamp < 5.0f;
	bool sawFresh = GetTimeSinceLastSawEnemy() < 10.0f;

	float bestScore = -1.0f, scores[LS_KNOW_ANGLES];
	bool behind[LS_KNOW_ANGLES] = {};
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
		float score = 1.0f + float(an.weight);
		// (the enemy's side of the map first)
		if (enemyHome)
			score *= (tc - *enemyHome).Length2D() < (pev->origin - *enemyHome).Length2D() ? 1.5f : 0.7f;
		// (near something heard or seen lately)
		if ((noiseFresh && (tc - m_noisePosition).IsLengthLessThan(600.0f)) || (sawFresh && (tc - GetLastKnownEnemyPosition()).IsLengthLessThan(600.0f)))
			score *= 2.5f;
		// (moving: ahead first, nobody walks backwards into somewhere unchecked; behind only
		// once what's ahead is checked, and then just a quick look over the shoulder)
		float off = float(Q_abs(NormalizeAngle(UTIL_VecToYaw(tc - pev->origin) - heading)));
		behind[a] = !holding && off > 100.0f;
		if (behind[a])
			score *= fresh ? 0.03f : (style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER) ? 0.4f : 0.2f;
		// (checked a moment ago)
		for (int r = 0; r < 4; r++)
			if (m_lsScanArea[r] == an.area + 1 && now - m_lsScanAt[r] < 4.0f)
				score *= 0.2f;
		// (a teammate close by already watching it: on the radar, looking that way)
		bool covered = false;
		for (int m = 1; m <= gpGlobals->maxClients && !covered; m++)
		{
			CBasePlayer *mate = UTIL_PlayerByIndex(m);
			if (!mate || mate == this || !mate->IsAlive() || mate->m_iTeam != m_iTeam || (mate->pev->origin - pev->origin).IsLengthGreaterThan(1200.0f))
				continue;
			float toYaw = UTIL_VecToYaw(tc - mate->pev->origin);
			if (Q_abs(NormalizeAngle(toYaw - mate->pev->v_angle.y)) < 25.0f)
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
	if (bestA < 0)
		return false;
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
			for (int r = 0; r < 4; r++)
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
	Vector look = at[bestA];
	LsLookableSpot(&look);
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
	m_lsScanNextSlot = (m_lsScanNextSlot + 1) % 4;
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
		if (!mate || mate == this || !mate->IsAlive() || mate->m_iTeam != m_iTeam)
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
	if (!LsFight() || !LsTeamGame() || !LsExposureReady() || IsAttacking() || IsHurrying() || IsNotMoving() || GetTimeSinceLastSawEnemy() < 3.0f
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
	return (index > 0 && index < int(sizeof(s_buttonPressedAt) / sizeof(s_buttonPressedAt[0]))) ? s_buttonPressedAt[index] : none;
}

Vector BrushCenter(CBaseEntity *pEnt)
{
	return (pEnt->pev->absmin + pEnt->pev->absmax) * 0.5f;
}
} // namespace

void CCSBot::LsDoorButton()
{
	if (!LsFight() || !IsAlive() || IsAttacking() || IsUsingLadder())
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

	// breaking something in the way (a grate, a board, a window on its path)
	CBaseEntity *pBreak = m_lsBreak;
	if (pBreak)
	{
		if (now > m_lsBreakUntil || pBreak->pev->health <= 0.0f || pBreak->pev->solid == SOLID_NOT || GetTimeSinceLastSawEnemy() < 2.0f)
		{
			m_lsBreak = nullptr;
			ResetStuckMonitor();
			return;
		}
		// (a newer player stands there a moment before it clicks, and its shots are sloppier)
		if (now < m_lsBreakStartAt)
			return;
		Vector at = BrushCenter(pBreak);
		SetLookAt("Loner-Strike: breaking it", &at, PRIORITY_HIGH, 0.4f);
		if (IsLookingAtPosition(&at, 6.0f + 14.0f * (1.0f - LsSkill())) && !IsActiveWeaponReloading())
			PrimaryAttack();
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
				m_lsButton = nullptr;
				m_lsButtonBehind = nullptr;
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
	if (now < m_lsButtonCheckAt)
		return;
	m_lsButtonCheckAt = now + 0.3f;
	if (!m_isStuck && !(HasPath() && pev->velocity.Length2D() < 40.0f))
		return;

	// what's just in front (the way it's looking, or the way its path goes)
	Vector eye = GetEyePosition();
	Vector dirs[2];
	int nd = 0;
	UTIL_MakeVectors(pev->v_angle);
	dirs[nd++] = gpGlobals->v_forward;
	if (HasPath() && m_pathIndex >= 0 && m_pathIndex < m_pathLength)
		dirs[nd++] = m_path[m_pathIndex].pos - pev->origin;
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
		if (pHit->pev->takedamage != DAMAGE_NO && !(pHit->pev->spawnflags & SF_BREAK_TRIGGER_ONLY) && pHit->pev->health > 0.0f)
		{
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
	if (pDoor->m_toggle_state != TS_AT_BOTTOM)
		return;
	Vector doorAt = BrushCenter(pHit);
	Vector behind = doorAt + ahead * 64.0f;
	m_lsButtonBehind = TheNavAreaGrid.GetNearestNavArea(&behind);

	// a door opened by pressing use on it
	if (pHit->pev->spawnflags & SF_DOOR_USE_ONLY)
	{
		if (now - ButtonPressed(pHit->entindex()) < 3.0f)
			return;
		m_lsButton = pHit;
		m_lsButtonUntil = now + 4.0f;
		LsWhy("opening the door");
		return;
	}

	// a door opened by something else (it has a name): the button wired to it, close by
	if (FStringNull(pHit->pev->targetname))
	{
		m_lsButtonBehind = nullptr;
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
bool CCSBot::LsPreAim(const Vector &heard, float secs)
{
	if (!m_lastKnownArea)
		return false;
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
bool CCSBot::LsTakeCoverFrom(const Vector &threat, float minMove, bool fromFight)
{
	if (!m_lastKnownArea || IsUsingLadder() || (IsAttacking() && !fromFight))
		return false;
	Vector from = threat + Vector(0, 0, 17.0f);
	float threatD = float((pev->origin - threat).Length());
	CNavArea *queue[48];
	int head = 0, tail = 0;
	queue[tail++] = m_lastKnownArea;
	CNavArea *best = nullptr;
	float bestD = 500.0f;
	while (head < tail)
	{
		CNavArea *area = queue[head++];
		Vector c = *area->GetCenter();
		float d = float((c - pev->origin).Length2D());
		if (d > 500.0f)
			continue;
		if (d >= minMove && d < bestD && (c - threat).Length() > threatD - 100.0f && !LsBadArea(area))
		{
			TraceResult tr;
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
				bool seen = false;
				for (int j = 0; j < tail && !seen; j++)
					seen = queue[j] == adj;
				if (adj && !seen)
					queue[tail++] = adj;
			}
		}
	}
	if (!best)
		return false;
	Vector spot = *best->GetCenter();
	if (fromFight)
	{
		StopAttacking();
		IgnoreEnemies(LsTeamGame() ? 0.8f : 0.5f);
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
	Hide(&spot, hold);
	LsFallBack();
	LsWhy("to cover");
	return true;
}

// What kind of gun someone has out
enum { LSG_NONE, LSG_PISTOL, LSG_DEAGLE, LSG_SMG, LSG_SHOTGUN, LSG_RIFLE, LSG_SNIPER, LSG_MG };
static int LsGunClass(CBasePlayer *pPlayer)
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
bool CCSBot::LsMatchup(CBasePlayer *pEnemy)
{
	float now = gpGlobals->time;
	// hit with low cover in front: down behind it a moment, then back up to shoot (the better
	// ones; on aim maps it's how the whole fight goes: from the box, not out in the open)
	if (m_lsDuckUntil > 0.0f && now >= m_lsDuckUntil)
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
	if (now < m_lsMatchupAt || !IsEnemyVisible() || !pEnemy->IsAlive())
		return false;
	m_lsMatchupAt = now + 0.5f;
	float skill = LsSkill();
	float range = float((pEnemy->pev->origin - pev->origin).Length());

	if (pEnemy->IsBlind() && range < 1200.0f && !IsUsingSniperRifle() && now > m_lsPushUntil && RANDOM_FLOAT(0.0f, 1.0f) < 0.3f + 0.6f * skill)
	{
		m_lsPushUntil = now + RANDOM_FLOAT(1.0f, 1.8f);
		LsWhy("pushing a flashed one");
	}
	// (most of the time, more often the better it is: not every reload like a script; decided
	// once per reload)
	if (IsReloading() && skill > 0.4f && now > m_lsReloadCrouchAt)
	{
		m_lsReloadCrouchAt = now + 3.0f;
		if (RANDOM_FLOAT(0.0f, 1.0f) < 0.45f + 0.5f * skill && LsLowCoverToward(pEnemy->pev->origin))
			Crouch();
	}

	CBasePlayer *pDone = m_lsMatchupEnemy;
	if (pDone == pEnemy)
		return false;
	m_lsMatchupEnemy = pEnemy;
	int mine = LsGunClass(this), theirs = LsGunClass(pEnemy);
	bool bad = false;
	if (theirs == LSG_SNIPER && mine != LSG_SNIPER && range > 900.0f && IsPlayerLookingAtMe(pEnemy))
		bad = true;
	bool farGun = theirs == LSG_RIFLE || theirs == LSG_SNIPER || theirs == LSG_MG;
	if (farGun && ((mine == LSG_SMG && range > 1300.0f) || (mine == LSG_SHOTGUN && range > 800.0f) || (mine == LSG_PISTOL && range > 1100.0f)))
		bad = true;
	if (!bad || pEnemy->pev->health < 45.0f || skill < 0.35f)
		return false;
	if (m_state == &m_plantBombState || m_state == &m_defuseBombState || IsEscapingFromBomb())
		return false;
	float odds = 0.3f + 0.6f * skill;
	if (!LsTeamGame())
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
	if (here)
		LsForceBadArea(here, 6.0f + 8.0f * LsSkill());
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
		return;
	TraceResult tr;
	UTIL_TraceLine(pAttacker->pev->origin + pAttacker->pev->view_ofs, GetEyePosition(), ignore_monsters, ignore_glass, ENT(pAttacker->pev), &tr);
	if (tr.flFraction >= 1.0f)
		return;
	if (RANDOM_FLOAT(0.0f, 1.0f) > 0.25f + 0.7f * LsSkill())
		return;
	if (m_lastKnownArea)
		LsForceBadArea(m_lastKnownArea, 8.0f + 7.0f * LsSkill());
	if (LsTakeCoverFrom(pAttacker->pev->origin, 150.0f))
		LsWhy("wallbanged: moving");
}

// Mid-fight, turning to someone heard running at it (the aim leaves its enemy a moment): seen
// now, it's the one fought
bool CCSBot::LsRunnerGlance()
{
	if (!LsFight() || gpGlobals->time >= m_lsRunnerUntil)
		return false;
	CBasePlayer *pRunner = m_lsRunner;
	if (!pRunner || !pRunner->IsAlive() || IsBlind())
	{
		m_lsRunnerUntil = 0.0f;
		return false;
	}
	if (IsVisible(pRunner, false))
	{
		m_lsRunnerUntil = 0.0f;
		Attack(pRunner);
		return false;
	}
	m_lsRunnerSpot = pRunner->pev->origin + Vector(0, 0, HalfHumanHeight);
	Vector ang = UTIL_VecToAngles(m_lsRunnerSpot - GetEyePosition());
	ang.x = 360.0f - ang.x;
	SetLookAngles(ang.y, ang.x);
	return true;
}
