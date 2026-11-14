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

// Loner-Strike creatures: the types (creatures.ini), the registry, the providers, our own
// ls_creature entity and its behaviours, and the ls_creature server command. See creature.h.

#include "precompiled.h"
#ifndef _WIN32
#include <sys/time.h>
#include <signal.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ucontext.h>
#endif
#include <climits>

cvar_t ls_creature_file      = { "ls_creature_file", "addons/amxmodx/configs/gamemodes/creatures.ini", 0, 0.0f, nullptr };
cvar_t ls_creature_precache  = { "ls_creature_precache", "", 0, 0.0f, nullptr };	// "" none, "all", or type names
cvar_t ls_creature_max       = { "ls_creature_max", "48", 0, 48.0f, nullptr };
cvar_t ls_creature_budget    = { "ls_creature_budget", "40", 0, 40.0f, nullptr };	// warn past this many models + sounds of ours
cvar_t ls_creature_debug     = { "ls_creature_debug", "0", 0, 0.0f, nullptr };
cvar_t ls_creature_last      = { "ls_creature_last", "0", 0, 0.0f, nullptr };		// the last one spawned (entity index), for plugins
cvar_t ls_creature_alive     = { "ls_creature_alive", "0", 0, 0.0f, nullptr };		// how many are alive (all providers)
cvar_t ls_creature_ver       = { "ls_creature_ver", "2", 0, 2.0f, nullptr };	// 2: state in iuser3, target in enemy; behave default, speed, tier
cvar_t ls_creature_reserve   = { "ls_creature_reserve", "40 60", 0, 0.0f, nullptr };	// model and sound slots creatures always leave free
cvar_t ls_creature_sounds    = { "ls_creature_sounds", "2", 0, 2.0f, nullptr };		// sounds of each kind a type keeps (idle, alert, ...)
cvar_t ls_creature_skipped   = { "ls_creature_skipped", "", 0, 0.0f, nullptr };		// types left out this map for want of precache room
cvar_t ls_creature_precache_late = { "ls_creature_precache_late", "1", 0, 1.0f, nullptr };	// precache at ServerActivate (the real count)
cvar_t ls_creature_frags     = { "ls_creature_frags", "0", 0, 0.0f, nullptr };		// 1 = kills add the type's frags to the score (counts toward mp_fraglimit)

cvar_t ls_watchdog           = { "ls_watchdog", "10", 0, 10.0f, nullptr };
cvar_t ls_creature_adaptive  = { "ls_creature_adaptive", "1", 0, 1.0f, nullptr };	// lower the live cap while the server's fps is low (players never see it)
cvar_t ls_creature_fps_floor = { "ls_creature_fps_floor", "85", 0, 85.0f, nullptr };	// server fps under this for 2 s: lower the cap
cvar_t ls_creature_fps_ok    = { "ls_creature_fps_ok", "105", 0, 105.0f, nullptr };	// at or above this for 10 s: raise it again
cvar_t ls_creature_cap_now   = { "ls_creature_cap_now", "0", 0, 0.0f, nullptr };		// the live cap now (read-only: ls_creature_max, lowered while fps is low)
cvar_t ls_creature_friendly_max = { "ls_creature_friendly_max", "8", 0, 8.0f, nullptr };	// friendly creatures (bought turrets) alive at once, apart from ls_creature_max
cvar_t ls_creature_flee_last = { "ls_creature_flee_last", "0", 0, 0.0f, nullptr };	// with this many hostile creatures alive or fewer, none flees (0 = off)
cvar_t ls_perf               = { "ls_perf", "0", 0, 0.0f, nullptr };		// seconds between performance lines in the log (0 = off)
cvar_t ls_crash_quiet        = { "ls_crash_quiet", "1", 0, 1.0f, nullptr };		// a dedicated server's crash: log and end, no dialog (Windows/Wine)		// seconds without a frame before the marks are written (0 = off)

// ---------------------------------------------------------------------------------------------
// Watchdog (see creature.h)

struct LsWatchMark
{
	const char *where;
	int ent;
	float time;
};

static const unsigned LS_WATCH_MARKS = 64;
static LsWatchMark s_watchMarks[LS_WATCH_MARKS];
static volatile unsigned s_watchNext = 0;
static volatile unsigned s_watchFrames = 0;
static char s_watchPath[260] = "";
static char s_watchText[64] = "";		// the newest LsMarkText

void LsMarkText(const char *text)
{
	Q_strlcpy(s_watchText, text ? text : "");
}

void LsMark(const char *where, int ent)
{
	unsigned i = s_watchNext % LS_WATCH_MARKS;
	s_watchMarks[i].where = where;
	s_watchMarks[i].ent = ent;
	s_watchMarks[i].time = gpGlobals ? gpGlobals->time : 0.0f;
	s_watchNext = s_watchNext + 1;
}

void LsWatchdogFrame()
{
	s_watchFrames = s_watchFrames + 1;
	LsMark("frame");
	LsPerfFrame();
}

// ---- ls_perf: server frame rate, creature and bot think time, edicts, what a player would see ----

// (a steady clock without std::chrono: the Linux build must run on old libstdc++)
static double LsPerfNow()
{
#ifdef _WIN32
	static LARGE_INTEGER freq;
	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return double(now.QuadPart) / double(freq.QuadPart);
#else
	// (gettimeofday: clock_gettime needs a newer glibc than the build allows)
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	return tv.tv_sec + tv.tv_usec * 1e-6;
#endif
}

static struct
{
	double windowStart, lastFrame, secondStart;
	int frames, secondFrames, minSecondFrames;
	double worstFrame;
	double creatureFrame, botsFrame;		// this frame's time so far
	double creatureSum, creatureMax, botsSum, botsMax;
	int thinks, thinksMax, thinksFrame;
} s_perf;

void LsPerfAddCreature(double seconds)
{
	s_perf.creatureFrame += seconds;
	s_perf.thinksFrame++;
}

void LsPerfAddBots(double seconds)
{
	s_perf.botsFrame += seconds;
}

double LsPerfClock()
{
	return ls_perf.value > 0.0f ? LsPerfNow() : 0.0;
}

// entities a player standing where this one is would be sent (AddToFullPack's tests, before the
// engine's 256 a frame)
static int LsPerfViewCount(edict_t *host)
{
	unsigned char *pvs = ENGINE_SET_PVS(host->v.origin + host->v.view_ofs);
	int n = 0;
	for (int e = 1; e < gpGlobals->maxEntities; e++)
	{
		edict_t *ent = INDEXENT(e);
		if (!ent || ent->free || ent == host)
			continue;
		if (!ent->v.modelindex || !STRING(ent->v.model) || !STRING(ent->v.model)[0])
			continue;
		if (ent->v.effects & EF_NODRAW)
			continue;
		bool player = e <= gpGlobals->maxClients;
		if (player && !(ent->v.flags & (FL_CLIENT | FL_FAKECLIENT)))
			continue;
		if (!ENGINE_CHECK_VISIBILITY(ent, pvs))
			continue;
		n++;
	}
	return n;
}

static void LsPerfReport(double now, double span)
{
	int alive = 0, edicts = 0;
	for (int e = 1; e < gpGlobals->maxEntities; e++)
	{
		edict_t *ent = INDEXENT(e);
		if (!ent || ent->free)
			continue;
		edicts++;
		if (ent->v.deadflag == DEAD_NO && (ent->v.flags & FL_MONSTER) && ent->v.health > 0.0f)
			alive++;
	}
	int bots = 0, viewMax = 0, viewSum = 0, viewFull = 0;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		edict_t *p = INDEXENT(i);
		if (!p || p->free || !(p->v.flags & (FL_CLIENT | FL_FAKECLIENT)) || p->v.deadflag != DEAD_NO || p->v.health <= 0.0f)
			continue;
		int v = LsPerfViewCount(p);
		bots++;
		viewSum += v;
		viewMax = Q_max(viewMax, v);
		if (v >= 256)
			viewFull++;
	}
	int frames = Q_max(s_perf.frames, 1);
	UTIL_LogPrintf("ls_perf: %.0fs fps avg %.1f min %d (worst frame %.1f ms) | cap now %.0f | monsters alive %d, thinks %.1f/frame (max %d), think %.3f ms/frame avg %.2f max | bots %.3f ms/frame avg %.2f max | edicts %d of %d | view ents avg %.0f max %d over %d players, %d at 256+\n",
		span, s_perf.frames / span, s_perf.minSecondFrames == INT_MAX ? s_perf.frames : s_perf.minSecondFrames, s_perf.worstFrame * 1000.0,
		LsCreatureCapNow(), alive, double(s_perf.thinks) / frames, s_perf.thinksMax, s_perf.creatureSum * 1000.0 / frames, s_perf.creatureMax * 1000.0,
		s_perf.botsSum * 1000.0 / frames, s_perf.botsMax * 1000.0, edicts, gpGlobals->maxEntities,
		bots ? double(viewSum) / bots : 0.0, viewMax, bots, viewFull);
}

// The adaptive cap: each real second, the frames that came against the server's tick rate. Low
// for 2 seconds in a row: the cap drops to a little under what's alive (never under 12); good for
// 10 in a row: it creeps up 4, never past ls_creature_max. Spawns past it wait (the wave spawner
// tries again), nobody is removed, and difficulty is untouched: only how many at once.
static struct { double secondStart; int frames, low, good; float cap; } s_adapt;

static int LsAliveCount(bool friendly = false);

float LsCreatureCapNow()
{
	float limit = ls_creature_max.value;
	if (ls_creature_adaptive.value <= 0.0f || s_adapt.cap <= 0.0f)
		return limit;
	return Q_min(limit, s_adapt.cap);
}

static void LsAdaptFrame()
{
	double now = LsPerfNow();
	if (s_adapt.secondStart == 0.0 || now - s_adapt.secondStart > 5.0)
	{
		s_adapt.secondStart = now;
		s_adapt.frames = 0;
		return;
	}
	s_adapt.frames++;
	if (now - s_adapt.secondStart < 1.0)
		return;

	float fps = s_adapt.frames / float(now - s_adapt.secondStart);
	s_adapt.secondStart = now;
	s_adapt.frames = 0;

	// absolute fps (a server at sys_ticrate 300 or 1000 never reaches a share of it); a server
	// whose tick rate is under them is judged against 80 % / 95 % of it instead
	float tic = CVAR_GET_FLOAT("sys_ticrate");
	if (tic <= 0.0f)
		tic = 100.0f;
	float low = Q_min(ls_creature_fps_floor.value, tic * 0.8f);
	float ok = Q_max(low + 5.0f, Q_min(ls_creature_fps_ok.value, tic * 0.95f));
	float limit = ls_creature_max.value;
	if (s_adapt.cap <= 0.0f || s_adapt.cap > limit)
		s_adapt.cap = limit;

	if (ls_creature_adaptive.value > 0.0f && fps < low)
	{
		s_adapt.good = 0;
		if (++s_adapt.low >= 2)
		{
			s_adapt.low = 0;
			int alive = LsAliveCount();
			float cap = Q_max(12.0f, Q_min(s_adapt.cap - 4.0f, floorf(alive * 0.85f)));
			if (cap < s_adapt.cap)
			{
				UTIL_LogPrintf("ls_creature: server at %.0f fps (under %.0f): live cap %.0f -> %.0f\n", fps, low, s_adapt.cap, cap);
				s_adapt.cap = cap;
			}
		}
	}
	else if (fps >= ok)
	{
		s_adapt.low = 0;
		if (++s_adapt.good >= 10 && s_adapt.cap < limit)
		{
			s_adapt.good = 0;
			float cap = Q_min(limit, s_adapt.cap + 4.0f);
			UTIL_LogPrintf("ls_creature: server back at %.0f fps: live cap %.0f -> %.0f\n", fps, s_adapt.cap, cap);
			s_adapt.cap = cap;
		}
	}
	else
		s_adapt.low = s_adapt.good = 0;

	if (int(ls_creature_cap_now.value) != int(LsCreatureCapNow()))
		CVAR_SET_FLOAT("ls_creature_cap_now", LsCreatureCapNow());
}

void LsPerfFrame()
{
	LsAdaptFrame();
	if (ls_perf.value <= 0.0f)
	{
		s_perf.windowStart = 0.0;
		return;
	}
	double now = LsPerfNow();
	if (s_perf.windowStart == 0.0)
	{
		memset(&s_perf, 0, sizeof(s_perf));
		s_perf.windowStart = s_perf.lastFrame = s_perf.secondStart = now;
		s_perf.minSecondFrames = INT_MAX;
		return;
	}
	// the frame that just ended
	s_perf.frames++;
	s_perf.secondFrames++;
	s_perf.worstFrame = Q_max(s_perf.worstFrame, now - s_perf.lastFrame);
	s_perf.lastFrame = now;
	s_perf.creatureSum += s_perf.creatureFrame;
	s_perf.creatureMax = Q_max(s_perf.creatureMax, s_perf.creatureFrame);
	s_perf.botsSum += s_perf.botsFrame;
	s_perf.botsMax = Q_max(s_perf.botsMax, s_perf.botsFrame);
	s_perf.thinks += s_perf.thinksFrame;
	s_perf.thinksMax = Q_max(s_perf.thinksMax, s_perf.thinksFrame);
	s_perf.creatureFrame = s_perf.botsFrame = 0.0;
	s_perf.thinksFrame = 0;
	if (now - s_perf.secondStart >= 1.0)
	{
		s_perf.minSecondFrames = Q_min(s_perf.minSecondFrames, s_perf.secondFrames);
		s_perf.secondFrames = 0;
		s_perf.secondStart = now;
	}
	if (now - s_perf.windowStart >= ls_perf.value)
	{
		LsPerfReport(now, now - s_perf.windowStart);
		memset(&s_perf, 0, sizeof(s_perf));
		s_perf.windowStart = s_perf.lastFrame = s_perf.secondStart = LsPerfNow();
		s_perf.minSecondFrames = INT_MAX;
	}
}

static void LsWatchdogWrite(unsigned frames, int secs, bool resumed)
{
	FILE *f = fopen(s_watchPath, "a");
	if (!f)
		return;

	if (resumed)
	{
		fprintf(f, "frames came again after %d s\n\n", secs);
		fclose(f);
		return;
	}

	time_t now = time(nullptr);
	fprintf(f, "%sno server frame for %d s (frame %u); the last marks, oldest first (game time, where, entity):\n", ctime(&now), secs, frames);
	unsigned next = s_watchNext;
	unsigned count = (next < LS_WATCH_MARKS) ? next : LS_WATCH_MARKS;
	for (unsigned k = next - count; k != next; k++)
	{
		const LsWatchMark &m = s_watchMarks[k % LS_WATCH_MARKS];
		fprintf(f, "  %10.2f  %-28s %d\n", m.time, m.where ? m.where : "?", m.ent);
	}
	fprintf(f, "  (last text: %s)\n\n", s_watchText);
	fclose(f);
}

#ifdef _WIN32
// A crash on Windows: the faulting address and every return address on the stack that points
// into a loaded module (module+offset, for llvm-symbolizer with the CI's mp.pdb), with the
// watchdog's marks, to ls_crash.log beside ls_watchdog.log. Vectored, so it runs before the
// engine's own handler; access violations and the like only, the first few.
static void LsCrashModule(FILE *f, DWORD addr)
{
	MEMORY_BASIC_INFORMATION mbi;
	char name[MAX_PATH] = "?";
	if (VirtualQuery((LPCVOID)(size_t)addr, &mbi, sizeof(mbi)) && mbi.AllocationBase
		&& GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, sizeof(name)))
	{
		const char *base = Q_strrchr(name, '\\');
		fprintf(f, "  0x%08lx  %s+0x%lx\n", (unsigned long)addr, base ? base + 1 : name, (unsigned long)(addr - (DWORD)(size_t)mbi.AllocationBase));
	}
}

static bool LsCrashIsCode(DWORD addr)
{
	MEMORY_BASIC_INFORMATION mbi;
	if (!VirtualQuery((LPCVOID)(size_t)addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE)
		return false;
	return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static LONG WINAPI LsCrashHandler(EXCEPTION_POINTERS *ep)
{
	static volatile LONG s_written = 0;
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW
		&& code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION)
		return EXCEPTION_CONTINUE_SEARCH;

	if (InterlockedIncrement(&s_written) > 3 || !s_watchPath[0])
		return EXCEPTION_CONTINUE_SEARCH;

	char path[260];
	Q_strlcpy(path, s_watchPath);
	char *slash = Q_strrchr(path, '/');
	Q_strlcpy(slash ? slash + 1 : path, "ls_crash.log", sizeof(path) - (slash ? (slash + 1 - path) : 0));

	FILE *f = fopen(path, "a");
	if (!f)
		return EXCEPTION_CONTINUE_SEARCH;

	time_t now = time(nullptr);
	DWORD eip = ep->ContextRecord->Eip, esp = ep->ContextRecord->Esp;
	fprintf(f, "%sexception 0x%08lx at 0x%08lx (first-chance; the game may still handle it)\n", ctime(&now), (unsigned long)code, (unsigned long)eip);
	if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
		fprintf(f, "  %s 0x%08lx\n", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);

	fprintf(f, "where:\n");
	LsCrashModule(f, eip);
	fprintf(f, "return addresses on the stack (newest first):\n");
	int found = 0;
	for (DWORD *p = (DWORD *)(size_t)esp; found < 40 && p < (DWORD *)(size_t)esp + 4096; p++)
	{
		MEMORY_BASIC_INFORMATION mbi;
		if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
			break;
		if (LsCrashIsCode(*p))
		{
			LsCrashModule(f, *p);
			found++;
		}
	}

	fprintf(f, "last marks:\n");
	unsigned next = s_watchNext;
	unsigned count = (next < LS_WATCH_MARKS) ? next : LS_WATCH_MARKS;
	for (unsigned k = next - count; k != next; k++)
	{
		const LsWatchMark &m = s_watchMarks[k % LS_WATCH_MARKS];
		fprintf(f, "  %10.2f  %-28s %d\n", m.time, m.where ? m.where : "?", m.ent);
	}
	fprintf(f, "  (last text: %s)\n\n", s_watchText);
	fclose(f);
	return EXCEPTION_CONTINUE_SEARCH;
}

// A dedicated server that crashes ends quietly, with ls_crash.log written: the default handler
// would start Wine's/Windows' crash dialog (winedbg --auto, or Windows Error Reporting), which
// keeps the dead server holding its port until someone clicks it away - on a Mac's own desktop
// for a CrossOver server. Only for dedicated servers: the player's game keeps its usual behaviour
static LONG WINAPI LsUnhandled(EXCEPTION_POINTERS *ep)
{
	LsCrashHandler(ep);
	FILE *f = fopen(s_watchPath[0] ? s_watchPath : "ls_watchdog.log", "a");
	if (f)
	{
		fprintf(f, "unhandled exception 0x%08lx at 0x%08lx: the server ends (see ls_crash.log)\n\n",
			(unsigned long)ep->ExceptionRecord->ExceptionCode, (unsigned long)(size_t)ep->ExceptionRecord->ExceptionAddress);
		fclose(f);
	}
	TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
	return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#ifndef _WIN32
// A crash on Linux: the signal, the address, where (module and function: the Linux cs.so keeps
// its symbols) and the call stack, with the watchdog's marks, to ls_crash.log; then the signal
// goes on to whoever handled it before (the engine's own handler, or the default: a core).
static struct sigaction s_crashOld[32];

static void LsCrashWrite(int fd, const char *text, int len)
{
	ssize_t r = write(fd, text, len);
	(void)r;
}

static void LsCrashSignal(int sig, siginfo_t *info, void *ctx)
{
	static volatile int s_written = 0;
	if (++s_written <= 3 && s_watchPath[0])
	{
		char path[260];
		Q_strlcpy(path, s_watchPath);
		char *slash = Q_strrchr(path, '/');
		Q_strlcpy(slash ? slash + 1 : path, "ls_crash.log", sizeof(path) - (slash ? (slash + 1 - path) : 0));
		int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0)
		{
			char line[512];
			void *eip = nullptr;
#if defined(REG_EIP)
			eip = (void *)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_EIP];
#endif
			time_t now = time(nullptr);
			int n = Q_snprintf(line, sizeof(line), "%ssignal %d at %p (address %p)\n", ctime(&now), sig, eip, info ? info->si_addr : nullptr);
			LsCrashWrite(fd, line, n);
			Dl_info di;
			if (eip && dladdr(eip, &di))
			{
				n = Q_snprintf(line, sizeof(line), "where: %s (%s+0x%lx), module base %p\n", di.dli_fname ? di.dli_fname : "?", di.dli_sname ? di.dli_sname : "?",
					(unsigned long)((char *)eip - (char *)(di.dli_saddr ? di.dli_saddr : di.dli_fbase)), di.dli_fbase);
				LsCrashWrite(fd, line, n);
			}
			void *frames[48];
			int count = backtrace(frames, 48);
			LsCrashWrite(fd, "stack (newest first):\n", 22);
			backtrace_symbols_fd(frames, count, fd);
			LsCrashWrite(fd, "last marks:\n", 12);
			unsigned next = s_watchNext;
			unsigned marks = (next < LS_WATCH_MARKS) ? next : LS_WATCH_MARKS;
			for (unsigned k = next - marks; k != next; k++)
			{
				const LsWatchMark &m = s_watchMarks[k % LS_WATCH_MARKS];
				n = Q_snprintf(line, sizeof(line), "  %10.2f  %-28s %d\n", m.time, m.where ? m.where : "?", m.ent);
				LsCrashWrite(fd, line, n);
			}
			n = Q_snprintf(line, sizeof(line), "  (last text: %s)\n\n", s_watchText);
			LsCrashWrite(fd, line, n);
			close(fd);
		}
	}
	// on to the handler before ours (or the default), as if we weren't here
	sigaction(sig, &s_crashOld[sig], nullptr);
	raise(sig);
}

static void LsCrashSignalsInstall()
{
	struct sigaction sa;
	Q_memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = LsCrashSignal;
	sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigemptyset(&sa.sa_mask);
	const int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
	for (int sig : sigs)
	{
		struct sigaction old;
		if (sigaction(sig, nullptr, &old) == 0 && old.sa_sigaction == LsCrashSignal)
			continue;
		if (sigaction(sig, &sa, &old) == 0)
			s_crashOld[sig] = old;
	}
}
#endif

// (each map: the engine and Steam's crash handler may have set their own filter since)
void LsCrashQuietInstall()
{
#ifdef _WIN32
	if (IS_DEDICATED_SERVER() && ls_crash_quiet.value > 0.0f)
		SetUnhandledExceptionFilter(LsUnhandled);
#else
	LsCrashSignalsInstall();
#endif
}

// The second thread: once a second, did a frame come? (only reads; writes nothing the game uses)
static void LsWatchdogLoop()
{
	unsigned seen = s_watchFrames;
	int still = 0;
	bool told = false;
	for (;;)
	{
#ifdef _WIN32
		Sleep(1000);
#else
		sleep(1);
#endif
		unsigned frames = s_watchFrames;
		int limit = int(ls_watchdog.value);
		if (frames != seen || !g_bServerActive || limit <= 0)
		{
			if (told && frames != seen)
				LsWatchdogWrite(frames, still, true);
			seen = frames;
			still = 0;
			told = false;
			continue;
		}

		if (++still >= limit && !told)
		{
			told = true;
			LsWatchdogWrite(frames, still, false);
		}
	}
}

#ifdef _WIN32
static DWORD WINAPI LsWatchdogThread(LPVOID)
{
	LsWatchdogLoop();
	return 0;
}
#else
static void *LsWatchdogThread(void *)
{
	LsWatchdogLoop();
	return nullptr;
}
#endif

void LsWatchdogStart()
{
	static bool started = false;
	if (started)
		return;

	started = true;
	char dir[200];
	GET_GAME_DIR(dir);
	Q_snprintf(s_watchPath, sizeof(s_watchPath), "%s/ls_watchdog.log", dir);

#ifdef _WIN32
	AddVectoredExceptionHandler(1, LsCrashHandler);
	HANDLE h = CreateThread(nullptr, 64 * 1024, LsWatchdogThread, nullptr, 0, nullptr);
	if (h)
		CloseHandle(h);
#else
	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 64 * 1024);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&thread, &attr, LsWatchdogThread, nullptr);
	pthread_attr_destroy(&attr);
#endif
}

void LsCreatures_RegisterCVars()
{
	CVAR_REGISTER(&ls_watchdog);
	CVAR_REGISTER(&ls_crash_quiet);
	CVAR_REGISTER(&ls_perf);
	CVAR_REGISTER(&ls_creature_adaptive);
	CVAR_REGISTER(&ls_creature_friendly_max);
	CVAR_REGISTER(&ls_creature_flee_last);
	CVAR_REGISTER(&ls_creature_fps_floor);
	CVAR_REGISTER(&ls_creature_fps_ok);
	CVAR_REGISTER(&ls_creature_cap_now);
	LsWatchdogStart();
	CVAR_REGISTER(&ls_creature_file);
	CVAR_REGISTER(&ls_creature_precache);
	CVAR_REGISTER(&ls_creature_max);
	CVAR_REGISTER(&ls_creature_budget);
	CVAR_REGISTER(&ls_creature_debug);
	CVAR_REGISTER(&ls_creature_last);
	CVAR_REGISTER(&ls_creature_alive);
	CVAR_REGISTER(&ls_creature_ver);
	CVAR_REGISTER(&ls_creature_reserve);
	CVAR_REGISTER(&ls_creature_sounds);
	CVAR_REGISTER(&ls_creature_skipped);
	CVAR_REGISTER(&ls_creature_precache_late);
	CVAR_REGISTER(&ls_creature_frags);
}

