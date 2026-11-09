/*
*
*   Loner-Strike additions to the CZ bots: team play and objectives: trades, spacing, retakes, post-plant, rotations, round plans, kits, pickups.
*   (cs_bot_ls.cpp has the cvars and the rest; the habits here follow the same rules: bot_ls_fight,
*   LsSolo / LsMatch for the mode, LsTrait for each bot's own leanings)
*
*/

#include "precompiled.h"

// Team play each frame (cheap: timers first)
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
		m_lsForTimeRoll = -1;
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

	// a dropped bomb, seen on the radar: a T on its way somewhere or hiding (states that never
	// look for it) goes for it too, if it's one of the nearest
	if (m_iTeam == TERRORIST && now >= m_lsFetchCheckAt && TheCSBots()->GetLooseBomb() && !IsAttacking() && !LsOrderActive() && !m_lsSaving
		&& (IsMovingTo() || IsHiding()) && GetTask() != PLANT_BOMB)
	{
		m_lsFetchCheckAt = now + 1.0f;
		if (LsFetchesBomb())
		{
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
			m_lsSwapBox = nullptr;
		else if ((pBox->pev->origin - pev->origin).Make2D().IsLengthLessThan(80.0f))
		{
			if (pMine)
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
		&& !CSGameRules()->IsFreezePeriod() && !LsWeaponModeOn() && !TheCSBots()->IsBombPlanted() && GetHostageEscortCount() == 0
		&& GetTask() != DEFUSE_BOMB && GetTask() != COLLECT_HOSTAGES && GetTask() != RESCUE_HOSTAGES && GetTimeSinceLastSawEnemy() > 8.0f)
	{
		m_lsSwapCheckAt = now + 2.0f;
		CBasePlayerItem *pPrimary = m_rgpPlayerItems[PRIMARY_WEAPON_SLOT];
		bool poor = !pPrimary || pPrimary->m_iId == WEAPON_MP5N || pPrimary->m_iId == WEAPON_TMP || pPrimary->m_iId == WEAPON_MAC10 || pPrimary->m_iId == WEAPON_UMP45
			|| pPrimary->m_iId == WEAPON_P90 || pPrimary->m_iId == WEAPON_M3 || pPrimary->m_iId == WEAPON_XM1014;
		if (poor)
		{
			static const char *const better[] = { "models/w_ak47.mdl", "models/w_m4a1.mdl", "models/w_awp.mdl", "models/w_sg552.mdl", "models/w_aug.mdl",
				"models/w_galil.mdl", "models/w_famas.mdl" };
			CBaseEntity *pBox = nullptr;
			while ((pBox = UTIL_FindEntityByClassname(pBox, "weaponbox")))
			{
				if ((pBox->pev->origin - pev->origin).IsLengthGreaterThan(450.0f))
					continue;
				bool good = false;
				for (const char *g : better)
					good = good || FStrEq(STRING(pBox->pev->model), g);
				if (!good || !FVisible(pBox->pev->origin + Vector(0, 0, 8)))
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
				Vector to = pBox->pev->origin;
				MoveTo(&to, FASTEST_ROUTE);
				if (!HasPath())
					continue;
				m_lsSwapBox = pBox;
				m_lsSwapUntil = now + 6.0f;
				LsWhy("swap-gun");
				break;
			}
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

bool CCSBot::LsTeamHeard(GameEventType event, CBasePlayer *pPlayer)
{
	if (!LsFight() || !LsTeamGame() || event != EVENT_WEAPON_FIRED || !pPlayer || pPlayer == this || !pPlayer->IsAlive())
		return false;
	if (!LsFree() || TheCSBots()->IsRoundOver() || LsRetakeRush())
		return false;
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
	// close behind them, not on top of them
	Vector to = pPlayer->pev->origin - pev->origin;
	Vector spot = pPlayer->pev->origin - to * (200.0f / d);
	MoveTo(&spot, FASTEST_ROUTE);
	LsWhy("trade");
	return true;
}

// A CT on its own up against two or more Ts before the bomb's planted: most fall back and
// play for time (the team is coming, a site can be retaken); the bold ones take the fight
bool CCSBot::LsPlayForTime()
{
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
		m_lsForTimeRoll = RANDOM_FLOAT(0.0f, 1.0f) < Q_min(0.95f, (LsMatch() ? 0.8f : 0.55f) * (1.2f - bold) * lean) ? 1 : 0;
	}
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
	float away = float((pKiller->pev->origin - pev->origin).Length());
	float err = away * (0.15f - 0.08f * LsSkill());
	Vector heard = pKiller->pev->origin + Vector(RANDOM_FLOAT(-err, err), RANDOM_FLOAT(-err, err), HalfHumanHeight);
	SetLookAt("Loner-Strike: where the shots came from", &heard, PRIORITY_HIGH, RANDOM_FLOAT(1.0f, 2.0f));
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
	CBaseEntity *pBomb = TheCSBots()->GetLooseBomb();
	if (!pBomb)
		return true;
	float mine = float((pBomb->pev->origin - pev->origin).Length());
	int closer = 0;
	LsForTeammates(this, [&](CBasePlayer *pPlayer) {
		// (only bots that will go, or are going: one fetching it, or one standing about or
		// hunting and free; one on its way somewhere or hiding never looks for the bomb, and
		// counting it would leave the bomb lying there)
		if (!pPlayer->IsBot() || (pBomb->pev->origin - pPlayer->pev->origin).IsLengthGreaterThan(mine))
			return;
		CCSBot *pBot = static_cast<CCSBot *>(pPlayer);
		bool looks = pBot->m_state == &pBot->m_idleState || pBot->IsHunting() || pBot->IsMovingTo() || pBot->IsHiding();
		if (pBot->m_state == &pBot->m_fetchBombState || (looks && !pBot->IsAttacking() && !pBot->LsSavingGun() && !pBot->LsOrderActive() && pBot->GetTask() != PLANT_BOMB))
			closer++;
	});
	return closer < 2;
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

// The bomb site with the fewest living teammates within 1200 units (ties at random)
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
			if ((at - zone->m_center).IsLengthLessThan(1500.0f) && TheCSBots()->GetClosestZone(&at) == zone)
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
	for (auto pSpot : TheHidingSpotList)
	{
		const Vector *pos = pSpot->GetPosition();
		float d = float((*pos - bomb).Length2D());
		if (d < 150.0f || d > 1000.0f || fabs(pos->z - bomb.z) > 250.0f)
			continue;
		bool taken = false;
		LsForTeammates(this, [&](CBasePlayer *pPlayer) {
			if ((pPlayer->pev->origin - *pos).IsLengthLessThan(300.0f))
				taken = true;
			else if (pPlayer->IsBot())
			{
				CCSBot *pBot = static_cast<CCSBot *>(pPlayer);
				if (pBot->IsHiding() && (pBot->m_hideState.GetHidingSpot() - *pos).IsLengthLessThan(300.0f))
					taken = true;
			}
		});
		if (taken)
			continue;
		TraceResult tr;
		Vector eye = *pos + Vector(0, 0, HalfHumanHeight);
		UTIL_TraceLine(eye, bomb + Vector(0, 0, 8.0f), ignore_monsters, ENT(pev), &tr);
		float score = RANDOM_FLOAT(0.0f, 250.0f) - 0.4f * float(fabs(d - 500.0f)) + (tr.flFraction >= 1.0f ? 400.0f : 0.0f);
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
