/*
*
*   Loner-Strike additions to the CZ bots: game modes. A mode's components (gm_components, the
*   mode kit) tell the bots what the mode is about through ls_mode server commands: goals (hold
*   here, capture that, loot this crate), zones (keep out of the fire, stay in the safe zone),
*   ties between players (a squad, a traitor) and enemies that aren't players (creatures).
*   The bots act on them with what they already have: paths, hiding, senses, their persona.
*
*   Nothing here does anything until a mode sends something: no goals, no zones, no ties, no
*   enemy classes means the brain plays exactly as it does without this file.
*
*   ls_mode reset [all]                                    forget goals and zones (all: roles, ties, enemy classes too)
*   ls_mode set <id>                                       the mode's id (off: reset all)
*   ls_mode goal <who> <kind> <x> <y> <z> [ent] [prio] [until] [radius]
*   ls_mode goal_clear <who> [kind | ent]
*   ls_mode avoid <tag> <x> <y> <z> <r> <cost> [until]     cost 0-10 more to walk through, -1 never
*   ls_mode attract <tag> <x> <y> <z> <r> <weight> [until] [team]
*   ls_mode forget <tag>                                   an avoid or attract gone
*   ls_mode block <ent> <until>                            a closed door: no path through it till then (0: open)
*   ls_mode zone <cx> <cy> <cz> <r> [<ncx> <ncy> <ncz> <nr> <shrink_at>]
*   ls_mode zone_clear
*   ls_mode role <who> <role | ->
*   ls_mode friend <who> <who2> [0 | 1]                    who treats who2 as a teammate
*   ls_mode foe <who> <who2> [0 | 1]                       who treats who2 as an enemy
*   ls_mode enemyclass <classname> [threat]                entities of that class get shot (threat 0: not)
*   ls_mode enemyclass_clear
*   ls_mode status
*
*   who: a player index, all, t, ct or @role. until: seconds from now, 0 till cleared.
*   prio: 0-100; 80 and up every bot does it, 30-79 most do, below 30 a hint some take up.
*
*/

#include "precompiled.h"

cvar_t cv_ls_mode_ver = { "ls_mode_ver", "2", 0, 2.0f, nullptr };	// the plugins look for this before sending ls_mode

bool g_lsModeTies = false;	// any friend / foe set (BotRelationship checks it, bot.h)

// ---- what the mode has said (shared by every bot) -------------------------------------------

const int LS_MODE_ZONES = 32;		// avoids (bits in a path's "inside" mask: 32 at most)
const int LS_MODE_ATTRACTS = 192;	// attracts: a Battle Royale map's every crate
const int LS_MODE_CLASSES = 8;
const int LS_MODE_TARGETS = 64;

// an avoid, attract or blocked door: a circle on the ground (a box for a door)
struct LsModeZone
{
	bool used = false;
	int layer = -1;						// attract: below / surface / high (LsModeLayer), -1 not worked out yet
	char tag[24] = "";
	Vector at;
	float r = 0.0f;
	float value = 0.0f;					// avoid: the cost (-1 never); attract: the weight
	float until = 0.0f;					// 0 till forgotten
	int team = 0;						// attract: 0 everyone, 1 T, 2 CT
	bool box = false;					// a door's bounds instead of the circle
	Vector lo, hi;
};

// the safe zone, and the next one it shrinks to
struct LsModeRing
{
	bool on = false;
	Vector at;
	float r = 0.0f;
	bool next = false;
	Vector nextAt;
	float nextR = 0.0f;
	float shrinkAt = 0.0f;
};

struct LsModeClass
{
	char name[32];
	float threat;
};

static float s_lsNavMidZ = -99999.0f;	// the map's usual floor height (the nav's median), for layers
static bool s_lsModeOn = false;			// anything at all for the bots to act on (goals, zones, classes)
static bool s_lsNoBuy = false;			// nothing to buy (ls_mode nobuy 1): no time in the buy state
static bool s_lsPeace = false;			// a peace phase (ls_mode phase loot): no player is an enemy
static float s_lsPeaceEndAt = -100.0f;	// ...it ended then (each bot notices the fight is on a moment later)
static char s_lsModeId[32] = "";
static LsModeZone s_lsAvoid[LS_MODE_ZONES];		// avoids and blocked doors
static LsModeZone s_lsAttract[LS_MODE_ATTRACTS];
static LsModeRing s_lsRing;
static char s_lsRole[MAX_CLIENTS + 1][16];
static int s_lsLayer[MAX_CLIENTS + 1];
static float s_lsLayerInfl[MAX_CLIENTS + 1];	// ...its personality influence, plus 1 (0: not given)
static float s_lsPhaseInfl = 0.0f;				// the phase's personality influence, plus 1 (0: not given)
static int s_lsHeals[MAX_CLIENTS + 1];			// healing items each player carries (ls_mode heals), -1 unknown
static bool s_lsDown[MAX_CLIENTS + 1];			// knocked down (ls_mode down <who> 1|0)

// Knocked down by the mode: the mode says so (ls_mode down), or the old sign - no-target with the view
// a hand above the floor (with ls_down_finish the mode no longer hides them, so only the hint tells)
static bool LsModeIsDown(CBaseEntity *pPlayer)
{
	int i = pPlayer ? pPlayer->entindex() : 0;
	if (i >= 1 && i <= MAX_CLIENTS && s_lsDown[i])
		return pPlayer->IsAlive();
	// (no hint: no-target with the view low, or - not hidden, with ls_down_finish - the view low and
	// held to a crawl: crouching keeps its speed)
	if (!pPlayer)
		return false;
	// (the mode's red glow on the downed - gm_components' go_down: a glow shell 255 40 40)
	if (pPlayer->pev->renderfx == kRenderFxGlowShell && pPlayer->pev->rendercolor.x > 200.0f && pPlayer->pev->rendercolor.y < 80.0f
		&& pPlayer->pev->rendercolor.z < 80.0f && pPlayer->IsAlive())
		return true;
	return pPlayer->pev->view_ofs.z < 20.0f
		&& ((pPlayer->pev->flags & FL_NOTARGET) || (pPlayer->IsAlive() && pPlayer->pev->maxspeed > 2.0f && pPlayer->pev->maxspeed <= 60.0f));
}

// How far a bot's own personality and skill count against what the mode asked (an order's
// "influence", 0-100 as 0-1): 0 the order exactly as given, 1 the usual mix - an order sent
// without one - and 2 its personality and skill fully (each decision's personal part doubled)
static float LsModeSway(float infl)
{
	return infl < 0.0f ? 1.0f : 2.0f * Q_min(1.0f, infl);
}

// An influence argument (0-100) as 0-1, -1 when it isn't there
static float LsModeInflArg(int i)
{
	if (CMD_ARGC() <= i || !CMD_ARGV(i)[0])
		return -1.0f;
	float v = Q_atof(CMD_ARGV(i));
	return v < 0.0f ? -1.0f : Q_min(1.0f, v / 100.0f);
}
// what a mode offers each player to use (ls_mode offer): perk machines, the upgrade bench, the mystery
// box, a trap switch, a turret to place; the bot decides when to take one
enum { LS_OFFER_NONE, LS_OFFER_PERK, LS_OFFER_BENCH, LS_OFFER_BOX, LS_OFFER_TRAP, LS_OFFER_TURRET };
struct LsModeOffer
{
	int kind = LS_OFFER_NONE;
	Vector at;
	int ent = 0;
	int cost = 0;
	int order = 0;						// perks: the order to buy them in (lowest first)
	float nextTry = 0.0f;				// not before then (taken lately, or a box roll lost)
	float infl = -1.0f;					// personality influence 0-1 (-1 not given)
};
const int LS_MODE_OFFERS = 8;
static LsModeOffer s_lsOffer[MAX_CLIENTS + 1][LS_MODE_OFFERS];			// the mode's layer for each player (ls_mode layer): 0 surface, 1 below, 2 high, -1 none
static unsigned int s_lsFriend[MAX_CLIENTS + 1];	// bit j-1 set: player i treats player j as a teammate
static unsigned int s_lsFoe[MAX_CLIENTS + 1];		// ...as an enemy
static LsModeClass s_lsClass[LS_MODE_CLASSES];
static int s_lsClassCount = 0;
// the mode enemies alive now, looked up a few times a second for every bot at once
static EntityHandle<CBaseEntity> s_lsTarget[LS_MODE_TARGETS];
static float s_lsTargetThreat[LS_MODE_TARGETS];
static int s_lsTargetCount = 0;
static float s_lsTargetsAt = 0.0f;

static const char *const s_lsGoalNames[LS_GOAL_KINDS] = { "", "go_hold", "defend", "capture", "extract", "loot", "use",
	"ride", "investigate", "flee", "meeting", "offer", "hunt" };

// what counts as there, by kind, when the mode doesn't say
static const float s_lsGoalRadius[LS_GOAL_KINDS] = { 0.0f, 250.0f, 300.0f, 150.0f, 120.0f, 64.0f, 64.0f,
	150.0f, 150.0f, 800.0f, 150.0f, 64.0f, 150.0f };

static bool LsModeLive(const LsModeZone &z)
{
	return z.used && (z.until <= 0.0f || gpGlobals->time < z.until);
}

static int LsModeGoalKind(const char *name)
{
	if (FStrEq(name, "hold"))
		return LS_GOAL_HOLD;
	for (int k = 1; k < LS_GOAL_KINDS; k++)
		if (FStrEq(name, s_lsGoalNames[k]))
			return k;
	return LS_GOAL_NONE;
}

// Seconds from now as the game's time (0 stays 0: no end)
static float LsModeUntil(float secs)
{
	return secs > 0.0f ? gpGlobals->time + secs : 0.0f;
}

// A number from the command; nan, inf or something off any map marks the command bad (it's
// dropped: see LsModeBadArgs)
static bool s_lsArgBad = false;
static float LsModeArg(int i, float fallback = 0.0f)
{
	if (CMD_ARGC() <= i)
		return fallback;
	float v = Q_atof(CMD_ARGV(i));
	if (v != v || v > 1.0e6f || v < -1.0e6f)
	{
		s_lsArgBad = true;
		return fallback;
	}
	return v;
}

static bool LsModeBadArgs()
{
	if (s_lsArgBad)
		CONSOLE_ECHO("ls_mode %s: a number that isn't one (nan, inf or too big): ignored\n", CMD_ARGV(1));
	return s_lsArgBad;
}

// A radius kept sane: at least lo, at most a map's width
static float LsModeRadius(float r, float lo)
{
	return Q_max(lo, Q_min(r, 16384.0f));
}

static float LsModeDist2D(const Vector &a, const Vector &b)
{
	return (a - b).Make2D().Length();
}

// The way from one spot to another on the ground (any way, if they're the same spot)
static Vector LsModeDir2D(const Vector &from, const Vector &to)
{
	Vector d(to.x - from.x, to.y - from.y, 0.0f);
	float len = d.Length();
	return len > 1.0f ? d / len : Vector(1.0f, 0.0f, 0.0f);
}

// A spot inside a zone: in the circle (and not a floor above or below it), or the door's box
static bool LsModeInZone(const LsModeZone &z, const Vector &pos)
{
	if (z.box)
		return pos.x >= z.lo.x - 16.0f && pos.x <= z.hi.x + 16.0f && pos.y >= z.lo.y - 16.0f && pos.y <= z.hi.y + 16.0f
			&& pos.z >= z.lo.z - 72.0f && pos.z <= z.hi.z + 72.0f;
	float tall = Q_max(z.r, 128.0f);
	return LsModeDist2D(pos, z.at) < z.r && pos.z > z.at.z - tall && pos.z < z.at.z + tall;
}

// A nav area touching a zone (its nearest point, not only its centre: a big area half in the
// fire counts)
static bool LsModeAreaInZone(const LsModeZone &z, const CNavArea *area)
{
	const Extent *ext = area->GetExtent();
	const Vector *c = area->GetCenter();
	if (z.box)
		return ext->hi.x >= z.lo.x - 16.0f && ext->lo.x <= z.hi.x + 16.0f && ext->hi.y >= z.lo.y - 16.0f && ext->lo.y <= z.hi.y + 16.0f
			&& c->z >= z.lo.z - 72.0f && c->z <= z.hi.z + 72.0f;
	Vector nearest(Q_max(ext->lo.x, Q_min(z.at.x, ext->hi.x)), Q_max(ext->lo.y, Q_min(z.at.y, ext->hi.y)), c->z);
	return LsModeInZone(z, nearest);
}

static LsModeZone *LsModeFindTag(LsModeZone *list, const char *tag, bool make)
{
	LsModeZone *spare = nullptr;
	int count = list == s_lsAttract ? LS_MODE_ATTRACTS : LS_MODE_ZONES;
	for (int i = 0; i < count; i++)
	{
		if (list[i].used && FStrEq(list[i].tag, tag))
			return &list[i];
		if (!spare && !LsModeLive(list[i]))
			spare = &list[i];
	}
	if (!make)
		return nullptr;
	if (!spare)
	{
		CONSOLE_ECHO("ls_mode: no room for '%s' (%d at most)\n", tag, count);
		return nullptr;
	}
	*spare = LsModeZone();
	spare->used = true;
	Q_strlcpy(spare->tag, tag);
	return spare;
}

// The players a "who" names: an index, all, t, ct, or @role (bots only, if asked)
static int LsModeWho(const char *who, int *out, bool botsOnly)
{
	int n = 0;
	if (!who || !*who)
		return 0;
	bool all = FStrEq(who, "all"), t = FStrEq(who, "t"), ct = FStrEq(who, "ct");
	const char *role = who[0] == '@' ? who + 1 : nullptr;
	if (role && !*role)
	{
		CONSOLE_ECHO("ls_mode: '@' needs a role name (@zombie)\n");
		return 0;
	}
	int only = (all || t || ct || role) ? 0 : Q_atoi(who);
	if (!all && !t && !ct && !role && (only < 1 || only > gpGlobals->maxClients))
		return 0;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		if (only && i != only)
			continue;
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!UTIL_IsValidPlayer(pPlayer) || (botsOnly && !pPlayer->IsBot()))
			continue;
		if ((t && pPlayer->m_iTeam != TERRORIST) || (ct && pPlayer->m_iTeam != CT) || (role && !FStrEq(s_lsRole[i], role)))
			continue;
		out[n++] = i;
	}
	return n;
}

static void LsModeClearBotGoals(int kind, int ent)
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (UTIL_IsValidPlayer(pPlayer) && pPlayer->IsBot())
			static_cast<CCSBot *>(pPlayer)->LsModeClearGoals(kind, ent);
	}
}

// Forget what the components said (goals, zones); all: what the mode said too (roles, ties,
// enemy classes). (bots: their goals as well - not at a map start, when the players from the
// last map are gone and the new ones start with none)
static void LsModeReset(bool all, bool bots = true)
{
	for (int i = 0; i < LS_MODE_ZONES; i++)
		s_lsAvoid[i] = LsModeZone();
	for (int i = 0; i < LS_MODE_ATTRACTS; i++)
		s_lsAttract[i] = LsModeZone();
	s_lsNavMidZ = -99999.0f;
	s_lsRing = LsModeRing();
	s_lsPeace = false;
	s_lsPeaceEndAt = -100.0f;
	s_lsPhaseInfl = 0.0f;
	s_lsNoBuy = false;
	if (bots)
		LsModeClearBotGoals(0, 0);
	if (all)
	{
		s_lsModeId[0] = '\0';
		Q_memset(s_lsRole, 0, sizeof(s_lsRole));
		for (int i = 0; i <= MAX_CLIENTS; i++)
		{
			s_lsLayer[i] = -1;
			s_lsLayerInfl[i] = 0.0f;
			s_lsHeals[i] = -1;
			s_lsDown[i] = false;
			for (int k = 0; k < LS_MODE_OFFERS; k++)
				s_lsOffer[i][k] = LsModeOffer();
		}
		Q_memset(s_lsFriend, 0, sizeof(s_lsFriend));
		Q_memset(s_lsFoe, 0, sizeof(s_lsFoe));
		g_lsModeTies = false;
		s_lsClassCount = 0;
		s_lsTargetCount = 0;
		for (int i = 0; i < LS_MODE_TARGETS; i++)
			s_lsTarget[i] = nullptr;
	}
	s_lsModeOn = s_lsClassCount > 0;
}

void LsModeMapStart()
{
	LsModeReset(true, false);
	s_lsTargetsAt = 0.0f;
}

bool LsModeNoBuy()
{
	return s_lsNoBuy;
}

// ---- a peace phase (ls_mode phase loot) ----------------------------------------------------

static Vector LsModeOnMesh(const Vector &pos);

// Nobody's an enemy now: the mode's peace (a loot-up where nobody can be hurt), or the moment
// after it ends before this bot takes in that the fight is on (0.4-1.4 s: its own reaction, a
// newer player slower) - not everyone knifing whoever is next to them on the same frame
bool CCSBot::LsModePeace()
{
	if (s_lsPeace)
		return true;
	if (s_lsPeaceEndAt < 0.0f)
		return false;
	float since = gpGlobals->time - s_lsPeaceEndAt;
	if (since < 0.0f || since > 3.0f)
		return false;
	// (each its own moment: the slower, the later - none at all with an influence of 0)
	return since < LsModeSway(s_lsPhaseInfl - 1.0f) * (0.4f + 0.6f * LsTrait(205) + 0.4f * (1.0f - LsSkill()));
}

// Kitted out enough to go hunting: some armor, a pistol beside the main gun, and a spare
// magazine for it
bool CCSBot::LsModeKitted()
{
	CBasePlayerWeapon *pGun = static_cast<CBasePlayerWeapon *>(m_rgpPlayerItems[PRIMARY_WEAPON_SLOT]);
	if (!pGun || pev->armorvalue < 30.0f || !m_rgpPlayerItems[PISTOL_SLOT])
		return false;
	int spare = (pGun->m_iPrimaryAmmoType > 0 && pGun->m_iPrimaryAmmoType < MAX_AMMO_SLOTS) ? m_rgAmmo[pGun->m_iPrimaryAmmoType] : 0;
	return spare >= Q_max(pGun->iMaxClip(), 1);
}