// ---------------------------------------------------------------------------------------------
// Types

static LsCreatureType s_types[LS_CREATURE_MAX_TYPES];
static int s_typeCount = 0;
static bool s_typesLoaded = false;
static int s_precachedModels = 0;
static int s_precachedSounds = 0;
static int s_zapSprite = 0;			// sprites/lgtning.spr, precached with the first type that zaps

// A load builds the types here first, then merges them into s_types by name: a type keeps its
// slot for the whole server run (live creatures hold its index), new names are appended, and a
// name the file dropped is only disabled (no new spawns)
static LsCreatureType s_build[LS_CREATURE_MAX_TYPES];
static int s_buildCount = 0;

static bool s_mapRunning = false;		// between ServerActivate and the map's end

// Reentrant tokenizer (strtok can't nest: the type loader runs inside other loops): the next
// token of *cursor split on any of seps, null terminated in place, or null at the end
static char *LsNextToken(char **cursor, const char *seps)
{
	char *p = *cursor;
	while (*p && Q_strchr(seps, *p))
		p++;

	if (!*p)
	{
		*cursor = p;
		return nullptr;
	}

	char *start = p;
	while (*p && !Q_strchr(seps, *p))
		p++;

	if (*p)
		*p++ = '\0';

	*cursor = p;
	return start;
}

// A number from a file, a key or a command: anything not finite keeps the old value, the rest
// is clamped to what makes sense
static bool LsFinite(float v)
{
	return v == v && v < 1e30f && v > -1e30f;
}

static float LsNum(const char *value, float lo, float hi, float keep)
{
	float v = float(Q_atof(value));
	if (!LsFinite(v))
		return keep;

	return clamp(v, lo, hi);
}

// A position from a command: finite and inside the world (+-8192 leaves room past the usual +-4096)
static bool LsPosOk(const Vector &v)
{
	for (int i = 0; i < 3; i++)
	{
		if (!LsFinite(v[i]) || v[i] > 8192.0f || v[i] < -8192.0f)
			return false;
	}

	return true;
}

// A yaw from anywhere: finite, folded into -360..360 (the turn code loops on huge angles)
static float LsYaw(float yaw)
{
	if (!LsFinite(yaw))
		return 0.0f;

	return float(fmod(yaw, 360.0));
}

static const char *s_categoryNames[LSC_CATEGORY_COUNT] = { "infected", "alien", "wildlife", "mechanical", "boss", "special" };
static const char *s_behaviourNames[LSB_COUNT] = { "idle", "wander", "patrol", "guard", "chase", "flee" };
static const char *s_attackNames[] = { "none", "melee", "ranged" };
static const char *s_targetingNames[] = { "nearest", "weakest", "attacker", "random" };
static const char *s_animKeys[LSAN_COUNT] = { "anim_idle", "anim_walk", "anim_run", "anim_attack", "anim_flinch", "anim_die" };
static const char *s_soundKeys[LSS_COUNT] = { "snd_idle", "snd_alert", "snd_attack", "snd_hit", "snd_miss", "snd_pain", "snd_die" };

static int LsNameIndex(const char *value, const char **names, int count, int fallback)
{
	for (int i = 0; i < count; i++)
	{
		if (!Q_stricmp(value, names[i]))
			return i;
	}

	// (a number works too)
	if (value[0] >= '0' && value[0] <= '9')
	{
		int n = Q_atoi(value);
		if (n >= 0 && n < count)
			return n;
	}

	return fallback;
}

static int LsHatesFromString(const char *value)
{
	if (!Q_stricmp(value, "t") || !Q_stricmp(value, "terrorist") || !Q_stricmp(value, "1"))
		return 1;

	if (!Q_stricmp(value, "ct") || !Q_stricmp(value, "2"))
		return 2;

	return 0;
}

static Vector LsParseVector(const char *value, const Vector &keep)
{
	Vector v(0, 0, 0);
	if (sscanf(value, "%f %f %f", &v.x, &v.y, &v.z) != 3)
		return keep;

	for (int i = 0; i < 3; i++)
	{
		if (!LsFinite(v[i]) || v[i] > 8192.0f || v[i] < -8192.0f)
			return keep;
	}

	return v;
}

static Vector LsParseColor(const char *value, const Vector &keep)
{
	Vector v = LsParseVector(value, keep);
	for (int i = 0; i < 3; i++)
		v[i] = clamp(v[i], 0.0f, 255.0f);

	return v;
}

// Neutral starting values for a type the file names without a base
static void LsTypeDefaults(LsCreatureType *t, const char *name)
{
	Q_memset(t, 0, sizeof(*t));
	Q_strlcpy(t->name, name);
	Q_strlcpy(t->display, name);
	t->category = LSC_INFECTED;
	t->health = 100.0f;
	t->speed = 230.0f;
	t->walkSpeed = 80.0f;
	t->aggression = 1.0f;
	t->detectRadius = 1200.0f;
	t->detectFov = 160.0f;
	t->hearScale = 1.0f;
	t->attack = LSA_MELEE;
	t->damage = 20.0f;
	t->attackRange = 64.0f;
	t->attackRate = 1.0f;
	t->attackDelay = 0.4f;
	t->spread = 3.0f;
	t->fleeHealth = 0.0f;
	t->behaviour = LSB_WANDER;
	t->guardRadius = 400.0f;
	t->targeting = LST_NEAREST;
	t->hates = 0;
	t->respawn = 0.0f;
	t->corpseTime = 8.0f;
	t->moves = true;
	t->frags = 1;
	t->money = 0;
	Q_strlcpy(t->models, "models/zombie.mdl models/hostage.mdl");
	t->scale = 1.0f;
	t->hullMin = Vector(-16, -16, 0);
	t->hullMax = Vector(16, 16, 72);
	t->fallbackGlow = true;
	t->fallbackGlowColor = Vector(40, 140, 20);
	t->bloodColor = BLOOD_COLOR_RED;
	Q_strlcpy(t->sound[LSS_IDLE], "zombie/zo_idle1.wav zombie/zo_idle2.wav zombie/zo_idle3.wav zombie/zo_idle4.wav");
	Q_strlcpy(t->sound[LSS_ALERT], "zombie/zo_alert10.wav zombie/zo_alert20.wav zombie/zo_alert30.wav");
	Q_strlcpy(t->sound[LSS_ATTACK], "zombie/zo_attack1.wav zombie/zo_attack2.wav");
	Q_strlcpy(t->sound[LSS_HIT], "zombie/claw_strike1.wav zombie/claw_strike2.wav zombie/claw_strike3.wav | weapons/knife_hit1.wav weapons/knife_hit2.wav");
	Q_strlcpy(t->sound[LSS_MISS], "zombie/claw_miss1.wav zombie/claw_miss2.wav | weapons/knife_slash1.wav weapons/knife_slash2.wav");
	Q_strlcpy(t->sound[LSS_PAIN], "zombie/zo_pain1.wav zombie/zo_pain2.wav | player/pl_pain2.wav player/pl_pain4.wav");
	Q_strlcpy(t->sound[LSS_DIE], "zombie/zo_pain2.wav | player/die1.wav player/die3.wav");
	t->friendly = false;
	t->fleeHome = true;
	t->enrageBelow = 0.0f;
	t->enrageSpeed = 1.35f;
	t->enrageDamage = 1.3f;
	t->enrageAnim = 1.25f;
	t->tierSkin[0] = t->tierSkin[1] = -1;
	t->tierBody[0] = t->tierBody[1] = -1;
	t->tierHealth[0] = t->tierHealth[1] = 1.0f;
}

// (into the build list: see s_build)
static LsCreatureType *LsAddType(const char *name)
{
	for (int i = 0; i < s_buildCount; i++)
	{
		if (!Q_stricmp(s_build[i].name, name))
			return &s_build[i];
	}

	if (s_buildCount >= LS_CREATURE_MAX_TYPES || !*name)
		return nullptr;

	LsCreatureType *t = &s_build[s_buildCount++];
	LsTypeDefaults(t, name);
	return t;
}

// The first set: works on any CS install (player/hostage models tinted), better with
// Half-Life's own models when valve/models has them (most installs: the shared depot)
static void LsBuiltinTypes()
{
	LsCreatureType *t;

	t = LsAddType("zombie");
	Q_strlcpy(t->display, "Zombie");

	t = LsAddType("runner");
	Q_strlcpy(t->display, "Runner");
	t->health = 60.0f;
	t->speed = 300.0f;
	t->walkSpeed = 110.0f;
	t->damage = 10.0f;
	t->attackRate = 1.6f;
	t->attackDelay = 0.25f;
	t->behaviour = LSB_CHASE;
	t->bloodColor = BLOOD_COLOR_YELLOW;
	Q_strlcpy(t->models, "models/houndeye.mdl models/hostage.mdl");	// (has a real run; the HL zombie only walks)
	t->hullMin = Vector(-16, -16, 0);
	t->hullMax = Vector(16, 16, 36);
	t->fallbackGlowColor = Vector(160, 160, 20);
	Q_strlcpy(t->sound[LSS_IDLE], "houndeye/he_idle1.wav houndeye/he_idle2.wav houndeye/he_idle3.wav");
	Q_strlcpy(t->sound[LSS_ALERT], "houndeye/he_alert1.wav houndeye/he_alert2.wav houndeye/he_alert3.wav");
	Q_strlcpy(t->sound[LSS_ATTACK], "houndeye/he_attack1.wav houndeye/he_attack3.wav");
	Q_strlcpy(t->sound[LSS_PAIN], "houndeye/he_pain1.wav houndeye/he_pain3.wav | player/pl_pain2.wav");
	Q_strlcpy(t->sound[LSS_DIE], "houndeye/he_die1.wav houndeye/he_die2.wav | player/die1.wav");

	t = LsAddType("brute");
	t->enrageBelow = 0.4f;			// a heavy: enraged below 40%
	Q_strlcpy(t->display, "Brute");
	t->health = 450.0f;
	t->speed = 170.0f;
	t->walkSpeed = 70.0f;
	t->damage = 40.0f;
	t->attackRange = 72.0f;
	t->attackRate = 0.7f;
	t->attackDelay = 0.6f;
	t->category = LSC_ALIEN;
	t->frags = 3;
	t->bloodColor = BLOOD_COLOR_YELLOW;
	Q_strlcpy(t->models, "models/agrunt.mdl models/hostage.mdl");	// the alien grunt: walk, run, two swings
	t->fallbackGlowColor = Vector(120, 20, 20);
	Q_strlcpy(t->sound[LSS_IDLE], "agrunt/ag_idle1.wav agrunt/ag_idle2.wav agrunt/ag_idle3.wav agrunt/ag_idle4.wav");
	Q_strlcpy(t->sound[LSS_ALERT], "agrunt/ag_alert1.wav agrunt/ag_alert3.wav agrunt/ag_alert4.wav");
	Q_strlcpy(t->sound[LSS_ATTACK], "agrunt/ag_attack1.wav agrunt/ag_attack2.wav agrunt/ag_attack3.wav");
	Q_strlcpy(t->sound[LSS_PAIN], "agrunt/ag_pain1.wav agrunt/ag_pain2.wav agrunt/ag_pain3.wav | player/pl_pain2.wav");
	Q_strlcpy(t->sound[LSS_DIE], "agrunt/ag_die1.wav agrunt/ag_die4.wav | player/die1.wav");

	t = LsAddType("spitter");
	Q_strlcpy(t->display, "Spitter");
	t->category = LSC_ALIEN;
	t->health = 80.0f;
	t->speed = 220.0f;
	t->attack = LSA_RANGED;
	t->damage = 8.0f;
	t->attackRange = 900.0f;
	t->attackRate = 0.8f;
	t->attackDelay = 0.5f;
	t->spread = 4.0f;
	t->fleeHealth = 0.3f;
	t->bloodColor = BLOOD_COLOR_YELLOW;
	Q_strlcpy(t->models, "models/bullsquid.mdl models/hostage.mdl");
	t->hullMin = Vector(-16, -16, 0);		// (the engine moves anything wider than 36 as a 64x64x64 box: stuck in doors)
	t->hullMax = Vector(16, 16, 36);
	t->fallbackGlowColor = Vector(120, 200, 40);
	Q_strlcpy(t->sound[LSS_IDLE], "bullchicken/bc_idle1.wav bullchicken/bc_idle2.wav bullchicken/bc_idle3.wav");
	Q_strlcpy(t->sound[LSS_ALERT], "bullchicken/bc_idle4.wav bullchicken/bc_idle5.wav");
	Q_strlcpy(t->sound[LSS_ATTACK], "bullchicken/bc_attack2.wav bullchicken/bc_attack3.wav | weapons/sg550-1.wav");
	Q_strlcpy(t->sound[LSS_HIT], "bullchicken/bc_spithit1.wav bullchicken/bc_spithit2.wav");
	t->sound[LSS_MISS][0] = '\0';
	Q_strlcpy(t->sound[LSS_PAIN], "bullchicken/bc_pain1.wav bullchicken/bc_pain2.wav | player/pl_pain2.wav");
	Q_strlcpy(t->sound[LSS_DIE], "bullchicken/bc_die1.wav bullchicken/bc_die2.wav | player/die1.wav");

	t = LsAddType("hound");
	Q_strlcpy(t->display, "Hound");
	t->category = LSC_WILDLIFE;
	t->health = 70.0f;
	t->speed = 280.0f;
	t->walkSpeed = 90.0f;
	t->aggression = 0.35f;				// territorial: only what comes close to its spot
	t->behaviour = LSB_GUARD;
	t->guardRadius = 350.0f;
	t->damage = 12.0f;
	t->fleeHealth = 0.35f;
	t->bloodColor = BLOOD_COLOR_YELLOW;
	Q_strlcpy(t->models, "models/houndeye.mdl models/hostage.mdl");
	t->hullMin = Vector(-16, -16, 0);
	t->hullMax = Vector(16, 16, 36);
	t->fallbackGlowColor = Vector(200, 120, 20);
	Q_strlcpy(t->sound[LSS_IDLE], "houndeye/he_idle1.wav houndeye/he_idle2.wav houndeye/he_idle3.wav");
	Q_strlcpy(t->sound[LSS_ALERT], "houndeye/he_alert1.wav houndeye/he_alert2.wav houndeye/he_alert3.wav");
	Q_strlcpy(t->sound[LSS_ATTACK], "houndeye/he_attack1.wav houndeye/he_attack3.wav");
	Q_strlcpy(t->sound[LSS_PAIN], "houndeye/he_pain1.wav houndeye/he_pain3.wav | player/pl_pain2.wav");
	Q_strlcpy(t->sound[LSS_DIE], "houndeye/he_die1.wav houndeye/he_die2.wav | player/die1.wav");

	t = LsAddType("turret");
	Q_strlcpy(t->display, "Sentry");
	t->category = LSC_MECHANICAL;
	t->health = 150.0f;
	t->moves = false;
	t->speed = 0.0f;
	t->walkSpeed = 0.0f;
	t->attack = LSA_RANGED;
	t->damage = 6.0f;
	t->attackRange = 1200.0f;
	t->attackRate = 4.0f;
	t->attackDelay = 0.05f;
	t->spread = 3.0f;
	t->detectFov = 360.0f;
	t->hearScale = 0.0f;
	t->behaviour = LSB_GUARD;
	t->bloodColor = DONT_BLEED;
	t->corpseTime = 15.0f;
	Q_strlcpy(t->models, "models/sentry.mdl models/hostage.mdl");
	Q_strlcpy(t->anim[LSAN_IDLE], "spin");		// (sentry.mdl has no activities: its sequences by name)
	Q_strlcpy(t->anim[LSAN_ATTACK], "fire");
	Q_strlcpy(t->anim[LSAN_DIE], "die");
	t->hullMin = Vector(-16, -16, 0);
	t->hullMax = Vector(16, 16, 48);
	t->fallbackGlowColor = Vector(40, 80, 200);
	for (int s = 0; s < LSS_COUNT; s++)
		t->sound[s][0] = '\0';
	Q_strlcpy(t->sound[LSS_ALERT], "turret/tu_deploy.wav | weapons/zoom.wav");
	Q_strlcpy(t->sound[LSS_ATTACK], "turret/tu_fire1.wav | weapons/m249-1.wav");
	Q_strlcpy(t->sound[LSS_DIE], "turret/tu_die.wav turret/tu_die2.wav | weapons/debris1.wav");

	t = LsAddType("boss");
	t->enrageBelow = 0.4f;			// a heavy: enraged below 40%
	Q_strlcpy(t->display, "Abomination");
	t->category = LSC_BOSS;
	t->health = 3000.0f;
	t->speed = 200.0f;
	t->damage = 60.0f;
	t->attackRange = 80.0f;
	t->attackRate = 0.6f;
	t->attackDelay = 0.7f;
	t->detectRadius = 2000.0f;
	t->behaviour = LSB_CHASE;
	t->scale = 0.7f;					// (the gargantua is twice a player's height at 1; the hull stays player-sized)
	t->frags = 10;
	t->bloodColor = BLOOD_COLOR_YELLOW;
	Q_strlcpy(t->models, "models/garg.mdl models/hostage.mdl");
	t->fallbackGlowColor = Vector(160, 0, 160);
	Q_strlcpy(t->sound[LSS_IDLE], "garg/gar_idle1.wav garg/gar_idle2.wav garg/gar_idle3.wav garg/gar_breathe1.wav");
	Q_strlcpy(t->sound[LSS_ALERT], "garg/gar_alert1.wav garg/gar_alert2.wav garg/gar_alert3.wav");
	Q_strlcpy(t->sound[LSS_ATTACK], "garg/gar_attack1.wav garg/gar_attack2.wav garg/gar_attack3.wav");
	Q_strlcpy(t->sound[LSS_HIT], "garg/gar_stomp1.wav | weapons/knife_hit1.wav");
	Q_strlcpy(t->sound[LSS_PAIN], "garg/gar_pain1.wav garg/gar_pain2.wav garg/gar_pain3.wav | player/pl_pain2.wav");
	Q_strlcpy(t->sound[LSS_DIE], "garg/gar_die1.wav garg/gar_die2.wav | player/die1.wav");

	t = LsAddType("headcrab");
	Q_strlcpy(t->display, "Headcrab");
	t->health = 25.0f;
	t->speed = 200.0f;
	t->walkSpeed = 60.0f;
	t->damage = 8.0f;
	t->attackRange = 72.0f;
	t->attackDelay = 0.3f;
	t->detectRadius = 800.0f;
	t->bloodColor = BLOOD_COLOR_YELLOW;
	Q_strlcpy(t->models, "models/headcrab.mdl models/hostage.mdl");
	Q_strlcpy(t->anim[LSAN_ATTACK], "jump");	// (its leap is a range attack in the model; MELEE_ATTACK2 is a looping turn)
	t->hullMin = Vector(-16, -16, 0);
	t->hullMax = Vector(16, 16, 36);
	t->fallbackGlowColor = Vector(200, 200, 120);
	Q_strlcpy(t->sound[LSS_IDLE], "headcrab/hc_idle1.wav headcrab/hc_idle2.wav headcrab/hc_idle3.wav headcrab/hc_idle4.wav headcrab/hc_idle5.wav");
	Q_strlcpy(t->sound[LSS_ALERT], "headcrab/hc_alert1.wav headcrab/hc_alert2.wav");
	Q_strlcpy(t->sound[LSS_ATTACK], "headcrab/hc_attack1.wav headcrab/hc_attack2.wav headcrab/hc_attack3.wav");
	Q_strlcpy(t->sound[LSS_HIT], "headcrab/hc_headbite.wav | weapons/knife_hit1.wav");
	t->sound[LSS_MISS][0] = '\0';
	Q_strlcpy(t->sound[LSS_PAIN], "headcrab/hc_pain1.wav headcrab/hc_pain2.wav headcrab/hc_pain3.wav | player/pl_pain2.wav");
	Q_strlcpy(t->sound[LSS_DIE], "headcrab/hc_die1.wav headcrab/hc_die2.wav | player/die1.wav");

	// Half-Life's other monsters (valve/models, the depot every CS install shares)
	t = LsAddType("vortigaunt");
	Q_strlcpy(t->display, "Vortigaunt");
	t->category = LSC_ALIEN;
	t->health = 70.0f;
	t->speed = 190.0f;
	t->walkSpeed = 60.0f;
	t->attack = LSA_RANGED;			// the electric zap: "zapattack1" by its range-attack activity
	t->shotFx = 1;
	t->damage = 12.0f;
	t->attackRange = 750.0f;
	t->attackRate = 0.6f;
	t->attackDelay = 0.8f;				// (the charge-up is part of the swing: it can be shot before it lets go)
	t->spread = 2.0f;
	t->bloodColor = BLOOD_COLOR_YELLOW;
	Q_strlcpy(t->models, "models/islave.mdl models/hostage.mdl");
	t->fallbackGlowColor = Vector(60, 200, 60);
	Q_strlcpy(t->sound[LSS_IDLE], "aslave/slv_word1.wav aslave/slv_word2.wav aslave/slv_word3.wav aslave/slv_word4.wav");
	Q_strlcpy(t->sound[LSS_ALERT], "aslave/slv_alert1.wav aslave/slv_alert3.wav aslave/slv_alert4.wav");
	Q_strlcpy(t->sound[LSS_ATTACK], "weapons/electro4.wav weapons/electro5.wav weapons/electro6.wav");
	Q_strlcpy(t->sound[LSS_HIT], "");
	t->sound[LSS_MISS][0] = '\0';
	Q_strlcpy(t->sound[LSS_PAIN], "aslave/slv_pain1.wav aslave/slv_pain2.wav | player/pl_pain2.wav");
	Q_strlcpy(t->sound[LSS_DIE], "aslave/slv_die1.wav aslave/slv_die2.wav | player/die1.wav");

	// (Half-Life's own baby gargantua model isn't in the shared depot: the gargantua, smaller)
	t = LsAddType("babygarg");
	t->enrageBelow = 0.4f;			// a heavy: enraged below 40%
	Q_strlcpy(t->display, "Baby gargantua");
	t->category = LSC_BOSS;
	t->health = 900.0f;
	t->speed = 210.0f;
	t->walkSpeed = 80.0f;
	t->damage = 35.0f;
	t->attackRange = 72.0f;
	t->attackRate = 0.8f;
	t->attackDelay = 0.5f;
	t->detectRadius = 1600.0f;
	t->behaviour = LSB_CHASE;
	t->scale = 0.45f;
	t->frags = 5;
	t->bloodColor = BLOOD_COLOR_YELLOW;
	Q_strlcpy(t->models, "models/babygarg.mdl models/garg.mdl models/hostage.mdl");
	t->fallbackGlowColor = Vector(160, 0, 160);
	Q_strlcpy(t->sound[LSS_IDLE], "garg/gar_idle1.wav garg/gar_idle2.wav garg/gar_breathe1.wav");
	Q_strlcpy(t->sound[LSS_ALERT], "garg/gar_alert1.wav garg/gar_alert2.wav");
	Q_strlcpy(t->sound[LSS_ATTACK], "garg/gar_attack1.wav garg/gar_attack2.wav");
	Q_strlcpy(t->sound[LSS_HIT], "garg/gar_stomp1.wav | weapons/knife_hit1.wav");
	Q_strlcpy(t->sound[LSS_PAIN], "garg/gar_pain1.wav garg/gar_pain2.wav | player/pl_pain2.wav");
	Q_strlcpy(t->sound[LSS_DIE], "garg/gar_die1.wav | player/die1.wav");

	t = LsAddType("snark");
	Q_strlcpy(t->display, "Snark");
	t->health = 6.0f;
	t->speed = 320.0f;
	t->walkSpeed = 160.0f;
	t->damage = 4.0f;
	t->attackRange = 48.0f;
	t->attackRate = 2.5f;
	t->attackDelay = 0.1f;
	t->detectRadius = 1000.0f;
	t->detectFov = 360.0f;
	t->behaviour = LSB_CHASE;
	t->corpseTime = 0.5f;
	t->frags = 0;
	t->bloodColor = BLOOD_COLOR_YELLOW;
	Q_strlcpy(t->models, "models/w_squeak.mdl models/headcrab.mdl");
	Q_strlcpy(t->anim[LSAN_IDLE], "idle");			// (w_squeak.mdl has no activities)
	Q_strlcpy(t->anim[LSAN_WALK], "run");
	Q_strlcpy(t->anim[LSAN_RUN], "run");
	Q_strlcpy(t->anim[LSAN_ATTACK], "jump");
	t->hullMin = Vector(-8, -8, 0);
	t->hullMax = Vector(8, 8, 30);
	t->fallbackGlowColor = Vector(200, 60, 60);
	for (int s = 0; s < LSS_COUNT; s++)
		t->sound[s][0] = '\0';
	Q_strlcpy(t->sound[LSS_IDLE], "squeek/sqk_hunt1.wav squeek/sqk_hunt2.wav squeek/sqk_hunt3.wav");
	Q_strlcpy(t->sound[LSS_ALERT], "squeek/sqk_deploy1.wav");
	Q_strlcpy(t->sound[LSS_ATTACK], "squeek/sqk_hunt2.wav");
	Q_strlcpy(t->sound[LSS_DIE], "squeek/sqk_blast1.wav squeek/sqk_die1.wav");

	t = LsAddType("babycrab");
	Q_strlcpy(t->display, "Baby headcrab");
	t->health = 8.0f;
	t->speed = 230.0f;
	t->walkSpeed = 80.0f;
	t->damage = 3.0f;
	t->attackRange = 56.0f;
	t->attackRate = 2.0f;
	t->attackDelay = 0.2f;
	t->detectRadius = 900.0f;
	t->behaviour = LSB_CHASE;
	t->corpseTime = 1.5f;
	t->frags = 0;
	t->bloodColor = BLOOD_COLOR_YELLOW;
	Q_strlcpy(t->models, "models/baby_headcrab.mdl models/headcrab.mdl");	// (its leap is its range-attack sequence)
	t->hullMin = Vector(-8, -8, 0);
	t->hullMax = Vector(8, 8, 30);
	t->fallbackGlowColor = Vector(200, 200, 120);
	for (int s = 0; s < LSS_COUNT; s++)
		t->sound[s][0] = '\0';
	Q_strlcpy(t->sound[LSS_ATTACK], "headcrab/hc_attack1.wav headcrab/hc_attack2.wav");
	Q_strlcpy(t->sound[LSS_HIT], "headcrab/hc_headbite.wav");
	Q_strlcpy(t->sound[LSS_DIE], "headcrab/hc_die1.wav headcrab/hc_die2.wav");

	t = LsAddType("miniturret");
	Q_strlcpy(t->display, "Mini turret");
	t->category = LSC_MECHANICAL;
	t->health = 80.0f;
	t->moves = false;
	t->speed = 0.0f;
	t->walkSpeed = 0.0f;
	t->attack = LSA_RANGED;
	t->damage = 4.0f;
	t->attackRange = 900.0f;
	t->attackRate = 6.0f;
	t->attackDelay = 0.05f;
	t->spread = 4.0f;
	t->detectFov = 360.0f;
	t->hearScale = 0.0f;
	t->behaviour = LSB_GUARD;
	t->bloodColor = DONT_BLEED;
	t->corpseTime = 15.0f;
	Q_strlcpy(t->models, "models/miniturret.mdl models/sentry.mdl");
	Q_strlcpy(t->anim[LSAN_IDLE], "spin");
	Q_strlcpy(t->anim[LSAN_ATTACK], "fire");
	Q_strlcpy(t->anim[LSAN_DIE], "die");
	t->hullMin = Vector(-16, -16, 0);
	t->hullMax = Vector(16, 16, 32);
	for (int s = 0; s < LSS_COUNT; s++)
		t->sound[s][0] = '\0';
	Q_strlcpy(t->sound[LSS_ALERT], "turret/tu_alert.wav turret/tu_spinup.wav");
	Q_strlcpy(t->sound[LSS_ATTACK], "turret/tu_fire1.wav");
	Q_strlcpy(t->sound[LSS_DIE], "turret/tu_die.wav turret/tu_die2.wav turret/tu_die3.wav");

	t = LsAddType("assassin");
	Q_strlcpy(t->display, "Assassin");
	t->category = LSC_SPECIAL;
	t->health = 60.0f;
	t->speed = 320.0f;
	t->walkSpeed = 90.0f;
	t->attack = LSA_RANGED;			// a silenced pistol ("shoot")
	t->damage = 8.0f;
	t->attackRange = 650.0f;
	t->attackRate = 2.0f;
	t->attackDelay = 0.15f;
	t->spread = 4.0f;
	t->hops = 0.35f;
	t->flank = true;
	t->fleeHealth = 0.3f;
	t->detectFov = 200.0f;
	Q_strlcpy(t->models, "models/hassassin.mdl models/hostage.mdl");
	t->fallbackGlowColor = Vector(20, 20, 20);
	for (int s = 0; s < LSS_COUNT; s++)
		t->sound[s][0] = '\0';
	Q_strlcpy(t->sound[LSS_ATTACK], "weapons/pl_gun1.wav weapons/pl_gun2.wav | weapons/usp1.wav");
	Q_strlcpy(t->sound[LSS_PAIN], "player/pl_pain2.wav player/pl_pain4.wav");
	Q_strlcpy(t->sound[LSS_DIE], "player/die1.wav player/die3.wav");

	t = LsAddType("soldier");
	Q_strlcpy(t->display, "Soldier");
	t->category = LSC_SPECIAL;
	t->health = 110.0f;
	t->speed = 250.0f;
	t->walkSpeed = 70.0f;
	t->attack = LSA_RANGED;			// MP5 bursts
	t->damage = 6.0f;
	t->attackRange = 1200.0f;
	t->attackRate = 4.0f;
	t->attackDelay = 0.1f;
	t->spread = 5.0f;
	t->flank = true;
	t->fleeHealth = 0.25f;
	t->bloodColor = BLOOD_COLOR_RED;
	Q_strlcpy(t->models, "models/hgrunt.mdl models/player/gign/gign.mdl");
	Q_strlcpy(t->anim[LSAN_ATTACK], "standing_mp5");
	t->fallbackGlowColor = Vector(0, 0, 0);
	t->fallbackGlow = false;
	for (int s = 0; s < LSS_COUNT; s++)
		t->sound[s][0] = '\0';
	Q_strlcpy(t->say[LSS_IDLE], "HG_IDLE");		// (Half-Life's grunt chatter, through sentences: no slots)
	Q_strlcpy(t->say[LSS_ALERT], "HG_ALERT");
	Q_strlcpy(t->sound[LSS_ATTACK], "hgrunt/gr_mgun1.wav hgrunt/gr_mgun2.wav hgrunt/gr_mgun3.wav | weapons/mp5-1.wav");
	Q_strlcpy(t->sound[LSS_PAIN], "hgrunt/gr_pain1.wav hgrunt/gr_pain2.wav hgrunt/gr_pain3.wav | player/pl_pain2.wav");
	Q_strlcpy(t->sound[LSS_DIE], "hgrunt/gr_die1.wav hgrunt/gr_die2.wav hgrunt/gr_die3.wav | player/die1.wav");

	t = LsAddType("gonarch");
	t->enrageBelow = 0.4f;			// a heavy: enraged below 40%
	Q_strlcpy(t->display, "Gonarch");
	t->category = LSC_BOSS;
	t->health = 3500.0f;
	t->speed = 190.0f;
	t->walkSpeed = 90.0f;
	t->damage = 50.0f;
	t->attackRange = 96.0f;
	t->attackRate = 0.7f;
	t->attackDelay = 0.6f;
	t->detectRadius = 2000.0f;
	t->behaviour = LSB_CHASE;
	t->scale = 0.55f;					// (she fills a room at 1; the hull stays player-sized)
	t->frags = 10;
	t->bloodColor = BLOOD_COLOR_YELLOW;
	Q_strlcpy(t->models, "models/big_mom.mdl models/hostage.mdl");
	t->fallbackGlowColor = Vector(200, 160, 40);
	for (int s = 0; s < LSS_COUNT; s++)
		t->sound[s][0] = '\0';
	Q_strlcpy(t->sound[LSS_IDLE], "gonarch/gon_sack1.wav gonarch/gon_sack2.wav gonarch/gon_sack3.wav");
	Q_strlcpy(t->sound[LSS_ALERT], "gonarch/gon_alert1.wav gonarch/gon_alert2.wav gonarch/gon_alert3.wav");
	Q_strlcpy(t->sound[LSS_ATTACK], "gonarch/gon_attack1.wav gonarch/gon_attack2.wav gonarch/gon_attack3.wav");
	Q_strlcpy(t->sound[LSS_HIT], "gonarch/gon_step1.wav | weapons/knife_hit1.wav");
	Q_strlcpy(t->sound[LSS_PAIN], "gonarch/gon_pain2.wav gonarch/gon_pain4.wav gonarch/gon_pain5.wav | player/pl_pain2.wav");
	Q_strlcpy(t->sound[LSS_DIE], "gonarch/gon_die1.wav | player/die1.wav");
}

