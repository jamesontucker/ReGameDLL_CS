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

void CCSBot::OnEvent(GameEventType event, CBaseEntity *pEntity, CBaseEntity *pOther)
{
	LS_PROF("OnEvent");
	GetGameState()->OnEvent(event, pEntity, pOther);
	GetChatter()->OnEvent(event, pEntity, pOther);

	// Morale adjustments happen even for dead players
	switch (event)
	{
	case EVENT_TERRORISTS_WIN:
		if (m_iTeam == CT)
		{
			DecreaseMorale();
		}
		else
		{
			IncreaseMorale();
		}
		break;
	case EVENT_CTS_WIN:
		if (m_iTeam == CT)
		{
			IncreaseMorale();
		}
		else
		{
			DecreaseMorale();
		}
		break;
#ifdef REGAMEDLL_FIXES
	case EVENT_NEW_MATCH:
		m_morale = POSITIVE;		// starting a new round makes everyone a little happy
		break;
#endif
	}

	// Loner-Strike: its own death (where, by whom), noted before the alive check below
	if (event == EVENT_PLAYER_DIED && pEntity == this && LsFight())
		LsNoteDeath((pOther && pOther->IsPlayer()) ? static_cast<CBasePlayer *>(pOther) : nullptr);
	// (and a person's, which no bot notes for itself: human teammates count as much)
	else if (event == EVENT_PLAYER_DIED && pEntity && pEntity != this && LsFight() && static_cast<CBasePlayer *>(pEntity)->IsPlayer()
		&& !static_cast<CBasePlayer *>(pEntity)->IsBot())
		LsNoteAnyDeath(static_cast<CBasePlayer *>(pEntity));

	// (a CT down by a site: the CT side reads a site being hit off the radar)
	if (event == EVENT_PLAYER_DIED && pEntity && LsFight() && static_cast<CBasePlayer *>(pEntity)->IsPlayer())
		LsCtSiteDeath(static_cast<CBasePlayer *>(pEntity));

	if (!IsAlive())
		return;

	CBasePlayer *pPlayer = static_cast<CBasePlayer *>(pEntity);

	// Loner-Strike: a flash going off. It knows someone over there may be blind only when it saw
	// the pop, or a teammate threw it (they call their flashes), never from the game's own flag
	if (event == EVENT_FLASHBANG_GRENADE_EXPLODED && pOther && LsFight())
	{
		const Vector *pop = (const Vector *)pOther;
		bool ours = pPlayer && pPlayer->IsPlayer() && (pPlayer == this || !IsEnemy(pPlayer));
		if (ours || IsVisible(pop, true))
		{
			m_lsFlashPopAt = gpGlobals->time;
			m_lsFlashPopPos = *pop;
			// (a flash blinds about 2 s: a good player's guess is close, a newer one's way off)
			m_lsFlashGuess = 2.0f + RANDOM_FLOAT(-1.0f, 1.0f) * (1.0f - 0.75f * LsSkill());
		}
		// (only heard: the bang, from roughly over there)
		else if ((*pop - pev->origin).IsLengthLessThan(1500.0f))
		{
			float err = 350.0f * (1.0f - 0.6f * LsSkill());
			m_lsFlashHeardAt = gpGlobals->time;
			m_lsFlashHeardPos = *pop + Vector(RANDOM_FLOAT(-err, err), RANDOM_FLOAT(-err, err), 0.0f);
		}
	}

	// Loner-Strike: a teammate down: the radar shows where, the kill feed with what
	if (event == EVENT_PLAYER_DIED && pPlayer && pPlayer != this && pPlayer->IsPlayer() && LsFight())
		LsFeed(pPlayer, (pOther && pOther->IsPlayer()) ? static_cast<CBasePlayer *>(pOther) : nullptr);

	// Loner-Strike: a hit of its own: not whiffing
	if (event == EVENT_PLAYER_TOOK_DAMAGE && pOther == this)
		m_lsShotsNoHit = 0;

	// Loner-Strike: one of its own team hit an enemy who's still up and hurt: "he's lit" (called
	// by the shooter; once per hit for the whole team, from the first bot that hears it)
	// (only a bot calls it - a person may not - and only a hit on someone it could see, not a
	// wallbang it can't judge; the hp as a player would call it, to the nearest ten)
	if (event == EVENT_PLAYER_TOOK_DAMAGE && pEntity && pEntity->IsPlayer() && pOther && pOther->IsPlayer() && pOther != this && LsFight()
		&& static_cast<CBasePlayer *>(pOther)->IsBot() && static_cast<CBasePlayer *>(pOther)->m_iTeam == m_iTeam
		&& IsEnemy(static_cast<CBasePlayer *>(pEntity)) && pEntity->IsAlive() && pEntity->pev->health < 60.0f
		&& static_cast<CCSBot *>(pOther)->IsVisible(static_cast<CBasePlayer *>(pEntity), false))
		LsCallHurt(static_cast<CBasePlayer *>(pEntity), m_iTeam, pEntity->pev->health, pEntity->pev->origin);

	// Loner-Strike: hit: from someone it can't see, through a wall?
	if (event == EVENT_PLAYER_TOOK_DAMAGE && pEntity == this && pOther && pOther->IsPlayer())
	{
		// (running to cover, enemies ignored: hit, it fights back; hit where it's hiding, that
		// spot isn't cover - avoided a while)
		// (only an enemy's: a teammate's stray bullet or grenade scratch isn't the spot failing)
		if (LsFight() && pOther && pOther->IsPlayer() && IsEnemy(static_cast<CBasePlayer *>(pOther)))
		{
			m_ignoreEnemiesTimer.Invalidate();
			if (IsHiding() && IsAtHidingSpot() && m_lastKnownArea)
				LsForceBadArea(m_lastKnownArea, 15.0f);
		}
		LsHurtBy(static_cast<CBasePlayer *>(pOther));
	}

	// Loner-Strike: something breaking (the event carries the glass, not who broke it)
	if (event == EVENT_BREAK_GLASS || event == EVENT_BREAK_WOOD || event == EVENT_BREAK_METAL || event == EVENT_BREAK_CONCRETE)
	{
		LsHeardBreak(pEntity);
		// (a floor that broke: a hole now, until the round restarts)
		if ((event == EVENT_BREAK_GLASS || event == EVENT_BREAK_WOOD) && IsFloorBreakable(pEntity))
			LsHoleCheck();
		return;
	}

	// Loner-Strike: an enemy's bullets landing close (or passing close): being shot at
	if (event == EVENT_BULLET_IMPACT)
	{
		if (LsFight() && pPlayer && pOther)
		{
			LsShotAt(pPlayer, *reinterpret_cast<Vector *>(pOther));
			LsImpactNearMate(pPlayer, *reinterpret_cast<Vector *>(pOther));
		}
		return;
	}

	// Loner-Strike: a teammate dying in sight is remembered as danger by this bot alone
	if (event == EVENT_PLAYER_DIED && pPlayer && pPlayer != this && pPlayer->IsPlayer() && BotRelationship(pPlayer) == BOT_TEAMMATE)
		LsSawTeammateDie(pPlayer);
	// ...and Ts dying at the round's site early: the team may switch sites
	if (event == EVENT_PLAYER_DIED && pPlayer && pPlayer->IsPlayer() && LsFight())
		LsTSiteDeath(pPlayer);
	// ...and an enemy dying early in the round to a teammate is a reason to push
	if (event == EVENT_PLAYER_DIED && pPlayer && pPlayer->IsPlayer() && pOther && pOther->IsPlayer())
		LsEarlyPick(pPlayer, static_cast<CBasePlayer *>(pOther));
	// ...and one it killed itself: its aim stays on them a moment, as people's does
	// (where they stood, at head height; only the newer ones look down at the body; not in
	// deathmatch, where the next one's already coming)
	if (event == EVENT_PLAYER_DIED && pPlayer && pPlayer != this && pOther == this && LsFight() && !LsSolo() && !IsBlind())
	{
		// (the good ones keep their aim on the spot; casual players let go, a glance down and away)
		bool release = LsSkill() < 0.5f && LsTrait(95) < 0.6f;
		Vector spot = release ? pPlayer->pev->origin + Vector(RANDOM_FLOAT(-40.0f, 40.0f), RANDOM_FLOAT(-40.0f, 40.0f), -20.0f)
			: pPlayer->pev->origin + Vector(0, 0, HalfHumanHeight + 10.0f);
		SetLookAt("Loner-Strike: the kill", &spot, PRIORITY_MEDIUM, RANDOM_FLOAT(0.3f, 0.5f) + 0.5f * (1.0f - LsAggro()));
		m_lsShotsNoHit = 0;
	}

	// If we just saw a nearby friend die, and we haven't yet acquired an enemy
	// automatically acquire our dead friend's killer
	if (!IsAttacking() && (GetDisposition() == ENGAGE_AND_INVESTIGATE || GetDisposition() == OPPORTUNITY_FIRE))
	{
		if (event == EVENT_PLAYER_DIED)
		{
			CBasePlayer *pVictim = pPlayer;

			if (BotRelationship(pVictim) == BOT_TEAMMATE)
			{
				CBasePlayer *pKiller = static_cast<CBasePlayer *>(pOther);

				// check that attacker is an enemy (for friendly fire, etc)
				if (pKiller && pKiller->IsPlayer() && BotRelationship(pKiller) == BOT_ENEMY)
				{
					// Loner-Strike: a teammate killed close by but out of sight - the shots heard, the
					// kill feed, where they were on the radar: the same reaction as seeing it (a look
					// the killer's way as heard, and by who it is, holding the angle or going to help),
					// not carrying on with its back to it
					// (same level: on nuke, a death on the floor below is heard but isn't round the
					// corner)
					// (a knife or a silenced gun makes nothing to hear: no direction from it)
					CBasePlayerWeapon *pKillGun = static_cast<CBasePlayerWeapon *>(pKiller->m_pActiveItem);
					bool lsQuietKill = pKillGun && (pKillGun->m_iId == WEAPON_KNIFE
						|| (pKillGun->m_iId == WEAPON_M4A1 && (pKillGun->m_iWeaponState & WPNSTATE_M4A1_SILENCED))
						|| (pKillGun->m_iId == WEAPON_USP && (pKillGun->m_iWeaponState & WPNSTATE_USP_SILENCED)));
					if (LsFight() && !IsSniper() && !lsQuietKill && !IsVisible(&pVictim->pev->origin) && (pVictim->pev->origin - pev->origin).IsLengthLessThan(900.0f)
						&& Q_abs(pVictim->pev->origin.z - pev->origin.z) < 140.0f && !IsVisible(pKiller, true))
					{
						// (no return: the kill feed, the death noted near it and the rest below still
						// happen for this death)
						LsAvenge(pVictim, pKiller);
					}

					// check if we saw our friend die - dont check FOV - assume we're aware of our surroundings in combat
					// snipers stay put
					if (!IsSniper() && IsVisible(&pVictim->pev->origin))
					{
						// Loner-Strike: a killer it can't see: a look his way (as heard) and on
						// to where the teammate fell, not a run straight at him
						if (LsFight() && !IsVisible(pKiller, true))
						{
							LsAvenge(pVictim, pKiller);
							return;
						}

						// people are dying - we should hurry
						Hurry(RANDOM_FLOAT(10.0f, 15.0f));

						// if we're hiding with only our knife, be a little more cautious
						const float knifeAmbushChance = 50.0f;
						if (!IsHiding() || !IsUsingKnife() || RANDOM_FLOAT(0, 100) < knifeAmbushChance)
						{
							PrintIfWatched("Attacking our friend's killer!\n");
							LsWhy("avenge");
							Attack(pKiller);
							return;
						}
					}
				}
			}
		}
	}

	switch (event)
	{
	case EVENT_PLAYER_DIED:
	{
		CBasePlayer *pVictim = pPlayer;
		CBasePlayer *pKiller = (pOther && pOther->IsPlayer()) ? static_cast<CBasePlayer *>(pOther) : nullptr;

		// if the human player died in the single player game, tell the team
		if (CSGameRules()->IsCareer() && !pVictim->IsBot() && BotRelationship(pVictim) == BOT_TEAMMATE)
		{
			GetChatter()->Say("CommanderDown", 20.0f);
		}

		// keep track of the last player we killed
		if (pKiller == this)
		{
			m_lastVictimID = pVictim->entindex();
		}

		// react to teammate death
		if (BotRelationship(pVictim) == BOT_TEAMMATE)
		{
			// chastise friendly fire from humans
			if (pKiller && !pKiller->IsBot() && BotRelationship(pKiller) == BOT_TEAMMATE && pKiller != this)
			{
				GetChatter()->KilledFriend();
			}

			if (IsHunting())
			{
				PrintIfWatched("Rethinking hunt due to teammate death\n");
				Idle();
				return;
			}

			if (IsAttacking())
			{
				if (GetTimeSinceLastSawEnemy() > 0.4f)
				{
					PrintIfWatched("Rethinking my attack due to teammate death\n");

					// allow us to sneak past windows, doors, etc
					IgnoreEnemies(1.0f);

					// move to last known position of enemy - this could cause us to flank if
					// the danger has changed due to our teammate's recent death
					SetTask(MOVE_TO_LAST_KNOWN_ENEMY_POSITION, GetEnemy());
					MoveTo(&GetLastKnownEnemyPosition());
					return;
				}
			}
		}
		// an enemy was killed
		else
		{
			if (pKiller && BotRelationship(pKiller) == BOT_TEAMMATE)
			{
				// only chatter about enemy kills if we see them occur, and they were the last one we see
				if (GetNearbyEnemyCount() <= 1)
				{
					// report if number of enemies left is few and we killed the last one we saw locally
					GetChatter()->EnemiesRemaining();

					if (IsVisible(&pVictim->pev->origin, CHECK_FOV))
					{
						// congratulate teammates on their kills
						if (pKiller != this)
						{
							float delay = RANDOM_FLOAT(2.0f, 3.0f);
							if (pKiller->IsBot())
							{
								if (RANDOM_FLOAT(0.0f, 100.0f) < 40.0f)
									GetChatter()->Say("NiceShot", 3.0f, delay);
							}
							else
							{
								// humans get the honorific
								if (CSGameRules()->IsCareer())
									GetChatter()->Say("NiceShotCommander", 3.0f, delay);
								else
									GetChatter()->Say("NiceShotSir", 3.0f, delay);
							}
						}
					}
				}
			}
		}
		return;
	}
	case EVENT_TERRORISTS_WIN:
		if (m_iTeam == TERRORIST)
			GetChatter()->CelebrateWin();
		return;
	case EVENT_CTS_WIN:
		if (m_iTeam == CT)
			GetChatter()->CelebrateWin();
		return;
	case EVENT_BOMB_DEFUSED:
		if (m_iTeam == CT && TheCSBots()->GetBombTimeLeft() < 2.0)
			GetChatter()->Say("BarelyDefused");
		return;
	case EVENT_BOMB_PICKED_UP:
	{
		if (m_iTeam == CT && pPlayer)
		{
			// check if we're close enough to hear it
			const float bombPickupHearRangeSq = 1000.0f * 1000.0f;
			if ((pev->origin - pPlayer->pev->origin).LengthSquared() < bombPickupHearRangeSq)
			{
				GetChatter()->TheyPickedUpTheBomb();
			}
		}
		return;
	}
	case EVENT_BOMB_BEEP:
	{
		// if we don't know where the bomb is, but heard it beep, we've discovered it
		if (GetGameState()->IsPlantedBombLocationKnown() == false)
		{
			// check if we're close enough to hear it
			const float bombBeepHearRangeSq = 1000.0f * 1000.0f;
			if ((pev->origin - pEntity->pev->origin).LengthSquared() < bombBeepHearRangeSq)
			{
				// radio the news to our team
				if (m_iTeam == CT && GetGameState()->GetPlantedBombsite() == CSGameState::UNKNOWN)
				{
					const CCSBotManager::Zone *zone = TheCSBots()->GetZone(&pEntity->pev->origin);
					if (zone)
					{
						GetChatter()->FoundPlantedBomb(zone->m_index);
					}
				}

				// remember where the bomb is
				GetGameState()->UpdatePlantedBomb(&pEntity->pev->origin);
			}
		}
		return;
	}
	case EVENT_BOMB_PLANTED:
	{
		// if we're a CT, forget what we're doing and go after the bomb
		if (m_iTeam == CT)
		{
			Idle();
		}

		// if we are following someone, stop following
		if (IsFollowing())
		{
			StopFollowing();
			Idle();
		}

		OnEvent(EVENT_BOMB_BEEP, pOther);
		return;
	}
	case EVENT_BOMB_DEFUSE_ABORTED:
		PrintIfWatched("BOMB DEFUSE ABORTED\n");
		return;
	case EVENT_WEAPON_FIRED:
	case EVENT_WEAPON_FIRED_ON_EMPTY:
	case EVENT_WEAPON_RELOADED:
	{
		if (m_enemy == pEntity && IsUsingKnife())
			ForceRun(5.0f);
		break;
	}
	default:
	{
#ifdef REGAMEDLL_FIXES
		// Make sure that the entity is a player,
		// because here the entity can come as CBreakable with event EVENT_BREAK_METAL
		if (pPlayer && !pPlayer->IsPlayer())
			return;
#endif

		break;
	}
	}

	// Process radio events from our team
	if (pPlayer && BotRelationship(pPlayer) == BOT_TEAMMATE && event > EVENT_START_RADIO_1 && event < EVENT_END_RADIO)
	{
		// TODO: Distinguish between radio commands and responses
		if (event != EVENT_RADIO_AFFIRMATIVE && event != EVENT_RADIO_NEGATIVE && event != EVENT_RADIO_REPORTING_IN)
		{
			m_lastRadioCommand = event;
			m_lastRadioRecievedTimestamp = gpGlobals->time;
			m_radioSubject = pPlayer;
			m_radioPosition = pPlayer->pev->origin;
		}
	}

	// player_follows needs a player
	if (!pPlayer)
		return;

	// Loner-Strike: a hostage calling out heard by a T: a CT's at the hostages
	if (event == EVENT_HOSTAGE_CALLED_FOR_HELP && m_iTeam == TERRORIST && LsFight() && pEntity
		&& (pEntity->pev->origin - pev->origin).IsLengthLessThan(1200.0f) && !IsAttacking())
		LsTold(pEntity->pev->origin);

	if (!LsShirksObjective() && event == EVENT_HOSTAGE_CALLED_FOR_HELP && m_iTeam == CT && IsHunting())
	{
		if ((pEntity->pev->origin - pev->origin).IsLengthGreaterThan(1000.0f))
			return;

		Vector vecHostageOrigin = pEntity->Center();
		if (IsVisible(&vecHostageOrigin))
		{
			m_task = COLLECT_HOSTAGES;
			m_taskEntity = nullptr;

			Run();
			m_goalEntity = pEntity;

			MoveTo(&pEntity->pev->origin, m_hostageEscortCount == 0 ? SAFEST_ROUTE : FASTEST_ROUTE);
			PrintIfWatched("I'm fetching a hostage that called out to me\n");

			return;
		}
	}

	// don't pay attention to noise that friends make
	// (Loner-Strike: except a teammate's fight close by, to trade)
	if (!IsEnemy(pPlayer))
	{
		LsTeamHeard(event, pPlayer);
		return;
	}

	float range;
	PriorityType priority;
	bool isHostile;

	if (IsGameEventAudible(event, pEntity, pOther, &range, &priority, &isHostile) == false)
		return;

	if (event == EVENT_HOSTAGE_USED)
	{
		if (m_iTeam == CT)
			return;

		if ((pEntity->pev->origin - pev->origin).IsLengthGreaterThan(range))
			return;

		GetChatter()->HostagesBeingTaken();

		// (Loner-Strike: the rescue zone they'll take it to - the nearest to the hostage - and the
		// better ones wait partway along the way back, not at a random zone)
		if (LsFight() && !GetGameState()->GetNearestVisibleFreeHostage() && m_task != GUARD_HOSTAGE_RESCUE_ZONE && LsCutOffRescue(pEntity->pev->origin))
		{
			m_task = GUARD_HOSTAGE_RESCUE_ZONE;
			m_taskEntity = nullptr;
			SetDisposition(OPPORTUNITY_FIRE);
		}
		else if (!GetGameState()->GetNearestVisibleFreeHostage() && m_task != GUARD_HOSTAGE_RESCUE_ZONE
			&& (!LsFight() || (pEntity->pev->origin - pev->origin).IsLengthGreaterThan(700.0f)) && GuardRandomZone())
		{
			m_task = GUARD_HOSTAGE_RESCUE_ZONE;
			m_taskEntity = nullptr;

			SetDisposition(OPPORTUNITY_FIRE);
			PrintIfWatched("Trying to beat them to an escape zone!\n");
		}
	}

	// Loner-Strike: a door swinging shut by itself isn't someone coming (and the one who opened
	// it is long gone from it)
	if (event == EVENT_DOOR && pOther && LsFight())
		return;

	// check if noise is close enough for us to hear
	const Vector *newNoisePosition = &pPlayer->pev->origin;
	float newNoiseDist = (pev->origin - *newNoisePosition).Length();
	if (newNoiseDist < range)
	{
		// we heard the sound
		LsHeard(event, pPlayer, newNoiseDist);	// Loner-Strike
		if ((IsLocalPlayerWatchingMe() && cv_bot_debug.value == 3.0f) || cv_bot_debug.value == 4.0f)
		{
			PrintIfWatched("Heard noise (%s from %s, pri %s, time %3.1f)\n",
				(event == EVENT_WEAPON_FIRED) ? "Weapon fire " : "",
				STRING(pPlayer->pev->netname),
				(priority == PRIORITY_HIGH) ? "HIGH" : ((priority == PRIORITY_MEDIUM) ? "MEDIUM" : "LOW"),
				gpGlobals->time);
		}

		if (event == EVENT_PLAYER_FOOTSTEP && IsUsingSniperRifle() && newNoiseDist < 300.0)
			EquipPistol();

		// should we pay attention to it
		// if noise timestamp is zero, there is no prior noise
		if (m_noiseTimestamp > 0.0f)
		{
			// only overwrite recent sound if we are louder (closer), or more important - if old noise was long ago, its faded
			const float shortTermMemoryTime = 3.0f;
			if (gpGlobals->time - m_noiseTimestamp < shortTermMemoryTime)
			{
				// prior noise is more important - ignore new one
				if (priority < m_noisePriority)
					return;

				float oldNoiseDist = (pev->origin - m_noisePosition).Length();
				if (newNoiseDist >= oldNoiseDist)
					return;
			}
		}

		// find the area in which the noise occured
		// TODO: Better handle when noise occurs off the nav mesh
		// TODO: Make sure noise area is not through a wall or ceiling from source of noise
		// TODO: Change GetNavTravelTime to better deal with NULL destination areas
		CNavArea *noiseArea = TheNavAreaGrid.GetNavArea(newNoisePosition);
		if (!noiseArea)
			noiseArea = TheNavAreaGrid.GetNearestNavArea(newNoisePosition);

		if (!noiseArea)
		{
			PrintIfWatched("  *** Noise occurred off the nav mesh - ignoring!\n");
			return;
		}

		m_noiseArea = noiseArea;

		// remember noise priority
		m_noisePriority = priority;

		// randomize noise position in the area a bit - hearing isn't very accurate
		// the closer the noise is, the more accurate our placement
		// TODO: Make sure not to pick a position on the opposite side of ourselves.
		const float maxErrorRadius = 400.0f;
		const float maxHearingRange = 2000.0f;
		float errorRadius = maxErrorRadius * newNoiseDist / maxHearingRange;

		m_noisePosition.x = newNoisePosition->x + RANDOM_FLOAT(-errorRadius, errorRadius);
		m_noisePosition.y = newNoisePosition->y + RANDOM_FLOAT(-errorRadius, errorRadius);

		// make sure noise position remains in the same area
		m_noiseArea->GetClosestPointOnArea(&m_noisePosition, &m_noisePosition);

		m_isNoiseTravelRangeChecked = false;

		// note when we heard the noise
		m_noiseTimestamp = gpGlobals->time;
	}
}
