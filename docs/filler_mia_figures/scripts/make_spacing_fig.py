import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle
import os

OUT="/tmp/mia_figs"; os.makedirs(OUT,exist_ok=True)
LVT="#f28e2b"; HVT="#4e79a7"; GAP="#e6e6e6"
SMIN=2; WMIN=4
RED="#d62728"; GREEN="#1a7d1a"

def block(ax,x,w,y,h,vt,label="",kind="cell"):
    if kind=="gap":
        ax.add_patch(Rectangle((x,y),w,h,facecolor=GAP,edgecolor="#999",hatch="..",lw=1,zorder=1))
        if label: ax.text(x+w/2,y+h/2,label,ha="center",va="center",fontsize=7,color="#777")
        return
    hatch="///" if kind=="filler" else None
    ax.add_patch(Rectangle((x,y),w,h,facecolor=vt,alpha=0.85,edgecolor="black",lw=1.6,hatch=hatch,zorder=2))
    if label: ax.text(x+w/2,y+h/2,label,ha="center",va="center",fontsize=7.6,color="black",fontweight="bold",zorder=3)

def dist(ax,x0,x1,y,txt,col):
    ax.annotate("",xy=(x1,y),xytext=(x0,y),arrowprops=dict(arrowstyle="<->",color=col,lw=1.6))
    ax.text((x0+x1)/2,y+0.06,txt,ha="center",va="bottom",fontsize=7.6,color=col,fontweight="bold")

def tag(ax,x,y,txt,col):
    ax.text(x,y,txt,ha="center",va="center",fontsize=8,color=col,fontweight="bold")

fig,(a,b,c)=plt.subplots(3,1,figsize=(9.6,8.0))

# ===================== Panel A: the rule =====================
H=0.8
# bad row (top)
yb=1.6
block(a,0,3,yb,H,HVT,"HVT")
block(a,4,3,yb,H,HVT,"HVT")          # gap 3..4 = 1 < SMIN
block(a,3,1,yb,H,None,"","gap")
dist(a,3,4,yb+H+0.05,f"g=1  < Smin={SMIN}",RED)
a.add_patch(Rectangle((3,yb),1,H,facecolor="none",edgecolor=RED,ls="--",lw=2.2,zorder=5))
tag(a,8.6,yb+H/2,"SPACING\nVIOLATION",RED)
# good row (bottom)
yg=0.2
block(a,0,3,yg,H,HVT,"HVT")
block(a,5,3,yg,H,HVT,"HVT")          # gap 3..5 = 2 = SMIN
block(a,3,2,yg,H,None,"","gap")
dist(a,3,5,yg+H+0.05,f"g=2  >= Smin={SMIN}",GREEN)
tag(a,8.6,yg+H/2,"OK",GREEN)
a.set_xlim(-0.5,10.5); a.set_ylim(-0.1,2.95); a.axis("off")
a.set_title("(A) Implant min-spacing rule: two implant regions closer than Smin = violation",
            fontsize=9.5,loc="left",fontweight="bold")

# ===================== Panel B: three filling outcomes =====================
# left LVT [0,4], gap [4,8], HVT cell [8,12]; fill gap with HVT filler, vary gap-to-LVT
def scen(ax,y,fx0,label,ok,note):
    block(ax,0,4,y,H,LVT,"LVT")
    block(ax,8,4,y,H,HVT,"HVT")
    # leftover gap between LVT(=4) and filler(fx0)
    if fx0>4:
        block(ax,4,fx0-4,y,H,None,"","gap")
    block(ax,fx0,8-fx0,y,H,HVT,"FILL","filler")
    col=GREEN if ok else RED
    tag(ax,13.2,y+H/2,label,col)
    ax.text(-0.4,y+H/2,note,ha="right",va="center",fontsize=7.3,color="#333")
    return col

yb1,yb2,yb3=2.0,1.0,0.0
# B1 abut
scen(b,yb1,4,"OK (abut)",True,"abut")
dist(b,3.9,4.1,yb1+H+0.04,"gap=0",GREEN)
# B2 >= Smin
scen(b,yb2,6,"OK",True,">= Smin")
dist(b,4,6,yb2+H+0.04,f"gap=2 = Smin",GREEN)
b.add_patch(Rectangle((4,yb2),2,H,facecolor="none",edgecolor=GREEN,ls=":",lw=1.4,zorder=5))
# B3 sliver
scen(b,yb3,5,"VIOLATION",False,"sub-Smin sliver")
dist(b,4,5,yb3+H+0.04,"gap=1 < Smin",RED)
b.add_patch(Rectangle((4,yb3),1,H,facecolor="none",edgecolor=RED,ls="--",lw=2.2,zorder=5))
b.set_xlim(-2.4,14.6); b.set_ylim(-0.15,3.05); b.axis("off")
b.set_title("(B) Filling a gap (HVT filler next to LVT): abut  OR  leave >= Smin — never a sub-Smin sliver",
            fontsize=9.5,loc="left",fontweight="bold")

# ===================== Panel C: the trap — one bad filler, MIA + spacing both fail =====================
y=0.4
block(c,0,4.5,y,H,LVT,"LVT")
block(c,7.5,4.5,y,H,LVT,"LVT")
block(c,4.5,0.5,y,H,None,"","gap")
block(c,7,0.5,y,H,None,"","gap")
block(c,5,2,y,H,HVT,"FILL\nHVT","filler")   # width 2 < WMIN, gaps 0.5 < SMIN
# MIA bracket (width of HVT region)
c.annotate("",xy=(7,y-0.18),xytext=(5,y-0.18),arrowprops=dict(arrowstyle="<->",color=RED,lw=1.6))
c.text(6,y-0.52,f"HVT width = 2  < Wmin={WMIN}\n(MIA violation)",ha="center",va="top",fontsize=7.6,color=RED,fontweight="bold")
# spacing arrows on both sides
dist(c,4.5,5,y+H+0.05,"0.5 < Smin",RED)
dist(c,7,7.5,y+H+0.05,"0.5 < Smin",RED)
c.add_patch(Rectangle((5,y),2,H,facecolor="none",edgecolor=RED,ls="--",lw=2.2,zorder=5))
tag(c,12.6,y+H/2,"MIA + SPACING\nboth fail",RED)
c.set_xlim(-0.5,14.4); c.set_ylim(-1.25,1.6); c.axis("off")
c.set_title("(C) The trap: a too-narrow filler can break BOTH at once (its implant < Wmin AND < Smin to neighbors)",
            fontsize=9.5,loc="left",fontweight="bold")

from matplotlib.patches import Patch
fig.legend(handles=[
    Patch(facecolor=LVT,alpha=.85,edgecolor="black",label="LVT implant"),
    Patch(facecolor=HVT,alpha=.85,edgecolor="black",label="HVT implant"),
    Patch(facecolor="white",edgecolor="black",hatch="///",label="FILLER"),
    Patch(facecolor=GAP,edgecolor="#999",hatch="..",label="empty gap"),
], loc="lower center",ncol=4,fontsize=8.3,frameon=False,bbox_to_anchor=(0.5,-0.02))
fig.tight_layout(rect=[0,0.03,1,1])
fig.savefig(f"{OUT}/fig7_spacing.png",dpi=150,bbox_inches="tight")
print("saved fig7_spacing.png")
