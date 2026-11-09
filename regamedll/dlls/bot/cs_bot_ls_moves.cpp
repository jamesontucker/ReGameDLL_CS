/*
*
*   Loner-Strike additions to the CZ bots: fighting technique and movement: jiggle peeks, wide swings, off-angles, crouch taps, prefires, retreats, ladders, drops, boxes.
*   (cs_bot_ls.cpp has the cvars and the rest; the habits here follow the same rules: bot_ls_fight,
*   LsSolo / LsMatch for the mode, LsTrait for each bot's own leanings)
*
*   All of it is off with bot_ls_fight 0. Traits used here: 60-71.
*
*   - corners (LsCornerMoves): a blind corner ahead gets one of
*       a jiggle peek: a quick strafe out and back to bait a shot, before going round for real
*         (good players, on a pub or in a match)
*       a wide swing: round it at a run on the far side, so the angle opens all at once
*         (the bold ones, deathmatch most)
*       the outside line: kept off the corner's edge on the way in, so whoever's past it sees a
*         sliver of the bot at a time and from further off (careful players)
*   - off-angles (LsOffAngle): holding an area, some pick a spot off to the side of where an
*     enemy coming in would be looking, not the one straight ahead of them
*   - crouch taps (LsCrouchTapTime): a crouch in a fight is mostly a tap; only bots that
*     crouch a lot hold one now and then
*   - prefiring (LsPrefire): a hiding spot coming into view up close gets a short burst
*     (the bold or good ones, rarely; never with a teammate near it)
*   - falling back (LsFallBack): eyes stay on the threat (good players) or glance back at it,
*     instead of turning their back
*   - scoping (LsScopesClose): newer snipers scope in, and keep the rifle, with an enemy right
*     there; better ones don't (the stock pistol switch)
*   - ladders (LsLadderClimb, LsLadderYaw): the diagonal climb (turned on the ladder, W and a
*     strafe), about 40% faster, mostly good players
*   - silent drops (LsQuietDrop): sneaking, a small drop is walked off (quiet) rather than
*     jumped (the landing would be heard)
*   - panic (LsAttackBegins, LsPanicUpdate): newer pub players caught by surprise hold walk or
*     crouch at the wrong moment
*
*   Crouch-jumping up onto boxes and ledges: the stock bots already duck while jumping up
*   (DiscontinuityJump / m_isJumpCrouching), so there's nothing to add. Boosting isn't done:
*   see the notes at the end.
*
*/

#include "precompiled.h"

static const char LS_LOOKBACK[] = "Loner-Strike: looking back";
static const char LS_PREFIRE[] = "Loner-Strike: prefire";

// Any teammate within this range of a spot, or in the line from the eye to it
static bool LsTeammateNear(CCSBot *me, const Vector &spot, float range)
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!IsEntityValid(pPlayer) || pPlayer == me || !pPlayer->IsAlive() || me->IsEnemy(pPlayer))
			continue;
		if ((pPlayer->pev->origin - spot).IsLengthLessThan(range))
			return true;
	}
	TraceResult tr;
	Vector eye = me->pev->origin + me->pev->view_ofs;
	UTIL_TraceLine(eye, spot, dont_ignore_monsters, ENT(me->pev), &tr);
	if (tr.flFraction < 1.0f && tr.pHit)
	{
		CBaseEntity *pHit = CBaseEntity::Instance(tr.pHit);
		if (pHit && pHit->IsPlayer())
			return true;
	}
	return false;
}

// ---- corners -----------------------------------------------------------------------------

enum { LS_CORNER_PLAIN = 0, LS_CORNER_JIGGLE, LS_CORNER_WIDE, LS_CORNER_OUTSIDE, LS_CORNER_DONE };

// Free to play a corner: nothing more important going on, and nothing that makes a strafe
// or a detour a bad idea (a ladder, a jump, a crouch-through, a teammate in the way)
static bool LsCornerFree(CCSBot *me)
{
	if (me->IsAttacking() || me->IsUsingLadder() || me->IsOnLadder() || me->IsBlind() || me->IsSafe()
		|| !(me->pev->flags & FL_ONGROUND) || me->IsJumping() || me->IsCrouching() || me->IsReloading()
		|| me->IsHurrying() || me->GetHostageEscortCount() > 0 || me->IsDefusingBomb() || me->IsThrowingGrenade()
		|| me->GetEnemiesRemaining() <= 0 || me->LsFallingBack())
		return false;
	const CNavArea *area = me->GetLastKnownArea();
	if (area && (area->GetAttributes() & (NAV_JUMP | NAV_CROUCH)))
		return false;
	return true;
}

