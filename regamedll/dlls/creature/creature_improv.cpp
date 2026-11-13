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

// Loner-Strike creatures: locomotion. The hostage improv's movement (hostage_improv.cpp) for a
// creature: same path following, ledge jumps, ladders, unsticking, doors and breakables, with
// the creature's own speeds instead of the hostage's walk/run and without anything about
// hostages (chatter, rescue, fear). Kept as a sibling so CHostageImprov is untouched.

#include "precompiled.h"

CCreatureImprov::CCreatureImprov(CLsCreature *pCreature)
{
	m_creature = pCreature;
	m_walkSpeed = 100.0f;
	m_runSpeed = 250.0f;
	OnReset();
}

void CCreatureImprov::OnReset()
{
	m_lastKnownArea = nullptr;
	m_vel = Vector2D(0, 0);
	m_actualVel = Vector(0, 0, 0);
	m_lastPosition = m_creature->pev->origin;
	m_moveGoal = m_creature->pev->origin;
	m_hasGoal = false;
	m_moveType = Stopped;
	m_moveAngle = m_creature->pev->angles.y;
	m_isCrouching = false;
	m_isLookingAt = false;
	m_isFacingTo = false;
	m_path.Invalidate();
	m_repathTimer.Invalidate();
	m_repathFails = 0;
	m_failedGoal = Vector(0, 0, 0);
	m_directCheckTimer.Invalidate();
	m_directOk = false;
	m_directStuckTimer.Invalidate();
	m_jumpTimer.Invalidate();
	m_hasJumped = false;
	m_hasJumpedIntoAir = false;
	m_traversingLadder = false;
	m_wiggleDirection = FORWARD;
}

bool CCreatureImprov::IsAlive() const
{
	return m_creature->pev->deadflag == DEAD_NO;
}

void CCreatureImprov::MoveTo(const Vector &goal)
{
	// (a goal that only crept a little - a target shuffling about - keeps the path it has)
	if (m_hasGoal && m_path.IsValid() && (goal - m_moveGoal).IsLengthLessThan(48.0f))
	{
		m_moveGoal = goal;
		return;
	}

	m_moveGoal = goal;
	m_hasGoal = true;
	m_path.Invalidate();
	m_directStuckTimer.Invalidate();
	m_directCheckTimer.Invalidate();

	// (an unreachable target asked for again and again keeps its back-off; a new place doesn't)
	if (!m_repathFails || (goal - m_failedGoal).IsLengthGreaterThan(256.0f))
	{
		m_repathFails = 0;
		m_repathTimer.Invalidate();
	}
}

// Path searches across all creatures: at most two a frame, the rest wait their turn (a tick or two)
static bool LsPathBudget()
{
	static float s_frame = -1.0f;
	static int s_used = 0;

	if (gpGlobals->time != s_frame)
	{
		s_frame = gpGlobals->time;
		s_used = 0;
	}

	if (s_used >= 2)
		return false;

	s_used++;
	return true;
}

void CCreatureImprov::Stop()
{
	m_hasGoal = false;
	m_moveGoal = GetFeet();
	m_path.Invalidate();
	m_moveType = Stopped;
	m_directStuckTimer.Invalidate();
}

void CCreatureImprov::OnMoveToSuccess(const Vector &goal)
{
	m_hasGoal = false;
	m_creature->OnMoveToSuccess(goal);
}

void CCreatureImprov::OnMoveToFailure(const Vector &goal, MoveToFailureType reason)
{
	m_hasGoal = false;
	m_path.Invalidate();
	m_creature->OnMoveToFailure(goal, reason);
}

bool CCreatureImprov::IsAtMoveGoal(float error) const
{
	return (GetFeet() - m_moveGoal).IsLengthLessThan(error);
}

const Vector &CCreatureImprov::GetFeet() const
{
	return m_creature->pev->origin;
}

const Vector &CCreatureImprov::GetCentroid() const
{
	m_centroid = m_creature->pev->origin + Vector(0, 0, (m_creature->pev->maxs.z + m_creature->pev->mins.z) * 0.5f);
	return m_centroid;
}

const Vector &CCreatureImprov::GetEyes() const
{
	m_eye = m_creature->pev->origin + m_creature->pev->view_ofs;
	return m_eye;
}

bool CCreatureImprov::IsOnGround() const
{
	return (m_creature->pev->flags & FL_ONGROUND) != 0;
}

bool CCreatureImprov::IsVisible(const Vector &pos, bool testFOV) const
{
	TraceResult result;
	UTIL_TraceLine(GetEyes(), pos, ignore_monsters, ignore_glass, m_creature->edict(), &result);
	return result.flFraction == 1.0f;
}