// Picks the first group of a "a b | c d" list that has a file present (sounds), keeps up to max
static int LsPickFiles(const char *list, const char *prefix, char out[][64], int max)
{
	char buf[256];
	Q_strlcpy(buf, list);

	int count = 0;
	char *group = buf;
	while (group && *group && count == 0)
	{
		char *next = Q_strchr(group, '|');
		if (next)
			*next++ = '\0';

		char *save = group;
		while (*save && count < max)
		{
			while (*save == ' ' || *save == '\t')
				save++;

			char *end = save;
			while (*end && *end != ' ' && *end != '\t')
				end++;

			if (end == save)
				break;

			char one[64];
			int len = Q_min(int(end - save), int(sizeof(one) - 1));
			Q_strncpy(one, save, len);
			one[len] = '\0';

			char path[96];
			Q_snprintf(path, sizeof(path), "%s%s", prefix, one);
			if (g_pFileSystem->FileExists(path))
				Q_strlcpy(out[count++], one);

			save = end;
		}

		group = next;
	}

	return count;
}

static void LsSetTypeKey(LsCreatureType *t, const char *key, const char *value)
{
	if (!Q_stricmp(key, "base"))
	{
		// inherit everything from a type above (keeps the name)
		for (int b = 0; b < s_buildCount; b++)
		{
			if (&s_build[b] != t && !Q_stricmp(s_build[b].name, value))
			{
				char name[32];
				Q_strlcpy(name, t->name);
				*t = s_build[b];
				Q_strlcpy(t->name, name);
				break;
			}
		}
	}
	else if (!Q_stricmp(key, "name") || !Q_stricmp(key, "display")) Q_strlcpy(t->display, value);
	else if (!Q_stricmp(key, "category")) t->category = LsNameIndex(value, s_categoryNames, LSC_CATEGORY_COUNT, t->category);
	else if (!Q_stricmp(key, "hp") || !Q_stricmp(key, "health")) t->health = LsNum(value, 1.0f, 100000.0f, t->health);
	else if (!Q_stricmp(key, "speed")) t->speed = LsNum(value, 0.0f, 2000.0f, t->speed);
	else if (!Q_stricmp(key, "walk_speed")) t->walkSpeed = LsNum(value, 0.0f, 2000.0f, t->walkSpeed);
	else if (!Q_stricmp(key, "aggression")) t->aggression = LsNum(value, 0.0f, 1.0f, t->aggression);
	else if (!Q_stricmp(key, "detect_radius") || !Q_stricmp(key, "detect")) t->detectRadius = LsNum(value, 0.0f, 8192.0f, t->detectRadius);
	else if (!Q_stricmp(key, "detect_fov") || !Q_stricmp(key, "fov")) t->detectFov = LsNum(value, 10.0f, 360.0f, t->detectFov);
	else if (!Q_stricmp(key, "hearing")) t->hearScale = LsNum(value, 0.0f, 10.0f, t->hearScale);
	else if (!Q_stricmp(key, "attack")) t->attack = LsNameIndex(value, s_attackNames, ARRAYSIZE(s_attackNames), t->attack);
	else if (!Q_stricmp(key, "damage")) t->damage = LsNum(value, 0.0f, 10000.0f, t->damage);
	else if (!Q_stricmp(key, "attack_range")) t->attackRange = LsNum(value, 16.0f, 8192.0f, t->attackRange);
	else if (!Q_stricmp(key, "attack_rate")) t->attackRate = LsNum(value, 0.1f, 20.0f, t->attackRate);
	else if (!Q_stricmp(key, "attack_delay")) t->attackDelay = LsNum(value, 0.0f, 3.0f, t->attackDelay);
	else if (!Q_stricmp(key, "spread")) t->spread = LsNum(value, 0.0f, 45.0f, t->spread);
	else if (!Q_stricmp(key, "flee_hp")) t->fleeHealth = LsNum(value, 0.0f, 1.0f, t->fleeHealth);
	else if (!Q_stricmp(key, "flee_home")) t->fleeHome = Q_atoi(value) != 0;
	else if (!Q_stricmp(key, "behaviour") || !Q_stricmp(key, "behavior")) t->behaviour = LsNameIndex(value, s_behaviourNames, LSB_COUNT, t->behaviour);
	else if (!Q_stricmp(key, "radius") || !Q_stricmp(key, "guard_radius")) t->guardRadius = LsNum(value, 32.0f, 8192.0f, t->guardRadius);
	else if (!Q_stricmp(key, "target")) t->targeting = LsNameIndex(value, s_targetingNames, ARRAYSIZE(s_targetingNames), t->targeting);
	else if (!Q_stricmp(key, "hates")) t->hates = LsHatesFromString(value);
	else if (!Q_stricmp(key, "respawn")) t->respawn = LsNum(value, 0.0f, 3600.0f, t->respawn);
	else if (!Q_stricmp(key, "corpse_time")) t->corpseTime = LsNum(value, 0.0f, 120.0f, t->corpseTime);
	else if (!Q_stricmp(key, "moves")) t->moves = Q_atoi(value) != 0;
	else if (!Q_stricmp(key, "can_climb")) t->canClimb = Q_atoi(value) != 0;
	else if (!Q_strnicmp(key, "say_", 4))
	{
		// say_idle = HG_IDLE: a sentence group for that kind of sound
		static const char *kinds[LSS_COUNT] = { "idle", "alert", "attack", "hit", "miss", "pain", "die" };
		for (int k = 0; k < LSS_COUNT; k++)
			if (!Q_stricmp(key + 4, kinds[k]))
				Q_strlcpy(t->say[k], value);
	}
	else if (!Q_stricmp(key, "shot_fx")) t->shotFx = !Q_stricmp(value, "zap") ? 1 : !Q_stricmp(value, "none") ? 2 : 0;
	else if (!Q_stricmp(key, "hops")) t->hops = LsNum(value, 0.0f, 1.0f, t->hops);
	else if (!Q_stricmp(key, "flank")) t->flank = Q_atoi(value) != 0;
	else if (!Q_stricmp(key, "friendly")) t->friendly = Q_atoi(value) != 0;
	else if (!Q_stricmp(key, "enrage_below")) t->enrageBelow = LsNum(value, 0.0f, 0.95f, t->enrageBelow);
	else if (!Q_stricmp(key, "enrage_speed")) t->enrageSpeed = LsNum(value, 1.0f, 3.0f, t->enrageSpeed);
	else if (!Q_stricmp(key, "enrage_damage")) t->enrageDamage = LsNum(value, 1.0f, 5.0f, t->enrageDamage);
	else if (!Q_stricmp(key, "enrage_anim")) t->enrageAnim = LsNum(value, 1.0f, 3.0f, t->enrageAnim);
	else if (!Q_stricmp(key, "tier2_skin") || !Q_stricmp(key, "tier3_skin")) t->tierSkin[key[4] - '2'] = clamp(Q_atoi(value), -1, 255);
	else if (!Q_stricmp(key, "tier2_body") || !Q_stricmp(key, "tier3_body")) t->tierBody[key[4] - '2'] = clamp(Q_atoi(value), -1, 255);
	else if (!Q_stricmp(key, "tier2_hp") || !Q_stricmp(key, "tier3_hp")) t->tierHealth[key[4] - '2'] = LsNum(value, 0.1f, 20.0f, t->tierHealth[key[4] - '2']);
	else if (!Q_stricmp(key, "frags")) t->frags = clamp(Q_atoi(value), -100, 100);
	else if (!Q_stricmp(key, "money")) t->money = clamp(Q_atoi(value), -16000, 16000);
	else if (!Q_stricmp(key, "model") || !Q_stricmp(key, "models")) Q_strlcpy(t->models, value);
	else if (!Q_stricmp(key, "skin")) t->skin = clamp(Q_atoi(value), 0, 255);
	else if (!Q_stricmp(key, "body")) t->body = clamp(Q_atoi(value), 0, 255);
	else if (!Q_stricmp(key, "scale")) t->scale = LsNum(value, 0.1f, 4.0f, t->scale);
	else if (!Q_stricmp(key, "hull_min")) t->hullMin = LsParseVector(value, t->hullMin);
	else if (!Q_stricmp(key, "hull_max")) t->hullMax = LsParseVector(value, t->hullMax);
	else if (!Q_stricmp(key, "glow"))
	{
		t->glowColor = LsParseColor(value, t->glowColor);
		t->glow = !t->glowColor.IsZero();
	}
	else if (!Q_stricmp(key, "fallback_glow"))
	{
		t->fallbackGlowColor = LsParseColor(value, t->fallbackGlowColor);
		t->fallbackGlow = !t->fallbackGlowColor.IsZero();
	}
	else if (!Q_stricmp(key, "blood")) t->bloodColor = !Q_stricmp(value, "yellow") ? BLOOD_COLOR_YELLOW : !Q_stricmp(value, "none") ? DONT_BLEED : BLOOD_COLOR_RED;
	else if (!Q_stricmp(key, "sounds"))
	{
		// (the plan's short form: one list of everything; the idle/attack split is by file name)
		for (int s = 0; s < LSS_COUNT; s++)
			t->sound[s][0] = '\0';
		Q_strlcpy(t->sound[LSS_IDLE], value);
	}
	else if (!Q_stricmp(key, "ls"))
	{
		// the plan's backend block: ls = model=<path> anims=<walk> <run> <attack> <die>
		char buf[256];
		Q_strlcpy(buf, value);
		const int animOrder[] = { LSAN_WALK, LSAN_RUN, LSAN_ATTACK, LSAN_DIE };
		int animNext = -1;
		char *cursor = buf;
		char *tok;
		while ((tok = LsNextToken(&cursor, " \t")))
		{
			if (!Q_strnicmp(tok, "model=", 6))
			{
				Q_strlcpy(t->models, tok + 6);
				animNext = -1;
			}
			else if (!Q_strnicmp(tok, "anims=", 6))
			{
				animNext = 0;
				if (tok[6])
					Q_strlcpy(t->anim[animOrder[animNext++]], tok + 6);
			}
			else if (animNext >= 0 && animNext < int(ARRAYSIZE(animOrder)))
			{
				Q_strlcpy(t->anim[animOrder[animNext++]], tok);
			}
		}
	}
	else
	{
		for (int a = 0; a < LSAN_COUNT; a++)
		{
			if (!Q_stricmp(key, s_animKeys[a]))
			{
				Q_strlcpy(t->anim[a], value);
				return;
			}
		}
		for (int s = 0; s < LSS_COUNT; s++)
		{
			if (!Q_stricmp(key, s_soundKeys[s]))
			{
				Q_strlcpy(t->sound[s], value);
				return;
			}
		}
		// (mm = ..., bot = ...: the kit's backends, not ours)
	}
}

static char *LsTrim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;

	char *end = s + Q_strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
		*--end = '\0';

	return s;
}

// A type's hull: finite, min below max on each axis, low enough to crouch (Crouch caps the top
// at 30) - else the default human hull, with a word on the console
static void LsCheckHull(LsCreatureType *t)
{
	bool ok = t->hullMin.z <= 0.0f && t->hullMin.z >= -64.0f && t->hullMax.z >= 30.0f;
	for (int i = 0; i < 3 && ok; i++)
	{
		if (!LsFinite(t->hullMin[i]) || !LsFinite(t->hullMax[i]) || t->hullMin[i] >= t->hullMax[i]
			|| t->hullMax[i] - t->hullMin[i] > 256.0f)
			ok = false;
	}

	if (!ok)
	{
		CONSOLE_ECHO("ls_creature: type '%s' has a bad hull (%.0f %.0f %.0f / %.0f %.0f %.0f), using the default\n", t->name,
			t->hullMin.x, t->hullMin.y, t->hullMin.z, t->hullMax.x, t->hullMax.y, t->hullMax.z);
		t->hullMin = Vector(-16, -16, 0);
		t->hullMax = Vector(16, 16, 72);
	}

	// the engine moves monsters with the player hulls by size: up to 36 wide it's the standing
	// (32x32x72) or the crouched (32x32x36) one, wider is the 64x64x64 "large" hull that doesn't
	// fit through doors the nav mesh (made for players) leads through - so never wider than a player
	if (t->hullMin.x < -16.0f || t->hullMin.y < -16.0f || t->hullMax.x > 16.0f || t->hullMax.y > 16.0f)
	{
		CONSOLE_ECHO("ls_creature: type '%s' is wider than a player; its hull is narrowed to 32 (the nav mesh is for player-sized hulls)\n", t->name);
		t->hullMin.x = t->hullMin.y = -16.0f;
		t->hullMax.x = t->hullMax.y = 16.0f;
	}
}

static void LsLoadTypes()
{
	s_typesLoaded = true;
	s_buildCount = 0;
	LsBuiltinTypes();

	int length = 0;
	char *data = (char *)LOAD_FILE_FOR_ME(ls_creature_file.string, &length);
	if (data)
	{
		LsCreatureType *cur = nullptr;
		char *p = data;
		char *end = data + length;
		while (p < end)
		{
			char line[512];
			int n = 0;
			while (p < end && *p != '\n' && n < int(sizeof(line)) - 1)
				line[n++] = *p++;
			while (p < end && *p != '\n')
				p++;
			p++;
			line[n] = '\0';

			// (comments: ; # //)
			for (char *c = line; *c; c++)
			{
				if (*c == ';' || *c == '#' || (c[0] == '/' && c[1] == '/'))
				{
					*c = '\0';
					break;
				}
			}

			char *s = LsTrim(line);
			if (!*s)
				continue;

			if (*s == '[')
			{
				char *close = Q_strchr(s, ']');
				if (close)
					*close = '\0';
				cur = LsAddType(LsTrim(s + 1));
				if (!cur)
					CONSOLE_ECHO("ls_creature: too many types (max %d), '%s' skipped\n", LS_CREATURE_MAX_TYPES, s + 1);
				continue;
			}

			char *eq = Q_strchr(s, '=');
			if (!eq || !cur)
				continue;

			*eq = '\0';
			LsSetTypeKey(cur, LsTrim(s), LsTrim(eq + 1));
		}

		FREE_FILE(data);
	}

	// merge by name: a slot never moves or goes away while the server runs
	for (int i = 0; i < s_typeCount; i++)
		s_types[i].disabled = true;

	for (int b = 0; b < s_buildCount; b++)
	{
		LsCreatureType *fresh = &s_build[b];
		if (fresh->walkSpeed > fresh->speed)
			fresh->walkSpeed = fresh->speed;
		LsCheckHull(fresh);

		int slot = 0;
		while (slot < s_typeCount && Q_stricmp(s_types[slot].name, fresh->name))
			slot++;

		if (slot == s_typeCount)
		{
			if (s_typeCount >= LS_CREATURE_MAX_TYPES)
			{
				CONSOLE_ECHO("ls_creature: no room for new type '%s' until the server restarts\n", fresh->name);
				continue;
			}

			s_types[s_typeCount++] = *fresh;
			continue;
		}

		LsCreatureType *t = &s_types[slot];
		if (!t->precached)
		{
			*t = *fresh;
			continue;
		}

		// precached this map: the files (and the strings the engine holds) stay; the rest updates
		LsCreatureType keep = *t;
		*t = *fresh;
		t->precached = true;
		t->usingFallback = keep.usingFallback;
		t->iszName = keep.iszName;
		t->iszModel = keep.iszModel;
		Q_strlcpy(t->models, keep.models);
		Q_strlcpy(t->model, keep.model);
		t->hullMin = keep.hullMin;
		t->hullMax = keep.hullMax;
		Q_memcpy(t->sound, keep.sound, sizeof(t->sound));
		Q_memcpy(t->soundCount, keep.soundCount, sizeof(t->soundCount));
		Q_memcpy(t->soundFile, keep.soundFile, sizeof(t->soundFile));
		Q_memcpy(t->iszSound, keep.iszSound, sizeof(t->iszSound));
	}
}

int LsCreatureTypeIndex(const char *name)
{
	if (!s_typesLoaded)
		LsLoadTypes();

	if (!name || !*name)
		return -1;

	for (int i = 0; i < s_typeCount; i++)
	{
		if (!s_types[i].disabled && !Q_stricmp(s_types[i].name, name))
			return i;
	}

	return -1;
}

LsCreatureType *LsCreatureTypeAt(int index)
{
	if (!s_typesLoaded)
		LsLoadTypes();

	return (index >= 0 && index < s_typeCount) ? &s_types[index] : nullptr;
}

// Half-Life's models as current Steam has them (after the 25th anniversary update). The
// server picks a sequence by its number in its own copy, and each player's game draws that
// number from its copy: zombie.mdl gained a sequence near the start in that update, so an older
// server copy (HLDS from SteamCMD still has it) made "walk" the game's "attack2" and "attack1" its
// "falling". Size, CRC32 and sequence count of each file (Steam, 2026-11).
struct LsKnownModel
{
	const char *path;
	int size;
	unsigned int crc;
	int sequences;
};

