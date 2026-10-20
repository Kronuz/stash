#!/usr/bin/env python3
"""Exhaustive interleaving model of stash's add, walk, clean (and reap).

Sequentially consistent: every atomic access is one step, so memory orders rest
on the seq_cst argument in the docs, not on this model. One wheel level of
finest slots (Div 1); clean frees whole groups of G keys entirely below
first_valid, as the outermost level does.

Properties, checked on every reachable state:
  UAF   a producer reserves or fills in a node clean freed
  FV    first_valid passes the filing key of an insert in flight
  LOSS  clean frees an unfired live value, or a live value is stranded below
        first_valid once every producer is done
  DUP   a value fires twice
  REAP  the reaper drops a live value
  HOLE  a reserved slot is left unfilled once every producer is done (it pins
        first_valid for good); with abort=1 a producer may fail to allocate
        before its claim (stash's no_memory). A failure after the claim
        terminates the process (stash_out_of_memory_after_claim), so it has no
        successor state; the abort_after_reserve mutant continues instead and
        must reach HOLE.

Usage: stash_model.py VARIANT MUTANT STYLE [key=value ...]
  VARIANT  main | window | reap
  STYLE    sched (begin_key = first_valid every call) | cont (once per walk)
  knobs    slots, producers, leads, tmax, period, group, chunk, wide, abort
  The window has a fixed width of `wide` finest slots (stash.h: WINDOW_CHUNK *
  WINDOW_WIDE); stash_model_adaptive.py keeps the dropped adaptive shrink.
"""

import sys
from collections import deque

IDLE = 10**9


