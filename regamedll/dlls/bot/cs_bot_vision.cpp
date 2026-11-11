/*
*
*   This program is free software; you can redistribute it and/or modify it
*   under the terms of the GNU General Public License as published by the
*   Free Software Foundation; either version 2 of the License, or (at
*   your option) any later version.
*
*   This program is distributed in the hope that it will be useful, but
*   WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*   General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software Foundation,
*   Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*   In addition, as a special exception, the author gives permission to
*   link the code of this program with the Half-Life Game Engine ("HL
*   Engine") and Modified Game Libraries ("MODs") developed by Valve,
*   L.L.C ("Valve").  You must obey the GNU General Public License in all
*   respects for all of the code used other than the HL Engine and MODs
*   from Valve.  If you modify this file, you may extend this exception
*   to your version of the file, but you are not obligated to do so.  If
*   you do not wish to do so, delete this exception statement from your
*   version.
*
*/

#include "precompiled.h"

// Used to update view angles to stay on a ladder
float StayOnLadderLine(CCSBot *me, const CNavLadder *ladder)
{
	// determine our facing
	NavDirType faceDir = AngleToDirection(me->pev->v_angle.y);
	const float stiffness = 1.0f;

	// move toward ladder mount point
	switch (faceDir)
	{
	case NORTH:
		return (stiffness * (ladder->m_top.x - me->pev->origin.x));
	case EAST:
		return (stiffness * (ladder->m_top.y - me->pev->origin.y));
	case SOUTH:
		return (-stiffness * (ladder->m_top.x - me->pev->origin.x));
	case WEST:
		return (-stiffness * (ladder->m_top.y - me->pev->origin.y));
	}

	return 0.0f;
}

// Move actual view angles towards desired ones.
// This is the only place v_angle is altered.
// TODO: Make stiffness and turn rate constants timestep invariant.
void CCSBot::UpdateLookAngles()
{
	const float deltaT = g_flBotCommandInterval;
	float maxAccel;
	float stiffness;
	float damping;

#ifdef REGAMEDLL_ADD
	// If mimicing the player, don't modify the view angles
	if (cv_bot_mimic.value > 0)
		return;
#endif

	// springs are stiffer when attacking, so we can track and move between targets better
	if (IsAttacking())
	{
		stiffness = 300.0f;
		damping = 30.0f * (LsFight() ? LsAimDamping() : 1.0f);
		maxAccel = 3000.0f;

		// Loner-Strike: a flick in two parts, as hands do: a fast, loose throw for most of a big
		// turn (the better the player, the quicker), then a slower, steadier settle for the last
		// few degrees
		if (LsFight())
		{
			float off = float(fabs(NormalizeAngle(m_lookYaw - pev->v_angle.y)));
			if (off > 10.0f)
			{
				stiffness = 280.0f + 220.0f * LsSkill();
				damping = 20.0f;
				maxAccel = 3600.0f + 1800.0f * LsSkill();
			}
			else if (off < 4.0f)
			{
				stiffness = 220.0f;
				damping = 30.0f * LsAimDamping();
			}
		}
	}
	else
	{
		stiffness = 200.0f;
		damping = 25.0f;
		maxAccel = 3000.0f;

		// Loner-Strike: nothing going on (no enemy seen, not hurt or shot at, no loud noise
		// lately): looking about is a turn of the mouse, not a flick. Anything that startles
		// it turns it as fast as before
		if (LsFight() && LsCalm())
		{
			// (a quick flick of the wrist, a touch past and back: see the hand below)
			stiffness = 340.0f;
			damping = 24.0f;
			maxAccel = 4200.0f;
		}
	}

	// these may be overridden by ladder logic
	float useYaw = m_lookYaw;
	float usePitch = m_lookPitch;

	// Loner-Strike: walking about with nobody to fight, the view stays within a strafe of
	// where it's going (people don't walk backwards looking at something behind them)
	// (only while really on the move: at a walk or a stop the direction of travel jumps about
	// and would drag the view with it)
	// (and not while falling back looking over its shoulder at the threat)
	// (nor a look at something that startled it: shots, a teammate going down, a check)
	if (LsFight() && !IsAttacking() && !IsUsingLadder() && pev->velocity.Length2D() > 150.0f && !LsLookingBack()
		&& (m_lookAtSpotPriority < PRIORITY_HIGH || LsPuppetLooking()) && gpGlobals->time >= m_lsJiggleUntil)
	{
		float moveYaw = UTIL_VecToYaw(pev->velocity);
		float off = NormalizeAngle(useYaw - moveYaw);
		// (a corner peek strafes, so it may turn further; looking about while walking doesn't)
		const float most = (m_lookAtDesc == LS_PEEK) ? 100.0f : LsPuppetLooking() ? 160.0f : 70.0f;
		if (off > most)
			useYaw = moveYaw + most;
		else if (off < -most)
			useYaw = moveYaw - most;
	}

	// Ladders require precise movement, therefore we need to look at the
	// ladder as we approach and ascend/descend it.
	// If we are on a ladder, we need to look up or down to traverse it - override pitch in this case.
	// If we're trying to break something, though, we actually need to look at it before we can
	// look at the ladder
	if (IsUsingLadder())
	{
		// set yaw to aim at ladder
		Vector to = m_pathLadder->m_top - pev->origin;
		float idealYaw = UTIL_VecToYaw(to);

		NavDirType faceDir = m_pathLadder->m_dir;

		if (m_pathLadderFaceIn)
		{
			faceDir = OppositeDirection(faceDir);
		}

		const float lookAlongLadderRange = 100.0f;
		const float ladderPitch = 60.0f;

		// adjust pitch to look up/down ladder as we ascend/descend
		switch (m_pathLadderState)
		{
		case APPROACH_ASCENDING_LADDER:
		{
			Vector to = m_goalPosition - pev->origin;
			useYaw = idealYaw;

			if (to.IsLengthLessThan(lookAlongLadderRange))
				usePitch = -ladderPitch;
			break;
		}
		case APPROACH_DESCENDING_LADDER:
		{
			Vector to = m_goalPosition - pev->origin;
			useYaw = idealYaw;

			if (to.IsLengthLessThan(lookAlongLadderRange))
				usePitch = ladderPitch;
			break;
		}
		case FACE_ASCENDING_LADDER:
		{
			useYaw = idealYaw;
			usePitch = -ladderPitch;
			break;
		}
		case FACE_DESCENDING_LADDER:
		{
			useYaw = idealYaw;
			usePitch = ladderPitch;
			break;
		}
		case MOUNT_ASCENDING_LADDER:
		case ASCEND_LADDER:
		{
			useYaw = DirectionToAngle(faceDir) + StayOnLadderLine(this, m_pathLadder);
			usePitch = -ladderPitch;

			// Loner-Strike: turned on the ladder for a diagonal climb
			LsLadderYaw(DirectionToAngle(faceDir), &useYaw);
			break;
		}
		case MOUNT_DESCENDING_LADDER:
		case DESCEND_LADDER:
		{
			useYaw = DirectionToAngle(faceDir) + StayOnLadderLine(this, m_pathLadder);
			usePitch = ladderPitch;
			break;
		}
		case DISMOUNT_ASCENDING_LADDER:
		case DISMOUNT_DESCENDING_LADDER:
		{
			useYaw = DirectionToAngle(faceDir);
			break;
		}
		}
	}

	// Loner-Strike: the view moved by a hand on a mouse, not a spring (LsHandUpdate): planned
	// flicks with a bell-shaped speed curve, timed by distance and target size, landing a
	// little off and corrected; still in between. Ladders keep the stock spring
	if (LsFight() && !IsUsingLadder())
	{
		// (on the move and not fighting, the view stays within 90 degrees of where it's
		// going: people check as they go, they don't run backwards at a site. Over the
		// shoulder (to 140) only as a glance, under half a second and not straight after
		// another, about as often as the person in the playtest (10% of running time past
		// 100 degrees). A deliberate look back, or a real threat, still turns it all the way)
		float speed = float(pev->velocity.Length2D());
		// (its own "checking" looks, a spot behind it among them, are kept in too: on the run
		// they're a glance; it stops for a long one)
		bool lookBack = m_lookAtDesc && Q_strcmp(m_lookAtDesc, "Loner-Strike: looking back") == 0;
		bool checking = m_lookAtDesc && Q_strcmp(m_lookAtDesc, "Loner-Strike: checking") == 0;
		// (a noise behind: one right on it, 500 units or less, turns it all the way; one
		// further off is a look over the shoulder, as a person running somewhere would. The
		// stock look held the view on it 1-2 s while it ran on: running backwards)
		bool farNoise = m_lookAtDesc && Q_strcmp(m_lookAtDesc, "Noise") == 0
			&& (m_noisePosition - pev->origin).Make2D().IsLengthGreaterThan(500.0f);
		if (speed > 150.0f && !IsAttacking() && !IsEnemyVisible() && !lookBack
			&& !(m_lookAtSpotState != NOT_LOOKING_AT_SPOT && m_lookAtSpotPriority >= PRIORITY_HIGH && !checking && !farNoise))
		{
			float heading = UTIL_VecToYaw(pev->velocity);
			float off = NormalizeAngle(useYaw - heading);
			float now = gpGlobals->time;
			// a noise further off, away from its way: "huh?", a look straight at it for a
			// third to two thirds of a second, then back to where it's going and the noise let
			// go (once a noise: the same one heard again doesn't turn it again)
			bool huh = false;
			if (farNoise && Q_abs(off) > 90.0f)
			{
				if (m_lsHuhUntil <= 0.0f && m_lsHuhNoise != m_noiseTimestamp)
				{
					m_lsHuhNoise = m_noiseTimestamp;
					// whether it looks at all is who it is: careful, lurking and skilled ones
					// check sounds; a rusher set on where it's going mostly doesn't; and each
					// has its own habit (trait 96)
					int style = LsStyle();
					float p = 0.55f + 0.25f * LsSkill() - 0.35f * LsAggro() + 0.3f * (LsTrait(96) - 0.5f);
					if (style == LS_STYLE_CAREFUL || style == LS_STYLE_LURKER || style == LS_STYLE_NINJA || style == LS_STYLE_CAMPER)
						p += 0.2f;
					else if (style == LS_STYLE_RUSHER || style == LS_STYLE_KNIFER)
						p -= 0.25f;
					p = Q_max(0.1f, Q_min(0.95f, p));
					if (RANDOM_FLOAT(0.0f, 1.0f) < p)
						m_lsHuhUntil = now + RANDOM_FLOAT(0.35f, 0.7f);
				}
				if (m_lsHuhUntil > 0.0f && now < m_lsHuhUntil)
					huh = true;
				else
				{
					m_lsHuhUntil = 0.0f;
					ClearLookAt();
				}
			}
			else if (!farNoise)
				m_lsHuhUntil = 0.0f;
			if (huh)
				m_lsBehindSince = 0.0f;
			else if (Q_abs(off) <= 90.0f)
				m_lsBehindSince = 0.0f;
			else
			{
				if (m_lsBehindSince <= 0.0f && now >= m_lsBehindNext)
				{
					m_lsBehindSince = now;
					// (how often is who it is: careful ones, lurkers and ninjas check behind
					// often, rushers and knifers hardly; each its own habit, trait 97)
					float every = RANDOM_FLOAT(1.5f, 3.0f) * (1.3f - 0.6f * LsTrait(97));
					int style = LsStyle();
					if (style == LS_STYLE_CAREFUL || style == LS_STYLE_LURKER || style == LS_STYLE_NINJA)
						every *= 0.6f;
					else if (style == LS_STYLE_RUSHER || style == LS_STYLE_KNIFER)
						every *= 1.8f;
					m_lsBehindNext = now + every;
				}
				bool glance = m_lsBehindSince > 0.0f && now - m_lsBehindSince < 0.45f;
				float limit = glance ? 140.0f : 90.0f;
				if (off > limit)
					useYaw = heading + limit;
				else if (off < -limit)
					useYaw = heading - limit;
			}
		}
		else
			m_lsBehindSince = 0.0f;
		LsHandUpdate(useYaw, usePitch, deltaT);
		return;
	}

	// Yaw
	float angleDiff = NormalizeAngle(useYaw - pev->v_angle.y);

	// if almost at target angle, snap to it
	const float onTargetTolerance = 1.0f;
	// (Loner-Strike: only on a ladder; elsewhere it settles like a hand on a mouse)
	if (angleDiff < onTargetTolerance && angleDiff > -onTargetTolerance && (!LsFight() || IsUsingLadder() || fabs(m_lookYawVel) < 3.0f))
	{
		m_lookYawVel = 0.0f;
		pev->v_angle.y = useYaw;
	}
	else
	{
		// simple angular spring/damper
		float accel = stiffness * angleDiff - damping * m_lookYawVel;

		// limit rate
		if (accel > maxAccel)
			accel = maxAccel;

		else if (accel < -maxAccel)
			accel = -maxAccel;

		m_lookYawVel += deltaT * accel;
		pev->v_angle.y += deltaT * m_lookYawVel;
	}

	// Pitch
	// Actually, this is negative pitch.
	angleDiff = usePitch - pev->v_angle.x;

	angleDiff = NormalizeAngle(angleDiff);

	if (false && angleDiff < onTargetTolerance && angleDiff > -onTargetTolerance)
	{
		m_lookPitchVel = 0.0f;
		pev->v_angle.x = usePitch;
	}
	else
	{
		// simple angular spring/damper
		// double the stiffness since pitch is only +/- 90 and yaw is +/- 180
		// (Loner-Strike: a little less: up and down is the slower way for a wrist)
		float accel = (LsFight() ? 1.6f : 2.0f) * stiffness * angleDiff - damping * m_lookPitchVel;

		// limit rate
		if (accel > maxAccel)
			accel = maxAccel;

		else if (accel < -maxAccel)
			accel = -maxAccel;

		m_lookPitchVel += deltaT * accel;
		pev->v_angle.x += deltaT * m_lookPitchVel;
	}

	// limit range - avoid gimbal lock
	if (pev->v_angle.x < -89.0f)
		pev->v_angle.x = -89.0f;
	else if (pev->v_angle.x > 89.0f)
		pev->v_angle.x = 89.0f;

	pev->v_angle.z = 0.0f;
}