// What this bot does at this corner (rolled once per corner)
int CCSBot::LsPickCornerPlan()
{
	float skill = LsSkill();
	float aggro = LsAggro();
	bool sneaking = !IsRunning();

	// a jiggle: good players, a pub or a match (deathmatch is run and gun)
	float jiggle = 0.0f;
	if (!LsSolo() && skill >= 0.5f && LsTrait(60) < 0.75f)
		jiggle = (0.15f + 0.35f * skill) * (LsMatch() ? 1.4f : 1.0f);

	// a wide swing: the bold ones, at a run
	float wide = 0.0f;
	if (!sneaking && (aggro > 0.5f || LsSolo()) && LsTrait(61) < 0.6f)
		wide = (0.1f + 0.3f * aggro) * (LsSolo() ? 1.3f : 1.0f);

	// the outside line: careful players (most of them in a match)
	float outside = 0.0f;
	if (skill > 0.4f && (aggro < 0.65f || LsMatch()) && LsTrait(63) < 0.7f)
		outside = (0.2f + 0.3f * skill + (LsMatch() ? 0.15f : 0.0f)) * (LsSolo() ? 0.4f : 1.0f);

	float r = RANDOM_FLOAT(0.0f, 1.0f);
	if (r < jiggle)
		return LS_CORNER_JIGGLE;
	r -= jiggle;
	if (r < wide)
		return LS_CORNER_WIDE;
	r -= wide;
	if (r < outside)
		return LS_CORNER_OUTSIDE;
	return LS_CORNER_PLAIN;
}

// Room to step off the path at the corner by up to "want" units in direction "dir": a body
// fits all the way, with floor under it (not off a ledge) on the nav mesh. How far it can go
static float LsCornerRoom(CCSBot *me, const Vector &corner, const Vector2D &dir, float want)
{
	Vector from(corner.x, corner.y, corner.z + HalfHumanHeight + 1.0f);
	Vector to = from + Vector(dir.x, dir.y, 0.0f) * want;
	TraceResult tr;
	UTIL_TraceHull(from, to, ignore_monsters, human_hull, ENT(me->pev), &tr);
	if (tr.fStartSolid)
		return 0.0f;
	float room = want * tr.flFraction - 8.0f;
	if (room < 32.0f)
		return 0.0f;

	Vector at = from + Vector(dir.x, dir.y, 0.0f) * room;
	UTIL_TraceLine(at, at - Vector(0, 0, HalfHumanHeight + 24.0f), ignore_monsters, ENT(me->pev), &tr);
	if (tr.flFraction >= 1.0f)
		return 0.0f;
	if (!TheNavAreaGrid.GetNavArea(&tr.vecEndPos))
		return 0.0f;
	return room;
}

