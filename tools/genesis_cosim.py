#!/usr/bin/env python3
"""Genesis differential co-simulation coordinator.

Launches two deterministic instances of the genesis-cosim build, advances them
in lockstep on a shared checkpoint clock, compares their full-state chain hash
at each checkpoint, and HALTS at the first divergence — naming which subsystem
sub-hash split first (the decision procedure; see
recomp-template/DIFFERENTIAL-COSIMULATION.md).

Backends (env-selected in the same exe):
  recomp : the recompiled 68K drives the game (default)
  interp : GENESIS_FORCE_INTERP=1 — m68k_interp drives the whole program
           (pairing #1 B-side; requires the FORCE_INTERP build step)

Validation gates:
  --a recomp --b recomp        Gate 1  (determinism: must be 0 divergence)
  --a interp --b interp        Gate 2  (interp determinism)
  --a recomp --b recomp --inject-at K --inject reg:0:1
                               Gate 3  (must halt at ~K, naming cpu68k)
  --a recomp --b interp        pairing #1: the real first-divergence hunt

Usage:
  python genesis_cosim.py --a recomp --b recomp --stride 1 --max 2000
"""
import argparse, json, os, socket, subprocess, sys, time

SUBS = ["cpu68k","timing","ram","z80","z80ram","handshake","vdp","fm","psg","evq"]

# Per-game harness config. The runner/engine cosim is game-agnostic (WaitForVBla
# PC via GENESIS_COSIM_WAITVBL_PC; regions via g_game_spec/g_game_layout; the
# clown-measured cost table is per-game generated). Only the TOOLING needs to
# know each game's build-worktree dir, exe base name, and WaitForVBla entry PC.
# Legacy presets remain for unmigrated consumers. New consumers supply their
# own metadata with --game-config; no game addresses belong in shared tooling.
# Builds require the _cosim/_oracle_cosim targets (see COSIM.md).
GAMES = {
    "s1": {"wt": "_wt-cosim-s1", "exe": "SonicTheHedgehogRecomp",  "waitvbl": "29a8", "rom": "sonic.bin"},
    "puyo": {"wt": "_wt-puyo", "exe": "PuyoRecomp", "waitvbl": "32c", "rom": "puyo.bin"},
}
GAME = "s1"   # module-level selection; set by --game (or divergence_report)

def select_game(game, config=None):
    """Load caller-owned metadata without launching or altering the runtime."""
    global GAME
    if config:
        with open(config, encoding="utf-8") as source:
            metadata = json.load(source)
        if not isinstance(metadata, dict) or any(
                not isinstance(metadata.get(key), str) or not metadata[key].strip()
                for key in ("wt", "exe", "waitvbl", "rom")):
            raise ValueError("game config requires nonempty wt, exe, waitvbl and rom strings")
        if not 0 <= int(metadata["waitvbl"], 16) <= 0xffffff:
            raise ValueError("waitvbl must be a 24-bit hex address")
        GAMES[game] = metadata
    if game not in GAMES:
        raise ValueError(f"unknown game {game!r}; supply its --game-config JSON")
    GAME = game

def game_waitvbl():
    return GAMES[GAME]["waitvbl"]


def _find(name, explicit=None):
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    c = os.path.abspath(os.path.join(here, "..", "..", GAMES[GAME]["wt"],
                                     "build-cosim", "Release", name))
    if os.path.exists(c):
        return c
    sys.exit(f"{name} not found; build it or pass --exe/--exe-oracle "
             f"(game={GAME}, worktree={GAMES[GAME]['wt']})")

def find_exe(explicit):        # own-backend cosim (recomp + interp backends)
    return _find(f"{GAMES[GAME]['exe']}_cosim.exe", explicit)

def find_oracle_exe(explicit): # clownmdemu oracle cosim (pairing #2 B-side)
    return _find(f"{GAMES[GAME]['exe']}_oracle_cosim.exe", explicit)