// Return true if we can see the point
bool CCSBot::IsVisible(const Vector *pos, bool testFOV) const
{
	// we can't see anything if we're blind
	if (IsBlind())
		return false;

	// is it in my general viewcone?
	if (testFOV && !(const_cast<CCSBot *>(this)->FInViewCone(pos)))
		return false;

	// check line of sight against smoke
	if (TheCSBots()->IsLineBlockedBySmoke(&GetEyePosition(), pos))
		return false;

	// check line of sight
	// Must include CONTENTS_MONSTER to pick up all non-brush objects like barrels
	TraceResult result;
	UTIL_TraceLine(GetEyePosition(), *pos, ignore_monsters, ignore_glass, ENT(pev), &result);

	if (result.flFraction != 1.0f)
		return false;

	// Loner-Strike: traces pass through sky brushes, eyes don't (inferno's rooftops): on a map
	// with sky between places, a line through it isn't seen through
	if (LsFight() && LsSkyInPlay() && (GetEyePosition() - *pos).IsLengthGreaterThan(300.0f) && LsCrossesSky(GetEyePosition(), *pos, 96.0f))
		return false;

	return true;
}

// Return true if we can see any part of the player
// Check parts in order of importance. Return the first part seen in "visParts" if it is non-NULL.
bool CCSBot::IsVisible(CBasePlayer *pPlayer, bool testFOV, unsigned char *visParts) const
{
#ifdef REGAMEDLL_ADD // REGAMEDLL_FIXES ?
	if ((pPlayer->pev->flags & FL_NOTARGET) || (pPlayer->pev->effects & EF_NODRAW))
		return false;
#endif

	Vector spot = pPlayer->pev->origin;
	unsigned char testVisParts = NONE;

	// finish chest check
	if (IsVisible(&spot, testFOV))
		testVisParts |= CHEST;

	// check top of head
	spot = spot + Vector(0, 0, 25.0f);

	if (IsVisible(&spot, testFOV))
		testVisParts |= HEAD;

	// check feet
	const float standFeet = 34.0f;
	const float crouchFeet = 14.0f;

	if (pPlayer->pev->flags & FL_DUCKING)
		spot.z = pPlayer->pev->origin.z - crouchFeet;
	else
		spot.z = pPlayer->pev->origin.z - standFeet;

	// check feet
	if (IsVisible(&spot, testFOV))
		testVisParts |= FEET;

	// check "edges"
	const float edgeOffset = 13.0f;
	Vector2D dir = (pPlayer->pev->origin - pev->origin).Make2D();
	dir.NormalizeInPlace();

	Vector2D perp(-dir.y, dir.x);

	spot = pPlayer->pev->origin + Vector(perp.x * edgeOffset, perp.y * edgeOffset, 0);

	if (IsVisible(&spot, testFOV))
		testVisParts |= LEFT_SIDE;

	spot = pPlayer->pev->origin - Vector(perp.x * edgeOffset, perp.y * edgeOffset, 0);

	if (IsVisible(&spot, testFOV))
		testVisParts |= RIGHT_SIDE;

	if (visParts)
		*visParts = testVisParts;

	if (testVisParts != NONE)
	{
		// (Loner-Strike: its enemy seen with no smoke in the way, for the smoke rule below)
		if (LsFight())
			const_cast<CCSBot *>(this)->LsSawClear(pPlayer);
		return true;
	}

	// Loner-Strike: hidden only by smoke: an enemy it was on before the smoke, or one
	// shooting at it from in there, it still shoots at (for a moment)
	if (LsFight() && const_cast<CCSBot *>(this)->LsSeeIntoSmoke(pPlayer, testFOV, visParts))
		return true;

	return false;
}

bool CCSBot::IsEnemyPartVisible(VisiblePartType part) const
{
	if (!IsEnemyVisible())
		return false;

	return (m_visibleEnemyParts & part) != 0;
}

void CCSBot::UpdateLookAt()
{
	Vector to = m_lookAtSpot - EyePosition();
	Vector idealAngle = UTIL_VecToAngles(to);
	idealAngle.x = 360.0f - idealAngle.x;

	SetLookAngles(idealAngle.y, idealAngle.x);
}

