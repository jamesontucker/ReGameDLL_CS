/*
*
*   Loner-Strike: Hackers. A bot in the Hackers bot style (or the rare occasional hacker in
*   another style) cheats, the way a cheater on a pub server does: gm_humanbots picks each
*   one's kit from who it is and hands it over with
*
*       ls_bot <index> hack <letters> <level>
*
*   letters: w wallhack and ESP (knows where its enemies are through walls: the crosshair
*   follows them, it walks to them; a CT knows where the bomb is), a aimbot (the snap to the
*   head, no reaction time, sees all round it and through smoke), t triggerbot (fires the
*   instant an enemy crosses the crosshair), s spinbot (the body spins; the shots still go
*   where it aims), r no recoil, f no flash and no smoke, b wallbangs (fires through a thin
*   wall at someone it knows is there). Bunnyhops, no spread and backtrack are gm_humanbots'
*   (the bullets and the movement keys). "-" or no letters: no cheats.
*   level: 0 closet (could pass for a good player), 1 mixed, 2 rage (as blatant as it gets).
*
*   Not in modes the brain steers bots through (surf, deathrun): those leave bot_ls_fight off,
*   and every cheat here needs it.
*/

#include "precompiled.h"

// (for testing: "<letters> <level>" gives every bot that kit, "" leaves it to the plugin)
cvar_t cv_bot_ls_hack_test = { "bot_ls_hack_test", "", FCVAR_SERVER, 0.0f, nullptr };

void CCSBot::LsHackSet(const char *letters, int level)
{
	int kit = 0;
	for (const char *c = letters; c && *c; c++)
	{
		switch (*c)
		{
		case 'w': kit |= LS_HK_WALL; break;
		case 'a': kit |= LS_HK_AIM; break;
		case 't': kit |= LS_HK_TRIGGER; break;
		case 's': kit |= LS_HK_SPIN; break;
		case 'r': kit |= LS_HK_RECOIL; break;
		case 'f': kit |= LS_HK_NOFLASH; break;
		case 'b': kit |= LS_HK_BANG; break;
		}
	}
	m_lsHack = kit;
	m_lsHackLevel = Q_max(0, Q_min(2, level));
	m_lsHackTrigAt = 0.0f;
}

bool CCSBot::LsHack(int flag) const
{
	return (m_lsHack & flag) && LsFight();
}

// How long it takes to know an enemy it sees: an aimbot knows at once (a closet cheater
// leaves itself a moment, so it doesn't look like one)
float CCSBot::LsHackReaction(float t) const
{
	if (!LsHack(LS_HK_AIM))
		return t;
	return m_lsHackLevel >= 2 ? 0.0f : (m_lsHackLevel == 1 ? Q_min(t, 0.06f) : Q_min(t, 0.14f));
}

// A wallhacker sees all round it; a closet one keeps to a person's field of view
bool CCSBot::LsHackSeesAround() const
{
	return LsHack(LS_HK_AIM) && m_lsHackLevel >= 1;
}

bool CCSBot::LsHackIgnoresSmoke() const
{
	return LsHack(LS_HK_NOFLASH) || (LsHack(LS_HK_AIM) && m_lsHackLevel >= 1);
}

// The guns a bullet goes through a wall with, and how thick a wall
static float LsHackPierce(CBasePlayerWeapon *pWeapon)
{
	if (!pWeapon)
		return 0.0f;
	switch (pWeapon->m_iId)
	{
	case WEAPON_AWP: return 40.0f;
	case WEAPON_G3SG1: case WEAPON_SG550: case WEAPON_SCOUT: return 30.0f;
	case WEAPON_AK47: case WEAPON_M4A1: case WEAPON_SG552: case WEAPON_AUG: case WEAPON_M249: return 24.0f;
	case WEAPON_FAMAS: case WEAPON_GALIL: return 16.0f;
	case WEAPON_DEAGLE: return 12.0f;
	default: return 0.0f;
	}
}

// One wall between, thin enough for the gun
static bool LsHackThinWall(CCSBot *me, const Vector &from, const Vector &to, float most)
{
	TraceResult in, out;
	UTIL_TraceLine(from, to, ignore_monsters, ignore_glass, me->edict(), &in);
	if (in.flFraction >= 1.0f)
		return false;
	UTIL_TraceLine(to, from, ignore_monsters, ignore_glass, me->edict(), &out);
	if (out.flFraction >= 1.0f || out.fStartSolid)
		return false;
	// (the far side of the same wall: the entry and exit a wall's thickness apart, nothing
	// else on the way)
	return (out.vecEndPos - in.vecEndPos).IsLengthLessThan(most);
}

