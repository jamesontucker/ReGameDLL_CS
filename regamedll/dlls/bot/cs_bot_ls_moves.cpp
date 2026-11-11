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
		|| (me->IsHurrying() && me->LsSkill() < 0.5f) || me->GetHostageEscortCount() > 0 || me->IsDefusingBomb() || me->IsThrowingGrenade()
		|| me->GetEnemiesRemaining() <= 0 || me->LsFallingBack() || me->LsStuck())
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
	if (!LsFight() || !HasPath() || LsRushing())
		return false;
	float now = gpGlobals->time;

	// a jiggle under way: out, back (and maybe once more)
	if (now < m_lsJiggleUntil)
	{
		if (now >= m_lsJiggleJumpUntil && !LsCornerFree(this))
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
		m_lsJumpPeek = false;
		// (an AWPer known out past this corner: a good player jump-peeks it, to draw the shot
		// without giving it a still target)
		if (LsSkill() > 0.6f && LsAwpKnownToward(Vector(out.x, out.y, 0.0f)) && now >= m_lsNextJiggle
			&& RANDOM_FLOAT(0.0f, 1.0f) < 0.4f + 0.5f * LsSkill())
		{
			m_lsCornerPlan = LS_CORNER_JIGGLE;
			m_lsJumpPeek = true;
		}
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
		LsWhy(m_lsJumpPeek ? "jump-peek" : "jiggle");
		if (m_lsJumpPeek)
		{
			Jump(MUST_JUMP);
			m_lsJiggleJumpUntil = now + 0.9f;
			m_lsJiggleLeft = 1;
			m_lsJumpPeek = false;
		}
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
			if (IsSpotOccupied(m_me, spot->GetPosition()) || LsSpotCrowded(m_me, spot->GetPosition()))
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
	if (!LsFight() || LsSolo() || !stockPick || !source || !pos || IsSniper() || LsRushing())
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
	// (a sneaky one, now and then, takes the darkest of them: a small part of it, only between
	// spots that are already good angles)
	int style = LsStyle();
	int pick = RANDOM_LONG(0, offCount - 1);
	if ((style == LS_STYLE_NINJA || style == LS_STYLE_LURKER || style == LS_STYLE_CAMPER) && offCount > 1 && RANDOM_FLOAT(0.0f, 1.0f) < 0.2f)
	{
		// (only a clearly darker one - shade's never worth more than the angle and cover all
		// these spots already have)
		int dark = pick, least = LsLightAt(*offAngle[pick]), first = least;
		for (int i = 0; i < offCount && i < 12; i++)
		{
			int light = LsLightAt(*offAngle[i]);
			if (light < least)
			{
				least = light;
				dark = i;
			}
		}
		if (first - least > 40)
			pick = dark;
	}
	return offAngle[pick];
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
	if (me->IsAttacking() || me->IsBlind() || me->IsReloading() || me->IsActiveWeaponClipEmpty() || me->IsUsingKnife() || me->IsUsingGrenade()
		|| LsTeammateNear(me, at, 200.0f) || me->IsFriendInLineOfFire())
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
	if (!semi || me->LsTriggerTap())
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
	LsTraceAttack();
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
	// near the top with enemies about: eyes up on where it comes out (the better ones), not on
	// the rungs
	if (m_pathLadder && LsSkill() > 0.4f && LsLadderExposed())
	{
		const Vector &top = m_pathLadder->m_top;
		if (top.z > pev->origin.z && top.z - pev->origin.z < 120.0f)
		{
			NavDirType out = m_pathLadderFaceIn ? OppositeDirection(m_pathLadder->m_dir) : m_pathLadder->m_dir;
			float yaw = DirectionToAngle(out);
			Vector exit = top + Vector(BotCOS(yaw) * 80.0f, BotSIN(yaw) * 80.0f, HalfHumanHeight);
			SetLookAt("Loner-Strike: ladder top", &exit, PRIORITY_MEDIUM, 0.4f);
		}
	}
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
// UpdateLookAngles); a startle makes them quick again. pluginLooks false: a quick look a plugin
// asked for doesn't count (its next step, a spin's or a nod's hold, is still taken)
bool CCSBot::LsCalm(bool pluginLooks) const
{
	float now = gpGlobals->time;
	if (IsEnemyVisible() || GetTimeSinceLastSawEnemy() < 4.0f || GetTimeSinceAttacked() < 3.0f || now - m_lsShotAtAt < 3.0f)
		return false;
	if (m_noiseTimestamp > 0.0f && now - m_noiseTimestamp < 2.0f && m_noisePriority >= PRIORITY_MEDIUM)
		return false;
	// (a pre-aim at footsteps just now, or a quick look a plugin asked for: quick turns)
	if (now - m_lsPreAimAt < 1.5f || (pluginLooks && now < m_lsPuppet.lookUntil && m_lsPuppet.fast))
		return false;
	return !IsUsingLadder() && !IsBlind();
}

// Respawned, or moved by a plugin or a teleporter: a wall spray, a prefire or a fall-back look
// would carry on from the new place at the old one, and the path is from somewhere else
void CCSBot::LsStartOver()
{
	m_lsCornerAt = Vector(99999.0f, 99999.0f, 99999.0f);
	m_lsCornerPlan = 0;
	m_lsJiggleUntil = 0.0f;
	m_lsJiggleJumpUntil = 0.0f;
	m_lsBangUntil = 0.0f;
	m_lsPrefireUntil = 0.0f;
	m_lsBackUntil = 0.0f;
	m_lsFloorGun = nullptr;
	m_lsHeBlockedSince = -1.0f;
	m_lsWasBlind = m_lsBlindSpray = false;
	m_lsCarrierFollowUntil = 0.0f;
	m_lsLastPos = pev->origin;
	m_lsLastPosAt = gpGlobals->time;
}

void CCSBot::LsTeleportCheck()
{
	float now = gpGlobals->time;
	float gap = now - m_lsLastPosAt;
	Vector was = m_lsLastPos;
	m_lsLastPos = pev->origin;
	m_lsLastPosAt = now;
	// (faster than 1000 units a second, a fall aside: a long drop is fast too, but straight down)
	if (gap <= 0.0f || gap > 0.5f || !IsAlive())
		return;
	Vector moved = pev->origin - was;
	if (moved.Make2D().IsLengthLessThan(Q_max(256.0f, 1000.0f * gap)))
		return;
	LsStartOver();
	if (!LsFight())
		return;
	DestroyPath();
	ResetStuckMonitor();
	LsNadeStop();
	if (m_lsPuppet.any)
		LsPuppetRelease();
	LsWhy("teleported");
}

// An area it couldn't get into (under a ledge, a bad link in the nav): gave up twice in a row
// within half a minute and it's avoided for 30 s, so the next path goes another way
bool CCSBot::LsBadArea(const CNavArea *area) const
{
	if (!LsFight() || !area)
		return false;
	for (int i = 0; i < 4; i++)
	{
		if (m_lsBadAreas[i] == area && gpGlobals->time < m_lsBadAreaUntil[i])
		{
			// (smoked off now: crossed under the smoke, as people do with an AWP lane)
			Vector mid = *area->GetCenter() + Vector(0, 0, HalfHumanHeight);
			if (TheCSBots() && TheCSBots()->IsInsideSmokeCloud(&mid))
				return false;
			return true;
		}
	}
	return false;
}

void CCSBot::LsAreaStuck(const CNavArea *area)
{
	if (!LsFight() || !area || IsAttacking())
		return;
	float now = gpGlobals->time;
	// (walking, sneaking or held by a plugin it's only slow, not stuck; nor is a big area,
	// which takes a walker more than the give-up time to cross)
	const Extent *ext = area->GetExtent();
	if (!IsRunning() || m_lsSneaking || now < m_lsPuppet.walkUntil || now < m_lsPuppet.holdUntil
		|| ext->hi.x - ext->lo.x > 450.0f || ext->hi.y - ext->lo.y > 450.0f)
		return;
	if (area != m_lsFailArea || now - m_lsFailAt > 30.0f)
	{
		m_lsFailArea = area;
		m_lsFailAt = now;
		return;
	}
	// (the slot that's free, or ran out first)
	int slot = 0;
	for (int i = 1; i < 4; i++)
	{
		if (m_lsBadAreaUntil[i] < m_lsBadAreaUntil[slot])
			slot = i;
	}
	m_lsBadAreas[slot] = area;
	m_lsBadAreaUntil[slot] = now + 30.0f;
	m_lsFailArea = nullptr;
	LsWhy("bad-area");
}

// Flashed. Stock bots pick one direction and walk it for the whole flash. People panic in
// their own way and change their mind: the careful back off and crouch, the aggressive hold
// and spray where the enemy was, newer players stumble about. As it fades: a quick look back
// to where the trouble was
bool CCSBot::LsBlindTick()
{
	if (!LsFight())
		return false;
	float now = gpGlobals->time;
	if (!IsBlind())
	{
		if (m_lsWasBlind)
		{
			m_lsWasBlind = false;
			if (m_lsBlindWay == 3)
				StandUp();
			if (GetTimeSinceLastSawEnemy() < 8.0f)
			{
				Vector look = GetLastKnownEnemyPosition() + Vector(0, 0, HalfHumanHeight);
				SetLookAt("Loner-Strike: after the flash", &look, PRIORITY_HIGH, RANDOM_FLOAT(0.6f, 1.2f));
			}
		}
		return false;
	}
	float aggro = LsAggro(), skill = LsSkill();
	if (!m_lsWasBlind)
	{
		m_lsWasBlind = true;
		m_lsBlindTurnAt = 0.0f;
		// (a spray where they were: the aggressive, and only with someone seen just now)
		m_lsBlindSpray = GetTimeSinceLastSawEnemy() < 4.0f && !IsUsingKnife() && !IsUsingGrenade() && RANDOM_FLOAT(0.0f, 1.0f) < 0.08f + 0.4f * aggro;
	}
	if (now >= m_lsBlindTurnAt)
	{
		if (m_lsBlindWay == 3)
			StandUp();
		m_lsBlindTurnAt = now + RANDOM_FLOAT(0.4f, 0.9f);
		float r = RANDOM_FLOAT(0.0f, 1.0f);
		// 0 back off, 1/2 strafe, 3 crouch and hold, 4 stand still
		if (skill > 0.6f)
			m_lsBlindWay = r < 0.45f ? 0 : r < 0.75f ? 3 : r < 0.9f ? 1 + RANDOM_LONG(0, 1) : 4;
		else if (aggro > 0.65f)
			m_lsBlindWay = r < 0.5f ? 4 : r < 0.8f ? 1 + RANDOM_LONG(0, 1) : 0;
		else
			m_lsBlindWay = RANDOM_LONG(0, 4);
	}
	// (planting or defusing: it stays on it; crouching at most)
	bool onBomb = m_state == &m_plantBombState || m_state == &m_defuseBombState;
	if (onBomb && m_lsBlindWay != 3)
		m_lsBlindWay = 4;
	// (and keeps its finger on it: a flash doesn't undo the plant or defuse)
	if (onBomb)
		UseEnvironment();
	if (!IsAtHidingSpot() && !IsOnLadder())
	{
		switch (m_lsBlindWay)
		{
		case 0: MoveBackward(); break;
		case 1: StrafeLeft(); break;
		case 2: StrafeRight(); break;
		case 3: Crouch(); break;
		default: break;
		}
	}
	// (never a grenade's pin pulled blind, nor through a teammate)
	if (m_lsBlindSpray && !IsUsingGrenade() && !m_isWaitingToTossGrenade && !IsFriendInLineOfFire())
	{
		Vector at = GetLastKnownEnemyPosition() + Vector(0, 0, HalfHumanHeight);
		SetLookAt("Loner-Strike: spraying blind", &at, PRIORITY_HIGH, 0.3f);
		if (!IsActiveWeaponClipEmpty() && (!(IsUsingPistol() || IsUsingSniperRifle()) || int(now * 6.0f) % 2 == 0))
			PrimaryAttack();
	}
	return true;
}

// A sniper shot: people don't stand in the same doorway for the bolt to come back and get
// traded. The better ones step aside (left or right, one way per shot) and peek again from a
// little over; newer ones stand there
void CCSBot::LsSniperStep()
{
	if (!LsFight())
		return;
	float now = gpGlobals->time;
	if (now - m_lsSnipeStepAt > 1.2f)
		m_lsSnipeStepDir = RANDOM_FLOAT(0.0f, 1.0f) < 0.15f + 0.65f * LsSkill() ? RANDOM_LONG(1, 2) : 0;
	m_lsSnipeStepAt = now;
	// (not off a ledge or into a wall: floor and room that way, else the other way, else not at all)
	if (m_lsSnipeStepDir)
	{
		auto clear = [&](int dir) {
			Vector right(BotCOS(pev->v_angle.y - 90.0f), BotSIN(pev->v_angle.y - 90.0f), 0.0f);
			Vector side = pev->origin + right * (dir == 2 ? 40.0f : -40.0f);
			TraceResult tr;
			UTIL_TraceHull(pev->origin, side, ignore_monsters, head_hull, ENT(pev), &tr);
			if (tr.flFraction < 1.0f)
				return false;
			UTIL_TraceLine(side, side - Vector(0, 0, 64.0f + HalfHumanHeight), ignore_monsters, ENT(pev), &tr);
			return tr.flFraction < 1.0f;
		};
		if (!clear(m_lsSnipeStepDir))
			m_lsSnipeStepDir = clear(3 - m_lsSnipeStepDir) ? 3 - m_lsSnipeStepDir : 0;
	}
	if (m_lsSnipeStepDir == 1)
		StrafeLeft();
	else if (m_lsSnipeStepDir == 2)
		StrafeRight();
}

// Several enemies facing it at once (the threat ranking notes it): a person feels overrun and,
// unless bold, gets out; the bold, or someone with the upper hand on health, fights on. Rolled
// once per moment it happens, not every frame
bool CCSBot::LsOverwhelmed()
{
	if (!LsFight() || LsSolo())
		return false;
	float now = gpGlobals->time;
	if (now - m_lsOverwhelmedAt > 0.5f || now < m_lsOverwhelmRollAt)
		return false;
	m_lsOverwhelmRollAt = now + 4.0f;
	// (a newer player doesn't read being outnumbered: it takes the fight)
	if (RANDOM_FLOAT(0.0f, 1.0f) > 0.35f + 0.65f * LsSkill())
		return false;
	float bold = 0.6f * LsAggro() + 0.4f * LsTrait(92);
	return RANDOM_FLOAT(0.0f, 1.0f) > 0.25f + 0.6f * bold + (LsHealth() > 80.0f ? 0.1f : 0.0f);
}

// Hit by someone it isn't fighting (a flanker, while it's busy with another): it notices. The
// better ones turn to the new threat when they can see them; the rest at least look that way
// a moment (and in deathmatch and Gun Game too: being shot from the side is felt everywhere)
void CCSBot::LsFlankCheck()
{
	if (!LsFight() || !IsAttacking() || !IsAlive())
		return;
	float now = gpGlobals->time;
	if (now < m_lsFlankReactAt || GetTimeSinceAttacked() > 0.25f)
		return;
	CBasePlayer *pFlanker = GetAttacker();
	if (!pFlanker || !pFlanker->IsAlive() || pFlanker == GetEnemy() || !IsEnemy(pFlanker))
		return;
	m_lsFlankReactAt = now + 0.8f;
	if (IsVisible(pFlanker, false) && RANDOM_FLOAT(0.0f, 1.0f) < 0.35f + 0.5f * LsSkill())
	{
		Attack(pFlanker);
		LsWhy("flanked");
		return;
	}
	Vector at = pFlanker->pev->origin + Vector(0, 0, HalfHumanHeight);
	// (mid-fight the aim holds the view; a quick turn gets past it)
	if (IsAimingAtEnemy())
		LsUrgentTurn(at, RANDOM_FLOAT(0.3f, 0.6f), nullptr);
	else
		SetLookAt("Loner-Strike: shot from the side", &at, PRIORITY_HIGH, RANDOM_FLOAT(0.3f, 0.6f));
}

// The enemy's position as this bot's eyes and hand estimate it: what it saw a moment ago (60 ms
// for the best, 200 ms for a newcomer), a little noisy, run through a tracking filter (an
// alpha-beta filter, the steady form of a Kalman filter) that keeps a position and a velocity
// and predicts where they are now. A steady run is led well; a sudden change of direction takes
// the filter a moment to believe, so the aim carries on past and comes back, further the faster
// they were going, as people's does. The better the player, the quicker the filter adapts
Vector CCSBot::LsLaggedEnemyPos()
{
	CBasePlayer *pEnemy = m_enemy;
	float now = gpGlobals->time;
	if (!pEnemy)
		return m_lastEnemyPosition;
	float skill = LsSkill();
	if (pEnemy != static_cast<CBasePlayer *>(m_lsTrackOf) || now - m_lsTrackAt[(m_lsTrackN + 7) % 8] > 0.5f)
	{
		m_lsTrackOf = pEnemy;
		m_lsTrackN = 0;
		for (int i = 0; i < 8; i++)
		{
			m_lsTrackPos[i] = pEnemy->pev->origin;
			m_lsTrackAt[i] = now;
		}
		m_lsEstPos = pEnemy->pev->origin;
		m_lsEstVel = g_vecZero;
		m_lsEstAt = now;
	}
	if (now - m_lsTrackAt[(m_lsTrackN + 7) % 8] >= 0.03f)
	{
		m_lsTrackPos[m_lsTrackN % 8] = pEnemy->pev->origin;
		m_lsTrackAt[m_lsTrackN % 8] = now;
		m_lsTrackN++;
	}
	// what its eyes report: the position a moment ago
	float lag = 0.06f + 0.14f * (1.0f - skill) + 0.03f * LsTrait(94);
	float want = now - lag;
	Vector seen = m_lsTrackPos[(m_lsTrackN + 7) % 8];
	float seenAt = -1.0f, oldest = 1e9f;
	Vector oldestPos = seen;
	for (int i = 0; i < 8; i++)
	{
		if (m_lsTrackAt[i] <= want && m_lsTrackAt[i] > seenAt)
		{
			seenAt = m_lsTrackAt[i];
			seen = m_lsTrackPos[i];
		}
		if (m_lsTrackAt[i] < oldest)
		{
			oldest = m_lsTrackAt[i];
			oldestPos = m_lsTrackPos[i];
		}
	}
	if (seenAt < 0.0f)
		seen = oldestPos;
	// (and a little noisy: a few units, more for a newer player and further off)
	float range = float((pEnemy->pev->origin - pev->origin).Length());
	float noise = (2.0f + 6.0f * (1.0f - skill)) * Q_min(2.0f, range / 800.0f);
	seen.x += RANDOM_FLOAT(-noise, noise);
	seen.y += RANDOM_FLOAT(-noise, noise);

	// the filter: predict, then correct toward what was seen (alpha for position, beta for
	// velocity: higher for the better players, who adapt faster)
	float dt = Q_max(0.001f, now - m_lsEstAt);
	m_lsEstAt = now;
	Vector predicted = m_lsEstPos + m_lsEstVel * dt;
	Vector residual = seen - predicted;
	float alpha = 0.35f + 0.35f * skill, beta = (0.08f + 0.22f * skill) / dt * 0.1f;
	m_lsEstPos = predicted + residual * alpha;
	m_lsEstVel = m_lsEstVel + residual * Q_min(beta, 3.0f);
	m_lsEstVel.z = 0.0f;
	// (aim where it thinks they are now: the estimate carried forward over its own lag)
	return m_lsEstPos + m_lsEstVel * lag;
}

// A hand's tremor: a smooth wander that drifts back to centre (an Ornstein-Uhlenbeck walk),
// a fraction of a degree for a steady hand, more for a shaky one
void CCSBot::LsTremorTick()
{
	float dt = g_flBotCommandInterval;
	// (shakier under stress: hurt, hit just now, or a spray that hit nothing)
	float stress = (LsHealth() < 40.0f ? 0.6f : 0.0f) + (GetTimeSinceAttacked() < 1.5f ? 0.5f : 0.0f) + (gpGlobals->time < m_lsWhiffUntil ? 0.6f : 0.0f);
	float sigma = (0.25f + 0.9f * (1.0f - LsSkill())) * (1.0f + stress);
	// (a far target: people steady the hand for it, the better ones much more; a head across
	// a long room is smaller than a shaky hand's wander)
	if (IsAttacking() && m_enemy.IsValid())
	{
		float distant = Q_max(0.0f, Q_min(1.0f, (float((m_enemy->pev->origin - pev->origin).Length()) - 500.0f) / 1000.0f));
		sigma *= 1.0f - (0.25f + 0.35f * LsSkill()) * distant;
	}
	const float theta = 6.0f;
	auto gauss = []() { return (RANDOM_FLOAT(-1.0f, 1.0f) + RANDOM_FLOAT(-1.0f, 1.0f) + RANDOM_FLOAT(-1.0f, 1.0f)) * 0.577f; };
	m_lsTremorYaw += -theta * m_lsTremorYaw * dt + sigma * 2.4f * sqrtf(dt) * gauss();
	m_lsTremorPitch += -theta * m_lsTremorPitch * dt + sigma * 1.8f * sqrtf(dt) * gauss();
	m_lsTremorYaw = Q_max(-2.5f, Q_min(2.5f, m_lsTremorYaw));
	m_lsTremorPitch = Q_max(-2.0f, Q_min(2.0f, m_lsTremorPitch));
}

// ---- the hand -----------------------------------------------------------------------------
//
// How a person turns with a mouse (what players read as "human" or "bot" in a spectator's view):
//   - still most of the time; it moves in separate strokes, not a constant glide
//   - each stroke is planned: a ballistic flick with a bell-shaped speed curve (minimum jerk),
//     taking longer the further it goes and the smaller the target (Fitts's law)
//   - it lands a little off, more for a big or hurried flick and a less skilled hand, usually
//     a touch past (overshoot), then one or two small, slower corrections
//   - small drifts of the target are let go (a dead zone: lazy tracking), until it's far enough
//     off to be worth another stroke
//   - up-down is slower and less accurate than side to side
//   - after a calm look it rests a moment before the next
// Skill, how bold it is and the moment (a fight, a startle, calm) set the numbers.

static float LsMinJerk(float s)
{
	s = Q_max(0.0f, Q_min(1.0f, s));
	return s * s * s * (10.0f - 15.0f * s + 6.0f * s * s);
}

void CCSBot::LsHandUpdate(float goalYaw, float goalPitch, float dt)
{
	LsHand &h = m_lsHandM;
	float now = gpGlobals->time;
	float skill = LsSkill();
	bool fight = IsAttacking();
	bool calm = !fight && LsCalm();
	// (standing still and calm, a person barely moves the view: 88-90% still in the playtest,
	// against about half when moving; moving looks already matched)
	bool standing = calm && pev->velocity.Length2D() < 60.0f;
	float oldY = pev->v_angle.y, oldP = pev->v_angle.x;

	float offY = NormalizeAngle(goalYaw - pev->v_angle.y);
	float offP = NormalizeAngle(goalPitch - pev->v_angle.x);
	// (up-down counts for more: it's the harder way for a wrist)
	float off = sqrtf(offY * offY + 1.56f * offP * offP);

	// the target's size, in degrees: a person in a fight (smaller far away), a spot when calm
	float width = 4.0f;
	if (fight && m_enemy.IsValid())
	{
		float range = Q_max(64.0f, float((m_enemy->pev->origin - pev->origin).Length()));
		width = Q_max(0.6f, 2.0f * 57.2958f * atanf(14.0f / range));
	}

	if (h.moving)
	{
		// (the stroke is committed for its first 60%: the hand can't take in a change that fast;
		// after that, a goal that's moved well away aborts it for a new stroke from here)
		float driftY = NormalizeAngle(goalYaw - h.goalY), driftP = NormalizeAngle(goalPitch - h.goalP);
		float s = (now - h.t0) / h.dur;
		float k = LsMinJerk(s);
		pev->v_angle.y = h.y0 + NormalizeAngle(h.y1 - h.y0) * k;
		pev->v_angle.x = h.p0 + (h.p1 - h.p0) * k;
		// (and shot just now with the goal swung well away: a jolt, the stroke dropped at once)
		bool jolt = GetTimeSinceAttacked() < 0.15f && sqrtf(driftY * driftY + driftP * driftP) > 20.0f;
		if (jolt)
		{
			h.moving = false;
			h.corrections = 0;
			h.restUntil = now;
		}
		// (past the committed part, only a goal that jumped (another target) drops the stroke;
		// smaller drift is left to the correction after it lands. A dropped stroke restarts from
		// a standstill, which held fight flicks well under a person's speed)
		else if (s >= 0.6f && s < 1.0f && sqrtf(driftY * driftY + driftP * driftP) > Q_max(15.0f, 3.0f * width))
		{
			h.moving = false;
			h.corrections = 0;
			h.restUntil = now;
		}
		else if (s >= 1.0f)
		{
			h.moving = false;
			offY = NormalizeAngle(goalYaw - pev->v_angle.y);
			offP = NormalizeAngle(goalPitch - pev->v_angle.x);
			off = sqrtf(offY * offY + 1.56f * offP * offP);
			// (still off the target: a correction, slower and smaller, at most two, after the
			// moment it takes to see the miss)
			if (off > Q_max(fight ? 0.25f : 0.5f, 0.35f * width) && h.corrections < 2 && (fight || !calm || off > 2.5f))
			{
				h.corrections++;
				h.restUntil = now + RANDOM_FLOAT(0.01f, 0.03f);
			}
			else
			{
				h.corrections = 0;
				// (alert but not fighting, which a deathmatch mostly is: a look is held a moment too,
				// though shorter than a calm one)
				h.restUntil = standing ? now + RANDOM_FLOAT(0.6f, 1.6f) + 0.8f * LsTrait(90) : calm ? now + RANDOM_FLOAT(0.25f, 0.8f) + 0.6f * LsTrait(90) : fight ? now + RANDOM_FLOAT(0.02f, 0.08f) : now + RANDOM_FLOAT(0.2f, 0.55f) + 0.3f * LsTrait(90);
			}
		}
	}

	if (!h.moving)
	{
		// when to move: a big change at once; a small one only past the dead zone, after a rest
		float deadZone = fight ? Q_max(0.2f, 0.5f * width) * (1.6f - 0.8f * skill) : standing ? 6.0f + 5.0f * LsTrait(91) : calm ? 4.0f + 4.0f * LsTrait(91) : 3.5f + 2.0f * LsTrait(91);
		// (a new thing to look at, calmly: a reaction first, mostly quick, now and then slow: a
		// log-normal delay, shorter for the skilled. In a fight the stock reaction already applies)
		float jump = sqrtf(NormalizeAngle(goalYaw - h.lastY) * NormalizeAngle(goalYaw - h.lastY) + NormalizeAngle(goalPitch - h.lastP) * NormalizeAngle(goalPitch - h.lastP));
		if (!fight && jump > 20.0f)
		{
			float z = (RANDOM_FLOAT(-1.0f, 1.0f) + RANDOM_FLOAT(-1.0f, 1.0f) + RANDOM_FLOAT(-1.0f, 1.0f)) * 0.577f;
			float base = 0.12f + 0.06f * (1.0f - skill);
			float mu = -2.5f + 0.5f * (1.0f - skill), sigma = 0.4f + 0.15f * (1.0f - skill);
			h.reactUntil = now + base + expf(mu + sigma * z);
		}
		bool start = now >= h.reactUntil
			&& ((off > 25.0f) || (off > deadZone && now >= h.restUntil) || (h.corrections > 0 && off > 0.3f && now >= h.restUntil));
		if (start)
		{
			// how long (Fitts): a + b log2(1 + D/W), quicker for the skilled and in a fight
			// (the flick only has to get near: the correction does the precise part, so it's timed
			// against a looser target than the one aimed at: about 10 degrees calm, three times the
			// enemy's size in a fight. Measured against a person: about 45 deg per 0.1 s at the peak
			// of a big turn, about 5 on a small adjustment)
			// (fight 0.62, calm 0.8: batch 22 peaked at about 30 deg per 0.1 s on big fight turns and
			// 38 calm, against a person's 45)
			float b = (0.08f + 0.07f * (1.0f - skill)) * (fight ? 0.62f : calm ? 0.8f : 0.72f);
			float flickWidth = h.corrections > 0 ? width : fight ? Q_max(8.0f, 3.0f * width) : 10.0f;
			float dur = 0.1f + b * log2f(1.0f + off / flickWidth);
			// (and a hand has a top speed: a minimum-jerk stroke peaks at 1.875 times its average,
			// so a long turn takes long enough to stay under it; quicker for the skilled and in a
			// fight. Keeps the faster flicks from becoming inhuman snaps)
			// (batch 23: a person flicks past 90 deg per 0.1 s once or twice a minute; the bots
			// almost never did)
			float vmax = fight ? 700.0f + 450.0f * skill : 520.0f + 260.0f * skill;
			dur = Q_max(dur, 1.875f * off / vmax);
			if (m_lsPuppet.fast && now < m_lsPuppet.lookUntil)
				dur *= 0.6f;
			if (h.corrections > 0)
				dur = Q_max(0.05f, dur * 0.8f);
			h.dur = Q_max(0.06f, Q_min(0.65f, dur));

			// where it lands: off by a share of the distance (more for a big, quick flick and a
			// less skilled or rattled hand), usually a touch past
			float stress = (LsHealth() < 40.0f ? 0.5f : 0.0f) + (GetTimeSinceAttacked() < 1.5f ? 0.4f : 0.0f) + (now < m_lsWhiffUntil ? 0.5f : 0.0f);
			float spread = (h.corrections > 0 ? 0.04f : 0.08f) * (1.35f - skill) * (1.0f + stress);
			auto gauss = []() { return (RANDOM_FLOAT(-1.0f, 1.0f) + RANDOM_FLOAT(-1.0f, 1.0f) + RANDOM_FLOAT(-1.0f, 1.0f)) * 0.577f; };
			float along = 1.0f + (h.corrections > 0 ? 0.0f : 0.04f * (1.2f - skill)) + spread * gauss();
			float across = 0.5f * spread * gauss();
			h.y0 = pev->v_angle.y;
			h.p0 = pev->v_angle.x;
			// (along the direction of the move, and a little to the side of it)
			h.y1 = h.y0 + offY * along - offP * across;
			h.p1 = h.p0 + offP * along * 1.1f + offY * across * 0.5f;
			h.goalY = goalYaw;
			h.goalP = goalPitch;
			h.t0 = now;
			h.moving = true;
		}
	}

	h.lastY = goalYaw;
	h.lastP = goalPitch;
	if (pev->v_angle.x < -89.0f)
		pev->v_angle.x = -89.0f;
	else if (pev->v_angle.x > 89.0f)
		pev->v_angle.x = 89.0f;
	pev->v_angle.z = 0.0f;
	// (how fast it's turning, for what reads it: a flick blinds it to new things)
	m_lookYawVel = NormalizeAngle(pev->v_angle.y - oldY) / Q_max(0.001f, dt);
	m_lookPitchVel = (pev->v_angle.x - oldP) / Q_max(0.001f, dt);
}

// An area to keep out of for a while, now (a door in the way it can't open: no button for it,
// or one it couldn't get to): the next path goes round
void CCSBot::LsForceBadArea(const CNavArea *area, float secs)
{
	if (!area)
		return;
	float until = gpGlobals->time + secs;
	// (already listed: just a longer while)
	for (int i = 0; i < 4; i++)
	{
		if (m_lsBadAreas[i] == area)
		{
			m_lsBadAreaUntil[i] = Q_max(m_lsBadAreaUntil[i], until);
			return;
		}
	}
	int slot = 0;
	for (int i = 1; i < 4; i++)
	{
		if (m_lsBadAreaUntil[i] < m_lsBadAreaUntil[slot])
			slot = i;
	}
	m_lsBadAreas[slot] = area;
	m_lsBadAreaUntil[slot] = until;
}
