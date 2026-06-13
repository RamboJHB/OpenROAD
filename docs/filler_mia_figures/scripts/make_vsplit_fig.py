import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, FancyArrowPatch
import os

OUT="/tmp/mia_figs"; os.makedirs(OUT,exist_ok=True)
LVT="#f28e2b"; HVT="#4e79a7"; GAP="#e6e6e6"; WMIN=4

def bandcell(ax,x,w,y0,h,top_vt,bot_vt,label="",kind="cell"):
    hb=h/2
    if kind=="gap":
        ax.add_patch(Rectangle((x,y0),w,h,facecolor=GAP,edgecolor="#999",hatch="..",lw=1,zorder=1))
        if label: ax.text(x+w/2,y0+h/2,label,ha="center",va="center",fontsize=7,color="#777")
        return
    hatch="///" if kind=="filler" else None
    ax.add_patch(Rectangle((x,y0),w,hb,facecolor=bot_vt,alpha=0.85,edgecolor="none",hatch=hatch,zorder=2))
    ax.add_patch(Rectangle((x,y0+hb),w,hb,facecolor=top_vt,alpha=0.85,edgecolor="none",hatch=hatch,zorder=2))
    ax.plot([x,x+w],[y0+hb,y0+hb],color="white",lw=0.8,zorder=3)
    ax.add_patch(Rectangle((x,y0),w,h,facecolor="none",edgecolor="black",lw=1.8,zorder=4))
    if label: ax.text(x+w/2,y0+h/2,label,ha="center",va="center",fontsize=7.8,color="black",fontweight="bold",zorder=5)

def okbr(ax,x0,x1,y,txt,col="#1a7d1a"):
    ax.plot([x0,x0,x1,x1],[y,y+0.12,y+0.12,y],color=col,lw=1.8)
    ax.text((x0+x1)/2,y+0.15,txt,ha="center",va="bottom",fontsize=8,color=col,fontweight="bold")
def viol(ax,x,y,w,h,txt,ty):
    ax.add_patch(Rectangle((x,y),w,h,facecolor="none",edgecolor="#d62728",ls="--",lw=2.2,zorder=6))
    ax.text(x+w/2,ty,txt,ha="center",va="bottom",fontsize=7.8,color="#d62728",fontweight="bold")

fig,(a1,a2)=plt.subplots(2,1,figsize=(9.5,7.4))

# ============ PANEL (a): single row, top LVT / bottom HVT cell, ONE mixed filler ============
def panelA(ax, after):
    # bottom band = NMOS, top band = PMOS
    bandcell(ax,0,4,0,2,LVT,LVT,"cell")
    bandcell(ax,4,1,0,2,None,None,"gap","gap")
    bandcell(ax,5,2,0,2,LVT,HVT,"M")          # top LVT / bottom HVT
    if not after:
        bandcell(ax,7,2,0,2,None,None,"gap","gap")
    else:
        bandcell(ax,7,2,0,2,LVT,HVT,"FILL","filler")   # matching mixed filler
    bandcell(ax,9,3,0,2,LVT,LVT,"cell")
    # rails
    ax.plot([0,12],[2,2],color="#444",lw=2.5); ax.plot([0,12],[0,0],color="#444",lw=2.5)
    ax.text(12.2,1.5,"PMOS band\n(top Vt)",fontsize=7,va="center")
    ax.text(12.2,0.5,"NMOS band\n(bottom Vt)",fontsize=7,va="center")
    ax.text(-0.2,2,"VDD",ha="right",va="center",fontsize=7,color="#444")
    ax.text(-0.2,0,"VSS",ha="right",va="center",fontsize=7,color="#444")
    ax.set_xlim(-1.4,14.2); ax.set_ylim(-0.9,2.9); ax.set_aspect("equal"); ax.axis("off")

panelA(a1, after=False)
viol(a1,5,1,2,1,"top LVT band w=2  < Wmin",2.18)
viol(a1,5,0,2,1,"bottom HVT band w=2  < Wmin",-0.55)
a1.set_title("(a) BEFORE — one cell split top=LVT / bottom=HVT: BOTH bands violate MIA",
             fontsize=9.5,loc="left",fontweight="bold")
# AFTER inset row below? draw second small after-row to the same panel is messy; use a2-top instead.

# Reuse a2 for AFTER of panel(a) + inter-row note -> actually make a2 the AFTER+interrow.
# ============ PANEL (a-after) drawn at top of a2 ; plus inter-row two-filler note ============
panelA(a2, after=True)
okbr(a2,5,9,2.18,"top LVT band = 4  OK")
okbr(a2,5,9,-0.42,"bottom HVT band = 4  OK")
a2.text(7,2.66,"ONE mixed-Vt filler (top LVT / bottom HVT) fixes BOTH bands at once",
        ha="center",va="bottom",fontsize=8.2,color="#1a7d1a",fontweight="bold",zorder=7)
a2.set_title("(b) AFTER — fix M with a single matching top-LVT / bottom-HVT filler",
             fontsize=9.5,loc="left",fontweight="bold")

from matplotlib.patches import Patch
fig.legend(handles=[
    Patch(facecolor=LVT,alpha=.85,edgecolor="black",label="LVT implant"),
    Patch(facecolor=HVT,alpha=.85,edgecolor="black",label="HVT implant"),
    Patch(facecolor="white",edgecolor="black",hatch="///",label="FILLER (inserted)"),
    Patch(facecolor=GAP,edgecolor="#999",hatch="..",label="empty gap"),
], loc="lower center",ncol=4,fontsize=8.3,frameon=False,bbox_to_anchor=(0.5,-0.01))
fig.tight_layout(rect=[0,0.03,1,1])
fig.savefig(f"{OUT}/fig6_case3_vertical.png",dpi=150,bbox_inches="tight")
print("saved fig6_case3_vertical.png")
