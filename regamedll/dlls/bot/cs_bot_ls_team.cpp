/*
*
*   Loner-Strike additions to the CZ bots: team play and objectives: trades, spacing, retakes, post-plant, rotations, round plans, kits, pickups.
*   (cs_bot_ls.cpp has the cvars and the rest; the habits here follow the same rules: bot_ls_fight,
*   LsSolo / LsMatch for the mode, LsTrait for each bot's own leanings)
*
*/

#include "precompiled.h"

// Team play each frame (cheap: timers first)
// How far apart two places are for site purposes: height counts two and a half times (nuke's
// sites are one above the other: flat, they're the same place)
static float LsSiteDist(const Vector &a, const Vector &b)
{
	Vector d = a - b;
	d.z *= 2.5f;
	return float(d.Length());
}

static const CCSBotManager::Zone *LsNearZone(const Vector &at)
{
	const CCSBotManager::Zone *best = nullptr;
	float bestD = 1e9f;
	for (int z = 0; TheCSBots() && z < TheCSBots()->GetZoneCount(); z++)
	{
		const CCSBotManager::Zone *zone = TheCSBots()->GetZone(z);
		float d = LsSiteDist(zone->m_center, at);
		if (d < bestD)
		{
			bestD = d;
			best = zone;
		}
	}
	return best;
}

// A site being hit, as a CT side reads it off the radar and the kill feed: teammates dropping
// by one site within seconds. No commander - each CT elsewhere decides for itself whether to
// go (see LsTeamTick)
static int s_hitRound = -1, s_hitZone = -1, s_hitCount = 0, s_hitVictim = 0;
static float s_hitAt = -99.0f, s_hitStart = -99.0f, s_hitDeathAt = -1.0f;

void LsCtSiteDeath(CBasePlayer *pVictim)
{
	if (!pVictim || pVictim->m_iTeam != CT || !TheCSBots() || TheCSBots()->GetScenario() != CCSBotManager::SCENARIO_DEFUSE_BOMB
		|| TheCSBots()->IsBombPlanted() || TheCSBots()->GetZoneCount() < 2)
		return;
	float now = gpGlobals->time;
	// (every bot hears of the same death: counted once)
	if (now == s_hitDeathAt && pVictim->entindex() == s_hitVictim)
		return;
	s_hitDeathAt = now;
	s_hitVictim = pVictim->entindex();
	const CCSBotManager::Zone *zone = LsNearZone(pVictim->pev->origin);
	if (!zone || LsSiteDist(zone->m_center, pVictim->pev->origin) > 1100.0f)
		return;
	int z = zone->m_index;
	if (s_hitRound != LsRoundSerial() || s_hitZone != z || now - s_hitAt > 12.0f)
	{
		s_hitRound = LsRoundSerial();
		s_hitZone = z;
		s_hitCount = 0;
		s_hitStart = now;
	}
	s_hitCount++;
	s_hitAt = now;
}

// Out of bullets, how likely it still goes in with the knife: a knifer often, a rusher or a bold
// newer player now and then, nearly never a good careful one
static float LsEmptyDare(const CCSBot *me)
{
	int style = me->LsStyle();
	float base = style == LS_STYLE_KNIFER ? 0.5f : style == LS_STYLE_RUSHER ? 0.25f : 0.04f + 0.1f * me->LsAggro();
	return base * (1.2f - me->LsSkill());
}