// A blind corner coming up on the path: a jiggle peek, a wide swing or the outside line (or
// nothing special), the bot's own choice each corner. Moves the goal it's walking to for a
// swing or a line; true while a jiggle is moving the bot itself
bool CCSBot::LsCornerMoves(Vector *goal)
{
	if (!LsFight() || !HasPath())
		return false;
	float now = gpGlobals->time;

	// a jiggle under way: out, back (and maybe once more)
	if (now < m_lsJiggleUntil)
	{
		if (!LsCornerFree(this))
		{
			m_lsJiggleUntil = 0.0f;
			return false;
		}
		if (now >= m_lsJigglePhaseEnd)
		{
			if (m_lsJigglePhase == 0)
			{
				// back a touch further than out, to be properly behind cover again
				m_lsJigglePhase = 1;
				m_lsJigglePhaseEnd = now + RANDOM_FLOAT(0.15f, 0.25f);
			}
			else if (--m_lsJiggleLeft > 0)
			{
				m_lsJigglePhase = 0;
				m_lsJigglePhaseEnd = now + RANDOM_FLOAT(0.1f, 0.2f);
			}
			else
			{
				// done: now the real peek
				m_lsJiggleUntil = 0.0f;
				return false;
			}
		}
		Vector2D way = (m_lsJigglePhase == 0) ? m_lsJiggleOut : -m_lsJiggleOut;
		Vector to = pev->origin + Vector(way.x, way.y, 0.0f) * 100.0f;
		MoveTowardsPosition(&to);
		SetLookAt(LS_PEEK, &m_lsJiggleLook, PRIORITY_LOW, 0.5f, true);
		ResetStuckMonitor();
		return true;
	}

	if (!LsCornerFree(this) || m_isWaitingBehindFriend || m_pathIndex >= m_pathLength - 1)
		return false;

	// the corner: the path's next point, where it turns
	Vector corner = m_goalPosition;
	Vector2D in = (corner - pev->origin).Make2D();
	float dist = float(in.Length());
	if (dist < 8.0f || dist > 320.0f)
		return false;
	in = in / dist;

	// the way on past it (not round a ladder, a jump or a crouch-through)
	Vector past;
	bool found = false;
	for (int i = m_pathIndex + 1; i < m_pathLength; i++)
	{
		if (m_path[i].ladder || (m_path[i].area->GetAttributes() & (NAV_JUMP | NAV_CROUCH)))
			return false;
		if ((m_path[i].pos - corner).Make2D().IsLengthGreaterThan(160.0f))
		{
			past = m_path[i].pos;
			found = true;
			break;
		}
	}
	if (!found)
		return false;
	Vector2D out = (past - corner).Make2D();
	out.NormalizeInPlace();
	if (float(DotProduct(in, out)) > 0.5f)
		return false;

	// a new corner: what to do at this one
	if ((corner - m_lsCornerAt).Make2D().IsLengthGreaterThan(48.0f))
	{
		m_lsCornerAt = corner;
		m_lsCornerPlan = LsPickCornerPlan();
		m_lsCornerShift = Vector2D(0, 0);
		m_lsCornerBlind = false;
		if (m_lsCornerPlan == LS_CORNER_WIDE || m_lsCornerPlan == LS_CORNER_OUTSIDE)
		{
			// the outside of the turn: away from the corner's edge
			float turn = in.x * out.y - in.y * out.x;
			Vector2D left(-in.y, in.x);
			Vector2D away = (turn > 0.0f) ? -left : left;
			float want = (m_lsCornerPlan == LS_CORNER_WIDE) ? RANDOM_FLOAT(64.0f, 110.0f) : RANDOM_FLOAT(40.0f, 80.0f);
			float room = LsCornerRoom(this, corner, away, want);
			if (room > 0.0f)
				m_lsCornerShift = away * room;
			else
				m_lsCornerPlan = LS_CORNER_PLAIN;
		}
	}
	else
		m_lsCornerAt = corner;		// (the same corner, the point drifting a little)

	if (m_lsCornerPlan == LS_CORNER_PLAIN || m_lsCornerPlan == LS_CORNER_DONE)
		return false;

	// nothing to jiggle for once it can already see past the corner (a swing or a line,
	// once started, carries on round it)
	Vector eye = pev->origin + pev->view_ofs;
	Vector head = past + Vector(0, 0, HalfHumanHeight + 24.0f);
	TraceResult tr;
	if (m_lsCornerPlan == LS_CORNER_JIGGLE || !m_lsCornerBlind)
	{
		UTIL_TraceLine(eye, head, ignore_monsters, ENT(pev), &tr);
		if (tr.flFraction >= 1.0f)
		{
			m_lsCornerPlan = LS_CORNER_DONE;
			return false;
		}
		m_lsCornerBlind = true;
	}

	if (m_lsCornerPlan == LS_CORNER_JIGGLE)
	{
		if (dist > 80.0f || now < m_lsNextJiggle)
			return false;
		m_lsCornerPlan = LS_CORNER_DONE;
		m_lsNextJiggle = now + RANDOM_FLOAT(3.0f, 8.0f);
		m_lsJiggleOut = out;
		m_lsJiggleLook = head;
		m_lsJigglePhase = 0;
		m_lsJigglePhaseEnd = now + RANDOM_FLOAT(0.1f, 0.2f);
		// (the better ones sometimes do it twice)
		m_lsJiggleLeft = (LsSkill() > 0.75f && RANDOM_FLOAT(0.0f, 1.0f) < 0.35f) ? 2 : 1;
		m_lsJiggleUntil = now + 1.2f;
		LsWhy("jiggle");
		Vector to = pev->origin + Vector(out.x, out.y, 0.0f) * 100.0f;
		MoveTowardsPosition(&to);
		SetLookAt(LS_PEEK, &m_lsJiggleLook, PRIORITY_LOW, 0.5f, true);
		ResetStuckMonitor();
		return true;
	}

	// a wide swing starts further out, at a run; the outside line just keeps off the edge
	if (dist < 30.0f || (m_lsCornerPlan == LS_CORNER_WIDE && dist > 260.0f))
		return false;
	Vector shifted = *goal + Vector(m_lsCornerShift.x, m_lsCornerShift.y, 0.0f);
	UTIL_TraceHull(pev->origin, Vector(shifted.x, shifted.y, pev->origin.z), ignore_monsters, human_hull, ENT(pev), &tr);
	if (tr.flFraction < 1.0f || tr.fStartSolid)
		return false;
	*goal = shifted;
	if (m_lsCornerPlan == LS_CORNER_WIDE && !IsRunning())
		Run();
	return false;
}