// Just after the peace (a loot-up) ends, a while of gearing up first, as people do. Without a
// main gun: 40-80 s, each bot its own, fighting only what comes for it. With one: looting on till
// kitted out (90 s at most), but anyone in sight close by - landed together - is a fight now (a hot
// drop). Over for anyone once it's hit
bool CCSBot::LsModeGearing()
{
	LS_PROF("LsModeGearing");
	if (s_lsPeace || s_lsPeaceEndAt < 0.0f)
		return false;
	// (a new peace's end: a new gearing up)
	if (m_lsModeGearFor != s_lsPeaceEndAt)
	{
		m_lsModeGearFor = s_lsPeaceEndAt;
		m_lsModeGearDone = false;
	}
	if (m_lsModeGearDone)
		return false;
	float since = gpGlobals->time - s_lsPeaceEndAt;
	bool armed = m_rgpPlayerItems[PRIMARY_WEAPON_SLOT] != nullptr;
	// (hit: armed, the fight's on; unarmed, it fights whoever hit it - LsModeLetsBe - and goes
	// back to looting: a knife let loose on whoever's nearest set off the chains of knife kills)
	// (kitted out is a reason to go hunting only after the first 45 s: everyone landed armed on
	// some maps, and the whole lobby went hunting at once)
	// (unarmed: each its own while - the bold give up on a gun sooner, the careful look longer)
	float unarmed = 80.0f + LsModeSway(s_lsPhaseInfl - 1.0f) * (40.0f * LsTrait(206) - 30.0f * (LsAggro() - 0.5f) - 20.0f);
	if (since < 0.0f || since > (armed ? 90.0f : unarmed) || (armed && since > 45.0f && LsModeKitted())
		|| (armed && GetTimeSinceAttacked() < 2.0f))
	{
		m_lsModeGearDone = true;
		return false;
	}
	return true;
}

// Gearing up, someone in sight is left be - unless they're right on it, or coming at it facing it
// (then it's a fight, and the gearing up is over)
bool CCSBot::LsModeLetsBe(CBasePlayer *pPlayer)
{
	LS_PROF("LsModeLetsBe");
	// (hunting someone down: others a way off who aren't after it are left be - it keeps on toward the
	// hunted one; anyone close, or shooting at it, is a fight as usual)
	if (m_lsModeHunting.IsValid() && gpGlobals->time < m_lsModeHuntingUntil && pPlayer != (CBasePlayer *)m_lsModeHunting)
	{
		float d = (pPlayer->pev->origin - pev->origin).Length();
		bool threat = (pPlayer == GetAttacker() && GetTimeSinceAttacked() < 5.0f)
			|| (pPlayer->IsBot() && static_cast<CCSBot *>(pPlayer)->GetEnemy() == this && static_cast<CCSBot *>(pPlayer)->IsAttacking());
		if (!threat && d > 500.0f + 250.0f * m_lsModeHuntSway * LsAggro())
			return true;
	}
	if (gpGlobals->time < m_lsModeRevivingUntil)
	{
		float d = (pPlayer->pev->origin - pev->origin).Length();
		bool threat = (pPlayer == GetAttacker() && GetTimeSinceAttacked() < 5.0f)
			|| (pPlayer->IsBot() && static_cast<CCSBot *>(pPlayer)->GetEnemy() == this && static_cast<CCSBot *>(pPlayer)->IsAttacking());
		if (!threat && d > 700.0f)
			return true;
	}
	if (!LsModeGearing())
		return false;
	// (another bot: only once it's going for this one - two at one crate who bump or glance at each
	// other kept setting each other off; a person: right on it facing it, or coming at it facing it
	// from close by)
	Vector toMe = pev->origin - pPlayer->pev->origin;
	float d = toMe.Length();
	// (whoever hit it lately is a fight)
	bool threat = pPlayer == GetAttacker() && GetTimeSinceAttacked() < 5.0f;
	// (armed, and someone in sight who landed close by: a hot drop - fight now; in sight already,
	// this is only asked of players it sees)
	if (m_rgpPlayerItems[PRIMARY_WEAPON_SLOT] && d < 400.0f)
	{
		m_lsModeGearDone = true;
		LsWhy("mode gearing up: hot drop");
		return false;
	}
	if (!threat && pPlayer->IsBot())
	{
		CCSBot *pOther = static_cast<CCSBot *>(pPlayer);
		threat = pOther->IsAttacking() && pOther->GetEnemy() == this && pOther->IsEnemyVisible();
	}
	else if (!threat && d < 250.0f)
	{
		UTIL_MakeVectors(pPlayer->pev->v_angle);
		Vector dir = toMe / Q_max(d, 1.0f);
		bool facing = DotProduct(gpGlobals->v_forward, dir) > 0.9f;
		bool closing = DotProduct(pPlayer->pev->velocity, dir) > 100.0f;
		threat = facing && (d < 100.0f || closing);
	}
	// (that one is a fight - the rest are still left be: ending the gearing up here turned it on
	// whoever was nearest, and set off a chain)
	if (threat)
	{
		if (m_lsWhy == nullptr || Q_strcmp(m_lsWhy, "mode gearing up: someone came at it"))
			LsWhy("mode gearing up: someone came at it");
		return false;
	}
	return true;
}

// In the peace: a fight it was in ends, noises of others aren't chased, and it keeps its distance
// from anyone close (looting, not crowding the others for the moment the fighting starts)
void CCSBot::LsModePeaceTick()
{
	LS_PROF("LsModePeaceTick");
	if (IsAttacking())
		StopAttacking();
	if (IsFollowing())
		StopFollowing();
	ForgetNoise();
	LsModeRoomTick(350.0f);
}

// Room from anyone close: now and then it walks away from the nearest one within reach
void CCSBot::LsModeRoomTick(float reach)
{
	LS_PROF("LsModeRoomTick");
	float now = gpGlobals->time;
	if (now < m_lsModePeaceMoveAt || IsOnLadder() || IsAttacking())
		return;
	m_lsModePeaceMoveAt = now + RANDOM_FLOAT(1.5f, 3.0f);
	CBasePlayer *pNear = nullptr;
	float nearest = reach;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!UTIL_IsValidPlayer(pPlayer) || pPlayer == this || !pPlayer->IsAlive())
			continue;
		// (a squad loots together: a team player keeps by its squad mates, a loner gives them room too)
		if (g_lsModeTies && LsTeamwork() >= 0.5f && BotRelationship(pPlayer) == BOT_TEAMMATE)
			continue;
		float d = (pPlayer->pev->origin - pev->origin).Length();
		if (d < nearest)
		{
			nearest = d;
			pNear = pPlayer;
		}
	}
	if (!pNear || LsModeHeadedTo(m_lsModeMoveTo))
		return;
	Vector away = pev->origin + LsModeDir2D(pNear->pev->origin, pev->origin) * 600.0f;
	LsModeMove(LsModeOnMesh(away), false);
	m_lsModePushUntil = now + 4.0f;
	LsWhy("mode peace: giving them room");
}

// ---- ties between players (BotRelationship) -------------------------------------------------

int LsModeTie(const CBasePlayer *me, const CBasePlayer *them)
{
	int i = const_cast<CBasePlayer *>(me)->entindex(), j = const_cast<CBasePlayer *>(them)->entindex();
	if (i < 1 || i > MAX_CLIENTS || j < 1 || j > MAX_CLIENTS)
		return -1;
	unsigned int bit = 1u << (j - 1);
	if (s_lsFoe[i] & bit)
		return 1;
	if (s_lsFriend[i] & bit)
		return 0;
	return -1;
}

// A player left: their slot's role and ties go, and every tie to them (a new player in the
// slot starts with none)
void LsModePlayerGone(int index)
{
	if (index < 1 || index > MAX_CLIENTS)
		return;
	s_lsRole[index][0] = '\0';
	s_lsLayer[index] = -1;
	s_lsLayerInfl[index] = 0.0f;
	s_lsHeals[index] = -1;
	s_lsDown[index] = false;
	for (int k = 0; k < LS_MODE_OFFERS; k++)
		s_lsOffer[index][k] = LsModeOffer();
	s_lsFriend[index] = s_lsFoe[index] = 0;
	unsigned int bit = 1u << (index - 1);
	g_lsModeTies = false;
	for (int i = 1; i <= MAX_CLIENTS; i++)
	{
		s_lsFriend[i] &= ~bit;
		s_lsFoe[i] &= ~bit;
		if (s_lsFriend[i] || s_lsFoe[i])
			g_lsModeTies = true;
	}
}

// ---- path costs (PathCost, cs_bot.h) --------------------------------------------------------

// Anything that changes paths: an avoid, a blocked door or the safe zone (checked once a frame)
bool LsModeCosts()
{
	static float s_checkedAt = -1.0f;
	static bool s_any = false;
	if (!s_lsModeOn)
		return false;
	if (gpGlobals->time != s_checkedAt)
	{
		s_checkedAt = gpGlobals->time;
		s_any = s_lsRing.on;
		for (int i = 0; !s_any && i < LS_MODE_ZONES; i++)
			s_any = LsModeLive(s_lsAvoid[i]);
	}
	return s_any;
}

// How much more a walk through this area costs: 1 nothing extra; an avoid adds its cost, a
// no-go zone is never walked through (unless the bot is in it already: then it may walk out);
// outside the safe zone costs more the further out it is
float LsModeAreaCost(const CNavArea *area, unsigned int insideMask)
{
	float factor = 1.0f;
	for (int i = 0; i < LS_MODE_ZONES; i++)
	{
		const LsModeZone &z = s_lsAvoid[i];
		if (!LsModeLive(z) || !LsModeAreaInZone(z, area))
			continue;
		if (z.value < 0.0f)
		{
			if (!(insideMask & (1u << i)))
				return -1.0f;
			factor += 10.0f;
		}
		else
			factor += Q_min(z.value, 10.0f);
	}
	if (s_lsRing.on)
	{
		float out = LsModeDist2D(*area->GetCenter(), s_lsRing.at) - s_lsRing.r;
		if (out > 0.0f)
			factor += 4.0f * (1.0f + out / 1000.0f);
	}
	return factor;
}

// The avoid zones it's standing in: its paths may cross those (to get out)
void CCSBot::LsModeInside(unsigned int *avoidMask) const
{
	*avoidMask = 0;
	for (int i = 0; i < LS_MODE_ZONES; i++)
		if (LsModeLive(s_lsAvoid[i]) && LsModeInZone(s_lsAvoid[i], pev->origin))
			*avoidMask |= 1u << i;
}

// ---- mode enemies (creatures) ---------------------------------------------------------------

static bool LsModeTargetOk(CBaseEntity *pEntity)
{
	// (never a friendly creature: a player's bought turret)
	return pEntity && !FNullEnt(pEntity->pev) && pEntity->pev->takedamage != DAMAGE_NO && pEntity->pev->health > 0.0f
		&& pEntity->pev->deadflag == DEAD_NO && !(pEntity->pev->effects & EF_NODRAW) && !LsCreatureIsFriendly(pEntity);
}

static void LsModeRefreshTargets()
{
	LS_PROF("LsModeRefreshTargets");
	float now = gpGlobals->time;
	if (now < s_lsTargetsAt && now > s_lsTargetsAt - 1.0f)
		return;
	s_lsTargetsAt = now + 0.2f;
	s_lsTargetCount = 0;
	for (int c = 0; c < s_lsClassCount; c++)
	{
		CBaseEntity *pEntity = nullptr;
		while (s_lsTargetCount < LS_MODE_TARGETS && (pEntity = UTIL_FindEntityByClassname(pEntity, s_lsClass[c].name)))
		{
			// (never a player: "enemyclass player" mustn't turn bots on everyone, or themselves)
			if (pEntity->IsPlayer() || !LsModeTargetOk(pEntity))
				continue;
			s_lsTargetThreat[s_lsTargetCount] = s_lsClass[c].threat;
			s_lsTarget[s_lsTargetCount++] = pEntity;
		}
	}
}

// Something between a bot and a mode enemy (a barricade, a fence): shot through by design -
// one of the mode's own entities (ls_*) marked so (pev->iuser4 1: built to let bullets by, a
// fence of wire or boards with gaps)
static bool LsModeShootThrough(CBaseEntity *pThing)
{
	return pThing && !FNullEnt(pThing->pev) && !Q_strncmp(STRING(pThing->pev->classname), "ls_", 3) && pThing->pev->iuser4 == 1;
}

// ...or seen through: shot through by design, nothing drawn (a beam fence's box), or see-through
// (a window, a barricade faded with damage; the sight traces skip such "glass", bullets don't)
static bool LsModeSeeThrough(CBaseEntity *pThing)
{
	if (!pThing || FNullEnt(pThing->pev))
		return false;
	return LsModeShootThrough(pThing) || FStringNull(pThing->pev->model) || !STRING(pThing->pev->model)[0]
		|| (pThing->pev->effects & EF_NODRAW) || (pThing->pev->rendermode != kRenderNormal && pThing->pev->renderamt < 200.0f);
}

// A mode enemy it could touch: a body-sized line from it to them meets nothing else (a barricade
// stops this even where bullets and sight go through its gaps) - what a knife needs, and what
// it takes for one to be a threat close up
static bool LsModeReachable(CBaseEntity *pFrom, CBaseEntity *pTo)
{
	LS_PROF("LsModeReachable");
	TraceResult tr;
	UTIL_TraceHull(pFrom->pev->origin, pTo->Center(), dont_ignore_monsters, head_hull, pFrom->edict(), &tr);
	return tr.flFraction == 1.0f || tr.pHit == pTo->edict();
}

// One of the mode enemies alive now
static bool LsModeIsTarget(CBaseEntity *pThing)
{
	for (int k = 0; k < s_lsTargetCount; k++)
		if (s_lsTarget[k] == pThing)
			return LsModeTargetOk(pThing);
	return false;
}

// A mode enemy in sight: a point on it seen from the eyes (a monster's middle and its upper
// body; a brush creature's nearest side and its middle - a line to the middle of a brush
// stops on its own surface, which counts). *at gets the point seen; *traces counts the lines
bool CCSBot::LsModeSees(CBaseEntity *pEntity, bool testFOV, Vector *at, int *traces)
{
	LS_PROF("LsModeSees");
	const Vector eye = GetEyePosition();
	const Vector lo = pEntity->pev->absmin, hi = pEntity->pev->absmax;
	Vector points[2];
	if (pEntity->pev->solid == SOLID_BSP)
	{
		// (the box's nearest point to the eyes, an inch inside it)
		points[0] = Vector(Q_max(lo.x + 1.0f, Q_min(eye.x, hi.x - 1.0f)), Q_max(lo.y + 1.0f, Q_min(eye.y, hi.y - 1.0f)),
			Q_max(lo.z + 1.0f, Q_min(eye.z, hi.z - 1.0f)));
		points[1] = (lo + hi) * 0.5f;
	}
	else
	{
		points[0] = (lo + hi) * 0.5f;
		points[1] = Vector(points[0].x, points[0].y, Q_max(points[0].z, hi.z - 12.0f));
	}
	for (int i = 0; i < 2; i++)
	{
		const Vector &pos = points[i];
		if (testFOV && !FInViewCone(&pos))
			continue;
		if (TheCSBots()->IsLineBlockedBySmoke(&eye, &pos))
			continue;
		TraceResult result;
		UTIL_TraceLine(eye, pos, ignore_monsters, ignore_glass, ENT(pev), &result);
		(*traces)++;
		if (result.flFraction < 1.0f && result.pHit != pEntity->edict())
			continue;
		// (the line skipped "glass": anything solid in it that isn't see-through - a barricade,
		// opaque boards - hides it after all)
		UTIL_TraceLine(eye, pos, ignore_monsters, dont_ignore_glass, ENT(pev), &result);
		(*traces)++;
		if (result.flFraction < 1.0f && result.pHit != pEntity->edict() && !LsModeSeeThrough(CBaseEntity::Instance(result.pHit)))
			continue;
		*at = pos;
		return true;
	}
	return false;
}