void CCSBot::LsTeamTick()
{
	if (!LsFight() || !LsTeamGame() || !IsAlive())
		return;
	float now = gpGlobals->time;
	int round = LsRoundSerial();
	// (a call from last round is no reason to go anywhere now)
	if (m_lsTeamRound != round)
	{
		m_lsTeamRound = round;
		m_lsRotateAt = 0.0f;
		m_lsRotateZone = -1;
		m_lsForTimeRoll = -1;
	}

	// a site being hit (two teammates down by it within seconds, off the radar and the kill
	// feed): a CT elsewhere goes over to help, or not, by who it is - team players and the
	// careful mostly go, a camper anchoring its own site mostly stays, a newer player often
	// doesn't read it at all; in a match nearly all of them go. Decided once per hit
	if (m_iTeam == CT && s_hitRound == round && s_hitCount >= 2 && now - s_hitAt < 12.0f && m_lsRotateHitAt != s_hitStart
		&& !TheCSBots()->IsBombPlanted() && LsFree() && !IsAttacking() && !LsOrderActive() && !LsSolo())
	{
		m_lsRotateHitAt = s_hitStart;
		const CCSBotManager::Zone *zone = TheCSBots()->GetZone(s_hitZone);
		float d = zone ? LsSiteDist(zone->m_center, pev->origin) : 0.0f;
		if (zone && d > 600.0f && d < 5000.0f && LsNearZone(pev->origin) != zone)
		{
			int style = LsStyle();
			bool anchoring = style == LS_STYLE_CAMPER && GetTask() == GUARD_BOMB_ZONE;
			float lean = anchoring ? 0.3f : (style == LS_STYLE_LURKER || style == LS_STYLE_NINJA) ? 0.6f
				: (style == LS_STYLE_RUSHER || style == LS_STYLE_CAREFUL) ? 1.15f : 1.0f;
			float chance = (0.35f + 0.4f * LsTeamwork()) * lean * (0.5f + 0.5f * Q_min(1.0f, LsSkill() / 0.35f));
			// (a match's anchor holds its own site, as in LsRadioAnswer)
			if (LsMatch() && !anchoring)
				chance += 0.25f;
			if (RANDOM_FLOAT(0.0f, 1.0f) < chance)
			{
				const Vector *pos = TheCSBots()->GetRandomPositionInZone(zone);
				if (pos)
				{
					MoveTo(pos, FASTEST_ROUTE);
					// (and it stays on that site once there, not back to the one it left)
					m_lsRotateZone = s_hitZone;
					m_lsRotateZoneUntil = now + 45.0f;
					LsWhy("rotate: site hit");
				}
			}
		}
	}

	// hostage maps: a CT on its way in for the hostages, a short walk out and out of their
	// sight, with few teammates near, waits a few seconds for the others rather than walking in
	// alone (the Ts inside hold together; one at a time, the CTs were picked off - on assault
	// they lost four rounds in five). Once a round; rushers and knifers just go, the patient
	// wait longest
	if (m_iTeam == CT && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_RESCUE_HOSTAGES && m_lsHostStageRound != round
		&& GetTask() == COLLECT_HOSTAGES && GetHostageEscortCount() == 0 && IsMovingTo() && HasPath() && LsTimeLeft() > 50.0f && LsFree()
		&& !IsAttacking() && !IsEnemyVisible() && GetFriendsRemaining() >= 2 && !LsOrderActive() && now >= m_lsExecCheckAt)
	{
		m_lsExecCheckAt = now + 0.3f;
		float walk = GetPathDistanceRemaining();
		int style = LsStyle();
		if (style == LS_STYLE_RUSHER || style == LS_STYLE_KNIFER || LsLoner())
			m_lsHostStageRound = round;
		else if (walk > 500.0f && walk < 1100.0f)
		{
			TraceResult tr;
			UTIL_TraceLine(GetEyePosition(), GetPathEndpoint() + Vector(0, 0, 48.0f), ignore_monsters, ENT(pev), &tr);
			if (tr.flFraction < 1.0f)
			{
				m_lsHostStageRound = round;
				int nearMates = 0;
				LsForTeammates(this, [&](CBasePlayer *pPlayer) {
					if (pPlayer->IsAlive() && (pPlayer->pev->origin - pev->origin).IsLengthLessThan(700.0f))
						nearMates++;
				});
				// (standing where it is, still on its way for the hostages - a hide would end in
				// a fresh plan, and one in three went hunting instead; under the 5 s it takes to
				// give up on an area. Anything happening ends the wait: the hold gives way)
				if (nearMates < 2 && !LsPuppetActive())
				{
					float wait = Q_min(4.5f, (2.0f + 2.5f * (1.0f - LsAggro())) * (LsMatch() ? 1.3f : 1.0f));
					m_lsPuppet.any = true;
					m_lsPuppet.holdUntil = now + wait;
					Q_strlcpy(m_lsPuppet.holdTag, "ls");
					LsWhy("hostages: waiting for the team");
				}
			}
		}
	}

	// a CT side well ahead on numbers, half a minute in, no plant: the bold go and find them
	// (4 v 2: the clock's on the Ts, but so is the win)
	if (m_iTeam == CT && m_lsTempoPushRound != round && !TheCSBots()->IsBombPlanted() && LsTempo() >= 2.0f && LsFree() && !IsHiding()
		&& TheCSBots()->GetElapsedRoundTime() > 30.0f && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB)
	{
		m_lsTempoPushRound = round;
		if (LsSkill() > 0.3f && RANDOM_FLOAT(0.0f, 1.0f) < 0.3f + 0.5f * LsAggro())
		{
			Hunt();
			LsWhy("ahead on numbers: hunting");
		}
	}
	// a T starting to plant close by (the beeps of the keypad): heard - a CT goes for it
	if (m_iTeam == CT && now - m_lsPlantHeardAt > 4.0f && !TheCSBots()->IsBombPlanted() && !IsAttacking())
	{
		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *pT = UTIL_PlayerByIndex(i);
			if (!pT || !pT->IsAlive() || pT->m_iTeam != TERRORIST || !pT->m_pActiveItem || pT->m_pActiveItem->m_iId != WEAPON_C4)
				continue;
			if (!static_cast<CC4 *>(pT->m_pActiveItem)->m_bStartedArming || (pT->pev->origin - pev->origin).IsLengthGreaterThan(1300.0f))
				continue;
			m_lsPlantHeardAt = now;
			LsTold(pT->pev->origin);
			if (LsFree() && RANDOM_FLOAT(0.0f, 1.0f) < 0.4f + 0.5f * LsSkill())
			{
				Vector at = pT->pev->origin;
				Run();
				Hurry(4.0f);
				MoveTo(&at, FASTEST_ROUTE);
				LsWhy("heard the plant");
			}
			break;
		}
	}

	// a retake: the CTs don't all come in the same way - each, once a round, takes a way in from
	// a side the others aren't coming from (the defuser goes straight for it)
	if (m_iTeam == CT && m_lsRetakeWayRound != round && TheCSBots()->IsBombPlanted() && GetGameState()->GetBombPosition())
		LsRetakeApproach();

	// a CT AWPer early in the round takes a perch over the site it's going to hold (once a
	// round; a sniper who knows the map; not with an order on, nor in a fight)
	if (m_iTeam == CT && m_lsPerchRound != round && LsGunClass(this) == LSG_SNIPER && !TheCSBots()->IsBombPlanted() && LsFree()
		&& TheCSBots()->GetElapsedRoundTime() < 25.0f && LsSkill() > 0.3f && !CSGameRules()->IsFreezePeriod() && now >= m_lsPerchTryAt)
	{
		m_lsPerchTryAt = now + 2.0f;
		const CCSBotManager::Zone *site = nullptr;
		if (HasPath())
		{
			Vector end = GetPathEndpoint();
			site = TheCSBots()->GetZone(&end);
		}
		if (!site)
			site = TheCSBots()->GetClosestZone(&pev->origin);
		if (site && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB && LsTakePerch(site, false))
			LsWhy("awp perch");
		else
			m_lsPerchRound = round;
	}

	// the bomb down somewhere it hasn't seen: a teammate on that site, or one who just died
	// there, calls it ("bomb's at A"), as people do, so the retake goes to the right site
	// instead of guessing one
	if (m_iTeam == CT && TheCSBots()->IsBombPlanted() && !GetGameState()->IsPlantedBombLocationKnown() && now >= m_lsBombCallAt)
	{
		m_lsBombCallAt = now + 1.0f;
		CBaseEntity *pEnt = nullptr;
		while ((pEnt = UTIL_FindEntityByClassname(pEnt, "grenade")) != nullptr)
		{
			if (!static_cast<CGrenade *>(pEnt)->m_bIsC4)
				continue;
			Vector bomb = pEnt->pev->origin;
			bool called = false;
			for (int i = 1; i <= gpGlobals->maxClients && !called; i++)
			{
				CBasePlayer *pMate = UTIL_PlayerByIndex(i);
				if (!pMate || pMate == this || FNullEnt(pMate->pev) || pMate->m_iTeam != CT)
					continue;
				bool atBomb = (pMate->pev->origin - bomb).IsLengthLessThan(1200.0f);
				// (alive there, or a bot's body there from the last 20 s: its last words. A dead
				// person's position is their spectator camera, so not theirs)
				bool fresh = pMate->IsAlive() || (pMate->IsBot() && static_cast<CCSBot *>(pMate)->LsSinceDied() < 20.0f);
				called = atBomb && fresh;
			}
			if (called)
				GetGameState()->UpdatePlantedBomb(&bomb);
			break;
		}
	}

	// a T not hiding (on its way somewhere, hunting) near a defuse it can hear: the same as one
	// hiding would (LsDefuseHeard), once per defuse
	// (not one running from the blast, nor for a defuse that can't finish in time)
	CBasePlayer *pDefuser = TheCSBots()->GetBombDefuser();
	if (m_iTeam == TERRORIST && pDefuser && !IsHiding() && !IsAttacking() && !LsOrderActive() && !IsEscapingFromBomb()
		&& TheCSBots()->GetBombTimeLeft() > (pDefuser->m_bHasDefuser ? 5.0f : 10.0f) && now - m_lsDefuseActedAt > 12.0f
		&& now >= m_lsDefuseCheckAt)
	{
		m_lsDefuseCheckAt = now + 0.25f;
		// (quiet a while only after heading for it; one it saw and shot at, it keeps listening for)
		if (LsDefuseHeard(TheCSBots()->GetBombDefuser()) == 1 && IsMovingTo())
			m_lsDefuseActedAt = now;
	}

	// a dropped bomb, seen on the radar: the team's fetcher (LsBombFetcher) goes for it,
	// whatever it was doing
	if (m_iTeam == TERRORIST && now >= m_lsFetchCheckAt && TheCSBots()->GetLooseBomb() && !IsAttacking() && m_state != &m_fetchBombState
		&& GetTask() != PLANT_BOMB)
	{
		m_lsFetchCheckAt = now + 1.0f;
		if (LsFetchesBomb())
		{
			if (LsPuppetActive() && !LsOrderActive())
				LsPuppetRelease();
			FetchBomb();
			LsWhy("fetch-bomb");
		}
	}

	// the carrier sticks with a teammate for a while early on (people with the bomb go with
	// someone, not alone through mid), then goes to plant once at a site or when the time's up
	if (m_iTeam == TERRORIST && IsCarryingBomb() && !CSGameRules()->IsFreezePeriod() && !LsOrderActive())
	{
		if (m_lsCarrierFollowUntil > 0.0f)
		{
			CBasePlayer *pLeader = m_lsCarrierLeader;
			if (IsAtBombsite() || now > m_lsCarrierFollowUntil || !pLeader || !pLeader->IsAlive() || LsTimeLeft() < 35.0f || !IsFollowing())
			{
				m_lsCarrierFollowUntil = 0.0f;
				if (IsFollowing())
					StopFollowing();
				if (!IsAttacking())
					Idle();
			}
		}
		else if (m_lsCarrierFollowRound != round && !IsAttacking() && !IsAtBombsite() && LsStyle() != LS_STYLE_RUSHER && LsTimeLeft() > 45.0f
			&& TheCSBots()->GetElapsedRoundTime() > 2.0f && TheCSBots()->GetElapsedRoundTime() < 40.0f)
		{
			m_lsCarrierFollowRound = round;
			CBasePlayer *pBest = nullptr;
			float best = 1200.0f;
			LsForTeammates(this, [&](CBasePlayer *pPlayer) {
				if (pPlayer->IsBot() && (static_cast<CCSBot *>(pPlayer)->LsLoner() || static_cast<CCSBot *>(pPlayer)->IsCarryingBomb()))
					return;
				float d = float((pPlayer->pev->origin - pev->origin).Length());
				if (d < best)
				{
					best = d;
					pBest = pPlayer;
				}
			});
			if (pBest)
			{
				Follow(pBest);
				m_lsCarrierLeader = pBest;
				m_lsCarrierFollowUntil = now + RANDOM_FLOAT(10.0f, 20.0f);
				LsWhy("carrier-with-team");
			}
		}
	}

	// (nor goes to look into a noise with the bomb on its back: that's for the others)
	if (m_iTeam == TERRORIST && IsCarryingBomb() && m_state == &m_investigateNoiseState && !IsAttacking() && LsTimeLeft() > 20.0f)
	{
		Idle();
		LsWhy("carrier-no-noise");
	}

	// the carrier, on its way to plant: people don't go through the door first with the bomb,
	// nor chase someone they just saw; it lets a teammate go ahead (a few seconds at a time, a
	// few times a round, while there's time) and leaves the chasing to others
	if (m_iTeam == TERRORIST && IsCarryingBomb() && !IsAttacking() && !LsOrderActive() && !CSGameRules()->IsFreezePeriod()
		&& IsMovingTo() && now >= m_lsCarrierCheckAt)
	{
		m_lsCarrierCheckAt = now + 1.0f;
		if (m_lsCarrierRound != round)
		{
			m_lsCarrierRound = round;
			m_lsCarrierWaits = 0;
		}
		if (GetTask() == MOVE_TO_LAST_KNOWN_ENEMY_POSITION)
		{
			Idle();
			LsWhy("carrier-no-chase");
		}
		else if (GetTask() == PLANT_BOMB && HasPath() && !IsAtBombsite() && LsTimeLeft() > 40.0f && m_lsCarrierWaits < 2
			&& LsStyle() != LS_STYLE_RUSHER && TheCSBots()->GetElapsedRoundTime() < 75.0f)
		{
			Vector goal = GetPathEndpoint();
			float mine = float((goal - pev->origin).Length2D());
			int mates = 0;
			bool ahead = false;
			LsForTeammates(this, [&](CBasePlayer *pPlayer) {
				mates++;
				if (float((goal - pPlayer->pev->origin).Length2D()) < mine - 150.0f)
					ahead = true;
			});
			if (mine < 1500.0f && mine > 500.0f && mates > 0 && !ahead)
			{
				m_lsCarrierWaits++;
				Hide(GetLastKnownArea(), RANDOM_FLOAT(1.5f, 3.0f), 250.0f);
				LsWhy("carrier-waits");
			}
		}
	}

	// a T without the bomb, late with no plant: goes with the carrier to its site, as a team
	// does when the clock's running out (once a round; not mid-fight or under an order)
	if (m_iTeam == TERRORIST && !IsCarryingBomb() && m_lsEscortRound != round && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB
		&& !TheCSBots()->IsBombPlanted() && !CSGameRules()->IsFreezePeriod() && LsTimeLeft() < 25.0f && LsFree() && !m_lsSaving && now >= m_lsEscortCheckAt)
	{
		m_lsEscortCheckAt = now + 2.0f;
		CCSBot *pCarrier = nullptr;
		LsForTeammates(this, [&](CBasePlayer *pPlayer) {
			if (pPlayer->IsBot() && pPlayer->m_bHasC4)
				pCarrier = static_cast<CCSBot *>(pPlayer);
		});
		// (to its site: where it's walking if it's going to plant, else the site closest to it)
		if (pCarrier && (pCarrier->pev->origin - pev->origin).IsLengthLessThan(1500.0f))
		{
			const CCSBotManager::Zone *zone = TheCSBots()->GetClosestZone(&pCarrier->pev->origin);
			Vector to = (pCarrier->GetTask() == PLANT_BOMB && pCarrier->HasPath()) ? pCarrier->GetPathEndpoint() : zone ? zone->m_center : pCarrier->pev->origin;
			m_lsEscortRound = round;
			MoveTo(&to, FASTEST_ROUTE);
			Hurry(15.0f);
			LsWhy("escort-late-bomb");
		}
	}

	// the bomb still on its back with the clock nearly gone: to a site, now
	// (looked at every few seconds: after a fight, or from a hide or someone's "go" order, it
	// still has to get there)
	if (IsCarryingBomb() && now >= m_lsLateBombAt && !IsAttacking() && !m_lsSaving && !IsAtBombsite() && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB
		&& !CSGameRules()->IsFreezePeriod() && LsTimeLeft() < 25.0f)
	{
		m_lsLateBombAt = now + 5.0f;
		Hurry(25.0f);
		if (GetTask() != PLANT_BOMB && (IsHunting() || IsHiding() || IsMovingTo() || IsFollowing() || m_state == &m_idleState))
		{
			if (m_lsPuppet.any)
				LsPuppetRelease();
			Idle();
		}
		LsWhy("late-bomb");
	}

	// a CT with the clock on its side (no plant, little time left): holds where it is, as people
	// do, rather than hunting into the Ts; the bold few keep pushing. A plant still calls it off
	if (m_iTeam == CT && m_lsClockRound != round && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB
		&& !TheCSBots()->IsBombPlanted() && !CSGameRules()->IsFreezePeriod() && LsTimeLeft() < 14.0f + 16.0f * LsTrait(88) && !IsAttacking()
		&& !LsOrderActive() && GetTask() != GUARD_LOOSE_BOMB && GetTask() != GUARD_BOMB_ZONE && !GetGameState()->IsLooseBombLocationKnown()
		&& (IsHunting() || (IsMovingTo() && GetTask() != DEFUSE_BOMB)))
	{
		m_lsClockRound = round;
		if (RANDOM_FLOAT(0.0f, 1.0f) > 0.15f + 0.4f * LsAggro() - (LsMatch() ? 0.2f : 0.0f))
		{
			SetTask(HOLD_POSITION);
			Hide(GetLastKnownArea(), LsTimeLeft() + 5.0f, 500.0f, true);
			LsWhy("hold-the-clock");
		}
	}

	// hostage maps: a T that has reached its spot near the hostages holds it a while, rather
	// than circling them; a CT with the clock running out and no hostages yet hurries for them
	if (TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_RESCUE_HOSTAGES && !CSGameRules()->IsFreezePeriod() && !IsAttacking() && !LsOrderActive())
	{
		if (m_iTeam == TERRORIST && IsHunting() && m_lsHostHoldAt <= now && GetLastKnownArea() && LsFree())
		{
			bool byHostage = false;
			CBaseEntity *pHostage = nullptr;
			while (!byHostage && (pHostage = UTIL_FindEntityByClassname(pHostage, "hostage_entity")))
				byHostage = pHostage->IsAlive() && !(pHostage->pev->effects & EF_NODRAW) && (pHostage->pev->origin - pev->origin).IsLengthLessThan(600.0f);
			if (byHostage && RANDOM_FLOAT(0.0f, 1.0f) < 0.4f + 0.4f * (1.0f - LsAggro()))
			{
				float secs = RANDOM_FLOAT(8.0f, 20.0f);
				Hide(GetLastKnownArea(), secs, 400.0f);
				m_lsHostHoldAt = now + secs + RANDOM_FLOAT(5.0f, 15.0f);
				LsWhy("hold-hostages");
			}
			else
				m_lsHostHoldAt = now + RANDOM_FLOAT(4.0f, 8.0f);
		}
		else if (m_iTeam == CT && m_lsHostRushRound != round && LsTimeLeft() < 40.0f && GetHostageEscortCount() == 0 && CSGameRules()->m_iHostagesRescued == 0)
		{
			m_lsHostRushRound = round;
			Hurry(40.0f);
			LsWhy("hostages-late");
		}
	}

	// a teammate's call it looked into first: on its way now, unless something came up
	if (m_lsRotateAt > 0.0f && now >= m_lsRotateAt)
	{
		m_lsRotateAt = 0.0f;
		if (LsFree())
		{
			MoveTo(&m_lsRotateTo, FASTEST_ROUTE);
			LsWhy("rotate");
		}
	}

	// mid-round, out of a fight, with an SMG, a shotgun or nothing: a rifle or an AWP lying close
	// by is picked up (people swap for a dead player's AK). It walks over first and drops its
	// own only once there; one bot to a gun
	if (m_lsSwapBox.IsValid())
	{
		CBaseEntity *pBox = m_lsSwapBox;
		CBasePlayerItem *pMine = m_rgpPlayerItems[PRIMARY_WEAPON_SLOT];
		if (IsAttacking() || !IsMovingTo() || now > m_lsSwapUntil)
		{
			// (cut short by a fight: not a failed try - it may go back for it)
			if (IsAttacking())
				for (int r = 0; r < 4; r++)
					if (static_cast<CBaseEntity *>(m_lsSwapLast[r]) == pBox)
						m_lsSwapLastAt[r] = 0.0f;
			m_lsSwapBox = nullptr;
		}
		else if ((pBox->pev->origin - pev->origin).IsLengthLessThan(90.0f))
		{
			// (a pistol lying there: only one fits, so an empty one of its own goes first)
			const char *model = STRING(pBox->pev->model);
			bool pistol = model && (Q_strstr(model, "w_glock") || Q_strstr(model, "w_usp") || Q_strstr(model, "w_deagle") || Q_strstr(model, "w_p228")
				|| Q_strstr(model, "w_elite") || Q_strstr(model, "w_fiveseven"));
			CBasePlayerItem *pSide = m_rgpPlayerItems[PISTOL_SLOT];
			if (pistol && pSide && LsOutOfAmmo())
			{
				DropPlayerItem(STRING(pSide->pev->classname));
			}
			else if (pMine && !pistol)
			{
				DropPlayerItem(STRING(pMine->pev->classname));
				m_lsSwapDropAt = now;
				m_lsSwapDropPos = pev->origin;
			}
			m_lsSwapBox = nullptr;
		}
	}
	// (dropped its own, and someone took the new one first: its own back)
	else if (m_lsSwapDropAt > 0.0f && now - m_lsSwapDropAt > 1.0f)
	{
		m_lsSwapDropAt = 0.0f;
		if (!m_rgpPlayerItems[PRIMARY_WEAPON_SLOT] && !IsAttacking())
		{
			CBaseEntity *pOwn = nullptr;
			while ((pOwn = UTIL_FindEntityByClassname(pOwn, "weaponbox")))
			{
				if ((pOwn->pev->origin - m_lsSwapDropPos).IsLengthLessThan(150.0f))
				{
					Vector to = pOwn->pev->origin;
					MoveTo(&to, FASTEST_ROUTE);
					LsWhy("swap-back");
					break;
				}
			}
		}
	}
	else if (!TheCSBots()->IsRoundOver() && now >= m_lsSwapCheckAt && !IsAttacking() && !LsOrderActive() && !IsCarryingBomb() && LsFree()
		&& !CSGameRules()->IsFreezePeriod() && !LsWeaponModeOn() && (!TheCSBots()->IsBombPlanted() || LsOutOfAmmo()) && GetHostageEscortCount() == 0
		&& GetTask() != DEFUSE_BOMB && GetTask() != COLLECT_HOSTAGES && GetTask() != RESCUE_HOSTAGES && (GetTimeSinceLastSawEnemy() > 8.0f || LsOutOfAmmo()))
	{
		m_lsSwapCheckAt = now + (LsOutOfAmmo() ? 0.5f : 2.0f);
		bool empty = LsOutOfAmmo();
		CBasePlayerItem *pPrimary = m_rgpPlayerItems[PRIMARY_WEAPON_SLOT];
		// (a newer player often doesn't think of it: as if it had no eye for the better gun)
		bool lsNotices = RANDOM_FLOAT(0.0f, 1.0f) < 0.35f + 0.65f * LsSkill();
		bool poor = empty || lsNotices && (!pPrimary || pPrimary->m_iId == WEAPON_MP5N || pPrimary->m_iId == WEAPON_TMP || pPrimary->m_iId == WEAPON_MAC10 || pPrimary->m_iId == WEAPON_UMP45
			|| pPrimary->m_iId == WEAPON_P90 || pPrimary->m_iId == WEAPON_M3 || pPrimary->m_iId == WEAPON_XM1014);
		if (poor)
		{
			static const char *const better[] = { "models/w_ak47.mdl", "models/w_m4a1.mdl", "models/w_awp.mdl", "models/w_sg552.mdl", "models/w_aug.mdl",
				"models/w_galil.mdl", "models/w_famas.mdl" };
			CBaseEntity *pBox = nullptr, *pBest = nullptr;
			float bestCost = 1e9f;
			while ((pBox = UTIL_FindEntityByClassname(pBox, "weaponbox")))
			{
				// (out of bullets: any gun at all, and from further off - with enemies about only one
				// right there, a dive for it)
				float reach = !empty ? 450.0f : GetTimeSinceLastSawEnemy() > 1.5f ? 900.0f : 350.0f;
				if ((pBox->pev->origin - pev->origin).IsLengthGreaterThan(reach))
					continue;
				bool good = empty;
				for (const char *g : better)
					good = good || FStrEq(STRING(pBox->pev->model), g);
				if (!good || !FVisible(pBox->pev->origin + Vector(0, 0, 8)))
					continue;
				// (not out in the spot a teammate just died in, for a better gun - with no bullets
				// left, yes)
				// (a careful or good player leaves it; the greedy and the newer ones sometimes go
				// for the AK anyway)
				if (!empty && LsRecentDeathNear(m_iTeam, pBox->pev->origin, 400.0f, 15.0f)
					&& RANDOM_FLOAT(0.0f, 1.0f) < Q_min(0.95f, (0.3f + 0.65f * LsSkill()) * (1.0f - 0.4f * LsAggro())))
					continue;
				// (and nobody walks out to a gun that dropped seconds ago: whoever got that kill is
				// still looking at it - only with no bullets left)
				if (!empty && LsRecentDeathNear(m_iTeam, pBox->pev->origin, 400.0f, 6.0f))
					continue;
				// (one a living player dropped, a gift or a swap of their own, isn't fair game)
				CBaseEntity *pOwner = pBox->pev->owner ? CBaseEntity::Instance(pBox->pev->owner) : nullptr;
				if (pOwner && pOwner->IsPlayer() && pOwner->IsAlive())
					continue;
				// (another bot already on its way to it)
				bool taken = false;
				LsForTeammates(this, [&](CBasePlayer *pPlayer) {
					if (pPlayer->IsBot() && static_cast<CBaseEntity *>(static_cast<CCSBot *>(pPlayer)->m_lsSwapBox) == pBox)
						taken = true;
				});
				if (taken)
					continue;
				// (one try at each: it went for this one and didn't end up with it - can't take it,
				// or something else pulled it away - it doesn't keep going back and forth)
				bool tried = false;
				for (int r = 0; r < 4; r++)
					if (static_cast<CBaseEntity *>(m_lsSwapLast[r]) == pBox && now - m_lsSwapLastAt[r] < 20.0f)
						tried = true;
				if (tried)
					continue;
				// (the nearest one that qualifies, not the first the game lists - and with an enemy
				// about, not one out toward them)
				float cost = float((pBox->pev->origin - pev->origin).Length());
				if (GetTimeSinceLastSawEnemy() < 3.0f && (pBox->pev->origin - GetLastKnownEnemyPosition()).Length() < (pev->origin - GetLastKnownEnemyPosition()).Length())
					cost += 500.0f;
				if (cost < bestCost)
				{
					bestCost = cost;
					pBest = pBox;
				}
			}
			if (pBest)
			{
				Vector to = pBest->pev->origin;
				MoveTo(&to, FASTEST_ROUTE);
				m_lsSwapLast[m_lsSwapLastSlot] = pBest;
				m_lsSwapLastAt[m_lsSwapLastSlot] = now;
				m_lsSwapLastSlot = (m_lsSwapLastSlot + 1) % 4;
				if (HasPath())
				{
					m_lsSwapBox = pBest;
					m_lsSwapUntil = now + 6.0f;
					LsWhy("swap-gun");
				}
			}
		}
	}

	// out of bullets with no gun lying in sight: most keep a step behind the nearest teammate (a
	// trade, and a gun from the next one to fall) rather than hunt with the knife; a knifer, and
	// some rushers and bold newer players, keep on with the knife (decided once a round)
	// (not while there's an enemy about: it stays in the cover it took until they're out of sight
	// a while - back and forth between the cover and a teammate in a fight otherwise)
	if (LsFight() && LsTeamGame() && LsOutOfAmmo() && !LsWeaponModeOn() && !TheCSBots()->IsRoundOver() && !IsAttacking() && LsFree()
		&& !LsOrderActive() && !IsCarryingBomb() && !m_lsSwapBox.IsValid() && m_state == &m_huntState && now >= m_lsEmptyTagAt
		&& GetTimeSinceLastSawEnemy() > 3.0f)
	{
		m_lsEmptyTagAt = now + RANDOM_FLOAT(0.6f, 1.5f);
		if (m_lsEmptyRound != round)
		{
			m_lsEmptyRound = round;
			m_lsEmptyBold = RANDOM_FLOAT(0.0f, 1.0f) < Q_min(1.0f, 2.0f * LsEmptyDare(this));
		}
		CBasePlayer *pMate = nullptr;
		float best = 2500.0f;
		LsForTeammates(this, [&](CBasePlayer *pPlayer) {
			float d = float((pPlayer->pev->origin - pev->origin).Length());
			if (d < best)
			{
				best = d;
				pMate = pPlayer;
			}
		});
		// (already just behind them: it waits there, not back and forth between them and the hunt)
		if (!m_lsEmptyBold && pMate && best < 300.0f)
		{
			if (!LsPuppetActive())
			{
				m_lsPuppet.any = true;
				m_lsPuppet.holdUntil = now + RANDOM_FLOAT(1.2f, 2.0f);
				Q_strlcpy(m_lsPuppet.holdTag, "ls");
				LsWhy("out of ammo: with a teammate");
			}
		}
		else if (!m_lsEmptyBold && pMate)
		{
			Vector back = pev->origin - pMate->pev->origin;
			back.z = 0.0f;
			float len = float(back.Length());
			Vector spot = pMate->pev->origin + (len > 1.0f ? back * (RANDOM_FLOAT(140.0f, 220.0f) / len) : Vector(0, 0, 0));
			MoveTo(&spot, SAFEST_ROUTE);
			LsWhy("out of ammo: with a teammate");
		}
	}

	// the round's over: a bot without a gun picks one up from the floor close by
	if (TheCSBots()->IsRoundOver() && m_lsPickupRound != round && !m_bHasPrimary && !IsAttacking() && !LsWeaponModeOn())
	{
		m_lsPickupRound = round;
		CBaseEntity *pBox = nullptr, *pBest = nullptr;
		float best = 700.0f;
		static const char *const good[] = { "models/w_ak47.mdl", "models/w_m4a1.mdl", "models/w_awp.mdl", "models/w_sg552.mdl", "models/w_aug.mdl",
			"models/w_galil.mdl", "models/w_famas.mdl", "models/w_m249.mdl", "models/w_g3sg1.mdl", "models/w_sg550.mdl" };
		while ((pBox = UTIL_FindEntityByClassname(pBox, "weaponbox")))
		{
			const char *model = STRING(pBox->pev->model);
			if (FStrEq(model, "models/w_backpack.mdl"))
				continue;
			float d = float((pBox->pev->origin - pev->origin).Length());
			// (a rifle counts as twice as close as a pistol)
			for (const char *g : good)
			{
				if (FStrEq(model, g))
					d *= 0.5f;
			}
			if (d < best)
			{
				best = d;
				pBest = pBox;
			}
		}
		if (pBest && RANDOM_FLOAT(0.0f, 1.0f) < 0.7f)
		{
			MoveTo(&pBest->pev->origin, FASTEST_ROUTE);
			LsWhy("pickup");
		}
		return;
	}

	// a retake: a CT on its own holds back a few seconds before going in (once a round, when
	// there's time), so teammates can catch up rather than everyone going in one by one
	if (m_iTeam == CT && TheCSBots()->IsBombPlanted() && m_lsRetakeRound != round && !IsAttacking() && !IsDefusingBomb()
		&& GetFriendsRemaining() > 0 && GetNearbyFriendCount() == 0 && TheCSBots()->GetBombTimeLeft() > 22.0f)
	{
		const Vector *bomb = GetGameState()->GetBombPosition();
		if (bomb)
		{
			float d = float((*bomb - pev->origin).Length());
			if (d > 800.0f && d < 2200.0f)
			{
				m_lsRetakeRound = round;
				if (RANDOM_FLOAT(0.0f, 1.0f) < (LsMatch() ? 0.85f : 0.5f) * (0.5f + 0.5f * LsTeamwork()))
				{
					Hide(GetLastKnownArea(), RANDOM_FLOAT(3.0f, 6.0f), 200.0f);
					LsWhy("retake-wait");
				}
			}
		}
	}
}

