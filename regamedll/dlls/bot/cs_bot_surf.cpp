// Loner-Strike surf: a player's own movement run ahead, to plan and follow surf lines for bots.
// A port of the sim's step (~/loner-strike-sim/surfsim.py, itself pm_shared's: friction, ground
// and air acceleration with the air wish speed capped at 30, gravity in two halves, the four-bump
// fly move clipping off planes), checked against real servers there. The world and solid brush
// entities come from the engine's own hull traces; trigger_teleport, trigger_push and deadly
// trigger_hurt from the map's entities, as the game's touch functions treat them.
//
// ls_surfcheck <x> <y> <z> <yaw> [walk frames]: a fixed run from there (stand, walk off at yaw,
// then ride whatever ramp it lands on along its level line, or let go in the air), printing
// where it is every second - the same run the sim does, to check the two agree.

#include "precompiled.h"
#include <vector>
#include <cstdarg>
#include <cmath>

// (triggers.cpp keeps this to itself)
#ifndef SF_TELEPORT_KEEP_VELOCITY
#define SF_TELEPORT_KEEP_VELOCITY 512
#endif

namespace LsSurf
{

const float DT = 0.01f;				// a frame (the sim's 100 a second)
const float SURF_STOP_EPSILON = 0.1f;	// (pm_shared.h has STOP_EPSILON as a macro)

struct Trigger
{
	Vector lo, hi;
	int kind;						// 0 teleport, 1 push, 2 deadly hurt
	Vector to;						// teleport: where to (the destination's origin)
	bool keep;						// teleport: keeps velocity
	Vector push;					// push: speed * direction
	bool once;						// push: once (adds to velocity) or a base velocity while inside
};

static std::vector<Trigger> s_triggers;

// Exact angles: the bot code's BotCOS/BotSIN are a table in 1.4 degree steps and the engine's
// UTIL_VecToYaw rounds to whole degrees - a surf line needs better than either
static float Cos(float deg) { return float(cos(deg * (M_PI / 180.0))); }
static float Sin(float deg) { return float(sin(deg * (M_PI / 180.0))); }
static float Yaw(float x, float y) { return float(atan2(y, x) * (180.0 / M_PI)); }

// A trigger's own box (absmin/absmax are padded by a unit all round; the engine touches by the
// brush itself)
static void Box(CBaseEntity *pEnt, Trigger *t)
{
	t->lo = pEnt->pev->origin + pEnt->pev->mins;
	t->hi = pEnt->pev->origin + pEnt->pev->maxs;
}

struct Consts
{
	float gravity, accel, airaccel, friction, stopspeed, maxvel, maxspeed;
};

static Consts ReadConsts()
{
	Consts c;
	c.gravity = CVAR_GET_FLOAT("sv_gravity");
	c.accel = CVAR_GET_FLOAT("sv_accelerate");
	c.airaccel = CVAR_GET_FLOAT("sv_airaccelerate");
	c.friction = CVAR_GET_FLOAT("sv_friction");
	c.stopspeed = CVAR_GET_FLOAT("sv_stopspeed");
	c.maxvel = CVAR_GET_FLOAT("sv_maxvelocity");
	c.maxspeed = 250.0f;
	return c;
}

// The map's teleports, pushes and deadly hurts (turned-off ones left out)
static void LoadTriggers()
{
	s_triggers.clear();
	CBaseEntity *pEnt = nullptr;
	while ((pEnt = UTIL_FindEntityByClassname(pEnt, "trigger_teleport")) != nullptr)
	{
		if (pEnt->pev->solid != SOLID_TRIGGER || FStringNull(pEnt->pev->target))
			continue;
		edict_t *pDest = FIND_ENTITY_BY_TARGETNAME(nullptr, STRING(pEnt->pev->target));
		if (FNullEnt(pDest))
			continue;
		Trigger t;
		Box(pEnt, &t); t.kind = 0;
		t.to = VARS(pDest)->origin;
#ifdef REGAMEDLL_ADD
		t.keep = (pEnt->pev->spawnflags & SF_TELEPORT_KEEP_VELOCITY) != 0;
#else
		t.keep = false;
#endif
		t.once = false;
		s_triggers.push_back(t);
	}
	pEnt = nullptr;
	while ((pEnt = UTIL_FindEntityByClassname(pEnt, "trigger_push")) != nullptr)
	{
		if (pEnt->pev->solid != SOLID_TRIGGER)
			continue;
		Trigger t;
		Box(pEnt, &t); t.kind = 1;
		t.push = pEnt->pev->speed * pEnt->pev->movedir;
		t.once = (pEnt->pev->spawnflags & SF_TRIGGER_PUSH_ONCE) != 0;
		t.keep = false;
		s_triggers.push_back(t);
	}
	pEnt = nullptr;
	while ((pEnt = UTIL_FindEntityByClassname(pEnt, "trigger_hurt")) != nullptr)
	{
		if (pEnt->pev->solid != SOLID_TRIGGER || pEnt->pev->dmg < 50.0f)
			continue;
		Trigger t;
		Box(pEnt, &t); t.kind = 2;
		t.keep = t.once = false;
		s_triggers.push_back(t);
	}
}

struct Player
{
	Vector o, v, base;
	bool onground = false, dead = false;
	int teleported = -1;			// the trigger it was just teleported by, this frame
	Vector touched[4];
	int numTouched = 0;
	Consts c;

