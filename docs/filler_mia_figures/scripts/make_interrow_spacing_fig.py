import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle
import os

OUT="/tmp/mia_figs"; os.makedirs(OUT,exist_ok=True)
HVT="#4e79a7"; LVT="#f28e2b"; GAP="#e6e6e6"
RED="#d62728"; GREEN="#1a7d1a"; SMIN=2

# rows: row0 y[0..3], row1 y[3..6], shared rail at y=3
def scenario(ax,x0,W,bot,top,result,label,fill_between=False):
    # row outlines
    ax.add_patch(Rectangle((x0,3),W,3,facecolor="white",edgecolor="#888",lw=1.4,zorder=1))
    ax.add_patch(Rectangle((x0,0),W,3,facecolor="white",edgecolor="#888",lw=1.4,zorder=1))
    ax.plot([x0,x0+W],[3,3],color="#444",lw=2.4,zorder=2)   # shared rail
    ix=x0+0.7; iw=W-1.4
    # row1 implant (upper), row0 implant (lower)
    if fill_between:
        ax.add_patch(Rectangle((ix,0.5),iw,5.0,facecolor=HVT,alpha=0.85,edgecolor="black",lw=1.6,hatch="///",zorder=3))
        ax.text(x0+W/2,3.0,"FILL\n(2-row)",ha="center",va="center",fontsize=7.4,fontweight="bold",zorder=4)
    else:
        ax.add_patch(Rectangle((ix,bot),iw,5.5-bot,facecolor=HVT,alpha=0.85,edgecolor="black",lw=1.6,zorder=3))
        ax.add_patch(Rectangle((ix,0.5),iw,top-0.5,facecolor=HVT,alpha=0.85,edgecolor="black",lw=1.6,zorder=3))
        ax.text(x0+W/2,(bot+5.5)/2,"HVT",ha="center",va="center",fontsize=7.4,color="white",fontweight="bold",zorder=4)
        ax.text(x0+W/2,(0.5+top)/2,"HVT",ha="center",va="center",fontsize=7.4,color="white",fontweight="bold",zorder=4)
    col=GREEN if result=="OK" else RED
    # vertical distance arrow between the two implants
    g=bot-top
    ax.text(x0+W/2,result_y(result),label,ha="center",va="center",fontsize=8,color=col,fontweight="bold",zorder=6)
    if not fill_between and g>0.05:
        xa=x0+W-0.45
        ax.annotate("",xy=(xa,bot),xytext=(xa,top),arrowprops=dict(arrowstyle="<->",color=col,lw=1.7),zorder=6)
        ax.text(xa+0.15,(bot+top)/2,f"g={g:.0f}",ha="left",va="center",fontsize=7.6,color=col,fontweight="bold",zorder=6)
        if result!="OK":
            ax.add_patch(Rectangle((ix,top),iw,g,facecolor="none",edgecolor=RED,ls="--",lw=2.2,zorder=5))

def result_y(r): return 6.55

fig,ax=plt.subplots(figsize=(10.2,5.4))
# three scenarios side by side
scenario(ax,0,4.4, bot=3.0,top=3.0, result="OK",   label="abut  ->  2-row region\nOK")
scenario(ax,5.4,4.4, bot=4.0,top=2.0, result="OK", label=f"g=2 >= Smin\nOK")
scenario(ax,10.8,4.4, bot=3.4,top=2.6, result="VIOL", label="g=1 < Smin\nVIOLATION")

# row labels
ax.text(-0.35,4.5,"Row1",ha="right",va="center",fontsize=9,fontweight="bold")
ax.text(-0.35,1.5,"Row0",ha="right",va="center",fontsize=9,fontweight="bold")
ax.text(15.6,3.0,"shared\npower rail",ha="left",va="center",fontsize=7.5,color="#444")

ax.set_xlim(-1.6,17.4); ax.set_ylim(-0.4,7.2); ax.axis("off")
ax.set_title("Inter-row implant spacing — Smin also applies VERTICALLY between stacked rows\n"
             "(same rule as fig7, in the up/down direction): abut OR keep >= Smin, never a sub-Smin gap",
             fontsize=10.2,loc="left",fontweight="bold")

from matplotlib.patches import Patch
fig.legend(handles=[
    Patch(facecolor=HVT,alpha=.85,edgecolor="black",label="HVT implant (same layer in both rows)"),
], loc="lower center",ncol=1,fontsize=8.5,frameon=False,bbox_to_anchor=(0.5,-0.04))
fig.tight_layout(rect=[0,0.04,1,1])
fig.savefig(f"{OUT}/fig8_interrow_spacing.png",dpi=150,bbox_inches="tight")
print("saved fig8_interrow_spacing.png")