// Look at the given point in space for the given duration (-1 means forever)
void CCSBot::SetLookAt(const char *desc, const Vector *pos, PriorityType pri, float duration, bool clearIfClose, float angleTolerance)
{
	if (!pos)
		return;

	// Loner-Strike: nobody walks into a new area sideways or backwards. Walking, for the first
	// couple of seconds in an area, a look around (low priority: a hiding spot, an approach
	// point, a corner) well behind the way it's going is let go; a reaction (a noise, an enemy,
	// being shot at: medium and up) still turns it
	// (a really weak player - some newbies, always the same ones - still looks back going in)
	bool lsSloppy = LsSkill() < 0.3f && LsTrait(52) < 0.35f;
	if (pri == PRIORITY_LOW && LsFight() && !lsSloppy && !IsNotMoving() && !IsAttacking() && (gpGlobals->time - m_lsScanInAt < 2.0f || LsAboutToEnter()))
	{
		float heading = UTIL_VecToYaw(pev->velocity);
		if (Q_abs(NormalizeAngle(UTIL_VecToYaw(*pos - pev->origin) - heading)) > 100.0f)
			return;
	}

	// if currently looking at a point in space with higher priority, ignore this request
	if (m_lookAtSpotState != NOT_LOOKING_AT_SPOT && m_lookAtSpotPriority > pri)
		return;

	// Loner-Strike: no looking up at nothing. A spot well above eye level is only worth it
	// where someone could actually stand up there - a ledge, a window, a balcony: below the
	// spot there's floor within about a player's height. Out in the open (long A, the sky over
	// a courtyard) the look comes down to head height on the same line. Enemies and grenades
	// in the air aren't touched: only looks around (low and medium priority)
	Vector lsLevelled;
	if (LsFight() && pri <= PRIORITY_MEDIUM)
	{
		Vector eye = pev->origin + pev->view_ofs;
		Vector to = *pos - eye;
		float across = float(to.Length2D());
		if (to.z > 48.0f && to.z > 0.35f * across)
		{
			TraceResult tr;
			UTIL_TraceLine(*pos, *pos - Vector(0, 0, 110.0f), ignore_monsters, ENT(pev), &tr);
			bool ledge = tr.flFraction < 1.0f && !tr.fAllSolid && !tr.fStartSolid;
			if (!ledge)
			{
				lsLevelled = *pos;
				lsLevelled.z = eye.z + Q_min(to.z, 0.1f * across);
				pos = &lsLevelled;
			}
		}
	}

	// if already looking at this spot, just extend the time
	const float tolerance = 10.0f;
	if (m_lookAtSpotState != NOT_LOOKING_AT_SPOT && VectorsAreEqual(pos, &m_lookAtSpot, tolerance))
	{
		m_lookAtSpotDuration = duration;

		if (m_lookAtSpotPriority < pri)
			m_lookAtSpotPriority = pri;
	}
	else
	{
		// look at new spot
		m_lookAtSpot = *pos;
		m_lookAtSpotState = LOOK_TOWARDS_SPOT;
		m_lookAtSpotDuration = duration;
		m_lookAtSpotPriority = pri;
	}

	m_lookAtSpotAngleTolerance = angleTolerance;
	m_lookAtSpotClearIfClose = clearIfClose;
	m_lookAtDesc = desc;
}

// Block all "look at" and "look around" behavior for given duration - just look ahead
void CCSBot::InhibitLookAround(float duration)
{
	m_inhibitLookAroundTimestamp = gpGlobals->time + duration;
}

// Update enounter spot timestamps, etc
void CCSBot::UpdatePeripheralVision()
{
	// if we update at 10Hz, this ensures we test once every three
	const float peripheralUpdateInterval = 0.29f;
	if (gpGlobals->time - m_peripheralTimestamp < peripheralUpdateInterval)
		return;

	m_peripheralTimestamp = gpGlobals->time;

	if (m_spotEncounter)
	{
		// check LOS to all spots in case we see them with our "peripheral vision"
		Vector pos;
		for (auto &spotOrder : m_spotEncounter->spotList)
		{
			const Vector *spotPos = spotOrder.spot->GetPosition();

			pos.x = spotPos->x;
			pos.y = spotPos->y;
			pos.z = spotPos->z + HalfHumanHeight;

			if (!IsVisible(&pos, CHECK_FOV))
				continue;

			// can see hiding spot, remember when we saw it last
			SetHidingSpotCheckTimestamp(spotOrder.spot);
		}
	}
}

// Update the "looking around" behavior.
// Loner-Strike: whether a person could see from `from` to `to`: glass is looked through, and a
// door or a breakable window doesn't count as a wall (an angle on a door is held); only solid
// walls block. How far it got: 1 all the way.
static float LsClearTo(const Vector &from, const Vector &to, edict_t *self)
{
	Vector start = from;
	edict_t *skip = self;
	for (int step = 0; step < 4; step++)
	{
		TraceResult tr;
		UTIL_TraceLine(start, to, ignore_monsters, ignore_glass, skip, &tr);
		if (tr.flFraction >= 1.0f)
			return 1.0f;
		CBaseEntity *pHit = tr.pHit ? CBaseEntity::Instance(tr.pHit) : nullptr;
		bool seeThrough = pHit && (FClassnameIs(pHit->pev, "func_door") || FClassnameIs(pHit->pev, "func_door_rotating") || FClassnameIs(pHit->pev, "func_breakable"));
		if (!seeThrough)
		{
			float total = float((to - from).Length());
			return total > 1.0f ? float((tr.vecEndPos - from).Length()) / total : 1.0f;
		}
		// (on from where it hit, that door or window left out)
		start = tr.vecEndPos;
		skip = tr.pHit;
	}
	return 1.0f;
}

