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
	return !IsAttacking() && !IsBusy() && !IsCarryingBomb() && !m_lsSaving && !LsPuppetActive() && !IsDefusingBomb()
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
	return cv_bot_deathmatch.value > 0.0f || (CSGameRules() && CSGameRules()->IsFreeForAll());
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
		LsBareName(STRING(pev->netname), bare, sizeof(bare));
		unsigned int h = 2166136261U;
		for (const char *c = bare; *c; c++)
			h = (h ^ (unsigned char)*c) * 16777619U;
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
	if (Q_strcmp(m_lsMap, STRING(gpGlobals->mapname)) != 0)
	{
		Q_strlcpy(m_lsMap, STRING(gpGlobals->mapname));
		for (auto &d : m_lsDanger)
			d.amount = 0.0f;
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

bool LsOwnMemory()
{
	return cv_bot_ls_own_memory.value > 0.0f;
}

// A teammate died where this bot could see it
void CCSBot::LsSawTeammateDie(CBasePlayer *pVictim)
{
	if (!pVictim)
		return;

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
	unsigned int bucket = LsSolo() ? (unsigned int)(gpGlobals->time / drift) : (unsigned int)CSGameRules()->m_iTotalRoundsPlayed * 7919u + 13u;
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

	// the bomb is down: no time for that
	if (TheCSBots()->IsBombPlanted())
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
	LsPuppet &p = m_lsPuppet;
	if (!p.any)
		return;

	// a fight, or anything that matters, comes first
	// (and nothing holds it on a ladder or blinded)
	if (IsAttacking() || m_isEnemyVisible || LsBusy() || !LsPuppetActive() || IsOnLadder() || IsBlind())
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
		SetLookAt("Loner-Strike", &at, PRIORITY_HIGH, 0.3f, false, 2.0f);
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
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(Q_atoi(CMD_ARGV(1)));
		if (pPlayer && pPlayer->IsBot())
		{
			// (a missing value stays "not told": -1, so the profile's is used)
			float aggro = LsArg(3, -1.0f), team = LsArg(4, -1.0f);
			static_cast<CCSBot *>(pPlayer)->LsSetPersona(aggro < 0.0f ? -1.0f : Q_min(1.0f, aggro), team < 0.0f ? -1.0f : Q_min(1.0f, team),
				int(LsArg(5, -1.0f)), int(LsArg(6, 1.0f)), int(LsArg(7, 1.0f)), int(LsArg(8, -1.0f)));
		}
		return;
	}

	CCSBot *me = LsBotByIndex(CMD_ARGV(1));
	if (!me)
		return;

	const char *what = CMD_ARGV(2);
	float now = gpGlobals->time;

	// not while it's in a fight or busy with something that matters, nor while something
	// has startled it
	if (!FStrEq(what, "release") && (me->IsAttacking() || me->IsEnemyVisible() || me->LsBusy() || (LsFight() && !me->LsCalm())))
		return;

	me->LsPuppetCommand(what, now);
}

