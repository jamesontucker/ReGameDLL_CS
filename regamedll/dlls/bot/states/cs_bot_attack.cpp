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

// Begin attacking
void AttackState::OnEnter(CCSBot *me)
{
	me->m_lsDuckUntil = 0.0f;	// Loner-Strike: no duck left over from the last fight
	// (the matchup is weighed once a fight - but not again against the same enemy for a while:
	// a re-peek after taking cover isn't a new decision to take cover again)
	if (gpGlobals->time > me->m_lsMatchupUntil)
		me->m_lsMatchupEnemy = nullptr;
	me->m_lsReloadCrouched = false;
	me->LsFightBegins();	// Loner-Strike
	CBasePlayer *pEnemy = me->GetEnemy();

	// store our posture when the attack began
	me->PushPostureContext();
	me->DestroyPath();

	// if we are using a knife, try to sneak up on the enemy
	if (pEnemy && me->IsUsingKnife() && !me->IsPlayerFacingMe(pEnemy))
		me->Walk();
	else
		me->Run();

	me->GetOffLadder();
	me->ResetStuckMonitor();

	m_repathTimer.Invalidate();
	m_haveSeenEnemy = me->IsEnemyVisible();
	m_lsVisibleFor = 0.0f;
	m_lsVisibleAt = gpGlobals->time;
	m_nextDodgeStateTimestamp = 0.0f;
	m_firstDodge = true;
	m_isEnemyHidden = false;
	m_reacquireTimestamp = 0.0f;

	m_pinnedDownTimestamp = gpGlobals->time + (LsFight() ? me->LsPinnedDownTime() : RANDOM_FLOAT(7.0f, 10.0f));
	me->m_lsPinnedExtended = 0.0f;
	me->m_lsHoldSince = 0.0f;
	m_shieldToggleTimestamp = gpGlobals->time + RANDOM_FLOAT(2.0f, 10.0f);
	m_shieldForceOpen = false;

	// if we encountered someone while escaping, grab our weapon and fight!
	if (me->IsEscapingFromBomb())
		me->EquipBestWeapon();

	if (me->IsUsingKnife())
	{
		// can't crouch and hold with a knife
		m_crouchAndHold = false;
		me->StandUp();
	}
	else
	{
		// decide whether to crouch where we are, or run and gun (if we havent already - see CCSBot::Attack())
		if (!m_crouchAndHold)
		{
			if (pEnemy)
			{
				const float crouchFarRange = 750.0f;
				float crouchChance;

				// more likely to crouch if using sniper rifle or if enemy is far away
				if (me->IsUsingSniperRifle())
					crouchChance = 50.0f;
				else if ((me->pev->origin - pEnemy->pev->origin).IsLengthGreaterThan(crouchFarRange))
					crouchChance = 50.0f;
				else
					crouchChance = 20.0f * (1.0f - me->LsAggro());

				// Loner-Strike: crouching is a habit some have, not a coin flip for everyone
				if (LsFight())
					crouchChance *= me->LsCrouchFactor();

				if (RANDOM_FLOAT(0.0f, 100.0f) < crouchChance)
				{
					// make sure we can still see if we crouch
					TraceResult result;

					Vector origin = me->pev->origin;
					if (!me->IsCrouching())
					{
						// we are standing, adjust for lower crouch origin
						origin.z -= 20.0f;
					}

					UTIL_TraceLine(origin, pEnemy->EyePosition(), ignore_monsters, ignore_glass, ENT(me->pev), &result);

					if (result.flFraction == 1.0f)
					{
						m_crouchAndHold = true;
					}
				}
			}
		}

		if (m_crouchAndHold)
		{
			me->Crouch();
			me->PrintIfWatched("Crouch and hold attack!\n");
		}
	}

	m_scopeTimestamp = 0;
	m_didAmbushCheck = false;

	float skill = me->GetProfile()->GetSkill();

	// tendency to dodge is proportional to skill
	// (Loner-Strike: mostly; nobody stands like a post)
	float dodgeChance = LsFight() ? me->LsDodgeChance() : 80.0f * skill;

	if (me->IsUsingKnife())
	{
		dodgeChance *= 2.0f;
	}

	// high skill bots always dodge if outnumbered, or they see a sniper
	if (skill > 0.5f && me->IsOutnumbered())
	{
		dodgeChance = 100.0f;
	}

	m_dodge = (RANDOM_FLOAT(0.0f, 100.0f) < dodgeChance) != 0;

	// decide whether we might bail out of this fight
	m_isCoward = (RANDOM_FLOAT(0.0f, 100.0f) > 100.0f * me->LsAggro());

	// Loner-Strike: caught by surprise, a newer player may hold walk or crouch
	me->LsAttackBegins(m_crouchAndHold);
}