// No player enemy in sight, but a creature (an entity of a class the mode named) is: it gets
// shot like an enemy would be - the view on it, the trigger when the aim's there. The most
// threatening one for the distance is picked, and kept a moment (not flicking between them)
bool CCSBot::LsModeShootTick()
{
	LS_PROF("LsModeShootTick");
	if (s_lsClassCount <= 0 || IsAttacking() || m_isEnemyVisible || IsOnLadder() || IsBlind() || LsNadeBusy() || m_isWaitingToTossGrenade)
	{
		m_lsModeTarget = nullptr;
		return false;
	}
	LsModeRefreshTargets();

	float now = gpGlobals->time;
	const Vector eye = GetEyePosition();
	CBaseEntity *pTarget = m_lsModeTarget;
	if (now >= m_lsModeScanAt || !LsModeTargetOk(pTarget))
	{
		// (a scan: lines of sight to the nearest few; every half second - the one it's on is
		// checked every think anyway)
		m_lsModeScanAt = now + 0.5f;
		// the ones in range, nearest first; the lines of sight checked for the nearest few only
		int order[LS_MODE_TARGETS];
		float ranges[LS_MODE_TARGETS];
		int count = 0;
		for (int k = 0; k < s_lsTargetCount; k++)
		{
			CBaseEntity *pEntity = s_lsTarget[k];
			if (!LsModeTargetOk(pEntity))
				continue;
			float range = (pEntity->Center() - eye).Length();
			if (range > 2500.0f)
				continue;
			int at = count++;
			while (at > 0 && ranges[at - 1] > range)
			{
				ranges[at] = ranges[at - 1];
				order[at] = order[at - 1];
				at--;
			}
			ranges[at] = range;
			order[at] = k;
		}
		CBaseEntity *pBest = nullptr;
		float bestScore = 0.0f;
		int traces = 0;
		for (int n = 0; n < count && traces < 4; n++)
		{
			int k = order[n];
			CBaseEntity *pEntity = s_lsTarget[k];
			Vector seen;
			// (one it's already shooting doesn't have to be in front of it to stay its target;
			// nor one close by - its steps, its snarl - or one coming for this bot: it turns to them)
			CBaseMonster *pMonster = pEntity->MyMonsterPointer();
			bool comingForMe = pMonster && pMonster->m_hEnemy == this;
			bool sensed = pEntity == pTarget || ranges[n] < 400.0f || (comingForMe && ranges[n] < 900.0f);
			if (!LsModeSees(pEntity, !sensed, &seen, &traces))
				continue;
			float score = s_lsTargetThreat[k] / Q_max(ranges[n], 100.0f);
			if (score > bestScore)
			{
				bestScore = score;
				pBest = pEntity;
			}
		}
		if (pBest && pBest != pTarget)
			LsWhy("mode enemy");
		m_lsModeTarget = pBest;
		pTarget = pBest;
	}
	if (!pTarget)
		return false;

	Vector pos;
	int traces = 0;
	if (!LsModeSees(pTarget, false, &pos, &traces))
	{
		m_lsModeTarget = nullptr;
		return false;
	}

	m_lsModeSawAt = now;

	// a gun up (not the knife or a grenade, if it has one)
	if ((IsUsingKnife() || IsUsingGrenade()) && !LsKnifeOnly())
		EquipBestWeapon(true);

	SetLookAt("Loner-Strike: mode enemy", &pos, PRIORITY_HIGH, 0.5f, false, 3.0f);

	// how it fights: a gun is fired standing still (on the move it sprays wide, and the creatures
	// close in); one right up close, a step or two back (the bolder hold their ground); hurt, from
	// one shooting at range, to cover (running from a biter only gets it bitten); a knife closes in
	float gap = (pTarget->pev->origin - pev->origin).Make2D().Length();
	// (close: whether it can get at the bot at all - behind a gate it's no threat up close, and
	// no knife reaches it)
	bool reachable = gap < 250.0f && LsModeReachable(this, pTarget);
	// (hurt: the careful go for cover sooner, the bold fight on lower)
	if (LsHealth() < 25.0f + 25.0f * (1.0f - LsAggro()) && gap > 350.0f && gap < 900.0f && now >= m_lsModeCoverAt)
	{
		m_lsModeCoverAt = now + 4.0f;
		if (LsTakeCoverFrom(pTarget->pev->origin, 300.0f, true))
			LsWhy("mode enemy: hurt, to cover");
	}
	// (the better ones keep their distance sooner: kiting, not trading hits)
	else if (!IsUsingKnife() && gap < 140.0f + 140.0f * LsSkill() && reachable && LsAggro() < 0.75f)
	{
		m_lsModeBackFrom = pTarget->pev->origin;
		m_lsModeBackUntil = now + 0.35f;
	}
	else if (IsUsingKnife() && LsKnifeOnly() && gap > 60.0f && (gap >= 250.0f || reachable))
	{
		m_lsModeStepTo = pTarget->pev->origin;
		m_lsModeStepUntil = now + 0.35f;
	}
	else if (!IsUsingKnife())
	{
		// (a person's rhythm at range: a burst standing still, then a sidestep with the trigger
		// let go - counter-strafing; the bolder step more, close up it just stands and shoots)
		float cycle = 1.4f, stand = 1.4f * (0.85f - 0.35f * LsAggro());
		float phase = Q_fmod(now + float(LsSeed() % 97) * 0.13f, cycle);
		if (gap > 400.0f && phase > stand)
		{
			m_lsModeStrafeUntil = now + 0.15f;
			m_lsModeStrafeLeft = (int((now + float(LsSeed() % 97) * 0.13f) / cycle) % 2) == 0;
			return true;
		}
		m_lsModeStillUntil = now + 0.4f;
	}

	// no shot that can't reach it: the bullet's own line from the gun - a barricade, boards, a
	// fence that stops bullets in the way and the trigger stays off (it keeps watching; after a
	// moment it moves for another angle, or waits for them to break through)
	// (a trace with monsters in it: costly in a crowd of creatures and players, so kept 0.15 s for
	// the same target)
	if (now >= m_lsModeShotAt + 0.15f || m_lsModeShotFor != pTarget->entindex())
	{
		TraceResult shot;
		UTIL_TraceLine(GetGunPosition(), pos, dont_ignore_monsters, dont_ignore_glass, ENT(pev), &shot);
		CBaseEntity *pInWay = (shot.flFraction < 1.0f && shot.pHit != pTarget->edict()) ? CBaseEntity::Instance(shot.pHit) : nullptr;
		m_lsModeShotBlocked = shot.flFraction < 1.0f && shot.pHit != pTarget->edict() && !(pInWay && (LsModeShootThrough(pInWay) || LsModeIsTarget(pInWay)));
		m_lsModeShotAt = now;
		m_lsModeShotFor = pTarget->entindex();
	}
	if (m_lsModeShotBlocked)
	{
		if (m_lsModeBlockedSince < 0.0f)
		{
			m_lsModeBlockedSince = now;
			LsWhy("mode enemy: held fire, something in the way");
		}
		m_lsModeStillUntil = 0.0f;
		if (now - m_lsModeBlockedSince < 1.5f)
			return true;
		// (another angle: a spot near by, once in a while; meanwhile the rest of its play goes on)
		if (now >= m_lsModeAngleAt && !IsUsingLadder())
		{
			m_lsModeAngleAt = now + 4.0f;
			Vector spot;
			if (LsModePickSpot(pev->origin, 350.0f, &spot))
			{
				LsModeMove(spot, true);
				m_lsModePushUntil = now + 3.0f;
				LsWhy("mode enemy: moving for an angle");
			}
		}
		return false;
	}
	m_lsModeBlockedSince = -1.0f;

	// the trigger once the view's on it (the stock fire's tolerance: a body's width at that range)
	Vector to = pos - GetGunPosition();
	float range = to.Length();
	if (range < 1.0f)
		return true;
	UTIL_MakeVectors(pev->v_angle + pev->punchangle);
	float onTarget = DotProduct(gpGlobals->v_forward, to / range);
	float halfSize = IsUsingSniperRifle() ? HalfHumanWidth : 2.0f * HalfHumanWidth;
	if (onTarget < Q_cos(Q_atan(halfSize / range)))
		return true;

	if (IsUsingKnife())
	{
		// (the knife only when it's all it has - with a gun the switch is on its way - and only at
		// one it can reach: not a swing into a barricade)
		if (LsKnifeOnly() && range < 70.0f && reachable && !IsFriendInLineOfFire())
		{
			PrimaryAttack();
			m_lsModePressed |= 1;
		}
	}
	else if (IsActiveWeaponClipEmpty())
	{
		ReloadCheck();
		m_lsModePressed |= 4;
	}
	else if (!IsActiveWeaponReloading() && !IsFriendInLineOfFire() && (!IsUsingPistol() || LsTriggerTap()))
	{
		PrimaryAttack();
		m_lsModePressed |= 1;
	}
	return true;
}

// ---- goals (each bot's own) -----------------------------------------------------------------

// How much loot and pickups pull it: the bolder want more, campers and the careful less
float CCSBot::LsModeGreed()
{
	float greed = 0.35f + 0.5f * LsAggro() + 0.3f * (LsTrait(201) - 0.5f);
	int style = LsStyle();
	if (style == LS_STYLE_RUSHER || style == LS_STYLE_ROAMER)
		greed += 0.15f;
	else if (style == LS_STYLE_CAMPER || style == LS_STYLE_CAREFUL)
		greed -= 0.15f;
	return Q_max(0.0f, Q_min(1.0f, greed));
}

// A goal's advisory: whether this bot takes it up is up to who it is (rolled once, as it's
// given). Team jobs go by its teamwork, loot by its greed, running away by its caution
bool CCSBot::LsModeCares(int kind, int prio, const Vector &at, float infl, int ent)
{
	// (the influence: 0 every bot takes it up; 1 the usual; 2 its personality fully - the careful
	// may turn down a risky one, even one the mode says matters)
	float sway = LsModeSway(infl);
	if (kind == LS_GOAL_OFFER || sway <= 0.0f)
		return true;	// (an offer: its own pick)
	float lean;
	switch (kind)
	{
	case LS_GOAL_USE:
	{
		// (a downed teammate: by teamwork - a revive pays now, but it's the team players who go; a gate's
		// boards: the team-minded and the defensive repair, the lone and the aggressive rarely)
		CBaseEntity *pThing = ent > 0 && INDEXENT(ent) && !INDEXENT(ent)->free ? CBaseEntity::Instance(INDEXENT(ent)) : nullptr;
		if (pThing && pThing->IsPlayer())
			lean = 0.3f + 0.7f * LsTeamwork();
		else if (pThing && FClassnameIs(pThing->pev, "ls_barricade"))
			lean = 0.6f * LsTeamwork() + 0.4f * (1.0f - LsAggro());
		else
			lean = LsModeGreed();
		break;
	}
	case LS_GOAL_LOOT:
	case LS_GOAL_INVESTIGATE:
		lean = LsModeGreed();
		break;
	case LS_GOAL_FLEE:
		lean = 1.0f - LsAggro();
		break;
	case LS_GOAL_HUNT:
		lean = 0.5f + 0.5f * LsAggro();
		break;
	default:
		lean = LsTeamwork();
		break;
	}
	float chance = prio >= 30 ? 0.55f + 0.45f * lean : 0.1f + 0.5f * lean;
	// (how risky it is there: creatures, enemy players about - the careful less keen; for a
	// run-away, the keener)
	float risk = 0.0f;
	for (int k = 0; k < s_lsTargetCount; k++)
		if (LsModeTargetOk(s_lsTarget[k]) && (s_lsTarget[k]->pev->origin - at).IsLengthLessThan(600.0f))
			risk += 0.25f;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (UTIL_IsValidPlayer(pPlayer) && pPlayer->IsAlive() && !LsModeIsDown(pPlayer)
			&& BotRelationship(pPlayer) == BOT_ENEMY && (pPlayer->pev->origin - at).IsLengthLessThan(800.0f))
			risk += 0.35f;
	}
	risk = Q_min(1.0f, risk);
	// (holding a place is the job whatever's about - a defence is where the creatures come; an errand
	// into danger isn't)
	if (kind == LS_GOAL_FLEE)
		chance = Q_min(1.0f, chance + 0.3f * risk);
	// (a revive's risk is weighed as it goes in - who's watching, how brave it is - not here)
	else if ((kind == LS_GOAL_LOOT || kind == LS_GOAL_USE || kind == LS_GOAL_INVESTIGATE || kind == LS_GOAL_RIDE || kind == LS_GOAL_HUNT)
		&& !(kind == LS_GOAL_USE && ent > 0 && ent <= gpGlobals->maxClients))
		chance *= 1.0f - 0.5f * risk * (1.0f - LsAggro());
	// (one that matters - prio 80 up - is always taken up unless its personality has the full say)
	if (prio >= 80)
		chance = 1.0f - Q_max(0.0f, sway - 1.0f) * (1.0f - chance);
	else
		chance = 1.0f - sway * (1.0f - chance);
	return RANDOM_FLOAT(0.0f, 1.0f) < chance;
}

// A new goal: one of each kind (the same kind again replaces it); with every slot taken, the
// least important goes if the new one matters more
static Vector LsModeOnMesh(const Vector &pos);

void CCSBot::LsModeSetGoal(int kind, const Vector &at, float radius, int ent, int prio, float secs, float infl)
{
	// (an entity named has to be there now; it's kept by handle, so a new one in its slot later
	// isn't mistaken for it)
	CBaseEntity *pThing = nullptr;
	if (ent > 0)
	{
		edict_t *pEdict = INDEXENT(ent);
		if (!pEdict || pEdict->free || !pEdict->pvPrivateData)
			return;
		pThing = CBaseEntity::Instance(pEdict);
	}
	int slot = -1;
	for (int i = 0; i < LS_MODE_GOALS && slot < 0; i++)
		if (m_lsModeGoal[i].kind == kind)
			slot = i;
	for (int i = 0; i < LS_MODE_GOALS && slot < 0; i++)
		if (m_lsModeGoal[i].kind == LS_GOAL_NONE)
			slot = i;
	if (slot < 0)
	{
		for (int i = 0; i < LS_MODE_GOALS; i++)
			if (m_lsModeGoal[i].prio < prio && (slot < 0 || m_lsModeGoal[i].prio < m_lsModeGoal[slot].prio))
				slot = i;
		if (slot < 0)
			return;
	}
	LsModeGoal &g = m_lsModeGoal[slot];
	// (the same one again - a mode re-sends its goals - keeps its start)
	bool again = g.kind == kind && (kind == LS_GOAL_HUNT ? g.ent == ent : (g.at - at).IsLengthLessThan(64.0f));
	float startAt = g.startAt;
	// (the same kind of job again - a hold moved to another spot, a goal re-sent - keeps its answer:
	// rolled again each time, a fifth of the defenders dropped their spot at every reassignment)
	bool sameKind = g.kind == kind && g.ent == ent;
	bool care = g.care;
	g = LsModeGoal();
	g.kind = kind;
	g.at = at;
	g.radius = radius > 0.0f ? radius : s_lsGoalRadius[kind];
	g.ent = ent;
	g.thing = pThing;
	g.onMesh = LsModeOnMesh(at);
	g.prio = prio;
	g.until = LsModeUntil(secs);
	g.infl = infl;
	g.care = (sameKind && prio < 80) ? care : LsModeCares(kind, prio, at, infl, ent);
	// (on its way after a moment - the less skilled, the longer; a must-do one at once)
	g.startAt = again ? startAt
		: (prio >= 90 ? 0.0f : gpGlobals->time + LsModeSway(infl) * (1.0f - LsSkill()) * RANDOM_FLOAT(0.4f, 1.2f));
	if (m_lsModeGoalOn == slot)
		m_lsModeGoalOn = -1;
}

void CCSBot::LsModeClearGoals(int kind, int ent)
{
	for (int i = 0; i < LS_MODE_GOALS; i++)
	{
		LsModeGoal &g = m_lsModeGoal[i];
		if (g.kind == LS_GOAL_NONE || (kind && g.kind != kind) || (ent && g.ent != ent))
			continue;
		g = LsModeGoal();
		if (m_lsModeGoalOn == i)
			m_lsModeGoalOn = -1;
	}
}

bool CCSBot::LsModeHasGoal() const
{
	for (int i = 0; i < LS_MODE_GOALS; i++)
		if (m_lsModeGoal[i].kind != LS_GOAL_NONE)
			return true;
	return false;
}

// Its last mode MoveTo is the one it's on, to that spot (nothing else sent it elsewhere since)
bool CCSBot::LsModeHeadedTo(const Vector &to) const
{
	return IsMovingTo() && m_lsModeMoveAt >= 0.0f && Q_fabs(m_stateTimestamp - m_lsModeMoveAt) < 0.05f
		&& (m_lsModeMoveTo - to).IsLengthLessThan(48.0f);
}

bool CCSBot::LsModeMove(const Vector &to, bool hurry)
{
	LS_PROF("LsModeMove");
	if (LsModeHeadedTo(to))
		return false;
	m_lsModeMoveTo = to;
	MoveTo(&to, (hurry || LsAggro() > 0.6f) ? FASTEST_ROUTE : SAFEST_ROUTE);
	m_lsModeMoveAt = m_stateTimestamp;
	return true;
}

// A spot on the nav mesh near a point (the point itself if there's no mesh)
static Vector LsModeOnMesh(const Vector &pos)
{
	LS_PROF("LsModeOnMesh");
	CNavArea *area = TheNavAreaGrid.GetNearestNavArea(&pos, true);
	if (!area)
		return pos;
	Vector spot = pos;
	area->GetClosestPointOnArea(&pos, &spot);
	return spot;
}

// Stood in a no-go zone (or a strong avoid), or outside the safe zone: out of it, before
// anything but a fight - a person's order and the mode's own goals wait (true: on its way)
bool CCSBot::LsModeEscape()
{
	LS_PROF("LsModeEscape");
	if (IsAttacking() || m_isEnemyVisible || IsOnLadder() || LsOrderBusy())
		return false;
	float now = gpGlobals->time;
	Vector out;
	bool go = false;

	for (int i = 0; i < LS_MODE_ZONES && !go; i++)
	{
		const LsModeZone &z = s_lsAvoid[i];
		if (!LsModeLive(z) || (z.value >= 0.0f && z.value < 5.0f) || !LsModeInZone(z, pev->origin))
			continue;
		Vector center = z.box ? (z.lo + z.hi) * 0.5f : z.at;
		float reach = z.box ? Q_max(z.hi.x - z.lo.x, z.hi.y - z.lo.y) * 0.5f : z.r;
		out = center + LsModeDir2D(center, pev->origin) * (reach + 150.0f);
		out.z = pev->origin.z;
		go = true;
	}

	if (!go && s_lsRing.on)
	{
		// (the careful keep further from the edge; with the shrink coming, the next ring)
		float caution = 1.0f - LsAggro();
		Vector ringAt = s_lsRing.at;
		float ringR = s_lsRing.r;
		// (and each its own timing: some rotate early, some cut it fine, as people do)
		if (s_lsRing.next && s_lsRing.shrinkAt > 0.0f && s_lsRing.shrinkAt - now < 10.0f + 25.0f * caution + 20.0f * LsTrait(212))
		{
			ringAt = s_lsRing.nextAt;
			ringR = s_lsRing.nextR;
		}
		float margin = Q_min(100.0f + 300.0f * caution, 0.4f * ringR);
		float d = LsModeDist2D(pev->origin, ringAt);
		// (a goal inside the ring takes it in anyway)
		bool goalInside = m_lsModeGoalOn >= 0 && LsModeDist2D(m_lsModeGoal[m_lsModeGoalOn].at, ringAt) < ringR - margin;
		if (d > ringR - margin && !goalInside)
		{
			out = ringAt + LsModeDir2D(ringAt, pev->origin) * (0.5f * ringR);
			out.z = pev->origin.z;
			go = true;
		}
	}

	if (!go)
		return false;
	// (getting out comes before standing to shoot)
	m_lsModeStillUntil = m_lsModeBackUntil = 0.0f;
	// (already on its way out: sent lately, or its escape is still the MoveTo it's on)
	if (IsMovingTo() && (now - m_lsModeEscapeAt < 2.0f || Q_fabs(m_stateTimestamp - m_lsModeEscapeState) < 0.05f))
		return true;
	out = LsModeOnMesh(out);
	m_lsModeEscapeAt = now;
	m_lsModeMoveTo = out;
	MoveTo(&out, FASTEST_ROUTE);
	m_lsModeMoveAt = m_lsModeEscapeState = m_stateTimestamp;
	LsWhy("mode zone");
	return true;
}

// Hunting with nothing better to do: now and then off to somewhere the mode says is worth it
// (a weapon spawner, a crate, a supply drop), the greedier the likelier, the nearer the likelier
// Which layer a spot is on: 1 below (under the map's usual floor, with a roof over it), 2 high
// (well above it), 0 the surface. The usual floor is the nav's median height, worked out once a map
static int LsModeLayer(const Vector &spot)
{
	if (s_lsNavMidZ < -99998.0f)
	{
		std::vector<float> zs;
		zs.reserve(TheNavAreaList.size());
		for (auto area : TheNavAreaList)
			zs.push_back(area->GetCenter()->z);
		if (zs.empty())
			return 0;
		std::nth_element(zs.begin(), zs.begin() + zs.size() / 2, zs.end());
		s_lsNavMidZ = zs[zs.size() / 2];
	}
	if (spot.z > s_lsNavMidZ + 200.0f)
		return 2;
	if (spot.z < s_lsNavMidZ - 60.0f)
	{
		TraceResult tr;
		UTIL_TraceLine(spot + Vector(0, 0, 32), spot + Vector(0, 0, 600), ignore_monsters, nullptr, &tr);
		if (tr.flFraction < 1.0f && UTIL_PointContents(tr.vecEndPos + Vector(0, 0, 2)) != CONTENTS_SKY)
			return 1;
	}
	return 0;
}