void CCSBot::UpdateLookAround(bool updateNow)
{
	// check if looking around has been inhibited
	// Moved inhibit to allow high priority enemy lookats to still occur
	if (gpGlobals->time < m_inhibitLookAroundTimestamp)
		return;

	const float recentThreatTime = 0.25f; // 1.0f;

	// Unless we can hear them moving, in which case look towards the noise
	if (!IsEnemyVisible())
	{
		const float noiseStartleRange = 1000.0f;
		if (CanHearNearbyEnemyGunfire(noiseStartleRange))
		{
			Vector spot = m_noisePosition;
			spot.z += HalfHumanHeight + (LsFight() ? LsLookHeight() : 0.0f);	// (Loner-Strike: head height for good players)
			LsLookableSpot(&spot);

			SetLookAt("Check dangerous noise", &spot, PRIORITY_HIGH, recentThreatTime);
			InhibitLookAround(RANDOM_FLOAT(2.0f, 4.0f));

			return;
		}
	}

	// If we recently saw an enemy, look towards where we last saw them
	if (!IsLookingAtSpot(PRIORITY_MEDIUM) && gpGlobals->time - m_lastSawEnemyTimestamp < recentThreatTime)
	{
		ClearLookAt();

		Vector spot = m_lastEnemyPosition;

		// find enemy position on the ground
		if (GetSimpleGroundHeight(&m_lastEnemyPosition, &spot.z))
		{
			spot.z += HalfHumanHeight + (LsFight() ? LsLookHeight() : 0.0f);	// (Loner-Strike: head height for good players)
			// (gone behind cover: the edge they went round, not the wall in front of them)
			LsLookableSpot(&spot);
			SetLookAt("Last Enemy Position", &spot, PRIORITY_MEDIUM, RANDOM_FLOAT(2.0f, 3.0f), true);
			return;
		}
	}

	// Look at nearby enemy noises
	if (UpdateLookAtNoise())
		return;

	if (IsNotMoving())
	{
		// if we're sniping, zoom in to watch our approach points
		if (IsUsingSniperRifle())
		{
			// low skill bots don't pre-zoom
			if (GetProfile()->GetSkill() > 0.4f)
			{
				if (!IsViewMoving())
				{
					float range = ComputeWeaponSightRange();
					// (Loner-Strike: scoped only holding an angle someone could come down - a long
					// clear view, from a spot or a perch, once the round's under way; not at a box in
					// spawn)
					if (LsFight() && (range < 600.0f || IsSafe() || !(IsHiding() || GetTask() == MOVE_TO_SNIPER_SPOT)))
					{
						if (GetZoomLevel() != NO_ZOOM)
							SecondaryAttack();
					}
					else
						AdjustZoom(range);
				}
				else
				{
					// zoom out
					if (GetZoomLevel() != NO_ZOOM)
						SecondaryAttack();
				}
			}
		}

		if (!m_lastKnownArea)
			return;

		// (Loner-Strike: holding a spot, its angles from the map knowledge first - and with them
		// known, nothing else: the stock approach points are for maps it hasn't studied)
		if (LsScanAngles(true))
			return;
		{
			const LsAreaKnow *lk = LsFight() ? LsKnow(m_lastKnownArea) : nullptr;
			if (lk && lk->angleCount > 0)
				return;
		}

		if (gpGlobals->time < m_lookAroundStateTimestamp)
			return;

		// if we're sniping, switch look-at spots less often
		if (IsUsingSniperRifle())
			m_lookAroundStateTimestamp = gpGlobals->time + RANDOM_FLOAT(5.0f, 10.0f);
		// (Loner-Strike: a person holding a spot checks the openings in turn, a couple of seconds
		// each, a little longer the more patient they are)
		else if (LsFight())
		{
			// (campers and awpers sit on an angle longer, careful ones move on sooner)
			float hold = RANDOM_FLOAT(1.8f, 4.0f) + 1.2f * LsTrait(90);
			int style = LsStyle();
			if (style == LS_STYLE_CAMPER || style == LS_STYLE_AWPER)
				hold *= 1.5f;
			else if (style == LS_STYLE_CAREFUL)
				hold *= 0.75f;
			m_lookAroundStateTimestamp = gpGlobals->time + hold;
		}
		else
			m_lookAroundStateTimestamp = gpGlobals->time + RANDOM_FLOAT(1.0f, 2.0f);

		if (m_approachPointCount == 0)
		{
			ClearLookAt();
			return;
		}

		int which = RANDOM_LONG(0, m_approachPointCount - 1);
		// (Loner-Strike: only ways in it can actually see from here, not a wall in its face or
		// one round a corner it can't see past (people don't stare at a wall); mostly the one
		// nearest to where it's looking, a small move, now and then a bigger check. None in
		// view: down the longest open line from here instead, never a wall)
		if (LsFight())
		{
			int seen[MAX_APPROACH_POINTS];
			int count = 0;
			Vector eye = GetEyePosition();
			for (int i = 0; i < m_approachPointCount; i++)
			{
				Vector spot = m_approachPoint[i] + Vector(0, 0, HalfHumanHeight);
				float dist = float((spot - eye).Length());
				// (a doorway right beside a tight hold counts: the line-of-sight test is what
				// keeps walls out)
				if (dist < 64.0f)
					continue;
				if (LsClearTo(eye, spot, ENT(pev)) >= 0.95f)
					seen[count++] = i;
			}
			if (count > 0)
			{
				auto turn = [this](int i) {
					Vector to = m_approachPoint[i] - pev->origin;
					return Q_abs(NormalizeAngle(UTIL_VecToYaw(to) - pev->v_angle.y));
				};
				// (never the one it just watched, when there are others: checking them in turn)
				int pick[MAX_APPROACH_POINTS];
				int picks = 0;
				for (int k = 0; k < count; k++)
				{
					if (seen[k] != m_lsLastApproach || count == 1)
						pick[picks++] = seen[k];
				}
				which = pick[RANDOM_LONG(0, picks - 1)];
				// (the neighbouring one mostly: 70% of the time; a new player's checks are
				// all over the place, a good one's tidier)
				float tidy = 0.45f + 0.4f * LsSkill();
				if (picks > 1 && RANDOM_FLOAT(0.0f, 1.0f) < tidy)
				{
					int other = pick[RANDOM_LONG(0, picks - 1)];
					if (turn(other) < turn(which))
						which = other;
				}
				m_lsLastApproach = which;
			}
			else
			{
				// (the longest clear line of the eight round it, the ones nearer where it's
				// already looking counting a little longer: a corridor, a doorway, across a site)
				float best = -1.0f;
				Vector bestAt = eye;
				for (int k = 0; k < 8; k++)
				{
					float yaw = pev->v_angle.y + 45.0f * float(k);
					Vector dir(BotCOS(yaw), BotSIN(yaw), 0.0f);
					float open = 2000.0f * LsClearTo(eye, eye + dir * 2000.0f, ENT(pev));
					float score = open - 150.0f * Q_abs(NormalizeAngle(yaw - pev->v_angle.y)) / 45.0f;
					if (score > best)
					{
						best = score;
						bestAt = eye + dir * Q_min(open, 600.0f);
					}
				}
				if (best > 250.0f)
				{
					SetLookAt("Approach Point (Hiding)", &bestAt, PRIORITY_LOW);
					return;
				}
			}
		}
		Vector spot = m_approachPoint[which];

		// don't look at the floor, look roughly at chest level
		// TODO: If this approach point is very near, this will cause us to aim up in the air if were crouching
		spot.z += HalfHumanHeight + (LsFight() ? LsLookHeight() : 0.0f);	// (Loner-Strike: head height for good players)
		SetLookAt("Approach Point (Hiding)", &spot, PRIORITY_LOW);
		return;
	}

	// Loner-Strike: moving, the area's angles from the map knowledge (where an enemy could
	// come from) before anything else
	if (!IsSafe() && LsScanAngles(false))
		return;

	// Loner-Strike: and the corners it can feel out from the walls round it as it moves (before the encounter spots, whose
	// timer returns early most frames): where
	// a near surface (a box, a doorway's side, a tunnel's bend) gives way to open space beyond,
	// someone could be just past the edge. A flat wall has no edge and is never looked at
	if (LsFight() && !IsNotMoving() && !IsSafe() && !IsLookingAtSpot(PRIORITY_LOW) && gpGlobals->time >= m_lsCornerCheckAt)
	{
		// how often: the careful and campers check more, rushers less, newer players less
		float skill = LsSkill(), aggro = LsAggro();
		int style = LsStyle();
		float gap = RANDOM_FLOAT(0.8f, 1.5f) + 1.2f * (1.0f - skill);
		if (style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER || style == LS_STYLE_NINJA)
			gap *= 0.7f;
		else if (style == LS_STYLE_RUSHER || aggro > 0.75f)
			gap *= 1.35f;
		m_lsCornerCheckAt = gpGlobals->time + gap;
		// (a newer player misses checks; a good one rarely does)
		Vector corner;
		if (RANDOM_FLOAT(0.0f, 1.0f) < 0.35f + 0.65f * skill && LsCornerAhead(&corner))
		{
			// a newer player's eyes land roughly on it (low, off to a side); a good one's crosshair
			// sits on the edge at head height, and stays there while it walks past (slicing the pie)
			float sloppy = 1.0f - skill;
			corner.x += RANDOM_FLOAT(-50.0f, 50.0f) * sloppy;
			corner.y += RANDOM_FLOAT(-50.0f, 50.0f) * sloppy;
			corner.z = pev->origin.z + LsLookHeight();	// (chest for a newer player, head for a good one)
			float hold = 0.25f + 0.7f * skill;
			if (style == LS_STYLE_CAREFUL || style == LS_STYLE_NINJA)
				hold *= 1.3f;
			SetLookAt("Loner-Strike: corner", &corner, PRIORITY_LOW, hold, true, 10.0f);
			return;
		}
	}

	// Glance at "encouter spots" as we move past them
	if (m_spotEncounter)
	{
		// Check encounter spots
		if (!IsSafe() && !IsLookingAtSpot(PRIORITY_LOW))
		{
			// allow a short time to look where we're going
			if (gpGlobals->time < m_spotCheckTimestamp)
				return;

			// TODO: Use skill parameter instead of accuracy

			// lower skills have exponentially longer delays
			real_t asleep = (1.0f - GetProfile()->GetSkill());
			asleep *= asleep;
			asleep *= asleep;

			m_spotCheckTimestamp = gpGlobals->time + asleep * RANDOM_FLOAT(10.0f, 30.0f);
			// (Loner-Strike: even the sharpest player gives each check a moment, and looks where
			// they're going in between, not a flick from wall to wall every frame)
			if (LsFight())
				m_spotCheckTimestamp = Q_max(m_spotCheckTimestamp, gpGlobals->time + RANDOM_FLOAT(0.7f, 1.4f));

			// figure out how far along the path segment we are
			Vector delta = m_spotEncounter->path.to - m_spotEncounter->path.from;
			real_t length = delta.Length();

#ifdef REGAMEDLL_FIXES
			float adx = Q_abs(delta.x);
			float ady = Q_abs(delta.y);
#else
			float adx = float(Q_abs(int64(delta.x)));
			float ady = float(Q_abs(int64(delta.y)));
#endif
			real_t t;

			if (adx > ady)
				t = (pev->origin.x - m_spotEncounter->path.from.x) / delta.x;
			else
				t = (pev->origin.y - m_spotEncounter->path.from.y) / delta.y;

			// advance parameter a bit so we "lead" our checks
			const float leadCheckRange = 50.0f;
			t += leadCheckRange / length;

			if (t < 0.0f)
				t = 0.0f;
			else if (t > 1.0f)
				t = 1.0f;

			// collect the unchecked spots so far
			const int MAX_DANGER_SPOTS = 8;
			HidingSpot *dangerSpot[MAX_DANGER_SPOTS];
			int dangerSpotCount = 0;
			int dangerIndex = 0;

			const float checkTime = 10.0f;

			for (auto &spotOrder : m_spotEncounter->spotList)
			{
				// if we have seen this spot recently, we don't need to look at it
				if (gpGlobals->time - GetHidingSpotCheckTimestamp(spotOrder.spot) <= checkTime)
					continue;

				if (spotOrder.t > t)
					break;

				// (Loner-Strike: not a nook right beside it, nor one behind it: roughly ahead or to
				// the side of where it's going; one it can't see is checked round its cover below)
				if (LsFight())
				{
					Vector eye = GetEyePosition();
					Vector pos = *spotOrder.spot->GetPosition() + Vector(0, 0, HalfHumanHeight);
					Vector to = pos - eye;
					if (to.Length2D() < 160.0f)
						continue;
					Vector ahead = m_spotEncounter->path.to - m_spotEncounter->path.from;
					if (ahead.Length2D() > 1.0f && Q_abs(NormalizeAngle(UTIL_VecToYaw(to) - UTIL_VecToYaw(ahead))) > 110.0f)
						continue;
				}

				dangerSpot[dangerIndex++] = spotOrder.spot;
				if (dangerIndex >= MAX_DANGER_SPOTS)
					dangerIndex = 0;

				if (dangerSpotCount < MAX_DANGER_SPOTS)
					dangerSpotCount++;
			}

			if (dangerSpotCount)
			{
				// pick one of the spots at random
				int which = RANDOM_LONG(0, dangerSpotCount - 1);

				const Vector *checkSpot = dangerSpot[which]->GetPosition();

				Vector pos = *checkSpot;
				pos.z += HalfHumanHeight;

				// Loner-Strike: a spot behind cover (a box, a corner) is checked where it can be
				// seen from: the edge of that cover, the way a person pies a corner, never the box
				// face or the wall in between. None to see it round: it's left
				bool look = true;
				if (LsFight())
				{
					Vector eye = GetEyePosition();
					if (LsClearTo(eye, pos, ENT(pev)) < 0.95f)
					{
						Vector bend;
						look = BendLineOfSight(&eye, &pos, &bend) && (bend - eye).Length2D() > 96.0f;
						if (look)
						{
							bend.z = Q_max(bend.z, pos.z);
							pos = bend;
						}
					}
				}

				if (look)
				{
					// glance at the spot for minimum time
					SetLookAt("Encounter Spot", &pos, PRIORITY_LOW, 0, true, 10.0f);

					// Loner-Strike: or put a few bullets into it first (some, up close)
					LsPrefire(pos);
				}

				// immediately mark it as "checked", so we don't check it again
				// if we get distracted before we check it - that's the way it goes
				SetHidingSpotCheckTimestamp(dangerSpot[which]);
				if (look)
					return;
			}
		}
	}

}