static const LsKnownModel s_knownModels[] =
{
	{ "models/zombie.mdl", 647420, 0x1fb1b858u, 49 },
	{ "models/bullsquid.mdl", 333568, 0x72f5f488u, 24 },
	{ "models/houndeye.mdl", 116196, 0xca66883fu, 31 },
	{ "models/agrunt.mdl", 152320, 0x51974987u, 32 },
	{ "models/garg.mdl", 182172, 0x8eae0dc0u, 22 },
	{ "models/headcrab.mdl", 70024, 0xa226e829u, 21 },
	{ "models/sentry.mdl", 17744, 0x0027092du, 6 },
	{ "models/barney.mdl", 574100, 0x08b4d0d6u, 71 },
	{ "models/gman.mdl", 227952, 0x6ac90dbcu, 14 },
	{ "models/islave.mdl", 145312, 0x1b228735u, 30 },
	{ "models/hassassin.mdl", 257380, 0xf9f7b995u, 17 },
	{ "models/hgrunt.mdl", 608952, 0x7d6126f6u, 86 },			// (HL25: 82 -> 86 sequences)
	{ "models/big_mom.mdl", 111544, 0xddd12460u, 20 },
	{ "models/w_squeak.mdl", 12360, 0x1437f971u, 4 },
	{ "models/controller.mdl", 187652, 0xff9d759cu, 19 },
	{ "models/barnacle.mdl", 41752, 0x94c9ecf3u, 7 },
	{ "models/baby_headcrab.mdl", 36024, 0x52698e53u, 13 },
	{ "models/miniturret.mdl", 13876, 0x179014a3u, 6 },
};

// A loud word when the server's copy of a Half-Life model isn't Steam's current one: players
// would see the wrong animations (the sequences are numbered differently)
static void LsCheckModelVersion(const char *model)
{
	const LsKnownModel *known = nullptr;
	for (size_t i = 0; i < ARRAYSIZE(s_knownModels); i++)
	{
		if (!Q_stricmp(model, s_knownModels[i].path))
			known = &s_knownModels[i];
	}

	if (!known)
		return;

	int length = 0;
	byte *data = LOAD_FILE_FOR_ME((char *)model, &length);
	if (!data)
		return;

	int sequences = (length >= int(sizeof(studiohdr_t))) ? ((studiohdr_t *)data)->numseq : -1;
	CRC32_t crc;
	CRC32_INIT(&crc);
	CRC32_PROCESS_BUFFER(&crc, data, length);
	crc = CRC32_FINAL(crc);
	FREE_FILE(data);

	if (length == known->size && sequences == known->sequences && crc == known->crc)
		return;

	if (length != known->size || sequences != known->sequences)
	{
		CONSOLE_ECHO("ls_creature: WARNING: this server's %s isn't the one current Half-Life games have (%d sequences, %d bytes; Steam's has %d, %d).\n"
			"ls_creature: WARNING: players will see the wrong animations. Copy valve/%s from an up-to-date Half-Life (the Loner-Strike app does it at each start).\n",
			model, sequences, length, known->sequences, known->size, model);
	}
	else if (ls_creature_debug.value > 0.0f)
	{
		CONSOLE_ECHO("ls_creature: %s differs from Steam's copy (CRC) but has the same layout\n", model);
	}
}

// Precache one type's model and sounds: only while the map loads (the engine refuses later)
// ---- the precache budget -------------------------------------------------------------------
//
// The engine has 512 model and 512 sound slots, and a map that asks for one more fails to load.
// Every PRECACHE_* returns the slot it got, so the highest slot seen is how full the table is (an
// item already there gives its old, lower slot). Creatures keep a reserve free (ls_creature_reserve
// "40 60") and, short of room, drop a type's lesser sounds first, then the type itself, with a word
// on the console and in the log: never a failed map.
const int LS_MAX_MODELS = 512;
const int LS_MAX_SOUNDS = 512;
static int s_modelHigh = 0;		// highest model slot seen this map
static int s_soundHigh = 0;
static bool s_precacheOpen = false;	// the late window: ServerActivate, everything else already in
static char s_skipped[256] = "";

static int LsPrecacheModel(const char *name)
{
	int slot = PRECACHE_MODEL((char *)name);
	if (ls_creature_debug.value > 1.0f)
		CONSOLE_ECHO("ls_creature: precache model %s -> slot %d%s\n", name, slot, slot > s_modelHigh ? " (new)" : "");
	s_modelHigh = Q_max(s_modelHigh, slot);
	return slot;
}

static int LsPrecacheSound(const char *name)
{
	int slot = PRECACHE_SOUND((char *)name);
	if (ls_creature_debug.value > 1.0f)
		CONSOLE_ECHO("ls_creature: precache sound %s -> slot %d%s\n", name, slot, slot > s_soundHigh ? " (new)" : "");
	s_soundHigh = Q_max(s_soundHigh, slot);
	return slot;
}

static void LsReserve(int *models, int *sounds)
{
	*models = 40;
	*sounds = 60;
	sscanf(ls_creature_reserve.string, "%d %d", models, sounds);
	*models = clamp(*models, 0, 256);
	*sounds = clamp(*sounds, 0, 256);
}

// (a sound this map already has from us: no new slot)
static bool LsSoundOurs(const char *file)
{
	for (int i = 0; i < s_typeCount; i++)
	{
		const LsCreatureType *o = &s_types[i];
		if (!o->precached)
			continue;
		for (int k = 0; k < LSS_COUNT; k++)
			for (int j = 0; j < o->soundCount[k]; j++)
				if (!Q_stricmp(o->soundFile[k][j], file))
					return true;
	}
	return false;
}

static void LsNoteSkipped(const LsCreatureType *t, const char *why)
{
	CONSOLE_ECHO("ls_creature: '%s' %s (precache budget: models %d/%d, sounds %d/%d, reserve %s)\n", t->name, why,
		s_modelHigh, LS_MAX_MODELS, s_soundHigh, LS_MAX_SOUNDS, ls_creature_reserve.string);
	UTIL_LogPrintf("[ls_creature] \"%s\" %s (models %d/%d, sounds %d/%d)\n", t->name, why, s_modelHigh, LS_MAX_MODELS, s_soundHigh, LS_MAX_SOUNDS);
}

// Precache one type's model and sounds: only while the map loads (the engine refuses later)
bool LsCreaturePrecacheType(int index)
{
	LsCreatureType *t = LsCreatureTypeAt(index);
	if (!t)
		return false;

	if (t->precached)
		return true;

	if (g_bServerActive && !s_precacheOpen)
		return false;

	// the first model that's there (Half-Life's own when the install has them)
	char buf[256];
	Q_strlcpy(buf, t->models);
	t->model[0] = '\0';
	t->usingFallback = false;
	int candidate = 0;
	char *cursor = buf;
	for (char *tok; (tok = LsNextToken(&cursor, " \t")); candidate++)
	{
		if (g_pFileSystem->FileExists(tok))
		{
			Q_strlcpy(t->model, tok);
			t->usingFallback = (candidate > 0);
			break;
		}
	}

	if (!t->model[0])
	{
		CONSOLE_ECHO("ls_creature: no model found for '%s' (%s)\n", t->name, t->models);
		return false;
	}

	int reserveModels, reserveSounds;
	LsReserve(&reserveModels, &reserveSounds);

	// room for its model (and the zap's sprite)? Else the type stays out
	int modelsNeeded = 1 + ((t->shotFx == 1 && !s_zapSprite) ? 1 : 0);
	if (s_modelHigh + modelsNeeded > LS_MAX_MODELS - reserveModels)
	{
		LsNoteSkipped(t, "left out: no room for its model");
		Q_strlcat(s_skipped, t->name);
		Q_strlcat(s_skipped, " ");
		CVAR_SET_STRING("ls_creature_skipped", s_skipped);
		return false;
	}

	// its sounds: at most ls_creature_sounds of each kind (2), the files that are there
	int perKind = clamp(int(ls_creature_sounds.value), 0, LS_CREATURE_MAX_SOUNDS);
	for (int k = 0; k < LSS_COUNT; k++)
		t->soundCount[k] = Q_min(LsPickFiles(t->sound[k], "sound/", t->soundFile[k], LS_CREATURE_MAX_SOUNDS), perKind);

	// short of sound slots: the lesser kinds go first (idle chatter, the alert, a miss...), the
	// attack last; a creature without sounds is still a creature
	static const int dropOrder[LSS_COUNT] = { LSS_IDLE, LSS_ALERT, LSS_MISS, LSS_HIT, LSS_PAIN, LSS_DIE, LSS_ATTACK };
	auto newSounds = [&]() {
		int n = 0;
		for (int k = 0; k < LSS_COUNT; k++)
			for (int j = 0; j < t->soundCount[k]; j++)
				if (!LsSoundOurs(t->soundFile[k][j]))
					n++;
		return n;
	};
	int dropped = 0;
	for (int d = 0; d < LSS_COUNT && s_soundHigh + newSounds() > LS_MAX_SOUNDS - reserveSounds; d++)
	{
		dropped += t->soundCount[dropOrder[d]];
		t->soundCount[dropOrder[d]] = 0;
	}

	if (dropped)
		LsNoteSkipped(t, "kept with fewer sounds");

	LsCheckModelVersion(t->model);

	// (the engine keeps the pointer it's given: an engine string, never our arrays a reload rewrites)
	t->iszModel = ALLOC_STRING(t->model);
	LsPrecacheModel(STRING(t->iszModel));
	s_precachedModels++;

	for (int s = 0; s < LSS_COUNT; s++)
	{
		for (int i = 0; i < t->soundCount[s]; i++)
		{
			// (and one more look before each: the model may have shown the table fuller than we knew)
			if (!LsSoundOurs(t->soundFile[s][i]) && s_soundHigh + 1 > LS_MAX_SOUNDS - reserveSounds)
			{
				t->soundCount[s] = i;
				break;
			}
			t->iszSound[s][i] = ALLOC_STRING(t->soundFile[s][i]);
			LsPrecacheSound(STRING(t->iszSound[s][i]));
			s_precachedSounds++;
		}
	}

	t->iszName = ALLOC_STRING(t->name);
	t->precached = true;

	// (the zap's beam: one sprite for every type that uses it)
	if (t->shotFx == 1 && !s_zapSprite)
	{
		s_zapSprite = LsPrecacheModel("sprites/lgtning.spr");
		s_precachedModels++;
	}

	if (ls_creature_debug.value > 0.0f)
		CONSOLE_ECHO("ls_creature: precached '%s' (%s%s)\n", t->name, t->model, t->usingFallback ? ", stand-in" : "");

	return true;
}

static void LsPrecacheList()
{
	const char *want = ls_creature_precache.string;
	if (!want || !*want)
		return;

	LsLoadTypes();

	if (!Q_stricmp(want, "all"))
	{
		for (int i = 0; i < s_typeCount; i++)
			if (!s_types[i].disabled)
				LsCreaturePrecacheType(i);
	}
	else
	{
		char buf[256];
		Q_strlcpy(buf, want);
		char *cursor = buf;
		for (char *tok; (tok = LsNextToken(&cursor, " ,\t")); )
		{
			int index = LsCreatureTypeIndex(tok);
			if (index < 0)
				CONSOLE_ECHO("ls_creature: ls_creature_precache names unknown type '%s'\n", tok);
			else
				LsCreaturePrecacheType(index);
		}
	}

	CONSOLE_ECHO("ls_creature: %d models and %d sounds precached for creatures; the map now uses %d/%d models, %d/%d sounds%s%s\n",
		s_precachedModels, s_precachedSounds, s_modelHigh, LS_MAX_MODELS, s_soundHigh, LS_MAX_SOUNDS,
		s_skipped[0] ? "; left out: " : "", s_skipped);
}

// The map loads (CWorld::Precache): a fresh start; the list itself waits for ServerActivate, when
// the map's entities and the plugins have taken their slots and the count is real
void LsCreaturesPrecache()
{
	s_mapRunning = false;
	s_zapSprite = 0;
	s_precachedModels = 0;
	s_precachedSounds = 0;
	s_modelHigh = 0;
	s_soundHigh = 0;
	s_skipped[0] = '\0';
	CVAR_SET_STRING("ls_creature_skipped", "");

	// (a fresh read each map: the file may have changed, and the engine forgot the files)
	for (int i = 0; i < s_typeCount; i++)
		s_types[i].precached = false;

	s_typesLoaded = false;

	// (or now, the old way: ls_creature_precache_late 0)
	if (ls_creature_precache_late.value <= 0.0f)
		LsPrecacheList();
}

// ServerActivate, before the map runs: the engine still takes precaches here
static void LsPrecacheLate()
{
	if (ls_creature_precache_late.value <= 0.0f)
		return;

	s_precacheOpen = true;
	LsPrecacheList();
	s_precacheOpen = false;
}

// ---------------------------------------------------------------------------------------------
// The registry

static ILsCreature *s_list[LS_CREATURE_MAX_LIST];
static int s_listCount = 0;		// high-water mark (gone ones leave a null)
static int s_listLive = 0;		// non-null entries

// Adopted classnames (ls_creature adopt): entities other code made, wrapped
struct LsAdoptRule
{
	char classname[32];
	int type;
};

static LsAdoptRule s_adopt[8];
static int s_adoptCount = 0;
static float s_nextAdoptScan = 0.0f;
static float s_nextAliveCount = 0.0f;

void LsCreaturesServerActivate()
{
	LsPrecacheLate();
	s_mapRunning = true;
	Q_memset(&s_adapt, 0, sizeof(s_adapt));
	LsCrashQuietInstall();
}

bool LsCreaturesExist()
{
	return s_listLive > 0 || s_adoptCount > 0;
}

void LsCreatureRegister(ILsCreature *pCreature)
{
	for (int i = 0; i < s_listCount; i++)
	{
		if (!s_list[i])
		{
			s_list[i] = pCreature;
			s_listLive++;
			return;
		}
	}

	if (s_listCount >= LS_CREATURE_MAX_LIST)
	{
		delete pCreature;
		return;
	}

	s_list[s_listCount++] = pCreature;
	s_listLive++;
}

void LsCreatureUnregister(CBaseEntity *pEntity)
{
	for (int i = 0; i < s_listCount; i++)
	{
		if (s_list[i] && s_list[i]->Entity() == pEntity)
		{
			delete s_list[i];
			s_list[i] = nullptr;
			s_listLive--;
		}
	}

	while (s_listCount > 0 && !s_list[s_listCount - 1])
		s_listCount--;
}

int LsCreatureCount()
{
	return s_listCount;
}

ILsCreature *LsCreatureAt(int i)
{
	if (i < 0 || i >= s_listCount || !s_list[i])
		return nullptr;

	// (an entity freed behind our back - removed by other code - drops out here)
	if (!s_list[i]->Entity())
	{
		delete s_list[i];
		s_list[i] = nullptr;
		s_listLive--;
		return nullptr;
	}

	return s_list[i];
}

ILsCreature *LsCreatureOf(CBaseEntity *pEntity)
{
	if (!pEntity || !s_listLive)
		return nullptr;

	for (int i = 0; i < s_listCount; i++)
	{
		ILsCreature *c = LsCreatureAt(i);
		if (c && c->Entity() == pEntity)
			return c;
	}

	return nullptr;
}

// (friendly ones - bought turrets - have their own small cap and don't count here)
static int LsAliveCount(bool friendly)
{
	int alive = 0;
	for (int i = 0; i < s_listCount; i++)
	{
		ILsCreature *c = LsCreatureAt(i);
		if (c && c->IsAlive() && LsCreatureIsFriendly(c->Entity()) == friendly)
			alive++;
	}
	return alive;
}

static int s_friendlyAlive = 0;

bool LsCreatureIsFriendly(CBaseEntity *pEntity)
{
	ILsCreature *c = pEntity ? LsCreatureOf(pEntity) : nullptr;
	return c && !Q_strcmp(c->Provider(), "ls") && static_cast<CLsCreature *>(c->Entity())->Friendly();
}

static CLsCreature *LsNativeOf(CBaseEntity *pEntity)
{
	ILsCreature *c = pEntity ? LsCreatureOf(pEntity) : nullptr;
	return (c && c->Entity() && !Q_strcmp(c->Provider(), "ls")) ? static_cast<CLsCreature *>(c->Entity()) : nullptr;
}

static void LsUpdateAliveCount()
{
	int alive = LsAliveCount();
	s_friendlyAlive = LsAliveCount(true);

	if (int(ls_creature_alive.value) != alive)
		CVAR_SET_FLOAT("ls_creature_alive", alive);
}

static bool LsIsHostilePlayer(CBaseEntity *pOther, int hates)
{
	if (!pOther || !pOther->IsPlayer() || !pOther->IsAlive())
		return false;

	CBasePlayer *pPlayer = static_cast<CBasePlayer *>(pOther);
	if (pPlayer->GetObserverMode() != OBS_NONE || (pPlayer->m_iTeam != TERRORIST && pPlayer->m_iTeam != CT))
		return false;

	if (hates == 1 && pPlayer->m_iTeam != TERRORIST)
		return false;

	if (hates == 2 && pPlayer->m_iTeam != CT)
		return false;

	return true;
}

// The native provider's creatures, seen through the interface
class LsNativeCreature: public ILsCreature
{
public:
	LsNativeCreature(CLsCreature *pCreature) { m_handle = pCreature; }

	virtual CBaseEntity *Entity() const { return const_cast<EHANDLE &>(m_handle); }
	virtual entvars_t *Vars() const { CLsCreature *c = Get(); return c ? c->pev : nullptr; }
	virtual const char *Provider() const { return "ls"; }
	virtual const LsCreatureType *Type() const { CLsCreature *c = Get(); return c ? c->GetType() : nullptr; }
	virtual int Category() const { const LsCreatureType *t = Type(); return t ? t->category : LSC_INFECTED; }
	virtual bool IsAlive() const { CLsCreature *c = Get(); return c && c->IsAlive(); }
	virtual float Health() const { CLsCreature *c = Get(); return c ? c->pev->health : 0.0f; }
	virtual void SetTarget(CBaseEntity *pTarget) { CLsCreature *c = Get(); if (c) c->SetTarget(pTarget); }
	virtual CBaseEntity *GetTarget() const { CLsCreature *c = Get(); return c ? c->GetTarget() : nullptr; }
	virtual void SetBehaviour(int behaviour, const Vector *spot) { CLsCreature *c = Get(); if (c) c->SetBehaviour(behaviour, spot); }
	virtual int GetBehaviour() const { CLsCreature *c = Get(); return c ? c->GetBehaviour() : LSB_IDLE; }
	virtual float Threat() const
	{
		const LsCreatureType *t = Type();
		if (!t)
			return 0.5f;

		float dps = t->damage * t->attackRate;
		float threat = dps / 40.0f + (t->attack == LSA_RANGED ? 0.2f : 0.0f) + (t->category == LSC_BOSS ? 0.4f : 0.0f);
		return clamp(threat, 0.1f, 1.0f);
	}
	virtual bool IsHostileTo(CBaseEntity *pOther) const { CLsCreature *c = Get(); return c && c->IsHostileTo(pOther); }
	virtual bool LastNoise(Vector *pos, float *when, float *range) const { CLsCreature *c = Get(); return c && c->LastNoise(pos, when, range); }
	virtual void Kill() { CLsCreature *c = Get(); if (c) c->KillQuietly(); }

private:
	CLsCreature *Get() const
	{
		CBaseEntity *pEntity = const_cast<EHANDLE &>(m_handle);
		return static_cast<CLsCreature *>(pEntity);
	}

	EHANDLE m_handle;
};

// Anyone else's monster (MonsterMod's monster_*), wrapped by classname: the list, the bots and
// the events see it; its own code still runs it (target and behaviour are hints it can't take).
// Its private data isn't ours (another module's classes), so only the edict's entvars are read:
// no ReGameDLL virtual is ever called on it, and kill/remove are refused. Entity() hands out the
// pointer for identity and pev only.
class LsAdoptedCreature: public ILsCreature
{
public:
	LsAdoptedCreature(edict_t *pEdict, int type) { m_edict = pEdict; m_serial = pEdict->serialnumber; m_type = type; }

	virtual CBaseEntity *Entity() const
	{
		edict_t *pEdict = Edict();
		return pEdict ? (CBaseEntity *)GET_PRIVATE(pEdict) : nullptr;
	}
	virtual entvars_t *Vars() const { edict_t *pEdict = Edict(); return pEdict ? &pEdict->v : nullptr; }
	virtual const char *Provider() const { return "adopted"; }
	virtual const LsCreatureType *Type() const { return LsCreatureTypeAt(m_type); }
	virtual int Category() const { const LsCreatureType *t = Type(); return t ? t->category : LSC_ALIEN; }
	virtual bool IsAlive() const
	{
		edict_t *pEdict = Edict();
		return pEdict && pEdict->v.deadflag == DEAD_NO && pEdict->v.health > 0.0f && pEdict->v.takedamage != DAMAGE_NO;
	}
	virtual float Health() const { edict_t *pEdict = Edict(); return pEdict ? pEdict->v.health : 0.0f; }
	virtual void SetTarget(CBaseEntity *pTarget) {}
	virtual CBaseEntity *GetTarget() const { return nullptr; }
	virtual void SetBehaviour(int behaviour, const Vector *spot) {}
	virtual int GetBehaviour() const { return LSB_CHASE; }
	virtual float Threat() const { return clamp(0.4f + Health() / 600.0f, 0.4f, 1.0f); }
	virtual bool IsHostileTo(CBaseEntity *pOther) const { const LsCreatureType *t = Type(); return IsAlive() && LsIsHostilePlayer(pOther, t ? t->hates : 0); }
	virtual bool LastNoise(Vector *pos, float *when, float *range) const { return false; }
	virtual void Kill() {}		// (refused: see above; the command says why)

	edict_t *Edict() const
	{
		return (m_edict && !m_edict->free && m_edict->serialnumber == m_serial) ? m_edict : nullptr;
	}

private:
	edict_t *m_edict;
	int m_serial;
	int m_type;
};

static bool LsIsAdopted(ILsCreature *c)
{
	return !Q_strcmp(c->Provider(), "adopted");
}

static bool LsAdoptedHas(edict_t *pEdict)
{
	for (int i = 0; i < LsCreatureCount(); i++)
	{
		ILsCreature *c = LsCreatureAt(i);
		if (c && LsIsAdopted(c) && static_cast<LsAdoptedCreature *>(c)->Edict() == pEdict)
			return true;
	}

	return false;
}

// Classnames adopt refuses: players, the world, our own, items and brushes
static bool LsAdoptDenied(const char *classname)
{
	static const char *exact[] = { "player", "worldspawn", "ls_creature", "hostage_entity", "monster_scientist", "grenade",
		"weaponbox", "armoury_entity", "bodyque" };
	for (size_t i = 0; i < ARRAYSIZE(exact); i++)
	{
		if (!Q_stricmp(classname, exact[i]))
			return true;
	}

	static const char *prefixes[] = { "weapon_", "item_", "ammo_", "func_", "info_", "trigger_", "env_", "light", "path_", "game_", "multi" };
	for (size_t i = 0; i < ARRAYSIZE(prefixes); i++)
	{
		if (!Q_strnicmp(classname, prefixes[i], Q_strlen(prefixes[i])))
			return true;
	}

	return false;
}

static void LsAdoptScan()
{
	for (int r = 0; r < s_adoptCount; r++)
	{
		// (the engine's own search: another module's entities aren't in our classname hash)
		edict_t *pEdict = nullptr;
		while (!FNullEnt(pEdict = FIND_ENTITY_BY_CLASSNAME(pEdict, s_adopt[r].classname)))
		{
			if (pEdict->free || pEdict->v.deadflag != DEAD_NO || !GET_PRIVATE(pEdict) || LsAdoptedHas(pEdict))
				continue;

			LsCreatureRegister(new LsAdoptedCreature(pEdict, s_adopt[r].type));
		}
	}
}

// The native provider
class LsNativeProvider: public ILsCreatureProvider
{
public:
	virtual const char *Name() const { return "ls"; }
	virtual bool Available() const { return true; }
	virtual CBaseEntity *Spawn(int typeIndex, const Vector &origin, float yaw, const char *keys)
	{
		LsCreatureType *t = LsCreatureTypeAt(typeIndex);
		if (!t)
			return nullptr;

		// (only into a running map: not while one loads or after it ended)
		if (!s_mapRunning)
		{
			CONSOLE_ECHO("ls_creature: no map running\n");
			return nullptr;
		}

		if (!LsPosOk(origin))
			return nullptr;

		yaw = LsYaw(yaw);

		if (!t->precached && !LsCreaturePrecacheType(typeIndex))
		{
			CONSOLE_ECHO("ls_creature: type '%s' wasn't precached when the map loaded - add it to ls_creature_precache (or place one in the map)\n", t->name);
			return nullptr;
		}

		int alive = LsAliveCount(false);
		if (alive >= int(LsCreatureCapNow()) && !Q_strstr(keys ? keys : "", "friendly=1") && !t->friendly)
		{
			CONSOLE_ECHO("ls_creature: %d alive already (ls_creature_max %.0f, live cap now %.0f)\n", alive, ls_creature_max.value, LsCreatureCapNow());
			return nullptr;
		}

		// (keep a margin of free edicts for players' items and effects)
		if (NUMBER_OF_ENTITIES() > gpGlobals->maxEntities - 100)
		{
			CONSOLE_ECHO("ls_creature: too few free edicts left (%d of %d used)\n", NUMBER_OF_ENTITIES(), gpGlobals->maxEntities);
			return nullptr;
		}

		edict_t *pEdict = CREATE_NAMED_ENTITY(MAKE_STRING("ls_creature"));
		if (FNullEnt(pEdict))
			return nullptr;

		CLsCreature *pCreature = GET_PRIVATE<CLsCreature>(pEdict);
		if (!pCreature)
		{
			REMOVE_ENTITY(pEdict);
			return nullptr;
		}

		pCreature->pev->origin = origin;
		pCreature->pev->angles = Vector(0, yaw, 0);

		KeyValueData kvd;
		char kvType[32];
		Q_strlcpy(kvType, t->name);
		kvd.szClassName = (char *)"ls_creature";
		kvd.szKeyName = (char *)"type";
		kvd.szValue = kvType;
		kvd.fHandled = FALSE;
		pCreature->KeyValue(&kvd);

		// key=value pairs after the position (same keys as the map entity)
		if (keys && *keys)
		{
			char buf[256];
			Q_strlcpy(buf, keys);
			char *cursor = buf;
			for (char *tok; (tok = LsNextToken(&cursor, " \t")); )
			{
				char *eq = Q_strchr(tok, '=');
				if (!eq)
					continue;

				*eq = '\0';
				kvd.szKeyName = tok;
				kvd.szValue = eq + 1;
				kvd.fHandled = FALSE;
				pCreature->KeyValue(&kvd);
			}
		}

		if (DispatchSpawn(pEdict) == -1 || (pCreature->pev->flags & FL_KILLME))
		{
			REMOVE_ENTITY(pEdict);
			return nullptr;
		}

		CVAR_SET_FLOAT("ls_creature_last", ENTINDEX(pEdict));
		LsUpdateAliveCount();
		return pCreature;
	}
};