// The layer this bot would rather loot on: kitted out, high ground for the sightlines; short of
// a main gun or armor, mostly below (the vaults' rifles); otherwise its own taste - and a few
// bold ones go where the best loot (and the fights) are (-1: anywhere, the richest)
int CCSBot::LsModeLayerWant()
{
	LS_PROF("LsModeLayerWant");
	// (the mode's say first: Battle Royale spreads its players over the layers)
	int me = entindex();
	int told = me >= 1 && me <= MAX_CLIENTS ? s_lsLayer[me] : -1;
	// (with its personality given more say than usual, some go by their own taste after all)
	if (told >= 0 && LsTrait(214) >= 0.6f * Q_max(0.0f, LsModeSway(s_lsLayerInfl[me] - 1.0f) - 1.0f))
		return told;
	if (LsModeKitted())
		return 2;
	float taste = LsTrait(210);
	if (LsAggro() > 0.65f && taste < 0.35f)
		return -1;
	if ((!m_rgpPlayerItems[PRIMARY_WEAPON_SLOT] || pev->armorvalue < 30.0f) && LsTrait(211) < 0.5f)
		return 1;
	return taste < 0.33f ? 2 : (taste < 0.66f ? 1 : 0);
}

// What the mode offers (ls_mode offer), taken like a person would: only with the money for it and a
// little to spare, and when it's calm round here (no creature near, not mid-fight); perks in their
// order (Tough first), the bench when the money's there, the box now and then (the greedier, the
// more), the trap switch only when a gate's under heavy pressure (five creatures near it), a turret
// at a covered spot near where it's wanted. Taking one is walking over and using it (a use goal)
bool CCSBot::LsModeOfferTick()
{
	int me = entindex();
	if (me < 1 || me > MAX_CLIENTS)
		return false;
	float now = gpGlobals->time;
	if (now < m_lsModeOfferAt)
		return false;
	m_lsModeOfferAt = now + RANDOM_FLOAT(1.0f, 2.5f);
	// (already on its way to one it picked: those have a goal slot of their own, apart from the mode's
	// use goals - a gate repair, the shop - which kept replacing them on the way to a perk machine;
	// at 72 they come after a revive (75), before a repair (50) or the shop (25))
	for (int i = 0; i < LS_MODE_GOALS; i++)
	{
		const LsModeGoal &g = m_lsModeGoal[i];
		CBaseEntity *pThing = g.ent > 0 ? (CBaseEntity *)g.thing : nullptr;
		// (nor a revive: a teammate down comes first)
		if (g.kind == LS_GOAL_OFFER || (g.kind == LS_GOAL_USE && pThing && pThing->IsPlayer()))
			return false;
	}
	// (calm: nothing to shoot within 600 u)
	bool calm = now - m_lsModeSawAt > 3.0f;
	for (int k = 0; calm && k < s_lsTargetCount; k++)
		if (LsModeTargetOk(s_lsTarget[k]) && (s_lsTarget[k]->pev->origin - pev->origin).IsLengthLessThan(600.0f))
			calm = false;
	// (money kept for the next perk: the box or the bench only with that still left after - the box
	// at $950 in the first waves kept every bot short of a perk)
	int perkCost = 0;
	for (int k = 0; k < LS_MODE_OFFERS; k++)
		if (s_lsOffer[me][k].kind == LS_OFFER_PERK && (!perkCost || s_lsOffer[me][k].cost < perkCost))
			perkCost = s_lsOffer[me][k].cost;
	// (who it is: a gun player - bold, skilled - upgrades the gun before a turret; a careful team
	// player puts the turret down first and keeps more money by; a loner pulls the trap later)
	float aggro = LsAggro(), team = LsTeamwork(), skill = LsSkill();
	float tilt = 0.5f * (1.0f - aggro) + 0.5f * team - 0.5f * aggro - 0.5f * skill;
	// (saving for the bench: with the bench offered - the mode offers it once the perks are in and the
	// gun isn't upgraded, affordable or not - a bot keeps its price aside and skips the box till it's
	// done; a planner - careful, skilled - keeps it all and a bit more, an impulsive one less, and a
	// low-skill one now and then forgets. With a turret offer open, a team player keeps a little more
	// aside for that too)
	float planner = 0.5f * (1.0f - aggro) + 0.5f * skill;
	int benchSave = 0, turretSave = 0;
	for (int k = 0; k < LS_MODE_OFFERS; k++)
	{
		const LsModeOffer &o = s_lsOffer[me][k];
		float sway = LsModeSway(o.infl);
		if (o.kind == LS_OFFER_BENCH)
			benchSave = Q_max(benchSave, int(o.cost * Q_max(0.0f, 0.9f + sway * 0.6f * (planner - 0.5f))));
		else if (o.kind == LS_OFFER_TURRET)
			turretSave = Q_max(turretSave, int(o.cost * Q_max(0.0f, 0.25f * sway * team)));
	}
	if (benchSave && RANDOM_FLOAT(0.0f, 1.0f) < 0.35f * (1.0f - skill))
		benchSave = 0;	// (forgot)
	LsModeOffer *pBest = nullptr;
	int bestRank = 1000;
	for (int k = 0; k < LS_MODE_OFFERS; k++)
	{
		LsModeOffer &o = s_lsOffer[me][k];
		if (o.kind == LS_OFFER_NONE || now < o.nextTry)
			continue;
		float sway = LsModeSway(o.infl);
		// (money kept over the price: the careful more; the less skilled spend early)
		int spare = int((200.0f + sway * (250.0f * (1.0f - aggro) - 100.0f)) * (0.5f + skill));
		// (a turret is defense, bought when it can be: W1's at $2500 never came with a perk's price kept
		// on top as well)
		if ((o.kind == LS_OFFER_BOX || o.kind == LS_OFFER_BENCH) && m_iAccount < o.cost + perkCost + spare)
			continue;
		if (o.kind == LS_OFFER_BOX && m_iAccount < o.cost + benchSave + turretSave)
			continue;
		// (a turret is for the team: a selfish bot spends on itself instead)
		if (o.kind == LS_OFFER_TURRET && team < 0.3f && sway >= 1.0f && LsTrait(215) > team + 0.4f)
			continue;
		int rank = 1000;
		if (o.kind == LS_OFFER_TRAP)
		{
			int pressure = 0;
			for (int t = 0; t < s_lsTargetCount; t++)
				if (LsModeTargetOk(s_lsTarget[t]) && (s_lsTarget[t]->pev->origin - o.at).IsLengthLessThan(500.0f))
					pressure++;
			float most = 5.0f + sway * (2.0f * aggro + 2.0f * (1.0f - team) - 2.0f);
			if (pressure >= Q_max(2, int(most + 0.5f)) && m_iAccount >= o.cost)
				rank = 0;	// (urgent: even mid-fight)
		}
		else if (calm && m_iAccount >= o.cost + spare)
		{
			if (o.kind == LS_OFFER_PERK)
			{
				// (its own pick among them: the mode's order - Tough 0, Feet 1, Revive 2, Double tap 3,
				// Hands 4 - shifted by who it is: the bold want the damage ones sooner, the careful
				// health and revive)
				float shift = 0.0f;
				if (o.order == 3 || o.order == 4)
					shift = -4.0f * (aggro - 0.5f);
				else if (o.order == 0 || o.order == 2)
					shift = 4.0f * (aggro - 0.5f);
				rank = int(10.0f + float(o.order) + sway * shift + 0.5f);
			}
			// (the gun before a turret: the mode offers the bench once the perks are in, and a $2500
			// turret taken first left no money for it - bots never upgraded at a bench)
			else if (o.kind == LS_OFFER_BENCH)
				rank = int(30.0f + sway * 15.0f * tilt);
			else if (o.kind == LS_OFFER_TURRET)
				rank = int(40.0f - sway * 15.0f * tilt);
			else if (o.kind == LS_OFFER_BOX)
			{
				// (now and then: a roll a minute or so, the greedier the likelier)
				// (the impulsive gamble on it more)
				if (RANDOM_FLOAT(0.0f, 1.0f) < 0.325f + sway * (0.35f * (LsModeGreed() - 0.5f) + 0.2f * (0.5f - planner)))
					rank = 60;
				else
					o.nextTry = now + RANDOM_FLOAT(40.0f, 80.0f);
			}
		}
		if (rank < bestRank)
		{
			bestRank = rank;
			pBest = &o;
		}
	}
	if (!pBest)
		return false;
	pBest->nextTry = now + 20.0f;
	m_lsModeOfferKind = pBest->kind;
	m_lsModeOfferInfl = pBest->infl;
	m_lsModeOfferCost = pBest->cost;
	m_lsModeOfferMoney = -1;
	Vector at = pBest->at;
	if (pBest->kind == LS_OFFER_TURRET)
	{
		// (a covered spot near where it's wanted, facing that way)
		Vector spot;
		if (LsModePickSpot(pBest->at, 400.0f, &spot))
			at = spot;
		LsModeSetGoal(LS_GOAL_OFFER, at, 64.0f, 0, 72, 20.0f, pBest->infl);
	}
	else
		LsModeSetGoal(LS_GOAL_OFFER, at, 64.0f, pBest->ent, pBest->kind == LS_OFFER_TRAP ? 90 : 72, 20.0f, pBest->infl);
	LsWhy(pBest->kind == LS_OFFER_PERK ? "mode offer: a perk" : pBest->kind == LS_OFFER_BENCH ? "mode offer: the bench"
		: pBest->kind == LS_OFFER_BOX ? "mode offer: the box" : pBest->kind == LS_OFFER_TRAP ? "mode offer: the trap, gate under pressure"
		: "mode offer: placing a turret");
	return true;
}

// Knocked down (Last Stand, Battle Royale): the mode lays the player down - no-target, the view a
// hand above the floor - with only a pistol, at a crawl
bool CCSBot::LsModeDowned() const
{
	return LsModeIsDown(const_cast<CCSBot *>(this));
}

// A spot that's no place to crawl to: outside the safe zone, or with a creature close by
static bool LsModeBadCrawl(const Vector &spot)
{
	if (s_lsRing.on && LsModeDist2D(spot, s_lsRing.at) > s_lsRing.r - 100.0f)
		return true;
	for (int k = 0; k < s_lsTargetCount; k++)
		if (LsModeTargetOk(s_lsTarget[k]) && (s_lsTarget[k]->pev->origin - spot).IsLengthLessThan(300.0f))
			return true;
	return false;
}

// Down as a person is down: a teammate right by it (reviving) - still, not a crawl that breaks it;
// else, now and then, a crawl toward the nearest teammate who's up, or to cover close by - not into
// the open, the zone or creatures, and nothing if there's nowhere better. The pistol fights as usual
void CCSBot::LsModeDownTick()
{
	float now = gpGlobals->time;
	CBasePlayer *pMate = nullptr;
	float mateDist = 1500.0f;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		// (a teammate who's down too is no help)
		if (LsModeIsDown(pPlayer))
			return;
		float d = (pPlayer->pev->origin - pev->origin).Length();
		if (d < mateDist)
		{
			mateDist = d;
			pMate = pPlayer;
		}
	});
	if (pMate && mateDist < 90.0f)
	{
		m_lsModeStillUntil = now + 0.5f;
		if (m_lsWhy == nullptr || Q_strcmp(m_lsWhy, "mode down: still for the revive"))
			LsWhy("mode down: still for the revive");
		return;
	}
	if (IsAttacking() || now < m_lsModeDownAt || IsOnLadder() || !m_repathTimer.IsElapsed())
		return;
	m_lsModeDownAt = now + RANDOM_FLOAT(2.5f, 4.0f);
	Vector to;
	bool go = false;
	if (pMate && mateDist > 150.0f)
	{
		// (a stretch toward them: they'll come the rest of the way)
		to = LsModeOnMesh(pev->origin + LsModeDir2D(pev->origin, pMate->pev->origin) * Q_min(300.0f, mateDist - 100.0f));
		go = !LsModeBadCrawl(to);
	}
	if (!go && LsModePickSpot(pev->origin, 300.0f, &to))
		go = !LsModeBadCrawl(to);
	if (!go)
	{
		m_lsModeStillUntil = now + 1.0f;
		return;
	}
	LsModeMove(to, false);
	LsWhy(pMate ? "mode down: crawling toward a teammate" : "mode down: crawling to cover");
}

// A spot on high ground to hold: a hiding spot (one with a view - a sniper spot - likelier) or the
// middle of an open area, on the high layer, inside the zone, not where others are; near (the
// next along the route) when it's already up high
bool CCSBot::LsModePickHigh(Vector *out)
{
	LS_PROF("LsModePickHigh");
	bool onHigh = LsModeLayer(pev->origin) == 2;
	float reach = onHigh ? 1200.0f : 2500.0f;
	float bestScore = -1.0f;
	auto consider = [&](const Vector &pos, float bonus) {
		float d = (pos - pev->origin).Length();
		if (d > reach || (onHigh && d < 200.0f) || LsModeLayer(pos) != 2)
			return;
		if (s_lsRing.on && LsModeDist2D(pos, s_lsRing.at) > s_lsRing.r - 200.0f)
			return;
		int crowd = 0;
		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
			if (UTIL_IsValidPlayer(pPlayer) && pPlayer != this && pPlayer->IsAlive() && (pPlayer->pev->origin - pos).IsLengthLessThan(600.0f))
				crowd++;
		}
		float score = (bonus + RANDOM_FLOAT(0.0f, 0.6f)) / (1.0f + crowd) - d / 6000.0f;
		if (score > bestScore)
		{
			bestScore = score;
			*out = pos;
		}
	};
	for (auto area : TheNavAreaList)
	{
		const Vector *c = area->GetCenter();
		if ((*c - pev->origin).IsLengthGreaterThan(reach + 200.0f) || c->z < s_lsNavMidZ + 200.0f)
			continue;
		for (auto spot : *area->GetHidingSpotList())
			consider(*spot->GetPosition(), spot->IsGoodSniperSpot() ? 1.0f : (spot->HasGoodCover() ? 0.6f : 0.4f));
		if (area->GetSizeX() > 64.0f && area->GetSizeY() > 64.0f)
			consider(*c, 0.3f);
	}
	return bestScore >= 0.0f;
}

// High ground held like a person holds it: a sightline for a while (its patience: 20-60 s), watching
// and taking the shots there are, then the next spot along the high route - or off hunting, now
// and then. The zone, an empty gun and a fight take it off (true: holding or on its way)
bool CCSBot::LsModePerchTick()
{
	LS_PROF("LsModePerchTick");
	float now = gpGlobals->time;
	if (LsModeOursHiding() && now < m_lsModePerchUntil)
		return true;
	if (now < m_lsModePerchAt || LsOutOfAmmo() || !(IsHunting() || m_state == &m_idleState || LsModeOursHiding()))
		return false;
	m_lsModePerchAt = now + RANDOM_FLOAT(1.0f, 3.0f);
	if (!m_repathTimer.IsElapsed())
		return false;
	// (now and then a restless one goes looking for a fight instead)
	if (m_lsModePerchUntil > 0.0f && RANDOM_FLOAT(0.0f, 1.0f) < 0.15f + 0.3f * LsAggro())
	{
		m_lsModePerchUntil = 0.0f;
		m_lsModePerchAt = now + RANDOM_FLOAT(20.0f, 40.0f);
		return false;
	}
	Vector spot;
	if (!LsModePickHigh(&spot))
		return false;
	float patience = 0.5f * (1.0f - LsAggro()) + 0.5f * LsTrait(213);
	float hold = 20.0f + 40.0f * patience;
	float travel = (spot - pev->origin).Length() / 200.0f;
	Hide(&spot, hold + travel, false);
	if (!IsHiding())
		return false;
	m_lsModeHideState = m_stateTimestamp;
	m_lsModePerchUntil = now + hold + travel;
	LsWhy("mode high: holding a sightline");
	return true;
}

// Off to somewhere a mode says is worth it (a weapon spawner, a crate, a supply drop). Hunting
// with nothing better to do, now and then (the greedier, the likelier); gearing up or in a loot-up,
// every time. Spread out as people do: its own layer counts double, a place with others at it or
// heading to it counts for less, and loot outside the zone (or the next one, closing soon) isn't
// worth the trip
bool CCSBot::LsModeAttractTick(bool gearing)
{
	LS_PROF("LsModeAttractTick");
	float now = gpGlobals->time;
	if (!IsHunting() || now < m_lsModeAttractAt)
		return false;
	m_lsModeAttractAt = now + (gearing ? 1.5f : 3.0f);

	// (where the others are and are headed)
	Vector others[MAX_CLIENTS * 2];
	int nOthers = 0;
	for (int i = 1; i <= gpGlobals->maxClients && nOthers < MAX_CLIENTS * 2 - 1; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!UTIL_IsValidPlayer(pPlayer) || pPlayer == this || !pPlayer->IsAlive())
			continue;
		others[nOthers++] = pPlayer->pev->origin;
		if (pPlayer->IsBot())
		{
			CCSBot *pBot = static_cast<CCSBot *>(pPlayer);
			if (pBot->HasPath())
				others[nOthers++] = pBot->GetPathEndpoint();
		}
	}
	Vector ringAt = s_lsRing.at;
	float ringR = s_lsRing.on ? s_lsRing.r : 0.0f;
	if (s_lsRing.on && s_lsRing.next && s_lsRing.shrinkAt > 0.0f && s_lsRing.shrinkAt - now < 30.0f)
	{
		ringAt = s_lsRing.nextAt;
		ringR = s_lsRing.nextR;
	}
	int want = gearing ? LsModeLayerWant() : 0;

	const LsModeZone *pBest = nullptr;
	float bestScore = 0.0f;
	for (int i = 0; i < LS_MODE_ATTRACTS; i++)
	{
		LsModeZone &z = s_lsAttract[i];
		if (!LsModeLive(z) || (z.team == 1 && m_iTeam != TERRORIST) || (z.team == 2 && m_iTeam != CT))
			continue;
		float d = (z.at - pev->origin).Length();
		if (d < Q_max(z.r, 100.0f) || (ringR > 0.0f && LsModeDist2D(z.at, ringAt) > ringR - 100.0f))
			continue;
		float score = z.value / (1.0f + d / 1500.0f);
		if (gearing)
		{
			if (z.layer < 0)
				z.layer = LsModeLayer(z.at);
			// (its own layer counts double; one the mode told it, three times - and the others
			// hardly: a bot meant for below goes straight down from the trench mouth)
			int me = entindex();
			bool told = me >= 1 && me <= MAX_CLIENTS && s_lsLayer[me] >= 0;
			float sway = told ? LsModeSway(s_lsLayerInfl[me] - 1.0f) : 1.0f;
			if (want >= 0 && z.layer == want)
				score *= told ? 6.0f / (1.0f + sway) : 2.0f;
			else if (told)
				score *= 0.1f + 0.3f * sway;
			// (the bold ones go where it's richest; everyone else steers clear of the crowd)
			if (want >= 0)
			{
				int crowd = 0;
				for (int k = 0; k < nOthers; k++)
					if ((others[k] - z.at).IsLengthLessThan(700.0f))
						crowd++;
				score /= 1.0f + float(crowd);
			}
		}
		if (score > bestScore)
		{
			bestScore = score;
			pBest = &z;
		}
	}
	// (gearing up, the best worthwhile loot every time; otherwise a roll)
	if (!pBest || (!gearing && RANDOM_FLOAT(0.0f, 1.0f) >= Q_min(1.0f, bestScore) * LsModeGreed()))
		return false;
	LsModeMove(LsModeOnMesh(pBest->at), false);
	LsWhy(gearing ? "mode loot: its own pick" : "mode attract");
	return true;
}

