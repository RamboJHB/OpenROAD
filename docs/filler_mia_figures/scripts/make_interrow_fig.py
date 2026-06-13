import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, FancyArrowPatch
import os

OUT = "/tmp/mia_figs"
os.makedirs(OUT, exist_ok=True)
HVT = "#4e79a7"; LVT = "#f28e2b"; GAP = "#e6e6e6"
WMIN = 4

def box(ax, x, y, w, h, fc, label="", kind="cell"):
    if kind == "gap":
        ax.add_patch(Rectangle((x,y),w,h,facecolor=GAP,edgecolor="#999",hatch="..",lw=1,zorder=1))
        if label: ax.text(x+w/2,y+h/2,label,ha="center",va="center",fontsize=7.5,color="#777")
        return
    hatch = "///" if kind=="filler" else None
    ax.add_patch(Rectangle((x,y),w,h,facecolor=fc,alpha=0.85,edgecolor="black",lw=1.6,hatch=hatch,zorder=2))
    if label: ax.text(x+w/2,y+h/2,label,ha="center",va="center",fontsize=8,color="white",fontweight="bold")

def rails(ax, xmax):
    ax.plot([0,xmax],[1,1],color="#444",lw=3,zorder=4)
    ax.text(xmax+0.15,1,"shared\npower rail",va="center",ha="left",fontsize=7,color="#444")
    ax.plot([0,xmax],[0,0],color="#bbb",lw=1.5,zorder=0)
    ax.plot([0,xmax],[2,2],color="#bbb",lw=1.5,zorder=0)
    ax.text(-0.2,0.5,"Row0",ha="right",va="center",fontsize=8,fontweight="bold")
    ax.text(-0.2,1.5,"Row1",ha="right",va="center",fontsize=8,fontweight="bold")

def viol(ax, x, y, w, h, txt, ty=None):
    ax.add_patch(Rectangle((x,y),w,h,facecolor="none",edgecolor="#d62728",ls="--",lw=2.4,zorder=5))
    ax.text(x+w/2, (ty if ty is not None else y+h+0.12), txt, ha="center", va="bottom",
            fontsize=8, color="#d62728", fontweight="bold")

def okbracket(ax, x0, x1, y, txt, col="#1a7d1a"):
    ax.plot([x0,x0,x1,x1],[y,y+0.12,y+0.12,y],color=col,lw=1.8)
    ax.text((x0+x1)/2,y+0.16,txt,ha="center",va="bottom",fontsize=8.5,color=col,fontweight="bold")

def base(ax, xmax, title):
    ax.set_xlim(-1.2,xmax+1.8); ax.set_ylim(-0.7,3.0); ax.set_aspect("equal"); ax.axis("off")
    ax.set_title(title, fontsize=10, loc="left", fontweight="bold")

XMAX = 13
fig, axes = plt.subplots(3,1, figsize=(9.5,8.2))

# common neighbors (both rows): left LVT 0-5, right LVT 9-13
def neighbors(ax):
    box(ax,0,1,5,1,LVT,"LVT cell")     # row1 left
    box(ax,0,0,5,1,LVT,"LVT cell")     # row0 left
    box(ax,9,1,4,1,LVT,"LVT cell")     # row1 right
    box(ax,9,0,4,1,LVT,"LVT cell")     # row0 right

# ---------- Panel A: BEFORE ----------
ax=axes[0]; neighbors(ax)
box(ax,5,0,2,2,HVT,"HVT\n(2-row cell M)")   # 2-row tall narrow HVT
box(ax,7,1,2,1,"gap","gap","gap"); box(ax,7,0,2,1,"gap","gap","gap")
viol(ax,5,0,2,2,"HVT implant width = 2  <  Wmin = 4   (spans BOTH rows)  ->  MIA violation", ty=2.25)
rails(ax,XMAX)
base(ax,XMAX,"(A) BEFORE: a 2-row-tall narrow HVT region violates MIA")

# ---------- Panel B: NAIVE (fill top row only) ----------
ax=axes[1]; neighbors(ax)
box(ax,5,0,2,2,HVT,"HVT\nM")
box(ax,7,1,2,1,HVT,"FILL\nHVT","filler")     # filler in ROW1 only
box(ax,7,0,2,1,"gap","gap","gap")            # row0 still empty
okbracket(ax,5,9,2.18,"Row1 HVT = 4  OK")    # row1 fixed
viol(ax,5,0,2,1,"Row0 HVT still = 2  X", ty=-0.45)
viol(ax,7,1,2,1,"1-row HVT over empty Row0  ->  INTER-ROW MIA violation", ty=2.5)
rails(ax,XMAX)
base(ax,XMAX,"(B) NAIVE: fill only Row1 -> fixes Row1 but BREAKS Row0  (= paper's inter-row violation)")

# ---------- Panel C: CORRECT (fill both rows aligned) ----------
ax=axes[2]; neighbors(ax)
box(ax,5,0,2,2,HVT,"HVT\nM")
box(ax,7,0,2,2,HVT,"FILL\nHVT","filler")     # 2-row-tall filler
okbracket(ax,5,9,2.18,"HVT width = 4  >=  Wmin   (BOTH rows)  OK")
# spacing annotation at x=9 (HVT filler | LVT cell abut)
ax.add_patch(FancyArrowPatch((9,2.6),(9,2.05),arrowstyle="-|>",mutation_scale=12,color="#1a7d1a",lw=1.5))
ax.text(9,2.62,"implant spacing rule OK\n(flush abut to LVT, no sub-Smin sliver)",
        ha="center",va="bottom",fontsize=8,color="#1a7d1a",fontweight="bold")
ax.text(7,-0.45,"2-row-tall HVT filler keeps both rows aligned",ha="center",va="top",
        fontsize=8,color="#1a7d1a")
rails(ax,XMAX)
base(ax,XMAX,"(C) CORRECT: fill BOTH rows (aligned) -> MIA ok in both rows + spacing ok")

from matplotlib.patches import Patch
fig.legend(handles=[
    Patch(facecolor=HVT,alpha=.85,edgecolor="black",label="HVT implant"),
    Patch(facecolor=LVT,alpha=.85,edgecolor="black",label="LVT implant"),
    Patch(facecolor="white",edgecolor="black",hatch="///",label="FILLER (inserted)"),
    Patch(facecolor=GAP,edgecolor="#999",hatch="..",label="empty gap"),
], loc="lower center", ncol=4, fontsize=8.5, frameon=False, bbox_to_anchor=(0.5,-0.01))

fig.tight_layout(rect=[0,0.02,1,1])
fig.savefig(f"{OUT}/fig5_interrow.png", dpi=150, bbox_inches="tight")
print("saved fig5_interrow.png")