	static void Trace(const Vector &a, const Vector &b, TraceResult *tr)
	{
		UTIL_TraceHull(a, b, ignore_monsters, human_hull, nullptr, tr);
	}

	static Vector Clip(const Vector &v, const Vector &n)
	{
		float back = DotProduct(v, n);
		Vector out = v - n * back;
		for (int i = 0; i < 3; i++)
		{
			if (out[i] > -SURF_STOP_EPSILON && out[i] < SURF_STOP_EPSILON)
				out[i] = 0.0f;
		}
		return out;
	}

	void Categorize()
	{
		if (v.z > 180.0f)
		{
			onground = false;
			return;
		}
		TraceResult tr;
		Trace(o, o - Vector(0, 0, 2), &tr);
		onground = tr.flFraction < 1.0f && tr.vecPlaneNormal.z >= 0.7f;
		if (onground && !tr.fStartSolid && !tr.fAllSolid)
			o = tr.vecEndPos;
	}

	void FlyMove()
	{
		Vector planes[5];
		int numPlanes = 0;
		Vector primal = v, orig = v;
		float left = DT, allFrac = 0.0f;
		for (int bump = 0; bump < 4; bump++)
		{
			if (v.x == 0.0f && v.y == 0.0f && v.z == 0.0f)
				break;
			TraceResult tr;
			Trace(o, o + v * left, &tr);
			allFrac += tr.flFraction;
			if (tr.fAllSolid)
			{
				v = g_vecZero;
				return;
			}
			if (tr.flFraction > 0.0f)
			{
				o = tr.vecEndPos;
				orig = v;
				numPlanes = 0;
			}
			if (tr.flFraction == 1.0f)
				break;
			if (numTouched < 4)
				touched[numTouched++] = tr.vecPlaneNormal;
			left -= left * tr.flFraction;
			if (numPlanes >= 5)
			{
				v = g_vecZero;
				break;
			}
			planes[numPlanes++] = tr.vecPlaneNormal;
			if (numPlanes == 1 && !onground)
			{
				v = Clip(orig, planes[0]);
				orig = v;
			}
			else
			{
				int i;
				for (i = 0; i < numPlanes; i++)
				{
					v = Clip(orig, planes[i]);
					int j;
					for (j = 0; j < numPlanes; j++)
					{
						if (j != i && DotProduct(v, planes[j]) < 0.0f)
							break;
					}
					if (j == numPlanes)
						break;
				}
				if (i == numPlanes)
				{
					if (numPlanes != 2)
					{
						v = g_vecZero;
						break;
					}
					Vector dir = CrossProduct(planes[0], planes[1]);
					v = dir * DotProduct(dir, v);
				}
				if (DotProduct(v, primal) <= 0.0f)
				{
					v = g_vecZero;
					break;
				}
			}
		}
		if (allFrac == 0.0f)
			v = g_vecZero;
	}

	void Accelerate(const Vector &wishdir, float wishspeed, float accel, bool air)
	{
		float wsp = air ? Q_min(wishspeed, 30.0f) : wishspeed;
		float add = wsp - DotProduct(v, wishdir);
		if (add <= 0.0f)
			return;
		float a = Q_min(accel * wishspeed * DT, add);
		v = v + wishdir * a;
	}