// Loner-Strike: the nearest edge ahead (within about 100 degrees of where it's going) with
// open space past it, at waist height (a crate) or eye height (a doorway); not one it checked
// a moment ago. Where to look: just past the edge, at head height.
bool CCSBot::LsCornerAhead(Vector *out)
{
	const int N = 27;                       // -104 .. +104 degrees, 8 apart
	const float RANGE = 1200.0f;
	float now = gpGlobals->time;
	Vector eye = GetEyePosition();
	float base = pev->velocity.Length2D() > 60.0f ? UTIL_VecToYaw(pev->velocity) : pev->v_angle.y;

	// (from the map knowledge when it has this area's corners: one sight check, not a fan of
	// probes; the probes below are for maps it hasn't studied yet)
	const LsAreaKnow *lk = m_lastKnownArea ? LsKnow(m_lastKnownArea) : nullptr;
	if (lk && lk->cornerCount > 0)
	{
		float bestK = -1.0f;
		Vector bestAt;
		for (int c = 0; c < lk->cornerCount; c++)
		{
			const LsKnownCorner &kc = lk->corners[c];
			Vector at(kc.x, kc.y, kc.z);
			Vector to = at - pev->origin;
			float dist = float(to.Length2D());
			if (dist < 140.0f || dist > 1000.0f)
				continue;
			if (Q_abs(NormalizeAngle(UTIL_VecToYaw(to) - base)) > 104.0f)
				continue;
			if (now - m_lsGlanceCornerAt < 4.0f && (at - m_lsGlanceCorner).IsLengthLessThan(128.0f))
				continue;
			// (just past the edge, the way it opens, at head height)
			float open = float(kc.yaw) * (360.0f / 256.0f);
			Vector look = at + Vector(BotCOS(open) * 40.0f, BotSIN(open) * 40.0f, HalfHumanHeight + LsLookHeight());
			float score = 1000.0f - dist;
			if (GetTimeSinceLastSawEnemy() < 15.0f && (at - GetLastKnownEnemyPosition()).IsLengthLessThan(800.0f))
				score += 600.0f;
			if (score <= bestK)
				continue;
			TraceResult tr;
			UTIL_TraceLine(eye, look, ignore_monsters, ignore_glass, ENT(pev), &tr);
			if (tr.flFraction < 1.0f)
				continue;
			bestK = score;
			bestAt = look;
		}
		if (bestK >= 0.0f)
		{
			m_lsGlanceCorner = bestAt;
			m_lsGlanceCornerAt = now;
			*out = bestAt;
			return true;
		}
		return false;
	}
	float heights[2] = { pev->origin.z, eye.z };
	int style = LsStyle();
	bool rusher = style == LS_STYLE_RUSHER || LsAggro() > 0.8f;
	bool longLines = style == LS_STYLE_CAMPER || style == LS_STYLE_AWPER;
	bool recentEnemy = GetTimeSinceLastSawEnemy() < 15.0f;
	Vector enemyAt = GetLastKnownEnemyPosition();
	float bestScore = -1.0f;
	Vector best;
	for (int h = 0; h < 2; h++)
	{
		float d[N];
		Vector from(eye.x, eye.y, heights[h]);
		for (int i = 0; i < N; i++)
		{
			float yaw = base + 8.0f * float(i - N / 2);
			Vector dir(BotCOS(yaw), BotSIN(yaw), 0.0f);
			d[i] = RANGE * LsClearTo(from, from + dir * RANGE, ENT(pev));
		}
		for (int i = 0; i + 1 < N; i++)
		{
			float nearD = Q_min(d[i], d[i + 1]), farD = Q_max(d[i], d[i + 1]);
			// (a real gap past the edge; not a wall at its shoulder, nor one far off)
			if (farD - nearD < 220.0f || nearD < 140.0f || nearD > 1000.0f)
				continue;
			float turn = 8.0f * (float(i - N / 2) + 0.5f);
			// (a rusher looks where it's going: edges well off to the side are left)
			if (rusher && Q_abs(turn) > 55.0f)
				continue;
			float yaw = base + turn;
			Vector dir(BotCOS(yaw), BotSIN(yaw), 0.0f);
			Vector at = eye + dir * (nearD + 24.0f);
			at.z = eye.z;
			if (now - m_lsGlanceCornerAt < 4.0f && (at - m_lsGlanceCorner).IsLengthLessThan(128.0f))
				continue;
			// (a corner a teammate is at, or just came from, is covered)
			bool covered = false;
			for (int m = 1; m <= gpGlobals->maxClients && !covered; m++)
			{
				CBasePlayer *mate = UTIL_PlayerByIndex(m);
				if (mate && mate != this && mate->IsAlive() && BotRelationship(mate) == BOT_TEAMMATE && (mate->pev->origin - at).IsLengthLessThan(250.0f))
					covered = true;
			}
			if (covered)
				continue;
			// (the nearest first, those nearer straight ahead a little before; one near where an
			// enemy was seen or heard lately well before the rest; a camper or awper favours the
			// edges with a long way open past them)
			float score = 1000.0f / nearD - 0.004f * Q_abs(turn);
			if (recentEnemy && (at - enemyAt).IsLengthLessThan(700.0f))
				score += 6.0f;
			if (longLines)
				score += farD / 600.0f;
			if (score > bestScore)
			{
				bestScore = score;
				best = at;
			}
		}
	}
	if (bestScore < 0.0f)
		return false;
	m_lsGlanceCorner = best;
	m_lsGlanceCornerAt = now;
	*out = best;
	return true;
}

// "Bend" our line of sight around corners until we can "see" the point.
bool CCSBot::BendLineOfSight(const Vector *eye, const Vector *point, Vector *bend) const
{
	// if we can directly see the point, use it
	TraceResult result;
	UTIL_TraceLine(*eye, *point + Vector(0, 0, HalfHumanHeight), ignore_monsters, ENT(pev), &result);

	if (result.flFraction == 1.0f && !result.fStartSolid)
	{
		// can directly see point, no bending needed
		*bend = *point;
		return true;
	}

	// "bend" our line of sight until we can see the approach point
	Vector v = *point - *eye;
	float startAngle = UTIL_VecToYaw(v);
	float length = v.Length2D();
	v.NormalizeInPlace();

	float angleInc = 10.0f;
	for (float angle = angleInc; angle <= 135.0f; angle += angleInc)
	{
		// check both sides at this angle offset
		for (int side = 0; side < 2; side++)
		{
			float actualAngle = side ? (startAngle + angle) : (startAngle - angle);

			float dx = BotCOS(actualAngle);
			float dy = BotSIN(actualAngle);

			// compute rotated point ray endpoint
			Vector rotPoint(eye->x + length * dx, eye->y + length * dy, point->z);

			TraceResult result;
			UTIL_TraceLine(*eye, rotPoint + Vector(0, 0, HalfHumanHeight), ignore_monsters, ENT(pev), &result);

			// if this ray started in an obstacle, skip it
			if (result.fStartSolid)
			{
				continue;
			}

			Vector ray = rotPoint - *eye;
			float rayLength = ray.NormalizeInPlace();
			float visibleLength = rayLength * result.flFraction;

			// step along ray, checking if point is visible from ray point
			const float bendStepSize = 50.0f;
			for (float bendLength = bendStepSize; bendLength <= visibleLength; bendLength += bendStepSize)
			{
				// compute point along ray
				Vector rayPoint = *eye + bendLength * ray;

				// check if we can see approach point from this bend point
				UTIL_TraceLine(rayPoint, *point + Vector(0, 0, HalfHumanHeight), ignore_monsters, ENT(pev), &result);

				if (result.flFraction == 1.0f && !result.fStartSolid)
				{
					// target is visible from this bend point on the ray - use this point on the ray as our point

					// keep "bent" point at correct height along line of sight
					if (!GetGroundHeight(&rayPoint, &rayPoint.z))
					{
						rayPoint.z = point->z;
					}

					*bend = rayPoint;
					return true;
				}
			}
		}
	}

	*bend = *point;

	// bending rays didn't help - still can't see the point
	return false;
}