// Find "simple" ground height, treating current nav area as part of the floor
bool CCreatureImprov::GetSimpleGroundHeightWithFloor(const Vector *pos, float *height, Vector *normal)
{
	if (GetSimpleGroundHeight(pos, height, normal))
	{
		if (m_lastKnownArea && m_lastKnownArea->IsOverlapping(pos))
			*height = Q_max((*height), m_lastKnownArea->GetZ(pos));

		return true;
	}

	return false;
}

bool CCreatureImprov::DiscontinuityJump(float ground, bool onlyJumpDown, bool mustJump)
{
	if (IsJumping() || IsCrouching() || IsUsingLadder())
		return false;

	float dz = ground - GetFeet().z;

	if ((dz > StepHeight && !onlyJumpDown) || dz < -JumpHeight)
	{
		Jump();
		return true;
	}

	return false;
}

bool CCreatureImprov::Jump()
{
	if (IsCrouching() || !m_jumpTimer.IsElapsed())
		return false;

	m_hasJumped = false;

	if (m_creature->pev->velocity.LengthSquared() < 1.0f)
		m_creature->pev->origin.z += 2.0f;

	const float minJumpInterval = 3.0f;
	m_jumpTimer.Start(minJumpInterval);
	return true;
}

void CCreatureImprov::ResetJump()
{
	if (m_hasJumpedIntoAir)
	{
		if (IsOnGround())
			m_jumpTimer.Invalidate();
	}
	else if (!IsOnGround())
	{
		m_hasJumpedIntoAir = true;
	}
}

void CCreatureImprov::Crouch()
{
	if (IsCrouching())
		return;

	m_isCrouching = true;
	m_minCrouchTimer.Start(1.0f);

	Vector maxs = m_creature->pev->maxs;
	maxs.z = Q_min(maxs.z, 30.0f);
	UTIL_SetSize(m_creature->pev, m_creature->pev->mins, maxs);
}

void CCreatureImprov::StandUp()
{
	if (!IsCrouching() || !m_minCrouchTimer.IsElapsed())
		return;

	const LsCreatureType *type = m_creature->GetType();
	float height = type ? type->hullMax.z : 62.0f;

	// (room over the head first: the hostage's lines up, fewer of them)
	for (int y = -12; y <= 12; y += 12)
	{
		for (int x = -12; x <= 12; x += 12)
		{
			TraceResult result;
			UTIL_TraceLine(GetFeet() + Vector(0, 0, 3), GetFeet() + Vector(x, y, height + 8.0f), ignore_monsters, ignore_glass, m_creature->edict(), &result);
			if (result.flFraction < 1.0f)
				return;
		}
	}

	UTIL_SetSize(m_creature->pev, type ? type->hullMin : Vector(-16, -16, 0), type ? type->hullMax : Vector(16, 16, 62));
	m_isCrouching = false;
}

float CCreatureImprov::MaxSpeed() const
{
	switch (m_moveType)
	{
	case Walking: return IsCrouching() ? Q_min(m_walkSpeed, 80.0f) : m_walkSpeed;
	case Running: return IsCrouching() ? Q_min(m_runSpeed, 110.0f) : m_runSpeed;
	default: return 0.0f;
	}
}

// Turn the body towards a spot (the hostage's turn: proportional, a full turn when it's behind)
bool CCreatureImprov::FaceTowards(const Vector &target, float deltaT)
{
	Vector2D to = (target - GetFeet()).Make2D();
	if (to.IsLengthLessThan(1.0f))
		return true;

	to.NormalizeInPlace();

	Vector2D lat(BotCOS(m_moveAngle), BotSIN(m_moveAngle));
	Vector2D dir(-lat.y, lat.x);

	float dot = DotProduct(to, dir);
	bool behind = false;

	if (DotProduct(to, lat) < 0.0f)
	{
		dot = (dot >= 0.0f) ? 1.0f : -1.0f;
		behind = true;
	}

	const float maxTurnRate = 0.05f;
	if (behind || Q_fabs(dot) >= maxTurnRate)
	{
		const float tolerance = 300.0f;
		m_moveAngle = NormalizeAngle(dot * deltaT * tolerance + m_moveAngle);
		m_creature->pev->angles.y = m_moveAngle;
		return false;
	}

	return true;
}

void CCreatureImprov::TrackPath(const Vector &pathGoal, float deltaT)
{
	FaceTowards(pathGoal, deltaT);
	MoveTowards(pathGoal, deltaT);
	m_jumpTarget = pathGoal;
}