	bool InBox(const Trigger &t) const
	{
		return o.x + 16.0f > t.lo.x && o.x - 16.0f < t.hi.x && o.y + 16.0f > t.lo.y && o.y - 16.0f < t.hi.y
			&& o.z + 36.0f > t.lo.z && o.z - 36.0f < t.hi.z;
	}

	void Triggers()
	{
		teleported = -1;
		for (const Trigger &t : s_triggers)
		{
			if (t.kind == 2 && InBox(t))
			{
				dead = true;
				return;
			}
		}
		for (size_t k = 0; k < s_triggers.size(); k++)
		{
			const Trigger &t = s_triggers[k];
			if (t.kind == 0 && InBox(t))
			{
				o = t.to + Vector(0, 0, 36.0f + 1.0f);
				if (!t.keep)
					v = g_vecZero;
				onground = false;
				teleported = int(k);
				return;
			}
		}
		const Trigger *push = nullptr;
		for (const Trigger &t : s_triggers)
		{
			if (t.kind == 1 && InBox(t))
			{
				if (t.once)
					v = v + t.push;
				else
					push = &t;
				break;
			}
		}
		// (left the push: its momentum kept, as the engine does)
		if (!push && (base.x != 0.0f || base.y != 0.0f || base.z != 0.0f))
			v = v + base * (1.0f + DT * 0.5f);
		base = push ? push->push : g_vecZero;
	}

	// One frame with this command (yaw in degrees, forward and side move)
	void Step(float yaw, float fmove, float smove)
	{
		numTouched = 0;
		Categorize();
		v.z -= c.gravity * 0.5f * DT;
		if (onground)
		{
			v.z = 0.0f;
			float sp = v.Length2D();
			if (sp > 0.1f)
			{
				float drop = Q_max(sp, c.stopspeed) * c.friction * DT;
				float ns = Q_max(0.0f, sp - drop) / sp;
				v.x *= ns;
				v.y *= ns;
			}
		}
		for (int i = 0; i < 3; i++)
			v[i] = Q_max(-c.maxvel, Q_min(c.maxvel, v[i]));
		float cy = Cos(yaw), sy = Sin(yaw);
		Vector wv(cy * fmove + sy * smove, sy * fmove - cy * smove, 0.0f);
		float ws = wv.Length2D();
		Vector wd = ws > 0.0f ? wv * (1.0f / ws) : g_vecZero;
		ws = Q_min(ws, c.maxspeed);
		Accelerate(wd, ws, onground ? c.accel : c.airaccel, !onground);
		v = v + base;
		FlyMove();
		v = v - base;
		Categorize();
		if (!onground)
			v.z -= c.gravity * 0.5f * DT;
		else
			v.z = 0.0f;
		Triggers();
	}