// A teammate's radio call (bot_ls_fight, team modes): people don't all drop everything for
// it. "Follow me" and the like from another bot: now and then (a person asking: as before).
// "Need backup" / "enemy spotted": the newer players run there blindly; the rest take a moment
// to look and only go if they're not busy themselves (the fight they're in comes first)
bool CCSBot::LsRadioAnswer()
{
	if (!LsFight() || !LsTeamGame() || !m_radioSubject)
		return false;
	switch (m_lastRadioCommand)
	{
	case EVENT_RADIO_FOLLOW_ME:
	case EVENT_RADIO_COVER_ME:
	case EVENT_RADIO_STICK_TOGETHER_TEAM:
	case EVENT_RADIO_REGROUP_TEAM:
		// (a person's order stands: another bot's "follow me" doesn't pull it off)
		if (LsOrderActive() || LsPuppetActive() || m_lsPlantOrderRound == LsRoundSerial())
			return true;
		if (!m_radioSubject->IsBot() || LsMatch())
			return false;
		return RANDOM_FLOAT(0.0f, 1.0f) > 0.25f;
	case EVENT_RADIO_ENEMY_SPOTTED:
	case EVENT_RADIO_NEED_BACKUP:
	case EVENT_RADIO_TAKING_FIRE:
	{
		if (!LsFree())
			return true;
		if (LsLoner() && RANDOM_FLOAT(0.0f, 1.0f) < 0.6f)
			return true;
		// (a match's anchor holds its own site: a call from the other one is someone else's)
		if (LsMatch() && GetTask() == GUARD_BOMB_ZONE && (LsStyle() == LS_STYLE_CAMPER || LsStyle() == LS_STYLE_CAREFUL)
			&& TheCSBots()->GetClosestZone(&pev->origin) != TheCSBots()->GetClosestZone(&m_radioSubject->pev->origin))
			return true;
		float skill = LsSkill();
		if ((m_radioPosition - pev->origin).IsLengthGreaterThan(2500.0f))
			return true;
		int closer = 0;
		float mine = float((m_radioPosition - pev->origin).Length());
		LsForTeammates(this, [&](CBasePlayer *pPlayer) {
			if (pPlayer->IsAlive() && (m_radioPosition - pPlayer->pev->origin).IsLengthLessThan(mine))
				closer++;
		});
		if (closer >= 2)
			return true;
		if (m_lastRadioCommand == EVENT_RADIO_ENEMY_SPOTTED)
		{
			Vector look = m_radioPosition + Vector(0, 0, HalfHumanHeight);
			SetLookAt("Loner-Strike: a call", &look, PRIORITY_MEDIUM, 1.0f);
			return true;
		}
		m_lsRotateTo = m_radioPosition;
		if (skill < 0.4f && !LsMatch())
		{
			// the blind rotate
			MoveTo(&m_lsRotateTo, FASTEST_ROUTE);
			LsWhy("rotate-blind");
			GetChatter()->Say("OnMyWay");
			return true;
		}
		Vector look = m_radioPosition + Vector(0, 0, HalfHumanHeight);
		SetLookAt("Loner-Strike: a call", &look, PRIORITY_MEDIUM, 1.0f);
		m_lsRotateAt = gpGlobals->time + RANDOM_FLOAT(1.0f, 2.5f);
		return true;
	}
	default:
		return false;
	}
}