// Its nearest living enemy, through walls (closer counts more than a wall)
CBasePlayer *CCSBot::LsHackNearestEnemy(float range)
{
	CBasePlayer *best = nullptr;
	float bestDist = range;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *p = UTIL_PlayerByIndex(i);
		if (!p || p == this || !p->IsAlive() || p->m_iTeam == m_iTeam || (p->m_iTeam != TERRORIST && p->m_iTeam != CT))
			continue;
		float d = (p->pev->origin - pev->origin).Length();
		if (d < bestDist)
		{
			bestDist = d;
			best = p;
		}
	}
	return best;
}

// The view straight at a spot, as the engine keeps it (pitch down positive)
static Vector LsHackAngleTo(const Vector &from, const Vector &to)
{
	Vector ang = UTIL_VecToAngles(to - from);
	ang.x = -ang.x;
	ang.x = NormalizeAngle(ang.x);
	ang.y = NormalizeAngle(ang.y);
	ang.z = 0.0f;
	return ang;
}

// Each frame, after the bot has aimed and looked as it would
void CCSBot::LsHackUpkeep()
{
	if (cv_bot_ls_hack_test.string && cv_bot_ls_hack_test.string[0] && gpGlobals->time >= m_lsHackTestAt)
	{
		m_lsHackTestAt = gpGlobals->time + 1.0f;
		char letters[16] = "";
		int level = 1, team = 0;
		sscanf(cv_bot_ls_hack_test.string, "%15s %d %d", letters, &level, &team);
		// (a team given: only that side hacks)
		if (team && team != m_iTeam)
			Q_strlcpy(letters, "-");
		if (m_lsHackLetters[0] == 0 || Q_strcmp(letters, m_lsHackLetters) || level != m_lsHackLevel)
		{
			Q_strlcpy(m_lsHackLetters, letters);
			LsHackSet(letters, level);
		}
	}
	if (!m_lsHack || !LsFight() || !IsAlive() || (pev->flags & FL_FROZEN))
		return;

	const float now = gpGlobals->time;
	const Vector eye = GetEyePosition();
	CBasePlayerWeapon *pWeapon = GetActiveWeapon();
	bool canShoot = pWeapon && !IsReloading() && !IsUsingKnife() && !IsUsingGrenade() && pWeapon->m_iId != WEAPON_C4;

	// aimbot: on the head of the enemy it's fighting, at once (rage), in a frame or two
	// (mixed), or quick but smooth (closet, which now and then takes the body)
	if (LsHack(LS_HK_AIM) && IsAimingAtEnemy() && m_enemy.IsValid() && m_enemy->IsAlive() && IsEnemyVisible())
	{
		CBasePlayer *pEnemy = static_cast<CBasePlayer *>(static_cast<CBaseEntity *>(m_enemy));
		if (pEnemy != m_lsHackAimAt)
		{
			m_lsHackAimAt = pEnemy;
			m_lsHackBody = m_lsHackLevel == 0 && RANDOM_FLOAT(0.0f, 1.0f) < 0.35f;
		}
		Vector spot = pEnemy->pev->origin + pEnemy->pev->view_ofs;
		spot.z -= m_lsHackBody ? 20.0f : 1.0f;
		Vector ang = LsHackAngleTo(eye, spot);
		if (!LsHack(LS_HK_RECOIL))
		{
			// (a script's recoil control: the kick taken straight back off)
			ang.x -= 2.0f * pev->punchangle.x;
			ang.y -= 2.0f * pev->punchangle.y;
		}
		float take = m_lsHackLevel >= 2 ? 1.0f : (m_lsHackLevel == 1 ? 0.6f : 0.3f);
		Vector v = pev->v_angle;
		v.y = NormalizeAngle(v.y + NormalizeAngle(ang.y - v.y) * take);
		v.x = v.x + (ang.x - v.x) * take;
		pev->v_angle = v;
		SetLookAngles(ang.y, ang.x);
		m_lookYawVel = 0.0f;
		m_lookPitchVel = 0.0f;
	}
	else
		m_lsHackAimAt = nullptr;

	// wallhack: the crosshair on the nearest enemy through the walls (a closet cheater only
	// close by, and not so it shows while running), now and then off to where they are, and
	// a CT knows where the bomb is
	if (LsHack(LS_HK_WALL))
	{
		if (!IsAttacking() && !IsEnemyVisible())
		{
			float range = m_lsHackLevel >= 2 ? 3500.0f : (m_lsHackLevel == 1 ? 2000.0f : 900.0f);
			CBasePlayer *pThem = LsHackNearestEnemy(range);
			if (pThem)
			{
				Vector head = pThem->pev->origin + pThem->pev->view_ofs;
				SetLookAt("Loner-Strike: walls", &head, m_lsHackLevel >= 1 ? PRIORITY_HIGH : PRIORITY_MEDIUM, 0.3f, false, 3.0f);

				// wallbang: one wall between, thin enough for the gun, and the crosshair on them
				float pierce = LsHackPierce(pWeapon);
				Vector chest = pThem->pev->origin;
				if (LsHack(LS_HK_BANG) && canShoot && pierce > 0.0f && (chest - eye).IsLengthLessThan(2200.0f)
					&& IsLookingAtPosition(&head, 2.5f) && now >= m_lsHackBangCheckAt)
				{
					m_lsHackBangCheckAt = now + 0.1f;
					if (LsHackThinWall(this, eye, chest, pierce))
					{
						if (now >= m_lsHackBangUntil)
							LsWhy("hack-wallbang");
						m_lsHackBangUntil = now + (m_lsHackLevel >= 2 ? 0.35f : 0.2f);
					}
				}
				if (now < m_lsHackBangUntil && canShoot)
					PrimaryAttack();

				// off to where they are, when it isn't doing anything that matters more
				if (m_lsHackLevel >= 1 && IsHunting() && now >= m_lsHackSeekAt)
				{
					m_lsHackSeekAt = now + RANDOM_FLOAT(6.0f, 12.0f);
					MoveTo(&pThem->pev->origin, FASTEST_ROUTE);
				}
			}
		}

		if (m_iTeam == CT && TheCSBots()->IsBombPlanted() && !GetGameState()->IsPlantedBombLocationKnown())
		{
			CBaseEntity *pThing = nullptr;
			while ((pThing = UTIL_FindEntityByClassname(pThing, "grenade")))
			{
				CGrenade *pBomb = static_cast<CGrenade *>(pThing);
				if (pBomb->m_bIsC4)
				{
					GetGameState()->UpdatePlantedBomb(&pBomb->pev->origin);
					break;
				}
			}
		}
	}

	// triggerbot: an enemy under the crosshair is shot, at once (closet: a human's moment)
	if (LsHack(LS_HK_TRIGGER) && canShoot)
	{
		UTIL_MakeVectors(pev->v_angle + pev->punchangle);
		TraceResult tr;
		UTIL_TraceLine(eye, eye + gpGlobals->v_forward * 8192.0f, dont_ignore_monsters, ignore_glass, edict(), &tr);
		CBaseEntity *pHit = tr.pHit ? CBaseEntity::Instance(tr.pHit) : nullptr;
		bool enemyThere = pHit && pHit->IsPlayer() && pHit->IsAlive() && static_cast<CBasePlayer *>(pHit)->m_iTeam != m_iTeam
			&& (!LsHackIgnoresSmoke() ? !TheCSBots()->IsLineBlockedBySmoke(&eye, &tr.vecEndPos) : true);
		if (enemyThere)
		{
			if (m_lsHackTrigAt <= 0.0f && !IsAttacking())
				LsWhy("hack-trigger");
			if (m_lsHackTrigAt <= 0.0f)
				m_lsHackTrigAt = now + (m_lsHackLevel >= 2 ? 0.0f : (m_lsHackLevel == 1 ? RANDOM_FLOAT(0.02f, 0.06f) : RANDOM_FLOAT(0.07f, 0.15f)));
			if (now >= m_lsHackTrigAt)
				PrimaryAttack();
		}
		else
			m_lsHackTrigAt = 0.0f;
	}
}