// Each think, after a person's order: shoot a creature in sight, get out of a bad zone, then
// the most important goal it cares about. A fight comes first, then a person's order, then the
// mode; with nothing to do here, hunting may be pulled toward an attract
void CCSBot::LsModeTick()
{
	LS_PROF("LsModeTick");
	m_lsModePressed = 0;
	// (who it is, readable by plugins - the Studio's Bot Skill / Bot Trait getters: vuser2 = skill,
	// aggression, teamwork (0-1, what the decisions use); vuser3 = loot greed 0-1, playstyle (-1 none,
	// LS_STYLE_*), trait bits: 1 bold, 2 cautious, 4 team player, 8 loner, 16 greedy, 32 skilled, 64 novice)
	if (gpGlobals->time >= m_lsModeNatureAt)
	{
		m_lsModeNatureAt = gpGlobals->time + 1.0f;
		float skill = LsSkill(), aggro = LsAggro(), team = LsTeamwork(), greed = LsModeGreed();
		int bits = (aggro >= 0.65f ? 1 : 0) | (aggro <= 0.35f ? 2 : 0) | (team >= 0.6f ? 4 : 0) | (team <= 0.3f ? 8 : 0)
			| (greed >= 0.65f ? 16 : 0) | (skill >= 0.65f ? 32 : 0) | (skill <= 0.35f ? 64 : 0);
		pev->vuser2 = Vector(skill, aggro, team);
		pev->vuser3 = Vector(greed, float(LsStyle()), float(bits));
	}
	// (a peace ends any fight, also one carried over from before it in a state the rest of this
	// skips - a bot still in the buy state kept "attacking" from the round start, and set its
	// neighbours off when the fighting began)
	if (s_lsPeace && IsAttacking())
		StopAttacking();
	if (!s_lsModeOn || !IsAlive() || IsBuying() || (CSGameRules() && CSGameRules()->IsFreezePeriod()))
		return;
	// (testing: who it is, once a map, in the trace)
	if (m_lsModePersonaFor < 0.0f || m_lsModePersonaFor > gpGlobals->time)
	{
		m_lsModePersonaFor = gpGlobals->time;
		LsTracePersona(LsModeGreed());
	}

	// (a peace phase: no fights, room between players; then the goals - crates - as usual)
	if (s_lsPeace)
		LsModePeaceTick();
	// (gearing up after it: far noises aren't chased, the loot is)
	bool gearing = LsModeGearing();
	if (gearing && IsNoiseHeard() && (m_noisePosition - pev->origin).IsLengthGreaterThan(600.0f))
		ForgetNoise();
	// (the first minute of fighting - geared up or not - room from anyone who isn't its fight yet:
	// people spread out early)
	if (gearing || (!s_lsPeace && s_lsPeaceEndAt >= 0.0f && gpGlobals->time - s_lsPeaceEndAt < 60.0f && !IsAttacking()))
		LsModeRoomTick(300.0f);

	// (knocked down: crawling, shooting back, or lying still for a revive - nothing else)
	if (LsModeDowned())
	{
		LsModeShootTick();
		LsModeDownTick();
		return;
	}

	// (a player it's hunting, in sight: the fight is with them, unless someone closer is on it)
	LsModeHuntPrefer();

	bool shooting = LsModeShootTick();

	if (LsModeEscape())
		return;
	// (what the mode offers - a perk, the bench, the box, a turret - in a calm moment; the trap when a
	// gate's swamped, even mid-fight)
	if (!IsOnLadder() && (!shooting || s_lsTargetCount >= 5) && LsModeOfferTick())
		return;
	// (mid-fight with a creature: no new spot, no errand; it shoots)
	if (shooting)
		return;
	// (a healing item, at a moment of its own choosing - and still while it works)
	if (LsModeHealTick())
		return;
	// (Battle Royale: a squad mate down near by that the mode didn't send it to - it goes anyway, as
	// people do: a team player more readily)
	if (!s_lsPeace && s_lsRing.on && g_lsModeTies && gpGlobals->time >= m_lsModeMateDownAt)
	{
		m_lsModeMateDownAt = gpGlobals->time + RANDOM_FLOAT(1.5f, 3.0f);
		CBasePlayer *pDown = nullptr;
		float downDist = 1500.0f + 1000.0f * LsTeamwork();
		LsForTeammates(this, [&](CBasePlayer *pPlayer) {
			if (!LsModeIsDown(pPlayer))
				return;
			float d = (pPlayer->pev->origin - pev->origin).Length();
			if (d < downDist)
			{
				downDist = d;
				pDown = pPlayer;
			}
		});
		bool have = false;
		for (int i = 0; i < LS_MODE_GOALS && pDown; i++)
			if (m_lsModeGoal[i].kind == LS_GOAL_USE && m_lsModeGoal[i].ent == pDown->entindex())
				have = true;
		if (pDown && !have && !LsModeDowned() && RANDOM_FLOAT(0.0f, 1.0f) < 0.4f + 0.6f * LsTeamwork())
		{
			LsModeSetGoal(LS_GOAL_USE, pDown->pev->origin, 64.0f, pDown->entindex(), 75, 40.0f);
			LsWhy("mode revive: a squad mate down near by");
		}
	}
	// (Battle Royale: strayed well away from its squad - back toward them, over a mode's errand)
	// (gearing up too, a looser leash: squads loot near each other)
	if (!s_lsPeace && s_lsRing.on && (LsModeSquadTick(true, gearing) || (gpGlobals->time < m_lsModeSquadUntil && LsModeHeadedTo(m_lsModeMoveTo))))
		return;

	// the goal to act on: the most important it cares about (the nearer of two alike); spent
	// ones (run out, their entity gone) go
	float now = gpGlobals->time;
	int best = -1;
	float bestDist = 0.0f;
	for (int i = 0; i < LS_MODE_GOALS; i++)
	{
		LsModeGoal &g = m_lsModeGoal[i];
		if (g.kind == LS_GOAL_NONE)
			continue;
		if ((g.until > 0.0f && now >= g.until) || (g.ent > 0 && !g.thing.IsValid()))
		{
			LsModeClearGoals(g.kind, 0);
			continue;
		}
		if (!g.care)
			continue;
		float d = (g.at - pev->origin).Length();
		if (best < 0 || g.prio > m_lsModeGoal[best].prio || (g.prio == m_lsModeGoal[best].prio && d < bestDist))
		{
			best = i;
			bestDist = d;
		}
	}
	m_lsModeGoalOn = best;

	if (best < 0)
	{
		// (gearing up: off to loot rather than hunting)
		if ((gearing || s_lsPeace) && !shooting && LsModeAttractTick(true))
			return;
		// (Battle Royale after the gearing up: a fight between others near by - in on it or away
		// from it - and back toward its squad when it's strayed)
		if (!gearing && !s_lsPeace && s_lsRing.on && (LsModeThirdTick() || LsModeSquadTick(false, false)))
			return;
		// (geared up and a high-ground player: a sightline held a while, then the next along)
		if (!gearing && !s_lsPeace && s_lsPeaceEndAt >= 0.0f && !shooting && LsModeLayerWant() == 2 && LsModePerchTick())
			return;
		// (a creature mode's free players: roam like one)
		if (!shooting && !(s_lsClassCount > 0 && LsFree() && !m_isEnemyVisible && !IsOnLadder() && LsModeRoamTick()))
			LsModeAttractTick();
		return;
	}
	// (a fight, a person's order, a plugin's habit, the bomb: the goal waits)
	// (testing: a revive it holds and whether it's on it now, every 2 s)
	if (cv_bot_ls_trace.value > 0.0f && gpGlobals->time >= m_lsModeReviveHeldAt)
		for (int i = 0; i < LS_MODE_GOALS; i++)
		{
			const LsModeGoal &rg = m_lsModeGoal[i];
			CBaseEntity *pR = rg.ent > 0 ? (CBaseEntity *)rg.thing : nullptr;
			if (rg.kind != LS_GOAL_USE || !pR || !pR->IsPlayer())
				continue;
			m_lsModeReviveHeldAt = gpGlobals->time + 2.0f;
			static char note[160];
			Q_snprintf(note, sizeof(note), "revive-held d=%d care=%d chosen=%d free=%d enemyvis=%d atk=%d dist=%.0f state=%s", rg.ent, rg.care ? 1 : 0,
				best == i ? 1 : 0, LsFree() ? 1 : 0, m_isEnemyVisible ? 1 : 0, IsAttacking() ? 1 : 0, (pR->pev->origin - pev->origin).Length(),
				m_state ? m_state->GetName() : "-");
			LsTraceNote(note);
		}
	// (a revive right by the downed one goes on with an enemy in sight - whether that's a risk it
	// takes is the revive's own weighing, by who it is)
	const LsModeGoal &gb = m_lsModeGoal[best];
	CBaseEntity *pBestThing = gb.ent > 0 ? (CBaseEntity *)gb.thing : nullptr;
	// (Battle Royale's squads only: Last Stand's revives were right as they were - 40 % - and its
	// creatures are the fight a reviver mustn't turn its back on)
	bool reviveBest = (gb.kind == LS_GOAL_USE || gb.kind == LS_GOAL_HOLD) && pBestThing && pBestThing->IsPlayer() && s_lsRing.on;
	bool reviveClose = reviveBest && (pBestThing->pev->origin - pev->origin).IsLengthLessThan(300.0f);
	// (on its way to a squad mate bleeding out, for the vision: enemies a way off who aren't after it are
	// left be a moment - it keeps going; anyone close or shooting at it is a fight as usual)
	m_lsModeRevivingUntil = reviveBest ? gpGlobals->time + 1.0f : 0.0f;
	CBasePlayer *pFoe = GetEnemy();
	bool farFight = reviveBest && m_isEnemyVisible && pFoe && (pFoe->pev->origin - pev->origin).IsLengthGreaterThan(700.0f)
		&& GetTimeSinceAttacked() > 2.0f;
	if (farFight && IsAttacking())
	{
		StopAttacking();
		LsWhy("mode revive: leaving a far fight for them");
	}
	// (a fight it's lost sight of, with a squad mate bleeding out: it breaks off and goes to them)
	if (reviveBest && IsAttacking() && !m_isEnemyVisible && GetTimeSinceAttacked() > 2.0f && GetTimeSinceLastSawEnemy() > 1.5f)
	{
		StopAttacking();
		LsWhy("mode revive: breaking off a fight for them");
	}
	// (right by them, a fight on doesn't stop it either: whether it goes in now is the revive's weighing)
	if ((!LsFree() && !(reviveClose && IsAttacking())) || (m_isEnemyVisible && !reviveClose && !farFight) || IsOnLadder())
		return;
	if (LsModePushTick(m_lsModeGoal[best]))
		return;
	LsModeGoalTick(m_lsModeGoal[best]);
}

// Healing (Battle Royale's items: the mode says what each carries, ls_mode heals): when is its own
// call - the careful patch up early and soon after a fight, the bold fight on and heal only well
// hurt and once it's been quiet a while; not with an enemy in sight. It asks the mode to start
// one (ls_comp_heal) and stands still while it works (the mode says so: ls_mode heal)
bool CCSBot::LsModeHealTick()
{
	float now = gpGlobals->time;
	int me = entindex();
	if (now < m_lsModeHealUntil)
	{
		if (GetTimeSinceAttacked() < 0.5f)
		{
			m_lsModeHealUntil = 0.0f;	// (shot: the mode stops it, and it fights)
			return false;
		}
		m_lsModeStillUntil = now + 0.3f;
		return true;
	}
	if (me < 1 || me > MAX_CLIENTS || s_lsHeals[me] <= 0 || now < m_lsModeHealAt || IsAttacking() || m_isEnemyVisible
		|| IsOnLadder() || LsModeDowned())
		return false;
	// (a squad mate bleeding out comes first - unless it's badly hurt itself)
	if (LsHealth() >= 30.0f)
		for (int i = 0; i < LS_MODE_GOALS; i++)
		{
			const LsModeGoal &g = m_lsModeGoal[i];
			if (g.kind == LS_GOAL_USE && g.care && g.ent > 0 && g.ent <= gpGlobals->maxClients)
				return false;
		}
	m_lsModeHealAt = now + RANDOM_FLOAT(1.5f, 2.5f);
	float aggro = LsAggro();
	if (LsHealth() >= 50.0f + 40.0f * (1.0f - aggro) || GetTimeSinceAttacked() < 1.5f + 5.0f * aggro)
		return false;
	SERVER_COMMAND(UTIL_VarArgs("ls_comp_heal %d\n", me));
	m_lsModeHealAt = now + 4.0f;
	LsWhy("mode heal: patching up");
	return false;
}

// Battle Royale, a squad: back toward the nearest squad mate who's up when it's strayed - a team
// player keeps close, a loner wanders far (true: on its way)
bool CCSBot::LsModeSquadTick(bool overGoal, bool loose)
{
	float now = gpGlobals->time;
	// (over a goal: only a light one - a crate, a camping spot; a revive or anything that matters goes on)
	if (overGoal)
		for (int i = 0; i < LS_MODE_GOALS; i++)
			if (m_lsModeGoal[i].kind != LS_GOAL_NONE && m_lsModeGoal[i].care && m_lsModeGoal[i].prio >= 60)
			{
				m_lsModeSquadUntil = 0.0f;	// (and a walk back to the squad already on gives way)
				return false;
			}
	if (!g_lsModeTies || now < m_lsModeSquadAt || now < m_lsModePushUntil || IsOnLadder() || !m_repathTimer.IsElapsed())
		return false;
	m_lsModeSquadAt = now + RANDOM_FLOAT(3.0f, 5.0f);
	CBasePlayer *pMate = nullptr;
	float mateDist = 4000.0f;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		if (LsModeIsDown(pPlayer))
			return;
		float d = (pPlayer->pev->origin - pev->origin).Length();
		if (d < mateDist)
		{
			mateDist = d;
			pMate = pPlayer;
		}
	});
	// (a squad stays in reach of each other - a knocked one bleeds out in 45 s; over an errand of the
	// mode's own - a crate, a camping spot - only when it's strayed well beyond that)
	float leash = (300.0f + 900.0f * (1.0f - LsTeamwork())) * (overGoal ? 1.6f : 1.0f) * (loose ? 1.5f : 1.0f);
	if (!pMate || mateDist < leash)
		return false;
	Vector spot;
	if (!LsModePickSpot(pMate->pev->origin, 250.0f, &spot))
		spot = LsModeOnMesh(pMate->pev->origin);
	if (s_lsRing.on && LsModeDist2D(spot, s_lsRing.at) > s_lsRing.r - 100.0f)
		return false;
	LsModeMove(spot, mateDist > 1500.0f);
	m_lsModePushUntil = now + 6.0f;
	m_lsModeSquadUntil = overGoal ? now + 6.0f : 0.0f;
	LsWhy("mode squad: back to the squad");
	return true;
}

// Battle Royale: two others fighting near by (bots shooting at each other, both its enemies). The
// bold and the skilled go in on it - to cover on the near side, for the one left standing - the
// careful keep out of it, moving off if it's close; hurt, less keen (true: on its way)
bool CCSBot::LsModeThirdTick()
{
	float now = gpGlobals->time;
	if (now < m_lsModeThirdAt || now < m_lsModePushUntil || IsOnLadder() || !m_repathTimer.IsElapsed())
		return false;
	m_lsModeThirdAt = now + RANDOM_FLOAT(4.0f, 7.0f);
	Vector fight;
	float fightDist = 2000.0f;
	bool found = false;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!UTIL_IsValidPlayer(pPlayer) || pPlayer == this || !pPlayer->IsAlive() || !pPlayer->IsBot()
			|| BotRelationship(pPlayer) != BOT_ENEMY)
			continue;
		CCSBot *pA = static_cast<CCSBot *>(pPlayer);
		CBasePlayer *pB = pA->GetEnemy();
		if (!pA->IsAttacking() || !pB || pB == this || !pB->IsAlive() || BotRelationship(pB) != BOT_ENEMY)
			continue;
		Vector mid = (pA->pev->origin + pB->pev->origin) * 0.5f;
		float d = (mid - pev->origin).Length();
		if (d > 500.0f && d < fightDist)
		{
			fightDist = d;
			fight = mid;
			found = true;
		}
	}
	if (!found)
		return false;
	float aggro = LsAggro();
	float keen = 0.15f + 0.5f * aggro + 0.25f * LsSkill() - (LsHealth() < 75.0f ? 0.25f : 0.0f);
	if (RANDOM_FLOAT(0.0f, 1.0f) < keen)
	{
		Vector closeBy = fight + LsModeDir2D(fight, pev->origin) * 350.0f;
		Vector spot;
		if (!LsModePickSpot(closeBy, 300.0f, &spot))
			spot = LsModeOnMesh(closeBy);
		if (s_lsRing.on && LsModeDist2D(spot, s_lsRing.at) > s_lsRing.r - 100.0f)
			return false;
		LsModeMove(spot, aggro > 0.6f);
		m_lsModePushUntil = now + 10.0f;
		LsWhy("mode third party: in on a fight");
		return true;
	}
	if (fightDist < 1000.0f && aggro < 0.45f)
	{
		Vector away = LsModeOnMesh(pev->origin + LsModeDir2D(fight, pev->origin) * 500.0f);
		if (s_lsRing.on && LsModeDist2D(away, s_lsRing.at) > s_lsRing.r - 100.0f)
			return false;
		LsModeMove(away, false);
		m_lsModePushUntil = now + 6.0f;
		LsWhy("mode third party: keeping out of it");
		return true;
	}
	return false;
}