// ---- crouching in a fight ---------------------------------------------------------------

// Players tap crouch in a fight (a fraction of a second, for the first shots); only the ones
// who crouch a lot hold one now and then
float CCSBot::LsCrouchTapTime()
{
	if (LsCrouchFactor() >= 2.0f && RANDOM_FLOAT(0.0f, 1.0f) < 0.35f)
		return RANDOM_FLOAT(0.6f, 1.1f);
	return RANDOM_FLOAT(0.15f, 0.4f);
}

// ---- off-angles ---------------------------------------------------------------------------

// The hiding spots with cover near where it's looking for one, free of players
class LsSpotCollector
{
public:
	LsSpotCollector(CBaseEntity *me, const Vector *origin, float range) : m_me(me), m_origin(origin), m_range(range), m_count(0) {}
	enum { MAX_SPOTS = 64 };

	bool operator()(CNavArea *area)
	{
		const HidingSpotList *list = area->GetHidingSpotList();
		for (HidingSpotList::const_iterator it = list->begin(); it != list->end() && m_count < MAX_SPOTS; it++)
		{
			const HidingSpot *spot = *it;
			if (!(spot->GetFlags() & HidingSpot::IN_COVER))
				continue;
			if (m_range > 0.0f && (*spot->GetPosition() - *m_origin).IsLengthGreaterThan(m_range))
				continue;
			if (IsSpotOccupied(m_me, spot->GetPosition()))
				continue;
			m_spot[m_count++] = spot->GetPosition();
		}
		return m_count < MAX_SPOTS;
	}

	CBaseEntity *m_me;
	const Vector *m_origin;
	float m_range;
	const Vector *m_spot[MAX_SPOTS];
	int m_count;
};

