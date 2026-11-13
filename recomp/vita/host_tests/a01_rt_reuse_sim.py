#!/usr/bin/env python3
# Copied verbatim (below this comment) from
#   the internal FABLE A0-1 notes (a01_rt_reuse_sim.py)
# the host model written for FABLE-A0-1-STALL.md (FABLE A0-1, 2026-11-11).
# recomp/vita/test_fbo_deferred_attach.py imports VitaGL, dry and wet from it
# to replay the S1 and S4 sequences with the ISAAC_VITA_FBO_DEFERRED_ATTACH
# rule.  Do not edit the model here; it is evidence, not a fixture to tune.
"""FABLE A0-1: host model of vitaGL's FBO render-target lifecycle with the Isaac RT reuse.

Usage: python -I a01_rt_reuse_sim.py

The reuse policy below is a line-by-line Python port of
  recomp/vita/vitagl-stock-reference/isaac_fbo_rt_reuse.c (lane r60-stall-20261026)
with HAVE_ISAAC_FBO_RT_REUSE_LEASE=1 and HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE=1 (both are required by the
wait-probe build, CMakeLists.txt:2572-2574) and without the profile counters.  The vitaGL side models
only what the ph120.wr / wb / wg counters see (patch 0036 + 0021/0024/0033):
  attach(w,h)  = _glFramebufferTexture2D COLOR_ATTACHMENT0 (shared.h)
  draw()       = scene_reset on the FBO: depth create when missing, RT create when fb->target == NULL
  swap()       = vglSwapBuffers -> inline garbage_collector (purge bucket + isaacFboRtCollect)
It is a model (INFERRED), not the device: no failures, no OOM path, one FBO.
"""
A = (480, 272)
B = (784, 472)


def observed_size(w, h):
    return (w == 480 and h == 272) or (w == 784 and h == 472)


def interlude_size(w, h):
    return (w == 64 and h == 56) or (w == 232 and h == 208)


RETIRES_CURRENT = 2


class Reuse:
    def __init__(self):
        self.owner = 0
        self.active = [0, 0, 0]  # target, w, h
        self.spare = [0, 0, 0]
        self.spare_bucket = 0
        self.spare_reusable = 0
        self.interlude = 0
        self.lease_started = 0
        self.lease_last_bucket = 0
        self.recovery_depth = 0
        self.recovery_poison = 0

    def invalidate_owner(self, owner):
        if owner and self.owner == owner:
            self.owner = 0
            self.active[0] = 0
            self.spare_reusable = 0
            self.interlude = 0

    def created(self, owner, target, w, h, compatible):
        if self.recovery_depth or self.recovery_poison:
            self.invalidate_owner(owner)
            return
        if self.interlude and owner == self.owner:
            if (target and compatible and not self.active[0] and self.active[1] == w and self.active[2] == h
                    and interlude_size(w, h)):
                self.active[0] = target
                return
            self.invalidate_owner(owner)
            return
        if not owner or not target or not compatible or not observed_size(w, h):
            self.invalidate_owner(owner)
            return
        if self.owner and self.owner != owner:
            return
        if not self.owner and self.spare[0]:
            return
        self.owner = owner
        self.active = [target, w, h]

    def switch(self, owner, current, old_w, old_h, new_w, new_h, compatible, purge_bucket):
        """returns (result, next)"""
        if self.recovery_depth or self.recovery_poison:
            compatible = 0
        if (owner and current and compatible and 0 <= purge_bucket < 4 and self.owner == owner
                and self.active[0] == current and self.active[1] == old_w and self.active[2] == old_h
                and (old_w != new_w or old_h != new_h) and self.spare[0] and self.spare[0] != current
                and self.spare_reusable and observed_size(self.spare[1], self.spare[2])
                and not self.lease_started):
            if self.interlude and self.spare[1] == new_w and self.spare[2] == new_h:
                self.active = list(self.spare)
                self.spare[0] = 0
                self.spare_reusable = 0
                self.interlude = 0
                return RETIRES_CURRENT, self.active[0]
            if interlude_size(new_w, new_h) and (self.interlude or observed_size(old_w, old_h)):
                self.active = [0, new_w, new_h]
                self.interlude = 1
                return RETIRES_CURRENT, 0
        if (not owner or not current or not compatible or purge_bucket < 0 or self.owner != owner
                or self.active[0] != current or self.active[1] != old_w or self.active[2] != old_h
                or not observed_size(old_w, old_h) or not observed_size(new_w, new_h)
                or (old_w == new_w and old_h == new_h)):
            self.invalidate_owner(owner)
            return 0, 0
        replacement = [0, 0, 0]
        if self.spare[0]:
            if (not self.spare_reusable or self.spare[1] != new_w or self.spare[2] != new_h
                    or self.spare[0] == current):
                self.invalidate_owner(owner)
                return 0, 0
            replacement = list(self.spare)
        previous = list(self.active)
        self.active = replacement
        self.spare = previous
        self.spare_bucket = purge_bucket
        self.spare_reusable = 1
        self.lease_started = 0
        return 1, replacement[0]

    def collect(self, purge_bucket):
        if not self.spare[0]:
            return 0
        expire = False
        if self.interlude:
            if self.spare_bucket != purge_bucket:
                return 0
            expire = True
        if not expire:
            if self.lease_started:
                if (self.spare_reusable and self.owner and self.active[0] and self.active[0] != self.spare[0]
                        and not self.recovery_depth and not self.recovery_poison):
                    if purge_bucket == self.lease_last_bucket:
                        return 0
                    if purge_bucket == ((self.lease_last_bucket + 1) & 3):
                        self.lease_last_bucket = purge_bucket
                        if purge_bucket != self.spare_bucket:
                            return 0
            else:
                if self.spare_bucket != purge_bucket:
                    return 0
                if (0 <= purge_bucket < 4 and self.spare_reusable and self.owner and self.active[0]
                        and self.active[0] != self.spare[0] and not self.recovery_depth
                        and not self.recovery_poison):
                    self.lease_started = 1
                    self.lease_last_bucket = purge_bucket
                    return 0
        self.interlude = 0
        target = self.spare[0]
        self.spare[0] = 0
        self.spare_reusable = 0
        self.lease_started = 0
        return target


