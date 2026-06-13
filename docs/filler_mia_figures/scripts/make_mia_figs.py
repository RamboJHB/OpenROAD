import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, FancyArrowPatch
import os

OUT = "/tmp/mia_figs"
os.makedirs(OUT, exist_ok=True)

HVT = "#4e79a7"   # blue
LVT = "#f28e2b"   # orange
GAP = "#e6e6e6"
WMIN = 4

def draw_row(ax, segs, y=0.0, h=1.0):
    """segs: list of dicts: x,w,kind('cell'/'filler'/'gap'/'mixed'),vt,label, split(for mixed)"""
    for s in segs:
        x, w, kind = s["x"], s["w"], s["kind"]
        if kind == "gap":
            ax.add_patch(Rectangle((x, y), w, h, facecolor=GAP, edgecolor="#999999",
                                   hatch="..", lw=1.0, zorder=1))
            ax.text(x + w/2, y + h/2, s.get("label", "gap"), ha="center", va="center",
                    fontsize=8, color="#666666")
        elif kind == "mixed":
            sp = s["split"]
            ax.add_patch(Rectangle((x, y), sp, h, facecolor=s["vtL"], alpha=0.85, edgecolor="none", zorder=1))
            ax.add_patch(Rectangle((x+sp, y), w-sp, h, facecolor=s["vtR"], alpha=0.85, edgecolor="none", zorder=1))
            ax.add_patch(Rectangle((x, y), w, h, facecolor="none", edgecolor="black", lw=2.4, zorder=3))
            ax.text(x + w/2, y + h + 0.12, s.get("label",""), ha="center", va="bottom", fontsize=8.5, fontweight="bold")
        else:
            fc = s["vt"]
            hatch = "///" if kind == "filler" else None
            ax.add_patch(Rectangle((x, y), w, h, facecolor=fc, alpha=0.85,
                                   edgecolor="black", lw=1.6, hatch=hatch, zorder=2))
            ax.text(x + w/2, y + h/2, s.get("label",""), ha="center", va="center",
                    fontsize=8.5, color="white", fontweight="bold")

def bracket(ax, x0, x1, ok, vt, y=1.25, label=None):
    col = "#1a7d1a" if ok else "#d62728"
    ax.plot([x0, x0, x1, x1], [y, y+0.12, y+0.12, y], color=col, lw=1.8)
    w = x1 - x0
    if label == "":
        return
    txt = label if label else f"w={w} " + ("OK" if ok else "<Wmin!")
    ax.text((x0+x1)/2, y+0.18, txt, ha="center", va="bottom", fontsize=8.5,
            color=col, fontweight="bold")

def arrow_insert(ax, x, y0=-0.55, y1=-0.05):
    ax.add_patch(FancyArrowPatch((x, y0), (x, y1), arrowstyle="-|>", mutation_scale=14,
                                 color="#333333", lw=1.6))

def base(ax, xmax, title):
    ax.set_xlim(-0.5, xmax+0.5)
    ax.set_ylim(-0.9, 2.0)
    ax.set_aspect("equal")
    ax.axis("off")
    ax.set_title(title, fontsize=10.5, loc="left", fontweight="bold")

def legend(fig):
    from matplotlib.patches import Patch
    handles = [
        Patch(facecolor=HVT, alpha=0.85, edgecolor="black", label="HVT implant"),
        Patch(facecolor=LVT, alpha=0.85, edgecolor="black", label="LVT implant"),
        Patch(facecolor="white", edgecolor="black", hatch="///", label="FILLER (inserted)"),
        Patch(facecolor=GAP, edgecolor="#999999", hatch="..", label="empty gap"),
    ]
    fig.legend(handles=handles, loc="lower center", ncol=4, fontsize=8.5, frameon=False,
               bbox_to_anchor=(0.5, -0.02))