void CCSBot::LsPuppetCommand(const char *what, float now)
{
	LsPuppet &p = m_lsPuppet;
	const float most = 30.0f;

	// release [tag]: with a tag, only that plugin's hold goes (one plugin's habit doesn't
	// cancel another's)
	if (FStrEq(what, "release"))
	{
		const char *tag = CMD_ARGC() > 3 ? CMD_ARGV(3) : "";
		if (tag[0] && p.holdTag[0] && !FStrEq(tag, p.holdTag))
			return;
		LsPuppetRelease();
		return;
	}

	// hold <secs> [tag]: the longest hold asked for stands
	if (FStrEq(what, "hold"))
	{
		float until = now + Q_min(LsArg(3, 1.0f), most);
		if (until > p.holdUntil)
		{
			p.holdUntil = until;
			Q_strlcpy(p.holdTag, CMD_ARGC() > 4 ? CMD_ARGV(4) : "");
		}
	}
	else if (FStrEq(what, "crouch"))
	{
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
		p.lookUntil = now + Q_min(LsArg(6, 1.0f), most);
		p.nodEvery = FStrEq(what, "nod") ? Q_max(0.08f, LsArg(7, 0.35f)) : 0.0f;
		// (look ... 1: a quick look, a flick or a spin; a nod is always quick)
		p.fast = FStrEq(what, "nod") || LsArg(7) > 0.0f;
	}
	else if (FStrEq(what, "goto"))
	{
		Vector to(LsArg(3), LsArg(4), LsArg(5));
		MoveTo(&to, SAFEST_ROUTE);
		if (LsArg(6) > 0.0f)
			p.walkUntil = now + most;
	}
	else if (FStrEq(what, "follow"))
	{
		CBasePlayer *pLeader = UTIL_PlayerByIndex(Q_atoi(CMD_ARGV(3)));
		if (!pLeader || !pLeader->IsAlive() || pLeader == this || BotRelationship(pLeader) != BOT_TEAMMATE)
			return;
		Follow(pLeader);
		p.followUntil = now + Q_min(LsArg(4, 20.0f), 60.0f);
	}
	else
	{
		return;
	}

	p.any = true;
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
			// (Ts work toward the bomb sites, by their own ways, rather than into dead ends)
			for (int z = 0; z < TheCSBots()->GetZoneCount() && kept < 16; z++)
				keep[kept++] = TheCSBots()->GetZone(z)->m_center;
		}
		else if (m_iTeam == TERRORIST && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_RESCUE_HOSTAGES)
		{
			CBaseEntity *pHostage = nullptr;
			while (kept < 16 && (pHostage = UTIL_FindEntityByClassname(pHostage, "hostage_entity")))
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

	const int tries = 12;
	int picks[tries];
	for (int t = 0; t < tries; t++)
		picks[t] = RANDOM_LONG(0, count - 1);

	CNavArea *best = nullptr;
	float bestScore = -1.0f;
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
			if (fromMe < 600.0f)
				score -= 1500.0f;

			float nearest = 3000.0f;
			for (int k = 0; k < n; k++)
				nearest = Q_min(nearest, float((*c - avoid[k]).Length2D()));
			score += nearest;

			// a deathmatch with hardly anyone left: toward where an enemy was last seen
			if (LsSolo() && GetEnemiesRemaining() <= 2 && GetTimeSinceLastSawEnemy() < 30.0f)
				score -= 0.5f * float((*c - GetLastKnownEnemyPosition()).Length2D());

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
				score += site < 1400.0f ? 400.0f - 220.0f * float(Q_min(crowd[at], 4)) : -1500.0f;
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
		if ((pPlayer->pev->origin - pev->origin).IsLengthLessThan(320.0f))
			crowded = true;
	});
	if (!crowded || RANDOM_FLOAT(0.0f, 1.0f) > 0.7f)
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
	return tr.flFraction < 1.0f;
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
		&& CSGameRules() && CSGameRules()->GetRoundRemainingTime() > 40.0f;
	bool loud = IsHurrying() || GetTimeSinceAttacked() < 5.0f || GetTimeSinceLastSawEnemy() < 6.0f || IsSafe()
		|| (TheCSBots() && TheCSBots()->IsBombPlanted()) || (IsCarryingBomb() && !sneakyCarrier) || GetHostageEscortCount() > 0;

	bool want = false;
	if (!loud && sneaky > 0.45f)
	{
		float danger = (LsOwnMemory() && GetLastKnownArea()) ? LsGetDanger(GetLastKnownArea()) : 0.0f;
		bool lateRound = TheCSBots() && TheCSBots()->GetElapsedRoundTime() > 25.0f;
		int round = CSGameRules() ? CSGameRules()->m_iTotalRoundsPlayed : 0;
		if (m_lsSneakRound != round)
		{
			m_lsSneakRound = round;
			m_lsSneakLate = RANDOM_FLOAT(0.0f, 1.0f) < sneaky;
		}
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
// noise x,y,why,seconds since why)
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
	fprintf(s_file, "%.2f,%d,%d,%s,%d,%d,%d,%.0f,%.0f,%.0f,%.0f,%s,%.1f\n",
		gpGlobals->time, entindex(), m_iTeam, m_state ? m_state->GetName() : "-", int(GetTask()),
		pEnemy ? pEnemy->entindex() : 0, pLeader ? pLeader->entindex() : 0,
		goal.x, goal.y, noise ? noise->x : 0.0f, noise ? noise->y : 0.0f,
		m_lsWhy, m_lsWhy[0] ? gpGlobals->time - m_lsWhyAt : 0.0f);
	fflush(s_file);
}