void CCreatureImprov::MoveTowards(const Vector &pos, float deltaT)
{
	// open doors and break boards in the way
	ClearPath();

	// jump up ledges and down drops, looking ahead (the hostage's three looks)
	if ((!m_lastKnownArea || !(m_lastKnownArea->GetAttributes() & NAV_NO_JUMP))
		&& !IsUsingLadder() && !IsJumping() && IsOnGround() && !IsCrouching())
	{
		float ground;
		Vector aheadRay(pos.x - GetFeet().x, pos.y - GetFeet().y, 0);
		aheadRay.NormalizeInPlace();

		bool jumped = false;
		if (IsRunning())
		{
			Vector normal;
			Vector stepAhead = GetFeet() + 80.0f * aheadRay;
			stepAhead.z += HumanHeight;

			if (GetSimpleGroundHeightWithFloor(&stepAhead, &ground, &normal) && normal.z > 0.9f)
				jumped = DiscontinuityJump(ground, true);
		}

		if (!jumped)
		{
			Vector stepAhead = GetFeet() + 30.0f * aheadRay;
			stepAhead.z += HumanHeight;

			if (GetSimpleGroundHeightWithFloor(&stepAhead, &ground))
				jumped = DiscontinuityJump(ground);
		}

		if (!jumped)
		{
			Vector stepAhead = GetFeet() + 10.0f * aheadRay;
			stepAhead.z += HumanHeight;

			if (GetSimpleGroundHeightWithFloor(&stepAhead, &ground))
				DiscontinuityJump(ground, true, true);
		}
	}

	Vector move = (pos - GetFeet());
	move.z = 0;

	if (!move.IsZero())
		move.NormalizeInPlace();

	// (the hostage's friction is 3/s, so accelerating at 3x the top speed settles on it)
	float accelRate = MaxSpeed() * 3.0f + 100.0f;

	m_vel.x += move.x * accelRate * deltaT;
	m_vel.y += move.y * accelRate * deltaT;
}

bool CCreatureImprov::TraverseLadder(const CNavLadder *ladder, NavTraverseType how, const Vector *approachPos, const Vector *departPos, float deltaT)
{
	Vector goal;

	if (how == GO_LADDER_DOWN)
	{
		goal = ladder->m_bottom;
		AddDirectionVector(&goal, ladder->m_dir, 16);

		if (ladder->m_top.z - HalfHumanHeight > GetFeet().z && (GetFeet() - goal).Make2D().IsLengthLessThan(50.0f))
			m_creature->pev->velocity.z = -100.0f;

		if (ladder->m_bottom.z + HalfHumanHeight > GetFeet().z)
			return true;
	}
	else if (m_traversingLadder)
	{
		goal = ladder->m_top;
		AddDirectionVector(&goal, ladder->m_dir, 16);

		if ((GetFeet() - goal).Make2D().IsLengthLessThan(100.0f))
			Walk();

		if ((GetFeet() - goal).Make2D().IsLengthLessThan(40.0f))
			m_creature->pev->velocity.z = 150.0f;

		if (GetFeet().z > ladder->m_top.z)
			m_traversingLadder = false;
	}
	else
	{
		goal = GetFeet();
		if (departPos)
		{
			float closeRange = 1e12f;
			CNavArea *tops[3] = { ladder->m_topForwardArea, ladder->m_topLeftArea, ladder->m_topRightArea };
			for (int i = 0; i < 3; i++)
			{
				if (!tops[i])
					continue;

				float range = (*departPos - *tops[i]->GetCenter()).LengthSquared();
				if (range < closeRange)
				{
					closeRange = range;
					goal = *tops[i]->GetCenter();
				}
			}
		}

		if ((GetFeet() - goal).Make2D().IsLengthLessThan(20.0f))
			return true;
	}

	TrackPath(goal, deltaT);
	return false;
}

// Stuck: shove about at random, jump now and then (the hostage's wiggle)
void CCreatureImprov::Wiggle()
{
	if (m_wiggleTimer.IsElapsed())
	{
		m_wiggleDirection = static_cast<NavRelativeDirType>(RANDOM_LONG(FORWARD, LEFT));
		m_wiggleTimer.Start(RANDOM_FLOAT(0.3f, 0.5f));
	}

	const float force = 15.0f;
	Vector dir(BotCOS(m_moveAngle), BotSIN(m_moveAngle), 0.0f);
	Vector lat(-dir.y, dir.x, 0.0f);

	switch (m_wiggleDirection)
	{
	case FORWARD: ApplyForce(dir * force); break;
	case BACKWARD: ApplyForce(dir * -force); break;
	case LEFT: ApplyForce(lat * force); break;
	case RIGHT: ApplyForce(lat * -force); break;
	default: break;
	}

	if (m_follower.GetStuckDuration() > 0.5f && m_wiggleJumpTimer.IsElapsed() && Jump())
		m_wiggleJumpTimer.Start(RANDOM_FLOAT(0.75f, 1.2f));
}