class VitaGL:
    """One FBO.  lazy=True models the proposed fix: an attach reaches vitaGL only when a draw follows it."""

    def __init__(self, lazy=False):
        self.r = Reuse()
        self.fb = 0x1000
        self.w = self.h = 0
        self.target = 0
        self.depth = False
        self.purge_idx = 0
        self.clean_idx = 1
        self.rt_purge = [[], [], [], []]
        self.next_handle = 1
        self.lazy = lazy
        self.pending = None
        self.c = {}
        self.reset()

    def reset(self):
        self.c = dict(att=0, sz=0, sw=0, nx=0, rr=0, rd=0, dp=0, rcA=0, rcB=0, rcO=0, gd=0, guest_att=0)

    def attach(self, size):
        self.c["guest_att"] += 1
        if self.lazy:
            self.pending = size
            return
        self._attach(size)

    def _attach(self, size):
        c = self.c
        c["att"] += 1
        old_w, old_h = self.w, self.h
        self.w, self.h = size
        if old_w != self.w or old_h != self.h:
            c["sz"] += 1
            res, nxt = self.r.switch(self.fb, self.target, old_w, old_h, self.w, self.h, 1, self.purge_idx)
            if res:
                c["sw"] += 1
                c["nx"] += nxt != 0
                if res & RETIRES_CURRENT:
                    c["rr"] += 1
                    self.rt_purge[self.purge_idx].append(self.target)
                self.target = nxt
            else:
                if self.target:
                    c["rr"] += 1
                    self.rt_purge[self.purge_idx].append(self.target)
                    self.target = 0
            if self.depth:
                c["rd"] += 1
                self.depth = False

    def draw(self):
        c = self.c
        if self.lazy and self.pending is not None:
            self._attach(self.pending)
            self.pending = None
        if not self.depth:
            c["dp"] += 1
            self.depth = True
        if not self.target:
            self.target = self.next_handle
            self.next_handle += 1
            if (self.w, self.h) == A:
                c["rcA"] += 1
            elif (self.w, self.h) == B:
                c["rcB"] += 1
            else:
                c["rcO"] += 1
            self.r.created(self.fb, self.target, self.w, self.h, 1)

    def swap(self):
        self.c["gd"] += len(self.rt_purge[self.clean_idx])
        self.rt_purge[self.clean_idx] = []
        if self.r.collect(self.clean_idx):
            self.c["gd"] += 1
        self.clean_idx = (self.clean_idx + 1) % 4
        self.purge_idx = (self.purge_idx + 1) % 4