// An enemy's bullet landed close by, or passed close by on its way to the wall behind
// (EVENT_BULLET_IMPACT): people notice being shot at. One it can see is fought; otherwise
// it turns towards where the shots came from and treats it as a loud noise there.
void CCSBot::LsShotAt(CBasePlayer *pShooter, const Vector &impact)
{
	if (!IsAlive() || IsBlind() || !pShooter->IsPlayer() || pShooter == this || !pShooter->IsAlive() || !IsEnemy(pShooter))
		return;
	if (IsAttacking() && IsEnemyVisible())
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
	if (!IsAlive() || IsBlind() || !pWeapon || IsActiveWeaponClipEmpty() || !LsBangGun(pWeapon) || IsFriendInLineOfFire())
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
	TraceResult in, out, mates;
	UTIL_TraceLine(eye, target, ignore_monsters, ENT(pev), &in);
	if (in.flFraction >= 1.0f || in.fStartSolid)
		return false;
	UTIL_TraceLine(target, eye, ignore_monsters, ENT(pev), &out);
	if (out.fStartSolid || out.flFraction >= 1.0f)
		return false;
	if ((in.vecEndPos - out.vecEndPos).IsLengthGreaterThan(pWeapon->m_iId == WEAPON_AWP ? 40.0f : 22.0f))
		return false;
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
	if (LsTrait(32) > 0.2f + 0.4f * skill || RANDOM_FLOAT(0.0f, 1.0f) > 0.6f)
		return false;
	return LsBangThrough(GetLastKnownEnemyPosition() + Vector(0, 0, 10.0f));
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
	// (a plugin's hold or look gives way to an enemy heard close by)
	if (LsPuppetActive() && dist < 700.0f)
		LsPuppetRelease();
	bool busy = m_state == &m_plantBombState || m_state == &m_defuseBombState || IsUsingLadder() || m_lsSaving || LsPuppetActive() || IsBlind();

	// in a fight with them: their reload is the moment to push
	if (IsAttacking())
	{
		if (event == EVENT_WEAPON_RELOADED && GetEnemy() == pPlayer && !LsMatch() && aggro > 0.5f && RANDOM_FLOAT(0.0f, 1.0f) < aggro)
			m_lsPushUntil = now + RANDOM_FLOAT(1.0f, 2.0f);
		return;
	}
	if (busy)
		return;
	if (event != EVENT_WEAPON_RELOADED && event != EVENT_PLAYER_FOOTSTEP && event != EVENT_WEAPON_FIRED)
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
		Vector glance = pev->origin + pev->view_ofs + Vector(0, 0, dz > 0.0f ? 80.0f : -80.0f);
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
	if (dist < 650.0f && skill > 0.65f && LsTrait(33) < skill - 0.35f && now - m_lsBangHeardAt > 6.0f && RANDOM_FLOAT(0.0f, 1.0f) < 0.35f)
	{
		m_lsBangHeardAt = now;
		if (LsBangThrough(heard + Vector(0, 0, 10.0f)))
			return;
	}

	// pre-aim where they'd come out
	if ((event == EVENT_PLAYER_FOOTSTEP || event == EVENT_WEAPON_RELOADED) && dist < 700.0f && now - m_lsPreAimAt > 1.5f && RANDOM_FLOAT(0.0f, 1.0f) < 0.4f + 0.5f * skill)
	{
		m_lsPreAimAt = now;
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