CBasePlayer *CCSBot::FindMostDangerousThreat()
{
	// maximum number of simulataneously attendable threats
#ifdef REGAMEDLL_FIXES
	const int MAX_THREATS = MAX_CLIENTS;
#else
	const int MAX_THREATS = 16;
#endif

	struct CloseInfo
	{
		CBasePlayer *enemy;
		float range;
	};

	CloseInfo threat[MAX_THREATS];
	int threatCount = 0;

#ifdef REGAMEDLL_ADD
	int prevIndex = m_enemyQueueIndex - 1;
	if (prevIndex < 0)
		prevIndex = MAX_ENEMY_QUEUE - 1;

	CBasePlayer *currentThreat = m_enemyQueue[prevIndex].player;
#endif

	m_bomber = nullptr;
	m_closestVisibleFriend = nullptr;
	m_closestVisibleHumanFriend = nullptr;

#ifdef REGAMEDLL_ADD
	m_isEnemySniperVisible = false;
	CBasePlayer* sniperThreat = NULL;
	float sniperThreatRange = 99999999999.9f;
	bool sniperThreatIsFacingMe = false;
#endif

	float closeFriendRange = 99999999999.9f;
	float closeHumanFriendRange = 99999999999.9f;

	int i;

	{
		for (i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);

			if (!UTIL_IsValidPlayer(pPlayer))
				continue;

			// is it a player?
			if (!pPlayer->IsPlayer())
				continue;

			// ignore self
			if (pPlayer->entindex() == entindex())
				continue;

			// is it alive?
			if (!pPlayer->IsAlive())
				continue;


#ifdef REGAMEDLL_ADD // REGAMEDLL_FIXES ?
			if ((pPlayer->pev->flags & FL_NOTARGET) || (pPlayer->pev->effects & EF_NODRAW))
				continue;
#endif
			// is it an enemy?
			if (BotRelationship(pPlayer) == BOT_TEAMMATE)
			{
				TraceResult result;
				UTIL_TraceLine(GetEyePosition(), pPlayer->pev->origin, ignore_monsters, ignore_glass, edict(), &result);
				if (result.flFraction == 1.0f)
				{
					// update watch timestamp
					int idx = pPlayer->entindex() - 1;
					m_watchInfo[idx].timestamp = gpGlobals->time;
					m_watchInfo[idx].isEnemy = false;

					// keep track of our closest friend
					Vector to = pev->origin - pPlayer->pev->origin;
					float rangeSq = to.LengthSquared();
					if (rangeSq < closeFriendRange)
					{
						m_closestVisibleFriend = pPlayer;
						closeFriendRange = rangeSq;
					}

					// keep track of our closest human friend
					if (!pPlayer->IsBot() && rangeSq < closeHumanFriendRange)
					{
						m_closestVisibleHumanFriend = pPlayer;
						closeHumanFriendRange = rangeSq;
					}
				}

				continue;
			}

			// check if this enemy is fully
			unsigned char visParts;
			if (!IsVisible(pPlayer, CHECK_FOV, &visParts))
				continue;

#ifdef REGAMEDLL_ADD
			// do we notice this enemy? (always notice current enemy)
			// (Loner-Strike: and once noticed, still noticed for a moment: the roll repeated every
			// think, failing now and then, kept dropping someone in plain view out of the reaction
			// and stretched a 0.3 s reaction into seconds)
			int lsIdx = pPlayer->entindex() - 1;
			bool lsNoticed = LsFight() && lsIdx >= 0 && lsIdx < MAX_CLIENTS && m_watchInfo[lsIdx].isEnemy && m_watchInfo[lsIdx].timestamp > 0.0f
				&& gpGlobals->time - m_watchInfo[lsIdx].timestamp < 1.5f;
			if (pPlayer != currentThreat && !lsNoticed)
			{
				if (!IsNoticable(pPlayer, visParts))
				{
					continue;
				}
			}
#endif

			// update watch timestamp
			int idx = pPlayer->entindex() - 1;
			// (Loner-Strike trace: first in view after a few seconds without them)
			// (and the eyes go to what caught them: someone noticed at the edge of the view, with
			// the eyes on a noise elsewhere, kept slipping out of view and the reaction started
			// over - a person looks straight at the movement)
			if (LsFight() && (m_watchInfo[idx].timestamp <= 0.0f || gpGlobals->time - m_watchInfo[idx].timestamp > 3.0f))
			{
				LsTraceSeen(pPlayer);
				if (!IsAttacking() && pPlayer != GetEnemy())
				{
					Vector at = pPlayer->pev->origin + Vector(0, 0, HalfHumanHeight);
					SetLookAt("Loner-Strike: caught the eye", &at, PRIORITY_HIGH, 0.6f);
				}
			}
			m_watchInfo[idx].timestamp = gpGlobals->time;
			m_watchInfo[idx].isEnemy = true;

			// note if we see the bomber
			if (pPlayer->IsBombGuy())
			{
				m_bomber = pPlayer;
			}

			// keep track of all visible threats
			Vector d = pev->origin - pPlayer->pev->origin;
			float distSq = d.LengthSquared();

#ifdef REGAMEDLL_ADD
			CBasePlayerWeapon *pCurrentWeapon = static_cast<CBasePlayerWeapon *>(pPlayer->m_pActiveItem);
			if (pCurrentWeapon && isSniperRifle(pCurrentWeapon))
			{
				m_isEnemySniperVisible = true;
				if (sniperThreat)
				{
					if (IsPlayerLookingAtMe(pPlayer))
					{
						if (sniperThreatIsFacingMe)
						{
							// several snipers are facing us - keep closest
							if (distSq < sniperThreatRange)
							{
								sniperThreat = pPlayer;
								sniperThreatRange = distSq;
								sniperThreatIsFacingMe = true;
							}
						}
						else
						{
							// even if this sniper is farther away, keep it because he's aiming at us
							sniperThreat = pPlayer;
							sniperThreatRange = distSq;
							sniperThreatIsFacingMe = true;
						}
					}
					else
					{
						// this sniper is not looking at us, only consider it if we dont have a sniper facing us
						if (!sniperThreatIsFacingMe && distSq < sniperThreatRange)
						{
							sniperThreat = pPlayer;
							sniperThreatRange = distSq;
						}
					}
				}
				else
				{
					// first sniper we see
					sniperThreat = pPlayer;
					sniperThreatRange = distSq;
					sniperThreatIsFacingMe = IsPlayerLookingAtMe(pPlayer);
				}
			}
#endif

			// maintain set of visible threats, sorted by increasing distance
			if (threatCount == 0)
			{
				threat[0].enemy = pPlayer;
				threat[0].range = distSq;
				threatCount = 1;
			}
			else
			{
				// find insertion point
				int j;
				for (j = 0; j < threatCount; j++)
				{
					if (distSq < threat[j].range)
						break;
				}

				// shift lower half down a notch
				for (int k = threatCount - 1; k >= j; k--)
					threat[k + 1] = threat[k];

				// insert threat into sorted list
				threat[j].enemy = pPlayer;
				threat[j].range = distSq;

				if (threatCount < MAX_THREATS)
					threatCount++;
			}
		}
	}
	{
		// track the maximum enemy and friend counts we've seen recently
		int prevEnemies = m_nearbyEnemyCount;
		int prevFriends = m_nearbyFriendCount;
		m_nearbyEnemyCount = 0;
		m_nearbyFriendCount = 0;

		for (i = 0; i < MAX_CLIENTS; i++)
		{
			if (m_watchInfo[i].timestamp <= 0.0f)
				continue;

			const float recentTime = 3.0f;
			if (gpGlobals->time - m_watchInfo[i].timestamp < recentTime)
			{
				if (m_watchInfo[i].isEnemy)
					m_nearbyEnemyCount++;
				else
					m_nearbyFriendCount++;
			}
		}

		// note when we saw this batch of enemies
		if (prevEnemies == 0 && m_nearbyEnemyCount > 0)
		{
			m_firstSawEnemyTimestamp = gpGlobals->time;
		}

		if (prevEnemies != m_nearbyEnemyCount || prevFriends != m_nearbyFriendCount)
		{
			PrintIfWatched("Nearby friends = %d, enemies = %d\n", m_nearbyFriendCount, m_nearbyEnemyCount);
		}
	}
	{
		// Track the place where we saw most of our enemies
		struct PlaceRank
		{
			unsigned int place;
			int count;
		};
		static PlaceRank placeRank[MAX_PLACES_PER_MAP];
		int locCount = 0;

		PlaceRank common;
		common.place = 0;
		common.count = 0;

		for (i = 0; i < threatCount; i++)
		{
			// find the area the player/bot is standing on
			CNavArea *area;
			CCSBot *pBot = static_cast<CCSBot *>(threat[i].enemy);

			if (pBot->IsBot())
			{
				area = pBot->GetLastKnownArea();
			}
			else
			{
				area = TheNavAreaGrid.GetNearestNavArea(&threat[i].enemy->pev->origin);
			}

			if (!area)
				continue;

			unsigned int threatLoc = area->GetPlace();
			if (!threatLoc)
				continue;

			// if place is already in set, increment count
			int j;
			for (j = 0; j < locCount; j++)
			{
				if (placeRank[j].place == threatLoc)
					break;
			}

			if (j == locCount)
			{
				// new place
				if (locCount < MAX_PLACES_PER_MAP)
				{
					placeRank[locCount].place = threatLoc;
					placeRank[locCount].count = 1;

					if (common.count == 0)
						common = placeRank[locCount];

					locCount++;
				}
			}
			else
			{
				// others are in that place, increment
				placeRank[j].count++;

				// keep track of the most common place
				if (placeRank[j].count > common.count)
					common = placeRank[j];
			}
		}

		// remember most common place
		m_enemyPlace = common.place;
	}

	{
		if (threatCount == 0)
			return nullptr;

		int t;

		// Loner-Strike: the biggest threat, as a person weighs it: close, facing me, shooting me;
		// the one already being fought keeps a little edge (no flicking between targets). Two
		// coming at it from the side outrank one busy with someone else
		if (LsFight())
		{
			CBasePlayer *pBest = nullptr;
			float best = 1e18f;
			int facing = 0;
			float scored[MAX_THREATS + 1];
			for (t = 0; t < threatCount; t++)
			{
				CBasePlayer *pThreat = threat[t].enemy;
				scored[t] = -1.0f;
				if (pThreat->IsProtectedByShield())
					continue;
				float score = threat[t].range;
				bool looking = IsPlayerLookingAtMe(pThreat);
				if (looking)
				{
					score *= 0.3f;
					facing++;
				}
				if (GetAttacker() == pThreat && GetTimeSinceAttacked() < 1.5f)
					score *= 0.4f;
				// (one called nearly dead: the quick kill - the ones who listen to callouts)
				float lsHp = 100.0f;
				if (LsSkill() > 0.35f && LsKnownHurt(pThreat, m_iTeam, &lsHp) && lsHp < 40.0f)
					score *= 0.75f;
				// (darkness, a little: someone still in the dark, far off and not shooting is
				// noticed a moment later - a small part of it, never up close)
				if (threat[t].range > 600.0f * 600.0f && pThreat->pev->light_level < 20 && pThreat->pev->velocity.Length2D() < 80.0f
					&& !(GetAttacker() == pThreat && GetTimeSinceAttacked() < 3.0f) && pThreat != static_cast<CBasePlayer *>(m_enemy)
					&& RANDOM_FLOAT(0.0f, 1.0f) < 0.4f - 0.2f * LsSkill())
					continue;
				if (pThreat == static_cast<CBasePlayer *>(m_enemy))
					score *= 0.6f;
				// an AWPer is the one to deal with first, more so one looking this way (the
				// better players know; a newer one just shoots whoever's nearest)
				CBasePlayerWeapon *pGun = static_cast<CBasePlayerWeapon *>(pThreat->m_pActiveItem);
				if (pGun && pGun->m_iId == WEAPON_AWP && LsSkill() > 0.35f)
					score *= looking ? 0.45f : 0.75f;
				scored[t] = score;
				if (score < best)
				{
					best = score;
					pBest = pThreat;
				}
			}
			// (several facing it at once: it feels it; LsOverwhelmed reads this)
			if (facing >= 2)
				m_lsOverwhelmedAt = gpGlobals->time;
			// (no flicking between two close threats: it stays on the one it's fighting for a
			// moment, and switches only for one clearly worse, or one that's shooting it)
			CBasePlayer *pCur = m_enemy;
			if (pBest && pCur && pBest != pCur && pCur->IsAlive())
			{
				float curScore = -1.0f;
				for (t = 0; t < threatCount; t++)
				{
					if (threat[t].enemy == pCur)
					{
						curScore = scored[t];
						break;
					}
				}
				bool shotBy = GetAttacker() == pBest && GetTimeSinceAttacked() < 0.5f;
				if (curScore >= 0.0f && !shotBy && (gpGlobals->time - m_lsThreatSince < 0.4f + 0.4f * (1.0f - LsSkill()) || best > 0.5f * curScore))
					return pCur;
				// (a newer player tunnels on the one it saw first while it can still see them:
				// the flanker gets it from the side)
				if (curScore >= 0.0f && !shotBy && LsSkill() < 0.35f)
					return pCur;
			}
			if (pBest && pBest != static_cast<CBasePlayer *>(m_enemy))
				m_lsThreatSince = gpGlobals->time;
			if (pBest)
				return pBest;
		}

#ifdef REGAMEDLL_ADD
		bool sawCloserThreat = false;
		bool sawCurrentThreat = false;
		for (t = 0; t < threatCount; t++)
		{
			if (threat[t].enemy == currentThreat)
			{
				sawCurrentThreat = true;
			}
			else if (threat[t].enemy != currentThreat && IsSignificantlyCloser(threat[t].enemy, currentThreat))
			{
				sawCloserThreat = true;
			}
		}

		if (sawCurrentThreat && !sawCloserThreat)
		{
			return currentThreat;
		}

		// if we are a sniper and we see a sniper threat, attack it unless
		// there are other close enemies facing me
		if (IsSniper() && sniperThreat)
		{
			const float closeCombatRange = 500.0f;

			for (t = 0; t < threatCount; ++t)
			{
				if (threat[t].range < closeCombatRange && IsPlayerLookingAtMe(threat[t].enemy))
				{
					return threat[t].enemy;
				}
			}

			return sniperThreat;
		}
#endif

		// otherwise, find the closest threat that without using shield
		for (t = 0; t < threatCount; t++)
		{
			if (!threat[t].enemy->IsProtectedByShield())
			{
				return threat[t].enemy;
			}
		}
	}

	// return closest threat
	return threat[0].enemy;
}