// Holding an area: someone coming in looks ahead of them first, at the spots in front of the
// way they come. Some bots (good or sneaky ones) pick a spot off to the side instead, one
// they'd have to turn to find. Picked from the hiding spots near the one stock chose, judged
// from the ways into the area (its approach areas); the stock pick if none is off-angle
const Vector *CCSBot::LsOffAngle(const Vector *stockPick, const Vector *pos, CNavArea *source, float range)
{
	if (!LsFight() || LsSolo() || !stockPick || !source || !pos || IsSniper())
		return stockPick;

	float skill = LsSkill();
	float aggro = LsAggro();
	float bent = 0.5f * skill + 0.5f * (1.0f - aggro) + (LsMatch() ? 0.15f : 0.0f);
	if (LsTrait(62) > bent - 0.1f || RANDOM_FLOAT(0.0f, 1.0f) > 0.7f)
		return stockPick;

	// the ways in: where an enemy steps into view, and the way they're facing then
	const int MAX_WAYS = 8;
	Vector wayAt[MAX_WAYS];
	Vector2D wayDir[MAX_WAYS];
	int ways = 0;
	for (int i = 0; i < source->GetApproachInfoCount() && ways < MAX_WAYS; i++)
	{
		const CNavArea::ApproachInfo *info = source->GetApproachInfo(i);
		if (!info->here.area || !info->prev.area)
			continue;
		Vector ap;
		if (info->prevToHereHow <= GO_WEST)
		{
			float halfWidth;
			info->prev.area->ComputePortal(info->here.area, (NavDirType)info->prevToHereHow, &ap, &halfWidth);
			ap.z = info->here.area->GetZ(&ap);
		}
		else
			ap = *info->here.area->GetCenter();
		const Vector &ahead = info->next.area ? *info->next.area->GetCenter() : *info->here.area->GetCenter();
		Vector2D dir = (ahead - ap).Make2D();
		if (dir.IsLengthLessThan(1.0f))
			dir = (*info->here.area->GetCenter() - *info->prev.area->GetCenter()).Make2D();
		if (dir.IsLengthLessThan(1.0f))
			continue;
		dir.NormalizeInPlace();
		wayAt[ways] = ap;
		wayDir[ways] = dir;
		ways++;
	}
	if (!ways)
		return stockPick;

	LsSpotCollector spots(this, pos, range);
	SearchSurroundingAreas(source, pos, spots, range);
	if (spots.m_count < 2)
		return stockPick;

	// a spot's angle: how far off the way they face it is, for every way in it can see
	// (off only if it's off for all of them); it has to see at least one
	const Vector *offAngle[LsSpotCollector::MAX_SPOTS];
	int offCount = 0;
	int step = (spots.m_count > 24) ? spots.m_count / 24 + 1 : 1;
	for (int s = RANDOM_LONG(0, step - 1); s < spots.m_count; s += step)
	{
		const Vector &spot = *spots.m_spot[s];
		Vector spotEye = spot + Vector(0, 0, 48.0f);
		float least = 180.0f;
		int seen = 0;
		for (int w = 0; w < ways; w++)
		{
			Vector2D toSpot = (spot - wayAt[w]).Make2D();
			float d = float(toSpot.Length());
			if (d < 1.0f)
				continue;
			TraceResult tr;
			UTIL_TraceLine(spotEye, wayAt[w] + Vector(0, 0, 48.0f), ignore_monsters, ENT(pev), &tr);
			if (tr.flFraction < 1.0f)
				continue;
			// (right on top of the way in isn't an angle, it's a collision)
			if (d < 128.0f)
			{
				least = 0.0f;
				break;
			}
			float cosOff = float(DotProduct(toSpot / d, wayDir[w]));
			float off = 57.29578f * acosf(Q_max(-1.0f, Q_min(1.0f, cosOff)));
			least = Q_min(least, off);
			seen++;
		}
		if (seen && least >= 50.0f && least <= 150.0f)
			offAngle[offCount++] = spots.m_spot[s];
	}
	if (!offCount)
		return stockPick;
	LsWhy("off-angle");
	return offAngle[RANDOM_LONG(0, offCount - 1)];
}

// ---- prefiring --------------------------------------------------------------------------

// A hiding spot comes into view close by while clearing an area: some players put a few
// bullets into it before they've seen anyone. The bold or good ones, rarely, with a gun
// that has the ammo for it, and never at a spot a teammate is near or in the way of
void CCSBot::LsPrefire(const Vector &spot)
{
	if (!LsFight())
		return;
	float now = gpGlobals->time;
	if (now < m_lsNextPrefire || now < m_lsPrefireUntil)
		return;
	if (IsAttacking() || IsSafe() || IsBlind() || IsReloading() || IsUsingLadder() || m_lsSneaking || !IsRunning()
		|| IsDefusingBomb() || IsThrowingGrenade() || GetHostageEscortCount() > 0)
		return;
	if (IsUsingKnife() || IsUsingGrenade() || IsUsingSniperRifle() || !GetActiveWeapon() || GetActiveWeaponAmmoRatio() < 0.35f)
		return;

	float skill = LsSkill();
	float aggro = LsAggro();
	float want = 0.5f * aggro + 0.5f * skill;
	if (LsTrait(64) > want - 0.25f)
		return;
	m_lsNextPrefire = now + RANDOM_FLOAT(1.0f, 2.0f);
	float chance = LsSolo() ? 0.35f : LsMatch() ? 0.3f : 0.2f;
	if (RANDOM_FLOAT(0.0f, 1.0f) > chance)
		return;

	// close, in sight now, and no teammate there
	Vector at = spot + Vector(0, 0, 10.0f);		// (about head height)
	Vector eye = pev->origin + pev->view_ofs;
	float range = float((at - eye).Length());
	if (range < 120.0f || range > 550.0f)
		return;
	TraceResult tr;
	UTIL_TraceLine(eye, at, ignore_monsters, ENT(pev), &tr);
	if (tr.flFraction < 1.0f)
		return;
	if (LsTeammateNear(this, at, 250.0f))
		return;

	m_lsPrefireAt = at;
	m_lsPrefireUntil = now + 0.9f;
	m_lsPrefireFireEnd = 0.0f;
	m_lsNextPrefire = now + RANDOM_FLOAT(8.0f, 15.0f);
	SetLookAt(LS_PREFIRE, &m_lsPrefireAt, PRIORITY_MEDIUM, 0.8f);
	LsWhy("prefire");
}

