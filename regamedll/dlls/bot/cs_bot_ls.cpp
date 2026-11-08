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
*   bot_ls_solo        1  in deathmatch-type modes (bot_deathmatch 1: deathmatch, gun game)
*                         every bot plays for itself: it heads somewhere its teammates
*                         aren't, leaves a fight a closer teammate is already going to,
*                         breaks off when it finds itself walking with one, and doesn't
*                         follow anyone on the radio. Objective maps keep their teamwork
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

void Bot_LsRegisterCVars()
{
	CVAR_REGISTER(&cv_bot_ls_own_memory);
	CVAR_REGISTER(&cv_bot_ls_routes);
	CVAR_REGISTER(&cv_bot_ls_lines);
	CVAR_REGISTER(&cv_bot_ls_pauses);
	CVAR_REGISTER(&cv_bot_ls_solo);
}

// Every bot for itself (deathmatch-type modes, bot_ls_solo)
bool LsSolo()
{
	return cv_bot_ls_solo.value > 0.0f && TheCSBots() && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEATHMATCH;
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
unsigned int CCSBot::LsSeed()
{
	if (!(pev->netname == m_lsSeedName))
	{
		m_lsSeedName = pev->netname;
		unsigned int h = 2166136261U;
		for (const char *c = STRING(pev->netname); c && *c; c++)
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
	if (!LsOwnMemory() || !pVictim)
		return;

	// see it happen, not only hear about it (a little less than dying there yourself)
	if (!IsVisible(&pVictim->pev->origin, CHECK_FOV))
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

	// mostly its own lasting taste, with a part that changes every couple of minutes
	const float drift = 120.0f;
	unsigned int bucket = (unsigned int)(gpGlobals->time / drift);
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
	float taste = 0.7f * LsNoise(r.seed, c->x, c->y, 700.0f) + 0.3f * LsNoise(r.driftSeed, c->x, c->y, 500.0f);
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
	float strength = cv_bot_ls_pauses.value;
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

	// only when nothing is going on
	if (IsAttacking() || GetTimeSinceLastSawEnemy() < 8.0f || IsOnLadder() || IsCarryingBomb() || LsBusy())
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
	if (IsAttacking() || m_isEnemyVisible || LsBusy() || !LsPuppetActive())
	{
		LsPuppetRelease();
		return;
	}

	float now = gpGlobals->time;

	if (now < p.holdUntil)
	{
		ClearMovement();
		ResetStuckMonitor();
	}

	if (now < p.crouchUntil)
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
	CCSBot *me = LsBotByIndex(CMD_ARGV(1));
	if (!me)
		return;

	const char *what = CMD_ARGV(2);
	float now = gpGlobals->time;

	// not while it's in a fight or busy with something that matters
	if (!FStrEq(what, "release") && (me->IsAttacking() || me->IsEnemyVisible() || me->LsBusy()))
		return;

	me->LsPuppetCommand(what, now);
}

void CCSBot::LsPuppetCommand(const char *what, float now)
{
	LsPuppet &p = m_lsPuppet;
	const float most = 30.0f;

	if (FStrEq(what, "release"))
	{
		LsPuppetRelease();
		return;
	}

	if (FStrEq(what, "hold"))
	{
		p.holdUntil = now + Q_min(LsArg(3, 1.0f), most);
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
		p.nodEvery = FStrEq(what, "nod") ? Q_max(0.15f, LsArg(7, 0.35f)) : 0.0f;
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

// Living teammates, other than this bot
template <typename F>
static void LsForTeammates(CCSBot *me, F f)
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!pPlayer || pPlayer == me || !pPlayer->IsAlive() || pPlayer->m_iTeam != me->m_iTeam)
			continue;
		f(pPlayer);
	}
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
		if (n < 64)
			avoid[n++] = pPlayer->pev->origin;
		if (n < 64 && pPlayer->IsBot())
		{
			CCSBot *pBot = static_cast<CCSBot *>(pPlayer);
			if (pBot->HasPath())
				avoid[n++] = pBot->GetPathEndpoint();
		}
	});

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

			const Vector *c = area->GetCenter();
			float score = RANDOM_FLOAT(0.0f, 500.0f);

			// somewhere to go, not where it is
			float fromMe = (*c - pev->origin).Length2D();
			if (fromMe < 600.0f)
				score -= 1500.0f;

			float nearest = 3000.0f;
			for (int k = 0; k < n; k++)
				nearest = Q_min(nearest, (*c - avoid[k]).Length2D());
			score += nearest;

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