// A teammate's gunfire close by (team modes): a team player not busy with its own fight goes
// to back them up, to trade if they go down. Loners mostly don't; once every few seconds
// A CT once the bomb is down, not defusing yet, who can still get there and defuse in time
// (running, with the kit or without): people on a retake against the clock run and skip the
// careful habits (corner jiggles, off-angles, going over to trade); one who can't make it
// isn't hurried (saving, or making the most of it, is up to the rest of the brain).
bool CCSBot::LsRetakeRush()
{
	if (!LsFight() || m_iTeam != CT || !TheCSBots()->IsBombPlanted() || IsDefusingBomb() || !IsAlive() || LsOrderActive())
		return false;
	float left = TheCSBots()->GetBombTimeLeft();
	const Vector *bomb = GetGameState()->GetBombPosition();
	// (where it is not known yet: the sites are checked in a hurry too)
	float dist = bomb ? float((*bomb - pev->origin).Length2D()) : 1500.0f;
	float defuse = m_bHasDefuser ? 5.0f : 10.0f;
	return dist / 230.0f + defuse + 1.0f < left;
}

// ---- the Ts' site execute ------------------------------------------------------------
//
// A pub team that wins a site gets there together: they gather a short way out (each on its
// own way in: long and catwalk, say), wait for the others, and go in at once, so the CTs
// holding it face several at a time instead of one after another. Ts 400-900 units' walk from
// the round's site, where the site can't see them, stop and wait; three waiting (fewer if fewer
// are left), the first waiting five to eight seconds, or the clock getting short, and they all go. Lurkers on the
// other site, and a team down to one or two, don't wait.

static int s_execRound = -1;
static const CCSBotManager::Zone *s_execSite = nullptr;
static float s_execFirstAt = 0.0f;
static float s_execGoAt = 0.0f;

/// The round's push: (re)set for a new round, or when the team switches site
// The push's parts this round (who's taken which) and where each is going in the site, so the
// next one picks what's missing and somewhere else: no commander, each reads the others
static int s_roleCount[5];
static Vector s_claimed[16];
static int s_claimedCount = 0;

static void LsExecuteRound(const CCSBotManager::Zone *site)
{
	if (s_execRound != LsRoundSerial() || s_execSite != site)
	{
		s_execRound = LsRoundSerial();
		s_execSite = site;
		s_execFirstAt = 0.0f;
		s_execGoAt = 0.0f;
		for (int i = 0; i < 5; i++)
			s_roleCount[i] = 0;
		s_claimedCount = 0;
	}
}

// A spot in the site away from where the others are going (spread, not stacked); a flanker's
// is across the site from where it is (it comes in from the other side)
static const Vector *LsSpreadSpot(const CCSBotManager::Zone *site, const Vector &from, bool farSide)
{
	const Vector *best = nullptr;
	float bestScore = -1e9f;
	for (int t = 0; t < 10; t++)
	{
		const Vector *pos = TheCSBots()->GetRandomPositionInZone(site);
		if (!pos)
			continue;
		float apart = 1000.0f;
		for (int i = 0; i < s_claimedCount; i++)
			apart = Q_min(apart, float((*pos - s_claimed[i]).Length2D()));
		float score = Q_min(apart, 450.0f) + (farSide ? 0.6f : -0.3f) * float((*pos - from).Length2D()) + RANDOM_FLOAT(0.0f, 80.0f);
		if (score > bestScore)
		{
			bestScore = score;
			best = pos;
		}
	}
	if (best && s_claimedCount < 16)
		s_claimed[s_claimedCount++] = *best;
	return best;
}

// Its part in the push, from its gun, its style and what's still missing: an AWPer takes a
// perch over the site (one), a lurker or ninja comes in another way (one, with three or more
// left), the bold ones go first (one or two), the rest trade a step behind
int CCSBot::LsPickRole()
{
	int style = LsStyle();
	int alive = LsAlive(TERRORIST);
	int role;
	if (IsCarryingBomb())
		role = s_roleCount[LSR_ENTRY] == 0 && alive <= 2 ? LSR_ENTRY : LSR_TRADE;	// (the bomb follows the entry in)
	else if (LsGunClass(this) == LSG_SNIPER && s_roleCount[LSR_SNIPER] == 0 && alive >= 3)
		role = LSR_SNIPER;
	else if ((style == LS_STYLE_LURKER || style == LS_STYLE_NINJA) && s_roleCount[LSR_FLANK] == 0 && alive >= 3)
		role = LSR_FLANK;
	else
	{
		// (the entry goes to whoever suits it best of those still to pick, not the first to
		// think: a careful one doesn't go first just because it got there first. Only ones in
		// the push and close enough to go in count)
		int round = LsRoundSerial();
		float mine = LsEntryScore();
		int better = 0;
		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *pT = UTIL_PlayerByIndex(i);
			if (!pT || pT == this || FNullEnt(pT->pev) || !pT->IsAlive() || !pT->IsBot() || pT->m_iTeam != TERRORIST)
				continue;
			CCSBot *pB = static_cast<CCSBot *>(pT);
			// (nor ones whose part is already plain: a flanker-to-be, the sniper, the bomb, one in a fight)
			int st = pB->LsStyle();
			if (pB->IsCarryingBomb() || pB->IsAttacking() || LsGunClass(pB) == LSG_SNIPER || pB->m_state == &pB->m_fetchBombState
				|| pB->m_lsPlantOrderRound == round
				|| ((st == LS_STYLE_LURKER || st == LS_STYLE_NINJA) && s_roleCount[LSR_FLANK] == 0 && alive >= 3))
				continue;
			if (pB->m_lsRoleRound == round || pB->LsOrderActive() || pB->LsLurksOtherSite() || !s_execSite
				|| (s_execSite->m_center - pB->pev->origin).Make2D().IsLengthGreaterThan(1700.0f))
				continue;
			if (pB->LsEntryScore() > mine)
				better++;
		}
		bool bold = style == LS_STYLE_RUSHER || LsAggro() > 0.65f;
		if (better == 0 && (s_roleCount[LSR_ENTRY] == 0 || (bold && s_roleCount[LSR_ENTRY] < 2)))
			role = LSR_ENTRY;
		else
			role = LSR_TRADE;
	}
	s_roleCount[role]++;
	return role;
}

void CCSBot::LsExecuteGo(const CCSBotManager::Zone *site)
{
	int round = LsRoundSerial();
	float now = gpGlobals->time;
	if (m_lsRoleRound != round)
	{
		m_lsRoleRound = round;
		m_lsRole = LsPickRole();
		m_lsPushArrived = false;
		m_lsPushGoAt = now;
		// (a trader a beat behind the entry: close enough to trade, not on its heels)
		m_lsTradeGoAt = m_lsRole == LSR_TRADE ? now + RANDOM_FLOAT(0.6f, 1.3f) : 0.0f;
		if (m_lsRole == LSR_SNIPER && LsTakePerch(site, true))
		{
			m_lsPushRound = round;
			LsWhy("execute: awp perch");
			return;
		}
		if (m_lsRole == LSR_SNIPER)
			m_lsRole = LSR_TRADE;
	}
	if (now < m_lsTradeGoAt)
		return;
	const Vector *pos = LsSpreadSpot(site, pev->origin, m_lsRole == LSR_FLANK);
	if (!pos)
		return;
	m_lsPushRound = round;
	m_lsPushDest = *pos;
	m_lsPushGoAt = now;
	if (IsCarryingBomb())
		SetTask(PLANT_BOMB);
	// (the flanker takes the quieter way round; the rest the quick one)
	MoveTo(pos, m_lsRole == LSR_FLANK ? SAFEST_ROUTE : FASTEST_ROUTE);
	if (m_lsRole == LSR_ENTRY)
		Run();
	LsWhy(m_lsRole == LSR_ENTRY ? "execute: entry" : m_lsRole == LSR_FLANK ? "execute: flank" : "execute: trade");
}