// The burst, once the gun points at the spot
static void LsPrefireTick(CCSBot *me, const Vector &at, float &until, float &fireEnd)
{
	float now = gpGlobals->time;
	if (now >= until)
		return;
	if (me->IsAttacking() || me->IsReloading() || me->IsActiveWeaponClipEmpty() || me->IsUsingKnife() || me->IsUsingGrenade()
		|| LsTeammateNear(me, at, 200.0f))
	{
		until = 0.0f;
		return;
	}
	me->SetLookAt(LS_PREFIRE, &at, PRIORITY_MEDIUM, 0.3f);
	Vector dir = at - (me->pev->origin + me->pev->view_ofs);
	dir.NormalizeInPlace();
	UTIL_MakeVectors(me->pev->v_angle + me->pev->punchangle);
	if (fireEnd <= 0.0f)
	{
		if (float(DotProduct(gpGlobals->v_forward, dir)) < 0.995f)
			return;
		fireEnd = now + RANDOM_FLOAT(0.12f, 0.3f);
		until = fireEnd;
	}
	// (a pistol or a pump needs the trigger let go between shots)
	bool semi = me->IsUsingPistol() || me->IsUsingShotgun();
	if (!semi || int(now * 8.0f) % 2 == 0)
		me->PrimaryAttack();
}

// ---- falling back -----------------------------------------------------------------------

// Starting to fall back from a fight (a retreat, a run to cover to reload): good players
// back off with their eyes on the threat, others glance back over their shoulder now and
// then; newer ones just turn and run
void CCSBot::LsFallBack()
{
	if (!LsFight())
		return;
	float skill = LsSkill();
	if (GetEnemy())
		m_lsBackFrom = GetLastKnownEnemyPosition();
	else if (GetAttacker())
		m_lsBackFrom = GetAttacker()->pev->origin;
	else
		return;

	if (skill > 0.6f && LsTrait(66) < 0.7f)
		m_lsBackStyle = 2;
	else if (skill > 0.35f || LsTrait(66) < 0.5f)
		m_lsBackStyle = 1;
	else
	{
		m_lsBackStyle = 0;
		return;
	}
	m_lsBackFrom.z += HalfHumanHeight + LsLookHeight();
	m_lsBackUntil = gpGlobals->time + (m_lsBackStyle == 2 ? RANDOM_FLOAT(2.5f, 4.0f) : RANDOM_FLOAT(3.0f, 5.0f));
	m_lsNextGlance = gpGlobals->time + RANDOM_FLOAT(0.3f, 0.8f);
}

bool CCSBot::LsFallingBack() const
{
	return gpGlobals->time < m_lsBackUntil;
}

// (looking back at the threat right now: the view may turn further from the way it's going)
bool CCSBot::LsLookingBack() const
{
	return gpGlobals->time < m_lsBackUntil && m_lookAtDesc == LS_LOOKBACK;
}

static void LsFallBackTick(CCSBot *me, const Vector &from, float &until, int style, float &nextGlance)
{
	float now = gpGlobals->time;
	if (now >= until)
		return;
	// there: hiding takes it from here (and a fight, a ladder or a blinding ends it)
	if (!me->IsHiding() || me->IsAtHidingSpot() || me->IsAttacking() || me->IsUsingLadder() || me->IsBlind())
	{
		until = 0.0f;
		return;
	}
	if (style == 2)
	{
		me->SetLookAt(LS_LOOKBACK, &from, PRIORITY_MEDIUM, 0.3f);
	}
	else if (now >= nextGlance)
	{
		me->SetLookAt(LS_LOOKBACK, &from, PRIORITY_MEDIUM, RANDOM_FLOAT(0.35f, 0.6f));
		nextGlance = now + RANDOM_FLOAT(1.0f, 1.8f);
	}
}