// Update our reaction time queue
void CCSBot::UpdateReactionQueue()
{
	// zombies dont see any threats
	if (cv_bot_zombie.value > 0.0f)
		return;

	// find biggest threat at this instant
	CBasePlayer *threat = FindMostDangerousThreat();

	int now = m_enemyQueueIndex;

#ifdef REGAMEDLL_ADD
	// reset timer
	m_attentionInterval.Start();
#endif

	// store a snapshot of its state at the end of the reaction time queue
	if (threat)
	{
		m_enemyQueue[now].player = threat;
		m_enemyQueue[now].isReloading = threat->IsReloading();
		m_enemyQueue[now].isProtectedByShield = threat->IsProtectedByShield();
	}
	else
	{
		m_enemyQueue[now].player = nullptr;
		m_enemyQueue[now].isReloading = false;
		m_enemyQueue[now].isProtectedByShield = false;
	}

	// queue is round-robin
	if (++m_enemyQueueIndex >= MAX_ENEMY_QUEUE)
		m_enemyQueueIndex = 0;

	if (m_enemyQueueCount < MAX_ENEMY_QUEUE)
		m_enemyQueueCount++;

	// clamp reaction time to enemy queue size
	float reactionTime = GetProfile()->GetReactionTime();
	float maxReactionTime = (MAX_ENEMY_QUEUE * g_flBotFullThinkInterval) - 0.01f;
	if (reactionTime > maxReactionTime)
		reactionTime = maxReactionTime;

	// "rewind" time back to our reaction time
	int reactionTimeSteps = int((reactionTime / g_flBotFullThinkInterval) + 0.5f);

	int i = now - reactionTimeSteps;
	if (i < 0)
		i += MAX_ENEMY_QUEUE;

	m_enemyQueueAttendIndex = byte(i);
}

// Return the most dangerous threat we are "conscious" of
CBasePlayer *CCSBot::GetRecognizedEnemy()
{
	if (m_enemyQueueAttendIndex >= m_enemyQueueCount)
		return nullptr;

	return m_enemyQueue[m_enemyQueueAttendIndex].player;
}

// Return true if the enemy we are "conscious" of is reloading
bool CCSBot::IsRecognizedEnemyReloading()
{
	if (m_enemyQueueAttendIndex >= m_enemyQueueCount)
		return false;

	return m_enemyQueue[m_enemyQueueAttendIndex].isReloading;
}

// Return true if the enemy we are "conscious" of is hiding behind a shield
bool CCSBot::IsRecognizedEnemyProtectedByShield()
{
	if (m_enemyQueueAttendIndex >= m_enemyQueueCount)
		return false;

	return m_enemyQueue[m_enemyQueueAttendIndex].isProtectedByShield;
}

// Return distance to closest enemy we are "conscious" of
float CCSBot::GetRangeToNearestRecognizedEnemy()
{
	const CBasePlayer *pEnemy = GetRecognizedEnemy();

	if (pEnemy)
	{
		return (pev->origin - pEnemy->pev->origin).Length();
	}

	return 99999999.9f;
}

// Blind the bot for the given duration
void CCSBot::Blind(float duration, float holdTime, float fadeTime, int alpha)
{
	// extend
	CBasePlayer::Blind(duration, holdTime, fadeTime, alpha);

	PrintIfWatched("I'm blind!\n");

	if (RANDOM_FLOAT(0.0f, 100.0f) < 33.3f)
	{
		GetChatter()->Say("Blinded", 1.0f);
	}

	// decide which way to move while blind
	m_blindMoveDir = static_cast<NavRelativeDirType>(RANDOM_LONG(1, NUM_RELATIVE_DIRECTIONS - 1));

	// if blinded while in combat - then spray and pray!
	m_blindFire = (RANDOM_FLOAT(0.0f, 100.0f) < 10.0f) != 0;

	// no longer safe
	AdjustSafeTime();
}