class Inst:
    """One cosim process + its control socket."""
    def __init__(self, name, exe, port, backend, stride, clock, extra_env=None,
                 waitvbl_pc="", visible=False, extra_args=()):
        self.name = name
        exe_dir = os.path.dirname(exe)
        rom = os.path.join(exe_dir, GAMES[GAME]["rom"])   # per-game read-only ROM
        # ISOLATED per-instance cwd: settings.ini / saves / logs must not be
        # shared between the two instances (a shared build dir races boot-time
        # state -> nondeterminism + crashes). See project memory "rule out
        # build-dir state". SDL2.dll still resolves from the exe's own dir.
        cwd = os.path.join(exe_dir, f"cosim_inst_{name}")
        os.makedirs(cwd, exist_ok=True)
        env = dict(os.environ)
        env["GENESIS_COSIM_PORT"]   = str(port)
        env["GENESIS_COSIM_STRIDE"] = str(stride)
        env["GENESIS_COSIM_CLOCK"]  = clock
        # No host audio SINK => deterministic (proposal Gate 1 requirement): the
        # real audio device runs a host-timing DRC thread. Dummy audio keeps the
        # chips advancing (main thread) but drops the real-rate consumer. Video
        # stays real — SDL's dummy video has no renderer and the runner needs one.
        env["SDL_AUDIODRIVER"] = "dummy"
        if backend == "interp":
            env["GENESIS_FORCE_INTERP"] = "1"
        # WaitForVBla entry PC: the interp yields there like the recomp stub, and
        # the oracle's cosim_cycles hook samples work cycles at that PC. Harmless
        # for the own recomp backend (it captures at glue_yield_for_vblank).
        if waitvbl_pc:
            env["GENESIS_COSIM_WAITVBL_PC"] = waitvbl_pc
        # backend == "oracle" is the clownmdemu exe (auto-visible internally).
        # Pairing #2: own-backend side must also hash the visible surface.
        if visible:
            env["GENESIS_COSIM_VISIBLE"] = "1"
        if extra_env:
            env.update(extra_env)
        self.log = open(os.path.join(cwd, f"cosim_{name}.log"), "w")
        self.proc = subprocess.Popen(
            [exe, rom, "--no-launcher", "--turbo", *extra_args],
            cwd=cwd, env=env, stdout=self.log, stderr=subprocess.STDOUT)
        self.sock = self._connect(port)
        self.buf = b""

    def _connect(self, port, timeout=30):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=2)
                s.settimeout(30)
                return s
            except OSError:
                if self.proc.poll() is not None:
                    sys.exit(f"[{self.name}] process died before the server came up "
                             f"(see cosim_{self.name}.log)")
                time.sleep(0.2)
        sys.exit(f"[{self.name}] could not connect on port {port}")

    def _line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError(f"[{self.name}] connection closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode().strip()

    def cmd(self, c):
        self.sock.sendall((c + "\n").encode())
        return self._line()

    def window(self, n):
        self.sock.sendall((f"window {n}\n").encode())
        rows = []
        while True:
            ln = self._line()
            if ln == "end":
                break
            rows.append(ln)
        return rows

    def cyclefields(self):
        """Cross-backend work-cycle ruler: {'work': <str>, 'cum': <str>}."""
        return self.kv(self.cmd("cyclefields"))

    def kv(self, resp):
        """Parse 'k v k v ...' tolerating leading status words (e.g. 'parked')
        by pairing EVERY adjacent token, first-wins. A step-2 parse misaligns on
        'parked cp N ...' and silently yields None for 'chain' — the classic
        silently-blind coordinator (None==None passes Gate 1 catching nothing)."""
        t = resp.split()
        d = {}
        for i in range(len(t) - 1):
            d.setdefault(t[i], t[i+1])
        return d

    def exited_cleanly(self, timeout=30):
        """After the connection closed: did the process end with exit code 0
        (an input script's EXIT 0)?"""
        try:
            return self.proc.wait(timeout=timeout) == 0
        except subprocess.TimeoutExpired:
            return False

    def close(self):
        try: self.sock.close()
        except Exception: pass
        try: self.proc.terminate()
        except Exception: pass
        self.log.close()


def step_or_none(inst):
    """One checkpoint step; None if the instance has closed (an input
    script's EXIT, or a crash)."""
    try:
        return inst.kv(inst.cmd("step 1"))
    except (EOFError, ConnectionError):
        return None


def report_divergence(a, b, cp):
    print(f"\n*** FIRST DIVERGENCE at checkpoint cp={cp} ***")
    sa, sb = a.kv(a.cmd("sub")), b.kv(b.cmd("sub"))
    print("  subsystem            A                    B                    split")
    for k in SUBS:
        va, vb = sa.get(k, "?"), sb.get(k, "?")
        mark = "  <-- SPLIT" if va != vb else ""
        print(f"  {k:<10} {va:>18}  {vb:>18}{mark}")
    for k, fields in (("timing", "timingfields"), ("vdp", "vdpfields")):
        if sa.get(k) != sb.get(k):
            ra = a.cmd(fields)
            fa, fb = a.kv(ra), b.kv(b.cmd(fields))
            print(f"\n  --- {fields} diff (A vs B) ---")
            for f in ra.split()[0::2]:
                if fa.get(f) != fb.get(f):
                    print(f"    {f:<14} {fa.get(f)}  vs  {fb.get(f)}")
    ca, cb = a.kv(a.cmd("cpu")), b.kv(b.cmd("cpu"))
    print("\n  --- 68K register diff (A vs B) ---")
    for k in ["pc","sr","usp"] + [f"d{i}" for i in range(8)] + [f"a{i}" for i in range(8)]:
        if ca.get(k) != cb.get(k):
            print(f"    {k:<4} {ca.get(k)}  vs  {cb.get(k)}")
    print("\n  --- A window (last checkpoints) ---")
    for r in a.window(60): print("   A", r)
    print("  --- B window ---")
    for r in b.window(60): print("   B", r)


def run_profile(a, b, args, pairing2):
    """Holistic divergence profile: step the whole deterministic demo without
    halting, tallying per-subsystem agreement at every checkpoint. Reports, per
    subsystem, the FIRST divergence checkpoint and the match/diverge counts over
    the run — a cross-the-board picture of how faithful the machine is, rather
    than only the first split. (First-divergence is still reported per subsystem;
    it is just not a stop condition here.)"""
    # Pairing #2 (visible surface) only populates a subset; the rest are 0 on
    # BOTH sides and would show a meaningless 100% match. Name the live set.
    VISIBLE = ["cpu68k", "ram", "z80", "z80ram", "vdp", "evq"]
    live = VISIBLE if pairing2 else SUBS
    stat = {k: {"first": None, "match": 0, "diff": 0} for k in SUBS}
    chain_first = None; chain_match = 0; chain_diff = 0
    cp = 0
    t0 = time.time()
    while cp < args.max:
        ra = a.kv(a.cmd("step 1"))
        rb = b.kv(b.cmd("step 1"))
        cp += 1
        sa, sb = a.kv(a.cmd("sub")), b.kv(b.cmd("sub"))
        assert all(k in sa and k in sb for k in SUBS), f"sub parse failed A={sa} B={sb}"
        for k in SUBS:
            if sa[k] == sb[k]:
                stat[k]["match"] += 1
            else:
                stat[k]["diff"] += 1
                if stat[k]["first"] is None:
                    stat[k]["first"] = cp
        ca, cb = ra.get("chain"), rb.get("chain")
        assert ca and cb, f"chain failed to parse (A={ra} B={rb})"
        if ca == cb:
            chain_match += 1
        else:
            chain_diff += 1
            if chain_first is None:
                chain_first = cp
        if cp % 100 == 0:
            rate = cp / max(1e-6, time.time() - t0)
            print(f"  cp={cp}/{args.max}  chain_first_div={chain_first}  ({rate:.1f} cp/s)")
    # ---- report ----
    clk = "frame" if args.clock == "frame" else "insn"
    print(f"\n===== HOLISTIC DIVERGENCE PROFILE ({args.a} vs {args.b}, "
          f"{cp} checkpoints on the {clk} clock) =====")
    if pairing2:
        print("  [pairing #2: visible surface — timing/handshake/fm/psg are not hashed "
              "cross-backend; ignore their rows]")
    print(f"  {'subsystem':<10} {'first_div_cp':>12} {'matched':>9} {'diverged':>9} {'match%':>8}")
    for k in SUBS:
        s = stat[k]
        tot = s["match"] + s["diff"]
        pct = 100.0 * s["match"] / tot if tot else 0.0
        live_mark = "" if k in live else "  (not hashed)"
        first = s["first"] if s["first"] is not None else "-- clean --"
        print(f"  {k:<10} {str(first):>12} {s['match']:>9} {s['diff']:>9} {pct:>7.2f}%{live_mark}")
    tot = chain_match + chain_diff
    pct = 100.0 * chain_match / tot if tot else 0.0
    print(f"  {'FULL-CHAIN':<10} {str(chain_first) if chain_first is not None else '-- clean --':>12} "
          f"{chain_match:>9} {chain_diff:>9} {pct:>7.2f}%")
    # earliest live-subsystem divergence = the holistic "first crack"
    live_firsts = [(stat[k]['first'], k) for k in live if stat[k]['first'] is not None]
    if live_firsts:
        fcp, fk = min(live_firsts)
        print(f"\n  earliest LIVE-subsystem divergence: '{fk}' at cp={fcp}"
              + (f" (~{fcp/59.94:.2f}s)" if args.clock == 'frame' else ""))
        # a field-level look at the first cracking subsystem, at the end state
        if fk == "cpu68k":
            ca, cb = a.kv(a.cmd("cpu")), b.kv(b.cmd("cpu"))
            diffs = [k for k in (["pc","sr","usp"]+[f"d{i}" for i in range(8)]+[f"a{i}" for i in range(8)])
                     if ca.get(k) != cb.get(k)]
            print(f"  cpu68k end-state differing fields: {diffs or 'none now (transient)'}")
    else:
        print(f"\n  NO live-subsystem divergence in {cp} checkpoints — bit-faithful across the demo.")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default="s1", help="game identity (default s1)")
    ap.add_argument("--game-config", help="caller-owned JSON: wt, exe, waitvbl (hex), rom")
    ap.add_argument("--exe")
    ap.add_argument("--exe-oracle")
    ap.add_argument("--a", default="recomp", choices=["recomp","interp","oracle"])
    ap.add_argument("--b", default="recomp", choices=["recomp","interp","oracle"])
    ap.add_argument("--porta", type=int, default=4600)
    ap.add_argument("--portb", type=int, default=4601)
    ap.add_argument("--stride", type=int, default=1)
    ap.add_argument("--clock", default="frame", choices=["frame","insn","cycle"],
                    help="frame=master_cycle (VDP raster; the only cross-backend-vs-oracle "
                         "ruler today). insn/cycle=g_cosim_cycle, the monotonic per-instruction "
                         "68K CYCLE-COST axis both own-backend backends bump identically "
                         "(psxrecomp-style guest cycle counter). 'cycle' is an alias for 'insn'.")
    ap.add_argument("--max", type=int, default=2000, help="max checkpoints to compare")
    ap.add_argument("--input-script",
                    help="replay this runner input script on both instances; a scripted "
                         "EXIT 0 on both, at the same checkpoint, ends the run as a pass")
    ap.add_argument("--start-cycle", type=int, default=0,
                    help="cycle clock: free-run to this cycle before the first checkpoint")
    ap.add_argument("--inject-at", type=int, default=0, help="apply injection at cp K (gate 3)")
    ap.add_argument("--inject", default="", help="reg:IDX:XOR or ram:OFF:XOR")
    ap.add_argument("--waitvbl-pc", default="",
                    help="hex PC of the game's WaitForVBla stub (default: game metadata). "
                         "The interp yields there like the recomp, and the "
                         "oracle samples work cycles there, so the backends stay program-aligned.")
    ap.add_argument("--subs", default="",
                    help="comma list of sub-hashes to compare instead of the full chain. "
                         "For pairing #1 use the cross-backend-comparable set "
                         "(ram,z80ram,handshake,vdp,fm,psg,evq) so the benign idle-wait "
                         "currency split (cpu68k,timing,z80) does not mask the audio divergence.")
    ap.add_argument("--profile", action="store_true",
                    help="HOLISTIC mode: do NOT halt at first divergence. Run the full "
                         "(deterministic) demo to --max checkpoints and report, per subsystem, "
                         "the first-divergence checkpoint + how many checkpoints matched vs "
                         "diverged. Answers 'across all systems, how divergent is this demo'.")
    args = ap.parse_args()
    try:
        select_game(args.game, args.game_config)
    except (OSError, ValueError) as error:
        ap.error(str(error))
    if not args.waitvbl_pc:
        args.waitvbl_pc = game_waitvbl()

    # Route each side to the right exe. Pairing #2 (any side == oracle) forces
    # the own-backend side into visible-surface mode so the hashes are comparable.
    own_exe = None; oracle_exe = None
    def exe_for(backend):
        nonlocal own_exe, oracle_exe
        if backend == "oracle":
            oracle_exe = oracle_exe or find_oracle_exe(args.exe_oracle)
            return oracle_exe
        own_exe = own_exe or find_exe(args.exe)
        return own_exe
    pairing2 = "oracle" in (args.a, args.b)
    vis_a = pairing2 and args.a != "oracle"
    vis_b = pairing2 and args.b != "oracle"
    print(f"A={args.a}@{args.porta}  B={args.b}@{args.portb}  "
          f"clock={args.clock} stride={args.stride} max={args.max}"
          + ("  [pairing #2: visible surface]" if pairing2 else ""))

    start_env = ({"GENESIS_COSIM_START_CYCLE": str(args.start_cycle)}
                 if args.start_cycle else None)
    script_args = (("--input-script", os.path.abspath(args.input_script))
                   if args.input_script else ())
    a = Inst("a", exe_for(args.a), args.porta, args.a, args.stride, args.clock,
             extra_env=start_env, waitvbl_pc=args.waitvbl_pc, visible=vis_a,
             extra_args=script_args)
    b = Inst("b", exe_for(args.b), args.portb, args.b, args.stride, args.clock,
             extra_env=start_env, waitvbl_pc=args.waitvbl_pc, visible=vis_b,
             extra_args=script_args)

    def do_inject(inst):
        kind, idx, xor = args.inject.split(":")
        inst.cmd(f"inject {kind} {idx} {xor}")

    subs = [s.strip() for s in args.subs.split(",") if s.strip()]
    if subs:
        print(f"comparing SUBSET of sub-hashes: {subs} (not the full chain)")

    if args.profile:
        try:
            return run_profile(a, b, args, pairing2)
        finally:
            a.close(); b.close()

    try:
        cp = 0
        while cp < args.max:
            if args.inject_at and cp == args.inject_at and args.inject:
                do_inject(a)  # perturb A only
                print(f"[gate3] injected {args.inject} into A at cp={cp}")
            ra, rb = step_or_none(a), step_or_none(b)
            if ra is None or rb is None:
                # An instance closed: with an input script, its EXIT. Both
                # must end at the same checkpoint, cleanly.
                if ra is None and rb is None and a.exited_cleanly() and b.exited_cleanly():
                    print(f"\nOK — script ended after {cp} checkpoints, ZERO divergence.")
                    return 0
                ended = [i.name for i, r in ((a, ra), (b, rb)) if r is None]
                print(f"\n*** instance(s) {','.join(ended).upper()} ended at cp={cp + 1}, "
                      f"the other did not (or exit code != 0) ***")
                return 1
            cp += 1
            if subs:
                # Compare a chosen subset of sub-hashes (pairing #1: the cross-
                # backend-comparable audio surface, excluding idle-wait currency).
                sa, sb = a.kv(a.cmd("sub")), b.kv(b.cmd("sub"))
                assert all(k in sa and k in sb for k in subs), f"sub parse failed A={sa} B={sb}"
                split = [k for k in subs if sa[k] != sb[k]]
                if split:
                    print(f"\n*** FIRST {subs}-DIVERGENCE at cp={ra.get('cp')}: split={split} ***")
                    report_divergence(a, b, ra.get("cp"))
                    return 1
            else:
                ca, cb = ra.get("chain"), rb.get("chain")
                # Guard against a silently-blind coordinator: a parse bug that
                # yields None for both would make every compare equal.
                assert ca and cb, f"chain failed to parse (A={ra} B={rb})"
                if ca != cb:
                    report_divergence(a, b, ra.get("cp"))
                    return 1
            if cp % 200 == 0:
                print(f"  cp={cp} (in agreement)")
        print(f"\nOK — {cp} checkpoints, ZERO divergence.")
        return 0
    finally:
        a.close(); b.close()


if __name__ == "__main__":
    sys.exit(main())
