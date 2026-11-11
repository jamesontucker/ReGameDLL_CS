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

// Begin the hunt
void HuntState::OnEnter(CCSBot *me)
{
	// lurking death
	// (Loner-Strike: not in deathmatch-type modes, where nobody creeps about with a knife, nor
	// when the knife is all it has: knife arenas)
	if (me->IsUsingKnife() && me->IsWellPastSafe() && !me->IsHurrying() && !(LsFight() && (LsSolo() || me->LsKnifeOnly())))
		me->Walk();
	else
		me->Run();

	me->StandUp();
	me->SetDisposition(CCSBot::ENGAGE_AND_INVESTIGATE);
	me->SetTask(CCSBot::SEEK_AND_DESTROY);
	me->DestroyPath();
}

// Hunt down our enemies
void HuntState::OnUpdate(CCSBot *me)
{
	// if we've been hunting for a long time, drop into Idle for a moment to
	// select something else to do
	const float huntingTooLongTime = 30.0f;
	if (gpGlobals->time - me->GetStateTimestamp() > huntingTooLongTime)
	{
		// stop being a rogue and do the scenario, since there must not be many enemies left to hunt
		me->PrintIfWatched("Giving up hunting, and being a rogue\n");
		me->SetRogue(false);
		me->Idle();
		return;
	}

	// scenario logic
	if (TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB)
	{
		if (me->m_iTeam == TERRORIST)
		{
			// if we have the bomb and it's time to plant, or we happen to be in a bombsite and it seems safe, do it
			if (me->IsCarryingBomb())
			{
				const float safeTime = 3.0f;
				if (TheCSBots()->IsTimeToPlantBomb() || (me->IsAtBombsite() && gpGlobals->time - me->GetLastSawEnemyTimestamp() > safeTime))
				{
					me->Idle();
					return;
				}
			}

			// if we notice the bomb lying on the ground, go get it
			// (Loner-Strike: the nearest one or two, not the whole team)
			if (me->NoticeLooseBomb() && me->LsFetchesBomb())
			{
				me->FetchBomb();
				return;
			}

			// if bomb has been planted, and we hear it, move to a hiding spot near the bomb and watch it
			const Vector *bombPos = me->GetGameState()->GetBombPosition();
			if (!me->LsShirksObjective() && me->GetGameState()->IsBombPlanted() && bombPos)
			{
				me->SetTask(CCSBot::GUARD_TICKING_BOMB);
				me->LsHideNearBomb(*bombPos);	// (Loner-Strike: spread out round it, not one spot for all)
				return;
			}
		}
		// CT
		else
		{
			if (!me->LsShirksObjective() && me->CanSeeLooseBomb())
			{
				CNavArea *looseBombArea = TheCSBots()->GetLooseBombArea();

				// if we are near the loose bomb and can see it, hide nearby and guard it
				me->SetTask(CCSBot::GUARD_LOOSE_BOMB);
				me->Hide(looseBombArea);

				if (looseBombArea)
					me->GetChatter()->AnnouncePlan("GoingToGuardLooseBomb", looseBombArea->GetPlace());

				return;
			}
			else if (TheCSBots()->IsBombPlanted())
			{
				// rogues will defuse a bomb, but not guard the defuser
				if (!me->LsShirksObjective() || !TheCSBots()->GetBombDefuser())
				{
					// search for the planted bomb to defuse
					me->Idle();
					return;
				}
			}
		}
	}
	else if (TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_RESCUE_HOSTAGES)
	{
		if (me->m_iTeam == TERRORIST)
		{
			if (me->GetGameState()->AreAllHostagesBeingRescued())
			{
				// all hostages are being rescued, head them off at the escape zones
				if (me->GuardRandomZone())
				{
					me->SetTask(CCSBot::GUARD_HOSTAGE_RESCUE_ZONE);
					me->PrintIfWatched("Trying to beat them to an escape zone!\n");
					me->SetDisposition(CCSBot::OPPORTUNITY_FIRE);
					me->GetChatter()->GuardingHostageEscapeZone(IS_PLAN);
					return;
				}
			}

			// if safe time is up, and we stumble across a hostage, guard it
			if (!me->LsShirksObjective() && !me->IsSafe())
			{
				CHostage *pHostage = me->GetGameState()->GetNearestVisibleFreeHostage();
				if (pHostage)
				{
					CNavArea *area = TheNavAreaGrid.GetNearestNavArea(&pHostage->pev->origin);
					if (area)
					{
						// we see a free hostage, guard it
						me->SetTask(CCSBot::GUARD_HOSTAGES);
						me->Hide(area);
						me->PrintIfWatched("I'm guarding hostages\n");
						me->GetChatter()->GuardingHostages(area->GetPlace(), IS_PLAN);
						return;
					}
				}
			}
		}
	}

	// listen for enemy noises
	if (me->ShouldInvestigateNoise())
	{
		me->InvestigateNoise();
		return;
	}
	// (Loner-Strike: stopped a moment to hold an angle on a sound: no moving on, and the eyes
	// stay on that angle - still hunting, so no re-plan when it's over)
	if (LsFight() && gpGlobals->time < me->m_lsHuntHoldUntil)
	{
		// (standing still on purpose: not stuck, and not giving up on the area it's in)
		me->ClearMovement();
		me->ResetStuckMonitor();
		return;
	}

	// look around
	me->UpdateLookAround();

	// Loner-Strike: shift-walk where it's worth being quiet (the sneaky ones)
	me->LsSneak();

	// if we have reached our destination area, pick a new one
	// if our path fails, pick a new one
	// (Loner-Strike: or, every bot for itself, it finds itself walking with a teammate)
	if ((me->GetLastKnownArea() == m_huntArea || me->UpdatePathMovement() != CCSBot::PROGRESSING || (me->LsLoner() && me->LsShouldSpread()))
		&& !(LsFight() && gpGlobals->time < me->m_lsHuntRetryAt))
	{
		// Loner-Strike: every bot for itself: somewhere its teammates aren't (and on a hostage
		// map every T, so the Ts hold the building rather than wander to the CT spawn)
		if (me->LsLoner() || (LsFight() && me->m_iTeam == TERRORIST && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_RESCUE_HOSTAGES))
		{
			m_huntArea = me->LsPickHuntArea();
			if (m_huntArea)
			{
				me->LsWhy("hunt");
				// (in deathmatch the quickest way there: the safest kept teammates on the
				// same few routes, away from the enemy; a few who've been killed lately
				// take the long way round, a flank)
				bool flank = LsSolo() && LsOwnMemory() && me->LsSinceDied() < 30.0f && RANDOM_FLOAT(0.0f, 1.0f) < 0.15f + 0.15f * me->LsTrait(84);
				// (no way there: the stock pick below, not another random one next frame)
				if (me->ComputePath(m_huntArea, nullptr, (LsSolo() && !flank) ? FASTEST_ROUTE : SAFEST_ROUTE))
					return;
			}
		}

		// Loner-Strike: on a bomb or hostage map: somewhere worth going - forward, toward the
		// objectives and the other side, never wandering back to its own spawn
		if (LsFight() && !LsSolo() && TheCSBots()->GetScenario() != CCSBotManager::SCENARIO_DEATHMATCH)
		{
			m_huntArea = me->LsObjectiveHuntArea();
			if (m_huntArea && me->ComputePath(m_huntArea, nullptr, SAFEST_ROUTE))
			{
				me->LsWhy("hunt: forward");
				return;
			}
		}

		me->LsWhy("hunt-stock");
		m_huntArea = nullptr;
		float oldest = 0.0f;

		int areaCount = 0;
		const float minSize = 150.0f;
		for (auto area : TheNavAreaList)
		{
			areaCount++;

			// skip the small areas
			const Extent *extent = area->GetExtent();
			if (extent->hi.x - extent->lo.x < minSize || extent->hi.y - extent->lo.y < minSize)
				continue;

			// keep track of the least recently cleared area
			real_t age = gpGlobals->time - area->GetClearedTimestamp(me->m_iTeam - 1);
			if (age > oldest)
			{
				oldest = age;
				m_huntArea = area;
			}
		}

		// if all the areas were too small, pick one at random
		// (Loner-Strike: only then - stock always overwrote the oldest-area pick with this)
		if (!m_huntArea || !LsFight())
		{
			int which = RANDOM_LONG(0, areaCount - 1);

			areaCount = 0;
			for (auto area : TheNavAreaList)
			{
				m_huntArea = area;

				if (which == areaCount)
					break;

				which--;
			}
		}

		if (m_huntArea)
		{
			// create a new path to a far away area of the map
			// (Loner-Strike: no way there either: a moment before trying again, not a fresh pick
			// every think standing on the spot)
			if (!me->ComputePath(m_huntArea, nullptr, SAFEST_ROUTE) && LsFight())
				me->m_lsHuntRetryAt = gpGlobals->time + 1.0f;
		}
	}
}

// Done hunting
void HuntState::OnExit(CCSBot *me)
{
	;
}