// Each think, after the state machine
void CCSBot::LsMovesUpdate()
{
	if (!LsFight() || !IsAlive())
		return;
	LsPrefireTick(this, m_lsPrefireAt, m_lsPrefireUntil, m_lsPrefireFireEnd);
	LsFallBackTick(this, m_lsBackFrom, m_lsBackUntil, m_lsBackStyle, m_lsNextGlance);
}

// ---- scoping --------------------------------------------------------------------------------

// Newer players scope in with an enemy right in front of them (the zoom sound, the slow
// scope-in), and keep the sniper rifle instead of going to the pistol; better ones don't
bool CCSBot::LsScopesClose()
{
	if (!LsFight() || LsMatch())
		return false;
	return LsSkill() < 0.45f && LsTrait(67) < 0.6f;
}

// ---- panic ------------------------------------------------------------------------------

// A fight starts by surprise (an enemy close, or shot first): a newer pub player may freeze
// up the wrong way, holding walk or crouch for a moment
void CCSBot::LsAttackBegins(bool crouchAndHold)
{
	m_lsPanicUntil = 0.0f;
	m_lsPanicKind = 0;
	if (!LsFight() || LsMatch() || IsUsingKnife() || IsUsingLadder() || IsBlind())
		return;
	if (LsSkill() >= 0.4f || LsTrait(68) >= 0.55f)
		return;

	CBasePlayer *pEnemy = GetEnemy();
	bool close = pEnemy && (pEnemy->pev->origin - pev->origin).IsLengthLessThan(450.0f);
	if (!IsSurprised() && GetTimeSinceAttacked() > 1.0f && !close)
		return;
	if (RANDOM_FLOAT(0.0f, 1.0f) > (LsSolo() ? 0.3f : 0.45f))
		return;

	float now = gpGlobals->time;
	m_lsPanicUntil = now + RANDOM_FLOAT(0.5f, 1.2f);
	if (LsTrait(69) < 0.5f || crouchAndHold || !(pev->flags & FL_ONGROUND))
	{
		m_lsPanicKind = 1;
		Walk();
	}
	else
	{
		// (the crouch burst holds it, and stands it up after)
		m_lsPanicKind = 2;
		m_lsCrouchBurstUntil = m_lsPanicUntil;
		Crouch();
	}
}

void CCSBot::LsPanicUpdate()
{
	if (!m_lsPanicKind)
		return;
	if (gpGlobals->time >= m_lsPanicUntil || IsUsingLadder())
	{
		if (m_lsPanicKind == 1 && !IsUsingKnife())
			Run();
		m_lsPanicKind = 0;
		return;
	}
	if (m_lsPanicKind == 1)
		Walk();
}

// ---- ladders --------------------------------------------------------------------------------
//
// On a ladder the engine (PM_LadderMove) turns movement into the ladder's face into climbing,
// and adds the up-and-down part of where the player looks. Turned about 63 degrees on the
// ladder, looking up as the bots do (60 degrees), W and the strafe away from the turn both
// push into the ladder and their sideways parts cancel: about 390 units a second up instead
// of 270. People call it the diagonal climb.

// Which way this bot climbs this ladder diagonally (1 turned left with D, -1 turned right
// with A), or 0 straight; rolled once per ladder. Not near the top (it straightens up to get
// off) and not crouched (a crouch-through at the top)
int CCSBot::LsLadderSide()
{
	if (!LsFight() || !m_pathLadder)
		return 0;
	if (m_lsLadderFor != m_pathLadderTimestamp)
	{
		m_lsLadderFor = m_pathLadderTimestamp;
		float skill = LsSkill();
		float chance = skill > 0.6f ? 0.85f : skill > 0.4f ? 0.3f : 0.05f;
		m_lsLadderSide = (RANDOM_FLOAT(0.0f, 1.0f) < chance) ? (LsTrait(70) < 0.5f ? 1 : -1) : 0;
	}
	if (!m_lsLadderSide || m_pathLadderState != ASCEND_LADDER || IsCrouching() || GetFeetZ() > m_pathLadderEnd - 64.0f)
		return 0;
	return m_lsLadderSide;
}