static LsNativeProvider s_nativeProvider;

CBaseEntity *LsCreatureSpawn(const char *type, const Vector &origin, float yaw, const char *keys)
{
	int index = LsCreatureTypeIndex(type);
	if (index < 0)
	{
		CONSOLE_ECHO("ls_creature: unknown type '%s' (ls_creature types)\n", type ? type : "");
		return nullptr;
	}

	return s_nativeProvider.Spawn(index, origin, yaw, keys);
}

void LsCreaturesServerDeactivate()
{
	s_mapRunning = false;

	for (int i = 0; i < s_listCount; i++)
	{
		delete s_list[i];
		s_list[i] = nullptr;
	}

	s_listCount = 0;
	s_listLive = 0;
	s_adoptCount = 0;
	s_nextAdoptScan = 0.0f;
	s_nextAliveCount = 0.0f;

	// (the engine's string pool is emptied with the map)
	for (int i = 0; i < s_typeCount; i++)
	{
		s_types[i].iszName = 0;
		s_types[i].precached = false;
	}
}

// Every game event the bots hear: creatures hear the same (shots, steps, breaking glass)
void LsCreaturesOnEvent(GameEventType event, CBaseEntity *pEntity, CBaseEntity *pOther)
{
	if (s_adoptCount > 0 && gpGlobals->time >= s_nextAdoptScan)
	{
		s_nextAdoptScan = gpGlobals->time + 1.0f;
		LsAdoptScan();
	}

	if (gpGlobals->time >= s_nextAliveCount || event == EVENT_ROUND_START)
	{
		s_nextAliveCount = gpGlobals->time + 1.0f;
		LsUpdateAliveCount();
	}

	for (int i = 0; i < s_listCount; i++)
	{
		ILsCreature *c = LsCreatureAt(i);
		if (!c || !c->IsAlive() || Q_strcmp(c->Provider(), "ls"))
			continue;

		static_cast<CLsCreature *>(c->Entity())->OnGameEvent(event, pEntity, pOther);
	}
}

// ---------------------------------------------------------------------------------------------
// The ls_creature entity

LINK_ENTITY_TO_CLASS(ls_creature, CLsCreature, CCSMonster)

// Never null: a creature whose slot somehow isn't there gets neutral values (it can't be
// spawned or set up with them - Setup wants a precached type - but nothing dereferences null)
const LsCreatureType *CLsCreature::GetType() const
{
	const LsCreatureType *t = LsCreatureTypeAt(m_type);
	if (t)
		return t;

	static LsCreatureType s_none;
	if (!s_none.name[0])
		LsTypeDefaults(&s_none, "none");

	return &s_none;
}

void CLsCreature::KeyValue(KeyValueData *pkvd)
{
	const char *key = pkvd->szKeyName;
	const char *value = pkvd->szValue;

	if (FStrEq(key, "type"))
	{
		m_type = LsCreatureTypeIndex(value);
		if (m_type < 0)
			CONSOLE_ECHO("ls_creature: unknown type '%s'\n", value);
	}
	else if (FStrEq(key, "behaviour") || FStrEq(key, "behavior"))
		m_homeBehaviour = LsNameIndex(value, s_behaviourNames, LSB_COUNT, -1) + 1;
	else if (FStrEq(key, "path") || FStrEq(key, "patrol"))
		m_patrolStart = ALLOC_STRING(value);
	else if (FStrEq(key, "radius"))
		m_guardRadius = LsNum(value, 0.0f, 8192.0f, 0.0f);
	else if (FStrEq(key, "respawn"))
		m_respawn = LsNum(value, 0.0f, 3600.0f, 0.0f);
	else if (FStrEq(key, "health") || FStrEq(key, "hp"))
		m_keyHealth = LsNum(value, 0.0f, 100000.0f, 0.0f);
	else if (FStrEq(key, "speed"))
		m_keySpeed = LsNum(value, 0.0f, 2000.0f, 0.0f);
	else if (FStrEq(key, "damage"))
		m_keyDamage = LsNum(value, 0.0f, 10000.0f, 0.0f);
	else if (FStrEq(key, "aggression"))
		m_keyAggression = LsNum(value, 0.0f, 1.0f, -1.0f) + 0.001f;		// (0 stays "set": passive)
	else if (FStrEq(key, "detect"))
		m_keyDetect = LsNum(value, 0.0f, 8192.0f, 0.0f);
	else if (FStrEq(key, "hates"))
		m_keyHates = LsHatesFromString(value) + 1;
	else if (FStrEq(key, "persist"))
		m_persist = Q_atoi(value) != 0;
	else if (FStrEq(key, "friendly"))
		m_keyFriendly = Q_atoi(value) ? 2 : 1;
	else if (FStrEq(key, "flee_hp"))
		m_keyFleeHp = LsNum(value, 0.0f, 1.0f, 0.0f) + 1.0f;	// (works on a live one too)
	else if (FStrEq(key, "owner"))
		m_ownerIndex = Q_atoi(value);
	else if (FStrEq(key, "tier"))
		SetTier(Q_atoi(value));		// (also on one already spawned: a plugin's DispatchKeyValue after the spawn)
	else
	{
		CBaseMonster::KeyValue(pkvd);
		return;
	}

	pkvd->fHandled = TRUE;
}

void CLsCreature::Precache()
{
	if (m_type >= 0)
		LsCreaturePrecacheType(m_type);
}

void CLsCreature::Spawn()
{
	// (the engine zero-fills a new entity's data: the keys set what they name before this)
	if (!m_improv)
	{
		// first spawn: a map entity is spawned before ServerActivate; one made by a command after
		m_fromMap = !g_bServerActive;
		m_spawnOrigin = pev->origin;
		m_spawnAngles = Vector(0, LsYaw(pev->angles.y), 0);
	}

	if (m_type < 0 || !LsCreatureTypeAt(m_type))
	{
		pev->flags |= FL_KILLME;
		return;
	}

	Precache();

	const LsCreatureType *t = GetType();
	if (!t->precached)
	{
		CONSOLE_ECHO("ls_creature: '%s' can't be spawned: its model isn't precached\n", t->name);
		pev->flags |= FL_KILLME;
		return;
	}

	MAKE_STRING_CLASS("ls_creature", pev);

	// the live cap holds for every new one, however it's made (a plugin's create_entity +
	// DispatchSpawn too, not only "ls_creature spawn"); placed in the map ones always come
	bool friendly = m_keyFriendly ? m_keyFriendly == 2 : GetType()->friendly;
	if (!m_improv && !m_fromMap && friendly)
	{
		int alive = LsAliveCount(true);
		if (alive >= int(ls_creature_friendly_max.value))
		{
			CONSOLE_ECHO("ls_creature: %d friendly ones alive already (ls_creature_friendly_max): not spawned\n", alive);
			pev->flags |= FL_KILLME;
			return;
		}
	}
	else if (!m_improv && !m_fromMap)
	{
		int alive = LsAliveCount(false);
		if (alive >= int(LsCreatureCapNow()))
		{
			if (ls_creature_debug.value > 0.0f)
				CONSOLE_ECHO("ls_creature: %d alive already (ls_creature_max %.0f, live cap now %.0f): not spawned\n", alive, ls_creature_max.value, LsCreatureCapNow());
			pev->flags |= FL_KILLME;
			return;
		}
	}

	if (!m_improv)
	{
		m_improv = new CCreatureImprov(this);
		LsCreatureRegister(new LsNativeCreature(this));
	}

	Setup();
}

// Everything that a spawn and a respawn share
void CLsCreature::Setup()
{
	const LsCreatureType *t = GetType();
	if (!t || !t->precached)
	{
		// (can't happen with stable type slots; never run on a type that isn't there)
		pev->flags |= FL_KILLME;
		return;
	}

	pev->origin = m_spawnOrigin;
	pev->angles = m_spawnAngles;
	pev->movetype = t->moves ? MOVETYPE_STEP : MOVETYPE_TOSS;
	pev->solid = SOLID_SLIDEBOX;
	pev->takedamage = DAMAGE_AIM;
	pev->flags |= FL_MONSTER;
	pev->deadflag = DEAD_NO;
	pev->effects &= ~EF_NODRAW;
	pev->max_health = (m_keyHealth > 0.0f) ? m_keyHealth : t->health;
	pev->health = pev->max_health;
	pev->gravity = 1.0f;
	pev->friction = 1.0f;
	pev->velocity = Vector(0, 0, 0);
	pev->view_ofs = Vector(0, 0, t->hullMax.z - 8.0f);
	pev->netname = t->iszName;
	pev->skin = t->skin;
	pev->body = t->body;
	pev->scale = t->scale;
	pev->frame = 0;
	pev->framerate = 1.0f;

	SET_MODEL(edict(), STRING(t->iszModel));
	UTIL_SetSize(pev, t->hullMin, t->hullMax);
	UTIL_SetOrigin(pev, pev->origin);

	// a tougher tier: more health when the type says so (the mode usually scales health itself)
	if (Tier() >= 2)
		pev->health = pev->max_health = pev->max_health * t->tierHealth[Tier() - 2];

	m_enraged = false;
	m_animScale = 1.0f;
	m_friendly = m_keyFriendly ? m_keyFriendly == 2 : t->friendly;
	pev->team = m_friendly ? 1 : 0;		// (plugins: pev_team 1 = friendly)
	ApplyLook();

	m_bloodColor = t->bloodColor;
	m_speedScale = 1.0f;
	m_runSpeed = (m_keySpeed > 0.0f) ? m_keySpeed : t->speed;
	m_walkSpeed = Q_min(t->walkSpeed, m_runSpeed);
	if (m_walkSpeed <= 0.0f)
		m_walkSpeed = m_runSpeed * 0.4f;
	m_damage = (m_keyDamage > 0.0f) ? m_keyDamage : t->damage;
	m_aggression = (m_keyAggression > 0.0f) ? clamp(m_keyAggression - 0.001f, 0.0f, 1.0f) : t->aggression;
	m_detect = (m_keyDetect > 0.0f) ? m_keyDetect : t->detectRadius;
	m_hates = (m_keyHates > 0) ? m_keyHates - 1 : t->hates;
	if (m_guardRadius <= 0.0f)
		m_guardRadius = t->guardRadius;
	if (m_respawn <= 0.0f)
		m_respawn = t->respawn;

	// (m_homeBehaviour is the key's behaviour + 1: 0 = not given, the type's, or a patrol when a path was)
	m_behaviour = (m_homeBehaviour > 0) ? m_homeBehaviour - 1 : m_patrolStart ? LSB_PATROL : t->behaviour;
	if (!t->moves)
		m_behaviour = LSB_GUARD;

	m_home = m_spawnOrigin;
	m_homeAngles = m_spawnAngles;
	m_mode = MODE_HOME;
	m_target = nullptr;
	m_forcedTarget = nullptr;
	m_lastAttacker = nullptr;
	m_noticing = nullptr;
	m_hEnemy = nullptr;
	m_targetSeenAt = -1000.0f;
	m_searchUntil = 0.0f;
	m_fleeUntil = 0.0f;
	m_fleeCooldown = 0.0f;
	m_patrolCorner = nullptr;
	m_waitUntil = gpGlobals->time + RANDOM_FLOAT(0.5f, 2.0f);
	m_nextSense = gpGlobals->time + RANDOM_FLOAT(0.1f, 0.4f);
	m_nextFullThink = 0.0f;
	m_nextRepath = 0.0f;
	m_attackStart = 0.0f;
	m_attackLanded = false;
	m_nextAttack = 0.0f;
	m_nextIdleSound = gpGlobals->time + RANDOM_FLOAT(4.0f, 12.0f);
	m_painSoundAt = 0.0f;
	m_flinchUntil = 0.0f;
	m_skipTarget = nullptr;
	m_skipUntil = 0.0f;
	m_cantReachSince = 0.0f;
	m_animMoving = false;
	m_attackFramerate = 1.0f;
	m_attackAnimEnd = 0.0f;
	m_stuckSecs = 0;
	m_crumbCount = 0;
	m_crumbNext = 0;
	m_nextProgress = gpGlobals->time + 1.0f;
	m_deadAt = 0.0f;
	m_anim = -1;
	m_noiseAt = -1000.0f;
	m_noiseRange = 0.0f;

	m_improv->OnReset();
	m_improv->SetSpeeds(m_walkSpeed, m_runSpeed);

	// (a spot inside a player or a wall - a wave spawner's random point - moves to a free one nearby)
	if (!FreeSpot(pev->origin))
	{
		Vector spot = pev->origin;
		if (FindFreeSpot(&spot))
			UTIL_SetOrigin(pev, spot);
	}

	if (t->moves)
		DROP_TO_FLOOR(edict());

	m_progressPos = pev->origin;
	ResolveAnims();
	m_anim = -1;
	SetAnim(LSAN_IDLE, true);

	SetThink(&CLsCreature::CreatureThink);
	SetTouch(nullptr);
	pev->nextthink = gpGlobals->time + RANDOM_FLOAT(0.05f, 0.2f);
}

void CLsCreature::ComeBack()
{
	// (somebody standing on the spot: wait a moment rather than spawn inside them)
	TraceResult tr;
	const LsCreatureType *t = GetType();
	if (!t)
		return;

	UTIL_TraceHull(m_spawnOrigin + Vector(0, 0, 2), m_spawnOrigin + Vector(0, 0, 3), dont_ignore_monsters,
		(t->hullMax.z > 40.0f) ? human_hull : head_hull, edict(), &tr);
	if (tr.fStartSolid || tr.fAllSolid)
	{
		m_deadAt = gpGlobals->time - m_respawn + 1.0f;
		return;
	}

	Setup();
}

// Map-placed creatures (and persistent ones) come back each round; the rest are released
int CLsCreature::ObjectCaps()
{
	return (CBaseMonster::ObjectCaps() & ~FCAP_ACROSS_TRANSITION) | ((m_fromMap || m_persist) ? FCAP_MUST_RESET : FCAP_MUST_RELEASE);
}

void CLsCreature::Restart()
{
	if (m_type < 0 || !m_improv)
		return;

	Setup();
}

#ifdef REGAMEDLL_API
void CLsCreature::OnDestroy()
{
	LsCreatureUnregister(this);
	delete m_improv;
	m_improv = nullptr;
	CBaseMonster::OnDestroy();
}
#endif

void CLsCreature::Touch(CBaseEntity *pOther)
{
	if (m_improv && IsAlive())
		m_improv->OnTouch(pOther);
}

// Creatures don't block each other (a horde squeezes through a door; one stopped in a choke
// doesn't jam the rest, and two never end up stuck inside each other): the engine asks this for
// every collision and trace. No creatures: always yes, as stock
int LsCreatureShouldCollide(edict_t *pentTouched, edict_t *pentOther)
{
	if (!s_listLive || !pentTouched || !pentOther)
		return 1;

	if (!(pentTouched->v.flags & FL_MONSTER) || !(pentOther->v.flags & FL_MONSTER))
		return 1;

	if (FClassnameIs(pentTouched, "ls_creature") && FClassnameIs(pentOther, "ls_creature"))
		return 0;

	return 1;
}

// Room for its hull here (players and walls; other creatures don't count, see above)
bool CLsCreature::FreeSpot(const Vector &spot) const
{
	const LsCreatureType *t = GetType();
	bool tall = t->hullMax.z > 36.0f;
	TraceResult tr;
	Vector center = spot + Vector(0, 0, tall ? 37.0f : 19.0f);
	UTIL_TraceHull(center, center, dont_ignore_monsters, tall ? human_hull : head_hull, ENT(pev), &tr);
	return !tr.fStartSolid && !tr.fAllSolid;
}

// The nearest free spot around (rings out to 128), one it could walk to from here
bool CLsCreature::FindFreeSpot(Vector *spot) const
{
	Vector from = *spot;
	for (float r = 32.0f; r <= 128.0f; r += 32.0f)
	{
		for (int i = 0; i < 8; i++)
		{
			float yaw = i * 45.0f + r;
			Vector to = from + Vector(BotCOS(yaw), BotSIN(yaw), 0.0f) * r;
			if (!FreeSpot(to))
				continue;

			TraceResult tr;
			UTIL_TraceLine(from + Vector(0, 0, 24), to + Vector(0, 0, 24), ignore_monsters, ENT(pev), &tr);
			if (tr.flFraction < 1.0f)
				continue;

			*spot = to;
			return true;
		}
	}

	return false;
}

// Would anyone see it pop from one spot to another
bool CLsCreature::SeenByAnyone(const Vector &spot) const
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!pPlayer || !pPlayer->IsAlive())
			continue;

		Vector eyes = pPlayer->pev->origin + pPlayer->pev->view_ofs;
		if ((eyes - spot).IsLengthGreaterThan(3000.0f))
			continue;

		TraceResult tr;
		UTIL_TraceLine(eyes, spot + Vector(0, 0, 36), ignore_monsters, ignore_glass, pPlayer->edict(), &tr);
		if (tr.flFraction == 1.0f)
			return true;
	}

	return false;
}

static int s_stuckTeleports = 0;

// Once a second: trying to go somewhere and getting nowhere. 2 s: hop and shove sideways;
// 4 s: a fresh path; 7 s (then every 4): back to a spot it walked through lately - or, past 15 s,
// to where it spawned - when nobody can see it go or arrive
void CLsCreature::UpdateStuck()
{
	float now = gpGlobals->time;
	if (now < m_nextProgress)
		return;

	m_nextProgress = now + 1.0f;

	const LsCreatureType *t = GetType();
	bool trying = t->moves && m_improv->HasGoal() && !m_improv->IsStopped() && m_mode != MODE_ATTACK;
	float moved = (pev->origin - m_progressPos).Make2D().Length();
	m_progressPos = pev->origin;

	if (!trying || moved > 24.0f)
	{
		m_stuckSecs = 0;

		// (a breadcrumb every ~100 units of real progress, on the ground)
		if (moved > 24.0f && (pev->flags & FL_ONGROUND))
		{
			int last = (m_crumbNext + 3) % 4;
			if (!m_crumbCount || (m_crumbs[last] - pev->origin).IsLengthGreaterThan(96.0f))
			{
				m_crumbs[m_crumbNext] = pev->origin;
				m_crumbNext = (m_crumbNext + 1) % 4;
				m_crumbCount = Q_min(m_crumbCount + 1, 4);
			}
		}
		return;
	}

	m_stuckSecs++;

	if (m_stuckSecs == 2)
	{
		m_improv->Jump();
		float yaw = pev->angles.y + (RANDOM_LONG(0, 1) ? 90.0f : -90.0f);
		m_improv->ApplyForce(Vector(BotCOS(yaw), BotSIN(yaw), 0.0f) * 200.0f);
		return;
	}

	if (m_stuckSecs == 4)
	{
		m_improv->ForceRepath();
		float yaw = pev->angles.y + 180.0f + RANDOM_FLOAT(-60.0f, 60.0f);
		m_improv->ApplyForce(Vector(BotCOS(yaw), BotSIN(yaw), 0.0f) * 250.0f);
		return;
	}

	if (m_stuckSecs < 7 || (m_stuckSecs - 7) % 4 != 0 || SeenByAnyone(pev->origin))
		return;

	// newest breadcrumb first that isn't right here, free, and out of sight
	Vector dest;
	bool found = false;
	for (int k = 1; k <= m_crumbCount && !found; k++)
	{
		const Vector &crumb = m_crumbs[(m_crumbNext - k + 8) % 4];
		if ((crumb - pev->origin).IsLengthLessThan(48.0f) || !FreeSpot(crumb) || SeenByAnyone(crumb))
			continue;

		dest = crumb;
		found = true;
	}

	if (!found && m_stuckSecs >= 15 && FreeSpot(m_spawnOrigin) && !SeenByAnyone(m_spawnOrigin))
	{
		dest = m_spawnOrigin;
		found = true;
	}

	if (!found)
		return;

	if (ls_creature_debug.value > 0.0f)
		CONSOLE_ECHO("ls_creature: %s#%d stuck %d s at %.0f %.0f %.0f, back to %.0f %.0f %.0f\n", t->name, entindex(), m_stuckSecs,
			pev->origin.x, pev->origin.y, pev->origin.z, dest.x, dest.y, dest.z);

	UTIL_SetOrigin(pev, dest);
	pev->velocity = Vector(0, 0, 0);
	m_progressPos = dest;
	m_stuckSecs = 0;
	m_crumbCount = 0;
	m_improv->ForceRepath();
	m_nextRepath = 0.0f;
	s_stuckTeleports++;
}

// Players by its "hates"; a friendly one (a bought turret) only the other creatures, and they it
bool CLsCreature::IsHostileTo(CBaseEntity *pOther) const
{
	if (!const_cast<CLsCreature *>(this)->IsAlive() || !pOther || pOther == this)
		return false;
	CLsCreature *pCreature = LsNativeOf(pOther);
	if (pCreature)
		return pCreature->m_friendly != m_friendly && pCreature->IsAlive();
	if (m_friendly)
		return false;
	return LsIsHostilePlayer(pOther, m_hates);
}

bool CLsCreature::LastNoise(Vector *pos, float *when, float *range) const
{
	if (m_noiseAt < 0.0f)
		return false;

	if (pos) *pos = m_noisePos;
	if (when) *when = m_noiseAt;
	if (range) *range = m_noiseRange;
	return true;
}

void CLsCreature::MakeNoise(float range)
{
	m_noisePos = pev->origin;
	m_noiseAt = gpGlobals->time;
	m_noiseRange = range;
}

void CLsCreature::PlaySound(int kind, float volume)
{
	const LsCreatureType *t = GetType();
	if (!t)
		return;

	// a sentence (sound/sentences.txt, the same file on every CS install): the game sends its number
	// and each player's game finds the words itself, so it takes no sound precache slot
	if (t->say[kind][0] && SENTENCEG_PlayRndSz(edict(), t->say[kind], volume, ATTN_NORM, 0, PITCH_NORM + RANDOM_LONG(-3, 3)) >= 0)
	{
		MakeNoise((kind == LSS_IDLE) ? 600.0f : 1000.0f);
		return;
	}

	if (t->soundCount[kind] <= 0)
		return;

	const char *file = STRING(t->iszSound[kind][RANDOM_LONG(0, t->soundCount[kind] - 1)]);
	int channel = (kind == LSS_ATTACK || kind == LSS_HIT || kind == LSS_MISS) ? CHAN_WEAPON : CHAN_VOICE;
	EMIT_SOUND_DYN(edict(), channel, file, volume, ATTN_NORM, 0, PITCH_NORM + RANDOM_LONG(-5, 5));

	// (what it says is heard: idle grunts not far, an attack or a death cry further)
	MakeNoise((kind == LSS_IDLE) ? 600.0f : (kind == LSS_ATTACK && t->attack == LSA_RANGED) ? 1500.0f : 1000.0f);
}

// The sequence for a state, from the model's own table: the type's name first, then by
// activity (weighted, as the model asks), then player-model names
int CLsCreature::PickSequence(int anim)
{
	const LsCreatureType *t = GetType();
	int seq = t->anim[anim][0] ? LookupSequence(t->anim[anim]) : -1;
	if (seq >= 0)
		return seq;

	// (a ranged attacker spits: its range attack before any melee one)
	static const int acts[LSAN_COUNT][3] =
	{
		{ ACT_IDLE, ACT_IDLE, ACT_IDLE },
		{ ACT_WALK, ACT_RUN, ACT_IDLE },
		{ ACT_RUN, ACT_WALK, ACT_IDLE },
		{ ACT_MELEE_ATTACK1, ACT_MELEE_ATTACK2, ACT_RANGE_ATTACK1 },
		{ ACT_SMALL_FLINCH, ACT_BIG_FLINCH, ACT_FLINCH_CHEST },
		{ ACT_DIESIMPLE, ACT_DIEFORWARD, ACT_DIEBACKWARD },
	};
	static const int rangedActs[3] = { ACT_RANGE_ATTACK1, ACT_RANGE_ATTACK2, ACT_MELEE_ATTACK1 };
	const int *list = (anim == LSAN_ATTACK && t->attack == LSA_RANGED) ? rangedActs : acts[anim];

	for (int i = 0; i < 3 && seq < 0; i++)
		seq = LookupActivity(list[i]);

	// (player models: sequences by name)
	if (seq < 0)
	{
		static const char *names[LSAN_COUNT] = { "idle1", "walk", "run", "ref_shoot_knife", "gutshot", "death1" };
		seq = LookupSequence(names[anim]);
	}

	return seq;
}

