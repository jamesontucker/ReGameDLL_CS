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
	if (!LsFight() || LsSolo() || !IsAlive())
		return;
	float now = gpGlobals->time;
	int round = CSGameRules()->m_iTotalRoundsPlayed;

	// a teammate's call it looked into first: on its way now, unless something came up
	if (m_lsRotateAt > 0.0f && now >= m_lsRotateAt)
	{
		m_lsRotateAt = 0.0f;
		if (!IsAttacking() && !IsBusy() && !IsCarryingBomb())
		{
			MoveTo(&m_lsRotateTo, FASTEST_ROUTE);
			LsWhy("rotate");
		}
	}

	// the round's over: a bot without a gun picks one up from the floor close by
	if (TheCSBots()->IsRoundOver() && m_lsPickupRound != round && !m_bHasPrimary && !IsAttacking())
	{
		m_lsPickupRound = round;
		CBaseEntity *pBox = nullptr, *pBest = nullptr;
		float best = 700.0f;
		while ((pBox = UTIL_FindEntityByClassname(pBox, "weaponbox")))
		{
			float d = float((pBox->pev->origin - pev->origin).Length());
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

	// a retake: a CT on its own waits for a teammate before going in (once a round), when
	// there's time for it
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
					Hide(GetLastKnownArea(), RANDOM_FLOAT(3.0f, 6.0f));
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
	if (!LsFight() || LsSolo() || !m_radioSubject)
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
		if (IsAttacking() || IsBusy() || IsCarryingBomb())
			return true;
		if (LsLoner() && RANDOM_FLOAT(0.0f, 1.0f) < 0.6f)
			return true;
		float skill = GetProfile()->GetSkill();
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
bool CCSBot::LsTeamHeard(GameEventType event, CBasePlayer *pPlayer)
{
	if (!LsFight() || LsSolo() || event != EVENT_WEAPON_FIRED || !pPlayer || pPlayer == this || !pPlayer->IsAlive())
		return false;
	if (IsAttacking() || IsBusy() || IsCarryingBomb() || IsUsingLadder())
		return false;
	float now = gpGlobals->time;
	if (now - m_lsTradeAt < 5.0f)
		return false;
	float d = float((pPlayer->pev->origin - pev->origin).Length());
	if (d < 250.0f || d > 1000.0f)
		return false;
	m_lsTradeAt = now;
	float chance = LsLoner() ? 0.15f : 0.35f + 0.5f * LsTeamwork();
	if (LsMatch())
		chance = Q_min(0.95f, chance + 0.2f);
	if (RANDOM_FLOAT(0.0f, 1.0f) > chance)
		return false;
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
	if (!LsFight() || LsSolo() || m_iTeam != CT || TheCSBots()->GetScenario() != CCSBotManager::SCENARIO_DEFUSE_BOMB)
		return false;
	if (TheCSBots()->IsBombPlanted() || GetNearbyFriendCount() > 0 || OutnumberedCount() < 2)
		return false;
	float bold = LsAggro();
	// (campers and careful players hold back; rushers and knifers go in)
	float lean = (LsStyle() == LS_STYLE_CAMPER || LsStyle() == LS_STYLE_CAREFUL) ? 1.3f : (LsStyle() == LS_STYLE_RUSHER || LsStyle() == LS_STYLE_KNIFER) ? 0.5f : 1.0f;
	return RANDOM_FLOAT(0.0f, 1.0f) < Q_min(0.95f, (LsMatch() ? 0.8f : 0.55f) * (1.2f - bold) * lean);
}

// Guarding the planted bomb: an enemy who ducked out of sight is let come to it (the clock is
// on the Ts' side), rather than chased. Disciplined players nearly always; a few on a pub chase
bool CCSBot::LsHoldsPlant()
{
	if (!LsFight() || LsSolo() || m_iTeam != TERRORIST || !TheCSBots()->IsBombPlanted() || GetTask() != GUARD_TICKING_BOMB)
		return false;
	// (a rusher can't sit still; a camper loves it)
	float lean = LsStyle() == LS_STYLE_CAMPER ? 1.3f : LsStyle() == LS_STYLE_RUSHER ? 0.6f : 1.0f;
	return RANDOM_FLOAT(0.0f, 1.0f) < Q_min(0.97f, (LsMatch() ? 0.9f : 0.65f) * lean);
}

// A teammate got a kill early in the round (a CT down in the first 40 s, Ts on a bomb map):
// the bold push on to the site while the CTs are a man down
void CCSBot::LsEarlyPick(CBasePlayer *pVictim, CBasePlayer *pKiller)
{
	if (!LsFight() || LsSolo() || m_iTeam != TERRORIST || pVictim->m_iTeam != CT || pKiller == pVictim || pKiller->m_iTeam != TERRORIST)
		return;
	if (TheCSBots()->GetScenario() != CCSBotManager::SCENARIO_DEFUSE_BOMB || TheCSBots()->IsBombPlanted() || TheCSBots()->GetElapsedRoundTime() > 40.0f)
		return;
	float aggro = LsAggro();
	if (RANDOM_FLOAT(0.0f, 1.0f) < (LsMatch() ? 0.75f : 0.4f + 0.4f * aggro))
		Hurry(RANDOM_FLOAT(8.0f, 14.0f));
}