// Hunting a player down (a hunt goal: ent is the player) as a person would: toward where they are
// now, the route picked again as they move; the bold push right in, the careful stop at range - a
// covered spot their side of the target - and shoot from there (the influence scales that: at 0
// every bot goes straight for them). Others in the way are fought as usual (the goal waits for any
// fight); while the hunted one is dead, nothing (the plain brain plays on)
void CCSBot::LsModeHuntTick(LsModeGoal &g)
{
	float now = gpGlobals->time;
	CBaseEntity *pThing = g.ent > 0 ? (CBaseEntity *)g.thing : nullptr;
	if (!pThing || !pThing->IsPlayer() || pThing == this || !pThing->IsAlive())
		return;
	CBasePlayer *pTarget = static_cast<CBasePlayer *>(pThing);
	float d = (pTarget->pev->origin - pev->origin).Length();
	float aggro = LsAggro();
	float standoff = 150.0f + LsModeSway(g.infl) * 450.0f * (1.0f - aggro);
	// (in range, in sight: that's where it fights from - the fight is the stock brain's)
	if (d < standoff && IsVisible(pTarget, false))
	{
		m_lsModeStillUntil = now + 0.3f;
		SetLookAt("Loner-Strike: the hunted one", &pTarget->pev->origin, PRIORITY_MEDIUM, 0.5f);
		return;
	}
	// (on its way already, and they haven't gone far from where it's headed)
	bool moved = (pTarget->pev->origin - m_lsModeMoveTo).IsLengthGreaterThan(Q_max(250.0f, standoff * 0.5f));
	if (now < g.nextTry && LsModeHeadedTo(m_lsModeMoveTo) && !moved)
		return;
	if (!m_repathTimer.IsElapsed())
		return;
	g.nextTry = now + RANDOM_FLOAT(1.5f, 2.5f);
	Vector to = LsModeOnMesh(pTarget->pev->origin);
	if (standoff > 350.0f && d > standoff)
	{
		// (the careful: a covered spot at range on its side of them)
		Vector atRange = pTarget->pev->origin + LsModeDir2D(pTarget->pev->origin, pev->origin) * (0.8f * standoff);
		Vector spot;
		if (LsModePickSpot(atRange, 300.0f, &spot))
			to = spot;
	}
	m_lsModeMoveTo = to;
	MoveTo(&to, (aggro > 0.5f || LsModeSway(g.infl) <= 0.0f) ? FASTEST_ROUTE : SAFEST_ROUTE);
	m_lsModeMoveAt = m_stateTimestamp;
	LsWhy("mode hunt");
}

// The hunted one in sight (in its view): it turns on them - unless the enemy it's fighting is right
// on it (within 500 u and shooting at it); the less skilled notice a moment later
void CCSBot::LsModeHuntPrefer()
{
	float now = gpGlobals->time;
	if (now < m_lsModeHuntAt)
		return;
	m_lsModeHuntAt = now + 0.2f + 0.5f * (1.0f - LsSkill());
	CBasePlayer *pTarget = nullptr;
	for (int i = 0; i < LS_MODE_GOALS; i++)
	{
		const LsModeGoal &g = m_lsModeGoal[i];
		CBaseEntity *pThing = g.ent > 0 ? (CBaseEntity *)g.thing : nullptr;
		if (g.kind == LS_GOAL_HUNT && g.care && pThing && pThing->IsPlayer() && pThing != this && pThing->IsAlive()
			&& (g.until <= 0.0f || now < g.until) && now >= g.startAt)
		{
			pTarget = static_cast<CBasePlayer *>(pThing);
			m_lsModeHuntSway = LsModeSway(g.infl);
		}
	}
	// (who it's after, for the vision: others a way off are left be meanwhile)
	m_lsModeHunting = pTarget;
	m_lsModeHuntingUntil = pTarget ? now + 1.5f : 0.0f;
	if (!pTarget || GetEnemy() == pTarget || BotRelationship(pTarget) != BOT_ENEMY || (pTarget->pev->flags & FL_NOTARGET)
		|| IsOnLadder() || !IsVisible(pTarget, true))
		return;
	CBasePlayer *pNow = GetEnemy();
	if (pNow && pNow->IsAlive() && IsAttacking() && (pNow->pev->origin - pev->origin).IsLengthLessThan(500.0f)
		&& GetTimeSinceAttacked() < 2.0f)
		return;
	Attack(pTarget);
	LsWhy("mode hunt: the hunted one in sight");
}

// Where its side is based: the middle of the spots its teammates hold for the mode, or else of
// its living teammates (false: alone)
bool CCSBot::LsModeHome(Vector *home)
{
	LS_PROF("LsModeHome");
	Vector held(0, 0, 0), all(0, 0, 0);
	int nHeld = 0, nAll = 0;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		all = all + pPlayer->pev->origin;
		nAll++;
		if (!pPlayer->IsBot())
			return;
		CCSBot *pMate = static_cast<CCSBot *>(pPlayer);
		if (pMate->m_lsModeGoalOn >= 0)
		{
			const LsModeGoal &mg = pMate->m_lsModeGoal[pMate->m_lsModeGoalOn];
			if (mg.kind == LS_GOAL_DEFEND || mg.kind == LS_GOAL_HOLD)
			{
				held = held + mg.at;
				nHeld++;
			}
		}
	});
	if (nHeld)
		*home = held / float(nHeld);
	else if (nAll)
		*home = all / float(nAll);
	return nHeld || nAll;
}

// A free player in a creature mode (no goal of its own - the mode's holders have theirs) plays
// like a roaming person: back to the others when hurt, over to a teammate in a fight, out after
// the creatures (the bolder, the more), and with nothing on, round the area its side holds rather
// than across the map; between waves now and then it tags along with a person (true: sent)
bool CCSBot::LsModeRoamTick()
{
	LS_PROF("LsModeRoamTick");
	float now = gpGlobals->time;
	if (now < m_lsModeRoamAt || now < m_lsModePushUntil || IsFollowing())
		return false;
	m_lsModeRoamAt = now + RANDOM_FLOAT(2.0f, 4.0f);
	// (on an errand of its own already)
	if (LsModeHeadedTo(m_lsModeMoveTo))
		return false;

	Vector home;
	bool hasHome = LsModeHome(&home);
	float homeDist = hasHome ? LsModeDist2D(home, pev->origin) : 0.0f;
	bool quiet = s_lsTargetCount == 0;
	float aggro = LsAggro();

	// hurt and away from the others: back
	if (LsHealth() < 45.0f && hasHome && homeDist > 600.0f && !m_lsModeTarget.IsValid())
	{
		LsModeMove(LsModeOnMesh(home), true);
		m_lsModePushUntil = now + 8.0f;
		LsWhy("mode roam: hurt, back to the others");
		return true;
	}

	// a teammate fighting creatures a way off: over to help (the team players more often)
	CCSBot *pHelp = nullptr;
	float helpDist = 2500.0f;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		if (!pPlayer->IsBot())
			return;
		CCSBot *pMate = static_cast<CCSBot *>(pPlayer);
		float d = (pMate->pev->origin - pev->origin).Length();
		if (now - pMate->m_lsModeSawAt < 1.5f && d > 700.0f && d < helpDist)
		{
			helpDist = d;
			pHelp = pMate;
		}
	});
	if (pHelp && now - m_lsModeSawAt > 3.0f && RANDOM_FLOAT(0.0f, 1.0f) < 0.3f + 0.5f * LsTeamwork())
	{
		LsModeMove(LsModeOnMesh(pHelp->pev->origin), true);
		m_lsModePushUntil = now + 10.0f;
		LsWhy("mode roam: helping a teammate");
		return true;
	}

	// creatures about, none in sight: after the nearest (its own way there: the safest route
	// goes round, often from the side)
	if (!quiet && now - m_lsModeSawAt > 3.0f && LsHealth() >= 50.0f && RANDOM_FLOAT(0.0f, 1.0f) < 0.2f + 0.6f * aggro)
	{
		CBaseEntity *pNearest = nullptr;
		float nearest = 2500.0f;
		for (int k = 0; k < s_lsTargetCount; k++)
		{
			CBaseEntity *pEntity = s_lsTarget[k];
			if (!LsModeTargetOk(pEntity) || (hasHome && LsModeDist2D(pEntity->pev->origin, home) > 3000.0f))
				continue;
			float d = (pEntity->pev->origin - pev->origin).Length();
			if (d < nearest)
			{
				nearest = d;
				pNearest = pEntity;
			}
		}
		if (pNearest)
		{
			m_lsModeMoveTo = LsModeOnMesh(pNearest->pev->origin);
			MoveTo(&m_lsModeMoveTo, SAFEST_ROUTE);
			m_lsModeMoveAt = m_stateTimestamp;
			m_lsModePushUntil = now + 10.0f;
			LsWhy("mode roam: hunting");
			return true;
		}
	}

	// between waves, now and then: with a person for a bit
	if (quiet && RANDOM_FLOAT(0.0f, 1.0f) < 0.1f * LsTeamwork())
	{
		CBasePlayer *pHuman = nullptr;
		LsForTeammates(this, [&](CBasePlayer *pPlayer) {
			if (!pHuman && !pPlayer->IsBot() && (pPlayer->pev->origin - pev->origin).IsLengthLessThan(1200.0f))
				pHuman = pPlayer;
		});
		if (pHuman)
		{
			Follow(pHuman);
			m_lsModePushUntil = now + RANDOM_FLOAT(12.0f, 25.0f);
			LsWhy("mode roam: tagging along");
			return true;
		}
	}

	// nothing on: round the area its side holds (a hunt would take it across the map), the
	// restless ones further and more often
	if (hasHome && (IsHunting() || m_state == &m_idleState) && (homeDist > 1600.0f || RANDOM_FLOAT(0.0f, 1.0f) < 0.35f + 0.4f * aggro))
	{
		Vector spot;
		if (LsModePickSpot(home, 700.0f + 800.0f * aggro, &spot))
		{
			LsModeMove(spot, false);
			LsWhy("mode roam: patrol");
			return true;
		}
	}
	return false;
}

// Defending with nothing in sight for a while but creatures about: the bolder, healthy ones now
// and then go out to the nearest one rather than wait for it (not off a capture or an extract,
// not far from what they're defending)
bool CCSBot::LsModePushTick(const LsModeGoal &g)
{
	LS_PROF("LsModePushTick");
	float now = gpGlobals->time;
	if (now < m_lsModePushAt || (g.kind != LS_GOAL_DEFEND && g.kind != LS_GOAL_HOLD) || s_lsTargetCount == 0
		|| m_lsModeTarget.IsValid() || now - m_lsModeSawAt < 4.0f || LsHealth() < 70.0f || IsFollowing())
		return false;
	m_lsModePushAt = now + RANDOM_FLOAT(5.0f, 9.0f);
	if (RANDOM_FLOAT(0.0f, 1.0f) >= LsModeSway(g.infl) * (0.1f + 0.35f * LsAggro()))
		return false;
	float leash = 2.0f * g.radius + 1200.0f;
	CBaseEntity *pNearest = nullptr;
	float nearest = 1800.0f;
	for (int k = 0; k < s_lsTargetCount; k++)
	{
		CBaseEntity *pEntity = s_lsTarget[k];
		if (!LsModeTargetOk(pEntity) || LsModeDist2D(pEntity->pev->origin, g.at) > leash)
			continue;
		float d = (pEntity->pev->origin - pev->origin).Length();
		if (d < nearest)
		{
			nearest = d;
			pNearest = pEntity;
		}
	}
	if (!pNearest)
		return false;
	LsModeMove(LsModeOnMesh(pNearest->pev->origin), false);
	m_lsModePushUntil = now + 12.0f;
	LsWhy("mode push out");
	return true;
}