// Once per model: the movement sequences and how fast each one's feet move
void CLsCreature::ResolveAnims()
{
	int keep = pev->sequence;
	void *pmodel = GET_MODEL_PTR(ENT(pev));

	for (int a = LSAN_IDLE; a <= LSAN_RUN; a++)
	{
		m_seq[a] = PickSequence(a);
		m_seqGround[a] = 0.0f;
		if (m_seq[a] >= 0 && pmodel)
		{
			float fps = 0.0f, ground = 0.0f;
			pev->sequence = m_seq[a];
			::GetSequenceInfo(pmodel, pev, &fps, &ground);
			m_seqGround[a] = ground;
		}
	}

	// (a model without its own run - the HL zombie - plays its walk faster instead)
	m_hasRun = m_seq[LSAN_RUN] >= 0 && m_seq[LSAN_RUN] != m_seq[LSAN_WALK] && m_seqGround[LSAN_RUN] > 1.0f;
	pev->sequence = keep;

	if (ls_creature_debug.value > 3.0f)
		CONSOLE_ECHO("ls_creature: %s#%d idle %d walk %d (%.0f) run %d (%.0f)%s\n", GetType()->name, entindex(), m_seq[LSAN_IDLE],
			m_seq[LSAN_WALK], m_seqGround[LSAN_WALK], m_seq[LSAN_RUN], m_seqGround[LSAN_RUN], m_hasRun ? "" : " (no run: walk sped up)");
}

// How it looks: the type's tint (or the stand-in's: a hostage tinted green reads as "zombie"),
// then the tier's, then enraged. The same on every type, no extra models: tier 2 a yellow shell,
// tier 3 a red-orange shell and 8% bigger, enraged a pulsing bright red shell (UpdateAnimation).
// Skins and bodies only where the type names them for the tier (the model has them).
void CLsCreature::ApplyLook()
{
	const LsCreatureType *t = GetType();
	pev->renderfx = kRenderFxNone;
	pev->rendermode = kRenderNormal;
	pev->renderamt = 0;
	pev->skin = t->skin;
	pev->body = t->body;
	pev->scale = t->scale;
	if (t->glow || (t->fallbackGlow && t->usingFallback))
	{
		pev->renderfx = kRenderFxGlowShell;
		pev->rendercolor = t->glow ? t->glowColor : t->fallbackGlowColor;
		pev->renderamt = 12;
	}

	int tier = Tier();
	if (tier >= 2)
	{
		if (t->tierSkin[tier - 2] >= 0)
			pev->skin = t->tierSkin[tier - 2];
		if (t->tierBody[tier - 2] >= 0)
			pev->body = t->tierBody[tier - 2];
		pev->renderfx = kRenderFxGlowShell;
		pev->rendercolor = (tier == 2) ? Vector(255, 200, 0) : Vector(255, 70, 10);
		pev->renderamt = (tier == 2) ? 10 : 14;
		if (tier == 3)
			pev->scale = t->scale * 1.08f;
	}

	if (m_enraged)
	{
		pev->renderfx = kRenderFxGlowShell;
		pev->rendercolor = Vector(255, 0, 0);
		pev->renderamt = 20;
	}
}

// Hurt below the type's enrage_below: faster, harder hits, quicker swings, once until it respawns.
// The tell: a deep roar (its alert or pain sound at a low pitch: nothing new to precache) and the
// pulsing red shell.
void CLsCreature::Enrage()
{
	const LsCreatureType *t = GetType();
	if (m_enraged || !t)
		return;

	m_enraged = true;
	m_runSpeed *= t->enrageSpeed;
	m_walkSpeed *= t->enrageSpeed;
	m_damage *= t->enrageDamage;
	m_animScale = t->enrageAnim;
	ApplyLook();

	int kind = t->soundCount[LSS_ALERT] > 0 ? LSS_ALERT : LSS_PAIN;
	if (t->soundCount[kind] > 0)
		EMIT_SOUND_DYN(edict(), CHAN_VOICE, STRING(t->iszSound[kind][RANDOM_LONG(0, t->soundCount[kind] - 1)]), VOL_NORM, ATTN_NORM * 0.5f, 0, 72);
	MakeNoise(1500.0f);

	if (ls_creature_debug.value > 0.0f)
		CONSOLE_ECHO("ls_creature: %s#%d enraged at %.0f of %.0f health\n", t->name, entindex(), pev->health, pev->max_health);
}

void CLsCreature::SetAnim(int anim, bool restart)
{
	int seq = (anim <= LSAN_RUN) ? m_seq[anim] : PickSequence(anim);
	if (seq < 0)
	{
		m_anim = anim;
		return;
	}

	// (the same sequence keeps playing - walk to run on a model with one walk doesn't jump back
	// to frame 0 - unless asked to restart, or a one-shot that ended)
	if (seq == pev->sequence && !restart && !(m_fSequenceFinished && !m_fSequenceLoops))
	{
		m_anim = anim;
		return;
	}

	pev->sequence = seq;
	pev->frame = 0;
	ResetSequenceInfo();
	m_anim = anim;

	if (ls_creature_debug.value > 3.0f)
	{
		static const char *names[LSAN_COUNT] = { "idle", "walk", "run", "attack", "flinch", "die" };
		CONSOLE_ECHO("ls_creature: %s#%d %s -> sequence %d\n", GetType()->name, entindex(), names[anim], seq);
	}
}

void CLsCreature::UpdateAnimation()
{
	const LsCreatureType *t = GetType();
	if (m_mode == MODE_DEAD || !t)
		return;

	float now = gpGlobals->time;
	float speed = m_improv->GetActualVelocity().Make2D().Length();

	// a swing or a flinch plays out once (started where it happens), then movement again
	if (m_anim == LSAN_ATTACK && now < m_attackAnimEnd && !m_fSequenceFinished)
	{
		pev->framerate = m_attackFramerate;
		return;
	}

	if (m_anim == LSAN_FLINCH && now < m_flinchUntil && !m_fSequenceFinished)
	{
		pev->framerate = 1.0f;
		return;
	}

	// (moving or not, with a margin so it doesn't flicker between the two)
	// enraged: the red shell pulses
	if (m_enraged)
		pev->renderamt = 16.0f + 12.0f * fabs(sinf(now * 5.0f));

	m_animMoving = speed > (m_animMoving ? 8.0f : 20.0f);
	if (!m_animMoving)
	{
		SetAnim(LSAN_IDLE);
		pev->framerate = m_animScale;
		return;
	}

	// the run when the speed is nearer the run's than the walk's, feet matched to the ground
	int anim = LSAN_WALK;
	if (m_hasRun && speed > (m_seqGround[LSAN_WALK] + m_seqGround[LSAN_RUN]) * 0.5f)
		anim = LSAN_RUN;

	SetAnim(anim);
	float ground = m_seqGround[anim];
	pev->framerate = (ground > 1.0f) ? clamp(speed / ground, 0.3f, 4.0f) : 1.0f;
}

void CLsCreature::SetTarget(CBaseEntity *pTarget)
{
	m_forcedTarget = pTarget;
	if (pTarget && IsHostileTo(pTarget))
	{
		m_target = pTarget;
		m_hEnemy = pTarget;
		m_targetLastSeen = pTarget->pev->origin;
		m_targetSeenAt = gpGlobals->time;
		if (m_mode != MODE_ATTACK)
			m_mode = MODE_CHASE;
	}
	else if (!pTarget)
	{
		m_target = nullptr;
		m_hEnemy = nullptr;
		if (m_mode == MODE_CHASE || m_mode == MODE_ATTACK)
			GoHome();
	}
}

void CLsCreature::SetBehaviour(int behaviour, const Vector *spot)
{
	if (behaviour < 0 || behaviour >= LSB_COUNT)
		return;

	const LsCreatureType *t = GetType();
	if (t && !t->moves && behaviour != LSB_GUARD && behaviour != LSB_IDLE)
		return;

	if (spot)
		m_home = *spot;

	if (behaviour == LSB_FLEE)
	{
		StartFlee(spot ? *spot : pev->origin + Vector(RANDOM_FLOAT(-1, 1), RANDOM_FLOAT(-1, 1), 0) * 100.0f);
		return;
	}

	m_behaviour = behaviour;
	m_patrolCorner = nullptr;
	if (m_mode == MODE_HOME || m_mode == MODE_SEARCH)
		GoHome();
}

// 0 or 1 = normal, 2, 3 (see ApplyLook); on a live one the health keeps its share
void CLsCreature::SetTier(int tier)
{
	int before = Tier();
	m_tier = clamp(tier, 1, 3);
	if (m_improv && IsAlive() && Tier() != before)
	{
		const LsCreatureType *t = GetType();
		float ratio = (before >= 2 ? 1.0f / t->tierHealth[before - 2] : 1.0f) * (Tier() >= 2 ? t->tierHealth[Tier() - 2] : 1.0f);
		pev->max_health *= ratio;
		pev->health *= ratio;
		ApplyLook();
	}
}

void CLsCreature::SetSpeedScale(float scale)
{
	scale = clamp(scale, 0.2f, 3.0f);
	float ratio = scale / Q_max(m_speedScale, 0.01f);
	m_runSpeed *= ratio;
	m_walkSpeed *= ratio;
	m_speedScale = scale;
}

// Back to what it does on its own: its spawn behaviour (the key's, a patrol, or the type's), its
// spawn spot as home, no target it was told to attack. (A hunter picks its next target itself.)
void CLsCreature::Resume()
{
	const LsCreatureType *t = GetType();
	if (!t || !IsAlive())
		return;
	m_forcedTarget = nullptr;
	m_target = nullptr;
	m_hEnemy = nullptr;
	m_home = m_spawnOrigin;
	m_behaviour = (m_homeBehaviour > 0) ? m_homeBehaviour - 1 : m_patrolStart ? LSB_PATROL : t->behaviour;
	if (!t->moves)
		m_behaviour = LSB_GUARD;
	m_patrolCorner = nullptr;
	if (m_mode != MODE_DEAD)
		GoHome();
}

void CLsCreature::PublishState()
{
	int state = LSST_NONE;
	switch (m_mode)
	{
	case MODE_HOME: state = LSST_IDLE + clamp(m_behaviour, 0, int(LSB_FLEE)); break;
	case MODE_CHASE: state = LSST_CHASE; break;
	case MODE_ATTACK: state = LSST_ATTACK; break;
	case MODE_SEARCH: state = LSST_SEARCH; break;
	case MODE_FLEE: state = LSST_FLEE; break;
	case MODE_DEAD: state = LSST_DEAD; break;
	}
	pev->iuser3 = state;
	CBaseEntity *pTarget = m_target;
	pev->enemy = (pTarget && m_mode != MODE_DEAD) ? pTarget->edict() : nullptr;
}

void CLsCreature::KillQuietly()
{
	if (IsAlive())
		Killed(VARS(INDEXENT(0)), GIB_NEVER);
}

// What it sees: a hostile player inside its sight radius, in its view cone (all round when
// it's already alert), not behind a wall
bool CLsCreature::CanSee(CBaseEntity *pOther, bool alert, bool thorough) const
{
	Vector eyes = pev->origin + pev->view_ofs;
	Vector body = pOther->pev->origin + pOther->pev->view_ofs * 0.5f;
	Vector to = body - eyes;
	float dist = to.Length();

	float radius = m_detect * (alert ? 1.5f : 1.0f);
	if (dist > radius)
		return false;

	const LsCreatureType *t = GetType();
	if (!t)
		return false;

	if (!alert && t->detectFov < 359.0f && dist > 64.0f)
	{
		UTIL_MakeVectors(Vector(0, pev->angles.y, 0));
		Vector2D dir = to.Make2D();
		dir.NormalizeInPlace();
		float cosHalf = BotCOS(t->detectFov * 0.5f);
		if (DotProduct(dir, gpGlobals->v_forward.Make2D()) < cosHalf)
			return false;
	}

	// (a smoke cloud between hides them as it does from the bots)
	if (TheCSBots() && TheCSBots()->IsLineBlockedBySmoke(&eyes, &body))
		return false;

	TraceResult tr;
	UTIL_TraceLine(eyes, pOther->pev->origin + pOther->pev->view_ofs, ignore_monsters, ignore_glass, ENT(pev), &tr);
	if (tr.flFraction == 1.0f)
		return true;

	// (whom it's after gets a second look, at the body: head behind a crate isn't gone)
	if (!thorough)
		return false;

	UTIL_TraceLine(eyes, body, ignore_monsters, ignore_glass, ENT(pev), &tr);
	return tr.flFraction == 1.0f;
}

bool CLsCreature::WantsToFight(CBaseEntity *pOther, float dist) const
{
	if (pOther == m_forcedTarget || pOther == m_lastAttacker)
		return m_aggression > 0.0f;

	if (m_aggression >= 0.7f)
		return true;

	if (m_aggression <= 0.0f)
		return false;

	// guards and territorial animals: whoever comes near the spot
	if (m_behaviour == LSB_GUARD)
		return (pOther->pev->origin - m_home).Length() < m_guardRadius;

	return dist < m_detect * m_aggression;
}

void CLsCreature::Notice(CBaseEntity *pOther, bool alert)
{
	m_target = pOther;
	m_hEnemy = pOther;
	m_targetLastSeen = pOther->pev->origin;
	m_targetSeenAt = gpGlobals->time;
	m_noticing = nullptr;

	if (m_mode != MODE_ATTACK && m_mode != MODE_FLEE)
		m_mode = MODE_CHASE;

	if (!alert)
		PlaySound(LSS_ALERT);
}

void CLsCreature::UpdateSenses()
{
	const LsCreatureType *t = GetType();
	// (alert: after someone, searching, or hurt a moment ago - it looks all round then)
	bool alert = m_target || gpGlobals->time < m_searchUntil || (m_painSoundAt > 0.0f && gpGlobals->time < m_painSoundAt + 5.0f);

	// told whom to attack: it knows where they are
	CBaseEntity *pForced = m_forcedTarget;
	if (pForced && !IsHostileTo(pForced))
	{
		m_forcedTarget = nullptr;
		pForced = nullptr;
	}

	if (pForced)
	{
		if (m_target != pForced)
			Notice(pForced, true);

		m_targetLastSeen = pForced->pev->origin;
		m_targetSeenAt = gpGlobals->time;
		return;
	}

	CBaseEntity *pCurrent = m_target;
	if (pCurrent && !IsHostileTo(pCurrent))
	{
		m_target = nullptr;
		m_hEnemy = nullptr;
		pCurrent = nullptr;
	}

	// everyone it can see now, the pick by its targeting
	CBaseEntity *pBest = nullptr;
	float bestScore = 1e9f;
	float bestDist = 0.0f;
	bool currentSeen = false;

	// (only the nearest few in range get sight traces - and whom it's after, always)
	const int maxLooks = 4;
	CBaseEntity *looks[maxLooks + 1];
	float lookDist[maxLooks + 1];
	int lookCount = 0;
	float range = m_detect * 1.5f;

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!pPlayer || !IsHostileTo(pPlayer) || SkipsTarget(pPlayer))
			continue;

		float dist = (pPlayer->pev->origin - pev->origin).Length();
		if (dist > range || pPlayer == pCurrent)
			continue;

		// (insertion into the short sorted list)
		int at = lookCount;
		while (at > 0 && lookDist[at - 1] > dist)
			at--;

		if (at >= maxLooks)
			continue;

		for (int k = Q_min(lookCount, maxLooks - 1); k > at; k--)
		{
			looks[k] = looks[k - 1];
			lookDist[k] = lookDist[k - 1];
		}

		looks[at] = pPlayer;
		lookDist[at] = dist;
		lookCount = Q_min(lookCount + 1, maxLooks);
	}

	// creatures on the other side: a friendly one (a bought turret) looks for creatures, the
	// rest for friendly ones (only while there are any: nothing extra otherwise)
	if (m_friendly || s_friendlyAlive > 0)
	{
		for (int i = 0; i < s_listCount; i++)
		{
			ILsCreature *c = LsCreatureAt(i);
			CBaseEntity *pOther = c ? c->Entity() : nullptr;
			if (!pOther || pOther == pCurrent || !c->IsAlive() || !IsHostileTo(pOther))
				continue;
			float dist = (pOther->pev->origin - pev->origin).Length();
			if (dist > range)
				continue;
			int at = lookCount;
			while (at > 0 && lookDist[at - 1] > dist)
				at--;
			if (at >= maxLooks)
				continue;
			for (int k = Q_min(lookCount, maxLooks - 1); k > at; k--)
			{
				looks[k] = looks[k - 1];
				lookDist[k] = lookDist[k - 1];
			}
			looks[at] = pOther;
			lookDist[at] = dist;
			lookCount = Q_min(lookCount + 1, maxLooks);
		}
	}

	if (pCurrent)
	{
		looks[lookCount] = pCurrent;
		lookDist[lookCount] = (pCurrent->pev->origin - pev->origin).Length();
		lookCount++;
	}

	for (int l = 0; l < lookCount; l++)
	{
		CBaseEntity *pPlayer = looks[l];
		float dist = lookDist[l];

		if (!CanSee(pPlayer, alert || pPlayer == pCurrent, pPlayer == pCurrent))
			continue;

		if (pPlayer == pCurrent)
		{
			currentSeen = true;
			m_targetLastSeen = pPlayer->pev->origin;
			m_targetSeenAt = gpGlobals->time;
		}

		float score = dist;
		switch (t->targeting)
		{
		case LST_WEAKEST: score = pPlayer->pev->health * 10.0f + dist * 0.1f; break;
		case LST_ATTACKER: if (pPlayer == m_lastAttacker) score = -1.0f; break;
		case LST_RANDOM: score = RANDOM_FLOAT(0.0f, 1000.0f); break;
		default: break;
		}

		if (score < bestScore)
		{
			bestScore = score;
			pBest = pPlayer;
			bestDist = dist;
		}
	}

	// a hunter knows where everyone is (zombie hordes): the nearest, seen or not
	if (!pBest && m_behaviour == LSB_CHASE && !pCurrent)
	{
		float closest = 1e9f;
		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
			if (!pPlayer || !IsHostileTo(pPlayer) || SkipsTarget(pPlayer))
				continue;

			float dist = (pPlayer->pev->origin - pev->origin).Length();
			if (dist < closest)
			{
				closest = dist;
				pBest = pPlayer;
			}
		}

		if (pBest)
		{
			Notice(pBest, true);
			return;
		}
	}

	if (pCurrent)
	{
		// (keeps whom it's after unless one much closer shows up)
		if (pBest && pBest != pCurrent && (!currentSeen || bestDist < 0.5f * (pCurrent->pev->origin - pev->origin).Length())
			&& WantsToFight(pBest, bestDist))
			Notice(pBest, true);

		if (m_behaviour == LSB_CHASE && pCurrent == m_target)
		{
			m_targetLastSeen = pCurrent->pev->origin;
			m_targetSeenAt = gpGlobals->time;
		}
		return;
	}

	if (!pBest)
	{
		m_noticing = nullptr;
		return;
	}

	// a passive one runs from whoever comes close
	if (m_aggression <= 0.0f || m_behaviour == LSB_FLEE)
	{
		if (bestDist < m_detect * 0.5f && m_mode != MODE_FLEE && gpGlobals->time >= m_fleeCooldown && t->moves)
			StartFlee(pBest->pev->origin);
		return;
	}

	if (!WantsToFight(pBest, bestDist))
		return;

	// noticing takes a moment at range (instant up close or when alert)
	if (alert || bestDist < m_detect * 0.35f)
	{
		Notice(pBest, alert);
		return;
	}

	if (m_noticing != pBest)
	{
		m_noticing = pBest;
		m_noticeStart = gpGlobals->time;
		m_improv->FaceTo(pBest->pev->origin);
		return;
	}

	float needed = 0.15f + 0.85f * (bestDist / Q_max(m_detect, 1.0f));
	if (gpGlobals->time - m_noticeStart >= needed)
		Notice(pBest, false);
}

void CLsCreature::OnGameEvent(GameEventType event, CBaseEntity *pEntity, CBaseEntity *pOther)
{
	const LsCreatureType *t = GetType();
	if (!t || t->hearScale <= 0.0f || m_mode == MODE_DEAD || m_mode == MODE_ATTACK)
		return;

	// a bullet landing close: someone's shooting this way
	if (event == EVENT_BULLET_IMPACT && pOther && pEntity && IsHostileTo(pEntity))
	{
		const Vector *impact = (const Vector *)pOther;
		if ((*impact - pev->origin).IsLengthLessThan(150.0f) && !m_target)
		{
			if (m_aggression > 0.0f && m_behaviour != LSB_FLEE)
			{
				m_searchSpot = pEntity->pev->origin;
				m_searchUntil = gpGlobals->time + 10.0f;
				m_mode = MODE_SEARCH;
			}
			else if (t->moves && gpGlobals->time >= m_fleeCooldown)
				StartFlee(pEntity->pev->origin);
		}
		return;
	}

	if (!pEntity || !pEntity->IsPlayer() || !IsHostileTo(pEntity))
		return;

	// (EVENT_WEAPON_FIRED needs the player's gun; the bots' table is the same one)
	float range;
	PriorityType priority;
	bool isHostile;
	if (!IsGameEventAudible(event, pEntity, pOther, &range, &priority, &isHostile))
		return;

	float dist = (pEntity->pev->origin - pev->origin).Length();
	if (dist > range * t->hearScale)
		return;

	if (m_target && m_target != pEntity)
		return;

	if (m_target == pEntity)
	{
		// (heard whom it's after: fresher idea where they are, not a sighting)
		if (m_mode == MODE_CHASE && gpGlobals->time - m_targetSeenAt > 1.0f)
			m_targetLastSeen = pEntity->pev->origin;
		return;
	}

	if (m_aggression <= 0.0f || m_behaviour == LSB_FLEE)
	{
		if (priority != PRIORITY_LOW && dist < 600.0f && t->moves && m_mode != MODE_FLEE && gpGlobals->time >= m_fleeCooldown)
			StartFlee(pEntity->pev->origin);
		return;
	}

	// go and look (a guard only near its spot)
	if (!t->moves)
	{
		m_improv->FaceTo(pEntity->pev->origin);
		return;
	}

	if (m_behaviour == LSB_GUARD && (pEntity->pev->origin - m_home).Length() > m_guardRadius * 1.5f)
	{
		m_improv->FaceTo(pEntity->pev->origin);
		return;
	}

	if (m_mode == MODE_HOME || m_mode == MODE_SEARCH)
	{
		// (where it came from, roughly: further away, rougher)
		float err = Q_min(dist * 0.15f, 250.0f);
		m_searchSpot = pEntity->pev->origin + Vector(RANDOM_FLOAT(-err, err), RANDOM_FLOAT(-err, err), 0.0f);
		m_searchUntil = gpGlobals->time + 12.0f;
		m_mode = MODE_SEARCH;
		m_nextRepath = 0.0f;
	}
}

void CLsCreature::TraceAttack(entvars_t *pevAttacker, float flDamage, Vector vecDir, TraceResult *ptr, int bitsDamageType)
{
	if (pev->takedamage == DAMAGE_NO)
		return;

	// (the base does hit groups - head x3 - and the blood)
	CBaseMonster::TraceAttack(pevAttacker, flDamage, vecDir, ptr, bitsDamageType);
}

BOOL CLsCreature::TakeDamage(entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType)
{
	const LsCreatureType *t = GetType();
	if (pev->takedamage == DAMAGE_NO || !IsAlive() || !t || !LsFinite(flDamage) || flDamage <= 0.0f)
		return FALSE;

	CBaseEntity *pAttacker = pevAttacker ? CBaseEntity::Instance(pevAttacker) : nullptr;

	// creatures don't hurt each other (a brute's swing past a runner), but do a friendly one (a
	// bought turret) and it them; players don't hurt a friendly one
	if (pAttacker && pAttacker != this && LsCreatureOf(pAttacker) && !IsHostileTo(pAttacker))
		return FALSE;
	if (m_friendly && pAttacker && pAttacker->IsPlayer())
		return FALSE;

	pev->health -= flDamage;
	m_bitsDamageType |= bitsDamageType;

	if (pev->health <= 0.0f)
	{
		pev->health = 0.0f;
		Killed(pevAttacker, GIB_NORMAL);
		return TRUE;
	}

	if (t->enrageBelow > 0.0f && !m_enraged && pev->health < pev->max_health * t->enrageBelow)
		Enrage();
	else if (gpGlobals->time > m_painSoundAt + 0.6f)
	{
		m_painSoundAt = gpGlobals->time;
		PlaySound(LSS_PAIN);
	}

	// a big hit makes it flinch (not mid-swing)
	if (flDamage > pev->max_health * 0.15f && !m_attackStart && t->moves && gpGlobals->time > m_flinchUntil + 2.0f)
	{
		SetAnim(LSAN_FLINCH, true);
		float duration = GetSequenceDuration();
		m_flinchUntil = gpGlobals->time + clamp(duration, 0.3f, 1.0f);
		m_improv->Stop();
	}

	if (pAttacker && (pAttacker->IsPlayer() || LsNativeOf(pAttacker)) && IsHostileTo(pAttacker))
	{
		m_lastAttacker = pAttacker;

		// hurt badly: run (once in a while)
		float fleeHp = (m_keyFleeHp > 0.0f) ? m_keyFleeHp - 1.0f : t->fleeHealth;
		if (fleeHp > 0.0f && pev->health < pev->max_health * fleeHp && t->moves
			&& m_mode != MODE_FLEE && gpGlobals->time >= m_fleeCooldown)
		{
			StartFlee(pAttacker->pev->origin);
		}
		// passive: run from whoever hurt it
		else if (m_aggression <= 0.0f && t->moves)
		{
			if (m_mode != MODE_FLEE)
				StartFlee(pAttacker->pev->origin);
		}
		// otherwise fight back: it knows where the shot came from
		else if (m_mode != MODE_FLEE && (!m_target || (t->targeting == LST_ATTACKER && m_target != pAttacker)))
		{
			Notice(pAttacker, true);
		}
	}

	return TRUE;
}

