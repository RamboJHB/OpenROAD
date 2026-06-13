import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, FancyArrowPatch
import os

OUT="/tmp/mia_figs"; os.makedirs(OUT,exist_ok=True)
HVT="#4e79a7"; LVT="#f28e2b"; GAP="#e6e6e6"; RED="#d62728"; GREEN="#1a7d1a"; SMIN=2

def cell(ax,x,w,y,h,color,label="",kind="cell"):
    if kind=="gap":
        ax.add_patch(Rectangle((x,y),w,h,facecolor=GAP,edgecolor="#999",hatch="..",lw=1,zorder=1)); return
    hatch="///" if kind=="filler" else None
    ax.add_patch(Rectangle((x,y),w,h,facecolor=color,alpha=0.85,edgecolor="black",lw=1.7,hatch=hatch,zorder=2))
    if label: ax.text(x+w/2,y+h/2,label,ha="center",va="center",fontsize=8,
                      color=("black" if kind=="filler" else "white"),fontweight="bold",zorder=3)

def hdist(ax,x0,x1,y,txt,col):
    ax.annotate("",xy=(x1,y),xytext=(x0,y),arrowprops=dict(arrowstyle="<->",color=col,lw=1.8))
    ax.text((x0+x1)/2,y+0.08,txt,ha="center",va="bottom",fontsize=8,color=col,fontweight="bold")

fig,ax=plt.subplots(figsize=(11,4.8))
# rows: Row0 y[0,2] LVT, Row1 y[2,4] HVT, rail at 2
ax.plot([0,12],[2,2],color="#444",lw=2.6,zorder=4)
ax.plot([0,12],[0,0],color="#444",lw=2.6,zorder=4)
ax.plot([0,12],[4,4],color="#444",lw=2.6,zorder=4)

# Row0 (LVT) — different Vt, full context
cell(ax,0,12,0,2,LVT,"",)
ax.text(4,0.7,"Row0 = LVT cells  (different Vt)",ha="center",va="center",fontsize=8.5,color="white",fontweight="bold",zorder=3)

# Row1 (HVT): HVT cell | HVT filler (abut left = OK) | sliver gap | HVT cell (right = VIOLATION)
cell(ax,0,3.5,2,2,HVT,"HVT cell")
cell(ax,3.5,3.0,2,2,HVT,"FILL\nHVT","filler")     # abuts left cell at x=3.5
cell(ax,6.5,1.0,2,2,None,"","gap")                # sliver gap 6.5..7.5 = 1 < Smin
cell(ax,7.5,4.5,2,2,HVT,"HVT cell")

# horizontal spacing annotations (LEFT/RIGHT = the real checks)
hdist(ax,3.4,3.6,4.15,"abut g=0  OK",GREEN)
hdist(ax,6.5,7.5,4.15,f"g=1 < Smin  VIOLATION",RED)
ax.add_patch(Rectangle((6.5,2),1.0,2,facecolor="none",edgecolor=RED,ls="--",lw=2.4,zorder=6))

# vertical = NOT a spacing check
xv=10.0
ax.add_patch(FancyArrowPatch((xv,2.85),(xv,1.15),arrowstyle="<->",mutation_scale=12,color=RED,lw=1.5,ls=(0,(2,2)),zorder=5))
ax.plot([xv-0.4,xv+0.4],[2.3,1.7],color=RED,lw=3,zorder=7)   # cross-out
ax.plot([xv-0.4,xv+0.4],[1.7,2.3],color=RED,lw=3,zorder=7)
ax.text(xv+0.6,2.0,"up/down:\nNOT checked",ha="left",va="center",fontsize=8.2,color=RED,fontweight="bold",zorder=7)

ax.text(-0.3,3.0,"Row1\nHVT",ha="right",va="center",fontsize=9,fontweight="bold")
ax.text(-0.3,1.0,"Row0\nLVT",ha="right",va="center",fontsize=9,fontweight="bold")
ax.text(12.2,2.0,"shared\npower rail",ha="left",va="center",fontsize=7.6,color="#444")

ax.set_xlim(-1.5,15.4); ax.set_ylim(-0.5,4.9); ax.axis("off")
ax.set_title("Implant spacing is checked LEFT/RIGHT — between the inserted filler and its in-row cells\n"
             "(abut OR >= Smin, no sub-Smin sliver). Adjacent rows are different Vt, so up/down is NOT checked.",
             fontsize=10.2,loc="left",fontweight="bold")

from matplotlib.patches import Patch
fig.legend(handles=[
    Patch(facecolor=HVT,alpha=.85,edgecolor="black",label="HVT implant (Row1)"),
    Patch(facecolor=LVT,alpha=.85,edgecolor="black",label="LVT implant (Row0)"),
    Patch(facecolor="white",edgecolor="black",hatch="///",label="filler (matches its row's Vt)"),
    Patch(facecolor=GAP,edgecolor="#999",hatch="..",label="empty gap"),
], loc="lower center",ncol=4,fontsize=8.3,frameon=False,bbox_to_anchor=(0.5,-0.02))
fig.tight_layout(rect=[0,0.03,1,1])
fig.savefig(f"{OUT}/fig7b_horizontal_spacing.png",dpi=150,bbox_inches="tight")
print("saved fig7b_horizontal_spacing.png")