// A perch over a site for an AWPer, from the map knowledge: somewhere outside it with a good
// long view into it (500-1800 units off), high ground better, not seen from everywhere, on
// its own side (a T's on the way in, a CT's toward its spawn); held a while
bool CCSBot::LsTakePerch(const CCSBotManager::Zone *site, bool tSide)
{
	if (!LsSightReady() || !site)
		return false;
	Vector mid = site->m_center;
	CNavArea *midArea = TheNavAreaGrid.GetNearestNavArea(&mid);
	const Vector *own = LsEnemySpawn(m_iTeam == CT ? TERRORIST : CT);	// (its own spawn)
	if (!midArea || !own)
		return false;
	float siteToOwn = float((mid - *own).Length2D());
	CNavArea *best = nullptr;
	float bestScore = -1e9f;
	for (unsigned short i = 0; i < 65000; i++)
	{
		CNavArea *a = LsKnowArea(i);
		if (!a)
			break;
		const Vector &c = *a->GetCenter();
		float d = float((c - mid).Length2D());
		if (d < 500.0f || d > 1800.0f || TheCSBots()->GetZone(&c) == site)
			continue;
		// (its side of the site)
		if (float((c - *own).Length2D()) > siteToOwn)
			continue;
		int q = LsSightQuality(a, midArea);
		if (q < 40)
			continue;
		const LsAreaKnow *k = LsKnow(a);
		// (no crawlspace, nowhere nobody could stand, no spot others are holding)
		if ((k && k->ceiling * 4 < 70) || !LsHeadroom(a))
			continue;
		if (LsSpotCrowded(this, &c))
			continue;
		float score = float(q) + (k ? 0.4f * float(k->high) - 300.0f * float(k->sees) / 2000.0f : 0.0f) + RANDOM_FLOAT(0.0f, 40.0f);
		if (score > bestScore)
		{
			bestScore = score;
			best = a;
		}
	}
	if (!best)
		return false;
	m_lsPerchRound = LsRoundSerial();
	Vector at = *best->GetCenter();
	SetTask(MOVE_TO_SNIPER_SPOT);
	Hide(&at, RANDOM_FLOAT(18.0f, 30.0f), true);
	return true;
}

bool CCSBot::LsLurksOtherSite()
{
	// (said yes to a person's call: with the team; heard it and passed: its own way, the other site)
	int round = LsRoundSerial();
	if (m_lsCallRound == round || IsCarryingBomb())
		return false;
	if (m_lsCallSkipRound == round)
		return true;
	return LsStyle() == LS_STYLE_LURKER ? LsTrait(89) < 0.6f : LsTrait(89) < 0.12f;
}

// How fit it is to go in first: the bold and the rushers, a gun for close work, health; never
// the bomb (it follows the entry in) nor a sniper (that's the perch)
float CCSBot::LsEntryScore()
{
	int style = LsStyle();
	float score = LsAggro() + 0.4f * LsSkill() + 0.3f * float(pev->health) / 100.0f;
	if (style == LS_STYLE_RUSHER || style == LS_STYLE_KNIFER)
		score += 1.0f;
	else if (style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER || style == LS_STYLE_AWPER)
		score -= 0.5f;
	int gun = LsGunClass(this);
	if (gun == LSG_RIFLE || gun == LSG_SMG || gun == LSG_SHOTGUN || gun == LSG_MG)
		score += 0.3f;
	else if (gun == LSG_SNIPER)
		score -= 1.5f;
	if (IsCarryingBomb())
		score -= 2.0f;
	return score;
}

bool CCSBot::LsExecuting()
{
	// (over for it once it's at its spot in the site, or after 25 s: from there it plays the site)
	return m_iTeam == TERRORIST && m_lsPushRound == LsRoundSerial() && m_lsExecSite == LsTSite() && !TheCSBots()->IsBombPlanted() && IsAlive()
		&& !LsOrderActive() && !m_lsPushArrived && gpGlobals->time - m_lsPushGoAt < 25.0f && gpGlobals->time >= m_lsPushGoAt;
}

void CCSBot::LsExecuteTick()
{
	if (!LsFight() || !LsTeamGame() || m_iTeam != TERRORIST || !IsAlive() || TheCSBots()->GetScenario() != CCSBotManager::SCENARIO_DEFUSE_BOMB
		|| TheCSBots()->IsBombPlanted() || CSGameRules()->IsFreezePeriod() || LsOrderActive() || m_lsPlantOrderRound == LsRoundSerial())
		return;
	// (one on its way to a dropped bomb gets it first: the go, or a wait at the stage, pulled
	// it off the bomb, the fetch check sent it back, and it went to and fro till the time ran
	// out; with the bomb in hand it joins the push)
	if (m_state == &m_fetchBombState)
		return;
	int round = LsRoundSerial();
	float now = gpGlobals->time;
	const CCSBotManager::Zone *site = LsTSite();
	if (!site || LsLurksOtherSite() || (GetFriendsRemaining() == 0 && m_lsCallRound != round))
		return;
	LsExecuteRound(site);
	// (its own marks count for this round's site only: a switch starts over)
	if (m_lsExecSite != site)
	{
		m_lsExecSite = site;
		m_lsStagedRound = -1;
		m_lsPushRound = -1;
	}
	if (m_lsPushRound == round)
	{
		// at its spot: it holds there, watching the site, while the bomb comes in (the carrier
		// goes on to plant)
		if (!m_lsPushArrived && m_lsRole != LSR_SNIPER && !IsAttacking() && !IsCarryingBomb()
			&& (m_lsPushDest - pev->origin).Make2D().IsLengthLessThan(150.0f))
		{
			m_lsPushArrived = true;
			// (a person's call done: gm_botchat's "still on it" mark goes)
			if (m_lsCallRound == round)
				pev->vuser1.x = 0.0f;
			Hide(GetLastKnownArea(), RANDOM_FLOAT(6.0f, 14.0f), 300.0f);
			LsWhy("execute: holding the site");
		}
		return;
	}

	// gone already: one who comes up after it goes straight in
	float dist = float((site->m_center - pev->origin).Length2D());
	// (close by the way it's walking, not as the crow flies: a site across a wall or water
	// is far; and out of the site's sight, so it waits where it can't be seen. A straight
	// 1,100 units stopped Ts in the open on inferno and aztec)
	bool closeBy = false;
	if (HasPath() && (GetPathEndpoint() - site->m_center).IsLengthLessThan(900.0f))
	{
		float walk = GetPathDistanceRemaining();
		if (walk > 400.0f && walk < 900.0f)
		{
			TraceResult tr;
			UTIL_TraceLine(GetEyePosition(), site->m_center + Vector(0, 0, 48.0f), ignore_monsters, ENT(pev), &tr);
			closeBy = tr.flFraction < 1.0f;
		}
	}
	// a person's call: a rush goes now; and one who said yes heads for the site from wherever it
	// is (running for a rush), the push picking it up once it's close
	bool callRush = false;
	// (and on a rush the bomb carrier goes with them: it only heard where the team's going)
	bool called = LsTeamCalled(&callRush) && (m_lsCallRound == round || (callRush && IsCarryingBomb()));
	if (LsTeamCalled(&callRush) && callRush && s_execGoAt <= 0.0f)
		s_execGoAt = now;
	// (not over a cover run, an avenge hold, a trade or a noise it's looking into: those first)
	if (called && dist >= 1700.0f && !IsAttacking() && now >= m_lsExecCheckAt && (LsFree() || IsCarryingBomb()) && !IsHiding()
		&& m_ignoreEnemiesTimer.IsElapsed() && now - m_lsCoverPickAt > 3.0f && m_state != &m_investigateNoiseState
		&& (!HasPath() || (GetPathEndpoint() - site->m_center).IsLengthGreaterThan(900.0f)))
	{
		m_lsExecCheckAt = now + 0.5f;
		const Vector *in = TheCSBots()->GetRandomPositionInZone(site);
		Vector to = in ? *in : site->m_center;
		MoveTo(&to, FASTEST_ROUTE);
		if (callRush)
			Run();
		LsWhy(callRush ? "call: rushing in" : "call: heading for the site");
		return;
	}
	if (s_execGoAt > 0.0f)
	{
		// (in a fight at the go: it goes once the fight is over)
		if (dist < 1700.0f && !IsAttacking())
			LsExecuteGo(site);
		return;
	}

	// the team left and the clock: whether to wait at all (looked at a few times a second)
	if (now < m_lsExecCheckAt)
		return;
	m_lsExecCheckAt = now + 0.3f;
	int alive = 0, staged = 0;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pT = UTIL_PlayerByIndex(i);
		if (!pT || FNullEnt(pT->pev) || !pT->IsAlive() || pT->m_iTeam != TERRORIST)
			continue;
		alive++;
		if (pT->IsBot() && static_cast<CCSBot *>(pT)->m_lsStagedRound == round && static_cast<CCSBot *>(pT)->m_lsExecSite == site)
			staged++;
	}
	float left = LsTimeLeft();
	bool go = alive <= 2 || left < 45.0f || staged >= Q_min(3, alive)
		|| (s_execFirstAt > 0.0f && now - s_execFirstAt > LsExecPatience());
	if (go && (staged > 0 || alive <= 2 || left < 45.0f))
	{
		s_execGoAt = now;
		return;
	}

	// near the site, not in a fight: wait out of sight for the others
	if (closeBy && m_lsStagedRound != round && !IsAttacking() && !IsEnemyVisible() && GetTimeSinceAttacked() > 3.0f)
	{
		m_lsStagedRound = round;
		if (s_execFirstAt <= 0.0f)
			s_execFirstAt = now;
		// (where it stands, out of the way of the path: a few seconds at most)
		Hide(GetLastKnownArea(), 10.0f, 150.0f);
		LsWhy("stage");
	}
}

// How long it waits at the stage for the others before it calls the go: 5-8 s, a rusher
// (or an aggressive one) a lot less, a careful one a bit more
float CCSBot::LsExecPatience()
{
	float wait = 5.0f + 3.0f * LsTrait(90) - 2.5f * LsAggro();
	int style = LsStyle();
	if (style == LS_STYLE_RUSHER || style == LS_STYLE_KNIFER)
		wait -= 1.5f;
	else if (style == LS_STYLE_CAREFUL || style == LS_STYLE_CAMPER)
		wait += 1.5f;
	// (well ahead on numbers: no point waiting long)
	if (LsTempo() >= 2.0f)
		wait *= 0.6f;
	return Q_max(2.0f, wait);
}