// Acting on one goal: on its way there (sent back on its way if something else took it off),
// then what the kind does there. A spot it can't get to rests a while before it tries again
void CCSBot::LsModeGoalTick(LsModeGoal &g)
{
	LS_PROF("LsModeGoalTick");
	float now = gpGlobals->time;
	Vector to = g.at - pev->origin;
	float dist = to.Make2D().Length();
	bool there = dist < Q_max(g.radius * 0.8f, 48.0f) && Q_fabs(to.z) < 100.0f;
	bool hurry = g.prio >= 80 || g.kind == LS_GOAL_EXTRACT || g.kind == LS_GOAL_FLEE
		|| (g.kind == LS_GOAL_USE && g.ent > 0 && g.thing.IsValid() && ((CBaseEntity *)g.thing)->IsPlayer());	// (a teammate bleeding out)
	// (a moment before it gets going: the less skilled react later)
	if (now < g.startAt)
		return;
	if (g.kind == LS_GOAL_HUNT)
	{
		LsModeHuntTick(g);
		return;
	}

	// running from it: far enough is done
	if (g.kind == LS_GOAL_FLEE)
	{
		if (dist > g.radius)
			return;
		Vector away = g.at + LsModeDir2D(g.at, pev->origin) * (g.radius + 300.0f);
		away.z = pev->origin.z;
		if (now >= g.nextTry && LsModeMove(LsModeOnMesh(away), true))
		{
			g.nextTry = now + 2.0f;
			LsWhy("mode flee");
		}
		return;
	}

	// riding a platform to the goal: wait for it level with the floor, step on, stand still
	if (g.kind == LS_GOAL_RIDE)
	{
		if (there)
		{
			LsModeClearGoals(LS_GOAL_RIDE, 0);
			return;
		}
		CBaseEntity *pPlat = g.ent > 0 ? (CBaseEntity *)g.thing : nullptr;
		if (pPlat)
		{
			if (pev->groundentity == pPlat->edict())
			{
				m_lsModeStillUntil = now + 0.5f;
				return;
			}
			Vector top = pPlat->Center();
			top.z = pPlat->pev->absmax.z;
			float platDist = LsModeDist2D(top, pev->origin);
			bool level = Q_fabs(top.z - pev->absmin.z) < 24.0f && pPlat->pev->velocity.IsLengthLessThan(20.0f);
			if (platDist < 400.0f)
			{
				// (close: on it when it's here and still, else wait facing it)
				if (level)
				{
					m_lsModeStepTo = top;
					m_lsModeStepUntil = now + 0.5f;
				}
				else
					m_lsModeStillUntil = now + 0.5f;
				SetLookAt("Loner-Strike: the platform", &top, PRIORITY_LOW, 1.0f);
				return;
			}
			// (far: to the floor by where the platform is now)
			if (now >= g.nextTry && LsModeMove(LsModeOnMesh(top), hurry))
				g.nextTry = now + 2.0f;
			return;
		}
		// (no platform named: just go there)
	}

	// a crate to open, a button to press, a downed teammate to revive (a hold on a player): close,
	// it edges up to it (the path ends against it), looks at it and holds use (the mode clears
	// the goal once it's open, or they're up)
	CBaseEntity *pGoalThing = g.ent > 0 ? (CBaseEntity *)g.thing : nullptr;
	bool revive = (g.kind == LS_GOAL_HOLD || g.kind == LS_GOAL_USE) && pGoalThing && pGoalThing->IsPlayer();
	// (a downed one crawls: the way there follows where they lie now)
	if (revive && (pGoalThing->pev->origin - g.onMesh).IsLengthGreaterThan(100.0f) && LsModeIsDown(pGoalThing))
	{
		g.at = pGoalThing->pev->origin;
		g.onMesh = LsModeOnMesh(g.at);
		g.nextTry = 0.0f;
	}
	// (up again, or gone: nothing to revive)
	if (revive && g.kind == LS_GOAL_USE && (!pGoalThing->IsAlive() || !LsModeIsDown(pGoalThing)))
	{
		LsModeClearGoals(g.kind, g.ent);
		return;
	}
	// (a revive as people do it: not into a crowd. What's round the downed one: creatures close by,
	// enemy players near with a line on them; the bolder go in with one about, the careful wait;
	// a long way off with anyone about, not at all; a smoke on them first if it has one; hit
	// mid-revive and hurt, or with several about, it backs off a moment and fights. A must-do
	// one (prio 90) goes anyway)
	int reviveThreat = 0;
	if (revive)
	{
		const Vector mate = pGoalThing->pev->origin;
		for (int k = 0; k < s_lsTargetCount; k++)
		{
			CBaseEntity *pEntity = s_lsTarget[k];
			if (!LsModeTargetOk(pEntity))
				continue;
			float d = (pEntity->pev->origin - mate).Length();
			reviveThreat += (d < 250.0f) ? 2 : (d < 450.0f ? 1 : 0);
		}
		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
			// (a downed enemy - flagged no-target - is no threat: knocks in a duel are often both ways)
			if (!UTIL_IsValidPlayer(pPlayer) || !pPlayer->IsAlive() || pPlayer == pGoalThing || BotRelationship(pPlayer) != BOT_ENEMY
				|| LsModeIsDown(pPlayer))
				continue;
			float d = (pPlayer->pev->origin - mate).Length();
			if (d > 900.0f)
				continue;
			TraceResult tr;
			UTIL_TraceLine(pPlayer->pev->origin + pPlayer->pev->view_ofs, mate + Vector(0, 0, 8), ignore_monsters, pPlayer->edict(), &tr);
			// (one busy fighting someone else is less of a worry: close by, one; a way off, none)
			bool busy = pPlayer->IsBot() && static_cast<CCSBot *>(pPlayer)->IsAttacking() && static_cast<CCSBot *>(pPlayer)->GetEnemy()
				&& static_cast<CCSBot *>(pPlayer)->GetEnemy() != this && static_cast<CCSBot *>(pPlayer)->GetEnemy() != pGoalThing;
			if (tr.flFraction == 1.0f || tr.pHit == pGoalThing->edict())
				reviveThreat += busy ? (d < 350.0f ? 1 : 0) : (d < 350.0f ? 2 : 1);
		}
		// (its own smoke on them a moment ago: they can't see in)
		if (now - m_lsModeSmokeAt < 12.0f && (m_lsModeSmokeOn - mate).IsLengthLessThan(200.0f))
			reviveThreat = 0;
	}
	float sway = LsModeSway(g.infl);
	// (testing: how a revive goes, every 2 s)
	if (revive && now >= m_lsModeReviveNoteAt)
	{
		m_lsModeReviveNoteAt = now + 2.0f;
		static char note[160];
		Q_snprintf(note, sizeof(note), "revive-state d=%d dist=%.0f threat=%d phase=%d atk=%d vis=%d hp=%.0f brave=%.2f", pGoalThing->entindex(),
			(pGoalThing->pev->origin - pev->origin).Length(), reviveThreat, g.phase, IsAttacking() ? 1 : 0, m_isEnemyVisible ? 1 : 0,
			LsHealth(), 0.5f * LsAggro() + 0.5f * LsTeamwork());
		LsTraceNote(note);
	}
	if (revive && g.prio < 90 && sway > 0.0f)
	{
		// (one enemy a way off is a risk most take; the bold take one close by, or two further off;
		// with its personality fully in charge, the careful take none and the boldest three)
		// (bravery: the bold and the team players go in under more; a careful loner waits)
		float brave = 0.5f * LsAggro() + 0.5f * LsTeamwork();
		int tolerance = sway >= 1.5f ? int(0.5f + 2.5f * brave) : int(0.6f + 2.4f * brave);
		float away = (pGoalThing->pev->origin - pev->origin).Length();
		// (waited long with them bleeding out: one watching from a way off is a risk the braver take
		// now - from cover by them, never into more than that)
		if (m_lsModeReviveFor != g.ent)
		{
			m_lsModeReviveFor = g.ent;
			m_lsModeReviveWaitFrom = now;
		}
		if (reviveThreat > tolerance && reviveThreat <= tolerance + 1 && reviveThreat <= 2 && away < 450.0f
			&& now - m_lsModeReviveWaitFrom > 16.0f - 10.0f * brave && LsHealth() >= 50.0f)
			tolerance = reviveThreat;
		if (g.phase == 1 && GetTimeSinceAttacked() < 1.0f && (LsHealth() < 50.0f || reviveThreat >= 2))
		{
			g.phase = 0;
			m_lsModeReviveBackUntil = now + RANDOM_FLOAT(3.0f, 6.0f);
			LsWhy("mode revive: under fire, backing off");
			return;
		}
		if (now < m_lsModeReviveBackUntil)
			return;
		if (reviveThreat > tolerance || (reviveThreat > 0 && away > 1600.0f))
		{
			// (a smoke on them, if it has one and isn't too far to throw it)
			if (away < 1200.0f && LsHasNade(WEAPON_SMOKEGRENADE) && now - m_lsModeSmokeAt > 15.0f && !IsAttacking())
			{
				m_lsModeSmokeAt = now;
				m_lsModeSmokeOn = pGoalThing->pev->origin;
				LsAskThrow("weapon_smokegrenade", pGoalThing->pev->origin);
				LsWhy("mode revive: smoke on them first");
				return;
			}
			// (not from far off; nearer, up to cover by them - where the one who downed them is in
			// sight to deal with - then the revive once it's clear)
			if (away > 1600.0f)
				return;
			if (away > 450.0f && now >= g.nextTry && m_repathTimer.IsElapsed())
			{
				g.nextTry = now + 4.0f;
				Vector cover;
				if (LsModePickSpot(pGoalThing->pev->origin, 400.0f, &cover))
				{
					LsModeMove(cover, false);
					LsWhy("mode revive: up to cover by them");
					return;
				}
			}
			// (the label says how much is about: 1 one enemy watching from a way off, 2 one close or two)
			static const char *const s_wait[] = { "mode revive: waiting for it to clear (1)", "mode revive: waiting for it to clear (2)",
				"mode revive: waiting for it to clear (3+)" };
			const char *why = s_wait[Q_min(2, Q_max(1, reviveThreat) - 1)];
			if (m_lsWhy != why)
				LsWhy(why);
			return;
		}
	}
	if (g.kind == LS_GOAL_LOOT || g.kind == LS_GOAL_USE || g.kind == LS_GOAL_OFFER || revive)
	{
		Vector at = g.at;
		CBaseEntity *pThing = g.ent > 0 ? (CBaseEntity *)g.thing : nullptr;
		if (pThing)
			at = pThing->Center();
		Vector toThing = at - pev->origin;
		float gap = toThing.Make2D().Length();
		bool handy = gap < 120.0f && Q_fabs(toThing.z) < 100.0f;
		// (a revive needs it right by them - within 80 u, origin to origin, for 4 s: close in, stay)
		bool brRevive = revive && s_lsRing.on;
		if (brRevive)
			handy = (pGoalThing->pev->origin - pev->origin).Length() < 70.0f;
		if (g.phase == 1)
		{
			if (brRevive && !handy && now < g.phaseUntil && (pGoalThing->pev->origin - pev->origin).IsLengthLessThan(200.0f))
			{
				m_lsModeStepTo = pGoalThing->pev->origin;
				m_lsModeStepUntil = now + 0.3f;
				return;
			}
			if (now >= g.phaseUntil || !handy)
			{
				LsModeClearGoals(g.kind, 0);
				return;
			}
			if (gap > (brRevive ? 30.0f : 48.0f))
			{
				m_lsModeStepTo = at;
				m_lsModeStepUntil = now + 0.3f;
			}
			else
				m_lsModeStillUntil = now + 0.3f;
			// (reviving: it stays on it - the fight it would turn to waits, unless it's shot: above)
			if (brRevive && IsAttacking())
				StopAttacking();
			SetLookAt("Loner-Strike: using", &at, PRIORITY_HIGH, 0.5f);
			// (a repair or a shop errand that suddenly cost real money - USE held on with the mystery box in
			// front of it rolled it twice in a second: off it)
			if (g.kind != LS_GOAL_OFFER && m_lsModeUseMoney >= 0 && m_iAccount < m_lsModeUseMoney - 400)
			{
				LsModeClearGoals(g.kind, 0);
				LsWhy("mode use: spent by mistake, off it");
				return;
			}
			// (bought: done - holding USE on at the box bought it again a second later)
			if (g.kind == LS_GOAL_OFFER)
			{
				if (m_lsModeOfferMoney < 0)
					m_lsModeOfferMoney = m_iAccount;
				else if (m_iAccount <= m_lsModeOfferMoney - Q_max(1, m_lsModeOfferCost / 2))
				{
					LsModeClearGoals(g.kind, 0);
					LsWhy("mode offer: bought");
					return;
				}
			}
			// (the bench upgrades the gun in hand: the main gun out first)
			if (g.kind == LS_GOAL_OFFER && m_lsModeOfferKind == LS_OFFER_BENCH && m_rgpPlayerItems[PRIMARY_WEAPON_SLOT]
				&& !(m_pActiveItem && m_pActiveItem == m_rgpPlayerItems[PRIMARY_WEAPON_SLOT]))
				EquipBestWeapon(true);
			UseEnvironment();
			m_lsModePressed |= 2;
			return;
		}
		if (handy)
		{
			// (a revive: the gun down - it can't shoot and get them up at once)
			if (brRevive && IsAttacking())
				StopAttacking();
			g.phase = 1;
			m_lsModeUseMoney = m_iAccount;
			// (a revive or a crate as long as the goal lasts, at most 15 s; a plain use - a shop, a
			// button - a few seconds)
			g.phaseUntil = now + (g.ent > 0 ? 15.0f : 6.0f);
			if (g.ent > 0 && g.until > 0.0f)
				g.phaseUntil = Q_min(g.phaseUntil, g.until);
			g.tries = 0;
			LsWhy("mode use");
			return;
		}
		// (close: straight up to where they lie now - they may have crawled from where they fell)
		if (brRevive && (pGoalThing->pev->origin - pev->origin).IsLengthLessThan(300.0f) && Q_fabs(toThing.z) < 60.0f)
		{
			if (IsAttacking())
				StopAttacking();
			m_lsModeStepTo = pGoalThing->pev->origin;
			m_lsModeStepUntil = now + 0.3f;
			return;
		}
		// (far: on its way, below)
	}

	// a spot to stand in an area, picked again now and then (hold, defend, capture, extract,
	// a meeting)
	if (g.kind == LS_GOAL_HOLD || g.kind == LS_GOAL_DEFEND || g.kind == LS_GOAL_CAPTURE || g.kind == LS_GOAL_EXTRACT
		|| g.kind == LS_GOAL_MEETING)
	{
		LsModeStationTick(g, dist);
		return;
	}

	// a look at a spot: there is done (the hunt it goes back to looks round)
	if (there)
	{
		LsModeClearGoals(g.kind, 0);
		return;
	}

	// on its way (again, if something took it off); a spot it can't get to rests a while
	if (now < g.nextTry || LsModeHeadedTo(g.onMesh))
		return;
	// (in cover a moment after being shot at: it waits)
	if (IsHiding() && GetTimeSinceAttacked() < 5.0f)
		return;
	if (++g.tries > 3)
	{
		g.tries = 0;
		g.nextTry = now + 15.0f;
		return;
	}
	g.nextTry = now + 2.0f;
	LsModeMove(g.onMesh, hurry);
	LsWhy("mode goal");
}

// The hide it's in is the one a station gave it (nothing since has changed its state)
bool CCSBot::LsModeOursHiding() const
{
	LS_PROF("LsModeOursHiding");
	return IsHiding() && m_lsModeHideState >= 0.0f && Q_fabs(m_stateTimestamp - m_lsModeHideState) < 0.05f;
}

// The spot it's standing at for a station, for its teammates picking theirs (null: none)
const Vector *CCSBot::LsModeSpot() const
{
	return (m_lsModeSpotOn && const_cast<CCSBot *>(this)->IsAlive() && LsModeOursHiding()) ? &m_lsModeSpot : nullptr;
}

// Somewhere to stand in an area: a hiding spot (or a nav area's middle) inside it, away from the
// spots teammates have and where they stand, not the one it was just at, somewhat at random, a
// covered one a little likelier, a near one a little likelier (false: nothing in the area)
bool CCSBot::LsModePickSpot(const Vector &center, float radius, Vector *out)
{
	LS_PROF("LsModePickSpot");
	Vector taken[MAX_CLIENTS];
	int nTaken = 0;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		const Vector *spot = pPlayer->IsBot() ? static_cast<CCSBot *>(pPlayer)->LsModeSpot() : nullptr;
		if (nTaken < MAX_CLIENTS)
			taken[nTaken++] = spot ? *spot : pPlayer->pev->origin;
	});

	float bestScore = -1.0f;
	float skill = LsSkill();
	auto consider = [&](const Vector &pos, bool cover) {
		if (LsModeDist2D(pos, center) > radius || Q_fabs(pos.z - center.z) > 250.0f)
			return;
		if (LsModeDist2D(pos, pev->origin) < 96.0f || (m_lsModeSpotOn && LsModeDist2D(pos, m_lsModeSpot) < 64.0f))
			return;
		float apart = 500.0f;
		for (int i = 0; i < nTaken; i++)
			apart = Q_min(apart, LsModeDist2D(pos, taken[i]));
		// (the skilled pick the covered spots, the rest more by chance)
		float score = apart / 500.0f + (cover ? 0.15f + 0.4f * skill : 0.0f) + RANDOM_FLOAT(0.0f, 0.25f + 0.5f * (1.0f - skill))
			- LsModeDist2D(pos, pev->origin) / 3000.0f;
		if (score > bestScore)
		{
			bestScore = score;
			*out = pos;
		}
	};
	for (auto area : TheNavAreaList)
	{
		const Vector *c = area->GetCenter();
		if (LsModeDist2D(*c, center) > radius + 150.0f || Q_fabs(c->z - center.z) > 300.0f)
			continue;
		const HidingSpotList *spots = area->GetHidingSpotList();
		for (auto spot : *spots)
			consider(*spot->GetPosition(), spot->HasGoodCover());
		// (open floor counts too, the bigger areas: a defence isn't only corners)
		if (area->GetSizeX() > 48.0f && area->GetSizeY() > 48.0f)
			consider(*c, false);
	}
	return bestScore >= 0.0f;
}

// Standing guard in an area like a person would: a spot of its own (not a teammate's), held a
// while (crouched, looking round the ways in, reacting to noises - the hide state), then another
// spot. Something of its own nearby (a noise to look into, a teammate's fight, following someone,
// pushing out) goes on while it's within the leash; a capture or extract keeps it in the area.
// Between waves (nothing to shoot alive) it moves round more and sometimes tags along with a
// person
void CCSBot::LsModeStationTick(LsModeGoal &g, float dist)
{
	LS_PROF("LsModeStationTick");
	float now = gpGlobals->time;
	bool tight = g.kind == LS_GOAL_CAPTURE || g.kind == LS_GOAL_EXTRACT || g.kind == LS_GOAL_MEETING;
	float leash = tight ? g.radius : 2.0f * g.radius + 300.0f;
	bool ours = LsModeOursHiding();
	bool quiet = s_lsClassCount > 0 && s_lsTargetCount == 0;
	g.phase = dist < g.radius ? 1 : 0;

	if (dist < leash)
	{
		// (its own business nearby - a noise, a fight to join, cover after being hurt: let it)
		if (now < m_lsModePushUntil || IsFollowing() || (!ours && !IsHunting() && m_state != &m_idleState))
			return;
		// (holding its spot: the hide state does the rest)
		if (ours && now < g.phaseUntil)
			return;
	}
	if (now < g.nextTry)
		return;
	// (the brain paths at most every half second; a hide asked for sooner fails its path and
	// drops the bot to idle - which paths again: the freeze. Next think, then)
	if (!m_repathTimer.IsElapsed())
		return;

	float aggro = LsAggro(), sway = LsModeSway(g.infl);
	float hold = quiet ? RANDOM_FLOAT(5.0f, 9.0f) : RANDOM_FLOAT(5.0f, 9.0f) * (1.0f + sway * (0.6f * (1.0f - aggro) - 0.3f));
	if (LsStyle() == LS_STYLE_CAMPER && !quiet)
		hold *= 1.0f + 0.5f * sway;

	// between waves, now and then: off with a person on its side for a bit
	if (quiet && !tight && dist < leash && RANDOM_FLOAT(0.0f, 1.0f) < 0.25f * sway * LsTeamwork())
	{
		CBasePlayer *pHuman = nullptr;
		LsForTeammates(this, [&](CBasePlayer *pPlayer) {
			if (!pHuman && !pPlayer->IsBot() && (pPlayer->pev->origin - pev->origin).IsLengthLessThan(1200.0f))
				pHuman = pPlayer;
		});
		if (pHuman)
		{
			Follow(pHuman);
			m_lsModePushUntil = now + RANDOM_FLOAT(12.0f, 25.0f);
			LsWhy("mode station: tagging along");
			return;
		}
	}

	Vector spot;
	bool found = LsModePickSpot(g.at, tight ? 0.8f * g.radius : g.radius, &spot);
	if (found)
	{
		// (the walk there counts against the hide's time: added on)
		Hide(&spot, hold + dist / 180.0f, tight);
		if (IsHiding())
		{
			m_lsModeSpot = spot;
			m_lsModeSpotOn = true;
		}
	}
	else
	{
		CNavArea *pArea = TheNavAreaGrid.GetNearestNavArea(&g.at);
		if (pArea)
			Hide(pArea, hold + dist / 180.0f, tight ? 0.8f * g.radius : g.radius, tight);
		m_lsModeSpotOn = false;
	}

	if (IsHiding())
	{
		m_lsModeHideState = m_stateTimestamp;
		g.phaseUntil = now + hold + dist / 180.0f;
		g.tries = 0;
		// (a hide that ends at once - its path failed on the way, the bot dropped to idle - isn't
		// tried again for a moment: a crowd at the gates otherwise had every bot pathing twice a
		// second, the most costly thing the bots did in Last Stand)
		g.nextTry = now + 1.5f;
		LsWhy("mode station");
		return;
	}
	// (no way there now: the plain brain carries on a few seconds before another try)
	g.nextTry = now + 3.0f;
	if (++g.tries > 3)
	{
		g.tries = 0;
		g.nextTry = now + 15.0f;
	}
	LsWhy("mode station: no spot");
}

// After the state machine each think: a stand-still or a step the mode's goal asked for this
// moment (on a moving platform the hide's spot would walk it off)
void CCSBot::LsModePostUpdate()
{
	LS_PROF("LsModePostUpdate");
	float now = gpGlobals->time;
	bool strafe = now < m_lsModeStrafeUntil && !IsOnLadder();
	bool still = now < m_lsModeStillUntil && !strafe, step = now < m_lsModeStepUntil, back = now < m_lsModeBackUntil && !IsOnLadder();
	if (IsAttacking() || !(still || step || back || strafe))
		return;
	// (ClearMovement resets the whole command, buttons too: the trigger and use the mode pressed
	// this think are pressed again - the stand-still is for shooting straight, the step for using)
	ClearMovement();
	if (strafe)
	{
		if (m_lsModeStrafeLeft)
			StrafeLeft();
		else
			StrafeRight();
	}
	else if (!still && step)
		MoveTowardsPosition(&m_lsModeStepTo);
	else if (!still && back)
		MoveAwayFromPosition(&m_lsModeBackFrom);
	if (m_lsModePressed & 1)
		PrimaryAttack();
	if (m_lsModePressed & 2)
		UseEnvironment();
	if (m_lsModePressed & 4)
		Reload();
	ResetStuckMonitor();
}

// ---- the command ----------------------------------------------------------------------------