// The command it sends: a spinbot's body turns round and round, the movement keys turned with it so it still goes where it means to, and the
// real aim put back for the frame it fires or uses something; no recoil takes the kick off
// first. Returns true when the view sent isn't the bot's own (put back after the move)
bool LsHackDrive(CBasePlayer *bot, Vector *angles, float *forward, float *strafe, unsigned short *buttons)
{
	if (!bot || !bot->IsBot() || !bot->IsAlive())
		return false;
	CCSBot *me = static_cast<CCSBot *>(bot);
	if (me->LsHack(LS_HK_RECOIL))
		me->pev->punchangle = g_vecZero;
	if (!me->LsHack(LS_HK_SPIN) || me->IsUsingLadder() || (me->pev->flags & FL_FROZEN) || (*buttons & (IN_ATTACK | IN_ATTACK2 | IN_USE)))
		return false;

	float rate = me->LsHackLevel() >= 2 ? 1440.0f : 900.0f;
	float spin = me->LsHackSpinStep(rate * g_flBotCommandInterval);
	float delta = NormalizeAngle(spin - angles->y);
	float rad = delta * float(M_PI) / 180.0f;
	float c = cosf(rad), s = sinf(rad);
	float f = *forward, r = *strafe;
	*forward = f * c - r * s;
	*strafe = f * s + r * c;
	angles->y = spin;
	// (not looking at the floor as well, HvH's anti-aim: measured 2026-11-25 it cost a rage kit a
	// third of its fights, 20-9 against 26-5 without the spin)
	return true;
}