bool CCSBot::LsTeamHeard(GameEventType event, CBasePlayer *pPlayer)
{
	if (!LsFight() || !LsTeamGame() || event != EVENT_WEAPON_FIRED || !pPlayer || pPlayer == this || !pPlayer->IsAlive())
		return false;
	if (!LsFree() || TheCSBots()->IsRoundOver() || LsRushing())
		return false;
	// (an empty gun: not running in to help - only the odd knifer or reckless one goes anyway)
	if (LsOutOfAmmo() && RANDOM_FLOAT(0.0f, 1.0f) >= LsEmptyDare(this))
	{
		m_lsTradeAt = gpGlobals->time;
		return false;
	}
	// (a bot shooting glass, the sky or a wall isn't in a fight)
	if (pPlayer->IsBot() && !static_cast<CCSBot *>(pPlayer)->IsAttacking())
		return false;
	float now = gpGlobals->time;
	if (now - m_lsTradeAt < 5.0f)
		return false;
	float d = float((pPlayer->pev->origin - pev->origin).Length());
	if (d < 250.0f || d > 1000.0f)
		return false;
	// (nor a person shooting at nothing: only with an enemy in their sight)
	if (!pPlayer->IsBot())
	{
		// (an enemy in front of them, in sight; looked for once a moment per shooter, for all
		// the bots: a sprayed clip would otherwise be thousands of traces)
		static float s_checkedAt[MAX_CLIENTS + 1];
		static bool s_foe[MAX_CLIENTS + 1];
		int who = pPlayer->entindex();
		if (who < 1 || who > MAX_CLIENTS)
			return false;
		if (now - s_checkedAt[who] > 0.5f || now < s_checkedAt[who])
		{
			s_checkedAt[who] = now;
			s_foe[who] = false;
			UTIL_MakeVectors(pPlayer->pev->v_angle);
			Vector fwd = gpGlobals->v_forward;
			for (int i = 1; i <= gpGlobals->maxClients && !s_foe[who]; i++)
			{
				CBasePlayer *pOther = UTIL_PlayerByIndex(i);
				if (!pOther || !pOther->IsAlive() || pOther->m_iTeam == pPlayer->m_iTeam || (pOther->m_iTeam != CT && pOther->m_iTeam != TERRORIST))
					continue;
				Vector to = pOther->pev->origin - pPlayer->pev->origin;
				float len = float(to.Length());
				if (len > 2500.0f || (len > 1.0f && float(DotProduct(to, fwd)) / len < 0.5f))
					continue;
				if (pPlayer->FVisible(pOther))
					s_foe[who] = true;
			}
		}
		if (!s_foe[who])
		{
			m_lsTradeAt = now - 3.5f;
			return false;
		}
	}
	m_lsTradeAt = now;
	// (loners mostly leave it, but a lurker or ninja may use it to get round them)
	float chance = LsLoner() ? ((LsStyle() == LS_STYLE_LURKER || LsStyle() == LS_STYLE_NINJA) ? 0.4f : 0.15f) : 0.35f + 0.5f * LsTeamwork();
	if (LsMatch())
		chance = Q_min(0.95f, chance + 0.2f);
	if (RANDOM_FLOAT(0.0f, 1.0f) > chance)
		return false;
	// how it helps, by who it is: the timid hang back out of it; lurkers, ninjas and the
	// careful good ones go round and flank the enemy (from another side); the rest go in close
	// behind them to trade
	float aggro = LsAggro();
	CBasePlayer *pFoe = pPlayer->IsBot() ? static_cast<CCSBot *>(pPlayer)->GetEnemy() : nullptr;
	if (aggro < 0.25f && LsTrait(93) < 0.5f && !LsMatch())
	{
		Hide(GetLastKnownArea(), RANDOM_FLOAT(3.0f, 6.0f), 400.0f);
		LsWhy("hang-back");
		return true;
	}
	bool flanker = LsStyle() == LS_STYLE_LURKER || LsStyle() == LS_STYLE_NINJA || (LsStyle() == LS_STYLE_CAREFUL && LsSkill() > 0.5f);
	if (flanker && pFoe && pFoe->IsAlive() && RANDOM_FLOAT(0.0f, 1.0f) < 0.6f)
	{
		// (a spot to the side of the enemy, away from the line the teammate is shooting along)
		Vector line = pFoe->pev->origin - pPlayer->pev->origin;
		line.z = 0.0f;
		if (!line.IsZero())
		{
			line = line.Normalize();
			Vector side(-line.y, line.x, 0.0f);
			if (DotProduct(side, pev->origin - pPlayer->pev->origin) < 0.0f)
				side = side * -1.0f;
			Vector flank = pFoe->pev->origin + side * 450.0f;
			CNavArea *pArea = TheNavAreaGrid.GetNearestNavArea(&flank);
			if (pArea)
			{
				Vector to = *pArea->GetCenter();
				MoveTo(&to, SAFEST_ROUTE);
				if (HasPath())
				{
					LsWhy("flank");
					return true;
				}
			}
		}
	}
	// near them for the trade, but its own angle: a spot 220-500 from them, clear of every
	// teammate, that sees where the enemy is (or the teammate's area, when it doesn't know) -
	// two players stacked on one spot are one spray's work; the nearer such spot to it. A
	// newer player sometimes just runs in right behind them
	const Vector &mate = pPlayer->pev->origin;
	CNavArea *foeArea = pFoe && pFoe->IsAlive() ? TheNavAreaGrid.GetNearestNavArea(&pFoe->pev->origin) : nullptr;
	CNavArea *mateArea = TheNavAreaGrid.GetNearestNavArea(&mate);
	CNavArea *bestArea = nullptr;
	float bestCost = 1e9f;
	if (RANDOM_FLOAT(0.0f, 1.0f) < 0.4f + 0.6f * LsSkill())
	{
		for (auto area : TheNavAreaList)
		{
			const Vector &c = *area->GetCenter();
			float fromMate = float((c - mate).Length2D());
			if (fromMate < 220.0f || fromMate > 500.0f || Q_abs(c.z - mate.z) > 160.0f)
				continue;
			if (area->GetAttributes() & (NAV_JUMP | NAV_CROUCH))
				continue;
			if (foeArea ? !LsSees(area, foeArea) : (mateArea && !LsSees(area, mateArea)))
				continue;
			bool crowded = false;
			LsForTeammates(this, [&](CBasePlayer *pOther) {
				if ((pOther->pev->origin - c).IsLengthLessThan(200.0f))
					crowded = true;
				// (nor where another bot's already heading: two traders the same tick)
				else if (pOther->IsBot() && static_cast<CCSBot *>(pOther)->HasPath()
					&& (static_cast<CCSBot *>(pOther)->GetPathEndpoint() - c).IsLengthLessThan(200.0f))
					crowded = true;
			});
			if (crowded)
				continue;
			// (nearer it, and not out past the teammate toward the enemy)
			float cost = float((c - pev->origin).Length2D());
			if (pFoe && pFoe->IsAlive() && (c - pFoe->pev->origin).Length2D() < (mate - pFoe->pev->origin).Length2D())
				cost += 400.0f;
			if (cost < bestCost)
			{
				bestCost = cost;
				bestArea = area;
			}
		}
	}
	Vector spot;
	if (bestArea)
		spot = *bestArea->GetCenter();
	else
	{
		Vector to = mate - pev->origin;
		spot = mate - to * (200.0f / d);
	}
	MoveTo(&spot, FASTEST_ROUTE);
	LsWhy("trade");
	return true;
}

// A CT on its own up against two or more Ts before the bomb's planted: most fall back and
// play for time (the team is coming, a site can be retaken); the bold ones take the fight
bool CCSBot::LsPlayForTime()
{
	// (once a contact: back once, then it holds where it got to)
	if (gpGlobals->time - m_lsForTimeAt < 20.0f && gpGlobals->time >= m_lsForTimeAt)
		return false;
	if (!LsFight() || !LsTeamGame() || m_iTeam != CT || TheCSBots()->GetScenario() != CCSBotManager::SCENARIO_DEFUSE_BOMB)
		return false;
	if (TheCSBots()->IsBombPlanted() || GetNearbyFriendCount() > 0 || OutnumberedCount() < 2)
		return false;
	// (rolled once a fight, not every think)
	if (m_lsForTimeRoll < 0)
	{
		float bold = LsAggro();
		// (campers and careful players hold back; rushers and knifers go in)
		float lean = (LsStyle() == LS_STYLE_CAMPER || LsStyle() == LS_STYLE_CAREFUL) ? 1.3f : (LsStyle() == LS_STYLE_RUSHER || LsStyle() == LS_STYLE_KNIFER) ? 0.5f : 1.0f;
		// (its team well ahead on numbers: help's coming, but so is the win - fewer wait)
		float numbers = LsTempo() >= 2.0f ? 0.6f : 1.0f;
		m_lsForTimeRoll = RANDOM_FLOAT(0.0f, 1.0f) < Q_min(0.95f, (LsMatch() ? 0.8f : 0.55f) * (1.2f - bold) * lean * numbers) ? 1 : 0;
	}
	if (m_lsForTimeRoll == 1)
		m_lsForTimeAt = gpGlobals->time;
	return m_lsForTimeRoll == 1;
}

// Guarding the planted bomb: an enemy who ducked out of sight is let come to it (the clock is
// on the Ts' side), rather than chased. Disciplined players nearly always; a few on a pub chase
bool CCSBot::LsHoldsPlant()
{
	if (!LsFight() || !LsTeamGame() || m_iTeam != TERRORIST || !TheCSBots()->IsBombPlanted() || GetTask() != GUARD_TICKING_BOMB)
		return false;
	// (a rusher can't sit still; a camper loves it)
	float lean = LsStyle() == LS_STYLE_CAMPER ? 1.3f : LsStyle() == LS_STYLE_RUSHER ? 0.6f : 1.0f;
	return RANDOM_FLOAT(0.0f, 1.0f) < Q_min(0.97f, (LsMatch() ? 0.9f : 0.65f) * lean);
}

// A T guarding the planted bomb, and a CT starts defusing it: only one it can see is shot at
// straight away. One it only hears takes a moment to register (less for the better ones),
// then it looks to where the sound is and heads there; too far off and it doesn't hear it
int CCSBot::LsDefuseHeard(CBasePlayer *pDefuser)
{
	if (!LsFight() || !LsTeamGame())
		return 0;
	float now = gpGlobals->time;
	// (too far to hear it: nothing to trace for)
	if ((pDefuser->pev->origin - pev->origin).IsLengthGreaterThan(2000.0f))
		return 2;
	if (IsVisible(pDefuser, true))
	{
		Attack(pDefuser);
		return 1;
	}
	float away = float((pDefuser->pev->origin - pev->origin).Length());
	if (away > 1400.0f)
		return 2;
	// (a new defuse: when it notices)
	if (now - m_lsDefuseHeardLast > 1.0f)
		m_lsDefuseNoticeAt = now + RANDOM_FLOAT(0.3f, 0.7f) + 0.6f * (1.0f - LsSkill()) + away / 2800.0f;
	m_lsDefuseHeardLast = now;
	if (now < m_lsDefuseNoticeAt)
		return 2;

	float err = away * (0.12f - 0.06f * LsSkill());
	Vector heard = pDefuser->pev->origin + Vector(RANDOM_FLOAT(-err, err), RANDOM_FLOAT(-err, err), HalfHumanHeight);
	SetLookAt("Loner-Strike: the defuse", &heard, PRIORITY_HIGH, RANDOM_FLOAT(1.0f, 2.0f));
	Vector to = pDefuser->pev->origin;
	MoveTo(&to, FASTEST_ROUTE);
	InhibitLookAround(10.0f);
	LsWhy("defuse-heard");
	return 1;
}

// A CT at the planted bomb about to defuse: without a kit it leaves it to a teammate with one
// who's close (and covers them), and one that couldn't finish in the time left gets out (the
// newer ones try anyway)
bool CCSBot::LsYieldsDefuse()
{
	if (!LsFight() || !LsTeamGame() || !TheCSBots()->IsBombPlanted())
		return false;
	const Vector *bomb = GetGameState()->GetBombPosition();
	if (!bomb)
		return false;
	float left = TheCSBots()->GetBombTimeLeft();
	if (left < (m_bHasDefuser ? 5.0f : 10.0f) + 0.2f)
	{
		// (out of the blast, as stock bots do when it's about to go: not to a hiding spot next
		// to it and back again)
		if (LsTrait(83) < 0.25f + 0.6f * LsSkill())
		{
			EscapeFromBomb();
			LsWhy("no-time-to-defuse");
			return true;
		}
		return false;
	}
	if (m_bHasDefuser || (TheCSBots()->GetBombDefuser() && TheCSBots()->GetBombDefuser() != this))
		return false;
	// (twice a round at most: after that it does it itself)
	int round = LsRoundSerial();
	if (m_lsYieldRound != round)
	{
		m_lsYieldRound = round;
		m_lsYields = 0;
	}
	if (m_lsYields >= 2)
		return false;
	// (a bot with a kit on its way to defuse, close, with time to get there and finish; a
	// person isn't counted: one defusing already has it, one who isn't may never)
	bool kitNear = false;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!pPlayer || pPlayer == this || !pPlayer->IsAlive() || !pPlayer->IsBot() || pPlayer->m_iTeam != m_iTeam || !pPlayer->m_bHasDefuser)
			continue;
		CCSBot *pBot = static_cast<CCSBot *>(pPlayer);
		if (pBot->GetTask() == DEFUSE_BOMB && !pBot->IsAttacking() && (pPlayer->pev->origin - *bomb).IsLengthLessThan(600.0f) && left > 7.0f)
			kitNear = true;
	}
	if (!kitNear)
		return false;
	m_lsYields++;
	SetTask(HOLD_POSITION);
	Hide(GetLastKnownArea(), RANDOM_FLOAT(3.0f, 6.0f), 300.0f);
	LsWhy("kit-defuses");
	return true;
}

// A teammate got a kill early in the round (a CT down in the first 40 s, Ts on a bomb map):
// the bold push on to the site while the CTs are a man down
void CCSBot::LsEarlyPick(CBasePlayer *pVictim, CBasePlayer *pKiller)
{
	if (!LsFight() || !LsTeamGame() || m_iTeam != TERRORIST || pVictim->m_iTeam != CT || pKiller == pVictim || pKiller->m_iTeam != TERRORIST)
		return;
	if (TheCSBots()->GetScenario() != CCSBotManager::SCENARIO_DEFUSE_BOMB || TheCSBots()->IsBombPlanted() || TheCSBots()->GetElapsedRoundTime() > 40.0f)
		return;
	float aggro = LsAggro();
	if (RANDOM_FLOAT(0.0f, 1.0f) < (LsMatch() ? 0.75f : 0.4f + 0.4f * aggro))
		Hurry(RANDOM_FLOAT(8.0f, 14.0f));
}