static void LsModeStatus()
{
	float now = gpGlobals->time;
	CONSOLE_ECHO("ls_mode '%s'%s\n", s_lsModeId, s_lsModeOn ? "" : " (nothing for the bots)");
	for (int i = 0; i < LS_MODE_ZONES; i++)
	{
		const LsModeZone &z = s_lsAvoid[i];
		if (LsModeLive(z))
			CONSOLE_ECHO("  %s %s at %.0f %.0f %.0f r %.0f cost %.1f, %.0fs left (0: no end)\n", z.box ? "block" : "avoid", z.tag, z.at.x, z.at.y, z.at.z, z.r, z.value,
				z.until > 0.0f ? z.until - now : 0.0f);
	}
	for (int i = 0; i < LS_MODE_ATTRACTS; i++)
	{
		const LsModeZone &z = s_lsAttract[i];
		if (LsModeLive(z))
			CONSOLE_ECHO("  attract %s at %.0f %.0f %.0f r %.0f weight %.2f team %d\n", z.tag, z.at.x, z.at.y, z.at.z, z.r, z.value, z.team);
	}
	if (s_lsRing.on)
	{
		CONSOLE_ECHO("  zone at %.0f %.0f %.0f r %.0f\n", s_lsRing.at.x, s_lsRing.at.y, s_lsRing.at.z, s_lsRing.r);
		if (s_lsRing.next)
			CONSOLE_ECHO("  next zone at %.0f %.0f %.0f r %.0f in %.0fs\n", s_lsRing.nextAt.x, s_lsRing.nextAt.y, s_lsRing.nextAt.z, s_lsRing.nextR, s_lsRing.shrinkAt - now);
	}
	for (int c = 0; c < s_lsClassCount; c++)
		CONSOLE_ECHO("  enemyclass %s threat %.1f\n", s_lsClass[c].name, s_lsClass[c].threat);
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!UTIL_IsValidPlayer(pPlayer))
			continue;
		if (s_lsRole[i][0] || s_lsFriend[i] || s_lsFoe[i])
			CONSOLE_ECHO("  %d %s: role '%s' friends %08x foes %08x\n", i, STRING(pPlayer->pev->netname), s_lsRole[i], s_lsFriend[i], s_lsFoe[i]);
		if (!pPlayer->IsBot())
			continue;
		CCSBot *pBot = static_cast<CCSBot *>(pPlayer);
		for (int g = 0; g < CCSBot::LS_MODE_GOALS; g++)
		{
			const CCSBot::LsModeGoal &goal = pBot->LsModeGoalAt(g);
			if (goal.kind != LS_GOAL_NONE)
				CONSOLE_ECHO("  %d %s: %s at %.0f %.0f %.0f r %.0f prio %d influence %d%s%s\n", i, STRING(pPlayer->pev->netname), s_lsGoalNames[goal.kind],
					goal.at.x, goal.at.y, goal.at.z, goal.radius, goal.prio, goal.infl < 0.0f ? -1 : int(goal.infl * 100.0f + 0.5f),
					goal.care ? "" : " (not taken up)", goal.phase ? " (there)" : "");
		}
	}
}

void Bot_LsModeCommand()
{
	const char *what = CMD_ARGV(1);
	int who[MAX_CLIENTS], who2[MAX_CLIENTS];
	s_lsArgBad = false;

	if (FStrEq(what, "reset"))
	{
		LsModeReset(FStrEq(CMD_ARGV(2), "all"));
		return;
	}
	if (FStrEq(what, "set"))
	{
		if (FStrEq(CMD_ARGV(2), "off") || !CMD_ARGV(2)[0])
			LsModeReset(true);
		else
			Q_strlcpy(s_lsModeId, CMD_ARGV(2));
		return;
	}
	if (FStrEq(what, "status"))
	{
		LsModeStatus();
		return;
	}
	// nobuy <1 | 0>: nothing to buy in this mode - bots don't wait in the buy state
	if (FStrEq(what, "nobuy"))
	{
		s_lsNoBuy = Q_atoi(CMD_ARGV(2)) != 0;
		return;
	}
	// phase <loot | peace | fight | off>: a peace (nobody can be hurt: a loot-up) - no player is
	// an enemy; its end starts the fight, each bot noticing a moment later
	if (FStrEq(what, "phase") || FStrEq(what, "noenemies"))
	{
		const char *phase = CMD_ARGV(2);
		bool peace = FStrEq(what, "noenemies") ? Q_atoi(phase) != 0 : (FStrEq(phase, "loot") || FStrEq(phase, "peace"));
		// (phase <phase> [influence]: how much each bot's own pace counts - when it notices the
		// fight is on, how long it gears up)
		s_lsPhaseInfl = LsModeInflArg(3) + 1.0f;
		if (s_lsPeace && !peace)
			s_lsPeaceEndAt = gpGlobals->time;
		s_lsPeace = peace;
		if (peace)
			s_lsModeOn = true;
		return;
	}

	// down <who> <1 | 0>: knocked down (crawling, waiting for a revive) or up again (also on death and
	// a new round, from the mode)
	if (FStrEq(what, "down"))
	{
		bool down = Q_atoi(CMD_ARGV(3)) != 0;
		int n = LsModeWho(CMD_ARGV(2), who, false);
		for (int i = 0; i < n; i++)
			s_lsDown[who[i]] = down;
		return;
	}
	// heals <who> <bandages> <medkits> <drinks>: the healing items a player carries (Battle Royale)
	if (FStrEq(what, "heals"))
	{
		int n = LsModeWho(CMD_ARGV(2), who, false);
		int total = Q_atoi(CMD_ARGV(3)) + Q_atoi(CMD_ARGV(4)) + Q_atoi(CMD_ARGV(5));
		for (int i = 0; i < n; i++)
			s_lsHeals[who[i]] = total;
		return;
	}
	// heal <who> <kind | -> [secs]: a heal started (it stands still that long) or stopped
	if (FStrEq(what, "heal"))
	{
		bool stop = FStrEq(CMD_ARGV(3), "-") || !CMD_ARGV(3)[0];
		float secs = Q_atof(CMD_ARGV(4));
		int n = LsModeWho(CMD_ARGV(2), who, true);
		for (int i = 0; i < n; i++)
			static_cast<CCSBot *>(UTIL_PlayerByIndex(who[i]))->m_lsModeHealUntil = stop ? 0.0f : gpGlobals->time + Q_min(Q_max(secs, 1.0f), 15.0f);
		return;
	}

	// hunt <who> <target> [prio] [until] [influence]: a hunt goal on that player (the same as goal
	// <who> hunt 0 0 0 <target> [prio] [until] 0 [influence])
	if (FStrEq(what, "hunt"))
	{
		int target = Q_atoi(CMD_ARGV(3));
		CBasePlayer *pTarget = (target >= 1 && target <= gpGlobals->maxClients) ? UTIL_PlayerByIndex(target) : nullptr;
		if (!pTarget || !UTIL_IsValidPlayer(pTarget))
		{
			CONSOLE_ECHO("ls_mode hunt <who> <target player index> [prio] [until] [influence 0-100]\n");
			return;
		}
		int prio = int(LsModeArg(4, 60.0f));
		float secs = LsModeArg(5), infl = LsModeInflArg(6);
		if (LsModeBadArgs())
			return;
		int n = LsModeWho(CMD_ARGV(2), who, true);
		for (int i = 0; i < n; i++)
			if (who[i] != target)
				static_cast<CCSBot *>(UTIL_PlayerByIndex(who[i]))->LsModeSetGoal(LS_GOAL_HUNT, pTarget->pev->origin, 0.0f, target, prio, secs, infl);
		if (n > 0)
			s_lsModeOn = true;
		return;
	}

	// goal <who> <kind> <x> <y> <z> [ent] [prio] [until] [radius] [influence]
	if (FStrEq(what, "goal"))
	{
		int kind = LsModeGoalKind(CMD_ARGV(3));
		if (kind == LS_GOAL_NONE || CMD_ARGC() < 7)
		{
			CONSOLE_ECHO("ls_mode goal <who> <kind> <x> <y> <z> [ent] [prio] [until] [radius] [influence 0-100]\n"
				"  kinds: go_hold defend capture extract loot use ride investigate flee meeting hunt (ent: the player)\n");
			return;
		}
		Vector at(LsModeArg(4), LsModeArg(5), LsModeArg(6));
		int ent = int(LsModeArg(7)), prio = int(LsModeArg(8, 50.0f));
		float secs = LsModeArg(9), radius = LsModeArg(10);
		float infl = LsModeInflArg(11);
		if (LsModeBadArgs())
			return;
		radius = radius > 0.0f ? LsModeRadius(radius, 16.0f) : 0.0f;
		int n = LsModeWho(CMD_ARGV(2), who, true);
		for (int i = 0; i < n; i++)
			static_cast<CCSBot *>(UTIL_PlayerByIndex(who[i]))->LsModeSetGoal(kind, at, radius, ent, prio, secs, infl);
		if (n > 0)
			s_lsModeOn = true;
		return;
	}
	// offer <who> <perk|bench|box|trap|turret> <x> <y> <z> <ent> <cost> [order]: something the bot may
	// use (the same ent or kind and position again replaces it); offer_clear <who> [kind | ent]
	if (FStrEq(what, "offer") || FStrEq(what, "offer_clear"))
	{
		const char *kn = CMD_ARGV(3);
		int kind = FStrEq(kn, "perk") ? LS_OFFER_PERK : FStrEq(kn, "bench") ? LS_OFFER_BENCH : FStrEq(kn, "box") ? LS_OFFER_BOX
			: FStrEq(kn, "trap") ? LS_OFFER_TRAP : FStrEq(kn, "turret") ? LS_OFFER_TURRET : LS_OFFER_NONE;
		int n = LsModeWho(CMD_ARGV(2), who, true);
		if (FStrEq(what, "offer_clear"))
		{
			int ent = kind ? 0 : Q_atoi(kn);
			for (int i = 0; i < n; i++)
				for (int k = 0; k < LS_MODE_OFFERS; k++)
				{
					LsModeOffer &o = s_lsOffer[who[i]][k];
					if (o.kind != LS_OFFER_NONE && (!CMD_ARGV(3)[0] || (kind && o.kind == kind) || (ent && o.ent == ent)))
						o = LsModeOffer();
				}
			return;
		}
		Vector at(LsModeArg(4), LsModeArg(5), LsModeArg(6));
		int ent = int(LsModeArg(7)), cost = int(LsModeArg(8)), order = int(LsModeArg(9));
		float infl = LsModeInflArg(10);
		if (!kind || CMD_ARGC() < 9 || LsModeBadArgs())
		{
			CONSOLE_ECHO("ls_mode offer <who> <perk|bench|box|trap|turret> <x> <y> <z> <ent> <cost> [order] [influence 0-100]\n");
			return;
		}
		for (int i = 0; i < n; i++)
		{
			LsModeOffer *pSlot = nullptr;
			for (int k = 0; k < LS_MODE_OFFERS && !pSlot; k++)
			{
				LsModeOffer &o = s_lsOffer[who[i]][k];
				if (o.kind == kind && (ent ? o.ent == ent : (o.at - at).IsLengthLessThan(32.0f)))
					pSlot = &o;
			}
			for (int k = 0; k < LS_MODE_OFFERS && !pSlot; k++)
				if (s_lsOffer[who[i]][k].kind == LS_OFFER_NONE)
					pSlot = &s_lsOffer[who[i]][k];
			if (!pSlot)
				continue;
			float nextTry = pSlot->kind == kind ? pSlot->nextTry : 0.0f;
			*pSlot = LsModeOffer();
			pSlot->kind = kind;
			pSlot->at = at;
			pSlot->ent = ent;
			pSlot->cost = cost;
			pSlot->order = order;
			pSlot->nextTry = nextTry;
			pSlot->infl = infl;
		}
		if (n > 0)
			s_lsModeOn = true;
		return;
	}
	// goal_clear <who> [kind | ent]
	if (FStrEq(what, "goal_clear"))
	{
		int kind = LsModeGoalKind(CMD_ARGV(3));
		int ent = kind ? 0 : Q_atoi(CMD_ARGV(3));
		int n = LsModeWho(CMD_ARGV(2), who, true);
		for (int i = 0; i < n; i++)
			static_cast<CCSBot *>(UTIL_PlayerByIndex(who[i]))->LsModeClearGoals(kind, ent);
		return;
	}
	// avoid <tag> <x> <y> <z> <r> <cost> [until]
	// attract <tag> <x> <y> <z> <r> <weight> [until] [team]
	if (FStrEq(what, "avoid") || FStrEq(what, "attract"))
	{
		bool avoid = FStrEq(what, "avoid");
		if (CMD_ARGC() < 8)
		{
			CONSOLE_ECHO("ls_mode %s <tag> <x> <y> <z> <r> <%s> [until]%s\n", what, avoid ? "cost" : "weight", avoid ? "" : " [team]");
			return;
		}
		Vector at(LsModeArg(3), LsModeArg(4), LsModeArg(5));
		float r = LsModeRadius(LsModeArg(6), 16.0f), value = LsModeArg(7), secs = LsModeArg(8);
		int team = avoid ? 0 : int(LsModeArg(9));
		if (LsModeBadArgs())
			return;
		LsModeZone *z = LsModeFindTag(avoid ? s_lsAvoid : s_lsAttract, CMD_ARGV(2), true);
		if (!z)
			return;
		z->at = at;
		z->r = r;
		z->value = value;
		z->until = LsModeUntil(secs);
		z->team = team;
		s_lsModeOn = true;
		return;
	}
	// forget <tag>
	if (FStrEq(what, "forget"))
	{
		if (LsModeZone *z = LsModeFindTag(s_lsAvoid, CMD_ARGV(2), false))
			*z = LsModeZone();
		if (LsModeZone *z = LsModeFindTag(s_lsAttract, CMD_ARGV(2), false))
			*z = LsModeZone();
		return;
	}
	// block <ent> <until>: the door's bounds are a no-go zone till then (0: open again)
	if (FStrEq(what, "block"))
	{
		int ent = Q_atoi(CMD_ARGV(2));
		char tag[24];
		Q_snprintf(tag, sizeof(tag), "block%d", ent);
		float secs = LsModeArg(3);
		if (LsModeBadArgs())
			return;
		edict_t *pEdict = ent > 0 ? INDEXENT(ent) : nullptr;
		if (secs <= 0.0f || !pEdict || pEdict->free)
		{
			if (LsModeZone *z = LsModeFindTag(s_lsAvoid, tag, false))
				*z = LsModeZone();
			return;
		}
		LsModeZone *z = LsModeFindTag(s_lsAvoid, tag, true);
		if (!z)
			return;
		z->box = true;
		z->lo = pEdict->v.absmin;
		z->hi = pEdict->v.absmax;
		z->at = (z->lo + z->hi) * 0.5f;
		z->value = -1.0f;
		z->until = LsModeUntil(secs);
		s_lsModeOn = true;
		return;
	}
	// zone <cx> <cy> <cz> <r> [<ncx> <ncy> <ncz> <nr> <shrink_at>]
	if (FStrEq(what, "zone"))
	{
		if (CMD_ARGC() < 6)
		{
			CONSOLE_ECHO("ls_mode zone <cx> <cy> <cz> <r> [<ncx> <ncy> <ncz> <nr> <shrink_at>]\n");
			return;
		}
		LsModeRing ring;
		ring.on = true;
		ring.at = Vector(LsModeArg(2), LsModeArg(3), LsModeArg(4));
		ring.r = LsModeRadius(LsModeArg(5), 64.0f);
		// (the next ring needs all five: its centre, radius and when it shrinks)
		if (CMD_ARGC() >= 11)
		{
			ring.next = true;
			ring.nextAt = Vector(LsModeArg(6), LsModeArg(7), LsModeArg(8));
			ring.nextR = LsModeRadius(LsModeArg(9), 64.0f);
			ring.shrinkAt = LsModeUntil(LsModeArg(10));
		}
		if (LsModeBadArgs())
			return;
		s_lsRing = ring;
		s_lsModeOn = true;
		return;
	}
	if (FStrEq(what, "zone_clear"))
	{
		s_lsRing = LsModeRing();
		return;
	}
	// layer <who> <below | surface | high | ->: where the mode wants the player to loot and stay
	if (FStrEq(what, "layer"))
	{
		const char *name = CMD_ARGV(3);
		int layer = FStrEq(name, "below") ? 1 : FStrEq(name, "high") ? 2 : FStrEq(name, "surface") ? 0 : -1;
		float infl = LsModeInflArg(4);
		int n = LsModeWho(CMD_ARGV(2), who, false);
		for (int i = 0; i < n; i++)
		{
			s_lsLayer[who[i]] = layer;
			s_lsLayerInfl[who[i]] = infl + 1.0f;
		}
		return;
	}
	// role <who> <role | ->
	if (FStrEq(what, "role"))
	{
		const char *role = FStrEq(CMD_ARGV(3), "-") ? "" : CMD_ARGV(3);
		int n = LsModeWho(CMD_ARGV(2), who, false);
		for (int i = 0; i < n; i++)
			Q_strlcpy(s_lsRole[who[i]], role);
		return;
	}
	// friend <who> <who2> [0 | 1], foe <who> <who2> [0 | 1]
	if (FStrEq(what, "friend") || FStrEq(what, "foe"))
	{
		bool on = LsModeArg(4, 1.0f) != 0.0f;
		if (LsModeBadArgs())
			return;
		unsigned int *ties = FStrEq(what, "friend") ? s_lsFriend : s_lsFoe;
		unsigned int *other = FStrEq(what, "friend") ? s_lsFoe : s_lsFriend;
		int n = LsModeWho(CMD_ARGV(2), who, false);
		int n2 = LsModeWho(CMD_ARGV(3), who2, false);
		for (int i = 0; i < n; i++)
		{
			for (int j = 0; j < n2; j++)
			{
				if (who[i] == who2[j])
					continue;
				unsigned int bit = 1u << (who2[j] - 1);
				if (on)
				{
					ties[who[i]] |= bit;
					other[who[i]] &= ~bit;	// (a friend isn't a foe as well)
				}
				else
					ties[who[i]] &= ~bit;
			}
		}
		g_lsModeTies = false;
		for (int i = 1; i <= MAX_CLIENTS && !g_lsModeTies; i++)
			g_lsModeTies = s_lsFriend[i] || s_lsFoe[i];
		return;
	}
	// enemyclass <classname> [threat]
	if (FStrEq(what, "enemyclass"))
	{
		const char *name = CMD_ARGV(2);
		float threat = LsModeArg(3, 1.0f);
		if (LsModeBadArgs())
			return;
		if (!name[0])
			return;
		int c = 0;
		while (c < s_lsClassCount && !FStrEq(s_lsClass[c].name, name))
			c++;
		if (threat <= 0.0f)
		{
			if (c < s_lsClassCount)
				s_lsClass[c] = s_lsClass[--s_lsClassCount];
		}
		else if (c < s_lsClassCount || s_lsClassCount < LS_MODE_CLASSES)
		{
			if (c == s_lsClassCount)
				s_lsClassCount++;
			Q_strlcpy(s_lsClass[c].name, name);
			s_lsClass[c].threat = threat;
			s_lsModeOn = true;
		}
		s_lsTargetsAt = 0.0f;
		return;
	}
	if (FStrEq(what, "enemyclass_clear"))
	{
		s_lsClassCount = 0;
		s_lsTargetCount = 0;
		return;
	}

	CONSOLE_ECHO("ls_mode: reset set status phase nobuy goal hunt goal_clear offer offer_clear avoid attract forget block zone zone_clear layer role friend foe enemyclass enemyclass_clear heals heal\n");
}
