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
#include <map>
#include <algorithm>
#include <chrono>

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
	float dt = DT;					// a step: the sim's 1/100 s, or a bot command's 1/30

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
		float left = dt, allFrac = 0.0f;
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
		float a = Q_min(accel * wishspeed * dt, add);
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
			v = v + base * (1.0f + dt * 0.5f);
		base = push ? push->push : g_vecZero;
	}

	// One frame with this command (yaw in degrees, forward and side move)
	void Step(float yaw, float fmove, float smove)
	{
		numTouched = 0;
		Categorize();
		v.z -= c.gravity * 0.5f * dt;
		if (onground)
		{
			v.z = 0.0f;
			float sp = v.Length2D();
			if (sp > 0.1f)
			{
				float drop = Q_max(sp, c.stopspeed) * c.friction * dt;
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
			v.z -= c.gravity * 0.5f * dt;
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

// ---- surf lines for bots ------------------------------------------------------------------
//
// bot_ls_surf 1 (Surf mode's cfg): the brain steers surf bots (gm_botsurf stands aside).
// bot_ls_surf 2: and says how the exploring's going every 10 s.
//
// The map is explored a little each frame (Go-Explore, as the sim found works): an archive of
// the best state reached in every 128-unit place (by height too: stacked stages apart), from each
// spawn area and every teleport's destination; again and again a place seldom tried is gone back
// to and short random lines flown on from there, moves held a few tenths of a second. A line
// counts only if copies of it a few units off, a little slower or faster, with some noise in the
// aim, all live through it too (lines that only work flown exactly don't survive a real bot).
// Each new place keeps the line that got there. Over open air a line flies on but nothing's kept.
//
// A bot in the air or on a ramp picks a place further on from the ones near it (somewhere new, or
// nearest an enemy), follows the line there (the move of the stretch of it it's on), and when it's
// drifted off by more than a stride, plans a short hop back onto it from where it really is. No
// line: it rides like gm_botsurf (along a ramp diving a little; in the air along its velocity).
// On a platform a couple of seconds: walks off toward the nearest ramp below. While it has an
// enemy it keeps its own aim (it can shoot): the move is turned to match.

cvar_t cv_bot_ls_surf = { "bot_ls_surf", "0", FCVAR_SERVER, 0.0f, nullptr };

static double NowMs()
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace LsSurf
{

const int CHUNK = 3;					// commands a move is held at the least
const float CELL = 128.0f;
const int ROBUST = 3;					// noisy copies every line must keep alive
const float NOISE = 1.0f;				// their aim noise, degrees a command
const int MAX_NODES = 30000;
const int MAX_ITERS = 4000;
const float HEIGHT_W = 1.5f, SPEED_W = 0.5f;

static float s_cmdDt = 0.04f;			// a bot command's time, as measured (25-30 a second)

// the moves: 0-9 ride a ramp at an angle down (+) or up (-) its face, 10-16 turn in the air, 17 let go
static const float RIDE_ANG[10] = { -20.0f, -10.0f, -5.0f, 0.0f, 4.0f, 8.0f, 12.0f, 18.0f, 25.0f, 35.0f };
static const float TURN_ANG[7] = { -12.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f, 12.0f };
const int MOVE_FREE = 17;

static void ApplyMove(const Player &p, int m, float lastYaw, float *yaw, float *fm, float *sm)
{
	Vector n;
	bool ramp = p.Ramp(&n);
	MoveKind kind = m < 10 ? RIDE : m < 17 ? TURN : FREE;
	float ang = m < 10 ? RIDE_ANG[m] : m < 17 ? TURN_ANG[m - 10] : 0.0f;
	Act(p, ramp, n, kind, ang, lastYaw, yaw, fm, sm);
}

// One move, held a chunk; false if it died or was teleported on the way
static bool StepChunk(Player &p, int m, float *lastYaw, float noise)
{
	for (int f = 0; f < CHUNK; f++)
	{
		float y, fm, sm;
		ApplyMove(p, m, *lastYaw, &y, &fm, &sm);
		*lastYaw = y;
		p.Step(noise > 0.0f ? y + RANDOM_FLOAT(-noise, noise) : y, fm, sm);
		if (p.dead || p.teleported >= 0)
			return false;
	}
	return true;
}

struct Node
{
	int parent;				// the place it was flown from (-1: a start)
	int firstAct, numActs;	// its moves from there, in s_acts
	float t;				// seconds from its start
	Player st;				// where it was, how fast
	bool landed;			// came to rest there (a place to stop)
};

struct Cell
{
	int node;
	int visits;
};

static std::vector<Node> s_nodes;
static std::vector<unsigned char> s_acts;
// where each move started from (position, velocity): a path is these, copied
struct Sample
{
	Vector o, v;
	unsigned char move;
};
static std::vector<Sample> s_samples;
static std::vector<std::vector<int>> s_children;
static std::map<long long, Cell> s_cells;
static std::vector<Vector> s_roots;			// start points: spawn areas, teleport destinations
static int s_rootsDone = 0;
static int s_iters = 0;
static float s_floorZ = -4096.0f;
static char s_map[64] = "";
static Consts s_consts;
static float s_constsAt = 0.0f;
static int s_replansLeft = 0, s_buildsLeft = 0, s_picksLeft = 0;
static double s_driveMs = 0.0;			// spent steering bots this frame (taken off the exploring)

// ---- a bot's surfing


struct SurfBot
{
	std::vector<Sample> path;		// a chunk apart
	int idx = 0;
	std::vector<unsigned char> plan;	// a hop back onto the line: moves a chunk each
	int planStep = 0;
	float lastYaw = 0.0f;
	float nextPick = 0.0f, nextReplan = 0.0f;
	float groundSince = 0.0f, exitYaw = 0.0f, exitUntil = 0.0f, exitNext = 0.0f;
	Vector lastPos;
	bool driven = false;		// steered last command (let go: the leftover move is cleared)
};
static SurfBot s_bots[33];

static long long Key(const Vector &o)
{
	long long x = (long long)floorf(o.x / CELL + 0.5f), y = (long long)floorf(o.y / CELL + 0.5f), z = (long long)floorf(o.z / 256.0f);
	return ((x + 1000) * 2000 + (y + 1000)) * 100 + (z + 50);
}

static float Energy(const Player &p)
{
	return HEIGHT_W * (p.o.z - s_floorZ) + SPEED_W * p.v.Length2D();
}

// Something solid under it short of the floor (not over open air, falling to it)
static bool Under(const Player &p)
{
	if (p.o.z <= s_floorZ + 20.0f)
		return false;
	TraceResult tr;
	Player::Trace(p.o, Vector(p.o.x, p.o.y, s_floorZ + 20.0f), &tr);
	return tr.flFraction < 1.0f && !tr.fStartSolid;
}

// A player as the explorer flies it: the map's physics at a bot's command rate
static Player NewPlayer()
{
	Player p;
	p.c = s_consts;
	p.dt = s_cmdDt;
	p.v = p.base = g_vecZero;
	return p;
}

// The nearest way off a platform onto a ramp below (gm_botsurf's walk-off): 16 ways out, as far
// as 600 units, looking down past the edge for a ramp
static bool FindExit(const Vector &o, float *yaw)
{
	float best = 99999.0f;
	bool found = false;
	TraceResult tr;
	for (int i = 0; i < 16; i++)
	{
		float a = i * 22.5f;
		Vector dir(Cos(a), Sin(a), 0.0f);
		for (int step = 1; step <= 12; step++)
		{
			float d = step * 50.0f;
			Vector to = o + dir * d;
			Player::Trace(o, to, &tr);
			if (tr.flFraction < 1.0f)
				break;
			Player::Trace(to, to - Vector(0, 0, 2000.0f), &tr);
			if (tr.flFraction >= 1.0f)
				break;
			if (tr.vecPlaneNormal.z >= 0.7f && tr.flFraction * 2000.0f < 40.0f)
				continue;
			if (tr.vecPlaneNormal.z > 0.05f && tr.vecPlaneNormal.z < 0.7f && d < best)
			{
				best = d;
				found = true;
				*yaw = a;
			}
			break;
		}
	}
	return found;
}

static void AddRoot(const Vector &at)
{
	for (const Vector &r : s_roots)
	{
		if ((r - at).Length() < 384.0f)
			return;
	}
	if (s_roots.size() < 32)
		s_roots.push_back(at);
}

// A start: settle there; on a platform, walk off the way FindExit says. The state once moving
// in the air or on a ramp (a teleport on the way: its destination's another start)
static bool RootState(Vector at, Player *out)
{
	Player p = NewPlayer();
	p.o = at;
	for (int i = 0; i < 15 && !p.dead && p.teleported < 0; i++)
		p.Step(0.0f, 0.0f, 0.0f);
	if (p.dead)
		return false;
	if (p.teleported >= 0)
	{
		AddRoot(p.o);
		return false;
	}
	if (!p.onground)
	{
		*out = p;
		return true;
	}
	float yaw;
	if (!FindExit(p.o, &yaw))
		return false;
	for (int i = 0; i < 300 && p.onground; i++)
		p.Step(yaw, 250.0f, 0.0f);
	if (p.teleported >= 0 && !p.dead)
		AddRoot(p.o);
	if (p.onground || p.dead || p.teleported >= 0)
		return false;
	*out = p;
	return true;
}

static int AddNode(int parent, int firstAct, int numActs, float t, const Player &st, bool landed)
{
	Node n;
	n.parent = parent; n.firstAct = firstAct; n.numActs = numActs; n.t = t; n.st = st; n.landed = landed;
	s_nodes.push_back(n);
	s_children.push_back(std::vector<int>());
	int id = int(s_nodes.size()) - 1;
	if (parent >= 0)
		s_children[parent].push_back(id);
	return id;
}

// a rollout under way (exploring stops and goes on mid-rollout, to keep to its time)
static struct Rollout
{
	bool active;
	int from, rolloutsLeft;
	Player q, twins[ROBUST];
	float qYaw, tw[ROBUST], t;
	int firstAct, numActs, m, hold, chunk;
} s_roll;

static void Reset()
{
	s_nodes.clear(); s_acts.clear(); s_samples.clear(); s_children.clear(); s_cells.clear(); s_roots.clear();
	s_rootsDone = 0; s_iters = 0;
	s_roll.active = false; s_roll.rolloutsLeft = 0;
	for (int i = 0; i < 33; i++)
		s_bots[i] = SurfBot();
	Q_strncpy(s_map, STRING(gpGlobals->mapname), sizeof(s_map) - 1);
	s_map[sizeof(s_map) - 1] = '\0';
	LoadTriggers();
	s_consts = ReadConsts();
	s_constsAt = gpGlobals->time;
	// the map's extent (the world entity's own bounds are always zero): every brush entity and spawn
	Vector wlo(1e9f, 1e9f, 1e9f), whi(-1e9f, -1e9f, -1e9f);
	float lowestSpawn = 1e9f;
	for (int i = 1; i < gpGlobals->maxEntities; i++)
	{
		edict_t *pEdict = INDEXENT(i);
		if (!pEdict || pEdict->free || FStringNull(pEdict->v.model) || STRING(pEdict->v.model)[0] != '*')
			continue;
		for (int k = 0; k < 3; k++)
		{
			wlo[k] = Q_min(wlo[k], pEdict->v.absmin[k]);
			whi[k] = Q_max(whi[k], pEdict->v.absmax[k]);
		}
	}
	const char *spawnNames[2] = { "info_player_start", "info_player_deathmatch" };
	for (int k = 0; k < 2; k++)
	{
		CBaseEntity *pEnt = nullptr;
		while ((pEnt = UTIL_FindEntityByClassname(pEnt, spawnNames[k])) != nullptr)
		{
			lowestSpawn = Q_min(lowestSpawn, pEnt->pev->origin.z);
			for (int j = 0; j < 3; j++)
			{
				wlo[j] = Q_min(wlo[j], pEnt->pev->origin[j]);
				whi[j] = Q_max(whi[j], pEnt->pev->origin[j]);
			}
		}
	}
	if (wlo.x > whi.x)
	{
		wlo = Vector(-4096, -4096, -4096);
		whi = Vector(4096, 4096, 4096);
	}
	float area = Q_max(1.0f, (whi.x - wlo.x) * (whi.y - wlo.y));
	// the floor: a teleport or deadly hurt under most of the map, below the spawns (the jail, a
	// kill floor); else the bottom of the map
	s_floorZ = wlo.z - 64.0f;
	for (const Trigger &t : s_triggers)
	{
		if (t.kind != 1 && t.hi.z < lowestSpawn && (t.hi.x - t.lo.x) * (t.hi.y - t.lo.y) > 0.25f * area)
			s_floorZ = Q_max(s_floorZ, t.hi.z);
	}
	// starts: every spawn area, every teleport's destination above the floor
	for (int k = 0; k < 2; k++)
	{
		CBaseEntity *pEnt = nullptr;
		while ((pEnt = UTIL_FindEntityByClassname(pEnt, spawnNames[k])) != nullptr)
			AddRoot(pEnt->pev->origin);
	}
	for (const Trigger &t : s_triggers)
	{
		if (t.kind == 0 && t.to.z > s_floorZ + 64.0f)
			AddRoot(t.to + Vector(0, 0, 37.0f));
	}
}

static Cell *PickCell()
{
	float total = 0.0f;
	// (a place it came to rest: nothing to fly on from there - a rollout would just stand)
	auto weight = [](const Cell &c) { const Node &n = s_nodes[c.node]; return n.landed ? 0.0f : (1.0f / Q_sqrt(1.0f + c.visits)) * (1.0f + Q_max(0.0f, Energy(n.st)) / 3000.0f); };
	for (auto &kv : s_cells)
		total += weight(kv.second);
	if (total <= 0.0f)
		return nullptr;
	float pick = RANDOM_FLOAT(0.0f, total);
	Cell *c = nullptr;
	for (auto &kv : s_cells)
	{
		if (weight(kv.second) <= 0.0f)
			continue;
		pick -= weight(kv.second);
		c = &kv.second;
		if (pick <= 0.0f)
			break;
	}
	return c;
}

static void StartRollout()
{
	Rollout &r = s_roll;
	r.q = s_nodes[r.from].st;
	r.q.dt = s_cmdDt;
	r.qYaw = 0.0f;
	for (int k = 0; k < ROBUST; k++)
	{
		r.twins[k] = r.q;
		r.tw[k] = 0.0f;
		// (a little slower or faster - the gun in hand sets the top speed - and the command rate a
		// touch off)
		r.twins[k].c.maxspeed = RANDOM_FLOAT(200.0f, 260.0f);

		// (a few units off - not inside the ramp it's pressed against: a copy stuck in it would
		// never die and pass anything)
		Vector at = r.q.o + Vector(RANDOM_FLOAT(-3.0f, 3.0f), RANDOM_FLOAT(-3.0f, 3.0f), 0.0f);
		TraceResult tr;
		Player::Trace(at, at, &tr);
		if (!tr.fStartSolid && !tr.fAllSolid)
			r.twins[k].o = at;
	}
	r.t = s_nodes[r.from].t;
	r.firstAct = (int)s_acts.size();
	r.numActs = 0;
	r.m = -1;
	r.hold = 0;
	r.chunk = 0;
	r.active = true;
}

// One chunk of the rollout under way; false when it's over
static bool RolloutChunk()
{
	Rollout &r = s_roll;
	Vector n;
	bool ramp = r.q.Ramp(&n);
	if (r.hold <= 0 || r.m < 0 || (r.m < 10) != ramp)
	{
		r.m = ramp ? RANDOM_LONG(0, 9) : RANDOM_LONG(10, MOVE_FREE);
		r.hold = RANDOM_LONG(3, 8);
	}
	r.hold--;
	Sample smp;
	smp.o = r.q.o; smp.v = r.q.v; smp.move = (unsigned char)r.m;
	s_samples.push_back(smp);
	s_acts.push_back((unsigned char)r.m);
	r.numActs++;
	r.t += CHUNK * r.q.dt;
	if (!StepChunk(r.q, r.m, &r.qYaw, 0.0f))
	{
		// a teleport somewhere that isn't the floor: a new start there
		if (r.q.teleported >= 0 && !r.q.dead && r.q.o.z > s_floorZ + 64.0f)
			AddRoot(r.q.o);
		return false;
	}
	for (int k = 0; k < ROBUST; k++)
	{
		// (a real bot's command time wanders about the average: a little each move)
		r.twins[k].dt = r.q.dt * RANDOM_FLOAT(0.9f, 1.1f);
		if (!StepChunk(r.twins[k], r.m, &r.tw[k], NOISE) || (r.twins[k].o - r.q.o).Length() > 64.0f)
			return false;
	}
	// over open air: flies on, nothing kept
	if (!Under(r.q))
		return ++r.chunk < 20;
	for (int k = 0; k < ROBUST; k++)
	{
		if (!Under(r.twins[k]) && r.q.v.z < -200.0f)
			return false;
	}
	long long key = Key(r.q.o);
	bool landed = r.q.onground && r.q.v.Length2D() < 20.0f;
	auto it = s_cells.find(key);
	if (it == s_cells.end() || Energy(r.q) > Energy(s_nodes[it->second.node].st) + 50.0f || (landed && !s_nodes[it->second.node].landed))
	{
		int id = AddNode(r.from, r.firstAct, r.numActs, r.t, r.q, landed);
		if (it == s_cells.end())
			s_cells[key] = { id, 0 };
		else
			it->second.node = id;
	}
	if (landed)
		return false;
	return ++r.chunk < 20;
}

// Explore for up to budget ms
static void Explore(double budgetMs)
{
	double start = NowMs();
	// new starts first
	while (s_rootsDone < (int)s_roots.size())
	{
		Player st;
		if (RootState(s_roots[s_rootsDone], &st))
		{
			int id = AddNode(-1, 0, 0, 0.0f, st, false);
			long long k = Key(st.o);
			if (s_cells.find(k) == s_cells.end())
				s_cells[k] = { id, 0 };
		}
		s_rootsDone++;
		if (NowMs() - start > budgetMs)
			return;
	}
	if (s_cells.empty())
		return;
	while (NowMs() - start < budgetMs)
	{
		if (!s_roll.active)
		{
			if (s_roll.rolloutsLeft <= 0)
			{
				if (s_iters >= MAX_ITERS || (int)s_nodes.size() >= MAX_NODES)
					return;
				Cell *c = PickCell();
				if (!c)
					return;
				s_iters++;
				c->visits++;
				s_roll.from = c->node;
				s_roll.rolloutsLeft = 4;
			}
			s_roll.rolloutsLeft--;
			StartRollout();
		}
		if (!RolloutChunk())
			s_roll.active = false;
	}
}

// ---- following ------------------------------------------------------------------------------

// The archive node nearest this state and going its way, with places on from it
static int NearestNode(const Vector &o, const Vector &v)
{
	int best = -1;
	float bestD = 160.0f;
	float sp = v.Length2D();
	for (int i = 0; i < (int)s_nodes.size(); i++)
	{
		if (s_children[i].empty())
			continue;
		const Player &st = s_nodes[i].st;
		float d = (st.o - o).Length();
		if (d >= bestD)
			continue;
		// (the same way: a line the other way along a ramp is no use)
		float sp2 = st.v.Length2D();
		if (sp > 100.0f && sp2 > 100.0f && (st.v.x * v.x + st.v.y * v.y) / (sp * sp2) < 0.7f)
			continue;
		best = i;
		bestD = d;
	}
	return best;
}

// A place further on from 'from' to head for: nearest the enemy it's after, else somewhere new to
// it - three to fifteen seconds on (breadth first: every way on from here gets a look)
static int PickTarget(int from, const Vector *enemy)
{
	std::vector<int> queue(1, from);
	int best = -1;
	float bestScore = -1e9f;
	float t0 = s_nodes[from].t;
	for (size_t q = 0; q < queue.size() && q < 3000; q++)
	{
		int n = queue[q];
		float dt = s_nodes[n].t - t0;
		if (dt < 15.0f)
		{
			for (int c : s_children[n])
				queue.push_back(c);
		}
		if (dt < 3.0f)
			continue;
		float score = enemy ? -(s_nodes[n].st.o - *enemy).Length() : dt + RANDOM_FLOAT(0.0f, 8.0f) + (s_nodes[n].landed ? 3.0f : 0.0f);
		if (score > bestScore)
		{
			bestScore = score;
			best = n;
		}
	}
	return best;
}

// The path from node 'from' to node 'to' (a descendant): the samples its moves were flown from
static void BuildPath(SurfBot &b, int from, int to)
{
	std::vector<int> chain;
	for (int n = to; n != from && n >= 0; n = s_nodes[n].parent)
		chain.push_back(n);
	b.path.clear();
	b.idx = 0;
	b.plan.clear();
	for (int c = (int)chain.size() - 1; c >= 0; c--)
	{
		const Node &n = s_nodes[chain[c]];
		for (int a = 0; a < n.numActs; a++)
			b.path.push_back(s_samples[n.firstAct + a]);
	}
	// (and where it ends)
	if (!chain.empty())
	{
		Sample end;
		end.o = s_nodes[to].st.o; end.v = s_nodes[to].st.v; end.move = MOVE_FREE;
		b.path.push_back(end);
	}
}

// A short hop back onto the line: a small beam search from where it really is to the line's point
// a little ahead (position, and velocity a bit). Nothing found: the best part-way one
static void Replan(SurfBot &b, const Player &me, const Sample &target)
{
	static const int RAMP_MOVES[5] = { 1, 3, 5, 7, 9 }, AIR_MOVES[4] = { 11, 13, 15, MOVE_FREE };
	struct Beam { Player p; float yaw; unsigned char moves[4]; int n; float score; };
	std::vector<Beam> beams(1);
	beams[0].p = me; beams[0].yaw = b.lastYaw; beams[0].n = 0; beams[0].score = 1e9f;
	const int depth = 4, width = 3;
	for (int d = 0; d < depth; d++)
	{
		std::vector<Beam> next;
		for (const Beam &bm : beams)
		{
			Vector n;
			bool ramp = bm.p.Ramp(&n);
			const int *moves = ramp ? RAMP_MOVES : AIR_MOVES;
			int count = ramp ? 5 : 4;
			for (int k = 0; k < count; k++)
			{
				Beam x = bm;
				if (!StepChunk(x.p, moves[k], &x.yaw, 0.0f))
					continue;
				x.moves[d] = (unsigned char)moves[k];
				x.n = d + 1;
				x.score = (x.p.o - target.o).Length() + 0.3f * (x.p.v - target.v).Length();
				next.push_back(x);
			}
		}
		if (next.empty())
			break;
		std::sort(next.begin(), next.end(), [](const Beam &a, const Beam &c) { return a.score < c.score; });
		if ((int)next.size() > width)
			next.resize(width);
		beams = next;
	}
	if (beams[0].n > 0)
	{
		b.plan.assign(beams[0].moves, beams[0].moves + beams[0].n);
		b.planStep = 0;
	}
}

// Where on its path the bot is: the stretch between samples it's nearest (a sample a chunk apart
// is 100+ units at speed), and how far off it
static void Locate(SurfBot &b, const Vector &o, float *off)
{
	int lo = Q_max(0, b.idx - 2), hi = Q_min((int)b.path.size() - 2, b.idx + 8);
	*off = 1e9f;
	int at = b.idx;
	for (int i = lo; i <= hi; i++)
	{
		Vector a = b.path[i].o, seg = b.path[i + 1].o - a;
		float len2 = float(DotProduct(seg, seg));
		float k = len2 > 0.01f ? Q_max(0.0f, Q_min(1.0f, float(DotProduct(o - a, seg)) / len2)) : 0.0f;
		float d = (a + seg * k - o).Length();
		if (d < *off)
		{
			*off = d;
			at = i;
		}
	}
	b.idx = at;
}

} // namespace LsSurf

// The surf maps' exploring, a little each frame (CCSBotManager::StartFrame)
void LsSurfStep()
{
	using namespace LsSurf;
	if (cv_bot_ls_surf.value <= 0.0f || !gpGlobals->mapname)
		return;
	if (Q_strcmp(s_map, STRING(gpGlobals->mapname)) != 0)
		Reset();
	// (the physics changed - a mode's cfg ran after the map started: explored again with it)
	if (gpGlobals->time - s_constsAt > 1.0f || gpGlobals->time < s_constsAt)
	{
		s_constsAt = gpGlobals->time;
		Consts c = ReadConsts();
		if (c.gravity != s_consts.gravity || c.airaccel != s_consts.airaccel || c.accel != s_consts.accel || c.friction != s_consts.friction)
			Reset();
	}
	static double s_lastFrame = 0.0;
	double nowMs = NowMs(), since = s_lastFrame > 0.0 ? nowMs - s_lastFrame : 10.0;
	s_lastFrame = nowMs;
	Explore(Q_max(0.3, Q_min(3.0, 0.25 * since) - s_driveMs));
	s_driveMs = 0.0;
	s_replansLeft = 1;
	s_buildsLeft = 1;
	s_picksLeft = 2;
	static float s_reportAt = 0.0f;
	if (cv_bot_ls_surf.value >= 2.0f && (gpGlobals->time >= s_reportAt || gpGlobals->time < s_reportAt - 20.0f))
	{
		s_reportAt = gpGlobals->time + 10.0f;
		int following = 0, alive = 0;
		for (int i = 1; i <= 32; i++)
		{
			if (!s_bots[i].path.empty())
				following++;
			if (s_bots[i].driven)
				alive++;
		}
		Say("LSSURF %s: floor %.0f, command %.3f s; %d starts, %d places, %d lines kept, %d tries; steering %d, %d following a line\n",
			s_map, s_floorZ, s_cmdDt, (int)s_roots.size(), (int)s_cells.size(), (int)s_nodes.size(), s_iters, alive, following);
	}
}

// A surf bot's move this command, if it steers it (LsSurfDrive)
static bool LsSurfSteer(CBasePlayer *bot, LsSurf::SurfBot &b, float msec, float *forward, float *strafe, unsigned short *buttons)
{
	using namespace LsSurf;
	float now = gpGlobals->time;
	Player me;
	me.c = s_consts;
	me.c.maxspeed = bot->pev->maxspeed > 0.0f ? bot->pev->maxspeed : 250.0f;
	me.dt = msec > 0.0f ? msec / 1000.0f : s_cmdDt;
	me.o = bot->pev->origin;
	me.v = bot->pev->velocity;
	me.base = bot->pev->basevelocity;
	me.onground = (bot->pev->flags & FL_ONGROUND) != 0;
	Vector n;
	bool ramp = !me.onground && me.Ramp(&n);
	float speed = me.v.Length2D();
	int move = -1;

	// a teleport (it moved far in a command): the line's gone, pick again now
	if ((me.o - b.lastPos).Length() > 300.0f && !b.path.empty())
	{
		b.path.clear();
		b.plan.clear();
		b.nextPick = now;
	}
	b.lastPos = me.o;

	// on the ground: at speed it's crossing a landing (on with the line); slow, it's on a platform:
	// a couple of seconds, then walk off toward the nearest ramp below
	if (me.onground && now < b.exitUntil)
	{
		// (walking off: on until it's off the edge, however fast)
		b.path.clear();
		b.plan.clear();
		bot->pev->v_angle = Vector(0, b.exitYaw, 0);
		*forward = 250.0f;
		*strafe = 0.0f;
		*buttons = (*buttons | IN_FORWARD) & ~(IN_BACK | IN_JUMP | IN_DUCK);
		return true;
	}
	if (me.onground && speed < 150.0f)
	{
		b.path.clear();
		b.plan.clear();
		if (b.groundSince == 0.0f || now < b.groundSince - 1.0f)
			b.groundSince = now + RANDOM_FLOAT(0.0f, 1.0f);
		else if (now - b.groundSince > 2.0f && now >= b.exitNext)
		{
			float yaw;
			if (FindExit(me.o, &yaw))
			{
				b.exitYaw = yaw;
				b.exitUntil = now + 3.0f;
				b.exitNext = now + 4.0f;
			}
			else
				b.exitNext = now + 5.0f + RANDOM_FLOAT(0.0f, 3.0f);
		}
		return false;
	}
	if (!me.onground)
	{
		b.exitUntil = 0.0f;
		b.groundSince = 0.0f;
	}
	else if (b.path.empty())
		return false;

	// a line to follow: from the archive place nearest it (a couple of picks a frame, all bots)
	if (b.path.empty() && now >= b.nextPick && s_buildsLeft > 0 && s_picksLeft > 0 && !s_nodes.empty())
	{
		s_picksLeft--;
		b.nextPick = now + 1.0f;
		int from = NearestNode(me.o, me.v);
		if (from >= 0)
		{
			// chasing, half the time: the nearest enemy alive within reach
			const Vector *enemy = nullptr;
			Vector ePos;
			float bestD = 3000.0f;
			for (int i = 1; i <= gpGlobals->maxClients; i++)
			{
				CBasePlayer *pl = UTIL_PlayerByIndex(i);
				if (!pl || pl == bot || !pl->IsAlive() || pl->m_iTeam == bot->m_iTeam)
					continue;
				float d = (pl->pev->origin - me.o).Length();
				if (d < bestD)
				{
					bestD = d;
					ePos = pl->pev->origin;
					enemy = &ePos;
				}
			}
			int to = PickTarget(from, RANDOM_FLOAT(0.0f, 1.0f) < 0.5f ? enemy : nullptr);
			if (to >= 0)
			{
				s_buildsLeft--;
				BuildPath(b, from, to);
			}
		}
	}

	if (b.path.size() >= 2)
	{
		float off;
		Locate(b, me.o, &off);
		if (b.idx >= (int)b.path.size() - 2 || off > 300.0f)
		{
			// the end of it, or lost it: ride on, and pick again in a moment
			b.path.clear();
			b.plan.clear();
			b.nextPick = now + (off > 300.0f ? 1.0f : 0.2f);
		}
		else
		{
			if (b.planStep >= (int)b.plan.size() * CHUNK)
				b.plan.clear();
			if (b.plan.empty() && off > 40.0f && s_replansLeft > 0 && now >= b.nextReplan && s_driveMs < 1.5)
			{
				s_replansLeft--;
				b.nextReplan = now + 0.5f;
				Replan(b, me, b.path[Q_min((int)b.path.size() - 1, b.idx + 4)]);
			}
			move = !b.plan.empty() ? b.plan[b.planStep++ / CHUNK] : b.path[b.idx].move;
		}
	}
	else
		b.path.clear();

	// no line: ride like gm_botsurf (along a ramp, diving 20), in the air along its velocity
	float yaw, fm, sm;
	if (move >= 0)
		ApplyMove(me, move, b.lastYaw, &yaw, &fm, &sm);
	else if (ramp)
		Act(me, true, n, RIDE, 20.0f, b.lastYaw, &yaw, &fm, &sm);
	else
		Act(me, false, n, FREE, 0.0f, b.lastYaw, &yaw, &fm, &sm);
	b.lastYaw = yaw;

	// with an enemy, it keeps its own aim (it can shoot) and the move's turned to match: the same
	// push, from where it's looking; else it looks the way it's going
	CCSBot *pBot = static_cast<CCSBot *>(bot);
	if (pBot->IsAttacking() && pBot->IsEnemyVisible())
	{
		float aim = bot->pev->v_angle.y;
		float wx = Cos(yaw) * fm + Sin(yaw) * sm, wy = Sin(yaw) * fm - Cos(yaw) * sm;
		*forward = wx * Cos(aim) + wy * Sin(aim);
		*strafe = wx * Sin(aim) - wy * Cos(aim);
		*buttons &= ~(IN_FORWARD | IN_BACK | IN_JUMP | IN_DUCK | IN_MOVELEFT | IN_MOVERIGHT);
		return true;
	}
	if (yaw > 180.0f)
		yaw -= 360.0f;
	else if (yaw < -180.0f)
		yaw += 360.0f;
	bot->pev->v_angle = Vector(0, yaw, 0);
	*forward = fm;
	*strafe = sm;
	*buttons &= ~(IN_FORWARD | IN_BACK | IN_JUMP | IN_DUCK | IN_MOVELEFT | IN_MOVERIGHT | IN_ATTACK);
	return true;
}

// A surf bot's movement this command (CBot::ExecuteCommand, msec its time): true if it set it
bool LsSurfDrive(CBasePlayer *bot, float msec, float *forward, float *strafe, unsigned short *buttons)
{
	using namespace LsSurf;
	if (cv_bot_ls_surf.value <= 0.0f)
		return false;
	int idx = bot->entindex();
	if (idx < 1 || idx > 32)
		return false;
	SurfBot &b = s_bots[idx];
	if (!bot->IsAlive() || Q_strcmp(s_map, STRING(gpGlobals->mapname)) != 0)
	{
		b = SurfBot();
		return false;
	}
	// (the command's real time, for the exploring: a bot thinks on a server frame, so 25-30 a second)
	if (msec > 0.0f && msec < 100.0f)
		s_cmdDt = 0.98f * s_cmdDt + 0.02f * (msec / 1000.0f);
	double t0 = NowMs();
	bool drove = LsSurfSteer(bot, b, msec, forward, strafe, buttons);
	s_driveMs += NowMs() - t0;
	// (let go after steering: its last strafe and forward aren't left running)
	if (!drove && b.driven)
	{
		*forward = 0.0f;
		*strafe = 0.0f;
		*buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);
	}
	b.driven = drove;
	return drove;
}