// A door in front: push it. A breakable: smash it (the hostage does the same)
void CCreatureImprov::ClearPath()
{
	if (!m_clearPathTimer.IsElapsed())
		return;

	m_clearPathTimer.Start(RANDOM_FLOAT(0.3f, 0.5f));

	TraceResult result;
	UTIL_MakeVectors(m_creature->pev->angles);
	UTIL_TraceLine(GetEyes(), GetEyes() + gpGlobals->v_forward * 64, ignore_monsters, dont_ignore_glass, m_creature->edict(), &result);

	if (result.flFraction == 1.0f || !result.pHit)
		return;

	edict_t *pHit = result.pHit;
	CBaseEntity *pObject = CBaseEntity::Instance(pHit);
	if (!pObject)
		return;

	if (FClassnameIs(pHit, "func_door") || FClassnameIs(pHit, "func_door_rotating"))
		pObject->Touch(m_creature);
	else if (FClassnameIs(pHit, "func_breakable") && pHit->v.takedamage == DAMAGE_YES)
		pObject->TakeDamage(m_creature->pev, m_creature->pev, 9999.9f, DMG_CLUB);
}

void CCreatureImprov::OnTouch(CBaseEntity *pOther)
{
	if (FStrEq(STRING(pOther->pev->classname), "worldspawn"))
	{
		// a step it can't climb by itself: lift it up (the hostage's step check, shortened)
		if (!IsMoving() || IsUsingLadder())
			return;

		UTIL_MakeVectors(m_creature->pev->angles);
		Vector ahead = gpGlobals->v_forward;
		ahead.z = 0;
		ahead.NormalizeInPlace();

		for (float range = 1.0f; range <= 30.5f; range += 5.0f)
		{
			Vector stepAhead = GetFeet() + ahead * range;
			stepAhead.z = GetEyes().z;

			float ground;
			Vector normal;
			if (GetSimpleGroundHeightWithFloor(&stepAhead, &ground, &normal))
			{
				float dz = ground - GetFeet().z;
				if (dz > 0.0f && dz < 18.0f)
				{
					m_creature->pev->origin.z = ground + 3.0f;
					m_inhibitObstacleAvoidance.Start(0.5f);
					break;
				}
			}
		}
	}
	else if (pOther->IsPlayer() || FClassnameIs(pOther->pev, "ls_creature"))
	{
		Vector2D to = (m_creature->pev->origin - pOther->pev->origin).Make2D();
		to.NormalizeInPlace();

		const float pushForce = 20.0f;
		m_vel.x += to.x * pushForce;
		m_vel.y += to.y * pushForce;
	}
}