#ifdef REGAMEDLL_ADD
bool CCSBot::IsNoticable(const CBasePlayer *pPlayer, unsigned char visibleParts) const
{
	float deltaT = m_attentionInterval.GetElapsedTime();

	// Loner-Strike: attention, as a person's: nothing new is taken in mid-flick (the eyes skip
	// over the world during a fast turn), and in a fight the edges of the view narrow: someone
	// well off to the side of the one being fought is often missed (unless they shoot: LsFlankCheck)
	if (LsFight())
	{
		if (fabs(m_lookYawVel) > 350.0f)
			return false;
		if (IsAttacking())
		{
			Vector to = pPlayer->pev->origin - pev->origin;
			float off = float(fabs(NormalizeAngle(UTIL_VecToYaw(to) - pev->v_angle.y)));
			if (off > 40.0f && RANDOM_FLOAT(0.0f, 1.0f) > 0.3f + 0.5f * LsSkill())
				return false;
		}
	}

	// all chances are specified in terms of a standard "quantum" of time
	// in which a normal person would notice something
	const float noticeQuantum = 0.25f;

	// determine percentage of player that is visible
	float coverRatio = 0.0f;

	if (visibleParts & CHEST)
	{
		const float chance = 40.0f;
		coverRatio += chance;
	}

	if (visibleParts & HEAD)
	{
		const float chance = 10.0f;
		coverRatio += chance;
	}

	if (visibleParts & LEFT_SIDE)
	{
		const float chance = 20.0f;
		coverRatio += chance;
	}

	if (visibleParts & RIGHT_SIDE)
	{
		const float chance = 20.0f;
		coverRatio += chance;
	}

	if (visibleParts & FEET)
	{
		const float chance = 10.0f;
		coverRatio += chance;
	}

	// compute range modifier - farther away players are harder to notice, depeding on what they are doing
	float range = (pPlayer->pev->origin - pev->origin).Length();
	const float closeRange = 300.0f;
	const float farRange = 1000.0f;

	float rangeModifier;
	if (range < closeRange)
	{
		rangeModifier = 0.0f;
	}
	else if (range > farRange)
	{
		rangeModifier = 1.0f;
	}
	else
	{
		rangeModifier = (range - closeRange) / (farRange - closeRange);
	}

	// harder to notice when crouched
	bool isCrouching = (pPlayer->pev->flags & FL_DUCKING) == FL_DUCKING;
	// moving players are easier to spot
	float playerSpeedSq = pPlayer->pev->velocity.LengthSquared();
	const float runSpeed = 200.0f;
	const float walkSpeed = 30.0f;
	float farChance, closeChance;
	if (playerSpeedSq > runSpeed * runSpeed)
	{
		// running players are always easy to spot (must be standing to run)
		return true;
	}
	else if (playerSpeedSq > walkSpeed * walkSpeed)
	{
		// walking players are less noticable far away
		if (isCrouching)
		{
			closeChance = 90.0f;
			farChance = 60.0f;
		}
		// standing
		else
		{
			closeChance = 100.0f;
			farChance = 75.0f;
		}
	}
	else
	{
		// motionless players are hard to notice
		if (isCrouching)
		{
			// crouching and motionless - very tough to notice
			closeChance = 80.0f;
			farChance = LsFight() ? 25.0f : 5.0f;		// takes about three seconds to notice (50% chance)
		}
		// standing
		else
		{
			// (Loner-Strike: someone standing in plain view far off is still seen - stock's 10%
			// a quarter second left a person holding an angle unseen for seconds at 800 units)
			closeChance = 100.0f;
			farChance = LsFight() ? 50.0f : 10.0f;
		}
	}

	// combine posture, speed, and range chances
	float dispositionChance = closeChance + (farChance - closeChance) * rangeModifier;

	// determine actual chance of noticing player
	float noticeChance = dispositionChance * coverRatio/100.0f;

	// scale by skill level
	noticeChance *= (0.5f + 0.5f * GetProfile()->GetSkill());

	// Loner-Strike: where in the view: someone near the middle of it is seen at once, one at the
	// edge of the view less readily (attention, as a person's)
	if (LsFight())
	{
		Vector to = pPlayer->pev->origin - pev->origin;
		float off = float(fabs(NormalizeAngle(UTIL_VecToYaw(to) - pev->v_angle.y)));
		noticeChance *= off < 15.0f ? 2.5f : off < 35.0f ? 1.4f : 0.7f;
	}

	// if we are alert, our chance of noticing is much higher
	//if (IsAlert())
	//{
	//	const float alertBonus = 50.0f;
	//	noticeChance += alertBonus;
	//}

	// scale by time quantum
	noticeChance *= deltaT / noticeQuantum;

	// there must always be a chance of detecting the enemy
	const float minChance = 0.1f;
	if (noticeChance < minChance)
	{
		noticeChance = minChance;
	}

	//PrintIfWatched("Notice chance = %3.2f\n", noticeChance);
	return (RANDOM_FLOAT(0.0f, 100.0f) < noticeChance);
}
#endif

// Loner-Strike (bot_ls_fight): a person who finds their crosshair on a wall a step away, with
// nothing going on, looks somewhere that makes sense. Only out of a fight (an enemy up against
// that wall is still watched), not while something else has it looking there on purpose (a
// noise, a plugin, planting or defusing, a ladder). A good player notices at once; a newer one
// takes a moment, and now and then doesn't notice at all. Where to: down the way it's going if
// that's open, else the longest open line near where it was looking.
void CCSBot::LsWallSanity()
{
	if (!LsFight() || !IsAlive())
		return;
	float now = gpGlobals->time;
	if (now < m_lsWallNext)
		return;
	m_lsWallNext = now + 0.15f;
	if (CSGameRules()->IsFreezePeriod() || IsAttacking() || IsEnemyVisible() || GetTimeSinceLastSawEnemy() < 2.5f || IsBlind()
		|| IsUsingLadder() || IsLookingAtSpot(PRIORITY_MEDIUM) || LsPuppetLooking() || LsPuppetActive() || LsOrderActive()
		|| m_state == &m_plantBombState || m_state == &m_defuseBombState)
	{
		m_lsWallSince = 0.0f;
		return;
	}

	Vector eye = GetEyePosition();
	UTIL_MakeVectors(pev->v_angle);
	Vector fwd = gpGlobals->v_forward;
	TraceResult tr;
	UTIL_TraceLine(eye, eye + fwd * 300.0f, ignore_monsters, ignore_glass, ENT(pev), &tr);
	CBaseEntity *pHit = tr.pHit ? CBaseEntity::Instance(tr.pHit) : nullptr;
	bool seeThrough = pHit && (FClassnameIs(pHit->pev, "func_door") || FClassnameIs(pHit->pev, "func_door_rotating") || FClassnameIs(pHit->pev, "func_breakable"));
	// (a surface a few steps away, more or less square on: not a doorway's edge it's checking)
	bool wall = tr.flFraction < 0.8f && !tr.fStartSolid && !seeThrough && DotProduct(tr.vecPlaneNormal, -fwd) > 0.55f;
	if (!wall)
	{
		m_lsWallSince = 0.0f;
		return;
	}
	if (m_lsWallSince <= 0.0f)
	{
		m_lsWallSince = now;
		return;
	}
	float skill = LsSkill();
	if (now - m_lsWallSince < 0.25f + 0.8f * (1.0f - skill))
		return;
	m_lsWallSince = 0.0f;
	// (a newer player doesn't always notice)
	if (RANDOM_FLOAT(0.0f, 1.0f) < 0.4f * (1.0f - skill))
	{
		m_lsWallNext = now + RANDOM_FLOAT(1.0f, 2.5f);
		return;
	}

	float moving = pev->velocity.Length2D();
	float goYaw = moving > 60.0f ? UTIL_VecToYaw(pev->velocity) : pev->v_angle.y;
	// (walking at a wall its path turns away from just ahead: that's a corner it's about to
	// round - the path's own look ahead has it, not a turn off somewhere else)
	if (moving > 60.0f && HasPath() && m_pathIndex < m_pathLength - 1 && (m_goalPosition - pev->origin).IsLengthLessThan(350.0f))
		return;
	float best = -1.0f;
	Vector bestAt = eye;
	for (int k = 0; k < 12; k++)
	{
		float yaw = pev->v_angle.y + 30.0f * float(k <= 6 ? k : k - 12);
		// (on the move: forward of it only - never a look back over the shoulder from here)
		if (moving > 60.0f && Q_abs(NormalizeAngle(yaw - goYaw)) > 80.0f)
			continue;
		Vector dir(BotCOS(yaw), BotSIN(yaw), 0.0f);
		float open = 1500.0f * LsClearTo(eye, eye + dir * 1500.0f, ENT(pev));
		if (open < 300.0f)
			continue;
		float score = open - 2.0f * float(Q_abs(NormalizeAngle(yaw - pev->v_angle.y)));
		if (moving > 60.0f)
			score += 600.0f - 8.0f * float(Q_abs(NormalizeAngle(yaw - goYaw)));
		if (score > best)
		{
			best = score;
			bestAt = eye + dir * Q_min(open, 600.0f);
		}
	}
	if (best < 0.0f)
		return;
	SetLookAt("Loner-Strike: off the wall", &bestAt, PRIORITY_MEDIUM, RANDOM_FLOAT(0.8f, 1.6f), true);
	m_lsWallNext = now + 1.0f;
	LsWhy("off the wall");
}

// Loner-Strike (bot_ls_fight): a spot to look at that's behind a wall from here (an enemy gone
// behind cover, a noise through a wall) becomes the edge it would be seen past: a person
// watches the corner someone went round, not the wall in front of them. Left as it is when it's
// in view, or there's no way round
void CCSBot::LsLookableSpot(Vector *spot)
{
	if (!LsFight() || !spot)
		return;
	Vector eye = GetEyePosition();
	if (LsClearTo(eye, *spot, ENT(pev)) >= 0.95f)
		return;
	// (it bends toward a point on the floor, and looks at it from chest height)
	Vector floor(spot->x, spot->y, spot->z - HalfHumanHeight), bend;
	if (!BendLineOfSight(&eye, &floor, &bend))
		return;
	// (the bend point is on the floor: at the spot's height, and only if it's not right here)
	if ((bend - eye).Length2D() < 64.0f)
		return;
	bend.z = spot->z;
	*spot = bend;
}