# ---------------- FIG 1: concept ----------------
fig, ax = plt.subplots(figsize=(9, 2.6))
segs = [
    {"x":0,"w":4,"kind":"cell","vt":LVT,"label":"LVT cell"},
    {"x":4,"w":2,"kind":"cell","vt":HVT,"label":"HVT"},
    {"x":6,"w":2,"kind":"gap"},
    {"x":8,"w":4,"kind":"cell","vt":LVT,"label":"LVT cell"},
]
draw_row(ax, segs)
bracket(ax, 0, 4, True, LVT)        # LVT left region ok
bracket(ax, 4, 6, False, HVT)       # HVT region too narrow
bracket(ax, 8, 12, True, LVT)
# wmin ruler
ax.plot([0, WMIN], [-0.75, -0.75], color="black", lw=1.4)
ax.plot([0,0],[-0.82,-0.68], color="black", lw=1.4); ax.plot([WMIN,WMIN],[-0.82,-0.68], color="black", lw=1.4)
ax.text(WMIN/2, -0.95, f"Wmin = {WMIN}", ha="center", va="top", fontsize=8.5)
base(ax, 12, "Fig 1  Concept: each continuous same-Vt implant region must be >= Wmin")
ax.text(5,1.0,"too small!",ha="center",fontsize=8,color="#d62728",style="italic")
legend(fig)
fig.tight_layout(); fig.savefig(f"{OUT}/fig1_concept.png", dpi=150, bbox_inches="tight"); plt.close(fig)

# ---------------- helper for before/after figs ----------------
def before_after(fname, title, before, after, brk_before, brk_after, xmax,
                 insert_xs=None, note_after=""):
    fig, (a1, a2) = plt.subplots(2, 1, figsize=(9, 4.4))
    draw_row(a1, before)
    for b in brk_before: bracket(a1, *b)
    base(a1, xmax, title + "   —  BEFORE (violation)")
    draw_row(a2, after)
    for b in brk_after: bracket(a2, *b)
    if insert_xs:
        for x in insert_xs: arrow_insert(a2, x)
        a2.text(insert_xs[0], -0.78, "insert filler", ha="center", va="top", fontsize=8, color="#333333")
    base(a2, xmax, "AFTER (fixed)")
    if note_after:
        a2.text(xmax/2, 1.75, note_after, ha="center", fontsize=9, color="#1a7d1a", fontweight="bold")
    legend(fig)
    fig.tight_layout(); fig.savefig(f"{OUT}/{fname}", dpi=150, bbox_inches="tight"); plt.close(fig)

# ---------------- FIG 2: case 1 - one filler, one type ----------------
before = [
    {"x":0,"w":4,"kind":"cell","vt":LVT,"label":"LVT cell"},
    {"x":4,"w":2,"kind":"cell","vt":HVT,"label":"HVT"},
    {"x":6,"w":2,"kind":"gap"},
    {"x":8,"w":4,"kind":"cell","vt":LVT,"label":"LVT cell"},
]
after = [
    {"x":0,"w":4,"kind":"cell","vt":LVT,"label":"LVT cell"},
    {"x":4,"w":2,"kind":"cell","vt":HVT,"label":"HVT"},
    {"x":6,"w":2,"kind":"filler","vt":HVT,"label":"FILL\nHVT"},
    {"x":8,"w":4,"kind":"cell","vt":LVT,"label":"LVT cell"},
]
before_after("fig2_case1_one_filler.png",
             "Case 1  Extend on ONE side with a single same-Vt filler",
             before, after,
             [(0,4,True,LVT),(4,6,False,HVT),(8,12,True,LVT)],
             [(0,4,True,LVT),(4,8,True,HVT),(8,12,True,LVT)],
             12, insert_xs=[7],
             note_after="HVT region 2 -> 4 : now >= Wmin")

# ---------------- FIG 3: case 2 - two fillers, same type ----------------
before = [
    {"x":0,"w":4,"kind":"cell","vt":LVT,"label":"LVT cell"},
    {"x":4,"w":1,"kind":"gap"},
    {"x":5,"w":2,"kind":"cell","vt":HVT,"label":"HVT"},
    {"x":7,"w":1,"kind":"gap"},
    {"x":8,"w":4,"kind":"cell","vt":LVT,"label":"LVT cell"},
]
after = [
    {"x":0,"w":4,"kind":"cell","vt":LVT,"label":"LVT cell"},
    {"x":4,"w":1,"kind":"filler","vt":HVT,"label":"F"},
    {"x":5,"w":2,"kind":"cell","vt":HVT,"label":"HVT"},
    {"x":7,"w":1,"kind":"filler","vt":HVT,"label":"F"},
    {"x":8,"w":4,"kind":"cell","vt":LVT,"label":"LVT cell"},
]
before_after("fig3_case2_two_same.png",
             "Case 2  Narrow gaps on both sides -> TWO fillers, but SAME Vt",
             before, after,
             [(0,4,True,LVT),(5,7,False,HVT),(8,12,True,LVT)],
             [(4,8,True,HVT,1.25,"implant w=4  OK (1+2+1)"),(0,4,True,LVT),(8,12,True,LVT)],
             12, insert_xs=[4.5,7.5],
             note_after="2 HVT fillers (one per side) -> HVT region = 4")