	// The ramp it's touching (0.05 < normal z < 0.7): this frame's contacts, or a probe
	bool Ramp(Vector *n) const
	{
		for (int i = 0; i < numTouched; i++)
		{
			if (touched[i].z > 0.05f && touched[i].z < 0.7f)
			{
				*n = touched[i];
				return true;
			}
		}
		static const Vector probes[5] = { Vector(0, 0, -1), Vector(1, 0, -0.5f), Vector(-1, 0, -0.5f), Vector(0, 1, -0.5f), Vector(0, -1, -0.5f) };
		for (int k = 0; k < 5; k++)
		{
			TraceResult tr;
			Trace(o, o + probes[k] * 20.0f, &tr);
			if (tr.flFraction < 1.0f && tr.vecPlaneNormal.z > 0.05f && tr.vecPlaneNormal.z < 0.7f)
			{
				*n = tr.vecPlaneNormal;
				return true;
			}
		}
		return false;
	}
};

// The strafe that pushes into a ramp, facing yaw: +400 or -400
static float IntoSide(float yaw, const Vector &n)
{
	float rx = Sin(yaw), ry = -Cos(yaw);
	return (rx * -n.x + ry * -n.y > 0.0f) ? 400.0f : -400.0f;
}

static float VelYaw(const Player &p, float fallback)
{
	return p.v.Length2D() > 1.0f ? Yaw(p.v.x, p.v.y) : fallback;
}

// A move (the sim's act): on a ramp, ride it facing along its level line (the way it's going)
// turned 'angle' degrees down the face (dive) or up it, strafing into it; in the air, turn
// 'angle' degrees a decision (strafing into the turn) or, 'free', let go
enum MoveKind { RIDE, TURN, FREE };
static void Act(const Player &p, bool onRamp, const Vector &n, MoveKind kind, float angle, float lastYaw, float *yaw, float *fmove, float *smove)
{
	float vy = VelYaw(p, lastYaw);
	*fmove = 0.0f;
	if (onRamp)
	{
		float hl = Q_sqrt(n.x * n.x + n.y * n.y);
		float nx = n.x / hl, ny = n.y / hl;
		float rx = -ny, ry = nx;
		if (p.v.x * rx + p.v.y * ry < 0.0f)
		{
			rx = -rx;
			ry = -ry;
		}
		float ly = Yaw(rx, ry);
		float oy = Yaw(nx, ny);
		float d = NormalizeAngle(oy - ly);
		*yaw = ly + (d >= 0.0f ? 1.0f : -1.0f) * angle;
		*smove = IntoSide(*yaw, n);
		return;
	}
	if (kind == FREE || p.v.Length2D() < 50.0f)
	{
		*yaw = vy;
		*smove = 0.0f;
		return;
	}
	float lead = angle == 0.0f ? 0.0f : (angle > 0.0f ? Q_min(3.0f, angle) : -Q_min(3.0f, -angle));
	*yaw = vy + lead;
	*smove = angle == 0.0f ? 0.0f : (angle > 0.0f ? -400.0f : 400.0f);
}

static void Say(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	SERVER_PRINT(buf);
}

} // namespace LsSurf

void LsSurfCheck_f()
{
	using namespace LsSurf;
	if (CMD_ARGC() < 5)
	{
		Say("ls_surfcheck <x> <y> <z> <yaw> [walk frames]\n");
		return;
	}
	LoadTriggers();
	Player p;
	p.c = ReadConsts();
	p.o = Vector(Q_atof(CMD_ARGV(1)), Q_atof(CMD_ARGV(2)), Q_atof(CMD_ARGV(3)));
	p.v = p.base = g_vecZero;
	float yaw = Q_atof(CMD_ARGV(4));
	int walk = CMD_ARGC() > 5 ? Q_atoi(CMD_ARGV(5)) : 400;
	// (a bad number would hang the angle tables' wrap-around loop; a huge walk, the server)
	if (!std::isfinite(yaw) || !std::isfinite(p.o.x) || !std::isfinite(p.o.y) || !std::isfinite(p.o.z))
	{
		Say("ls_surfcheck: numbers only\n");
		return;
	}
	yaw = fmodf(yaw, 360.0f);
	walk = Q_max(0, Q_min(walk, 2000));
	Say("ls_surfcheck: %d triggers; gravity %g accel %g airaccel %g friction %g stopspeed %g maxvel %g\n",
		int(s_triggers.size()), p.c.gravity, p.c.accel, p.c.airaccel, p.c.friction, p.c.stopspeed, p.c.maxvel);
	for (int i = 0; i < 50; i++)
		p.Step(yaw, 0, 0);
	int f = 0;
	for (; f < walk && (p.onground || f == 0); f++)
		p.Step(yaw, 250.0f, 0.0f);
	Say("  walked %d frames: (%.1f %.1f %.1f)\n", f, p.o.x, p.o.y, p.o.z);
	float last = yaw;
	for (int k = 1; k <= 500; k++)
	{
		Vector n;
		bool ramp = p.Ramp(&n);
		float y, fm, sm;
		Act(p, ramp, n, ramp ? RIDE : FREE, 0.0f, last, &y, &fm, &sm);
		last = y;
		p.Step(y, fm, sm);
		if (k % 100 == 0 || p.teleported >= 0 || p.dead)
			Say("  %.2f s: (%.1f %.1f %.1f) v (%.1f %.1f %.1f) ground %d%s%s\n", k * DT, p.o.x, p.o.y, p.o.z, p.v.x, p.v.y, p.v.z,
				int(p.onground), p.teleported >= 0 ? " TELEPORTED" : "", p.dead ? " DEAD" : "");
		if (p.teleported >= 0 || p.dead)
			break;
	}
}