void CCreatureImprov::UpdatePosition(float deltaT)
{
	CNavArea *area = TheNavAreaList.empty() ? nullptr : TheNavAreaGrid.GetNavArea(&m_creature->pev->origin);
	if (area)
		m_lastKnownArea = area;

	if (IsJumping())
	{
		if (!m_hasJumped)
		{
			m_hasJumped = true;
			m_hasJumpedIntoAir = false;
			m_creature->pev->velocity.z += 300.0f;
		}
		else
			ResetJump();

		Vector dir = m_jumpTarget - GetFeet();
		dir.z = 0;
		dir.NormalizeInPlace();

		const float pushSpeed = Q_max(100.0f, Q_min(m_runSpeed, 200.0f));
		m_creature->pev->velocity.x = dir.x * pushSpeed;
		m_creature->pev->velocity.y = dir.y * pushSpeed;

		FaceTowards(m_jumpTarget, deltaT);
		return;
	}

	if (m_isLookingAt && !m_hasGoal && !HasFaceTo())
		FaceTowards(m_viewGoal, deltaT);

	if (HasFaceTo() && FaceTowards(m_faceGoal, deltaT))
		ClearFaceTo();

	if (m_hasGoal && m_moveType != Stopped)
	{
		// close by and in plain sight: straight at it, no path (a target shuffling about)
		Vector toGoal = m_moveGoal - GetFeet();
		bool nearGoal = toGoal.Make2D().IsLengthLessThan(160.0f) && Q_fabs(toGoal.z) < 40.0f;
		if (nearGoal && m_directCheckTimer.IsElapsed())
		{
			m_directCheckTimer.Start(0.2f);
			m_directOk = IsVisible(m_moveGoal + Vector(0, 0, HalfHumanHeight));
		}
		bool direct = TheNavAreaList.empty() || (nearGoal && m_directOk);
		bool waiting = false;

		if (!direct && !m_path.IsValid() && m_repathTimer.IsElapsed())
		{
			if (!LsPathBudget())
			{
				waiting = true;
			}
			else
			{
				HostagePathCost pathCost;
				if (m_path.Compute(&GetFeet(), &m_moveGoal, pathCost))
				{
					m_repathFails = 0;
					m_follower.SetPath(&m_path);
					m_follower.SetImprov(this);
					m_follower.Reset();
					m_follower.Debug(ls_creature_debug.value > 1.0f);
				}
				else
				{
					// (no way there: wait longer each time it's still no way, straight at it meanwhile)
					m_failedGoal = m_moveGoal;
					m_repathTimer.Start(float(1 << Q_min(m_repathFails, 3)));
					m_repathFails++;
				}
			}
		}

		if (waiting)
		{
			// (its turn for a path search comes next frame: stand rather than walk into a wall)
		}
		else if (!direct && m_path.IsValid())
		{
			m_directStuckTimer.Invalidate();
			m_follower.Update(deltaT, m_inhibitObstacleAvoidance.IsElapsed());

			if (m_follower.IsStuck())
				Wiggle();
		}
		else if (m_hasGoal)
		{
			m_path.Invalidate();

			if (IsAtMoveGoal(24.0f))
			{
				OnMoveToSuccess(m_moveGoal);
			}
			else
			{
				TrackPath(m_moveGoal, deltaT);

				// (no headway for a while: stuck)
				if (m_actualVel.IsLengthLessThan(15.0f))
				{
					if (!m_directStuckTimer.HasStarted())
						m_directStuckTimer.Start();
					else if (m_directStuckTimer.GetElapsedTime() > 0.6f)
						Wiggle();

					if (m_directStuckTimer.GetElapsedTime() > 2.5f)
					{
						m_directStuckTimer.Invalidate();
						OnMoveToFailure(m_moveGoal, FAIL_STUCK);
					}
				}
				else
					m_directStuckTimer.Invalidate();
			}
		}
	}
	else
	{
		m_follower.ResetStuck();
	}

	const float friction = 3.0f;
	m_vel.x += m_vel.x * -friction * deltaT;
	m_vel.y += m_vel.y * -friction * deltaT;

	float speed = m_vel.NormalizeInPlace();
	float maxSpeed = Q_max(MaxSpeed(), 30.0f);
	if (speed > maxSpeed)
		speed = maxSpeed;

	m_vel.x *= speed;
	m_vel.y *= speed;

	// keep a little room from players and other creatures (the hostage's personal space)
	Vector velDir(m_vel.x, m_vel.y, 0.0f);
	float velLen = velDir.NormalizeInPlace();
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!pPlayer || !pPlayer->IsAlive())
			continue;

		Vector to = pPlayer->pev->origin - GetCentroid();
		to.z = 0;
		float range = to.NormalizeInPlace();

		const float spring = 40.0f;
		if (range >= spring)
			continue;

		ApplyForce(to * -(spring - range));
		ApplyForce(velDir * -0.1f * velLen);
	}

	for (int i = 0; i < LsCreatureCount(); i++)
	{
		ILsCreature *pOther = LsCreatureAt(i);
		if (!pOther || !pOther->IsAlive() || pOther->Entity() == m_creature)
			continue;

		entvars_t *pevOther = pOther->Vars();
		if (!pevOther)
			continue;

		Vector to = pevOther->origin - GetCentroid();
		to.z = 0;
		float range = to.NormalizeInPlace();

		const float spring = 40.0f;
		if (range >= spring)
			continue;

		ApplyForce(to * -(spring - range));
	}

	// (the pushes come on top of the speed: cap again, and a stopped one - swinging, flinching,
	// standing - only gets nudged, it doesn't drift through a crowd mid-swing)
	float capSpeed = (m_moveType == Stopped) ? 20.0f : Q_max(MaxSpeed(), 30.0f);
	float pushed = m_vel.Length();
	if (pushed > capSpeed)
		m_vel = m_vel * (capSpeed / pushed);

	m_creature->pev->velocity.x = m_vel.x;
	m_creature->pev->velocity.y = m_vel.y;
}

void CCreatureImprov::OnUpkeep(float deltaT)
{
	if (!IsAlive())
		return;

	if (deltaT > 0.0f)
	{
		m_actualVel = (m_creature->pev->origin - m_lastPosition) / deltaT;
		m_lastPosition = m_creature->pev->origin;
	}

	UpdatePosition(deltaT);
}