void AttackState::StopAttacking(CCSBot *me)
{
	if (me->m_task == CCSBot::SNIPING)
	{
		// stay in our hiding spot
		me->Hide(me->GetLastKnownArea(), -1.0f, 50.0f);
	}
	else
	{
		me->StopAttacking();
	}
}

// Perform attack behavior
void AttackState::OnUpdate(CCSBot *me)
{
	// can't be stuck while attacking
	me->ResetStuckMonitor();
	me->StopRapidFire();

	CBasePlayerWeapon *pWeapon = me->GetActiveWeapon();
	if (pWeapon)
	{
		if (pWeapon->m_iId == WEAPON_C4 ||
			pWeapon->m_iId == WEAPON_HEGRENADE ||
			pWeapon->m_iId == WEAPON_FLASHBANG ||
			pWeapon->m_iId == WEAPON_SMOKEGRENADE)
		{
			// (Loner-Strike: not Gun Game's grenade out on its grenade level, beyond arm's
			// length: it's what scores there, and it's thrown from the update)
			CBasePlayer *pFoe = me->GetEnemy();
			// (nor while a throw is on its way out)
			bool keep = LsFight() && pWeapon->m_iId == WEAPON_HEGRENADE && me->LsNadeOnly()
				&& ((pFoe && (pFoe->pev->origin - me->pev->origin).IsLengthGreaterThan(250.0f)) || me->LsTossing());
			if (!keep)
			{
				if (LsFight() && me->LsNadeOnly())
					me->LsHeAway(2.0f);
				me->EquipBestWeapon();
			}
		}
	}

	CBasePlayer *pEnemy = me->GetEnemy();
	if (!pEnemy)
	{
		StopAttacking(me);
		return;
	}

	// keep track of whether we have seen our enemy at least once yet
	if (!m_haveSeenEnemy)
		m_haveSeenEnemy = me->IsEnemyVisible();
	// (Loner-Strike: and for how long, this fight)
	if (me->IsEnemyVisible())
		m_lsVisibleFor += Q_min(0.5f, gpGlobals->time - m_lsVisibleAt);
	m_lsVisibleAt = gpGlobals->time;

	// Loner-Strike: the wrong gun for this range, a flashed enemy to push, low cover to reload behind
	// Loner-Strike: out of bullets, the knife all it has left: not a charge at someone shooting
	// it - away to cover (and a gun off the floor), unless they're right there
	// (a band between this and where it starts a knife fight, 250: no flipping at the edge)
	if (LsFight() && me->LsOutOfAmmo() && (pEnemy->pev->origin - me->pev->origin).IsLengthGreaterThan(380.0f))
	{
		if (me->LsTakeCoverFrom(pEnemy->pev->origin, 0.0f, true))
		{
			me->LsWhy("out of ammo: to cover");
			return;
		}
		StopAttacking(me);
		me->TryToRetreat();
		me->LsWhy("out of ammo: away");
		return;
	}
	// Loner-Strike: reloading with them in view and not right on top of it: out of sight while
	// the gun's useless (decided once a reload) - the good ones nearly always, newer ones often
	// just stand there reloading
	if (LsFight() && !LsSolo() && me->IsActiveWeaponReloading() && me->IsEnemyVisible()
		&& (pEnemy->pev->origin - me->pev->origin).IsLengthGreaterThan(400.0f) && gpGlobals->time - me->m_lsReloadRollAt > 5.0f)
	{
		me->m_lsReloadRollAt = gpGlobals->time;
		if (RANDOM_FLOAT(0.0f, 1.0f) < me->LsReloadCoverChance() && me->LsTakeCoverFrom(pEnemy->pev->origin, 0.0f, true))
		{
			me->LsWhy("reloading: out of sight");
			return;
		}
	}
	if (LsFight() && me->LsMatchup(pEnemy, m_crouchAndHold))
		return;

	// Retreat check
	// Do not retreat if the enemy is too close
	if (m_retreatTimer.IsElapsed())
	{
		// If we've been fighting this battle for awhile, we're "pinned down" and
		// need to do something else.
		// If we are outnumbered, retreat.
		// (Loner-Strike: not while it's doing fine: healthy and not hit lately; for a while)
		if (LsFight() && gpGlobals->time > m_pinnedDownTimestamp && me->LsHealth() > 60.0f && me->GetTimeSinceAttacked() > 2.0f
			&& me->m_lsPinnedExtended < 2.0f * me->LsPinnedDownTime())
		{
			m_pinnedDownTimestamp = gpGlobals->time + 2.0f;
			me->m_lsPinnedExtended += 2.0f;
		}

		bool isPinnedDown = (gpGlobals->time > m_pinnedDownTimestamp);

		// (Loner-Strike: in deathmatch everyone around is an enemy, so being outnumbered is
		// normal: only pinned down and hurt makes someone fall back)
		// (Loner-Strike: on a team map, a lone CT before the plant plays for time by its own
		// roll, in place of stock's "outnumbered by two: always back off")
		if (LsFight() && LsSolo()
			? ((isPinnedDown && me->LsHealth() < 35.0f)
				|| (!CSGameRules()->IsFreeForAll() && m_isCoward && me->LsStyle() != LS_STYLE_RUSHER
					&& (me->OutnumberedCount() >= 3 || (me->OutnumberedCount() >= 2 && me->LsHealth() < 50.0f))))
			: LsFight() ? (isPinnedDown || me->LsPlayForTime() || (me->IsOutnumbered() && m_isCoward) || me->LsOverwhelmed())
			: (isPinnedDown ||
			(me->IsOutnumbered() && m_isCoward) ||
			(me->OutnumberedCount() >= 2 && me->GetProfile()->GetAggression() < 1.0f)))
		{
			// tell our teammates our plight
			if (isPinnedDown)
				me->GetChatter()->PinnedDown();
			else
				me->GetChatter()->Scared();

			m_retreatTimer.Start(RANDOM_FLOAT(3.0f, 15.0f));

			// try to retreat
			if (me->TryToRetreat())
			{
				me->LsWhy("retreat");
				// if we are a sniper, equip our pistol so we can fire while retreating
				if (me->IsUsingSniperRifle())
				{
					me->EquipPistol();
				}
			}
			else
			{
				me->PrintIfWatched("I want to retreat, but no safe spots nearby!\n");
			}
		}
	}

	// Loner-Strike: Gun Game's grenade level with them too far for an arc: closing in, not
	// standing there holding it
	if (LsFight() && me->LsNadeOnly() && me->IsUsingHEGrenade() && (pEnemy->pev->origin - me->pev->origin).IsLengthGreaterThan(1100.0f))
	{
		me->ForceRun(3.0f);
		if (m_repathTimer.IsElapsed())
		{
			const Vector *to = me->IsEnemyVisible() ? &pEnemy->pev->origin : &me->GetLastKnownEnemyPosition();
			me->ComputePath(TheNavAreaGrid.GetNearestNavArea(to), to, FASTEST_ROUTE);
			m_repathTimer.Start(0.5f);
		}
		if (me->UpdatePathMovement(NO_SPEED_CHANGE) != CCSBot::PROGRESSING)
			me->DestroyPath();
		return;
	}

	// Knife fighting
	// We need to pathfind right to the enemy to cut him
	if (me->IsUsingKnife())
	{
		// can't crouch and hold with a knife
		m_crouchAndHold = false;
		me->StandUp();

		// if we are using a knife and our prey is looking towards us, run at him
		// (Loner-Strike: in deathmatch-type modes nobody creeps up with the knife: they run
		// at people, walking only the last bit behind someone)
		// (nor with the knife all it has, from far off: knife arenas)
		float knifeRange = float((pEnemy->pev->origin - me->pev->origin).Length());
		if (me->IsPlayerFacingMe(pEnemy) || (LsFight() && LsSolo() && knifeRange > 250.0f) || (LsFight() && me->LsKnifeOnly() && knifeRange > 400.0f))
		{
			me->ForceRun(5.0f);
			me->Hurry(10.0f);
		}
		else
		{
			me->Walk();
		}

		// slash our victim
		me->FireWeaponAtEnemy();

		// if our victim has moved, repath
		bool repath = false;
		if (me->HasPath())
		{
			const float repathRange = 100.0f;
			if ((me->GetPathEndpoint() - pEnemy->pev->origin).IsLengthGreaterThan(repathRange))
			{
				repath = true;
			}
		}
		else
		{
			repath = true;
		}

		if (repath && m_repathTimer.IsElapsed())
		{
			// (Loner-Strike: someone out of sight: to where it last saw them)
			const Vector *to = (LsFight() && !me->IsEnemyVisible()) ? &me->GetLastKnownEnemyPosition() : &pEnemy->pev->origin;
			me->ComputePath(TheNavAreaGrid.GetNearestNavArea(to), to, FASTEST_ROUTE);
			if (LsFight() && me->IsEnemyVisible() && (pEnemy->pev->origin - me->pev->origin).IsLengthLessThan(160.0f) && RANDOM_FLOAT(0.0f, 1.0f) < 0.25f)
				me->Jump();

			const float repathInterval = 0.5f;
			m_repathTimer.Start(repathInterval);
		}

		// move towards victim
		if (me->UpdatePathMovement(NO_SPEED_CHANGE) != CCSBot::PROGRESSING)
		{
			me->DestroyPath();
		}

		return;
	}

	// Simple shield usage
	if (me->HasShield())
	{
		if (me->IsEnemyVisible() && !m_shieldForceOpen)
		{
			if (!me->IsRecognizedEnemyReloading() && !me->IsReloading() && me->IsPlayerLookingAtMe(pEnemy))
			{
				// close up - enemy is pointing his gun at us
				if (!me->IsProtectedByShield())
					me->SecondaryAttack();
			}
			else
			{
				// enemy looking away or reloading his weapon - open up and shoot him
				if (me->IsProtectedByShield())
					me->SecondaryAttack();
			}
		}
		else
		{
			// can't see enemy, open up
			if (me->IsProtectedByShield())
				me->SecondaryAttack();
		}

		if (gpGlobals->time > m_shieldToggleTimestamp)
		{
			m_shieldToggleTimestamp = gpGlobals->time + RANDOM_FLOAT(0.5, 2.0f);

			// toggle shield force open
			m_shieldForceOpen = !m_shieldForceOpen;
		}
	}

	// check if our weapon range is bad and we should switch to pistol
	if (me->IsUsingSniperRifle())
	{
		// if we have a sniper rifle and our enemy is too close, switch to pistol
		// NOTE: Must be larger than NO_ZOOM range in AdjustZoom()
		const float sniperMinRange = 310.0f;
		// (Loner-Strike: newer players keep the sniper rifle up close)
		if ((pEnemy->pev->origin - me->pev->origin).IsLengthLessThan(sniperMinRange) && !me->LsScopesClose())
			me->EquipPistol();
	}
	else if (me->IsUsingShotgun())
	{
		// if we have a shotgun equipped and enemy is too far away, switch to pistol
		const float shotgunMaxRange = 1000.0f;
		if ((pEnemy->pev->origin - me->pev->origin).IsLengthGreaterThan(shotgunMaxRange))
			me->EquipPistol();
	}

	// if we're sniping, look through the scope - need to do this here in case a reload resets our scope
	if (me->IsUsingSniperRifle())
	{
		// for Scouts and AWPs, we need to wait for zoom to resume
		if (me->m_bResumeZoom)
		{
			m_scopeTimestamp = gpGlobals->time;
			// (Loner-Strike: the better ones step out of the line while the bolt comes back)
			me->LsSniperStep();
			return;
		}

		Vector toAimSpot3D = me->m_aimSpot - me->pev->origin;
		float targetRange = toAimSpot3D.Length();

		// (Loner-Strike: and scope in anyway, which takes a moment)
		if (me->LsScopesClose())
			targetRange = Q_max(targetRange, 400.0f);

		// dont adjust zoom level if we're already zoomed in - just fire
		if (me->GetZoomLevel() == CCSBot::NO_ZOOM && me->AdjustZoom(targetRange))
			m_scopeTimestamp = gpGlobals->time;

		const float waitScopeTime = 0.2f + me->GetProfile()->GetReactionTime();
		if (gpGlobals->time - m_scopeTimestamp < waitScopeTime)
		{
			// force us to wait until zoomed in before firing
			return;
		}
	}

	// see if we "notice" that our prey is dead
	if (me->IsAwareOfEnemyDeath())
	{
		// let team know if we killed the last enemy
		if (me->GetLastVictimID() == pEnemy->entindex() && me->GetNearbyEnemyCount() <= 1)
		{
			me->GetChatter()->KilledMyEnemy(pEnemy->entindex());
		}

		StopAttacking(me);
		return;
	}

	float notSeenEnemyTime = gpGlobals->time - me->GetLastSawEnemyTimestamp();

	// if we haven't seen our enemy for a moment, continue on if we dont want to fight, or decide to ambush if we do
	if (!me->IsEnemyVisible())
	{
		// (Loner-Strike: ducked behind its own cover - a duck, a crouch to reload - it knows just
		// where they are: none of the "they went behind cover" reactions (ambush, a spray at the
		// wall, a chase))
		if (LsFight() && (me->m_lsDuckUntil > 0.0f || (me->m_lsReloadCrouched && me->IsReloading())))
			return;

		// attend to nearby enemy gunfire
		if (notSeenEnemyTime > 0.5f && me->CanHearNearbyEnemyGunfire())
		{
			// give up the attack, since we didn't want it in the first place
			StopAttacking(me);

			me->SetLookAt("Nearby enemy gunfire", me->GetNoisePosition(), PRIORITY_HIGH, 0.0f);
			me->PrintIfWatched("Checking nearby threatening enemy gunfire!\n");
			return;
		}

		// check if we have lost track of our enemy during combat
		if (notSeenEnemyTime > 0.25f)
		{
			m_isEnemyHidden = true;
		}

		if (notSeenEnemyTime > 0.1f)
		{
			// (Loner-Strike: some spray the door or thin wall the enemy just went behind)
			if (me->LsWallbang(notSeenEnemyTime))
				return;

			if (me->GetDisposition() == CCSBot::ENGAGE_AND_INVESTIGATE)
			{
				// decide whether we should hide and "ambush" our enemy
				if (m_haveSeenEnemy && !m_didAmbushCheck)
				{
					// (Loner-Strike: in deathmatch people chase or move on, not sit and wait; a
					// short wait now and then. Elsewhere a little less, and not as long)
					const float hideChance = !LsFight() ? 33.3f : LsSolo() ? 3.0f : 22.0f;

					if (RANDOM_FLOAT(0.0, 100.0f) < hideChance)
					{
						float ambushTime = !LsFight() ? RANDOM_FLOAT(3.0f, 15.0f) : LsSolo() ? RANDOM_FLOAT(1.0f, 2.5f) : RANDOM_FLOAT(3.0f, 10.0f);

						// hide in ambush nearby
						// TODO: look towards where we know enemy is
						const Vector *spot = FindNearbyRetreatSpot(me, 200.0f);
						if (spot)
						{
							me->IgnoreEnemies(1.0f);
							me->Run();
							me->StandUp();
							me->Hide(spot, ambushTime, true);
							me->LsWhy("ambush");
							return;
						}
					}

					// don't check again
					m_didAmbushCheck = true;
				}
			}
			else
			{
				// give up the attack, since we didn't want it in the first place
				StopAttacking(me);
				return;
			}
		}
	}
	else
	{
		// we can see the enemy again - reset our ambush check
		// (Loner-Strike: once per fight - each re-peek re-rolling it made a run to a crouch spot
		// mid-duel likely)
		if (!LsFight())
			m_didAmbushCheck = false;

		// if the enemy is coming out of hiding, we need time to react
		if (m_isEnemyHidden)
		{
			m_reacquireTimestamp = gpGlobals->time + me->GetProfile()->GetReactionTime();
			m_isEnemyHidden = false;
		}
	}

	// if we haven't seen our enemy for a long time, chase after them
	float chaseTime = 2.0f + 2.0f * (1.0f - me->LsAggro());
	// (Loner-Strike: in deathmatch people go after someone who ducked out of sight, the
	// aggressive ones straight away)
	if (LsFight() && LsSolo())
		chaseTime = (0.3f + 1.7f * (1.0f - me->LsAggro())) * (me->LsHealth() < 20.0f ? 2.0f : 1.0f);
	else if (LsFight())
		chaseTime = Q_max(0.2f, chaseTime - me->LsChaseExtra());

	// if we are sniping, be very patient
	if (me->IsUsingSniperRifle())
		chaseTime += 3.0f;
	// if we are crouching, be a little patient
	else if (me->IsCrouching())
		chaseTime += 1.0f;

	// if we can't see the enemy, and have either seen him but currently lost sight of him,
	// or haven't yet seen him, chase after him (unless we are a sniper)
	if (!me->IsEnemyVisible() && (notSeenEnemyTime > chaseTime || !m_haveSeenEnemy))
	{
		// (Loner-Strike: guarding the planted bomb, most hold rather than chase)
		if (me->LsHoldsPlant())
		{
			StopAttacking(me);
			return;
		}

		// snipers don't chase their prey - they wait for their prey to come to them
		if (me->GetTask() == CCSBot::SNIPING)
		{
			StopAttacking(me);
			return;
		}

		// Loner-Strike: only a glimpse of them (gone before it could react): a careful or good
		// player doesn't charge through the gap after them - it holds an angle where they went;
		// the bold and the newer ones go after them
		// (a glimpse: under about 0.6 s in view this fight - a quick player recognises someone in
		// a frame, so "seen at all" isn't the test; the ones who don't hold go after them below)
		if (LsFight() && !LsSolo() && (!m_haveSeenEnemy || m_lsVisibleFor < 0.6f))
		{
			int style = me->LsStyle();
			float holdOdds = (0.15f + 0.6f * me->LsSkill()) * (1.0f - 0.5f * me->LsAggro())
				* ((style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER) ? 1.4f : (style == LS_STYLE_RUSHER || style == LS_STYLE_KNIFER) ? 0.3f : 1.0f);
			if (me->LsTrait(54) < holdOdds && me->LsPreAim(me->GetLastKnownEnemyPosition(), RANDOM_FLOAT(2.0f, 3.5f)))
			{
				StopAttacking(me);
				return;
			}
		}

		// move to last known position of enemy
		me->SetTask(CCSBot::MOVE_TO_LAST_KNOWN_ENEMY_POSITION, pEnemy);
		me->MoveTo(&me->GetLastKnownEnemyPosition());
		return;
	}

	// if we can't see our enemy at the moment, and were shot by
	// a different visible enemy, engage them instead
	const float hurtRecentlyTime = 3.0f;
	if (!me->IsEnemyVisible() &&
		me->GetTimeSinceAttacked() < hurtRecentlyTime &&
		me->GetAttacker() &&
		me->GetAttacker() != me->GetEnemy())
	{
		// if we can see them, attack, otherwise panic
		if (me->IsVisible(me->GetAttacker(), CHECK_FOV))
		{
			me->Attack(me->GetAttacker());
			me->PrintIfWatched("Switching targets to retaliate against new attacker!\n");
		}
		return;
	}

	// Loner-Strike: stop, then shoot (a skilled bot with a rifle at range)
	float lsRange = (pEnemy->pev->origin - me->pev->origin).Length2D();
	bool lsHoldFire = LsFight() && me->LsHoldFireToStop(lsRange);
	if (lsHoldFire)
	{
		if (me->m_lsHoldSince <= 0.0f)
			me->m_lsHoldSince = gpGlobals->time;
		// never stuck waiting to stop: it shoots anyway after a moment
		else if (gpGlobals->time - me->m_lsHoldSince > 0.6f)
			lsHoldFire = false;
	}
	else
		me->m_lsHoldSince = 0.0f;
	// (Loner-Strike: the wrong gun for the distance and it isn't being shot at: no spraying from
	// out of range - in closer first)
	bool lsClosing = false;
	if (LsFight() && me->LsOutOfRange(pEnemy, lsRange))
	{
		lsHoldFire = true;
		if (gpGlobals->time > me->m_lsPushUntil && !me->IsUsingSniperRifle())
			me->m_lsPushUntil = gpGlobals->time + RANDOM_FLOAT(0.8f, 1.4f);
		// (closing the distance is the point: it walks in, it doesn't stop dead in their view)
		lsClosing = !me->IsUsingSniperRifle() && !m_crouchAndHold;
		if (lsClosing)
			m_dodge = true;
	}

	// (Loner-Strike: the walkers slow to a walk to shoot at range with a rifle, back to a run after)
	if (LsFight() && !me->IsUsingKnife() && me->LsStopStyle() == 2)
	{
		bool lsWalk = lsRange > 350.0f && me->LsStopGun() && me->IsEnemyVisible() && !lsClosing;
		if (lsWalk && me->IsRunning())
			me->Walk();
		else if (!lsWalk && !me->IsRunning())
			me->Run();
	}

	if (gpGlobals->time > m_reacquireTimestamp && !lsHoldFire)
		me->FireWeaponAtEnemy();

	// Loner-Strike: a crouch while shooting, a jump to look round a corner (some bots)
	if (LsFight())
	{
		me->LsFightHabits(lsRange, m_crouchAndHold);
		me->LsPanicUpdate();
	}

	// Loner-Strike: hit, it moves (whatever it decided when the fight began)
	bool lsDucked = LsFight() && (me->m_lsDuckUntil > 0.0f || me->m_lsReloadCrouched);
	if (LsFight() && !m_dodge && !m_crouchAndHold && !lsDucked && me->GetTimeSinceAttacked() < 1.0f)
		m_dodge = true;

	// Loner-Strike: on top of a teammate, a step aside first
	// (not one crouched to hold or ducked behind cover to reload: that stays put)
	if (LsFight() && !m_crouchAndHold && !lsDucked && !me->IsUsingSniperRifle() && me->LsSpaceOut())
		return;

	// do dodge behavior
	// If sniping or crouching, stand still.
	if (m_dodge && !me->IsUsingSniperRifle() && !m_crouchAndHold && !lsDucked)
	{
		Vector toEnemy = pEnemy->pev->origin - me->pev->origin;
		float range = toEnemy.Length2D();

		// Loner-Strike: stopping to shoot ends the strafe now (with a counter-strafe)
		if (lsHoldFire && !lsClosing && m_dodgeState != STEADY_ON)
		{
			// (a crouch-stopper crouches instead of the counter-strafe)
			if (me->LsStopStyle() == 1)
				me->LsCrouchStop();
			else
				me->LsCounterStrafe(m_dodgeState == SLIDE_LEFT ? 1 : 2);
			m_dodgeState = STEADY_ON;
			m_nextDodgeStateTimestamp = gpGlobals->time + me->LsDodgeDuration(true);
		}

		const float hysterisRange = 125.0f;		// (+/-) m_combatRange

		float minRange = me->GetCombatRange() - hysterisRange;
		float maxRange = me->GetCombatRange() + hysterisRange;

		// move towards (or away from) enemy if we are using a knife, behind a corner, or we aren't very skilled
		// (Loner-Strike: not while stopping to shoot)
		if (!lsHoldFire && (me->GetProfile()->GetSkill() < 0.66f || !me->IsEnemyVisible()))
		{
			if (range > maxRange)
				me->MoveForward();
			else if (range < minRange && (!LsFight() || me->LsMayBackOff(toEnemy)))
				me->MoveBackward();
		}

		// don't dodge if enemy is facing away
		const float dodgeRange = 2000.0f;
		if (range > dodgeRange || !me->IsPlayerFacingMe(pEnemy))
		{
			m_dodgeState = STEADY_ON;
			m_nextDodgeStateTimestamp = 0.0f;
		}
		else if (lsHoldFire && !lsClosing)
		{
			// Loner-Strike: stopping dead to shoot: keys against its own slide (or a crouch)
			m_dodgeState = STEADY_ON;
			if (me->LsStopStyle() == 1)
				me->LsCrouchStop();
			else
				me->LsStopDead();
		}
		else if (gpGlobals->time >= m_nextDodgeStateTimestamp)
		{
			int next;

			// select next dodge state that is different that our current one
			do
			{
				// high-skill bots may jump when first engaging the enemy (if they are moving)
				const float jumpChance = 33.3f;
				if (m_firstDodge && me->GetProfile()->GetSkill() > 0.5f && RANDOM_FLOAT(0, 100) < jumpChance && !me->IsNotMoving())
					next = RANDOM_LONG(0, NUM_ATTACK_STATES - 1);
				else
					next = RANDOM_LONG(0, NUM_ATTACK_STATES - 2);
			}
			while (!m_firstDodge && next == m_dodgeState);

			// Loner-Strike: its own rhythm, and its own taste for stopping between strafes
			// (a counter-strafe stops it dead)
			if (LsFight() && next != JUMP && m_dodgeState != STEADY_ON && me->LsPickSteady())
				next = STEADY_ON;
			if (LsFight() && next == STEADY_ON && (m_dodgeState == SLIDE_LEFT || m_dodgeState == SLIDE_RIGHT))
				me->LsCounterStrafe(m_dodgeState == SLIDE_LEFT ? 1 : 2);

			m_dodgeState = (DodgeStateType)next;
			m_nextDodgeStateTimestamp = gpGlobals->time + (LsFight() ? me->LsDodgeDuration(next == STEADY_ON) : RANDOM_FLOAT(0.3f, 1.0f));

			// Loner-Strike: or just run at them for a moment (close-range guns, W-key players)
			if (LsFight() && !lsHoldFire && me->LsPushes(range))
				me->m_lsPushUntil = gpGlobals->time + RANDOM_FLOAT(0.4f, 1.1f);
			m_firstDodge = false;
		}

		if (LsFight() && gpGlobals->time < me->m_lsPushUntil && (!lsHoldFire || lsClosing) && !lsDucked)
			me->MoveForward();

		switch (m_dodgeState)
		{
		case STEADY_ON:
		{
			// Loner-Strike: the counter-strafe tap
			if (gpGlobals->time < me->m_lsCounterUntil)
			{
				if (me->m_lsCounterDir > 0)
					me->StrafeLeft();
				else
					me->StrafeRight();
			}
			break;
		}
		case SLIDE_LEFT:
		{
			me->StrafeLeft();
			break;
		}
		case SLIDE_RIGHT:
		{
			me->StrafeRight();
			break;
		}
		case JUMP:
		{
			if (me->m_isEnemyVisible)
			{
				me->Jump();
			}
			break;
		}
		}
	}
}

// Finish attack
void AttackState::OnExit(CCSBot *me)
{
	me->m_lsDuckUntil = 0.0f;
	me->m_lsReloadCrouched = false;
	me->PrintIfWatched("AttackState:OnExit()\n");

	m_crouchAndHold = false;

	// clear any noises we heard during battle
	me->ForgetNoise();
	me->ResetStuckMonitor();

	// resume our original posture
	me->PopPostureContext();

	// put shield away
	if (me->IsProtectedByShield())
		me->SecondaryAttack();

	me->StopRapidFire();
	me->ClearSurpriseDelay();
}