void CLsCreature::Killed(entvars_t *pevAttacker, int iGib)
{
	if (pev->deadflag != DEAD_NO)
		return;

	const LsCreatureType *t = GetType();

	pev->deadflag = DEAD_DYING;
	pev->takedamage = DAMAGE_NO;
	pev->solid = SOLID_NOT;
	pev->health = 0.0f;
	pev->velocity.x = pev->velocity.y = 0.0f;
	UTIL_SetOrigin(pev, pev->origin);	// (relinked as not solid: bullets and players pass the body)
	m_mode = MODE_DEAD;
	m_deadAt = gpGlobals->time;
	m_attackStart = 0.0f;
	m_target = nullptr;
	m_hEnemy = nullptr;
	m_forcedTarget = nullptr;
	if (m_improv)
		m_improv->Stop();

	SetAnim(LSAN_DIE, true);
	pev->framerate = 1.0f;
	PlaySound(LSS_DIE);

	// the killer's reward and the log line plugins can read (register_logevent)
	CBaseEntity *pKiller = pevAttacker ? CBaseEntity::Instance(pevAttacker) : nullptr;
	// (killed by a bought turret: the player who placed it gets the kill)
	CLsCreature *pByCreature = LsNativeOf(pKiller);
	if (pByCreature && pByCreature->m_friendly && pByCreature->m_ownerIndex > 0)
	{
		CBasePlayer *pOwner = UTIL_PlayerByIndex(pByCreature->m_ownerIndex);
		if (pOwner && pOwner->IsAlive())
			pKiller = pOwner;
	}
	if (pKiller && pKiller->IsPlayer() && pKiller != this)
	{
		CBasePlayer *pPlayer = static_cast<CBasePlayer *>(pKiller);
		if (t && t->frags && ls_creature_frags.value > 0.0f)
			pPlayer->AddPoints(t->frags, FALSE);
		if (t && t->money)
			pPlayer->AddAccount(t->money, RT_ENEMY_KILLED);

		UTIL_LogPrintf("\"%s<%i><%s><%s>\" triggered \"ls_creature_killed\" (creature \"%s\") (entity \"%d\")\n",
			STRING(pPlayer->pev->netname), GETPLAYERUSERID(pPlayer->edict()), GETPLAYERAUTHID(pPlayer->edict()),
			GetTeam(pPlayer->m_iTeam), t ? t->name : "", entindex());
	}
	else
	{
		UTIL_LogPrintf("World triggered \"ls_creature_killed\" (creature \"%s\") (entity \"%d\")\n", t ? t->name : "", entindex());
	}

	LsUpdateAliveCount();
	pev->nextthink = gpGlobals->time + 0.1f;
}

void CLsCreature::GoHome()
{
	m_mode = MODE_HOME;
	m_target = nullptr;
	m_hEnemy = nullptr;
	m_attackStart = 0.0f;
	m_waitUntil = gpGlobals->time + RANDOM_FLOAT(0.5f, 2.0f);
	m_nextRepath = 0.0f;
	m_improv->Stop();
}

void CLsCreature::StartFlee(const Vector &from)
{
	const LsCreatureType *t = GetType();
	if (!t->moves)
		return;

	// never: flee_hp=0 on this one; or only a few left (ls_creature_flee_last), when a mode
	// wants the round to end rather than players hunting a straggler across the map
	if (m_keyFleeHp > 0.0f && m_keyFleeHp - 1.0f <= 0.0f)
		return;
	if (ls_creature_flee_last.value > 0.0f && !m_friendly && LsAliveCount() <= int(ls_creature_flee_last.value))
		return;

	m_fleeFrom = from;
	m_fleeUntil = gpGlobals->time + RANDOM_FLOAT(5.0f, 9.0f);
	m_mode = MODE_FLEE;
	m_attackStart = 0.0f;

	Vector spot;
	if (PickFleeSpot(&spot, from))
	{
		m_improv->Run();
		m_improv->MoveTo(spot);
	}
}

bool CLsCreature::PickWanderSpot(Vector *spot, float radius) const
{
	if (!TheNavAreaList.empty())
	{
		// (a random area near home: reservoir pick over the list, one pass)
		CNavArea *pick = nullptr;
		int seen = 0;
		for (auto area : TheNavAreaList)
		{
			if ((*area->GetCenter() - m_home).Make2D().IsLengthGreaterThan(radius))
				continue;

			if (RANDOM_LONG(0, seen++) == 0)
				pick = area;
		}

		if (pick)
		{
			*spot = *pick->GetCenter();
			return true;
		}
	}

	// no mesh: a random direction from home, as far as the walls let it
	float yaw = RANDOM_FLOAT(0.0f, 360.0f);
	Vector dir(BotCOS(yaw), BotSIN(yaw), 0.0f);
	Vector start = m_home + Vector(0, 0, 36);
	TraceResult tr;
	UTIL_TraceLine(start, start + dir * RANDOM_FLOAT(radius * 0.3f, radius), ignore_monsters, ENT(pev), &tr);
	*spot = tr.vecEndPos - dir * 24.0f - Vector(0, 0, 36);
	return true;
}

bool CLsCreature::PickFleeSpot(Vector *spot, const Vector &from) const
{
	// back toward home (where it spawned: a wave's spawner, its post), so it comes back into the
	// fight from there after the cooldown instead of ranging off across the map; already near
	// home, or home is where the threat is, it just backs off nearby
	const LsCreatureType *t = GetType();
	bool homeward = t->fleeHome && (m_home - pev->origin).Make2D().IsLengthGreaterThan(300.0f)
		&& (m_home - from).Make2D().Length() > (pev->origin - from).Make2D().Length() * 0.8f;
	if (homeward && !TheNavAreaList.empty())
	{
		CNavArea *pick = nullptr;
		int seen = 0;
		for (auto area : TheNavAreaList)
		{
			if ((*area->GetCenter() - m_home).Make2D().IsLengthGreaterThan(350.0f))
				continue;
			if ((*area->GetCenter() - from).Make2D().IsLengthLessThan(300.0f))
				continue;
			if (RANDOM_LONG(0, seen++) == 0)
				pick = area;
		}
		if (pick)
		{
			*spot = *pick->GetCenter();
			return true;
		}
	}

	if (!TheNavAreaList.empty())
	{
		// the area that best puts distance between it and the threat, among some nearby
		CNavArea *best = nullptr;
		float bestScore = -1e9f;
		int tries = 0;
		for (auto area : TheNavAreaList)
		{
			float mine = (*area->GetCenter() - pev->origin).Make2D().Length();
			if (mine < 250.0f || mine > (t->fleeHome ? 800.0f : 1400.0f))
				continue;

			// (only some of them, at random: a busy map has thousands)
			if (RANDOM_LONG(0, 3) != 0)
				continue;

			float theirs = (*area->GetCenter() - from).Make2D().Length();
			float score = theirs - mine * 0.5f + RANDOM_FLOAT(0.0f, 150.0f);
			if (score > bestScore)
			{
				bestScore = score;
				best = area;
			}

			if (++tries > 60)
				break;
		}

		if (best)
		{
			*spot = *best->GetCenter();
			return true;
		}
	}

	Vector away = pev->origin - from;
	away.z = 0;
	if (away.IsZero())
		away = Vector(1, 0, 0);
	away.NormalizeInPlace();

	Vector start = pev->origin + Vector(0, 0, 36);
	TraceResult tr;
	UTIL_TraceLine(start, start + away * 700.0f, ignore_monsters, ENT(pev), &tr);
	*spot = tr.vecEndPos - away * 24.0f - Vector(0, 0, 36);
	return true;
}

CBaseEntity *CLsCreature::NextPatrolCorner(CBaseEntity *pFrom) const
{
	if (!pFrom)
		return m_patrolStart ? UTIL_FindEntityByTargetname(nullptr, STRING(m_patrolStart)) : nullptr;

	if (!pFrom->pev->target)
		return nullptr;

	return UTIL_FindEntityByTargetname(nullptr, STRING(pFrom->pev->target));
}

void CLsCreature::OnMoveToSuccess(const Vector &goal)
{
	switch (m_mode)
	{
	case MODE_HOME:
		if (m_behaviour == LSB_PATROL && m_patrolCorner)
		{
			CBaseEntity *pCorner = m_patrolCorner;
			float wait = 0.0f;
			if (FClassnameIs(pCorner->pev, "path_corner"))
				wait = static_cast<CPathCorner *>(pCorner)->GetDelay();

			m_waitUntil = gpGlobals->time + Q_max(wait, 0.5f);
			m_patrolCorner = NextPatrolCorner(pCorner);
		}
		else
			m_waitUntil = gpGlobals->time + RANDOM_FLOAT(2.0f, 6.0f);
		break;

	case MODE_SEARCH:
		// (got there: a look around, then home)
		m_waitUntil = gpGlobals->time + RANDOM_FLOAT(2.0f, 4.0f);
		m_searchUntil = Q_min(m_searchUntil, gpGlobals->time + 4.0f);
		break;

	case MODE_FLEE:
		m_fleeUntil = Q_min(m_fleeUntil, gpGlobals->time + RANDOM_FLOAT(1.0f, 3.0f));
		break;

	default:
		break;
	}
}

void CLsCreature::OnMoveToFailure(const Vector &goal, int reason)
{
	switch (m_mode)
	{
	case MODE_HOME:
		if (m_behaviour == LSB_PATROL && m_patrolCorner)
			m_patrolCorner = NextPatrolCorner(m_patrolCorner);
		m_waitUntil = gpGlobals->time + RANDOM_FLOAT(0.5f, 1.5f);
		break;

	case MODE_SEARCH:
		m_searchUntil = gpGlobals->time;
		break;

	case MODE_FLEE:
	{
		Vector spot;
		if (PickFleeSpot(&spot, m_fleeFrom))
			m_improv->MoveTo(spot);
		break;
	}

	default:
		// chase: try again shortly (the follower already wiggled)
		m_nextRepath = gpGlobals->time + 0.3f;
		break;
	}
}

void CLsCreature::UpdateHome()
{
	const LsCreatureType *t = GetType();
	float now = gpGlobals->time;

	if (!t->moves)
	{
		// a turret sweeps slowly
		if (!m_improv->HasFaceTo() && now >= m_waitUntil)
		{
			float yaw = m_homeAngles.y + RANDOM_FLOAT(-90.0f, 90.0f);
			m_improv->FaceTo(pev->origin + Vector(BotCOS(yaw), BotSIN(yaw), 0) * 100.0f);
			m_waitUntil = now + RANDOM_FLOAT(2.0f, 4.0f);
		}
		return;
	}

	switch (m_behaviour)
	{
	case LSB_IDLE:
	case LSB_FLEE:
	case LSB_CHASE:		// (a hunter with nobody left to hunt)
		if (m_improv->HasGoal())
			m_improv->Stop();
		break;

	case LSB_GUARD:
		if ((pev->origin - m_home).Make2D().IsLengthGreaterThan(40.0f))
		{
			if (!m_improv->HasGoal() && now >= m_waitUntil)
			{
				m_improv->Walk();
				m_improv->MoveTo(m_home);
			}
		}
		else if (m_improv->HasGoal())
		{
			m_improv->Stop();
			m_improv->FaceTo(pev->origin + Vector(BotCOS(m_homeAngles.y), BotSIN(m_homeAngles.y), 0) * 100.0f);
		}
		break;

	case LSB_PATROL:
		if (!m_patrolCorner && m_patrolStart)
			m_patrolCorner = NextPatrolCorner(nullptr);

		if (m_patrolCorner)
		{
			if (!m_improv->HasGoal() && now >= m_waitUntil)
			{
				CBaseEntity *pCorner = m_patrolCorner;
				m_improv->Walk();
				m_improv->MoveTo(pCorner->pev->origin);
			}
			break;
		}
		// (no path_corner chain: walk the nav areas around home, which is a wander)

	case LSB_WANDER:
	default:
		if (!m_improv->HasGoal() && now >= m_waitUntil)
		{
			Vector spot;
			if (PickWanderSpot(&spot, m_guardRadius))
			{
				m_improv->Walk();
				m_improv->MoveTo(spot);
			}
			m_waitUntil = now + RANDOM_FLOAT(8.0f, 14.0f);		// (if it never arrives)
		}
		break;
	}

	if (now >= m_nextIdleSound)
	{
		m_nextIdleSound = now + RANDOM_FLOAT(6.0f, 15.0f);
		PlaySound(LSS_IDLE, 0.8f);
	}
}

void CLsCreature::UpdateChase()
{
	const LsCreatureType *t = GetType();
	CBaseEntity *pTarget = m_target;
	float now = gpGlobals->time;

	if (!pTarget || !IsHostileTo(pTarget))
	{
		m_target = nullptr;
		m_hEnemy = nullptr;

		// lost them: look where they were last
		if (t->moves && now - m_targetSeenAt < 10.0f)
		{
			m_searchSpot = m_targetLastSeen;
			m_searchUntil = now + 10.0f;
			m_mode = MODE_SEARCH;
		}
		else
			GoHome();
		return;
	}

	float sinceSeen = now - m_targetSeenAt;

	// out of sight too long: go look where they were
	if (sinceSeen > 6.0f && m_behaviour != LSB_CHASE && m_forcedTarget != pTarget)
	{
		m_target = nullptr;
		m_hEnemy = nullptr;
		m_searchSpot = m_targetLastSeen;
		m_searchUntil = now + 10.0f;
		m_mode = t->moves ? MODE_SEARCH : MODE_HOME;
		return;
	}

	// a guard doesn't follow far from its spot
	if (m_behaviour == LSB_GUARD && m_forcedTarget != pTarget && (m_targetLastSeen - m_home).Length() > m_guardRadius * 1.6f)
	{
		GoHome();
		return;
	}

	float dist = Dist2D(pTarget);
	float dz = Q_fabs(pTarget->pev->origin.z - pev->origin.z);
	bool seenNow = sinceSeen < 0.4f;

	if (t->attack == LSA_MELEE && dist <= t->attackRange && dz < 56.0f && seenNow)
	{
		m_mode = MODE_ATTACK;
		return;
	}

	if (t->attack == LSA_RANGED && dist <= t->attackRange && seenNow)
	{
		m_mode = MODE_ATTACK;
		return;
	}

	if (!t->moves)
	{
		m_improv->FaceTo(m_targetLastSeen);
		return;
	}

	// can't get to them (up a ladder it can't climb, on a ledge): close below them and not getting
	// any higher. A few seconds of that and it leaves them be a while - another target, or back
	// to its round (it doesn't stand under the ladder for ever)
	bool onLadder = pTarget->IsPlayer() && static_cast<CBasePlayer *>(pTarget)->IsOnLadder() && !t->canClimb;
	bool below = dist < 250.0f && pTarget->pev->origin.z - pev->origin.z > 56.0f;
	if (!(below || onLadder))
		m_cantReachSince = 0.0f;
	else if (m_cantReachSince <= 0.0f || pev->origin.z > m_cantReachZ + 16.0f)
	{
		// (climbing stairs towards them: still getting there)
		m_cantReachSince = now;
		m_cantReachZ = pev->origin.z;
	}
	else if (now - m_cantReachSince > 3.0f && m_forcedTarget != pTarget)
	{
		m_skipTarget = pTarget;
		m_skipUntil = now + 8.0f;
		m_cantReachSince = 0.0f;
		GoHome();
		return;
	}

	m_improv->Run();
	m_improv->LookAt(m_targetLastSeen);

	// an assassin's hop: now and then a jump to one side while it closes in
	if (t->hops > 0.0f && dist > 150.0f && (pev->flags & FL_ONGROUND) && RANDOM_FLOAT(0.0f, 1.0f) < t->hops * 0.1f && m_improv->Jump())
	{
		float side = RANDOM_LONG(0, 1) ? 90.0f : -90.0f;
		float yaw = UTIL_VecToYaw(m_targetLastSeen - pev->origin) + side;
		m_improv->ApplyForce(Vector(BotCOS(yaw), BotSIN(yaw), 0.0f) * 220.0f);
	}

	// a flanker heads for a spot beside the target (its own side, kept) until it's close
	Vector goal = m_targetLastSeen;
	if (t->flank && dist > 350.0f)
	{
		float yaw = UTIL_VecToYaw(m_targetLastSeen - pev->origin) + ((entindex() & 1) ? 70.0f : -70.0f);
		Vector beside = m_targetLastSeen + Vector(BotCOS(yaw), BotSIN(yaw), 0.0f) * Q_min(dist * 0.5f, 300.0f);
		TraceResult tr;
		UTIL_TraceLine(m_targetLastSeen + Vector(0, 0, 36), beside + Vector(0, 0, 36), ignore_monsters, ENT(pev), &tr);
		if (tr.flFraction > 0.8f)
			goal = beside;
	}

	if (now >= m_nextRepath || (goal - m_lastMoveTo).IsLengthGreaterThan(96.0f))
	{
		m_nextRepath = now + RANDOM_FLOAT(0.4f, 0.7f);
		m_lastMoveTo = goal;
		m_improv->MoveTo(goal);
	}
}

void CLsCreature::StartAttack()
{
	const LsCreatureType *t = GetType();
	m_attackStart = gpGlobals->time;
	m_attackLanded = false;

	// (planted for the swing from its first frame, not sliding into it)
	if (t->moves)
	{
		m_improv->Stop();
		pev->velocity.x = pev->velocity.y = 0.0f;
	}

	SetAnim(LSAN_ATTACK, true);

	// (the swing fits the attack rate: a long HL swing plays faster, never slower than made)
	float duration = GetSequenceDuration();
	float period = 1.0f / (t->attackRate * m_animScale);
	m_attackFramerate = (duration > 0.0f) ? clamp(duration / period, m_animScale, 3.0f) : m_animScale;
	m_attackAnimEnd = gpGlobals->time + ((duration > 0.0f) ? duration / m_attackFramerate : period);
	pev->framerate = m_attackFramerate;
	PlaySound(LSS_ATTACK);
}

void CLsCreature::LandAttack()
{
	const LsCreatureType *t = GetType();
	CBaseEntity *pTarget = m_target;
	m_attackLanded = true;

	if (!pTarget || !IsHostileTo(pTarget))
		return;

	Vector eyes = pev->origin + pev->view_ofs;
	Vector body = pTarget->pev->origin + pTarget->pev->view_ofs * 0.6f;

	if (t->attack == LSA_MELEE)
	{
		// (still in reach and in front: a step back in time dodges it)
		float dist = Dist2D(pTarget);
		TraceResult tr;
		UTIL_TraceLine(eyes, body, ignore_monsters, ENT(pev), &tr);
		if (dist <= t->attackRange * 1.25f && Q_fabs(pTarget->pev->origin.z - pev->origin.z) < 64.0f && tr.flFraction == 1.0f)
		{
			pTarget->TakeDamage(pev, pev, m_damage, DMG_SLASH);
			pTarget->pev->punchangle.x = RANDOM_FLOAT(-6.0f, 6.0f);
			pTarget->pev->punchangle.z = RANDOM_FLOAT(-6.0f, 6.0f);
			PlaySound(LSS_HIT);
		}
		else
			PlaySound(LSS_MISS);
		return;
	}

	if (t->attack == LSA_RANGED)
	{
		// a shot with spread: the trace hits whatever is in the way (another player too)
		Vector dir = body - eyes;
		dir.NormalizeInPlace();
		Vector angles = UTIL_VecToAngles(dir);
		angles.x = -angles.x;
		angles.x += RANDOM_FLOAT(-t->spread, t->spread);
		angles.y += RANDOM_FLOAT(-t->spread, t->spread);
		UTIL_MakeVectors(angles);
		dir = gpGlobals->v_forward;

		TraceResult tr;
		UTIL_TraceLine(eyes, eyes + dir * (t->attackRange * 1.2f), dont_ignore_monsters, ENT(pev), &tr);

		CBaseEntity *pHit = tr.pHit ? CBaseEntity::Instance(tr.pHit) : nullptr;
		// (a creature only when it's on the other side; a friendly one's shot never hurts a player)
		if (pHit && pHit->pev->takedamage != DAMAGE_NO && (LsCreatureOf(pHit) ? IsHostileTo(pHit) : !(m_friendly && pHit->IsPlayer())))
		{
			ClearMultiDamage();
			pHit->TraceAttack(pev, m_damage, dir, &tr, DMG_BULLET);
			ApplyMultiDamage(pev, pev);
			PlaySound(LSS_HIT);
		}

		if (t->shotFx == 1 && s_zapSprite)
		{
			// an electric arc, the vortigaunt's
			MESSAGE_BEGIN(MSG_PAS, SVC_TEMPENTITY, eyes);
				WRITE_BYTE(TE_BEAMPOINTS);
				WRITE_COORD(eyes.x);
				WRITE_COORD(eyes.y);
				WRITE_COORD(eyes.z);
				WRITE_COORD(tr.vecEndPos.x);
				WRITE_COORD(tr.vecEndPos.y);
				WRITE_COORD(tr.vecEndPos.z);
				WRITE_SHORT(s_zapSprite);
				WRITE_BYTE(0);		// start frame
				WRITE_BYTE(10);		// frame rate
				WRITE_BYTE(2);		// life (0.2 s)
				WRITE_BYTE(40);		// width
				WRITE_BYTE(60);		// noise
				WRITE_BYTE(96);		// r g b: the vortigaunt's green
				WRITE_BYTE(255);
				WRITE_BYTE(128);
				WRITE_BYTE(200);	// brightness
				WRITE_BYTE(30);		// scroll
			MESSAGE_END();
		}
		else if (t->shotFx == 0)
		{
			MESSAGE_BEGIN(MSG_PAS, SVC_TEMPENTITY, eyes);
				WRITE_BYTE(TE_TRACER);
				WRITE_COORD(eyes.x);
				WRITE_COORD(eyes.y);
				WRITE_COORD(eyes.z);
				WRITE_COORD(tr.vecEndPos.x);
				WRITE_COORD(tr.vecEndPos.y);
				WRITE_COORD(tr.vecEndPos.z);
			MESSAGE_END();
		}
	}
}

void CLsCreature::UpdateAttack()
{
	const LsCreatureType *t = GetType();
	CBaseEntity *pTarget = m_target;
	float now = gpGlobals->time;

	if (!pTarget || !IsHostileTo(pTarget))
	{
		m_attackStart = 0.0f;
		m_mode = MODE_CHASE;
		return;
	}

	m_improv->FaceTo(pTarget->pev->origin);

	// it stands for the swing (no sliding at them mid-swing); between swings a melee one keeps
	// close, and a target stepping well out of reach mid-swing breaks it off
	float reachNow = Dist2D(pTarget);
	if (m_attackStart > 0.0f)
	{
		m_improv->Stop();
		if (t->attack == LSA_MELEE && !m_attackLanded && reachNow > t->attackRange * 1.6f)
		{
			m_attackStart = 0.0f;
			m_attackAnimEnd = 0.0f;
			m_mode = MODE_CHASE;
			return;
		}
	}
	else if (t->attack == LSA_MELEE && t->moves && reachNow > 40.0f)
	{
		m_improv->Run();
		m_improv->MoveTo(pTarget->pev->origin);
	}
	else
		m_improv->Stop();

	if (m_attackStart <= 0.0f)
	{
		// (out of reach or out of sight before the next swing: chase again)
		float dist = Dist2D(pTarget);
		bool seenNow = now - m_targetSeenAt < 0.4f;
		bool inReach = (t->attack == LSA_MELEE) ? (dist <= t->attackRange && Q_fabs(pTarget->pev->origin.z - pev->origin.z) < 56.0f)
			: (dist <= t->attackRange);

		if (!inReach || !seenNow)
		{
			m_mode = MODE_CHASE;
			return;
		}

		if (now >= m_nextAttack)
			StartAttack();
		return;
	}

	if (!m_attackLanded && now >= m_attackStart + t->attackDelay / m_animScale)
	{
		LandAttack();
		m_nextAttack = m_attackStart + 1.0f / (t->attackRate * m_animScale);
	}

	if (m_attackLanded && now >= m_nextAttack - 0.05f)
		m_attackStart = 0.0f;
}

void CLsCreature::UpdateSearch()
{
	float now = gpGlobals->time;

	if (now >= m_searchUntil)
	{
		GoHome();
		return;
	}

	if (!m_improv->HasGoal() && now >= m_waitUntil && (pev->origin - m_searchSpot).Make2D().IsLengthGreaterThan(48.0f))
	{
		m_improv->Run();
		m_improv->MoveTo(m_searchSpot);
		m_waitUntil = now + 1.0f;
	}
	else if (!m_improv->HasGoal() && !m_improv->HasFaceTo())
	{
		// (a look around)
		float yaw = RANDOM_FLOAT(0.0f, 360.0f);
		m_improv->FaceTo(pev->origin + Vector(BotCOS(yaw), BotSIN(yaw), 0) * 100.0f);
	}
}