// A teammate went down to someone this bot can't see: it knows roughly where the shots came
// from, looks that way, hurries a little, and (if it's free to) goes to where the teammate
// fell rather than straight at the killer (which would look like it sees through walls)
void CCSBot::LsAvenge(CBasePlayer *pVictim, CBasePlayer *pKiller)
{
	// (never off a plant, a defuse, a run from the bomb, or hostages in tow - nor pulled off a
	// person's order)
	if (m_state == &m_plantBombState || m_state == &m_defuseBombState || IsEscapingFromBomb() || m_hostageEscortCount > 0 || LsOrderActive())
		return;
	float away = float((pKiller->pev->origin - pev->origin).Length());
	float err = away * (0.15f - 0.08f * LsSkill());
	Vector heard = pKiller->pev->origin + Vector(RANDOM_FLOAT(-err, err), RANDOM_FLOAT(-err, err), HalfHumanHeight);
	SetLookAt("Loner-Strike: where the shots came from", &heard, PRIORITY_HIGH, RANDOM_FLOAT(1.0f, 2.0f));
	// (a player who knows better doesn't walk to the body - the killer's waiting round that
	// corner: it holds an angle on it, eyes on where the shots came from, and lets them come
	// or a teammate trade; the bold and the newer ones go in, as people do)
	// (how likely, and how long: skill and patience say hold, boldness says go - campers and
	// careful players sit on it longest, lurkers and ninjas a while, rushers hardly ever)
	int style = LsStyle();
	float lean = (style == LS_STYLE_CAMPER || style == LS_STYLE_CAREFUL) ? 1.35f : (style == LS_STYLE_LURKER || style == LS_STYLE_NINJA) ? 1.1f
		: (style == LS_STYLE_RUSHER || style == LS_STYLE_KNIFER) ? 0.25f : 1.0f;
	float holdOdds = Q_min(0.95f, (0.15f + 0.75f * LsSkill()) * (1.0f - 0.55f * LsAggro()) * lean);
	// (a CT with the bomb down and time to get there doesn't sit on an angle: the clock says go)
	bool holds = !LsSolo() && !LsRetakeRush() && RANDOM_FLOAT(0.0f, 1.0f) < holdOdds;
	if (holds && LsFree() && !IsAttacking())
	{
		float secs = RANDOM_FLOAT(3.0f, 6.0f) * (0.7f + 0.6f * (1.0f - LsAggro())) * (lean > 1.2f ? 1.4f : 1.0f);
		// (already holding a spot: it stays there - only the eyes go to the killer's angle)
		// (and one on its way to cover keeps going there - only the eyes turn)
		if (!IsHiding() && gpGlobals->time - m_lsCoverPickAt > 3.0f)
			Hide(GetLastKnownArea(), secs, 250.0f, true);
		// (the eyes stay on that angle for the hold, not just the first glance: the glance
		// above would block the pre-aim's lower priority)
		float watch = RANDOM_FLOAT(3.0f, 5.0f);
		ClearLookAt();
		if (!LsPreAim(heard, watch))
			SetLookAt("Loner-Strike: where the shots came from", &heard, PRIORITY_HIGH, watch);
		LsWhy("avenge: holding the angle");
		return;
	}
	// (on a timed retake it keeps its way to the bomb: the glance at the shots is all)
	if (LsRetakeRush())
		return;
	Hurry(RANDOM_FLOAT(4.0f, 7.0f));
	if (LsFree() && !LsLoner())
	{
		MoveTo(&pVictim->pev->origin, FASTEST_ROUTE);
		LsWhy("avenge");
	}
}

// A dropped bomb: the nearest one or two Ts go for it (the rest carry on), so the whole team
// doesn't run into the doorway the carrier just died in. Asked again each time a T thinks
// about what to do, counting only living bots: if those two die on the way, the next two
// go; a human standing nearer doesn't hold the bots back (they can't know he will)
bool CCSBot::LsFetchesBomb()
{
	if (!LsFight())
		return true;
	if (LsBombFetcher() != this)
		return false;
	// (where a teammate just died: not straight into that angle alone - with a teammate close,
	// or once a few seconds have gone by, or when the clock says now)
	CBaseEntity *pBomb = TheCSBots()->GetLooseBomb();
	// (careful players always wait for company; rushers sometimes just grab it - a steady
	// lean per bot)
	int fetchStyle = LsStyle();
	float careful = (fetchStyle == LS_STYLE_CAREFUL || fetchStyle == LS_STYLE_CAMPER) ? 1.0f
		: (fetchStyle == LS_STYLE_RUSHER || fetchStyle == LS_STYLE_KNIFER) ? 0.4f : 0.3f + 0.6f * LsSkill() - 0.3f * LsAggro();
	if (pBomb && LsBombHot(pBomb->pev->origin) && LsTimeLeft() > 30.0f && LsTrait(51) < careful)
	{
		bool company = false;
		LsForTeammates(this, [&](CBasePlayer *pPlayer) {
			if ((pPlayer->pev->origin - pev->origin).IsLengthLessThan(600.0f))
				company = true;
		});
		if (!company)
			return false;
	}
	return true;
}

// The one T bot that goes for a dropped bomb: the nearest that's free (not in a fight, not
// saving its gun, not under a person's order), picked for the team and picked again every
// two seconds, so one that dies, gets into a fight or gets stuck hands it on. (It was "go if
// fewer than two closer ones would": closer ones that never went, hunting or looking into a
// noise, kept everyone back and the bomb lay there: 41% never picked up, 25% for 0.1.2)
CCSBot *CCSBot::LsBombFetcher()
{
	static int s_index = 0;
	static float s_at = -100.0f;
	CBaseEntity *pBomb = TheCSBots()->GetLooseBomb();
	if (!pBomb)
		return nullptr;
	float now = gpGlobals->time;
	CBasePlayer *pKept = s_index ? UTIL_PlayerByIndex(s_index) : nullptr;
	if (now - s_at < 2.0f && now >= s_at && pKept && pKept->IsBot() && pKept->IsAlive() && pKept->m_iTeam == TERRORIST)
		return static_cast<CCSBot *>(pKept);
	s_at = now;
	s_index = 0;
	float best = 1e9f;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!pPlayer || FNullEnt(pPlayer->pev) || !pPlayer->IsBot() || !pPlayer->IsAlive() || pPlayer->m_iTeam != TERRORIST)
			continue;
		CCSBot *pBot = static_cast<CCSBot *>(pPlayer);
		if (pBot->IsAttacking() || pBot->LsSavingGun() || pBot->LsOrderActive())
			continue;
		float d = float((pBomb->pev->origin - pBot->pev->origin).Length());
		if (d < best)
		{
			best = d;
			s_index = i;
		}
	}
	CBasePlayer *pPick = s_index ? UTIL_PlayerByIndex(s_index) : nullptr;
	return pPick ? static_cast<CCSBot *>(pPick) : nullptr;
}

// A map with guns lying about (fy_, some aim_ maps): a bot without a primary at the start of
// a round runs for the best one close by, as people sprint for the AK. One bot to a gun.
void CCSBot::LsFloorGunTick()
{
	if (!LsFight() || LsSolo() || !IsAlive() || !CSGameRules() || CSGameRules()->IsFreezePeriod() || LsWeaponModeOn())
		return;
	int round = LsRoundSerial();
	if (m_lsFloorRound == round)
	{
		// (got one, or someone else did: a claim only lasts while it's still going for it)
		if (m_lsFloorGun.IsValid() && (m_bHasPrimary || (m_lsFloorGun->pev->effects & EF_NODRAW)))
		{
			bool taken = !m_bHasPrimary;
			m_lsFloorGun = nullptr;
			// (beaten to it: another close by, else on with the round)
			if (taken && TheCSBots()->GetElapsedRoundTime() < 15.0f)
				m_lsFloorRound = -1;
			else if (taken && IsMovingTo() && !IsAttacking() && !LsOrderActive())
				Idle();
		}
		return;
	}
	// (a claim from a round or a life before is no claim)
	m_lsFloorGun = nullptr;
	if (m_bHasPrimary || TheCSBots()->GetElapsedRoundTime() > 15.0f || IsAttacking() || IsCarryingBomb() || IsBuying())
		return;
	m_lsFloorRound = round;
	m_lsFloorGun = nullptr;

	CBaseEntity *pEnt = nullptr, *pBest = nullptr;
	float best = 1500.0f;
	while ((pEnt = UTIL_FindEntityByClassname(pEnt, "armoury_entity")))
	{
		if (pEnt->pev->effects & EF_NODRAW)
			continue;
		ArmouryItemPack item = static_cast<CArmoury *>(pEnt)->m_iItem;
		if (item >= ARMOURY_FLASHBANG && item != ARMOURY_FAMAS && item != ARMOURY_SG550 && item != ARMOURY_GALIL && item != ARMOURY_UMP45)
			continue;	// (grenades, armour, pistols: not worth the run)
		if (item == ARMOURY_SHIELD)
			continue;
		// (someone already on their way to it)
		bool taken = false;
		for (int i = 1; i <= gpGlobals->maxClients && !taken; i++)
		{
			CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
			if (pPlayer && pPlayer != this && pPlayer->IsBot() && pPlayer->IsAlive() && static_cast<CBaseEntity *>(static_cast<CCSBot *>(pPlayer)->m_lsFloorGun) == pEnt
				&& static_cast<CCSBot *>(pPlayer)->m_lsFloorRound == round)
				taken = true;
		}
		if (taken)
			continue;
		float d = float((pEnt->pev->origin - pev->origin).Length());
		// (a rifle or the AWP is worth a longer run than an SMG or a shotgun)
		bool rifle = item == ARMOURY_AK47 || item == ARMOURY_M4A1 || item == ARMOURY_AWP || item == ARMOURY_SG552 || item == ARMOURY_AUG
			|| item == ARMOURY_FAMAS || item == ARMOURY_GALIL;
		if (rifle)
			d *= 0.6f;
		if (d < best)
		{
			best = d;
			pBest = pEnt;
		}
	}
	if (!pBest)
		return;
	m_lsFloorGun = pBest;
	Vector to = pBest->pev->origin;
	MoveTo(&to, FASTEST_ROUTE);
	LsWhy("floor-gun");
}

// Rounds on a map with no objective (awp_, aim_, fy_ played in rounds): nothing makes anyone
// move, and both sides can hold till the clock runs out. After a quiet spell the bold go
// looking (sooner the bolder), and the last one alive on a side always does
void CCSBot::LsStalemateTick()
{
	float now = gpGlobals->time;
	if (now < m_lsStaleCheckAt)
		return;
	m_lsStaleCheckAt = now + RANDOM_FLOAT(2.0f, 4.0f);
	if (!LsFight() || LsSolo() || LsTeamGame() || !IsAlive() || IsAttacking() || IsHunting() || !CSGameRules() || CSGameRules()->IsFreezePeriod())
		return;
	// (only a bot standing about or holding a spot: not one following someone, on its way
	// somewhere or looking into a noise; and once a round)
	int round = LsRoundSerial();
	int mates = 0;
	LsForTeammates(this, [&](CBasePlayer *) { mates++; });
	// (once a round, or once more as the last one alive; never over a person's order)
	bool again = mates == 0 && (m_lsStaleLast != round || now - m_lsStaleLastAt > 20.0f);
	if ((m_lsStaleRound == round && !again) || LsOrderActive() || FStrEq(m_lsPuppet.holdTag, "bc") || IsFollowing() || IsMovingTo() || IsNoiseHeard()
		|| !(IsHiding() || m_state == &m_idleState))
		return;
	float aggro = LsAggro();
	float wait = mates == 0 ? 20.0f : 35.0f + 45.0f * (1.0f - aggro) + 20.0f * LsTrait(85);
	if (TheCSBots()->GetElapsedRoundTime() < wait || GetTimeSinceLastSawEnemy() < 12.0f || GetTimeSinceAttacked() < 8.0f)
		return;
	// (the most patient never do, unless they're the last, or it's been two minutes)
	if (mates > 0 && aggro < 0.3f && LsTrait(86) < 0.6f && TheCSBots()->GetElapsedRoundTime() < 120.0f + 60.0f * LsTrait(87))
		return;
	m_lsStaleRound = round;
	if (mates == 0)
	{
		m_lsStaleLast = round;
		m_lsStaleLastAt = now;
	}
	if (m_lsPuppet.any)
		LsPuppetRelease();
	Hunt();
	Hurry(RANDOM_FLOAT(6.0f, 12.0f));
	LsWhy("stalemate");
}