// The view for a diagonal climb: turned from straight at the ladder, a little more or less
// to drift back onto its line (instead of the stock line-keeping)
bool CCSBot::LsLadderYaw(float faceYaw, float *yaw)
{
	int side = LsLadderSide();
	if (!side)
		return false;
	float left = faceYaw + 90.0f;
	Vector2D lat(BotCOS(left), BotSIN(left));
	Vector2D off = (pev->origin - m_pathLadder->m_top).Make2D();
	float offLeft = off.x * lat.x + off.y * lat.y;
	float turn = Q_max(40.0f, Q_min(75.0f, 63.0f - side * offLeft));
	*yaw = faceYaw + side * turn;
	return true;
}

// While climbing: the strafe that goes with the turn, once the view has come round
void CCSBot::LsLadderClimb()
{
	if (!LsLadderSide())
		return;
	NavDirType faceDir = m_pathLadderFaceIn ? OppositeDirection(m_pathLadder->m_dir) : m_pathLadder->m_dir;
	float yaw;
	if (!LsLadderYaw(DirectionToAngle(faceDir), &yaw))
		return;
	if (Q_abs(NormalizeAngle(pev->v_angle.y - yaw)) > 12.0f)
		return;
	if (m_lsLadderSide > 0)
		StrafeRight();
	else
		StrafeLeft();
}

// ---- silent drops -------------------------------------------------------------------------

// Sneaking, a drop it could jump off is walked off instead when that's quiet: a landing at
// under about 290 units a second makes no sound, which is a fall of about 52 units; a jump
// off anything over a step lands louder. Only where the path really goes down (not over a gap)
bool CCSBot::LsQuietDrop(float dz)
{
	if (!LsFight() || IsRunning() || IsSafe() || IsUsingLadder())
		return false;
	if (-dz > 52.0f)
		return false;
	if (m_lastKnownArea && (m_lastKnownArea->GetAttributes() & NAV_JUMP))
		return false;
	if (!HasPath())
		return false;
	// (the path's next few points: one of them close by and lower)
	float feet = GetFeetZ();
	for (int i = m_pathIndex; i < m_pathLength && i < m_pathIndex + 3; i++)
	{
		const Vector &p = m_path[i].pos;
		if (p.z < feet - 20.0f && (p - pev->origin).Make2D().IsLengthLessThan(150.0f))
			return true;
	}
	return m_goalPosition.z < feet - 20.0f;
}

// ---- boosting -------------------------------------------------------------------------------
//
// Not done. A boost spot is a ledge only reachable standing on someone's head, and the nav
// mesh has nothing there: it's built by walking out from the spawns, so a boost-only ledge has
// no areas, no hiding spots and no link to get up to it. Without per-map lists of boost spots
// (where to stand, where it gets you, what to hold from up there) a bot couldn't know where to
// boost, couldn't walk about once up there, and two bots would have to meet and time a jump
// onto a crouched head - one more thing to get wrong and look less human than not doing it.

// Getting off a ladder into a crouch-only area, or where the ceiling is too low to stand (a
// vent at the top of a ladder, as on cs_assault): it has to duck to get in, as players do;
// standing, it bumps against the edge and gets stuck there
bool CCSBot::LsDucksOff(const CNavArea *area, const Vector &spot) const
{
	if (!LsFight() || !area)
		return false;
	if (area->GetAttributes() & NAV_CROUCH)
		return true;
	TraceResult tr;
	Vector floor(spot.x, spot.y, area->GetZ(&spot) + 2.0f);
	UTIL_TraceLine(floor, floor + Vector(0, 0, HumanHeight), ignore_monsters, ENT(pev), &tr);
	return tr.flFraction < 1.0f;
}

// Nothing has startled it lately: no enemy in sight or seen in the last few seconds, not hurt
// or shot at, no loud noise close by. Then its looks about are calm turns (LsCalm in
// UpdateLookAngles); a startle makes them quick again
bool CCSBot::LsCalm() const
{
	float now = gpGlobals->time;
	if (IsEnemyVisible() || GetTimeSinceLastSawEnemy() < 4.0f || GetTimeSinceAttacked() < 3.0f || now - m_lsShotAtAt < 3.0f)
		return false;
	if (m_noiseTimestamp > 0.0f && now - m_noiseTimestamp < 2.0f && m_noisePriority >= PRIORITY_MEDIUM)
		return false;
	return !IsUsingLadder() && !IsBlind();
}