void CLsCreature::UpdateFlee()
{
	float now = gpGlobals->time;

	if (now >= m_fleeUntil)
	{
		m_fleeCooldown = now + RANDOM_FLOAT(6.0f, 10.0f);

		// (still being shot at by whoever hurt it, and it fights back: back at them)
		CBaseEntity *pAttacker = m_lastAttacker;
		if (pAttacker && IsHostileTo(pAttacker) && m_aggression > 0.0f && m_behaviour != LSB_FLEE)
			Notice(pAttacker, true);
		else
			GoHome();
		return;
	}

	m_improv->Run();
	if (!m_improv->HasGoal())
	{
		Vector spot;
		if (PickFleeSpot(&spot, m_fleeFrom))
			m_improv->MoveTo(spot);
	}
}

void CLsCreature::CreatureThink()
{
	LsMark("creature think", entindex());
	double t0 = LsPerfClock();
	CreatureThinkBody();
	PublishState();
	if (t0 > 0.0)
		LsPerfAddCreature(LsPerfClock() - t0);
	LsMark("creature think end", entindex());
}

void CLsCreature::CreatureThinkBody()
{
	const float upkeepRate = 1.0f / 30.0f;
	float now = gpGlobals->time;

	pev->nextthink = now + upkeepRate;

	float interval = StudioFrameAdvance();
	DispatchAnimEvents(interval);

	const LsCreatureType *t = GetType();
	if (!t || !m_improv)
		return;

	if (m_mode == MODE_DEAD)
	{
		// the death animation ends on its last frame and stays there
		if (pev->deadflag == DEAD_DYING && (m_fSequenceFinished || now > m_deadAt + 3.0f))
		{
			pev->deadflag = DEAD_DEAD;
			pev->framerate = 0.0f;
			pev->frame = 255.0f;
		}

		float corpse = (m_respawn > 0.0f) ? Q_min(t->corpseTime, m_respawn) : t->corpseTime;
		if (!(pev->effects & EF_NODRAW) && now > m_deadAt + corpse)
		{
			// (a command's one is gone for good; a map's waits for its respawn or the next round)
			if (m_respawn <= 0.0f && !m_fromMap && !m_persist)
			{
				UTIL_Remove(this);
				return;
			}

			pev->effects |= EF_NODRAW;
		}

		if (m_respawn > 0.0f && now > m_deadAt + m_respawn)
			ComeBack();

		pev->nextthink = now + 0.1f;
		return;
	}

	m_improv->OnUpkeep(upkeepRate);

	// fell out of the world: gone
	if (pev->origin.z < -8000.0f)
	{
		KillQuietly();
		return;
	}

	if (now < m_nextFullThink)
	{
		UpdateAnimation();
		return;
	}

	m_nextFullThink = now + 0.1f;

	if (now >= m_nextSense)
	{
		m_nextSense = now + RANDOM_FLOAT(0.2f, 0.3f);
		UpdateSenses();
	}

	// (a flinch holds it still while it plays)
	if (now < m_flinchUntil)
	{
		m_improv->Stop();
		UpdateAnimation();
		return;
	}

	UpdateStuck();

	switch (m_mode)
	{
	case MODE_HOME: UpdateHome(); break;
	case MODE_CHASE: UpdateChase(); break;
	case MODE_ATTACK: UpdateAttack(); break;
	case MODE_SEARCH: UpdateSearch(); break;
	case MODE_FLEE: UpdateFlee(); break;
	default: break;
	}

	UpdateAnimation();

	if (ls_creature_debug.value > 0.0f)
	{
		static const char *modes[] = { "home", "chase", "attack", "search", "flee", "dead" };
		CBaseEntity *pTarget = m_target;
		UTIL_DrawBeamPoints(pev->origin, pev->origin + Vector(0, 0, 90), 2,
			m_mode == MODE_ATTACK ? 255 : 0, m_mode == MODE_HOME ? 255 : 0, m_mode == MODE_SEARCH ? 255 : 0);
		if (pTarget)
			UTIL_DrawBeamPoints(pev->origin + Vector(0, 0, 60), pTarget->pev->origin, 2, 255, 0, 0);
		if (ls_creature_debug.value > 2.0f)
			CONSOLE_ECHO("%s#%d %s hp %.0f\n", t->name, entindex(), modes[m_mode], pev->health);
	}
}

// ---------------------------------------------------------------------------------------------
// The barnacle (see creature.h)

LINK_ENTITY_TO_CLASS(ls_barnacle, CLsBarnacle, CCSMonster)

void CLsBarnacle::KeyValue(KeyValueData *pkvd)
{
	const char *key = pkvd->szKeyName;
	const char *value = pkvd->szValue;
	if (FStrEq(key, "health") || FStrEq(key, "hp"))
		m_maxHealth = LsNum(value, 1.0f, 10000.0f, 25.0f);
	else if (FStrEq(key, "damage"))
		m_bite = LsNum(value, 1.0f, 1000.0f, 10.0f);
	else if (FStrEq(key, "length"))
		m_maxLength = LsNum(value, 32.0f, 2048.0f, 512.0f);
	else if (FStrEq(key, "speed"))
		m_haul = LsNum(value, 10.0f, 600.0f, 80.0f);
	else if (FStrEq(key, "hates"))
		m_hates = LsHatesFromString(value);
	else
	{
		CBaseMonster::KeyValue(pkvd);
		return;
	}
	pkvd->fHandled = TRUE;
}

void CLsBarnacle::Precache()
{
	// (the budget rules as for creatures: no room, no barnacle - never a failed map)
	int reserveModels, reserveSounds;
	LsReserve(&reserveModels, &reserveSounds);
	m_ready = false;
	if (!g_pFileSystem->FileExists("models/barnacle.mdl"))
	{
		CONSOLE_ECHO("ls_barnacle: models/barnacle.mdl isn't there; no barnacle\n");
		return;
	}
	if (s_modelHigh + 1 > LS_MAX_MODELS - reserveModels)
	{
		CONSOLE_ECHO("ls_barnacle: no precache room for its model (models %d/%d); no barnacle\n", s_modelHigh, LS_MAX_MODELS);
		return;
	}
	LsPrecacheModel("models/barnacle.mdl");
	static const char *const sounds[] = { "barnacle/bcl_alert2.wav", "barnacle/bcl_chew1.wav", "barnacle/bcl_chew2.wav", "barnacle/bcl_die1.wav" };
	for (const char *snd : sounds)
	{
		if (s_soundHigh + 1 > LS_MAX_SOUNDS - reserveSounds)
			break;
		LsPrecacheSound(snd);
	}
	m_ready = true;
}

void CLsBarnacle::Spawn()
{
	if (m_maxHealth <= 0.0f)
		m_maxHealth = 25.0f;
	if (m_bite <= 0.0f)
		m_bite = 10.0f;
	if (m_maxLength <= 0.0f)
		m_maxLength = 512.0f;
	if (m_haul <= 0.0f)
		m_haul = 80.0f;

	Precache();
	if (!m_ready)
	{
		pev->flags |= FL_KILLME;
		return;
	}

	MAKE_STRING_CLASS("ls_barnacle", pev);
	SET_MODEL(edict(), "models/barnacle.mdl");

	// it always hangs from a ceiling: one left floating (or on the floor) moves up to the
	// ceiling above it; none within reach (open sky) leaves it where the mapper put it
	TraceResult up;
	UTIL_TraceLine(pev->origin, pev->origin + Vector(0, 0, 4096), ignore_monsters, ENT(pev), &up);
	if (!up.fStartSolid && !up.fAllSolid && up.flFraction < 1.0f)
	{
		bool sky = false;
		if (const char *tex = TRACE_TEXTURE(up.pHit ? up.pHit : ENT(0), pev->origin, up.vecEndPos + Vector(0, 0, 8)))
			sky = !Q_stricmp(tex, "sky");
		if (!sky)
		{
			if (ls_creature_debug.value > 0.0f)
				UTIL_LogPrintf("ls_barnacle: hung from the ceiling %.0f units above its spot\n", up.vecEndPos.z - pev->origin.z);
			pev->origin = up.vecEndPos;
		}
	}
	Setup();
}

void CLsBarnacle::Setup()
{
	pev->movetype = MOVETYPE_NONE;
	pev->solid = SOLID_SLIDEBOX;
	pev->takedamage = DAMAGE_AIM;
	pev->flags |= FL_MONSTER;
	pev->deadflag = DEAD_NO;
	pev->effects &= ~EF_NODRAW;
	pev->health = pev->max_health = m_maxHealth;
	UTIL_SetSize(pev, Vector(-16, -16, -32), Vector(16, 16, 0));
	UTIL_SetOrigin(pev, pev->origin);
	m_victim = nullptr;
	m_nextBite = 0.0f;
	m_bloodColor = BLOOD_COLOR_RED;

	pev->sequence = LookupSequence("idle3");
	pev->frame = 0;
	ResetSequenceInfo();
	InitBoneControllers();

	// the tongue: down to the floor below, at most its length
	TraceResult tr;
	UTIL_TraceLine(pev->origin, pev->origin - Vector(0, 0, m_maxLength), ignore_monsters, ENT(pev), &tr);
	SetTongue((pev->origin - tr.vecEndPos).Length() - 8.0f);

	SetThink(&CLsBarnacle::BarnacleThink);
	pev->nextthink = gpGlobals->time + RANDOM_FLOAT(0.1f, 0.3f);
}

void CLsBarnacle::Restart()
{
	if (!m_ready)
		return;
	Release();
	Setup();
}

void CLsBarnacle::SetTongue(float length)
{
	m_tongue = clamp(length, 8.0f, m_maxLength);
	SetBoneController(0, -m_tongue);
}

bool CLsBarnacle::Wants(CBasePlayer *pPlayer) const
{
	return pPlayer && pPlayer->IsAlive() && LsIsHostilePlayer(pPlayer, m_hates);
}

void CLsBarnacle::Release()
{
	CBaseEntity *pVictim = m_victim;
	if (pVictim && pVictim->IsPlayer())
		pVictim->pev->velocity = Vector(0, 0, -50);
	m_victim = nullptr;
}

void CLsBarnacle::BarnacleThink()
{
	pev->nextthink = gpGlobals->time + 0.05f;
	StudioFrameAdvance();

	if (pev->deadflag != DEAD_NO)
		return;

	CBaseEntity *pHeld = m_victim;
	CBasePlayer *pVictim = (pHeld && pHeld->IsPlayer()) ? static_cast<CBasePlayer *>(pHeld) : nullptr;
	if (pVictim && !Wants(pVictim))
	{
		Release();
		pVictim = nullptr;
		pev->sequence = LookupSequence("idle3");
		ResetSequenceInfo();
	}

	if (!pVictim)
	{
		// someone in the tongue's line: caught
		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
			if (!Wants(pPlayer))
				continue;

			Vector to = pPlayer->pev->origin - pev->origin;
			if (to.Make2D().IsLengthGreaterThan(20.0f) || to.z > 0.0f || -to.z > m_tongue + 36.0f)
				continue;

			m_victim = pPlayer;
			EMIT_SOUND_DYN(edict(), CHAN_WEAPON, "barnacle/bcl_alert2.wav", VOL_NORM, ATTN_NORM, 0, PITCH_NORM);
			if (ls_creature_debug.value > 0.0f)
				CONSOLE_ECHO("ls_barnacle#%d caught %s\n", entindex(), STRING(pPlayer->pev->netname));
			pev->sequence = LookupSequence("attack1");
			ResetSequenceInfo();
			break;
		}
		return;
	}

	// haul them up to the mouth, then chew
	Vector mouth = pev->origin - Vector(0, 0, 40);
	Vector at = pVictim->pev->origin + Vector(0, 0, pVictim->pev->maxs.z);		// (their head)
	float gap = at.z - mouth.z;
	Vector pos = pVictim->pev->origin;
	pos.x = pev->origin.x;
	pos.y = pev->origin.y;
	if (gap < -2.0f)
		pos.z += Q_min(-gap, m_haul * 0.05f);
	UTIL_SetOrigin(pVictim->pev, pos);
	pVictim->pev->velocity = Vector(0, 0, 0);
	SetTongue(pev->origin.z - (pos.z + pVictim->pev->maxs.z));

	if (gap >= -4.0f)
	{
		if (pev->sequence != LookupSequence("chewcycle"))
		{
			pev->sequence = LookupSequence("chewcycle");
			ResetSequenceInfo();
		}
		if (gpGlobals->time >= m_nextBite)
		{
			m_nextBite = gpGlobals->time + 1.0f;
			EMIT_SOUND_DYN(edict(), CHAN_VOICE, RANDOM_LONG(0, 1) ? "barnacle/bcl_chew1.wav" : "barnacle/bcl_chew2.wav", VOL_NORM, ATTN_NORM, 0, PITCH_NORM);
			pVictim->TakeDamage(pev, pev, m_bite, DMG_SLASH);
		}
	}
}

BOOL CLsBarnacle::TakeDamage(entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType)
{
	if (pev->takedamage == DAMAGE_NO || pev->deadflag != DEAD_NO || !LsFinite(flDamage) || flDamage <= 0.0f)
		return FALSE;

	pev->health -= flDamage;
	if (pev->health <= 0.0f)
		Killed(pevAttacker, GIB_NORMAL);
	return TRUE;
}

void CLsBarnacle::Killed(entvars_t *pevAttacker, int iGib)
{
	if (pev->deadflag != DEAD_NO)
		return;

	Release();
	pev->deadflag = DEAD_DEAD;
	pev->takedamage = DAMAGE_NO;
	pev->solid = SOLID_NOT;
	pev->health = 0.0f;
	UTIL_SetOrigin(pev, pev->origin);
	SetTongue(8.0f);
	pev->sequence = LookupSequence("death");
	pev->frame = 0;
	ResetSequenceInfo();
	EMIT_SOUND_DYN(edict(), CHAN_VOICE, "barnacle/bcl_die1.wav", VOL_NORM, ATTN_NORM, 0, PITCH_NORM);

	CBaseEntity *pKiller = pevAttacker ? CBaseEntity::Instance(pevAttacker) : nullptr;
	if (pKiller && pKiller->IsPlayer())
	{
		CBasePlayer *pPlayer = static_cast<CBasePlayer *>(pKiller);
		UTIL_LogPrintf("\"%s<%i><%s><%s>\" triggered \"ls_barnacle_killed\" (entity \"%d\")\n",
			STRING(pPlayer->pev->netname), GETPLAYERUSERID(pPlayer->edict()), GETPLAYERAUTHID(pPlayer->edict()),
			GetTeam(pPlayer->m_iTeam), entindex());
	}
}

// ---------------------------------------------------------------------------------------------
// The ls_creature server command

static CBaseEntity *LsCreatureEntArg(const char *arg)
{
	int index = Q_atoi(arg);
	if (index <= 0 || index >= gpGlobals->maxEntities)
		return nullptr;

	edict_t *pEdict = INDEXENT(index);
	if (FNullEnt(pEdict) || pEdict->free)
		return nullptr;

	CBaseEntity *pEntity = CBaseEntity::Instance(pEdict);
	return LsCreatureOf(pEntity) ? pEntity : nullptr;
}

// "all" / a type name / an entity index: each creature that matches
template <typename Functor>
static int LsForEachMatching(const char *arg, Functor func)
{
	int n = 0;
	bool all = !Q_stricmp(arg, "all");
	CBaseEntity *pOne = all ? nullptr : LsCreatureEntArg(arg);

	for (int i = 0; i < LsCreatureCount(); i++)
	{
		ILsCreature *c = LsCreatureAt(i);
		if (!c)
			continue;

		const LsCreatureType *t = c->Type();
		if (all || c->Entity() == pOne || (!pOne && t && !Q_stricmp(t->name, arg)))
		{
			func(c);
			n++;
		}
	}

	return n;
}

void LsCreature_f()
{
	LsMark("ls_creature command");
	const char *cmd = CMD_ARGV(1);

	if (!Q_stricmp(cmd, "spawn"))
	{
		// spawn <type> <x> <y> <z> [yaw] [key=value ...]
		if (CMD_ARGC() < 6)
		{
			CONSOLE_ECHO("usage: ls_creature spawn <type> <x> <y> <z> [yaw] [key=value ...]\n");
			return;
		}

		Vector origin(Q_atof(CMD_ARGV(3)), Q_atof(CMD_ARGV(4)), Q_atof(CMD_ARGV(5)));
		if (!LsPosOk(origin))
		{
			CONSOLE_ECHO("ls_creature: that position is outside the world\n");
			return;
		}

		int next = 6;
		float yaw = RANDOM_FLOAT(0.0f, 360.0f);
		if (CMD_ARGC() > 6 && !Q_strchr(CMD_ARGV(6), '='))
		{
			yaw = LsYaw(Q_atof(CMD_ARGV(6)));
			next = 7;
		}

		char keys[256] = "";
		for (int i = next; i < CMD_ARGC(); i++)
		{
			Q_strlcat(keys, CMD_ARGV(i));
			Q_strlcat(keys, " ");
		}

		CBaseEntity *pEntity = LsCreatureSpawn(CMD_ARGV(2), origin, yaw, keys);
		if (pEntity)
			CONSOLE_ECHO("ls_creature: %s spawned (entity %d)\n", CMD_ARGV(2), pEntity->entindex());
		return;
	}

	if (!Q_stricmp(cmd, "kill"))
	{
		int n = 0, foreign = 0;
		LsForEachMatching(CMD_ARGV(2), [&](ILsCreature *c) { if (LsIsAdopted(c)) foreign++; else { c->Kill(); n++; } });
		CONSOLE_ECHO("ls_creature: %d killed\n", n);
		if (foreign)
			CONSOLE_ECHO("ls_creature: %d adopted ones left alone - their own module runs them (use its commands)\n", foreign);
		return;
	}

	if (!Q_stricmp(cmd, "remove"))
	{
		CBaseEntity *gone[LS_CREATURE_MAX_LIST];
		int count = 0, foreign = 0;
		LsForEachMatching(CMD_ARGV(2), [&](ILsCreature *c) { if (LsIsAdopted(c)) foreign++; else if (count < LS_CREATURE_MAX_LIST) gone[count++] = c->Entity(); });
		if (foreign)
			CONSOLE_ECHO("ls_creature: %d adopted ones left alone - their own module runs them (use its commands)\n", foreign);
		for (int i = 0; i < count; i++)
		{
			LsCreatureUnregister(gone[i]);
			UTIL_Remove(gone[i]);
		}
		CONSOLE_ECHO("ls_creature: %d removed\n", count);
		return;
	}

	if (!Q_stricmp(cmd, "target"))
	{
		// target <ent|all|type> <entity index | 0>
		int index = Q_atoi(CMD_ARGV(3));
		CBaseEntity *pTarget = (index > 0 && index < gpGlobals->maxEntities && !FNullEnt(INDEXENT(index))) ? CBaseEntity::Instance(INDEXENT(index)) : nullptr;
		LsForEachMatching(CMD_ARGV(2), [&](ILsCreature *c) { c->SetTarget(pTarget); });
		return;
	}

	if (!Q_stricmp(cmd, "speed") || !Q_stricmp(cmd, "tier"))
	{
		// speed <ent|all|type> <0.2..3>: times its normal walk and run until it dies;
		// tier <ent|all|type> <0..3>
		if (CMD_ARGC() < 4)
		{
			CONSOLE_ECHO("usage: ls_creature %s <entity|all|type> <%s>\n", cmd, !Q_stricmp(cmd, "speed") ? "0.2..3" : "0..3");
			return;
		}
		bool speed = !Q_stricmp(cmd, "speed");
		float value = Q_atof(CMD_ARGV(3));
		LsForEachMatching(CMD_ARGV(2), [&](ILsCreature *c) {
			if (Q_strcmp(c->Provider(), "ls") || !c->Entity())
				return;
			CLsCreature *pCreature = static_cast<CLsCreature *>(c->Entity());
			if (speed)
				pCreature->SetSpeedScale(value);
			else
				pCreature->SetTier(int(value));
		});
		return;
	}

	if (!Q_stricmp(cmd, "behave") || !Q_stricmp(cmd, "behaviour") || !Q_stricmp(cmd, "behavior"))
	{
		// behave <ent|all|type> <idle|wander|patrol|guard|chase|flee|default> [x y z]
		if (!Q_stricmp(CMD_ARGV(3), "default"))
		{
			LsForEachMatching(CMD_ARGV(2), [&](ILsCreature *c) {
				if (!Q_strcmp(c->Provider(), "ls") && c->Entity())
					static_cast<CLsCreature *>(c->Entity())->Resume();
			});
			return;
		}
		int behaviour = LsNameIndex(CMD_ARGV(3), s_behaviourNames, LSB_COUNT, -1);
		if (behaviour < 0)
		{
			CONSOLE_ECHO("usage: ls_creature behave <entity|all|type> <idle|wander|patrol|guard|chase|flee> [x y z]\n");
			return;
		}

		bool hasSpot = CMD_ARGC() >= 7;
		Vector spot(Q_atof(CMD_ARGV(4)), Q_atof(CMD_ARGV(5)), Q_atof(CMD_ARGV(6)));
		if (hasSpot && !LsPosOk(spot))
		{
			CONSOLE_ECHO("ls_creature: that position is outside the world\n");
			return;
		}

		LsForEachMatching(CMD_ARGV(2), [&](ILsCreature *c) { c->SetBehaviour(behaviour, hasSpot ? &spot : nullptr); });
		return;
	}

	if (!Q_stricmp(cmd, "adopt"))
	{
		// adopt <classname> [type]: someone else's monsters (MonsterMod) join the list
		const char *classname = CMD_ARGV(2);
		if (!classname[0])
		{
			CONSOLE_ECHO("usage: ls_creature adopt <classname> [type]\n");
			return;
		}

		if (LsAdoptDenied(classname) || Q_strlen(classname) >= sizeof(s_adopt[0].classname))
		{
			CONSOLE_ECHO("ls_creature: '%s' can't be adopted (players, the world, items, brushes and our own are refused)\n", classname);
			return;
		}

		int r = 0;
		while (r < s_adoptCount && Q_stricmp(s_adopt[r].classname, classname))
			r++;

		if (r == s_adoptCount)
		{
			if (s_adoptCount >= int(ARRAYSIZE(s_adopt)))
			{
				CONSOLE_ECHO("ls_creature: too many adopted classnames\n");
				return;
			}
			s_adoptCount++;
		}

		Q_strlcpy(s_adopt[r].classname, classname);
		s_adopt[r].type = LsCreatureTypeIndex(CMD_ARGV(3));
		LsAdoptScan();
		return;
	}

	if (!Q_stricmp(cmd, "reload"))
	{
		LsLoadTypes();
		CONSOLE_ECHO("ls_creature: %d types (%s); new models and sounds wait for the next map\n", s_typeCount, ls_creature_file.string);
		return;
	}

	if (!Q_stricmp(cmd, "types"))
	{
		LsCreatureTypeIndex("");
		for (int i = 0; i < s_typeCount; i++)
		{
			const LsCreatureType *t = &s_types[i];
			CONSOLE_ECHO("%-12s %-10s hp %4.0f speed %3.0f %s %.0f dmg @ %.0f  %s  %s%s\n", t->name, s_categoryNames[t->category],
				t->health, t->speed, s_attackNames[t->attack], t->damage, t->attackRange, s_behaviourNames[t->behaviour],
				t->precached ? t->model : "(not precached)", t->precached && t->usingFallback ? " (stand-in)" : "");
		}
		return;
	}

	if (!Q_stricmp(cmd, "list") || !Q_stricmp(cmd, "status"))
	{
		int alive = 0;
		for (int i = 0; i < LsCreatureCount(); i++)
		{
			ILsCreature *c = LsCreatureAt(i);
			if (!c)
				continue;

			entvars_t *pevC = c->Vars();
			if (!pevC)
				continue;
			const LsCreatureType *t = c->Type();
			CBaseEntity *pTarget = c->GetTarget();
			if (c->IsAlive())
				alive++;

			CONSOLE_ECHO("#%-4d %-8s %-12s %s hp %4.0f  %s  at %.0f %.0f %.0f%s%s\n", ENTINDEX(ENT(pevC)), c->Provider(),
				t ? t->name : STRING(pevC->classname), c->IsAlive() ? "alive" : "dead ", c->Health(),
				s_behaviourNames[clamp(c->GetBehaviour(), 0, LSB_COUNT - 1)], pevC->origin.x, pevC->origin.y, pevC->origin.z,
				pTarget ? " -> " : "", pTarget ? STRING(pTarget->pev->netname) : "");
		}

		CONSOLE_ECHO("ls_creature: %d alive, %d listed; %d models + %d sounds precached\n", alive, s_listLive, s_precachedModels, s_precachedSounds);
		return;
	}

	CONSOLE_ECHO("ls_creature spawn <type> <x> <y> <z> [yaw] [key=value ...]\n"
		"ls_creature kill|remove <entity|all|type>\n"
		"ls_creature target <entity|all|type> <entity index|0>\n"
		"ls_creature behave <entity|all|type> <idle|wander|patrol|guard|chase|flee> [x y z]\n"
		"ls_creature adopt <classname> [type]\n"
		"ls_creature list | types | reload\n");
}