// The bomb site with the fewest living teammates within 1500 units (ties at random)
const CCSBotManager::Zone *CCSBot::LsQuietZone()
{
	const CCSBotManager::Zone *best = nullptr;
	float bestScore = 1e9f;
	for (int z = 0; z < TheCSBots()->GetZoneCount(); z++)
	{
		const CCSBotManager::Zone *zone = TheCSBots()->GetZone(z);
		float score = RANDOM_FLOAT(0.0f, 0.5f);
		// (where they are, or for a bot on its way, where it's going: at spawn they're all far
		// from both sites)
		// (by the site each is closest to: on nuke the two are stacked, a radius would count both)
		LsForTeammates(this, [&](CBasePlayer *pPlayer) {
			Vector at = pPlayer->pev->origin;
			if (pPlayer->IsBot() && static_cast<CCSBot *>(pPlayer)->HasPath())
				at = static_cast<CCSBot *>(pPlayer)->GetPathEndpoint();
			if (LsSiteDist(at, zone->m_center) < 1500.0f && LsNearZone(at) == zone)
				score += 1.0f;
		});
		if (score < bestScore)
		{
			bestScore = score;
			best = zone;
		}
	}
	return best ? best : TheCSBots()->GetRandomZone();
}

// At a bomb site with the bomb. Stock bots plant on the first step in, in the open at the
// entrance; people walk in to a spot with some cover first (a corner, behind a box, the far
// side). It plants straight away when shot at lately, with an enemy in sight, with the clock
// short, as a newer player, or once at its spot (or after 6 s of trying)
bool CCSBot::LsPlantHere()
{
	if (!LsFight())
		return true;
	float now = gpGlobals->time;
	int round = LsRoundSerial();
	if (GetTimeSinceAttacked() < 3.0f || now - m_lsShotAtAt < 3.0f || IsEnemyVisible() || LsTimeLeft() < 18.0f || LsSkill() < 0.25f)
		return true;
	if (m_lsPlantRound == round)
		return (m_lsPlantSpot - pev->origin).Make2D().IsLengthLessThan(70.0f) || now > m_lsPlantBy || !IsMovingTo();
	m_lsPlantRound = round;
	const CCSBotManager::Zone *zone = TheCSBots()->GetZone(&pev->origin);
	if (!zone)
		return true;
	// (already in the middle of it, or right by cover: here's fine)
	if ((pev->origin - zone->m_center).Make2D().IsLengthLessThan(320.0f))
		return true;
	// (a few spots in the site: further in from where it came in, and near a hiding spot)
	CNavArea *best = nullptr;
	float bestScore = -1e9f;
	for (int t = 0; t < 8; t++)
	{
		CNavArea *pArea = TheCSBots()->GetRandomAreaInZone(zone);
		if (!pArea || TheCSBots()->GetZone(pArea->GetCenter()) != zone)
			continue;	// (an area whose middle is outside the plant zone won't do)
		// (in toward the middle of the site, near cover; not across it to the far entrance)
		float d = float((*pArea->GetCenter() - pev->origin).Length2D());
		if (d < 140.0f || d > 500.0f)
			continue;
		float mid = float((*pArea->GetCenter() - zone->m_center).Length2D());
		float score = -mid + RANDOM_FLOAT(0.0f, 200.0f) + (pArea->GetHidingSpotList()->empty() ? 0.0f : 300.0f);
		// (a plant seen from fewer places is harder to retake and to defuse: the map knowledge
		// says how many areas see each spot; the ones who know it pick the hidden corner)
		const LsAreaKnow *k = LsKnow(pArea);
		if (k && LsKnows(LSK_DEFUSE_MATH))
			score -= 500.0f * Q_min(1.0f, float(k->sees) / 600.0f);
		if (score > bestScore)
		{
			bestScore = score;
			best = pArea;
		}
	}
	if (!best)
		return true;
	m_lsPlantSpot = *best->GetCenter();
	m_lsPlantBy = now + 4.0f;
	MoveTo(&m_lsPlantSpot, FASTEST_ROUTE);
	SetTask(PLANT_BOMB);
	LsWhy("plant-spot");
	return false;
}

// Guarding the planted bomb. Stock gives every guard the same best hiding spot near it, and
// three of them end up crouched shoulder to shoulder. People spread round it: a spot near the
// bomb (seeing it if possible) that no teammate is at or on the way to
void CCSBot::LsHideNearBomb(const Vector &bomb)
{
	CNavArea *pBombArea = TheNavAreaGrid.GetNavArea(&bomb);
	if (!LsFight())
	{
		Hide(pBombArea);
		return;
	}
	const Vector *best = nullptr;
	float bestScore = -1e9f;
	// (the teammates and where they hide, gathered once)
	Vector mates[MAX_CLIENTS], mateSpots[MAX_CLIENTS];
	int nm = 0, ns = 0;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		if (nm < MAX_CLIENTS)
			mates[nm++] = pPlayer->pev->origin;
		if (pPlayer->IsBot() && static_cast<CCSBot *>(pPlayer)->IsHiding() && ns < MAX_CLIENTS)
			mateSpots[ns++] = static_cast<CCSBot *>(pPlayer)->m_hideState.GetHidingSpot();
	});
	for (auto pSpot : TheHidingSpotList)
	{
		const Vector *pos = pSpot->GetPosition();
		float d = float((*pos - bomb).Length2D());
		if (d < 150.0f || d > 1000.0f || fabs(pos->z - bomb.z) > 250.0f)
			continue;
		bool taken = false;
		for (int m = 0; m < nm && !taken; m++)
			taken = (mates[m] - *pos).IsLengthLessThan(300.0f);
		for (int m = 0; m < ns && !taken; m++)
			taken = (mateSpots[m] - *pos).IsLengthLessThan(300.0f);
		if (taken)
			continue;
		TraceResult tr;
		Vector eye = *pos + Vector(0, 0, HalfHumanHeight);
		UTIL_TraceLine(eye, bomb + Vector(0, 0, 8.0f), ignore_monsters, ENT(pev), &tr);
		float score = RANDOM_FLOAT(0.0f, 250.0f) - 0.4f * float(fabs(d - 500.0f)) + (tr.flFraction >= 1.0f ? 400.0f : 0.0f);
		// (a crossfire: from a different side of the bomb than the teammates guarding it, so a
		// defuser can't face them all; the better ones set it up)
		if (LsSkill() > 0.35f)
		{
			Vector dir = *pos - bomb;
			dir.z = 0.0f;
			float len = float(dir.Length2D());
			LsForTeammates(this, [&](CBasePlayer *pPlayer) {
				Vector theirs = pPlayer->pev->origin - bomb;
				theirs.z = 0.0f;
				float tl = float(theirs.Length2D());
				if (len > 1.0f && tl > 1.0f && tl < 1200.0f && float(DotProduct(dir, theirs)) / (len * tl) > 0.64f)
					score -= 350.0f;
			});
		}
		if (score > bestScore)
		{
			bestScore = score;
			best = pos;
		}
	}
	if (!best)
	{
		Hide(pBombArea);
		return;
	}
	Hide(best);
	LsWhy("guard-spread");
}

const CCSBotManager::Zone *CCSBot::LsPlantOrder() const
{
	return m_lsPlantOrderRound == LsRoundSerial() ? m_lsPlantOrderZone : nullptr;
}

// A hostage being led away (heard): the CTs take it to the nearest rescue zone, so that's the
// one to cover. The better players wait partway along the way back - a third of the way from
// the zone toward the hostage - to catch them on the way; the rest guard the zone. One already
// close to the hostage leaves it to the noise (it goes after them)
bool CCSBot::LsCutOffRescue(const Vector &hostage)
{
	if ((hostage - pev->origin).IsLengthLessThan(700.0f))
		return false;
	const CCSBotManager::Zone *best = nullptr;
	float bestD = 1e9f;
	for (int i = 0; i < TheCSBots()->GetZoneCount(); i++)
	{
		const CCSBotManager::Zone *zone = TheCSBots()->GetZone(i);
		float d = float((zone->m_center - hostage).Length2D());
		if (d < bestD)
		{
			bestD = d;
			best = zone;
		}
	}
	if (!best)
		return false;
	CNavArea *spot = nullptr;
	if (LsSkill() > 0.4f && bestD > 600.0f)
	{
		Vector partway = best->m_center + (hostage - best->m_center) * 0.33f;
		spot = TheNavAreaGrid.GetNearestNavArea(&partway);
	}
	if (!spot)
		spot = TheCSBots()->GetRandomAreaInZone(best);
	if (!spot)
		return false;
	Hide(spot, RANDOM_FLOAT(20.0f, 35.0f), 500.0f);
	LsWhy("cut off the rescue");
	return true;
}

// The ways in to a retake taken this round (from which side of the bomb), so the next CT takes
// another: an entry straight in, the rest from round the other sides - a crossfire on the Ts
// holding it, not a queue through one door. Not the one going to defuse; the better players
static int s_retakeRound = -1;
static Vector2D s_retakeDirs[8];
static int s_retakeCount = 0;

void CCSBot::LsRetakeApproach()
{
	int round = LsRoundSerial();
	// (on its way to defuse counts too - a lone CT that chose not to wait gets that task the
	// same moment; only one already defusing goes straight on)
	if (!LsFree() || IsDefusingBomb() || IsHiding() || LsSkill() < 0.35f || !LsRetakeRush())
		return;
	// (after the wait for teammates a lone CT may do first - LsTeamTick - not instead of it)
	{
		float d = float((*GetGameState()->GetBombPosition() - pev->origin).Length());
		bool mayWait = GetFriendsRemaining() > 0 && GetNearbyFriendCount() == 0 && TheCSBots()->GetBombTimeLeft() > 22.0f && d > 800.0f && d < 2200.0f;
		if (mayWait && m_lsRetakeRound != round)
			return;
	}
	m_lsRetakeWayRound = round;
	if (s_retakeRound != round)
	{
		s_retakeRound = round;
		s_retakeCount = 0;
	}
	Vector bomb = *GetGameState()->GetBombPosition();
	Vector2D mine = (pev->origin - bomb).Make2D();
	if (mine.IsLengthLessThan(700.0f))
		return;		// (close already: in it goes)
	mine.NormalizeInPlace();
	// (the first comes straight from where it is)
	if (s_retakeCount == 0)
	{
		s_retakeDirs[s_retakeCount++] = mine;
		return;
	}
	// a side at least 70 degrees off every way already taken, the nearest to its own
	Vector best;
	float bestScore = -1e9f;
	bool found = false;
	for (int k = 0; k < 12; k++)
	{
		float yaw = float(k) * 30.0f;
		Vector2D dir(BotCOS(yaw), BotSIN(yaw));
		bool clash = false;
		for (int i = 0; i < s_retakeCount && !clash; i++)
			clash = float(DotProduct(dir, s_retakeDirs[i])) > 0.34f;
		if (clash)
			continue;
		Vector spot = bomb + Vector(dir.x, dir.y, 0.0f) * 550.0f;
		CNavArea *area = TheNavAreaGrid.GetNearestNavArea(&spot);
		if (!area || !LsHeadroom(area) || (*area->GetCenter() - spot).Make2D().IsLengthGreaterThan(250.0f))
			continue;
		float score = float(DotProduct(dir, mine));
		if (score > bestScore)
		{
			bestScore = score;
			best = *area->GetCenter();
			found = true;
			if (s_retakeCount < 8)
				s_retakeDirs[s_retakeCount] = dir;
		}
	}
	if (!found)
		return;
	if (s_retakeCount < 8)
		s_retakeCount++;
	MoveTo(&best, SAFEST_ROUTE);
	Hurry(4.0f);
	LsWhy("retake: another way in");
}