# ---------------- FIG 4: case 3 - ONE cell, TWO DIFFERENT fillers ----------------
before = [
    {"x":0,"w":3,"kind":"gap"},
    {"x":3,"w":3,"kind":"gap"},   # left gap (split for clarity none) -> treat as one gap 0-6
    {"x":6,"w":2,"kind":"mixed","vtL":HVT,"vtR":LVT,"split":1.0,"label":"M (mixed-Vt cell)"},
    {"x":8,"w":3,"kind":"gap"},
    {"x":11,"w":3,"kind":"gap"},
]
# simplify left/right gaps into single gaps
before = [
    {"x":0,"w":6,"kind":"gap","label":"empty"},
    {"x":6,"w":2,"kind":"mixed","vtL":HVT,"vtR":LVT,"split":1.0,"label":"M (mixed-Vt cell)"},
    {"x":8,"w":6,"kind":"gap","label":"empty"},
]
after = [
    {"x":3,"w":3,"kind":"filler","vt":HVT,"label":"FILL\nHVT"},
    {"x":6,"w":2,"kind":"mixed","vtL":HVT,"vtR":LVT,"split":1.0,"label":"M (mixed-Vt)"},
    {"x":8,"w":3,"kind":"filler","vt":LVT,"label":"FILL\nLVT"},
    {"x":0,"w":3,"kind":"gap","label":""},
    {"x":11,"w":3,"kind":"gap","label":""},
]
fig, (a1, a2) = plt.subplots(2, 1, figsize=(9, 4.6))
draw_row(a1, before)
# small red brackets (no text) under each tiny sub-region
bracket(a1, 6, 7, False, HVT, 1.25, "")
bracket(a1, 7, 8, False, LVT, 1.25, "")
# separated labels with leader lines
a1.plot([4.6,6.5],[1.55,1.37],color="#d62728",lw=1.0)
a1.text(4.5,1.6,"HVT part w=1\nVIOLATION",ha="center",va="bottom",fontsize=8,color="#d62728",fontweight="bold")
a1.plot([9.4,7.5],[1.55,1.37],color="#d62728",lw=1.0)
a1.text(9.5,1.6,"LVT part w=1\nVIOLATION",ha="center",va="bottom",fontsize=8,color="#d62728",fontweight="bold")
base(a1, 14, "Case 3  ONE cell needs TWO DIFFERENT fillers (mixed-Vt cell)   —  BEFORE")
a1.text(7,-0.25,"M has TWO implant regions: left=HVT, right=LVT  (both too small)",
        ha="center",fontsize=8,color="#d62728")
draw_row(a2, after)
bracket(a2, 3, 7, True, HVT, 1.25, "HVT region w=4  OK")
bracket(a2, 7, 11, True, LVT, 1.25, "LVT region w=4  OK")
arrow_insert(a2, 4.5); arrow_insert(a2, 9.5)
a2.text(4.5,-0.78,"HVT filler",ha="center",va="top",fontsize=8)
a2.text(9.5,-0.78,"LVT filler",ha="center",va="top",fontsize=8)
base(a2, 14, "AFTER (fixed): HVT filler on the left + LVT filler on the right")
a2.text(7,1.78,"one cell  ->  two different-Vt fillers",ha="center",fontsize=9.5,
        color="#1a7d1a",fontweight="bold")
legend(fig)
fig.tight_layout(); fig.savefig(f"{OUT}/fig4_case3_two_diff.png", dpi=150, bbox_inches="tight"); plt.close(fig)

print("done:", os.listdir(OUT))