def run(variant, mutant, style, slots=2, producers=2, leads=(0, 1), tmax=5,
        period=1, group=2, chunk=1, wide=1, cancel=False, abort=0, max_states=30_000_000):
    P = producers
    S = slots
    G = group
    C = chunk
    W0 = 0                      # initial window limit: everything far until the first move

    def parity(w):
        return w & 1             # the epoch parity is W's low bit, as in stash.h

    def next_window(floor, cur, width):
        # floor + width, kept above the current W and of the other parity.
        w = floor + width
        if w <= cur:
            w = cur + 1
        if parity(w) == parity(cur):
            w += 1
        return w

    # ---- state layout (a flat list; tuple for hashing) ----
    # global
    NOW, FV, LV, AFLOOR, IFLOOR, FVCAP, OVF, AW, WIN, PWIN, FPIN, WIDTH, OCC, WOCC = range(14)
    # walker
    WPH, WBEG, WEND, WPEND, WCUR, WSCAN, WCAP, WCNT, WFOUND = range(14, 23)
    BASE = 23
    BOARD = BASE                         # S slots
    FAR = BOARD + S                      # 2*S counters: FAR + par*S + stripe
    NODES = FAR + 2 * S                  # one per group: gen (0 = none), odd/even unused
    NGROUPS = (tmax + max(leads) + 2) // G + 2
    FREED = NODES + NGROUPS              # per group: highest freed gen
    LEAF = FREED + NGROUPS               # per key: tuple of entries
    NKEYS = NGROUPS * G
    PROD = LEAF + NKEYS                  # per producer: 8 fields
    PF = 9
    # producer fields: pc, key, w, fkey, slot, node, stripe, dead, fired
    PC, PKEY, PW, PFKEY, PSLOT, PNODE, PSTRIPE, PDEAD, PFIRED = range(PF)
    END = PROD + P * PF

    # walker phases
    W_IDLE, W_RF_FLOOR, W_RF_WIN, W_RF_SCAN, W_RF_OVF, W_VISIT = range(6)
    # producer pcs
    (P_START, P_A0, P_N0, P_N1, P_N2, P_F1, P_F2, P_F3, P_D, P_R, P_L, P_FI,
     P_REL, P_DONE) = range(14)
    P_F1B = 14

    init = [0] * END
    init[NOW] = 0
    init[FV] = 0
    init[LV] = 0
    init[AFLOOR] = 0
    init[IFLOOR] = 0
    init[FVCAP] = 0
    init[AW] = W0
    init[WIN] = W0
    init[PWIN] = W0
    init[FPIN] = 0
    init[WIDTH] = wide
    init[WPH] = W_IDLE
    init[WPEND] = IDLE
    for i in range(S):
        init[BOARD + i] = IDLE
    for k in range(NKEYS):
        init[LEAF + k] = ()
    for p in range(P):
        b = PROD + p * PF
        init[b + PC] = P_START
        init[b + PSLOT] = -1
        init[b + PNODE] = -1
        init[b + PSTRIPE] = p % S
    init = tuple(init)

    is_window = variant == "window"
    is_reap = variant == "reap"

    viol = {}

    def flag(name, st, why):
        if name not in viol:
            viol[name] = why

    # ---- producer step ----
    def producer_step(st, p):
        s = list(st)
        b = PROD + p * PF
        pc = s[b + PC]
        if pc == P_START:
            s[b + PKEY] = s[NOW] + leads[p % len(leads)]
            s[b + PC] = P_A0 if is_window else P_N0
        elif pc == P_A0:
            w = s[AW]
            s[b + PW] = w
            near = s[b + PKEY] < w
            if mutant == "all_far":
                near = False
            s[b + PC] = P_N0 if near else P_F1
        elif pc == P_N0:
            # relaxed floor read: announce at most the key it will file under
            fl = s[AFLOOR]
            k = s[b + PKEY]
            s[b + PW] = fl if k < fl else k      # PW reused as the announced key
            s[b + PC] = P_N1
        elif pc == P_N1:
            slot = -1
            if mutant != "no_announce":
                h = s[b + PSTRIPE]
                for j in range(S):
                    i = (h + j) % S
                    if s[BOARD + i] == IDLE:
                        slot = i
                        break
            if slot >= 0:
                s[BOARD + slot] = s[b + PW]
                s[b + PSLOT] = slot
            elif mutant != "no_announce":
                s[OVF] += 1
                s[b + PSLOT] = -2
            s[b + PC] = P_N2
        elif pc == P_N2:
            fl = s[AFLOOR]
            k = s[b + PKEY]
            if mutant == "no_floor_raise":
                s[b + PFKEY] = k
            else:
                s[b + PFKEY] = fl if k < fl else k
            s[b + PC] = P_D
        elif pc == P_F1 and mutant == "recheck_before_count":
            if s[AW] == s[b + PW]:
                s[b + PC] = P_F1B
            else:
                s[b + PC] = P_A0
        elif pc == P_F1B:
            s[FAR + parity(s[b + PW]) * S + s[b + PSTRIPE]] += 1
            s[b + PFKEY] = s[b + PKEY]
            s[b + PC] = P_D
        elif pc == P_F1:
            if mutant != "no_epoch":
                s[FAR + parity(s[b + PW]) * S + s[b + PSTRIPE]] += 1
            s[b + PC] = P_F2
        elif pc == P_F2:
            if mutant == "no_recheck" or mutant == "no_epoch" or s[AW] == s[b + PW]:
                s[b + PFKEY] = s[b + PKEY]
                s[b + PC] = P_D
            else:
                s[b + PC] = P_F3
        elif pc == P_F3:
            s[FAR + parity(s[b + PW]) * S + s[b + PSTRIPE]] -= 1
            s[b + PC] = P_A0
        elif pc == P_D:
            g = s[b + PFKEY] // G
            if s[NODES + g] == 0 or s[NODES + g] <= s[FREED + g]:
                # absent (never made, or freed): make a fresh node
                gen = max(s[NODES + g], s[FREED + g]) + 1
                s[NODES + g] = gen
                for k in range(g * G, g * G + G):
                    s[LEAF + k] = ()
            s[b + PNODE] = s[NODES + g]
            s[b + PC] = P_R
        elif pc == P_R:
            k = s[b + PFKEY]
            g = k // G
            if s[b + PNODE] <= s[FREED + g]:
                flag("UAF", st, f"p{p} reserves key {k} in freed node")
                return None
            s[LEAF + k] = s[LEAF + k] + ("S" + str(p),)
            s[b + PC] = P_L
        elif pc == P_L:
            k = s[b + PFKEY]
            if k > s[LV]:
                s[LV] = k
            s[b + PC] = P_FI
        elif pc == P_FI:
            k = s[b + PFKEY]
            g = k // G
            if s[b + PNODE] <= s[FREED + g]:
                flag("UAF", st, f"p{p} fills key {k} in freed node")
                return None
            ent = list(s[LEAF + k])
            ent[ent.index("S" + str(p))] = "V" + str(p)
            s[LEAF + k] = tuple(ent)
            s[b + PC] = P_REL
        elif pc == P_REL:
            sl = s[b + PSLOT]
            if sl >= 0:
                s[BOARD + sl] = IDLE
            elif sl == -2:
                s[OVF] -= 1
            elif is_window and mutant != "no_epoch":
                s[FAR + parity(s[b + PW]) * S + s[b + PSTRIPE]] -= 1
            s[b + PSLOT] = -1
            s[b + PC] = P_DONE
        else:
            return None
        return tuple(s)

    # ---- producer allocation failure (abort=1) ----
    # A failure before the claim (the levels above the leaf, the value box) comes
    # at the descent (P_D): add returns no_memory, InFlight's destructor releases
    # the announce (P_REL) and nothing is filed. A failure after the claim
    # terminates the process, so it is no state at all; the mutant instead
    # continues past it, as an unguarded failure would.
    def producer_abort(st, p):
        b = PROD + p * PF
        pc = st[b + PC]
        if mutant == "abort_after_reserve":
            if pc not in (P_L, P_FI):
                return None
        elif pc != P_D:
            return None
        s = list(st)
        s[b + PC] = P_REL
        return tuple(s)

    def in_flight_keys(s):
        for p in range(P):
            b = PROD + p * PF
            if P_D <= s[b + PC] <= P_REL:
                yield p, s[b + PFKEY]

    def set_fv(s, new):
        if new > s[FV]:
            s[FV] = new
            for p, k in in_flight_keys(s):
                if k < new:
                    flag("FV", s, f"first_valid {new} passed p{p}'s key {k}")
                    return False
        return True

    # ---- walker ----
    def begin_call(s):
        """Start a next() call: decide whether to refresh."""
        if style == "sched":
            s[WBEG] = s[FV]
            s[WPEND] = IDLE
        s[WFOUND] = 0
        s[WCUR] = s[WBEG]
        if mutant != "every_call" and s[WBEG] > s[FV]:
            s[WPH] = W_VISIT
            return
        floor = s[WEND] - 1
        advanced = floor > s[IFLOOR] and floor - s[IFLOOR] >= period
        pinned = s[FVCAP] < s[IFLOOR]
        if mutant == "no_pinned":
            pinned = False
        if not advanced and not pinned:
            s[WPH] = W_VISIT
            return
        s[WPH] = W_RF_FLOOR

    def walker_steps(st):
        s0 = st
        ph = st[WPH]
        out = []
        if ph == W_IDLE:
            if st[NOW] < tmax:
                s = list(st)
                s[NOW] += 1
                out.append(("tick", tuple(s)))
            if st[NOW] > 0:
                s = list(st)
                s[WBEG] = s[FV]
                s[WEND] = s[NOW]
                s[WPEND] = IDLE
                begin_call(s)
                out.append(("walk", tuple(s)))
            # clean: frees groups whose whole window is <= first_valid
            s = list(st)
            changed = False
            g = 0
            while (g + 1) * G <= s[FV]:
                if s[NODES + g] > s[FREED + g]:
                    for k in range(g * G, g * G + G):
                        for e in s[LEAF + k]:
                            if e[0] == "V":
                                q = int(e[1:])
                                if not s[PROD + q * PF + PDEAD]:
                                    flag("LOSS", st, f"clean frees live value of p{q} at key {k}")
                        s[LEAF + k] = ()
                    s[FREED + g] = s[NODES + g]
                    changed = True
                g += 1
            if changed:
                out.append(("clean", tuple(s)))
            if is_reap:
                # one reap pass from first_valid to last_valid: drop dead values only
                s = list(st)
                changed = False
                for k in range(s[FV], min(s[LV], NKEYS - 1) + 1):
                    g = k // G
                    if s[NODES + g] == 0 or s[NODES + g] <= s[FREED + g]:
                        continue
                    ent = list(s[LEAF + k])
                    for i, e in enumerate(ent):
                        if e[0] == "V":
                            q = int(e[1:])
                            dead = s[PROD + q * PF + PDEAD]
                            if dead or mutant == "reap_no_dead_test":
                                if not dead:
                                    flag("REAP", st, f"reaper dropped p{q}'s live value")
                                ent[i] = "N"
                                changed = True
                    s[LEAF + k] = tuple(ent)
                    if mutant == "reap_frees_node" and all(e == "N" for e in ent) and ent:
                        if all(all(x == "N" for x in s[LEAF + kk]) for kk in range(g * G, g * G + G)):
                            s[FREED + g] = s[NODES + g]
                            changed = True
                if mutant == "reap_moves_fv" and changed:
                    if not set_fv(s, s[LV]):
                        pass
                if changed:
                    out.append(("reap", tuple(s)))
            return out

        s = list(st)
        if ph == W_RF_FLOOR:
            floor = s[WEND] - 1
            if floor > s[IFLOOR]:
                s[IFLOOR] = floor
                s[AFLOOR] = floor
            s[WPH] = W_RF_WIN if is_window else W_RF_SCAN
            s[WSCAN] = 0
            s[WCAP] = s[IFLOOR]
            s[WCNT] = 0
        elif ph == W_RF_WIN:
            floor = s[IFLOOR]
            gate = not s[FPIN] or mutant == "no_advance_gate"
            if gate and floor + C > s[WIN]:
                # a move a chunk before the floor reaches W, at the fixed width
                s[PWIN] = s[WIN]
                s[WIN] = next_window(floor, s[WIN], wide)
                if mutant != "late_window_publish":
                    s[AW] = s[WIN]
            s[WPH] = W_RF_SCAN
            s[WSCAN] = 0
        elif ph == W_RF_SCAN:
            i = s[WSCAN]
            # 2*S steps when windowed: board key, then that line's far count
            if is_window:
                line, which = divmod(i, 2)
                if which == 0:
                    k = s[BOARD + line]
                    if k < s[WCAP]:
                        s[WCAP] = k
                else:
                    s[WCNT] += s[FAR + parity(s[PWIN]) * S + line]
                n = 2 * S
            else:
                k = s[BOARD + i]
                if k < s[WCAP]:
                    s[WCAP] = k
                n = S
            s[WSCAN] = i + 1
            if i + 1 == n:
                s[WPH] = W_RF_OVF
        elif ph == W_RF_OVF:
            if mutant == "late_window_publish":
                s[AW] = s[WIN]
            cap = s[WCAP]
            if s[OVF] and mutant != "no_overflow_cap":
                if s[FV] < cap:
                    cap = s[FV]
            if is_window:
                if mutant != "no_window_cap" and s[WIN] < cap:
                    cap = s[WIN]
                s[FPIN] = 1 if s[WCNT] else 0
                if s[FPIN] and mutant != "no_prev_hold" and s[PWIN] < cap:
                    cap = s[PWIN]
            s[FVCAP] = cap
            s[WPH] = W_VISIT
        elif ph == W_VISIT:
            k = s[WCUR]
            stop = k >= s[WEND] or k > s[LV]
            if not stop:
                g = k // G
                if s[NODES + g] and s[NODES + g] > s[FREED + g]:
                    ent = list(s[LEAF + k])
                    pend = False
                    for i, e in enumerate(ent):
                        if e[0] == "S":
                            pend = True
                        elif e[0] == "V":
                            q = int(e[1:])
                            ent[i] = "N"
                            qb = PROD + q * PF
                            if not s[qb + PDEAD]:
                                s[qb + PFIRED] += 1
                                if s[qb + PFIRED] > 1:
                                    flag("DUP", st, f"p{q} fired twice")
                                s[WFOUND] = 1
                                break
                    s[LEAF + k] = tuple(ent)
                    if pend and k < s[WPEND]:
                        s[WPEND] = k
                if s[WFOUND]:
                    # found: ret_next with begin_key at this slot
                    s[WBEG] = k
                    new = k - 1 if k > 0 else 0
                    if s[WPEND] != IDLE and s[WPEND] < new:
                        new = s[WPEND]
                    if mutant != "no_cap" and s[FVCAP] < new:
                        new = s[FVCAP]
                    set_fv(s, new)
                    begin_call(s)        # the consumer calls next() again
                    return [("visit", tuple(s))]
                s[WCUR] = k + 1
                return [("visit", tuple(s))]
            # not found: ret_next advances begin_key to the window end
            s[WBEG] = s[WEND]
            new = s[WEND] - 1
            if s[WPEND] != IDLE and s[WPEND] < new:
                new = s[WPEND]
            if mutant != "no_cap" and s[FVCAP] < new:
                new = s[FVCAP]
            set_fv(s, new)
            s[WPH] = W_IDLE
        return [("w", tuple(s))]

    def terminal_check(st):
        if any(st[PROD + p * PF + PC] != P_DONE for p in range(P)):
            return
        for k in range(NKEYS):
            g = k // G
            if st[NODES + g] and st[NODES + g] > st[FREED + g] and any(e[0] == "S" for e in st[LEAF + k]):
                flag("HOLE", st, f"reserved slot at key {k} left unfilled with every producer done")
        if st[WPH] != W_IDLE:
            return
        for k in range(NKEYS):
            g = k // G
            if not st[NODES + g] or st[NODES + g] <= st[FREED + g]:
                continue
            for e in st[LEAF + k]:
                if e[0] == "V":
                    q = int(e[1:])
                    if not st[PROD + q * PF + PDEAD] and k < st[FV]:
                        flag("LOSS", st, f"p{q}'s value at key {k} stranded below first_valid {st[FV]}")

    # 64-bit hash compaction: about n^2 / 2^65 odds of merging two states
    seen = {hash(init)}
    stack = [init]
    stats = {"far_moves_max": 0, "far_passed_max": 0, "race_inc_during_scan": 0,
             "stray_two_moves": 0, "stray_with_legit_same_parity": 0}
    while stack:
        st = stack.pop()
        terminal_check(st)
        if viol:
            break
        succ = []
        for p in range(P):
            n = producer_step(st, p)
            if n is not None:
                succ.append(n)
            if abort:
                n = producer_abort(st, p)
                if n is not None:
                    succ.append(n)
            if cancel:
                b = PROD + p * PF
                if st[b + PC] != P_START and not st[b + PDEAD]:
                    s = list(st)
                    s[b + PDEAD] = 1
                    succ.append(tuple(s))
        if viol:
            break
        for _, n in walker_steps(st):
            succ.append(n)
        if viol:
            break
        for n in succ:
            if is_window:
                # coverage: a far producer in flight while the window moved past its key
                for p in range(P):
                    b = PROD + p * PF
                    if P_F1 <= n[b + PC] <= P_REL and n[b + PSLOT] == -1 and n[b + PC] not in (P_N0, P_N1, P_N2):
                        lag = (n[WIN] - n[b + PW]) // C
                        if lag > stats["far_moves_max"]:
                            stats["far_moves_max"] = lag
                        passed = n[AFLOOR] - n[b + PKEY]
                        if passed > stats["far_passed_max"]:
                            stats["far_passed_max"] = passed
            if is_window:
                for p in range(P):
                    b = PROD + p * PF
                    # epoch read before a move, count raised while the post-move scan runs
                    if n[b + PC] == P_F2 and n[b + PW] != n[AW] and n[WPH] == W_RF_SCAN:
                        stats["race_inc_during_scan"] += 1
                    if n[b + PC] == P_F3 and parity(n[b + PW]) == parity(n[AW]):
                        stats["stray_two_moves"] += 1
                        for q in range(P):
                            c = PROD + q * PF
                            if q != p and P_D <= n[c + PC] <= P_REL and n[c + PSLOT] == -1 and parity(n[c + PW]) == parity(n[b + PW]) and n[c + PW] == n[AW]:
                                stats["stray_with_legit_same_parity"] += 1
            h = hash(n)
            if h not in seen:
                seen.add(h)
                stack.append(n)
                if len(seen) > max_states:
                    return "TRUNCATED", len(seen), stats
    if viol:
        return ", ".join(f"{k}: {v}" for k, v in viol.items()), len(seen), stats
    return "no violation", len(seen), stats


def main():
    a = sys.argv[1:]
    variant, mutant, style = a[0], a[1], a[2]
    kw = {}
    for x in a[3:]:
        k, v = x.split("=")
        if k == "leads":
            kw[k] = tuple(int(y) for y in v.split(","))
        elif k == "cancel":
            kw[k] = v == "1"
        else:
            kw[k] = int(v)
    res, n, stats = run(variant, mutant, style, **kw)
    extra = (f" window_moves_past_far={stats['far_moves_max']} floor_past_far_key={stats['far_passed_max']}"
             f" race_inc_during_scan={stats['race_inc_during_scan']} stray_two_moves={stats['stray_two_moves']}"
             f" stray_with_legit_same_parity={stats['stray_with_legit_same_parity']}") if variant == "window" else ""
    print(f"{variant:6} {mutant:18} {style:5} {' '.join(a[3:]):40} {n:>10} states  {res}{extra}")


if __name__ == "__main__":
    main()