def fmt(c):
    return ("att %4d sz %4d sw %4d nx %4d rr %4d rd %4d dp %4d | rc %3d (A %d, B %d) | gd %3d" % (
        c["att"], c["sz"], c["sw"], c["nx"], c["rr"], c["rd"], c["dp"], c["rcA"] + c["rcB"] + c["rcO"],
        c["rcA"], c["rcB"], c["gd"]))


def dry(gl, frames, read_every):
    """dry room: 5 passes into 480x272 surfaces per frame; every read_every frames one GetTexel of the
    784x472 Floor Surface before the render (attach, 1x1 read served by the backend, no draw)."""
    for f in range(frames):
        if read_every and f % read_every == 0:
            gl.attach(B)
        for _ in range(5):
            gl.attach(A)
            gl.draw()
        gl.swap()


def wet(gl, frames, reads_at=(), pushes=0, toggles=0, idle_swaps_at=None):
    """wet room: per frame, updates then render.
    toggles: read-only toggles per frame in the update (attach B, no draw; attach A, no draw)
    pushes:  update-time passes into the Floor Surface (attach B, draw; attach A, no draw)
    render:  attach A draw x3, attach B draw x2, attach A draw x2 (7 attaches, 2 size changes)"""
    for f in range(frames):
        n_tog = toggles + (1 if f in reads_at else 0)
        for i in range(max(n_tog, pushes)):
            if i < n_tog:
                gl.attach(B)
                gl.attach(A)
            if i < pushes:
                gl.attach(B)
                gl.draw()
                gl.attach(A)
        for _ in range(3):
            gl.attach(A)
            gl.draw()
        for _ in range(2):
            gl.attach(B)
            gl.draw()
        for _ in range(2):
            gl.attach(A)
            gl.draw()
        gl.swap()
        if idle_swaps_at is not None and f == idle_swaps_at[0]:
            for _ in range(idle_swaps_at[1]):
                gl.swap()


def main():
    print("== S1 dry steady state, 100 presents, one read per 10 presents (10 reads), after a 200-present warm-up")
    for lazy in (False, True):
        gl = VitaGL(lazy)
        dry(gl, 200, 10)
        gl.reset()
        dry(gl, 100, 10)
        print("  lazy=%d  %s" % (lazy, fmt(gl.c)))
    print("  measured WZa w49 (100 presents, 10 reads): att 510 sz 20 sw 5 nx 0 rr 5 rd 10 | rc 10 (A 10, B 0) | gd 10")

    print("== S2 wet idle, 116 presents, 10 read toggles, after warm-up")
    for lazy in (False, True):
        gl = VitaGL(lazy)
        wet(gl, 200)
        gl.reset()
        wet(gl, 116, reads_at=set(range(0, 116, 12)))
        print("  lazy=%d  %s" % (lazy, fmt(gl.c)))
    print("  measured WZa w3 (116 presents, 10 reads): att 822 sz 252 sw 252 nx 252 rr 0 rd 242 | rc 0 | gd 0")

    print("== S3 wet read storm from the armed steady state (no break event): 20 presents")
    for tog, push in ((3, 0), (0, 3), (2, 2)):
        gl = VitaGL(False)
        wet(gl, 200)
        gl.reset()
        wet(gl, 20, toggles=tog, pushes=push)
        print("  toggles=%d pushes=%d  %s" % (tog, push, fmt(gl.c)))

    print("== S4 same storm after N presents without a size switch (spare expires: 4 = bucket, 8 = lease): 20 presents")
    for idle in (3, 4, 7, 8):
        for tog, push in ((3, 0), (0, 3), (2, 2), (1, 2)):
            for lazy in (False, True):
                gl = VitaGL(lazy)
                wet(gl, 200, idle_swaps_at=(199, idle))
                gl.reset()
                wet(gl, 20, toggles=tog, pushes=push)
                print("  idle=%d toggles=%d pushes=%d lazy=%d  %s" % (idle, tog, push, lazy, fmt(gl.c)))
    print("  measured WZa w24 (20 presents, 252 reads): att 432 sz 158 sw 21 nx 18 rr 82 rd 101 dp 100 | "
          "rc 84 (A 31, B 53) | gd 75")


if __name__ == "__main__":
    main()
